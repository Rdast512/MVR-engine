#include "vk_mega_geometry.hpp"
#include <algorithm>
#include <cstring>
#include <format>
#include <numeric>
#include <stdexcept>
#include <utility>
#include "static_headers/logger.hpp"
#include "util/vk_tracy.hpp"
#include "util/vk_utils.hpp"

namespace
{
    // CPU-written build inputs; keeps every section 16 B aligned
    constexpr vk::DeviceSize kInputAlignment = 16;

    [[nodiscard]] constexpr vk::DeviceSize alignUp(vk::DeviceSize size, vk::DeviceSize alignment)
    {
        return (size + alignment - 1) / alignment * alignment;
    }

    // sections of one host-visible buffer, each starting aligned
    class InputLayout
    {
    public:
        [[nodiscard]] vk::DeviceSize add(vk::DeviceSize bytes)
        {
            const vk::DeviceSize offset = alignUp(size, kInputAlignment);
            size = offset + bytes;
            return offset;
        }
        [[nodiscard]] vk::DeviceSize bytes() const noexcept { return size; }

    private:
        vk::DeviceSize size = 0;
    };

    [[nodiscard]] std::string formatBytes(vk::DeviceSize bytes)
    {
        return std::format("{:.2f} MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }

    // build output readable by the next build, or by the host after the submit
    void buildBarrier(const vk::raii::CommandBuffer& cmd, vk::PipelineStageFlags2 dstStage, vk::AccessFlags2 dstAccess)
    {
        const vk::MemoryBarrier2 barrier{.srcStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                                         .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
                                         .dstStageMask = dstStage,
                                         .dstAccessMask = dstAccess};
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
    }

    [[nodiscard]] vk::StridedDeviceAddressRegionKHR stridedRegion(vk::DeviceAddress address, vk::DeviceSize stride,
                                                                  uint32_t count)
    {
        return {.deviceAddress = address, .stride = stride, .size = stride * count};
    }

    // places each structure at the next aligned offset of one allocation; returns the bytes it needs
    [[nodiscard]] vk::DeviceSize packOffsets(std::span<const uint32_t> sizes, vk::DeviceSize alignment,
                                             std::vector<vk::DeviceSize>& offsets)
    {
        offsets.resize(sizes.size());
        vk::DeviceSize total = 0;
        for (size_t i = 0; i < sizes.size(); ++i) {
            if (sizes[i] == 0) {
                throw std::runtime_error(std::format("cluster AS {} reported size 0", i));
            }
            offsets[i] = total;
            total = alignUp(total + sizes[i], alignment);
        }
        return total;
    }
} // namespace

MegaGeometry::OwnedBuffer::OwnedBuffer(OwnedBuffer&& other) noexcept
    : allocator(other.allocator),
      buffer(std::move(other.buffer)),
      memory(std::exchange(other.memory, nullptr)),
      address(std::exchange(other.address, 0)),
      mapped(std::exchange(other.mapped, nullptr)),
      bytes(std::exchange(other.bytes, 0))
{
}

MegaGeometry::OwnedBuffer& MegaGeometry::OwnedBuffer::operator=(OwnedBuffer&& other) noexcept
{
    if (this != &other) {
        reset();
        allocator = other.allocator;
        buffer = std::move(other.buffer);
        memory = std::exchange(other.memory, nullptr);
        address = std::exchange(other.address, 0);
        mapped = std::exchange(other.mapped, nullptr);
        bytes = std::exchange(other.bytes, 0);
    }
    return *this;
}

void MegaGeometry::OwnedBuffer::reset() noexcept
{
    destroyVmaBuffer(allocator, buffer, memory);
    address = 0;
    mapped = nullptr;
    bytes = 0;
}

MegaGeometry::MegaGeometry(const Device& device, const VkAllocator& allocator, FrameSync& frameSync)
    : device(device.vkdevice),
      queueFamilyIndices(device.queueFamilyIndices),
      limits(device.capabilities.clusterAccelerationStructure),
      allocator(allocator),
      frameSync(frameSync)
{
}

MegaGeometry::OwnedBuffer MegaGeometry::createOwnedBuffer(vk::DeviceSize bytes, vk::DeviceSize alignment,
                                                          vk::BufferUsageFlags2 usage, bool isHostVisible,
                                                          std::string_view debugName) const
{
    OwnedBuffer target;
    target.allocator = allocator.allocator;
    // the slack lets the start move up to the alignment the cluster properties ask for
    const vk::MemoryPropertyFlags properties = isHostVisible
        ? vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
        : vk::MemoryPropertyFlagBits::eDeviceLocal;
    createBuffer(bytes + alignment - 1, usage | vk::BufferUsageFlagBits2::eShaderDeviceAddress, properties,
                 target.buffer, target.memory, allocator.allocator, device, queueFamilyIndices, debugName,
                 isHostVisible ? VMA_ALLOCATION_CREATE_MAPPED_BIT : 0);
    const vk::DeviceAddress base = device.getBufferAddress({.buffer = *target.buffer});
    target.address = alignUp(base, alignment);
    target.bytes = bytes;
    if (isHostVisible) {
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(allocator.allocator, target.memory, &info);
        target.mapped = static_cast<std::byte*>(info.pMappedData) + (target.address - base);
    }
    return target;
}

template <typename Record>
void MegaGeometry::submit(Record&& record)
{
    const FrameSync::OneTimeCommands commands = frameSync.computeCommands();
    commands.cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    record(commands.cmd);
    submitAndWait(commands.cmd, commands.queue);
}

void MegaGeometry::buildModel(const ModelGeometry& geometry)
{
    ZoneScopedN("MegaGeometry::buildModel");
    ModelStructures model;
    model.blasAddresses.assign(geometry.primitives.size(), 0);
    // primitives without meshlets get no BLAS
    std::vector<uint32_t> blasPrimitives;
    for (uint32_t row = 0; row < geometry.primitives.size(); ++row) {
        if (geometry.primitives[row].meshletCount > 0) {
            blasPrimitives.push_back(row);
        }
    }
    if (blasPrimitives.empty()) {
        models.push_back(std::move(model));
        return;
    }

    const auto clusterCount = static_cast<uint32_t>(geometry.meshlets.size());
    const auto blasCount = static_cast<uint32_t>(blasPrimitives.size());
    vk::ClusterAccelerationStructureTriangleClusterInputNV triangleLimits{
        .vertexFormat = vk::Format::eR32G32B32Sfloat,
        .maxGeometryIndexValue = 0,
        .maxClusterUniqueGeometryCount = 1,
        // spec §10.2: no truncation until measured
        .minPositionTruncateBitCount = 0,
    };
    for (const GpuMeshletDesc& meshlet : geometry.meshlets) {
        triangleLimits.maxClusterTriangleCount = std::max<uint32_t>(triangleLimits.maxClusterTriangleCount,
                                                                    meshlet.triangleCount);
        triangleLimits.maxClusterVertexCount = std::max<uint32_t>(triangleLimits.maxClusterVertexCount,
                                                                  meshlet.vertexCount);
        triangleLimits.maxTotalTriangleCount += meshlet.triangleCount;
        triangleLimits.maxTotalVertexCount += meshlet.vertexCount;
    }
    if (triangleLimits.maxClusterTriangleCount > limits.maxTrianglesPerCluster ||
        triangleLimits.maxClusterVertexCount > limits.maxVerticesPerCluster) {
        throw std::runtime_error(std::format("meshlet of {} triangles / {} vertices exceeds the cluster AS limits {} / {}",
                                             triangleLimits.maxClusterTriangleCount,
                                             triangleLimits.maxClusterVertexCount, limits.maxTrianglesPerCluster,
                                             limits.maxVerticesPerCluster));
    }
    vk::ClusterAccelerationStructureClustersBottomLevelInputNV blasLimits{};
    for (const uint32_t row : blasPrimitives) {
        blasLimits.maxTotalClusterCount += geometry.primitives[row].meshletCount;
        blasLimits.maxClusterCountPerAccelerationStructure =
            std::max(blasLimits.maxClusterCountPerAccelerationStructure, geometry.primitives[row].meshletCount);
    }

    // data access: position fetch needs it (VUID-HitTriangleVertexPositionsKHR-None-08751, VUID-RuntimeSpirv-flags-08761)
    constexpr vk::BuildAccelerationStructureFlagsKHR kBuildFlags =
        vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace |
        vk::BuildAccelerationStructureFlagBitsKHR::eAllowDataAccess;
    vk::ClusterAccelerationStructureInputInfoNV clasInput{
        .maxAccelerationStructureCount = clusterCount,
        .flags = kBuildFlags,
        .opType = vk::ClusterAccelerationStructureOpTypeNV::eBuildTriangleCluster,
    };
    clasInput.opInput.pTriangleClusters = &triangleLimits;
    vk::ClusterAccelerationStructureInputInfoNV blasInput{
        .maxAccelerationStructureCount = blasCount,
        .flags = kBuildFlags,
        .opType = vk::ClusterAccelerationStructureOpTypeNV::eBuildClustersBottomLevel,
    };
    blasInput.opInput.pClustersBottomLevel = &blasLimits;

    // scratch serves every pass; the implicit query is the host's upper bound, logged against the real sizes
    vk::DeviceSize scratchBytes = 0;
    vk::DeviceSize clasEstimate = 0;
    for (vk::ClusterAccelerationStructureInputInfoNV* input : {&clasInput, &blasInput}) {
        for (const auto mode : {vk::ClusterAccelerationStructureOpModeNV::eImplicitDestinations,
                                vk::ClusterAccelerationStructureOpModeNV::eExplicitDestinations,
                                vk::ClusterAccelerationStructureOpModeNV::eComputeSizes}) {
            input->opMode = mode;
            const vk::AccelerationStructureBuildSizesInfoKHR sizes =
                device.getClusterAccelerationStructureBuildSizesNV(*input);
            scratchBytes = std::max(scratchBytes, sizes.buildScratchSize);
            if (input == &clasInput && mode == vk::ClusterAccelerationStructureOpModeNV::eImplicitDestinations) {
                clasEstimate = sizes.accelerationStructureSize;
            }
        }
    }

    using TriangleClusterInfo = vk::ClusterAccelerationStructureBuildTriangleClusterInfoNV;
    using BottomLevelInfo = vk::ClusterAccelerationStructureBuildClustersBottomLevelInfoNV;
    InputLayout layout;
    const vk::DeviceSize clasInfosOffset = layout.add(sizeof(TriangleClusterInfo) * clusterCount);
    const vk::DeviceSize blasInfosOffset = layout.add(sizeof(BottomLevelInfo) * blasCount);
    const vk::DeviceSize clasAddressesOffset = layout.add(sizeof(vk::DeviceAddress) * clusterCount);
    const vk::DeviceSize blasAddressesOffset = layout.add(sizeof(vk::DeviceAddress) * blasCount);
    const vk::DeviceSize sizesOffset = layout.add(sizeof(uint32_t) * std::max(clusterCount, blasCount));
    // cluster-local positions, parallel to meshletVertices; indices are the raster's 8-bit corners in raster order
    const vk::DeviceSize positionsOffset = layout.add(sizeof(glm::vec3) * geometry.meshletVertices.size());
    const vk::DeviceSize trianglesOffset = layout.add(geometry.meshletTriangles.size_bytes());

    OwnedBuffer inputs = createOwnedBuffer(
        layout.bytes(), kInputAlignment,
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eAccelerationStructureBuildInputReadOnlyKHR,
        true, "ClusterAsInputs");
    OwnedBuffer scratch = createOwnedBuffer(scratchBytes, limits.clusterScratchByteAlignment,
                                            vk::BufferUsageFlagBits2::eStorageBuffer, false, "ClusterAsScratch");
    const auto inputAddress = [&inputs](vk::DeviceSize offset) { return inputs.address + offset; };

    {
        std::vector<glm::vec3> positions(geometry.meshletVertices.size());
        std::ranges::transform(geometry.meshletVertices, positions.begin(),
                               [&](uint32_t vertex) { return geometry.vertices[vertex].pos; });
        std::memcpy(inputs.mapped + positionsOffset, positions.data(), std::span(positions).size_bytes());
    }
    std::memcpy(inputs.mapped + trianglesOffset, geometry.meshletTriangles.data(),
                geometry.meshletTriangles.size_bytes());
    std::vector<TriangleClusterInfo> clasInfos(clusterCount);
    for (uint32_t row = 0; row < clusterCount; ++row) {
        const GpuMeshletDesc& meshlet = geometry.meshlets[row];
        // clusterID is the meshlet row and the triangle order the raster's, so a hit decodes like a vis-buffer pixel
        clasInfos[row] = TriangleClusterInfo{
            .clusterID = row,
            .triangleCount = meshlet.triangleCount,
            .vertexCount = meshlet.vertexCount,
            .positionTruncateBitCount = triangleLimits.minPositionTruncateBitCount,
            .indexType = static_cast<uint32_t>(vk::ClusterAccelerationStructureIndexFormatFlagBitsNV::e8),
            // no micromaps until D (alpha)
            .opacityMicromapIndexType = 0,
            .indexBufferStride = sizeof(uint8_t),
            .vertexBufferStride = sizeof(glm::vec3),
            .indexBuffer = inputAddress(trianglesOffset + meshlet.triangleOffset),
            .vertexBuffer = inputAddress(positionsOffset + sizeof(glm::vec3) * meshlet.vertexOffset),
        };
    }
    std::memcpy(inputs.mapped + clasInfosOffset, clasInfos.data(), std::span(clasInfos).size_bytes());
    std::vector<BottomLevelInfo> blasInfos(blasCount);
    for (uint32_t i = 0; i < blasCount; ++i) {
        const GpuPrimitive& primitive = geometry.primitives[blasPrimitives[i]];
        // the primitive's meshlets are consecutive rows, so their CLAS addresses are too
        blasInfos[i] = BottomLevelInfo{
            .clusterReferencesCount = primitive.meshletCount,
            .clusterReferencesStride = sizeof(vk::DeviceAddress),
            .clusterReferences = inputAddress(clasAddressesOffset + sizeof(vk::DeviceAddress) * primitive.firstMeshlet),
        };
    }
    std::memcpy(inputs.mapped + blasInfosOffset, blasInfos.data(), std::span(blasInfos).size_bytes());

    const auto commandsInfo = [&](const vk::ClusterAccelerationStructureInputInfoNV& input,
                                  vk::ClusterAccelerationStructureOpModeNV mode, vk::DeviceSize infosOffset,
                                  vk::DeviceSize infoStride, vk::DeviceSize addressesOffset, uint32_t count) {
        vk::ClusterAccelerationStructureCommandsInfoNV info{
            .input = input,
            .scratchData = scratch.address,
            .srcInfosArray = stridedRegion(inputAddress(infosOffset), infoStride, count),
        };
        info.input.opMode = mode;
        if (mode == vk::ClusterAccelerationStructureOpModeNV::eComputeSizes) {
            info.dstSizesArray = stridedRegion(inputAddress(sizesOffset), sizeof(uint32_t), count);
        } else {
            info.dstAddressesArray =
                stridedRegion(inputAddress(addressesOffset), sizeof(vk::DeviceAddress), count);
        }
        return info;
    };
    const auto readSizes = [&](uint32_t count) {
        vmaInvalidateAllocation(allocator.allocator, inputs.memory, 0, VK_WHOLE_SIZE);
        const uint32_t* sizes = reinterpret_cast<const uint32_t*>(inputs.mapped + sizesOffset);
        return std::vector<uint32_t>(sizes, sizes + count);
    };
    // exact sizes: the host estimates are several times too large (clas_allocation.md)
    const auto placeStructures = [&](std::span<const uint32_t> sizes, vk::DeviceSize alignment,
                                     vk::DeviceSize addressesOffset, std::string_view name) {
        std::vector<vk::DeviceSize> offsets;
        const vk::DeviceSize bytes = packOffsets(sizes, alignment, offsets);
        OwnedBuffer target = createOwnedBuffer(bytes, alignment,
                                               vk::BufferUsageFlagBits2::eAccelerationStructureStorageKHR, false, name);
        auto* addresses = reinterpret_cast<vk::DeviceAddress*>(inputs.mapped + addressesOffset);
        for (size_t i = 0; i < offsets.size(); ++i) {
            addresses[i] = target.address + offsets[i];
        }
        return target;
    };

    constexpr auto kComputeSizes = vk::ClusterAccelerationStructureOpModeNV::eComputeSizes;
    constexpr auto kExplicit = vk::ClusterAccelerationStructureOpModeNV::eExplicitDestinations;
    submit([&](const vk::raii::CommandBuffer& cmd) {
        cmd.buildClusterAccelerationStructureIndirectNV(
            commandsInfo(clasInput, kComputeSizes, clasInfosOffset, sizeof(TriangleClusterInfo), 0, clusterCount));
        buildBarrier(cmd, vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead);
    });
    model.clas = placeStructures(readSizes(clusterCount), limits.clusterByteAlignment, clasAddressesOffset, "Clas");

    // BLAS sizes depend on the built CLAS they reference
    submit([&](const vk::raii::CommandBuffer& cmd) {
        cmd.buildClusterAccelerationStructureIndirectNV(commandsInfo(
            clasInput, kExplicit, clasInfosOffset, sizeof(TriangleClusterInfo), clasAddressesOffset, clusterCount));
        buildBarrier(cmd, vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                     vk::AccessFlagBits2::eAccelerationStructureReadKHR |
                         vk::AccessFlagBits2::eAccelerationStructureWriteKHR);
        cmd.buildClusterAccelerationStructureIndirectNV(
            commandsInfo(blasInput, kComputeSizes, blasInfosOffset, sizeof(BottomLevelInfo), 0, blasCount));
        buildBarrier(cmd, vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead);
    });
    model.blas =
        placeStructures(readSizes(blasCount), limits.clusterBottomLevelByteAlignment, blasAddressesOffset, "ClusterBlas");

    submit([&](const vk::raii::CommandBuffer& cmd) {
        cmd.buildClusterAccelerationStructureIndirectNV(commandsInfo(
            blasInput, kExplicit, blasInfosOffset, sizeof(BottomLevelInfo), blasAddressesOffset, blasCount));
    });

    const auto* blasAddresses = reinterpret_cast<const vk::DeviceAddress*>(inputs.mapped + blasAddressesOffset);
    for (uint32_t i = 0; i < blasCount; ++i) {
        model.blasAddresses[blasPrimitives[i]] = blasAddresses[i];
    }
    logInfo(std::format("model {}: {} CLAS in {} (host estimate {}), {} cluster BLAS for {} primitives in {}; "
                        "rt geometry total {}",
                        models.size(), clusterCount, formatBytes(model.clas.bytes), formatBytes(clasEstimate), blasCount,
                        geometry.primitives.size(), formatBytes(model.blas.bytes),
                        formatBytes(totalBytes() + model.clas.bytes + model.blas.bytes)),
            "MegaGeometry");
    models.push_back(std::move(model));
}

void MegaGeometry::eraseModel(ModelId modelId)
{
    models.erase(models.begin() + modelId);
    logInfo(std::format("erase model {}: rt geometry total {}", modelId, formatBytes(totalBytes())), "MegaGeometry");
}

vk::DeviceSize MegaGeometry::totalBytes() const noexcept
{
    return std::accumulate(models.begin(), models.end(), vk::DeviceSize{0},
                           [](vk::DeviceSize sum, const ModelStructures& model) {
                               return sum + model.clas.bytes + model.blas.bytes;
                           });
}
