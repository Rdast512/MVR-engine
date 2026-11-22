#include "vk_mega_geometry.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <numeric>
#include <stdexcept>
#include <utility>
#include "static_headers/logger.hpp"
#include "util/debug.hpp"
#include "util/vk_tracy.hpp"
#include "util/vk_utils.hpp"

namespace
{
    // CPU-written build inputs; keeps every section 16 B aligned
    constexpr vk::DeviceSize kInputAlignment = 16;
    // micromap data and triangle arrays (VUID-vkCmdBuildMicromapsEXT-pInfos-07515), micromap storage offsets
    // (VUID-VkMicromapCreateInfoEXT-offset-07438)
    constexpr vk::DeviceSize kMicromapAlignment = 256;

    // a micromap needs one triangle; a bake of special indices only never references it
    constexpr std::array<uint8_t, 1> kUnreferencedMicromapData{};
    constexpr std::array<VkMicromapTriangleEXT, 1> kUnreferencedMicromapTriangle{
        {{.dataOffset = 0, .subdivisionLevel = 0, .format = VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT}}};
    constexpr std::array<VkMicromapUsageEXT, 1> kUnreferencedMicromapUsage{
        {{.count = 1, .subdivisionLevel = 0, .format = VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT}}};

    [[nodiscard]] constexpr vk::DeviceSize alignUp(vk::DeviceSize size, vk::DeviceSize alignment)
    {
        return (size + alignment - 1) / alignment * alignment;
    }

    // sections of one host-visible buffer, each starting aligned
    class InputLayout
    {
    public:
        [[nodiscard]] vk::DeviceSize add(vk::DeviceSize bytes, vk::DeviceSize alignment = kInputAlignment)
        {
            const vk::DeviceSize offset = alignUp(size, alignment);
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

    // micromaps readable by the CLAS build; the CLAS passes then reuse the scratch the micromap builds wrote
    void micromapBarrier(const vk::raii::CommandBuffer& cmd)
    {
        const vk::MemoryBarrier2 barrier{.srcStageMask = vk::PipelineStageFlagBits2::eMicromapBuildEXT,
                                         .srcAccessMask = vk::AccessFlagBits2::eMicromapWriteEXT,
                                         .dstStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                                         .dstAccessMask = vk::AccessFlagBits2::eMicromapReadEXT |
                                                          vk::AccessFlagBits2::eAccelerationStructureReadKHR |
                                                          vk::AccessFlagBits2::eAccelerationStructureWriteKHR};
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
    }

    [[nodiscard]] vk::ClusterAccelerationStructureIndexFormatFlagBitsNV clusterIndexFormat(VkIndexType type)
    {
        return type == VK_INDEX_TYPE_UINT16 ? vk::ClusterAccelerationStructureIndexFormatFlagBitsNV::e16
                                            : vk::ClusterAccelerationStructureIndexFormatFlagBitsNV::e32;
    }

    [[nodiscard]] uint16_t indexStride(VkIndexType type)
    {
        return type == VK_INDEX_TYPE_UINT16 ? sizeof(uint16_t) : sizeof(uint32_t);
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
      scratchOffsetAlignment(device.capabilities.accelerationStructure.minAccelerationStructureScratchOffsetAlignment),
      allocator(allocator),
      frameSync(frameSync)
{
}

MegaGeometry::OwnedBuffer MegaGeometry::createOwnedBuffer(vk::DeviceSize bytes, vk::DeviceSize alignment,
                                                          vk::BufferUsageFlags2 usage, BufferMemory memoryMode,
                                                          std::string_view debugName) const
{
    OwnedBuffer target;
    target.allocator = allocator.allocator;
    vk::MemoryPropertyFlags properties = vk::MemoryPropertyFlagBits::eDeviceLocal;
    VmaAllocationCreateFlags allocFlags = 0;
    if (memoryMode == BufferMemory::HostWrite) {
        properties = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
        allocFlags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    } else if (memoryMode == BufferMemory::HostReadback) {
        properties = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent |
                     vk::MemoryPropertyFlagBits::eHostCached;
        allocFlags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    // MG-B1: align the buffer allocation directly via VMA so offset 0 satisfies hardware and VUID alignment
    createBuffer(std::max<vk::DeviceSize>(bytes, 1), usage | vk::BufferUsageFlagBits2::eShaderDeviceAddress, properties,
                 target.buffer, target.memory, allocator.allocator, device, queueFamilyIndices, debugName, allocFlags,
                 alignment);
    const vk::DeviceAddress base = device.getBufferAddress({.buffer = *target.buffer});
    if (alignment > 1 && base % alignment != 0) {
        throw std::runtime_error(
            std::format("MegaGeometry: {} address {:#x} is not {}-byte aligned", debugName, base, alignment));
    }
    target.address = base;
    target.bytes = bytes;
    if (memoryMode != BufferMemory::DeviceLocal) {
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(allocator.allocator, target.memory, &info);
        target.mapped = static_cast<std::byte*>(info.pMappedData);
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
    const auto start = std::chrono::steady_clock::now();
    ModelStructures model;
    model.blasAddresses.assign(geometry.primitives.size(), 0);
    model.micromapFlags.assign(geometry.primitives.size(), 0);
    if (!geometry.ommBakes.empty() && geometry.ommBakes.size() != geometry.primitives.size()) {
        throw std::logic_error(std::format("{} OMM bakes for {} primitives", geometry.ommBakes.size(),
                                           geometry.primitives.size()));
    }
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
    vk::ClusterAccelerationStructureClustersBottomLevelInputNV blasLimits{};
    for (const uint32_t row : blasPrimitives) {
        blasLimits.maxTotalClusterCount += geometry.primitives[row].meshletCount;
        blasLimits.maxClusterCountPerAccelerationStructure =
            std::max(blasLimits.maxClusterCountPerAccelerationStructure, geometry.primitives[row].meshletCount);
    }

    // one micromap per baked primitive, in micromap order
    struct MicromapPlan {
        uint32_t primitive = 0;
        std::span<const uint8_t> data;
        std::span<const VkMicromapTriangleEXT> triangles;
        std::span<const VkMicromapUsageEXT> usages;
        vk::MicromapBuildSizesInfoEXT sizes;
        vk::DeviceSize storageOffset = 0;
        vk::DeviceSize scratchOffset = 0;
        vk::DeviceSize dataOffset = 0;
        vk::DeviceSize trianglesOffset = 0;
        vk::DeviceSize indexOffset = 0;
    };
    const auto micromapBuildInfo = [](const MicromapPlan& plan) {
        return vk::MicromapBuildInfoEXT{
            .type = vk::MicromapTypeEXT::eOpacityMicromap,
            .flags = vk::BuildMicromapFlagBitsEXT::ePreferFastTrace,
            .mode = vk::BuildMicromapModeEXT::eBuild,
            .usageCountsCount = static_cast<uint32_t>(plan.usages.size()),
            // VkMicromapUsageEXT and vk::MicromapUsageEXT share one layout
            .pUsageCounts = reinterpret_cast<const vk::MicromapUsageEXT*>(plan.usages.data()),
            .triangleArrayStride = sizeof(VkMicromapTriangleEXT),
        };
    };
    std::vector<MicromapPlan> micromapPlans;
    std::vector<uint8_t> clusterHasOmm(clusterCount, 0);
    vk::DeviceSize micromapStorageBytes = 0;
    vk::DeviceSize micromapScratchBytes = 0;
    for (const uint32_t row : blasPrimitives) {
        if (geometry.ommBakes.empty() || geometry.ommBakes[row].triangleCount == 0) {
            continue;
        }
        const OmmBake& bake = geometry.ommBakes[row];
        const GpuPrimitive& primitive = geometry.primitives[row];
        uint32_t clusterTriangles = 0;
        for (uint32_t m = primitive.firstMeshlet; m < primitive.firstMeshlet + primitive.meshletCount; ++m) {
            clusterTriangles += geometry.meshlets[m].triangleCount;
            clusterHasOmm[m] = 1;
        }
        // the OMM index runs over the cluster triangles in cluster order (D1)
        if (bake.triangleCount != clusterTriangles) {
            throw std::logic_error(std::format("primitive {}: OMM bake of {} triangles, clusters hold {}", row,
                                               bake.triangleCount, clusterTriangles));
        }
        const bool isUnreferenced = bake.triangles.empty();
        MicromapPlan plan{
            .primitive = row,
            .data = isUnreferenced ? std::span<const uint8_t>(kUnreferencedMicromapData) : bake.arrayData,
            .triangles = isUnreferenced ? std::span<const VkMicromapTriangleEXT>(kUnreferencedMicromapTriangle)
                                        : bake.triangles,
            .usages = isUnreferenced ? std::span<const VkMicromapUsageEXT>(kUnreferencedMicromapUsage) : bake.usages,
            .sizes = {},
        };
        plan.sizes = device.getMicromapBuildSizesEXT(vk::AccelerationStructureBuildTypeKHR::eDevice,
                                                     micromapBuildInfo(plan));
        plan.storageOffset = alignUp(micromapStorageBytes, kMicromapAlignment);
        micromapStorageBytes = plan.storageOffset + plan.sizes.micromapSize;
        // builds of one call must not share scratch (VUID-vkCmdBuildMicromapsEXT-scratchData-11651)
        plan.scratchOffset = alignUp(micromapScratchBytes, scratchOffsetAlignment);
        micromapScratchBytes = plan.scratchOffset + plan.sizes.buildScratchSize;
        micromapPlans.push_back(plan);
    }

    // m8 MG-P1: split opaque and OMM clusters into disjoint CLAS builds so opaque clusters never pay for
    // eAllowClusterOpacityMicromapsNV
    std::vector<uint32_t> opaqueClusters;
    std::vector<uint32_t> ommClusters;
    opaqueClusters.reserve(clusterCount);
    for (uint32_t row = 0; row < clusterCount; ++row) {
        if (clusterHasOmm[row] != 0) {
            ommClusters.push_back(row);
        } else {
            opaqueClusters.push_back(row);
        }
    }
    const auto computeTriangleLimits = [&](std::span<const uint32_t> rows) {
        vk::ClusterAccelerationStructureTriangleClusterInputNV triLimits{
            .vertexFormat = vk::Format::eR32G32B32Sfloat,
            .maxGeometryIndexValue = 0,
            .maxClusterUniqueGeometryCount = 1,
            .minPositionTruncateBitCount = 0,
        };
        for (const uint32_t row : rows) {
            const GpuMeshletDesc& meshlet = geometry.meshlets[row];
            triLimits.maxClusterTriangleCount =
                std::max<uint32_t>(triLimits.maxClusterTriangleCount, meshlet.triangleCount);
            triLimits.maxClusterVertexCount =
                std::max<uint32_t>(triLimits.maxClusterVertexCount, meshlet.vertexCount);
            triLimits.maxTotalTriangleCount += meshlet.triangleCount;
            triLimits.maxTotalVertexCount += meshlet.vertexCount;
        }
        if (triLimits.maxClusterTriangleCount > limits.maxTrianglesPerCluster ||
            triLimits.maxClusterVertexCount > limits.maxVerticesPerCluster) {
            throw std::runtime_error(
                std::format("meshlet of {} triangles / {} vertices exceeds the cluster AS limits {} / {}",
                            triLimits.maxClusterTriangleCount, triLimits.maxClusterVertexCount,
                            limits.maxTrianglesPerCluster, limits.maxVerticesPerCluster));
        }
        return triLimits;
    };
    vk::ClusterAccelerationStructureTriangleClusterInputNV opaqueTriangleLimits =
        computeTriangleLimits(opaqueClusters);
    vk::ClusterAccelerationStructureTriangleClusterInputNV ommTriangleLimits = computeTriangleLimits(ommClusters);

    // data access: position fetch needs it (VUID-HitTriangleVertexPositionsKHR-None-08751, VUID-RuntimeSpirv-flags-08761)
    constexpr vk::BuildAccelerationStructureFlagsKHR kBuildFlags =
        vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace |
        vk::BuildAccelerationStructureFlagBitsKHR::eAllowDataAccess;
    vk::ClusterAccelerationStructureInputInfoNV opaqueClasInput{
        .maxAccelerationStructureCount = static_cast<uint32_t>(opaqueClusters.size()),
        .flags = kBuildFlags,
        .opType = vk::ClusterAccelerationStructureOpTypeNV::eBuildTriangleCluster,
    };
    opaqueClasInput.opInput.pTriangleClusters = &opaqueTriangleLimits;
    vk::ClusterAccelerationStructureInputInfoNV ommClasInput{
        .maxAccelerationStructureCount = static_cast<uint32_t>(ommClusters.size()),
        // in the size query too (VUID-VkClusterAccelerationStructureBuildTriangleClusterInfoNV-opacityMicromapArray-10881)
        .flags = kBuildFlags | vk::BuildAccelerationStructureFlagBitsKHR::eAllowClusterOpacityMicromapsNV,
        .opType = vk::ClusterAccelerationStructureOpTypeNV::eBuildTriangleCluster,
    };
    ommClasInput.opInput.pTriangleClusters = &ommTriangleLimits;
    vk::ClusterAccelerationStructureInputInfoNV blasInput{
        .maxAccelerationStructureCount = blasCount,
        .flags = kBuildFlags,
        .opType = vk::ClusterAccelerationStructureOpTypeNV::eBuildClustersBottomLevel,
    };
    blasInput.opInput.pClustersBottomLevel = &blasLimits;

    // MG-P6: scratch serves only eComputeSizes and eExplicitDestinations; eImplicitDestinations is queried solely for
    // the clasEstimate log and must not inflate buildScratchSize
    vk::DeviceSize scratchBytes = 0;
    vk::DeviceSize implicitScratchBytes = 0;
    vk::DeviceSize clasEstimate = 0;
    for (vk::ClusterAccelerationStructureInputInfoNV* input : {&opaqueClasInput, &ommClasInput, &blasInput}) {
        if (input->maxAccelerationStructureCount == 0) {
            continue;
        }
        for (const auto mode : {vk::ClusterAccelerationStructureOpModeNV::eExplicitDestinations,
                                vk::ClusterAccelerationStructureOpModeNV::eComputeSizes}) {
            input->opMode = mode;
            const vk::AccelerationStructureBuildSizesInfoKHR sizes =
                device.getClusterAccelerationStructureBuildSizesNV(*input);
            scratchBytes = std::max(scratchBytes, sizes.buildScratchSize);
        }
        input->opMode = vk::ClusterAccelerationStructureOpModeNV::eImplicitDestinations;
        const vk::AccelerationStructureBuildSizesInfoKHR implicitSizes =
            device.getClusterAccelerationStructureBuildSizesNV(*input);
        implicitScratchBytes = std::max(implicitScratchBytes, implicitSizes.buildScratchSize);
        if (input != &blasInput) {
            clasEstimate += implicitSizes.accelerationStructureSize;
        }
    }

    using TriangleClusterInfo = vk::ClusterAccelerationStructureBuildTriangleClusterInfoNV;
    using BottomLevelInfo = vk::ClusterAccelerationStructureBuildClustersBottomLevelInfoNV;
    const auto opaqueCount = static_cast<uint32_t>(opaqueClusters.size());
    const auto ommCount = static_cast<uint32_t>(ommClusters.size());
    InputLayout layout;
    // opaque and OMM clusters packed contiguously in split order for their indirect CLAS builds
    const vk::DeviceSize clasInfosOffset = layout.add(sizeof(TriangleClusterInfo) * clusterCount);
    const vk::DeviceSize blasInfosOffset = layout.add(sizeof(BottomLevelInfo) * blasCount);
    // destination addresses in split order (read/written by CLAS explicit build)
    const vk::DeviceSize clasDstAddressesOffset = layout.add(sizeof(vk::DeviceAddress) * clusterCount);
    // model-relative meshlet order (read by Cluster BLAS build so each primitive's clusters stay contiguous)
    const vk::DeviceSize clasAddressesOffset = layout.add(sizeof(vk::DeviceAddress) * clusterCount);
    const vk::DeviceSize blasAddressesOffset = layout.add(sizeof(vk::DeviceAddress) * blasCount);
    // cluster-local positions, parallel to meshletVertices; indices are the raster's 8-bit corners in raster order
    const vk::DeviceSize positionsOffset = layout.add(sizeof(glm::vec3) * geometry.meshletVertices.size());
    const vk::DeviceSize trianglesOffset = layout.add(geometry.meshletTriangles.size_bytes());
    // micromap build inputs and the clusters' OMM indices; the CLAS keeps neither after its build
    for (MicromapPlan& plan : micromapPlans) {
        plan.dataOffset = layout.add(plan.data.size_bytes(), kMicromapAlignment);
        plan.trianglesOffset = layout.add(plan.triangles.size_bytes(), kMicromapAlignment);
        plan.indexOffset = layout.add(geometry.ommBakes[plan.primitive].indices.size());
    }

    // the base alignment keeps the micromap sections 256 B aligned as addresses, not just as offsets
    OwnedBuffer inputs = createOwnedBuffer(
        layout.bytes(), kMicromapAlignment,
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eAccelerationStructureBuildInputReadOnlyKHR |
            vk::BufferUsageFlagBits2::eMicromapBuildInputReadOnlyEXT,
        BufferMemory::HostWrite, "ClusterAsInputs");
    // MG-B2: GPU -> CPU size readback in host-cached memory instead of uncached WC VRAM
    OwnedBuffer sizesReadback =
        createOwnedBuffer(sizeof(uint32_t) * std::max(clusterCount, blasCount), alignof(uint32_t),
                          vk::BufferUsageFlagBits2::eStorageBuffer, BufferMemory::HostReadback,
                          "ClusterAsSizesReadback");
    // the micromap builds and every cluster pass take turns on one scratch buffer
    OwnedBuffer scratch = createOwnedBuffer(std::max(scratchBytes, micromapScratchBytes),
                                            std::max<vk::DeviceSize>(limits.clusterScratchByteAlignment,
                                                                     scratchOffsetAlignment),
                                            vk::BufferUsageFlagBits2::eStorageBuffer, BufferMemory::DeviceLocal,
                                            "ClusterAsScratch");
    const auto inputAddress = [&inputs](vk::DeviceSize offset) { return inputs.address + offset; };

    std::vector<vk::DeviceAddress> micromapAddresses;
    if (!micromapPlans.empty()) {
        const std::string storageName = std::format("MicromapStorage_{}", models.size());
        model.micromapStorage = createOwnedBuffer(micromapStorageBytes, kMicromapAlignment,
                                                  vk::BufferUsageFlagBits2::eMicromapStorageEXT,
                                                  BufferMemory::DeviceLocal, storageName);
        model.micromaps.reserve(micromapPlans.size());
        for (const MicromapPlan& plan : micromapPlans) {
            // MG-B1: storage starts 256 B aligned and plan.storageOffset is a multiple of 256 B
            // (VUID-VkMicromapCreateInfoEXT-offset-07438)
            model.micromaps.emplace_back(device, vk::MicromapCreateInfoEXT{
                                                     .buffer = *model.micromapStorage.buffer,
                                                     .offset = plan.storageOffset,
                                                     .size = plan.sizes.micromapSize,
                                                     .type = vk::MicromapTypeEXT::eOpacityMicromap,
                                                 });
            setDebugName(device, model.micromaps.back(), std::format("Micromap_{}", plan.primitive));
            // EXT has no address query: the micromap lives at its storage address (D2 §1, verified at load)
            micromapAddresses.push_back(model.micromapStorage.address + plan.storageOffset);
            model.micromapFlags[plan.primitive] = 1;

            const OmmBake& bake = geometry.ommBakes[plan.primitive];
            std::memcpy(inputs.mapped + plan.dataOffset, plan.data.data(), plan.data.size_bytes());
            std::memcpy(inputs.mapped + plan.trianglesOffset, plan.triangles.data(), plan.triangles.size_bytes());
            std::memcpy(inputs.mapped + plan.indexOffset, bake.indices.data(), bake.indices.size());
        }
    }

    {
        std::vector<glm::vec3> positions(geometry.meshletVertices.size());
        std::ranges::transform(geometry.meshletVertices, positions.begin(),
                               [&](uint32_t vertex) { return geometry.vertices[vertex].pos; });
        std::memcpy(inputs.mapped + positionsOffset, positions.data(), std::span(positions).size_bytes());
    }
    std::memcpy(inputs.mapped + trianglesOffset, geometry.meshletTriangles.data(),
                geometry.meshletTriangles.size_bytes());
    std::vector<TriangleClusterInfo> clasInfosByRow(clusterCount);
    for (uint32_t row = 0; row < clusterCount; ++row) {
        const GpuMeshletDesc& meshlet = geometry.meshlets[row];
        // clusterID is the meshlet row and the triangle order the raster's, so a hit decodes like a vis-buffer pixel
        clasInfosByRow[row] = TriangleClusterInfo{
            .clusterID = row,
            .triangleCount = meshlet.triangleCount,
            .vertexCount = meshlet.vertexCount,
            .positionTruncateBitCount = 0,
            .indexType = static_cast<uint32_t>(vk::ClusterAccelerationStructureIndexFormatFlagBitsNV::e8),
            // set below for the clusters of baked primitives; opacityMicromapArray 0 disables OMMs
            .opacityMicromapIndexType = 0,
            .indexBufferStride = sizeof(uint8_t),
            .vertexBufferStride = sizeof(glm::vec3),
            .indexBuffer = inputAddress(trianglesOffset + meshlet.triangleOffset),
            .vertexBuffer = inputAddress(positionsOffset + sizeof(glm::vec3) * meshlet.vertexOffset),
        };
    }
    for (size_t i = 0; i < micromapPlans.size(); ++i) {
        const MicromapPlan& plan = micromapPlans[i];
        const GpuPrimitive& primitive = geometry.primitives[plan.primitive];
        const VkIndexType indexType = geometry.ommBakes[plan.primitive].indexType;
        // the primitive's index has one entry per cluster triangle in cluster order: each cluster takes its run
        vk::DeviceSize firstTriangle = 0;
        for (uint32_t row = primitive.firstMeshlet; row < primitive.firstMeshlet + primitive.meshletCount; ++row) {
            TriangleClusterInfo& info = clasInfosByRow[row];
            info.opacityMicromapIndexType = static_cast<uint32_t>(clusterIndexFormat(indexType));
            info.opacityMicromapIndexBufferStride = indexStride(indexType);
            info.opacityMicromapArray = micromapAddresses[i];
            info.opacityMicromapIndexBuffer = inputAddress(plan.indexOffset + firstTriangle * indexStride(indexType));
            firstTriangle += geometry.meshlets[row].triangleCount;
        }
    }
    // split order: [0, opaqueCount) opaque clusters, [opaqueCount, clusterCount) OMM clusters
    std::vector<uint32_t> splitOrder;
    splitOrder.reserve(clusterCount);
    splitOrder.insert(splitOrder.end(), opaqueClusters.begin(), opaqueClusters.end());
    splitOrder.insert(splitOrder.end(), ommClusters.begin(), ommClusters.end());
    std::vector<TriangleClusterInfo> clasInfos(clusterCount);
    for (uint32_t i = 0; i < clusterCount; ++i) {
        clasInfos[i] = clasInfosByRow[splitOrder[i]];
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
                                  vk::DeviceSize infoStride, vk::DeviceSize addressesOffset, uint32_t count,
                                  uint32_t sizesOffsetElements = 0) {
        vk::ClusterAccelerationStructureCommandsInfoNV info{
            .input = input,
            .scratchData = scratch.address,
            .srcInfosArray = stridedRegion(inputAddress(infosOffset), infoStride, count),
        };
        info.input.opMode = mode;
        if (mode == vk::ClusterAccelerationStructureOpModeNV::eComputeSizes) {
            info.dstSizesArray =
                stridedRegion(sizesReadback.address + vk::DeviceSize{sizesOffsetElements} * sizeof(uint32_t),
                              sizeof(uint32_t), count);
        } else {
            info.dstAddressesArray =
                stridedRegion(inputAddress(addressesOffset), sizeof(vk::DeviceAddress), count);
        }
        return info;
    };
    const auto readSizes = [&](uint32_t count) {
        vmaInvalidateAllocation(allocator.allocator, sizesReadback.memory, 0, VK_WHOLE_SIZE);
        const auto* sizes = reinterpret_cast<const uint32_t*>(sizesReadback.mapped);
        return std::vector<uint32_t>(sizes, sizes + count);
    };
    // exact sizes: the host estimates are several times too large (clas_allocation.md)
    const auto placeStructures = [&](std::span<const uint32_t> sizes, vk::DeviceSize alignment,
                                     vk::DeviceSize addressesOffset, std::string_view name,
                                     std::vector<vk::DeviceAddress>* outAddresses = nullptr) {
        std::vector<vk::DeviceSize> offsets;
        const vk::DeviceSize bytes = packOffsets(sizes, alignment, offsets);
        OwnedBuffer target = createOwnedBuffer(bytes, alignment,
                                               vk::BufferUsageFlagBits2::eAccelerationStructureStorageKHR,
                                               BufferMemory::DeviceLocal, name);
        std::vector<vk::DeviceAddress> addresses(offsets.size());
        for (size_t i = 0; i < offsets.size(); ++i) {
            addresses[i] = target.address + offsets[i];
        }
        std::memcpy(inputs.mapped + addressesOffset, addresses.data(), std::span(addresses).size_bytes());
        if (outAddresses != nullptr) {
            *outAddresses = std::move(addresses);
        }
        return target;
    };

    constexpr auto kComputeSizes = vk::ClusterAccelerationStructureOpModeNV::eComputeSizes;
    constexpr auto kExplicit = vk::ClusterAccelerationStructureOpModeNV::eExplicitDestinations;
    std::vector<vk::MicromapBuildInfoEXT> micromapBuilds;
    micromapBuilds.reserve(micromapPlans.size());
    for (size_t i = 0; i < micromapPlans.size(); ++i) {
        const MicromapPlan& plan = micromapPlans[i];
        vk::MicromapBuildInfoEXT info = micromapBuildInfo(plan);
        info.dstMicromap = *model.micromaps[i];
        info.scratchData.deviceAddress = scratch.address + plan.scratchOffset;
        info.data.deviceAddress = inputAddress(plan.dataOffset);
        info.triangleArray.deviceAddress = inputAddress(plan.trianglesOffset);
        micromapBuilds.push_back(info);
    }

    const vk::DeviceSize ommInfosOffset = clasInfosOffset + sizeof(TriangleClusterInfo) * opaqueCount;
    const vk::DeviceSize ommDstAddressesOffset = clasDstAddressesOffset + sizeof(vk::DeviceAddress) * opaqueCount;

    // micromaps before every CLAS pass that references them, in the same submit (D2 must-not)
    submit([&](const vk::raii::CommandBuffer& cmd) {
        if (!micromapBuilds.empty()) {
            cmd.buildMicromapsEXT(micromapBuilds);
            micromapBarrier(cmd);
        }
        if (opaqueCount > 0) {
            cmd.buildClusterAccelerationStructureIndirectNV(commandsInfo(
                opaqueClasInput, kComputeSizes, clasInfosOffset, sizeof(TriangleClusterInfo), 0, opaqueCount, 0));
            if (ommCount > 0) {
                buildBarrier(cmd, vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                             vk::AccessFlagBits2::eAccelerationStructureReadKHR |
                                 vk::AccessFlagBits2::eAccelerationStructureWriteKHR);
            }
        }
        if (ommCount > 0) {
            cmd.buildClusterAccelerationStructureIndirectNV(commandsInfo(
                ommClasInput, kComputeSizes, ommInfosOffset, sizeof(TriangleClusterInfo), 0, ommCount, opaqueCount));
        }
        buildBarrier(cmd, vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead);
    });
    // pack CLAS in original meshlet row order so each primitive's clusters stay contiguous in VRAM, then scatter
    // destination addresses into split order for the opaque and OMM indirect builds
    const std::vector<uint32_t> splitSizes = readSizes(clusterCount);
    std::vector<uint32_t> rowSizes(clusterCount, 0);
    for (uint32_t i = 0; i < clusterCount; ++i) {
        rowSizes[splitOrder[i]] = splitSizes[i];
    }
    std::vector<vk::DeviceAddress> clasByRow;
    model.clas = placeStructures(rowSizes, limits.clusterByteAlignment, clasAddressesOffset, "Clas", &clasByRow);
    std::vector<vk::DeviceAddress> splitClasAddresses(clusterCount, 0);
    for (uint32_t i = 0; i < clusterCount; ++i) {
        splitClasAddresses[i] = clasByRow[splitOrder[i]];
    }
    std::memcpy(inputs.mapped + clasDstAddressesOffset, splitClasAddresses.data(),
                std::span(splitClasAddresses).size_bytes());

    // BLAS sizes depend on the built CLAS they reference
    submit([&](const vk::raii::CommandBuffer& cmd) {
        if (opaqueCount > 0) {
            cmd.buildClusterAccelerationStructureIndirectNV(commandsInfo(opaqueClasInput, kExplicit, clasInfosOffset,
                                                                         sizeof(TriangleClusterInfo),
                                                                         clasDstAddressesOffset, opaqueCount));
            buildBarrier(cmd, vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                         vk::AccessFlagBits2::eAccelerationStructureReadKHR |
                             vk::AccessFlagBits2::eAccelerationStructureWriteKHR);
        }
        if (ommCount > 0) {
            cmd.buildClusterAccelerationStructureIndirectNV(commandsInfo(ommClasInput, kExplicit, ommInfosOffset,
                                                                         sizeof(TriangleClusterInfo),
                                                                         ommDstAddressesOffset, ommCount));
            buildBarrier(cmd, vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                         vk::AccessFlagBits2::eAccelerationStructureReadKHR |
                             vk::AccessFlagBits2::eAccelerationStructureWriteKHR);
        }
        cmd.buildClusterAccelerationStructureIndirectNV(
            commandsInfo(blasInput, kComputeSizes, blasInfosOffset, sizeof(BottomLevelInfo), 0, blasCount));
        buildBarrier(cmd, vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead);
    });
    std::vector<vk::DeviceAddress> builtBlasAddresses;
    model.blas = placeStructures(readSizes(blasCount), limits.clusterBottomLevelByteAlignment, blasAddressesOffset,
                                 "ClusterBlas", &builtBlasAddresses);

    submit([&](const vk::raii::CommandBuffer& cmd) {
        cmd.buildClusterAccelerationStructureIndirectNV(commandsInfo(
            blasInput, kExplicit, blasInfosOffset, sizeof(BottomLevelInfo), blasAddressesOffset, blasCount));
    });

    for (uint32_t i = 0; i < blasCount; ++i) {
        model.blasAddresses[blasPrimitives[i]] = builtBlasAddresses[i];
    }
    // host preparation and the waited submits
    const double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    logInfo(std::format("model {}: {} CLAS in {} (host estimate {}), {} cluster BLAS for {} primitives in {}, "
                        "{} micromaps in {}, scratch {} (implicit destinations {}); built in {:.1f} ms; rt geometry "
                        "total {}",
                        models.size(), clusterCount, formatBytes(model.clas.bytes), formatBytes(clasEstimate), blasCount,
                        geometry.primitives.size(), formatBytes(model.blas.bytes), model.micromaps.size(),
                        formatBytes(model.micromapStorage.bytes), formatBytes(scratch.bytes),
                        formatBytes(implicitScratchBytes), buildMs,
                        formatBytes(totalBytes() + model.clas.bytes + model.blas.bytes + model.micromapStorage.bytes)),
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
                               return sum + model.clas.bytes + model.blas.bytes + model.micromapStorage.bytes;
                           });
}
