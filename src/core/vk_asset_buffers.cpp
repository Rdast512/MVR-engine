#include "vk_asset_buffers.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <format>
#include <span>
#include <stdexcept>
#include <utility>
#include "Constants.h"
#include "static_headers/logger.hpp"
#include "util/debug.hpp"
#include "util/vk_tracy.hpp"
#include "util/vk_utils.hpp"

namespace
{
    // runs on every exit from its scope, also during unwinding
    template <std::invocable F>
    class ScopeExit
    {
    public:
        explicit ScopeExit(F onExit) : onExit(std::move(onExit)) {}
        ~ScopeExit() { onExit(); }

        ScopeExit(const ScopeExit&) = delete;
        ScopeExit& operator=(const ScopeExit&) = delete;
        ScopeExit(ScopeExit&&) = delete;
        ScopeExit& operator=(ScopeExit&&) = delete;

    private:
        F onExit;
    };

    struct AssetBufferInfo {
        const char* debugName;
        const char* tracyName;
        // Tracy keeps plot name pointers, so these must be literals
        const char* plotUsed;
        const char* plotCapacity;
        vk::DeviceSize stride;
    };

    // rows follow AssetBuffer order
    constexpr std::array<AssetBufferInfo, kAssetBufferCount> kAssetBufferInfo{{
        {"VertexBuffer", "GPU/Vertices", "Vulkan/VertexBytes", "Vulkan/VertexCapacityBytes", sizeof(GpuVertex)},
        {"MeshletBuffer", "GPU/Meshlets", "Vulkan/MeshletBytes", "Vulkan/MeshletCapacityBytes",
         sizeof(GpuMeshletDesc)},
        {"MeshletBoundsBuffer", "GPU/MeshletBounds", "Vulkan/MeshletBoundsBytes", "Vulkan/MeshletBoundsCapacityBytes",
         sizeof(GpuMeshletBounds)},
        {"MeshletGroupBuffer", "GPU/MeshletGroups", "Vulkan/MeshletGroupBytes", "Vulkan/MeshletGroupCapacityBytes",
         sizeof(GpuMeshletGroup)},
        {"MeshletPrimitiveBuffer", "GPU/MeshletPrimitives", "Vulkan/MeshletPrimitiveBytes",
         "Vulkan/MeshletPrimitiveCapacityBytes", sizeof(uint32_t)},
        {"MeshletVertexBuffer", "GPU/MeshletVertices", "Vulkan/MeshletVertexBytes",
         "Vulkan/MeshletVertexCapacityBytes", sizeof(uint32_t)},
        {"MeshletTriangleBuffer", "GPU/MeshletTriangles", "Vulkan/MeshletTriangleBytes",
         "Vulkan/MeshletTriangleCapacityBytes", sizeof(uint8_t)},
        {"NormalBuffer", "GPU/Normals", "Vulkan/NormalBytes", "Vulkan/NormalCapacityBytes", sizeof(glm::vec3)},
        {"TangentBuffer", "GPU/Tangents", "Vulkan/TangentBytes", "Vulkan/TangentCapacityBytes", sizeof(glm::vec4)},
        {"Uv1Buffer", "GPU/Uv1", "Vulkan/Uv1Bytes", "Vulkan/Uv1CapacityBytes", sizeof(glm::vec2)},
        {"IndexBuffer", "GPU/Indices", "Vulkan/IndexBytes", "Vulkan/IndexCapacityBytes", sizeof(uint32_t)},
        {"MaterialBuffer", "GPU/Materials", "Vulkan/MaterialBytes", "Vulkan/MaterialCapacityBytes",
         sizeof(GpuMaterial)},
        {"PbrExtBuffer", "GPU/PbrExtensions", "Vulkan/PbrExtBytes", "Vulkan/PbrExtCapacityBytes",
         sizeof(GpuMaterialExtension)},
        {"LightBuffer", "GPU/Lights", "Vulkan/LightBytes", "Vulkan/LightCapacityBytes", sizeof(GpuLight)},
        {"PrimitiveBuffer", "GPU/Primitives", "Vulkan/PrimitiveBytes", "Vulkan/PrimitiveCapacityBytes",
         sizeof(GpuPrimitive)},
        {"NodeBuffer", "GPU/Nodes", "Vulkan/NodeBytes", "Vulkan/NodeCapacityBytes", sizeof(GpuNode)},
    }};

    constexpr const char* kModelTableTracyName = "GPU/ModelTable";

    // GpuNode stores the affine part as rows; glm is column-major
    [[nodiscard]] std::array<glm::vec4, 3> affineRows(const glm::mat4& matrix)
    {
        std::array<glm::vec4, 3> rows{};
        for (int row = 0; row < 3; ++row) {
            rows[row] = {matrix[0][row], matrix[1][row], matrix[2][row], matrix[3][row]};
        }
        return rows;
    }

    [[nodiscard]] std::string formatBytes(vk::DeviceSize bytes)
    {
        constexpr vk::DeviceSize kKiB = 1024;
        constexpr vk::DeviceSize kMiB = kKiB * 1024;
        if (bytes < kKiB) {
            return std::format("{} B", bytes);
        }
        if (bytes < kMiB) {
            return std::format("{:.1f} KiB", static_cast<double>(bytes) / static_cast<double>(kKiB));
        }
        return std::format("{:.2f} MiB", static_cast<double>(bytes) / static_cast<double>(kMiB));
    }

    // per-upload staging offsets; keeps every memcpy destination and copy source 16 B aligned
    constexpr vk::DeviceSize kStagingAlignment = 16;

    [[nodiscard]] constexpr vk::DeviceSize alignUp(vk::DeviceSize size, vk::DeviceSize alignment)
    {
        return (size + alignment - 1) / alignment * alignment;
    }

    // M = T * R * S. v grows down: the rotation sign follows the Khronos sample renderer and the extension's example
    [[nodiscard]] GpuUvMatrix packUvMatrix(const MaterialUvTransform& uv)
    {
        const float c = std::cos(uv.rotation);
        const float s = std::sin(uv.rotation);
        return {.row0 = {c * uv.scale.x, s * uv.scale.y, uv.offset.x},
                .row1 = {-s * uv.scale.x, c * uv.scale.y, uv.offset.y}};
    }

    // the store keeps authored values for dedup and the UI; the GPU row is precomputed
    [[nodiscard]] GpuMaterial packMaterial(GpuMaterial material, const MaterialPbrExtension& ext)
    {
        material.emissiveFactor *= ext.emissiveStrength;
        const std::array<MaterialUvTransform, kMaterialUvSlotCount> transforms = {
            ext.baseColorUv, ext.metalRoughUv, ext.normalUv, ext.occlusionUv, ext.emissiveUv};
        for (uint32_t slot = 0; slot < kMaterialUvSlotCount; ++slot) {
            if (transforms[slot] != MaterialUvTransform{}) {
                material.uvMatrices[slot] = packUvMatrix(transforms[slot]);
                material.flags |= GpuMaterialFlag::UvTransformBaseColor << slot;
            }
        }
        return material;
    }

    [[nodiscard]] GpuUvTransform packUvTransform(const MaterialUvTransform& uv)
    {
        return {.offset = uv.offset, .scale = uv.scale, .rotation = uv.rotation};
    }

    [[nodiscard]] GpuTextureRef packTextureRef(const MaterialTextureRef& ref)
    {
        return {.tex = ref.tex,
                .samp = ref.samp,
                .uv = ref.uv,
                .normalScale = ref.normalScale,
                .uvXform = packUvTransform(ref.uvXform)};
    }

    [[nodiscard]] std::vector<GpuMaterialExtension> packMaterialExtensions(std::span<const MaterialPbrExtension> rows)
    {
        std::vector<GpuMaterialExtension> packed;
        packed.reserve(rows.size());
        for (const MaterialPbrExtension& ext : rows) {
            packed.push_back(GpuMaterialExtension{
                .flags = ext.flags,
                .baseColorUv = packUvTransform(ext.baseColorUv),
                .metalRoughUv = packUvTransform(ext.metalRoughUv),
                .normalUv = packUvTransform(ext.normalUv),
                .occlusionUv = packUvTransform(ext.occlusionUv),
                .emissiveUv = packUvTransform(ext.emissiveUv),
                .ior = ext.ior,
                .emissiveStrength = ext.emissiveStrength,
                .dispersion = ext.dispersion,
                .specularFactor = ext.specularFactor,
                .specularColorFactor = ext.specularColorFactor,
                .specular = packTextureRef(ext.specular),
                .specularColor = packTextureRef(ext.specularColor),
                .clearcoatFactor = ext.clearcoatFactor,
                .clearcoatRoughnessFactor = ext.clearcoatRoughnessFactor,
                .clearcoat = packTextureRef(ext.clearcoat),
                .clearcoatRoughness = packTextureRef(ext.clearcoatRoughness),
                .clearcoatNormal = packTextureRef(ext.clearcoatNormal),
                .sheenColorFactor = ext.sheenColorFactor,
                .sheenRoughnessFactor = ext.sheenRoughnessFactor,
                .sheenColor = packTextureRef(ext.sheenColor),
                .sheenRoughness = packTextureRef(ext.sheenRoughness),
                .transmissionFactor = ext.transmissionFactor,
                .transmission = packTextureRef(ext.transmission),
                .thicknessFactor = ext.thicknessFactor,
                .attenuationDistance = ext.attenuationDistance,
                .attenuationColor = ext.attenuationColor,
                .thickness = packTextureRef(ext.thickness),
                .iridescenceFactor = ext.iridescenceFactor,
                .iridescenceIor = ext.iridescenceIor,
                .iridescenceThicknessMin = ext.iridescenceThicknessMin,
                .iridescenceThicknessMax = ext.iridescenceThicknessMax,
                .iridescence = packTextureRef(ext.iridescence),
                .iridescenceThickness = packTextureRef(ext.iridescenceThickness),
                .anisotropyStrength = ext.anisotropyStrength,
                .anisotropyRotation = ext.anisotropyRotation,
                .anisotropy = packTextureRef(ext.anisotropy),
                .diffuseTransmissionFactor = ext.diffuseTransmissionFactor,
                .diffuseTransmissionColorFactor = ext.diffuseTransmissionColorFactor,
                .diffuseTransmission = packTextureRef(ext.diffuseTransmission),
                .diffuseTransmissionColor = packTextureRef(ext.diffuseTransmissionColor),
            });
        }
        return packed;
    }

    [[nodiscard]] vk::DeviceSize nextCapacity(vk::DeviceSize current, vk::DeviceSize needed)
    {
        if (needed == 0) {
            return current;
        }
        if (current == 0) {
            return needed;
        }
        vk::DeviceSize cap = current;
        while (cap < needed) {
            cap *= 2;
        }
        return cap;
    }
} // namespace

AssetBuffers::AssetBuffers(const Device& deviceWrapper, const VkAllocator& allocator, FrameSync& frameSync,
                           GeometryStore& geometryStore, MaterialStore& materialStore, LightStore& lightStore,
                           ObjectStorage& objectStorage)
    : allocator(allocator),
      device(deviceWrapper.vkdevice),
      queueFamilyIndices(deviceWrapper.queueFamilyIndices),
      frameSync(frameSync),
      objectStorage(objectStorage),
      geometryStore(geometryStore),
      materialStore(materialStore),
      lightStore(lightStore),
      megaGeometry(deviceWrapper, allocator, frameSync)
{
    logInfo("init() started", "AssetBuffers");
    // the table exists even when the startup load failed: the frame root always holds its address
    rebuildModelTable();
    flush();
}

AssetBuffers::~AssetBuffers()
{
    logInfo("Destructor called", "AssetBuffers");
    destroyHostBuffer(allocator.allocator, modelTable, kModelTableTracyName);
    for (size_t i = 0; i < kAssetBufferCount; ++i) {
        destroyAssetBuffer(static_cast<AssetBuffer>(i));
    }
}

void AssetBuffers::rebuildModelTable()
{
    ZoneScopedN("AssetBuffers::rebuildModelTable");
    const auto modelCount = static_cast<uint32_t>(modelAllocations.size());
    // callers hold the GPU idle, so rows are rewritten in place; the address changes only on growth
    if (modelCount > modelTableCapacity || modelTable.memory == nullptr) {
        destroyHostBuffer(allocator.allocator, modelTable, kModelTableTracyName);
        modelTableCapacity = std::max({modelCount, modelTableCapacity * 2, 1u});
        modelTable = createHostBuffer(sizeof(GpuModel) * static_cast<vk::DeviceSize>(modelTableCapacity),
                                      allocator.allocator, device, queueFamilyIndices, "ModelTable",
                                      kModelTableTracyName);
    }

    auto* rows = static_cast<GpuModel*>(modelTable.mapped);
    for (ModelId id = 0; id < modelCount; ++id) {
        // a stream the model does not have stays 0
        const auto slice = [&](AssetBuffer buffer) -> vk::DeviceAddress {
            return modelAllocations[id].gpu[std::to_underlying(buffer)].count > 0 ? modelAssetAddress(buffer, id) : 0;
        };
        GpuModel model{
            .vertices = slice(AssetBuffer::Vertices),
            .meshlets = slice(AssetBuffer::Meshlets),
            .meshletBounds = slice(AssetBuffer::MeshletBounds),
            .meshletGroups = slice(AssetBuffer::MeshletGroups),
            .meshletPrimitives = slice(AssetBuffer::MeshletPrimitives),
            .meshletVertices = slice(AssetBuffer::MeshletVertices),
            .meshletTriangles = slice(AssetBuffer::MeshletTriangles),
            .normals = slice(AssetBuffer::Normals),
            .tangents = slice(AssetBuffer::Tangents),
            .uv1 = slice(AssetBuffer::Uv1),
            .indices = slice(AssetBuffer::Indices),
            .nodes = slice(AssetBuffer::Nodes),
            .primitives = slice(AssetBuffer::Primitives),
            .nodeCount = modelAllocations[id].nodes.count,
        };
        const std::array<std::pair<vk::DeviceAddress, uint32_t>, 4> streams{{
            {model.normals, GpuModelAttribute::Normals},
            {model.tangents, GpuModelAttribute::Tangents},
            {model.uv1, GpuModelAttribute::Uv1},
            {model.indices, GpuModelAttribute::Indices},
        }};
        for (const auto& [address, bit] : streams) {
            if (address != 0) {
                model.attributeMask |= bit;
            }
        }
        rows[id] = model;
    }
    logInfo(std::format("model table: {} / {} rows bda=0x{:x}", modelCount, modelTableCapacity,
                         static_cast<uint64_t>(modelTable.address)),
            "AssetBuffers");
}

void AssetBuffers::destroyAssetBuffer(AssetBuffer id)
{
    DeviceBuffer& target = assetBuffers[std::to_underlying(id)];
    if (target.memory == nullptr) {
        return;
    }
    const AssetBufferInfo& info = kAssetBufferInfo[std::to_underlying(id)];
    logInfo(std::format("destroy {} (used {} / cap {} bda=0x{:x})", info.tracyName, formatBytes(target.usedBytes),
                         formatBytes(target.capacityBytes), static_cast<uint64_t>(target.address)),
             "AssetBuffers");
    destroyVmaBuffer(allocator.allocator, target.buffer, target.memory, info.tracyName);
    target.address = 0;
    target.usedBytes = 0;
    target.capacityBytes = 0;
}

uint32_t AssetBuffers::uploadedCount(AssetBuffer id) const noexcept
{
    return static_cast<uint32_t>(asset(id).usedBytes / kAssetBufferInfo[std::to_underlying(id)].stride);
}

vk::DeviceAddress AssetBuffers::modelAssetAddress(AssetBuffer id, ModelId modelId) const noexcept
{
    const vk::DeviceSize stride = kAssetBufferInfo[std::to_underlying(id)].stride;
    return asset(id).address + modelAllocations[modelId].gpu[std::to_underlying(id)].first * stride;
}

vk::DeviceSize AssetBuffers::modelGpuBytes(ModelId modelId) const noexcept
{
    vk::DeviceSize bytes = 0;
    for (size_t i = 0; i < kAssetBufferCount; ++i) {
        bytes += modelAllocations[modelId].gpu[i].count * kAssetBufferInfo[i].stride;
    }
    return bytes;
}

std::optional<ModelId> AssetBuffers::findModel(std::string_view key) const
{
    const auto it = std::ranges::find(modelAllocations, key, &ModelAllocation::key);
    if (it == modelAllocations.end()) {
        return std::nullopt;
    }
    return static_cast<ModelId>(it - modelAllocations.begin());
}

std::pair<vk::DeviceSize, vk::DeviceSize> AssetBuffers::assetTotals() const noexcept
{
    vk::DeviceSize usedBytes = 0;
    vk::DeviceSize capacityBytes = 0;
    for (const DeviceBuffer& buffer : assetBuffers) {
        usedBytes += buffer.usedBytes;
        capacityBytes += buffer.capacityBytes;
    }
    return {usedBytes, capacityBytes};
}

void AssetBuffers::createAssetBuffer(AssetBuffer id, vk::DeviceSize capacity, vk::raii::Buffer& buffer,
                                        VmaAllocation& memory) const
{
    createBuffer(capacity,
                 vk::BufferUsageFlagBits2::eTransferSrc | vk::BufferUsageFlagBits2::eTransferDst |
                     vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eDeviceLocal, buffer, memory, allocator.allocator, device,
                 queueFamilyIndices, kAssetBufferInfo[std::to_underlying(id)].debugName);
}

void AssetBuffers::installAssetBuffer(AssetBuffer id, vk::raii::Buffer&& buffer, VmaAllocation memory,
                                         vk::DeviceSize usedBytes, vk::DeviceSize capacityBytes)
{
    destroyAssetBuffer(id);

    DeviceBuffer& dst = assetBuffers[std::to_underlying(id)];
    const AssetBufferInfo& info = kAssetBufferInfo[std::to_underlying(id)];
    dst.buffer = std::move(buffer);
    dst.memory = memory;
    dst.usedBytes = usedBytes;
    dst.capacityBytes = capacityBytes;
    dst.address = device.getBufferAddress({.buffer = *dst.buffer});
    tracyResourceAlloc(static_cast<VkBuffer>(*dst.buffer), static_cast<size_t>(capacityBytes), info.tracyName);
}

void AssetBuffers::uploadAssetBuffers(std::span<const AssetUpload> uploads)
{
    ZoneScopedN("AssetBuffers::uploadAssetBuffers");
    std::vector<vk::DeviceSize> stagingOffsets(uploads.size());
    vk::DeviceSize stagingBytes = 0;
    for (size_t i = 0; i < uploads.size(); ++i) {
        stagingOffsets[i] = stagingBytes;
        stagingBytes = alignUp(stagingBytes + uploads[i].bytes.size_bytes(), kStagingAlignment);
    }
    if (stagingBytes == 0) {
        return;
    }
    ZoneValue(static_cast<uint64_t>(stagingBytes));

    struct Grown {
        AssetBuffer id;
        vk::raii::Buffer buffer;
        VmaAllocation memory; // null once installed
        vk::DeviceSize usedBytes;
        vk::DeviceSize capacityBytes;
    };
    std::vector<Grown> grown;
    vk::raii::Buffer staging({});
    VmaAllocation stagingMemory = nullptr;
    const FrameSync::OneTimeCommands oneTime = frameSync.oneTimeCommands();
    vk::raii::CommandBuffer& cmd = oneTime.cmd;
    bool isRecording = false;
    // success frees only the staging; a throw also frees the grown buffers and drops the recorded commands
    const ScopeExit cleanup([&] {
        if (isRecording) {
            try {
                cmd.reset();
            } catch (const vk::SystemError& error) {
                logError(std::format("upload command buffer reset failed: {}", error.what()), "AssetBuffers");
            }
        }
        for (Grown& next : grown) {
            destroyVmaBuffer(allocator.allocator, next.buffer, next.memory);
        }
        destroyVmaBuffer(allocator.allocator, staging, stagingMemory);
    });

    createBuffer(stagingBytes, vk::BufferUsageFlagBits2::eTransferSrc,
                 vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, staging,
                 stagingMemory, allocator.allocator, device, queueFamilyIndices, "AssetStaging");
    for (size_t i = 0; i < uploads.size(); ++i) {
        if (!uploads[i].bytes.empty()) {
            vmaCopyMemoryToAllocation(allocator.allocator, uploads[i].bytes.data(), stagingMemory, stagingOffsets[i],
                                      uploads[i].bytes.size_bytes());
        }
    }

    cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    isRecording = true;

    for (size_t i = 0; i < uploads.size(); ++i) {
        const AssetUpload& upload = uploads[i];
        DeviceBuffer& dst = assetBuffers[std::to_underlying(upload.id)];
        const std::string_view debugName = kAssetBufferInfo[std::to_underlying(upload.id)].debugName;
        const vk::DeviceSize srcBytes = upload.bytes.size_bytes();
        if (srcBytes == 0) {
            logInfo(std::format("skip {}: empty scratch (used {} / cap {})", debugName, formatBytes(dst.usedBytes),
                                 formatBytes(dst.capacityBytes)),
                     "AssetBuffers");
            continue;
        }
        const vk::DeviceSize writeEnd = upload.dstOffset + srcBytes;
        const vk::DeviceSize newUsed = std::max(dst.usedBytes, writeEnd);
        const vk::BufferCopy stagingCopy(stagingOffsets[i], upload.dstOffset, srcBytes);

        if (dst.memory != nullptr && writeEnd <= dst.capacityBytes) {
            logInfo(std::format("in place {}: {} at {} -> used {} (cap {})", debugName, formatBytes(srcBytes),
                                 formatBytes(upload.dstOffset), formatBytes(newUsed), formatBytes(dst.capacityBytes)),
                     "AssetBuffers");
            cmd.copyBuffer(staging, dst.buffer, stagingCopy);
            dst.usedBytes = newUsed;
            continue;
        }

        const vk::DeviceSize newCapacity = nextCapacity(dst.capacityBytes, newUsed);
        logInfo(std::format("{} {}: used {} -> {} (cap {} -> {})", dst.usedBytes == 0 ? "create" : "grow", debugName,
                             formatBytes(dst.usedBytes), formatBytes(newUsed), formatBytes(dst.capacityBytes),
                             formatBytes(newCapacity)),
                 "AssetBuffers");
        Grown& next = grown.emplace_back(Grown{.id = upload.id, .buffer = nullptr, .memory = nullptr,
                                               .usedBytes = newUsed, .capacityBytes = newCapacity});
        createAssetBuffer(upload.id, newCapacity, next.buffer, next.memory);
        // keep only the old bytes the upload does not overwrite: unordered copies must not overlap
        if (dst.memory != nullptr) {
            const vk::DeviceSize headBytes = std::min(dst.usedBytes, upload.dstOffset);
            if (headBytes > 0) {
                cmd.copyBuffer(dst.buffer, next.buffer, vk::BufferCopy(0, 0, headBytes));
            }
            if (writeEnd < dst.usedBytes) {
                cmd.copyBuffer(dst.buffer, next.buffer, vk::BufferCopy(writeEnd, writeEnd, dst.usedBytes - writeEnd));
            }
        }
        cmd.copyBuffer(staging, next.buffer, stagingCopy);
    }

    submitAndWait(cmd, oneTime.queue);
    isRecording = false;
    for (Grown& next : grown) {
        installAssetBuffer(next.id, std::move(next.buffer), std::exchange(next.memory, nullptr), next.usedBytes,
                           next.capacityBytes);
    }

    for (const AssetUpload& upload : uploads) {
        if (upload.bytes.empty()) {
            continue;
        }
        const DeviceBuffer& dst = asset(upload.id);
        const std::string message =
            std::format("{} ready used {} / cap {} bda=0x{:x}", kAssetBufferInfo[std::to_underlying(upload.id)].debugName,
                        formatBytes(dst.usedBytes), formatBytes(dst.capacityBytes), static_cast<uint64_t>(dst.address));
        logInfo(message, "AssetBuffers");
        TracyMessage(message.c_str(), message.size());
    }
}

std::span<const std::byte> AssetBuffers::scratchBytes(AssetBuffer id, const PackedScratch& packed) const
{
    // absent optional streams are zero-filled in scratch; skipping them leaves the slice empty and the bit clear
    const auto optionalStream = [this](const auto& column, uint32_t attribute) {
        return (geometryStore.presentStreams & attribute) != 0 ? std::as_bytes(std::span(column))
                                                               : std::span<const std::byte>{};
    };
    switch (id) {
    case AssetBuffer::Vertices:         return std::as_bytes(std::span(geometryStore.vertices));
    case AssetBuffer::Meshlets:         return std::as_bytes(std::span(geometryStore.meshlets));
    case AssetBuffer::MeshletBounds:    return std::as_bytes(std::span(geometryStore.meshletBounds));
    case AssetBuffer::MeshletPrimitives: return std::as_bytes(std::span(packed.meshletPrimitives));
    case AssetBuffer::MeshletGroups:
        return std::as_bytes(std::span(geometryStore.meshletGroups).subspan(geometryStore.flushedGroupCount));
    case AssetBuffer::MeshletVertices:  return std::as_bytes(std::span(geometryStore.meshletVertices));
    case AssetBuffer::MeshletTriangles: return std::as_bytes(std::span(geometryStore.meshletTriangles));
    case AssetBuffer::Normals:          return optionalStream(geometryStore.normals, GpuModelAttribute::Normals);
    case AssetBuffer::Tangents:         return optionalStream(geometryStore.tangents, GpuModelAttribute::Tangents);
    case AssetBuffer::Uv1:              return optionalStream(geometryStore.uv1, GpuModelAttribute::Uv1);
    case AssetBuffer::Indices:          return std::as_bytes(std::span(geometryStore.indices));
    // not model-sliced: appendMaterialUploads and uploadLights write them
    case AssetBuffer::Materials:
    case AssetBuffer::PbrExt:
    case AssetBuffer::Lights:           return {};
    case AssetBuffer::Primitives:       return std::as_bytes(std::span(packed.primitives));
    case AssetBuffer::Nodes:            return std::as_bytes(std::span(packed.nodes));
    case AssetBuffer::Count:            break;
    }
    return {};
}

void AssetBuffers::remapScratchOffsets()
{
    ZoneScopedN("AssetBuffers::remapScratchOffsets");
    // GPU data stays model-relative (see ModelAllocation); only the CPU catalog becomes absolute
    GeometryStore& geometry = geometryStore;
    const uint32_t vertexBase = uploadedCount(AssetBuffer::Vertices);
    const uint32_t meshletBase = uploadedCount(AssetBuffer::Meshlets);
    const uint32_t indexBase = uploadedCount(AssetBuffer::Indices);
    const uint32_t newPrimitives =
        static_cast<uint32_t>(geometry.primitiveDraws.size()) - geometry.flushedPrimitiveCount;
    logInfo(std::format("remap scratch: +{} prims onto gpu verts={} meshlets={} indices={}", newPrimitives,
                         vertexBase, meshletBase, indexBase),
             "AssetBuffers");
    for (uint32_t p = geometry.flushedPrimitiveCount; p < geometry.primitiveDraws.size(); ++p) {
        PrimitiveDraw& draw = geometry.primitiveDraws[p];
        draw.firstVertex += vertexBase;
        draw.firstIndex += indexBase;
        draw.meshlets.firstMeshlet += meshletBase;
    }
    for (EntityId id = 0; id < objectStorage.size(); ++id) {
        if (objectStorage.firstPrimitives[id] >= geometry.flushedPrimitiveCount) {
            objectStorage.meshletDraws[id].firstMeshlet += meshletBase;
        }
    }
}

std::vector<GpuPrimitive> AssetBuffers::packScratchPrimitives() const
{
    ZoneScopedN("AssetBuffers::packScratchPrimitives");
    std::vector<GpuPrimitive> packed;
    packed.reserve(geometryStore.primitiveDraws.size() - geometryStore.flushedPrimitiveCount);
    for (uint32_t p = geometryStore.flushedPrimitiveCount; p < geometryStore.primitiveDraws.size(); ++p) {
        const PrimitiveDraw& draw = geometryStore.primitiveDraws[p];
        const bool isDefaultMaterial =
            draw.materialId == kNoneIndex || draw.materialId == materialStore.defaultMaterialId();
        const uint32_t materialRow = isDefaultMaterial ? materialStore.defaultMaterialId() : draw.materialId;
        const bool isDoubleSided =
            (materialStore.infos[materialRow].material.flags & GpuMaterialFlag::DoubleSided) != 0;
        packed.push_back(GpuPrimitive{
            .firstMeshlet = draw.meshlets.firstMeshlet,
            .meshletCount = draw.meshlets.meshletCount,
            // material rows are shared, so the id is absolute
            .materialId = isDefaultMaterial ? kNoneIndex : draw.materialId,
            .firstVertex = draw.firstVertex,
            .vertexCount = draw.vertexCount,
            .firstIndex = draw.firstIndex,
            .indexCount = draw.indexCount,
            // catalog rows past the watermark are this model's, so subtracting it makes them model-relative
            .firstGroup = draw.firstGroup - geometryStore.flushedGroupCount,
            .groupCount = draw.groupCount,
            .flags = isDoubleSided ? GpuPrimitiveFlag::DoubleSided : 0u,
        });
    }
    return packed;
}

// before remapScratchOffsets: meshlet ranges are still scratch-relative, which is the model's slice
std::vector<uint32_t> AssetBuffers::packScratchMeshletPrimitives() const
{
    std::vector<uint32_t> packed(geometryStore.meshlets.size(), 0);
    for (uint32_t p = geometryStore.flushedPrimitiveCount; p < geometryStore.primitiveDraws.size(); ++p) {
        const MeshletDraw& range = geometryStore.primitiveDraws[p].meshlets;
        std::fill_n(packed.begin() + range.firstMeshlet, range.meshletCount, p - geometryStore.flushedPrimitiveCount);
    }
    return packed;
}

std::vector<GpuNode> AssetBuffers::packScratchNodes() const
{
    std::vector<GpuNode> packed;
    packed.reserve(geometryStore.nodeDraws.size() - geometryStore.flushedNodeCount);
    for (uint32_t n = geometryStore.flushedNodeCount; n < geometryStore.nodeDraws.size(); ++n) {
        const NodeDraw& node = geometryStore.nodeDraws[n];
        packed.push_back(GpuNode{
            .localToModel = affineRows(node.localToModel),
            .boundingSphere = node.boundingSphere,
            // catalog rows past the watermark are this model's
            .firstPrimitive = node.firstPrimitive - geometryStore.flushedPrimitiveCount,
            .primitiveCount = node.primitiveCount,
            .flags = node.flags,
            .pad0 = 0,
            .bboxMin = node.meshBounds.bboxMin,
            .pad1 = 0,
            .bboxMax = node.meshBounds.bboxMax,
            .pad2 = 0,
        });
    }
    return packed;
}

std::vector<GpuLight> AssetBuffers::packScratchLights() const
{
    ZoneScopedN("AssetBuffers::packScratchLights");
    std::vector<GpuLight> packed;
    packed.reserve(lightStore.instances.size());
    logInfo(std::format("pack lights: {} defs {} instances (gpu already {})", lightStore.defs.size(),
                         lightStore.instances.size(), lightStore.uploadedCount()),
             "AssetBuffers");
    for (const LightInstance& instance : lightStore.instances) {
        GpuLight light{};
        light.position = instance.position;
        light.direction = instance.direction;
        if (instance.defIndex < lightStore.defs.size()) {
            const LightDef& def = lightStore.defs[instance.defIndex];
            light.range = def.range;
            light.intensity = def.intensity;
            light.color = def.color;
            light.type = def.type;
            light.innerCone = def.innerCone;
            light.outerCone = def.outerCone;
        }
        packed.push_back(light);
    }
    return packed;
}

void AssetBuffers::flush()
{
    ZoneScopedN("AssetBuffers::flush");
    logInfo("flush() started", "AssetBuffers");
    logInfo(std::format("cpu scratch: verts={} meshlets={} meshletVerts={} triCorners={} indices={} "
                         "normals={} tangents={} uv1={} new materials={} lights={}/{} prims={} (flushed {})",
                         geometryStore.vertices.size(), geometryStore.meshlets.size(),
                         geometryStore.meshletVertices.size(), geometryStore.meshletTriangles.size(),
                         geometryStore.indices.size(), geometryStore.normals.size(), geometryStore.tangents.size(),
                         geometryStore.uv1.size(), materialStore.dirtyRows.size(), lightStore.defs.size(), lightStore.instances.size(), geometryStore.primitiveDraws.size(),
                         geometryStore.flushedPrimitiveCount),
             "AssetBuffers");

    // one model per flush keeps every slice contiguous and its GPU data model-relative
    const auto pendingEntities = std::ranges::count(objectStorage.modelIds, kInvalidModelId);
    if (pendingEntities > 1) {
        throw std::logic_error(std::format("flushGpuAssets: {} models pending, expected at most 1", pendingEntities));
    }
    if (pendingEntities == 0) {
        discardScratch();
        return;
    }
    const EntityId entity = objectStorage.size() - 1;
    if (objectStorage.modelIds[entity] != kInvalidModelId) {
        throw std::logic_error("flushGpuAssets: the pending entity must be the newest");
    }

    const auto modelId = static_cast<ModelId>(modelAllocations.size());
    ModelAllocation allocation{
        .gpu = {},
        .primitives = {geometryStore.flushedPrimitiveCount,
                       static_cast<uint32_t>(geometryStore.primitiveDraws.size()) - geometryStore.flushedPrimitiveCount},
        .auxBlobs = {geometryStore.flushedAuxBlobCount,
                     static_cast<uint32_t>(geometryStore.auxBlobs.size()) - geometryStore.flushedAuxBlobCount},
        .nodes = {geometryStore.flushedNodeCount,
                  static_cast<uint32_t>(geometryStore.nodeDraws.size()) - geometryStore.flushedNodeCount},
        .meshletGroups = {geometryStore.flushedGroupCount,
                          static_cast<uint32_t>(geometryStore.meshletGroups.size()) - geometryStore.flushedGroupCount},
        .key = objectStorage.names[entity],
        .meshletDraw = {},
        .localBounds = glm::vec4{0.0f},
        .lights = packScratchLights(),
        .materialRefs = {},
        .textureRefs = std::move(geometryStore.textureRefs),
        .refCount = 0,
    };

    allocation.localBounds = geometryStore.nodeBoundingSphere(allocation.nodes.first, allocation.nodes.count);
    PackedScratch packed{
        .primitives = packScratchPrimitives(),
        .nodes = packScratchNodes(),
        .meshletPrimitives = packScratchMeshletPrimitives(),
        .materialRows = {},
        .materials = {},
        .extensions = {},
    };
    remapScratchOffsets();

    logInfo(std::format("material cache: rows={} live={} new={} hits={} misses={}", materialStore.size(),
                         materialStore.liveCount(), materialStore.dirtyRows.size(), materialStore.cacheHits,
                         materialStore.cacheMisses),
             "AssetBuffers");

    std::array<uint32_t, kAssetBufferCount> firsts{};
    std::vector<AssetUpload> uploads;
    uploads.reserve(kAssetBufferCount);
    for (size_t i = 0; i < kAssetBufferCount; ++i) {
        const auto id = static_cast<AssetBuffer>(i);
        if (!isModelSliced(id)) {
            continue;
        }
        firsts[i] = uploadedCount(id);
        uploads.push_back({.id = id, .bytes = scratchBytes(id, packed), .dstOffset = asset(id).usedBytes});
    }
    appendMaterialUploads(packed, uploads);
    uploadAssetBuffers(uploads);
    materialStore.dirtyRows.clear();
    for (size_t i = 0; i < kAssetBufferCount; ++i) {
        if (isModelSliced(static_cast<AssetBuffer>(i))) {
            allocation.gpu[i] = {firsts[i], uploadedCount(static_cast<AssetBuffer>(i)) - firsts[i]};
        }
    }
    // from the load scratch, which is the model's slice; a cache hit (instantiateModel) builds nothing
    megaGeometry.buildModel({.vertices = geometryStore.vertices,
                             .meshlets = geometryStore.meshlets,
                             .meshletVertices = geometryStore.meshletVertices,
                             .meshletTriangles = geometryStore.meshletTriangles,
                             .primitives = packed.primitives});
    // remapped to GPU-absolute above
    allocation.meshletDraw = objectStorage.meshletDraws[entity];
    allocation.materialRefs = materialStore.commitModel();
    modelAllocations.push_back(std::move(allocation));
    attachEntity(entity, modelId);
    uploadLights();
    ++generation;

    geometryStore.clearScratch();
    lightStore.clearScratch();
    rebuildModelTable();
#if ENGINE_USE_MIMALLOC
    // load scratch (decoded images, geometry, file buffers) is freed by now; return those pages to the OS
    mi_collect(true);
#endif

    logInfo(std::format("gpu catalog: verts={} meshlets={} meshletVerts={} triCorners={} indices={} "
                         "materials={} lights={} prims={}",
                         uploadedCount(AssetBuffer::Vertices), uploadedCount(AssetBuffer::Meshlets),
                         uploadedCount(AssetBuffer::MeshletVertices), uploadedCount(AssetBuffer::MeshletTriangles),
                         uploadedCount(AssetBuffer::Indices), materialStore.liveCount(), lightStore.uploadedCount(),
                         geometryStore.primitiveDraws.size()),
             "AssetBuffers");
    const auto [gpuAssetBytes, gpuAssetCapacity] = assetTotals();
    logInfo(std::format("gpu ssbo total: used {} / cap {}", formatBytes(gpuAssetBytes),
                         formatBytes(gpuAssetCapacity)),
             "AssetBuffers");
    logInfo(std::format("cpu after flush: scratch verts={} meshlets={} | material cache={} lights scratch={}",
                         geometryStore.vertices.size(), geometryStore.meshlets.size(), materialStore.size(),
                         lightStore.instances.size()),
             "AssetBuffers");
#ifdef TRACY_ENABLE
    const std::string tracyMsg = std::format(
        "flush gpu assets total={} verts={} meshlets={} materials={} lights={}", formatBytes(gpuAssetBytes),
        uploadedCount(AssetBuffer::Vertices), uploadedCount(AssetBuffer::Meshlets), materialStore.liveCount(),
        lightStore.uploadedCount());
    TracyMessage(tracyMsg.c_str(), tracyMsg.size());
#endif
    tracyPlotResources();
}

void AssetBuffers::discardScratch()
{
    logInfo("flushGpuAssets: no new model, discarding load scratch", "AssetBuffers");
    geometryStore.discardScratch();
    lightStore.clearScratch();
    materialStore.discardModel();
#if ENGINE_USE_MIMALLOC
    mi_collect(true);
#endif
}

void AssetBuffers::appendMaterialUploads(PackedScratch& packed, std::vector<AssetUpload>& uploads) const
{
    packed.materialRows = materialStore.dirtyRows;
    std::ranges::sort(packed.materialRows);
    std::vector<MaterialPbrExtension> extensions;
    extensions.reserve(packed.materialRows.size());
    for (const uint32_t id : packed.materialRows) {
        packed.materials.push_back(packMaterial(materialStore.infos[id].material, materialStore.infos[id].extension));
        extensions.push_back(materialStore.infos[id].extension);
    }
    packed.extensions = packMaterialExtensions(extensions);

    const std::span<const GpuMaterial> materials(packed.materials);
    const std::span<const GpuMaterialExtension> packedExtensions(packed.extensions);
    for (size_t first = 0; first < packed.materialRows.size();) {
        size_t last = first + 1;
        while (last < packed.materialRows.size() && packed.materialRows[last] == packed.materialRows[last - 1] + 1) {
            ++last;
        }
        const vk::DeviceSize row = packed.materialRows[first];
        uploads.push_back({.id = AssetBuffer::Materials,
                           .bytes = std::as_bytes(materials.subspan(first, last - first)),
                           .dstOffset = row * sizeof(GpuMaterial)});
        uploads.push_back({.id = AssetBuffer::PbrExt,
                           .bytes = std::as_bytes(packedExtensions.subspan(first, last - first)),
                           .dstOffset = row * sizeof(GpuMaterialExtension)});
        first = last;
    }
}

void AssetBuffers::attachEntity(EntityId entity, ModelId modelId)
{
    ModelAllocation& model = modelAllocations[modelId];
    objectStorage.modelIds[entity] = modelId;
    objectStorage.localBounds[entity] = model.localBounds;
    ++model.refCount;
    for (GpuLight light : model.lights) {
        light.instanceIndex = entity;
        lightStore.uploaded.push_back(light);
    }
}

void AssetBuffers::uploadLights()
{
    const std::span<const std::byte> bytes = std::as_bytes(std::span(lightStore.uploaded));
    if (!bytes.empty()) {
        const std::array uploads{AssetUpload{.id = AssetBuffer::Lights, .bytes = bytes, .dstOffset = 0}};
        uploadAssetBuffers(uploads);
    }
    // the mirror may have shrunk; rows past it are stale and GpuFrameRoot::lightCount stops before them
    assetBuffers[std::to_underlying(AssetBuffer::Lights)].usedBytes = bytes.size_bytes();
}

EntityId AssetBuffers::instantiateModel(ModelId modelId, std::span<const Transform> transforms)
{
    ZoneScopedN("AssetBuffers::instantiateModel");
    const ModelAllocation& model = modelAllocations.at(modelId);
    const auto first = static_cast<EntityId>(objectStorage.size());
    for (const Transform& transform : transforms) {
        const EntityId entity = objectStorage.create(transform, model.meshletDraw, model.primitives.first,
                                                     model.primitives.count, model.key);
        attachEntity(entity, modelId);
        materialStore.noteReuse(model.materialRefs);
    }
    uploadLights();
    ++generation;
    logInfo(std::format("instance entities [{}, {}) of model {} '{}': {} entities share {} on GPU", first,
                         first + transforms.size(), modelId, model.key, model.refCount,
                         formatBytes(modelGpuBytes(modelId))),
             "AssetBuffers");
    tracyPlotResources();
    return first;
}

std::vector<uint32_t> AssetBuffers::eraseEntity(EntityId id)
{
    ZoneScopedN("AssetBuffers::eraseEntity");
    if (id >= objectStorage.size()) {
        throw std::out_of_range(std::format("eraseEntity: no entity {}", id));
    }
    const ModelId modelId = objectStorage.modelIds[id];
    logInfo(std::format("erase entity {} '{}' (model {})", id, objectStorage.names[id], modelId), "AssetBuffers");

    // owners are entity ids, which shift down past the erased one
    std::vector<GpuLight>& lightRows = lightStore.uploaded;
    std::erase_if(lightRows, [id](const GpuLight& light) { return light.instanceIndex == id; });
    for (GpuLight& light : lightRows) {
        if (light.instanceIndex > id) {
            --light.instanceIndex;
        }
    }
    objectStorage.erase(id);

    std::vector<uint32_t> releasedTextures;
    if (modelId != kInvalidModelId && --modelAllocations[modelId].refCount == 0) {
        releasedTextures = eraseModel(modelId);
    }
    uploadLights();
    ++generation;
    if (objectStorage.empty()) {
        geometryStore.extensionsUsed.clear();
        geometryStore.extensionsRequired.clear();
    }

    const auto [gpuAssetBytes, gpuAssetCapacity] = assetTotals();
    logInfo(std::format("gpu ssbo total after erase: used {} / cap {}", formatBytes(gpuAssetBytes),
                         formatBytes(gpuAssetCapacity)),
             "AssetBuffers");
    tracyPlotResources();
    return releasedTextures;
}

std::vector<uint32_t> AssetBuffers::eraseModel(ModelId modelId)
{
    ZoneScopedN("AssetBuffers::eraseModel");
    logInfo(std::format("erase model {} '{}': {} on GPU, {} primitives, {} materials, {} textures", modelId,
                         modelAllocations[modelId].key, formatBytes(modelGpuBytes(modelId)),
                         modelAllocations[modelId].primitives.count, modelAllocations[modelId].materialRefs.size(),
                         modelAllocations[modelId].textureRefs.size()),
             "AssetBuffers");
    ModelAllocation removed = std::move(modelAllocations[modelId]);
    compactAssetBuffers(removed);
    rebaseCatalog(modelId, removed);
    // the other models' CLAS and BLAS stay where they are
    megaGeometry.eraseModel(modelId);
    materialStore.release(removed.materialRefs);
    rebuildModelTable();
    return std::move(removed.textureRefs);
}

void AssetBuffers::compactAssetBuffers(const ModelAllocation& removed)
{
    ZoneScopedN("AssetBuffers::compactAssetBuffers");
    struct Compacted {
        AssetBuffer id;
        vk::raii::Buffer buffer;
        VmaAllocation memory;
        vk::DeviceSize usedBytes;
    };
    std::vector<Compacted> compacted;
    const FrameSync::OneTimeCommands oneTime = frameSync.oneTimeCommands();
    vk::raii::CommandBuffer& cmd = oneTime.cmd;
    bool isRecording = false;

    for (size_t i = 0; i < kAssetBufferCount; ++i) {
        const auto id = static_cast<AssetBuffer>(i);
        const ElementRange range = removed.gpu[i];
        if (range.count == 0) {
            continue;
        }
        const DeviceBuffer& src = assetBuffers[i];
        const vk::DeviceSize stride = kAssetBufferInfo[i].stride;
        const vk::DeviceSize cutFirst = range.first * stride;
        const vk::DeviceSize cutEnd = range.end() * stride;
        const vk::DeviceSize remaining = src.usedBytes - (cutEnd - cutFirst);
        if (remaining == 0) {
            destroyAssetBuffer(id);
            continue;
        }

        // exact fit: the point of unloading is returning the memory
        Compacted& next = compacted.emplace_back(Compacted{.id = id, .buffer = nullptr, .memory = nullptr,
                                                           .usedBytes = remaining});
        createAssetBuffer(id, remaining, next.buffer, next.memory);
        if (!isRecording) {
            cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
            isRecording = true;
        }
        if (cutFirst > 0) {
            cmd.copyBuffer(src.buffer, next.buffer, vk::BufferCopy(0, 0, cutFirst));
        }
        if (cutEnd < src.usedBytes) {
            cmd.copyBuffer(src.buffer, next.buffer, vk::BufferCopy(cutEnd, cutFirst, src.usedBytes - cutEnd));
        }
    }

    if (isRecording) {
        submitAndWait(cmd, oneTime.queue);
    }
    for (Compacted& next : compacted) {
        installAssetBuffer(next.id, std::move(next.buffer), next.memory, next.usedBytes, next.usedBytes);
    }
}

void AssetBuffers::rebaseCatalog(ModelId modelId, const ModelAllocation& removed)
{
    ZoneScopedN("AssetBuffers::rebaseCatalog");
    const auto gpuRange = [&removed](AssetBuffer buffer) { return removed.gpu[std::to_underlying(buffer)]; };
    // references past a removed slice move down by its size; kNoneIndex stays
    const auto shift = [](uint32_t& value, ElementRange cut) {
        if (value != kNoneIndex && value >= cut.end()) {
            value -= cut.count;
        }
    };
    const auto eraseSlice = [](auto& column, ElementRange cut) {
        column.erase(column.begin() + cut.first, column.begin() + cut.end());
    };

    GeometryStore& geometry = geometryStore;
    eraseSlice(geometry.primitiveDraws, removed.primitives);
    eraseSlice(geometry.auxBlobs, removed.auxBlobs);
    eraseSlice(geometry.nodeDraws, removed.nodes);
    eraseSlice(geometry.meshletGroups, removed.meshletGroups);
    geometry.flushedNodeCount -= removed.nodes.count;
    geometry.flushedGroupCount -= removed.meshletGroups.count;
    geometry.flushedPrimitiveCount -= removed.primitives.count;
    geometry.flushedAuxBlobCount -= removed.auxBlobs.count;

    // material ids are shared rows that never move
    for (PrimitiveDraw& draw : geometry.primitiveDraws) {
        shift(draw.meshlets.firstMeshlet, gpuRange(AssetBuffer::Meshlets));
        shift(draw.firstVertex, gpuRange(AssetBuffer::Vertices));
        shift(draw.firstIndex, gpuRange(AssetBuffer::Indices));
        shift(draw.firstGroup, removed.meshletGroups);
    }
    for (NodeDraw& node : geometry.nodeDraws) {
        shift(node.firstPrimitive, removed.primitives);
    }

    modelAllocations.erase(modelAllocations.begin() + modelId);
    for (EntityId other = 0; other < objectStorage.size(); ++other) {
        shift(objectStorage.firstPrimitives[other], removed.primitives);
        shift(objectStorage.meshletDraws[other].firstMeshlet, gpuRange(AssetBuffer::Meshlets));
        ModelId& otherModel = objectStorage.modelIds[other];
        if (otherModel != kInvalidModelId && otherModel > modelId) {
            --otherModel;
        }
    }
    for (ModelAllocation& allocation : modelAllocations) {
        for (size_t i = 0; i < kAssetBufferCount; ++i) {
            shift(allocation.gpu[i].first, removed.gpu[i]);
        }
        shift(allocation.primitives.first, removed.primitives);
        shift(allocation.auxBlobs.first, removed.auxBlobs);
        shift(allocation.nodes.first, removed.nodes);
        shift(allocation.meshletGroups.first, removed.meshletGroups);
        shift(allocation.meshletDraw.firstMeshlet, gpuRange(AssetBuffer::Meshlets));
    }
}

void AssetBuffers::tracyPlotResources() const
{
#ifdef TRACY_ENABLE
    uint32_t activeEntities = 0;
    uint32_t activeMeshlets = 0;
    for (EntityId id = 0; id < objectStorage.size(); ++id) {
        if ((objectStorage.flags[id] & EntityFlag::Active) == 0) {
            continue;
        }
        ++activeEntities;
        activeMeshlets += objectStorage.meshletDraws[id].meshletCount;
    }

    TracyPlot("Vulkan/EntityCount", static_cast<double>(objectStorage.size()));
    TracyPlot("Vulkan/ActiveEntities", static_cast<double>(activeEntities));
    TracyPlot("Vulkan/ActiveMeshlets", static_cast<double>(activeMeshlets));
    TracyPlot("Vulkan/PrimitiveCount", static_cast<double>(geometryStore.primitiveDraws.size()));
    TracyPlot("Vulkan/MeshletCount", static_cast<double>(uploadedCount(AssetBuffer::Meshlets)));
    TracyPlot("Vulkan/MeshletVertexCount", static_cast<double>(uploadedCount(AssetBuffer::MeshletVertices)));
    TracyPlot("Vulkan/MeshletTriangleCorners", static_cast<double>(uploadedCount(AssetBuffer::MeshletTriangles)));
    TracyPlot("Vulkan/VerticesInUse", static_cast<double>(uploadedCount(AssetBuffer::Vertices)));
    TracyPlot("Vulkan/IndexCount", static_cast<double>(uploadedCount(AssetBuffer::Indices)));
    TracyPlot("Vulkan/MaterialCount", static_cast<double>(materialStore.liveCount()));
    TracyPlot("Vulkan/MaterialCacheSize", static_cast<double>(materialStore.size()));
    TracyPlot("Vulkan/MaterialCacheHits", static_cast<double>(materialStore.cacheHits));
    TracyPlot("Vulkan/MaterialCacheMisses", static_cast<double>(materialStore.cacheMisses));
    TracyPlot("Vulkan/LightCount", static_cast<double>(lightStore.uploadedCount()));
    for (size_t i = 0; i < kAssetBufferCount; ++i) {
        TracyPlot(kAssetBufferInfo[i].plotUsed, static_cast<double>(assetBuffers[i].usedBytes));
        TracyPlot(kAssetBufferInfo[i].plotCapacity, static_cast<double>(assetBuffers[i].capacityBytes));
    }
    const auto [gpuAssetBytes, gpuAssetCapacity] = assetTotals();
    TracyPlot("Vulkan/GpuAssetBytes", static_cast<double>(gpuAssetBytes));
    TracyPlot("Vulkan/GpuAssetCapacityBytes", static_cast<double>(gpuAssetCapacity));
    TracyPlot("Vulkan/CpuScratchVertices", static_cast<double>(geometryStore.vertices.size()));
    TracyPlot("Vulkan/CpuScratchMeshlets", static_cast<double>(geometryStore.meshlets.size()));
    TracyPlot("Vulkan/NodeCount", static_cast<double>(geometryStore.nodeDraws.size()));
    TracyPlot("Vulkan/MeshBdaReady",
              static_cast<double>(asset(AssetBuffer::Vertices).address != 0 &&
                                  asset(AssetBuffer::Meshlets).address != 0 &&
                                  asset(AssetBuffer::MeshletVertices).address != 0 &&
                                  asset(AssetBuffer::MeshletTriangles).address != 0));
#endif
}
