#include "vk_resource_manager.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <span>
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>
#include "Constants.h"
#include "static_headers/logger.hpp"
#include "util/debug.hpp"
#include "util/vk_tracy.hpp"
#include "util/vk_utils.hpp"

namespace
{
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
        {"MeshletVertexBuffer", "GPU/MeshletVertices", "Vulkan/MeshletVertexBytes",
         "Vulkan/MeshletVertexCapacityBytes", sizeof(uint32_t)},
        {"MeshletTriangleBuffer", "GPU/MeshletTriangles", "Vulkan/MeshletTriangleBytes",
         "Vulkan/MeshletTriangleCapacityBytes", sizeof(uint8_t)},
        {"NormalBuffer", "GPU/Normals", "Vulkan/NormalBytes", "Vulkan/NormalCapacityBytes", sizeof(glm::vec3)},
        {"TangentBuffer", "GPU/Tangents", "Vulkan/TangentBytes", "Vulkan/TangentCapacityBytes", sizeof(glm::vec4)},
        {"Uv1Buffer", "GPU/Uv1", "Vulkan/Uv1Bytes", "Vulkan/Uv1CapacityBytes", sizeof(glm::vec2)},
        {"JointBuffer", "GPU/Joints", "Vulkan/JointBytes", "Vulkan/JointCapacityBytes",
         sizeof(std::array<uint16_t, 4>)},
        {"WeightBuffer", "GPU/Weights", "Vulkan/WeightBytes", "Vulkan/WeightCapacityBytes", sizeof(glm::vec4)},
        {"IndexBuffer", "GPU/Indices", "Vulkan/IndexBytes", "Vulkan/IndexCapacityBytes", sizeof(uint32_t)},
        {"MorphPosBuffer", "GPU/MorphPos", "Vulkan/MorphPosBytes", "Vulkan/MorphPosCapacityBytes", sizeof(glm::vec3)},
        {"MorphNrmBuffer", "GPU/MorphNrm", "Vulkan/MorphNrmBytes", "Vulkan/MorphNrmCapacityBytes", sizeof(glm::vec3)},
        {"MorphTanBuffer", "GPU/MorphTan", "Vulkan/MorphTanBytes", "Vulkan/MorphTanCapacityBytes", sizeof(glm::vec4)},
        {"MorphTargetBuffer", "GPU/MorphTargets", "Vulkan/MorphTargetBytes", "Vulkan/MorphTargetCapacityBytes",
         sizeof(GpuMorphTarget)},
        {"MorphWeightBuffer", "GPU/MorphWeights", "Vulkan/MorphWeightBytes", "Vulkan/MorphWeightCapacityBytes",
         sizeof(float)},
        {"MaterialBuffer", "GPU/Materials", "Vulkan/MaterialBytes", "Vulkan/MaterialCapacityBytes",
         sizeof(GpuMaterial)},
        {"PbrExtBuffer", "GPU/PbrExtensions", "Vulkan/PbrExtBytes", "Vulkan/PbrExtCapacityBytes",
         sizeof(GpuMaterialExtension)},
        {"LightBuffer", "GPU/Lights", "Vulkan/LightBytes", "Vulkan/LightCapacityBytes", sizeof(GpuLight)},
        {"PrimitiveBuffer", "GPU/Primitives", "Vulkan/PrimitiveBytes", "Vulkan/PrimitiveCapacityBytes",
         sizeof(GpuPrimitive)},
        {"NodeBuffer", "GPU/Nodes", "Vulkan/NodeBytes", "Vulkan/NodeCapacityBytes", sizeof(GpuNode)},
    }};

    constexpr const char* kColorTracyName = "GPU/ColorMSAA";
    constexpr const char* kDepthTracyName = "GPU/Depth";
    constexpr const char* kInstanceUboTracyName = "GPU/InstanceUBO";
    constexpr const char* kFrameRootTracyName = "GPU/FrameRoot";
    constexpr const char* kModelTableTracyName = "GPU/ModelTable";
    constexpr const char* kNodeInstanceTracyName = "GPU/NodeInstances";
    constexpr const char* kDrawRecordTracyName = "GPU/DrawRecords";

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

    // scratch offsets are still model-relative: call before remapScratchOffsets
    [[nodiscard]] std::vector<GpuMorphTarget> packMorphTargets(std::span<const MorphTarget> targets)
    {
        std::vector<GpuMorphTarget> packed;
        packed.reserve(targets.size());
        for (const MorphTarget& target : targets) {
            packed.push_back({.posOffset = target.posOffset, .nrmOffset = target.nrmOffset, .tanOffset = target.tanOffset});
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

ResourceManager::ResourceManager(const Device &deviceWrapper,
               const VkAllocator &allocator,
               GeometryStore &geometryStoreIn,
               MaterialStore &materialStoreIn,
               LightStore &lightStoreIn,
               ObjectStorage &objectStorageIn)
    : deviceWrapper(deviceWrapper),
      allocator(allocator),
      physicalDevice(deviceWrapper.physicalDevice),
      device(deviceWrapper.vkdevice),
      queueFamilyIndices(deviceWrapper.queueFamilyIndices),
      graphicsQueue(deviceWrapper.graphicsQueue),
      transferQueue(deviceWrapper.transferQueue),
      objectStorage(objectStorageIn),
      geometryStore(geometryStoreIn),
      materialStore(materialStoreIn),
      lightStore(lightStoreIn),
      graphicsIndex(deviceWrapper.graphicsIndex),
      transferIndex(deviceWrapper.transferIndex),
      msaaSamples(deviceWrapper.msaaSamples)
{
    logInfo("Initialized", "ResourceManager");
}

void ResourceManager::destroyInstanceUboBuffers()
{
    ZoneScopedN("ResourceManager::destroyInstanceUboBuffers");
    for (HostBuffer& ubo : instanceUbos)
    {
        destroyHostBuffer(ubo, kInstanceUboTracyName);
    }
    instanceCapacity = 0;
}

void ResourceManager::createHostBuffer(HostBuffer& target, vk::DeviceSize size, const std::string& debugName,
                                       const char* tracyName) const
{
    createBuffer(size,
                 vk::BufferUsageFlagBits2::eUniformBuffer | vk::BufferUsageFlagBits2::eStorageBuffer |
                     vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, target.buffer,
                 target.memory, allocator.allocator, device, queueFamilyIndices, debugName,
                 VMA_ALLOCATION_CREATE_MAPPED_BIT);
    VmaAllocationInfo allocationInfo{};
    vmaGetAllocationInfo(allocator.allocator, target.memory, &allocationInfo);
    target.mapped = allocationInfo.pMappedData;
    target.address = device.getBufferAddress({.buffer = *target.buffer});
    tracyResourceAlloc(static_cast<VkBuffer>(*target.buffer), static_cast<size_t>(size), tracyName);
}

void ResourceManager::destroyHostBuffer(HostBuffer& target, const char* tracyName) const
{
    // persistently mapped (MAPPED_BIT): VMA unmaps on destroy
    destroyVmaBuffer(allocator.allocator, target.buffer, target.memory, tracyName);
    target.mapped = nullptr;
    target.address = 0;
}

void ResourceManager::rebuildModelTable()
{
    ZoneScopedN("ResourceManager::rebuildModelTable");
    const auto modelCount = static_cast<uint32_t>(modelAllocations.size());
    // callers hold the GPU idle, so rows are rewritten in place; the address changes only on growth
    if (modelCount > modelTableCapacity || modelTable.memory == nullptr) {
        destroyHostBuffer(modelTable, kModelTableTracyName);
        modelTableCapacity = std::max({modelCount, modelTableCapacity * 2, 1u});
        createHostBuffer(modelTable, sizeof(GpuModel) * static_cast<vk::DeviceSize>(modelTableCapacity),
                         "ModelTable", kModelTableTracyName);
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
            .meshletVertices = slice(AssetBuffer::MeshletVertices),
            .meshletTriangles = slice(AssetBuffer::MeshletTriangles),
            .normals = slice(AssetBuffer::Normals),
            .tangents = slice(AssetBuffer::Tangents),
            .uv1 = slice(AssetBuffer::Uv1),
            .joints = slice(AssetBuffer::Joints),
            .weights = slice(AssetBuffer::Weights),
            .indices = slice(AssetBuffer::Indices),
            .morphPos = slice(AssetBuffer::MorphPos),
            .morphNrm = slice(AssetBuffer::MorphNrm),
            .morphTan = slice(AssetBuffer::MorphTan),
            .morphTargets = slice(AssetBuffer::MorphTargets),
            .morphWeights = slice(AssetBuffer::MorphWeights),
            .nodes = slice(AssetBuffer::Nodes),
            .nodeCount = modelAllocations[id].nodes.count,
        };
        const std::array<std::pair<vk::DeviceAddress, uint32_t>, 7> streams{{
            {model.normals, GpuModelAttribute::Normals},
            {model.tangents, GpuModelAttribute::Tangents},
            {model.uv1, GpuModelAttribute::Uv1},
            {model.joints, GpuModelAttribute::Joints},
            {model.weights, GpuModelAttribute::Weights},
            {model.indices, GpuModelAttribute::Indices},
            {model.morphTargets, GpuModelAttribute::Morphs},
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
            "ResourceManager");
}

void ResourceManager::writeFrameRoot(uint32_t frameSlot)
{
    *static_cast<GpuFrameRoot*>(frameRoots[frameSlot].mapped) = GpuFrameRoot{
        .camera = cameraAddresses[frameSlot],
        .instances = instanceUbos[frameSlot].address,
        .models = modelTable.address,
        .primitives = asset(AssetBuffer::Primitives).address,
        .materials = asset(AssetBuffer::Materials).address,
        .pbrExtensions = asset(AssetBuffer::PbrExt).address,
        .lights = asset(AssetBuffer::Lights).address,
        .tlas = 0,
        .nodeInstances = nodeInstanceBuffers[frameSlot].address,
        .drawRecords = drawRecordBuffers[frameSlot].address,
        .lightCount = lightStore.uploadedCount(),
        .instanceCount = objectStorage.size(),
        .primitiveCount = uploadedCount(AssetBuffer::Primitives),
        .nodeInstanceCount = nodeInstanceCount,
        .drawRecordCount = drawRecordCount[frameSlot],
        .pad0 = 0,
    };
}

void ResourceManager::destroyAssetBuffer(AssetBuffer id)
{
    DeviceBuffer& target = assetBuffers[std::to_underlying(id)];
    if (target.memory == nullptr) {
        return;
    }
    const AssetBufferInfo& info = kAssetBufferInfo[std::to_underlying(id)];
    logInfo(std::format("destroy {} (used {} / cap {} bda=0x{:x})", info.tracyName, formatBytes(target.usedBytes),
                         formatBytes(target.capacityBytes), static_cast<uint64_t>(target.address)),
             "ResourceManager");
    destroyVmaBuffer(allocator.allocator, target.buffer, target.memory, info.tracyName);
    target.address = 0;
    target.usedBytes = 0;
    target.capacityBytes = 0;
}

uint32_t ResourceManager::uploadedCount(AssetBuffer id) const noexcept
{
    return static_cast<uint32_t>(asset(id).usedBytes / kAssetBufferInfo[std::to_underlying(id)].stride);
}

vk::DeviceAddress ResourceManager::modelAssetAddress(AssetBuffer id, ModelId modelId) const noexcept
{
    const vk::DeviceSize stride = kAssetBufferInfo[std::to_underlying(id)].stride;
    return asset(id).address + modelAllocations[modelId].gpu[std::to_underlying(id)].first * stride;
}

vk::DeviceSize ResourceManager::modelGpuBytes(ModelId modelId) const noexcept
{
    vk::DeviceSize bytes = 0;
    for (size_t i = 0; i < kAssetBufferCount; ++i) {
        bytes += modelAllocations[modelId].gpu[i].count * kAssetBufferInfo[i].stride;
    }
    return bytes;
}

std::optional<ModelId> ResourceManager::findModel(std::string_view key) const
{
    const auto it = std::ranges::find(modelAllocations, key, &ModelAllocation::key);
    if (it == modelAllocations.end()) {
        return std::nullopt;
    }
    return static_cast<ModelId>(it - modelAllocations.begin());
}

std::pair<vk::DeviceSize, vk::DeviceSize> ResourceManager::assetTotals() const noexcept
{
    vk::DeviceSize usedBytes = 0;
    vk::DeviceSize capacityBytes = 0;
    for (const DeviceBuffer& buffer : assetBuffers) {
        usedBytes += buffer.usedBytes;
        capacityBytes += buffer.capacityBytes;
    }
    return {usedBytes, capacityBytes};
}

vk::raii::CommandBuffer& ResourceManager::oneTimeTransferCmd()
{
    return transferCommandBuffer.empty() ? commandBuffers[0] : transferCommandBuffer[0];
}

const vk::raii::Queue& ResourceManager::oneTimeTransferQueue() const noexcept
{
    return transferCommandBuffer.empty() ? graphicsQueue : transferQueue;
}

ResourceManager::~ResourceManager()
{
    logInfo("Destructor called", "ResourceManager");
    destroyInstanceUboBuffers();
    for (HostBuffer& root : frameRoots) {
        destroyHostBuffer(root, kFrameRootTracyName);
    }
    destroyHostBuffer(modelTable, kModelTableTracyName);
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        destroyHostBuffer(nodeInstanceBuffers[i], kNodeInstanceTracyName);
        destroyHostBuffer(drawRecordBuffers[i], kDrawRecordTracyName);
    }
    for (size_t i = 0; i < kAssetBufferCount; ++i) {
        destroyAssetBuffer(static_cast<AssetBuffer>(i));
    }
    destroyAttachment(colorAttachment, kColorTracyName);
    destroyAttachment(depthAttachment, kDepthTracyName);
}

void ResourceManager::init()
{
    logInfo("init() started", "ResourceManager");
    createCommandPool();
    createCommandBuffers();
    createUniformBuffers();
    flushGpuAssets();
}

void ResourceManager::createSyncObjects()
{
    logInfo("createSyncObjects() started", "ResourceManager");
    presentCompleteSemaphore.clear();
    renderFinishedSemaphore.clear();
    inFlightFences.clear();

    // acquire semaphore and fence per frame-in-flight
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        presentCompleteSemaphore.emplace_back(device, vk::SemaphoreCreateInfo());
        inFlightFences.emplace_back(device, vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled});
    }
    // present signal per swapchain image
    for (size_t i = 0; i < swapChainImageCount; i++) {
        renderFinishedSemaphore.emplace_back(device, vk::SemaphoreCreateInfo());
    }
}

void ResourceManager::updateUniformBuffers(uint32_t currentImage)
{
    ZoneScopedN("ResourceManager::updateUniformBuffer");
    ensureInstanceCapacity(objectStorage.size());
    writeFrameRoot(currentImage);
    // reads prevModelMatrices before the entity loop below advances them
    writeNodeInstances(currentImage);

    // write-combined memory: whole rows, never read back
    auto* instances = static_cast<GpuObjectUB*>(instanceUbos[currentImage].mapped);
    for (EntityId id = 0; id < objectStorage.size(); ++id) {
        if ((objectStorage.flags[id] & EntityFlag::Active) == 0) {
            continue;
        }
        const glm::mat4 model = computeModelMatrix(objectStorage.transforms[id]);
        instances[id] = GpuObjectUB{
            .modelMatrix = model,
            .prevModelMatrix = objectStorage.prevModelMatrices[id],
            .boundingSphere = transformBoundingSphere(model, objectStorage.localBounds[id]),
            // entities loaded from one file share its model
            .modelIndex = objectStorage.modelIds[id],
            .instanceFlags = objectStorage.flags[id],
            .pad0 = 0,
            .pad1 = 0,
        };
        objectStorage.prevModelMatrices[id] = model;
    }
}

void ResourceManager::createCommandPool()
{
    logInfo("createCommandPool() started", "ResourceManager");
    vk::CommandPoolCreateInfo poolInfo{.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                       .queueFamilyIndex = graphicsIndex};
    commandPool = vk::raii::CommandPool(device, poolInfo);
    setDebugName(device, commandPool, "GraphicsCommandPool");
    if (deviceWrapper.hasDedicatedTransferQueue()) {
        vk::CommandPoolCreateInfo transferPoolInfo{.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                                   .queueFamilyIndex = transferIndex};
        transferCommandPool = vk::raii::CommandPool(device, transferPoolInfo);
        setDebugName(device, transferCommandPool, "TransferCommandPool");
    }
}

void ResourceManager::createCommandBuffers()
{
    logInfo("createCommandBuffers() started", "ResourceManager");
    commandBuffers.clear();
    vk::CommandBufferAllocateInfo allocInfo{.commandPool = commandPool,
                                            .level = vk::CommandBufferLevel::ePrimary,
                                            .commandBufferCount = MAX_FRAMES_IN_FLIGHT};
    commandBuffers = vk::raii::CommandBuffers(device, allocInfo);
    for (size_t i = 0; i < commandBuffers.size(); ++i) {
        setDebugName(device, commandBuffers[i], std::format("GraphicsCommandBuffer_{}", i));
    }
    if (deviceWrapper.hasDedicatedTransferQueue()) {
        vk::CommandBufferAllocateInfo transferAllocInfo{.commandPool = transferCommandPool,
                                                        .level = vk::CommandBufferLevel::ePrimary,
                                                        .commandBufferCount = MAX_FRAMES_IN_FLIGHT};
        transferCommandBuffer = vk::raii::CommandBuffers(device, transferAllocInfo);
        for (size_t i = 0; i < transferCommandBuffer.size(); ++i) {
            setDebugName(device, transferCommandBuffer[i], std::format("TransferCommandBuffer_{}", i));
        }
    }
    logInfo(std::format("Command buffers allocated: {}", commandBuffers.size()), "ResourceManager");
    logInfo(std::format("Transfer command buffers allocated: {}", transferCommandBuffer.size()), "ResourceManager");
}

void ResourceManager::createAssetBuffer(AssetBuffer id, vk::DeviceSize capacity, vk::raii::Buffer& buffer,
                                        VmaAllocation& memory) const
{
    createBuffer(capacity,
                 vk::BufferUsageFlagBits2::eTransferSrc | vk::BufferUsageFlagBits2::eTransferDst |
                     vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eDeviceLocal, buffer, memory, allocator.allocator, device,
                 queueFamilyIndices, kAssetBufferInfo[std::to_underlying(id)].debugName);
}

void ResourceManager::installAssetBuffer(AssetBuffer id, vk::raii::Buffer&& buffer, VmaAllocation memory,
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

void ResourceManager::uploadAssetBuffers(std::span<const AssetUpload> uploads)
{
    ZoneScopedN("ResourceManager::uploadAssetBuffers");
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

    vk::raii::Buffer staging({});
    VmaAllocation stagingMemory = nullptr;
    createBuffer(stagingBytes, vk::BufferUsageFlagBits2::eTransferSrc,
                 vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, staging,
                 stagingMemory, allocator.allocator, device, queueFamilyIndices, "AssetStaging");
    for (size_t i = 0; i < uploads.size(); ++i) {
        if (!uploads[i].bytes.empty()) {
            vmaCopyMemoryToAllocation(allocator.allocator, uploads[i].bytes.data(), stagingMemory, stagingOffsets[i],
                                      uploads[i].bytes.size_bytes());
        }
    }

    struct Grown {
        AssetBuffer id;
        vk::raii::Buffer buffer;
        VmaAllocation memory;
        vk::DeviceSize usedBytes;
        vk::DeviceSize capacityBytes;
    };
    std::vector<Grown> grown;
    vk::raii::CommandBuffer& cmd = oneTimeTransferCmd();
    cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    for (size_t i = 0; i < uploads.size(); ++i) {
        const AssetUpload& upload = uploads[i];
        DeviceBuffer& dst = assetBuffers[std::to_underlying(upload.id)];
        const std::string_view debugName = kAssetBufferInfo[std::to_underlying(upload.id)].debugName;
        const vk::DeviceSize srcBytes = upload.bytes.size_bytes();
        if (srcBytes == 0) {
            logInfo(std::format("skip {}: empty scratch (used {} / cap {})", debugName, formatBytes(dst.usedBytes),
                                 formatBytes(dst.capacityBytes)),
                     "ResourceManager");
            continue;
        }
        const vk::DeviceSize writeEnd = upload.dstOffset + srcBytes;
        const vk::DeviceSize newUsed = std::max(dst.usedBytes, writeEnd);
        const vk::BufferCopy stagingCopy(stagingOffsets[i], upload.dstOffset, srcBytes);

        if (dst.memory != nullptr && writeEnd <= dst.capacityBytes) {
            logInfo(std::format("in place {}: {} at {} -> used {} (cap {})", debugName, formatBytes(srcBytes),
                                 formatBytes(upload.dstOffset), formatBytes(newUsed), formatBytes(dst.capacityBytes)),
                     "ResourceManager");
            cmd.copyBuffer(staging, dst.buffer, stagingCopy);
            dst.usedBytes = newUsed;
            continue;
        }

        const vk::DeviceSize newCapacity = nextCapacity(dst.capacityBytes, newUsed);
        logInfo(std::format("{} {}: used {} -> {} (cap {} -> {})", dst.usedBytes == 0 ? "create" : "grow", debugName,
                             formatBytes(dst.usedBytes), formatBytes(newUsed), formatBytes(dst.capacityBytes),
                             formatBytes(newCapacity)),
                 "ResourceManager");
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

    submitAndWait(cmd, oneTimeTransferQueue());
    for (Grown& next : grown) {
        installAssetBuffer(next.id, std::move(next.buffer), next.memory, next.usedBytes, next.capacityBytes);
    }
    destroyVmaBuffer(allocator.allocator, staging, stagingMemory);

    for (const AssetUpload& upload : uploads) {
        if (upload.bytes.empty()) {
            continue;
        }
        const DeviceBuffer& dst = asset(upload.id);
        const std::string message =
            std::format("{} ready used {} / cap {} bda=0x{:x}", kAssetBufferInfo[std::to_underlying(upload.id)].debugName,
                        formatBytes(dst.usedBytes), formatBytes(dst.capacityBytes), static_cast<uint64_t>(dst.address));
        logInfo(message, "ResourceManager");
        TracyMessage(message.c_str(), message.size());
    }
}

std::span<const std::byte> ResourceManager::scratchBytes(AssetBuffer id, const PackedScratch& packed) const
{
    // absent optional streams are zero-filled in scratch; skipping them leaves the slice empty and the bit clear
    const auto optionalStream = [this](const auto& column, uint32_t attribute) {
        return (geometryStore.presentStreams & attribute) != 0 ? std::as_bytes(std::span(column))
                                                               : std::span<const std::byte>{};
    };
    switch (id) {
    case AssetBuffer::Vertices:         return std::as_bytes(std::span(geometryStore.vertices));
    case AssetBuffer::Meshlets:         return std::as_bytes(std::span(geometryStore.meshlets));
    case AssetBuffer::MeshletVertices:  return std::as_bytes(std::span(geometryStore.meshletVertices));
    case AssetBuffer::MeshletTriangles: return std::as_bytes(std::span(geometryStore.meshletTriangles));
    case AssetBuffer::Normals:          return optionalStream(geometryStore.normals, GpuModelAttribute::Normals);
    case AssetBuffer::Tangents:         return optionalStream(geometryStore.tangents, GpuModelAttribute::Tangents);
    case AssetBuffer::Uv1:              return optionalStream(geometryStore.uv1, GpuModelAttribute::Uv1);
    case AssetBuffer::Joints:           return optionalStream(geometryStore.joints0, GpuModelAttribute::Joints);
    case AssetBuffer::Weights:          return optionalStream(geometryStore.weights0, GpuModelAttribute::Weights);
    case AssetBuffer::Indices:          return std::as_bytes(std::span(geometryStore.indices));
    case AssetBuffer::MorphPos:         return std::as_bytes(std::span(geometryStore.morphPos));
    case AssetBuffer::MorphNrm:         return std::as_bytes(std::span(geometryStore.morphNrm));
    case AssetBuffer::MorphTan:         return std::as_bytes(std::span(geometryStore.morphTan));
    case AssetBuffer::MorphTargets:     return std::as_bytes(std::span(packed.morphTargets));
    case AssetBuffer::MorphWeights:
        return std::as_bytes(std::span(geometryStore.morphWeights).subspan(geometryStore.flushedMorphWeightCount));
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

void ResourceManager::remapScratchOffsets()
{
    ZoneScopedN("ResourceManager::remapScratchOffsets");
    // GPU data stays model-relative (see ModelAllocation); only the CPU catalog becomes absolute
    GeometryStore& geometry = geometryStore;
    const uint32_t vertexBase = uploadedCount(AssetBuffer::Vertices);
    const uint32_t meshletBase = uploadedCount(AssetBuffer::Meshlets);
    const uint32_t indexBase = uploadedCount(AssetBuffer::Indices);
    const uint32_t newPrimitives =
        static_cast<uint32_t>(geometry.primitiveDraws.size()) - geometry.flushedPrimitiveCount;
    const uint32_t newMorphTargets =
        static_cast<uint32_t>(geometry.morphTargets.size()) - geometry.flushedMorphTargetCount;
    logInfo(std::format("remap scratch: +{} prims +{} morphTargets onto gpu verts={} meshlets={} indices={}",
                         newPrimitives, newMorphTargets, vertexBase, meshletBase, indexBase),
             "ResourceManager");
    for (uint32_t p = geometry.flushedPrimitiveCount; p < geometry.primitiveDraws.size(); ++p) {
        PrimitiveDraw& draw = geometry.primitiveDraws[p];
        draw.firstVertex += vertexBase;
        draw.firstIndex += indexBase;
        draw.meshlets.firstMeshlet += meshletBase;
    }
    auto bump = [](uint32_t& value, uint32_t delta) {
        if (value != kNoneIndex) {
            value += delta;
        }
    };
    const uint32_t morphPosBase = uploadedCount(AssetBuffer::MorphPos);
    const uint32_t morphNrmBase = uploadedCount(AssetBuffer::MorphNrm);
    const uint32_t morphTanBase = uploadedCount(AssetBuffer::MorphTan);
    for (uint32_t t = geometry.flushedMorphTargetCount; t < geometry.morphTargets.size(); ++t) {
        MorphTarget& target = geometry.morphTargets[t];
        bump(target.posOffset, morphPosBase);
        bump(target.nrmOffset, morphNrmBase);
        bump(target.tanOffset, morphTanBase);
    }
    for (EntityId id = 0; id < objectStorage.size(); ++id) {
        if (objectStorage.firstPrimitives[id] >= geometry.flushedPrimitiveCount) {
            objectStorage.meshletDraws[id].firstMeshlet += meshletBase;
        }
    }
}

std::vector<GpuPrimitive> ResourceManager::packScratchPrimitives() const
{
    ZoneScopedN("ResourceManager::packScratchPrimitives");
    std::vector<GpuPrimitive> packed;
    packed.reserve(geometryStore.primitiveDraws.size() - geometryStore.flushedPrimitiveCount);
    for (uint32_t p = geometryStore.flushedPrimitiveCount; p < geometryStore.primitiveDraws.size(); ++p) {
        const PrimitiveDraw& draw = geometryStore.primitiveDraws[p];
        const bool isDefaultMaterial =
            draw.materialId == kNoneIndex || draw.materialId == materialStore.defaultMaterialId();
        const bool hasMorphWeights = draw.morphCount > 0 && draw.morphWeightFirst != kNoneIndex;
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
            .morphFirst = draw.morphCount > 0 ? draw.morphFirst - geometryStore.flushedMorphTargetCount : 0,
            .morphCount = draw.morphCount,
            .morphWeightFirst =
                hasMorphWeights ? draw.morphWeightFirst - geometryStore.flushedMorphWeightCount : kNoneIndex,
        });
    }
    return packed;
}

std::vector<GpuNode> ResourceManager::packScratchNodes() const
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
        });
    }
    return packed;
}

void ResourceManager::rebuildNodeInstanceLayout()
{
    entityNodeInstanceFirst.resize(objectStorage.size());
    uint32_t rows = 0;
    for (EntityId id = 0; id < objectStorage.size(); ++id) {
        entityNodeInstanceFirst[id] = rows;
        const ModelId modelId = objectStorage.modelIds[id];
        if (modelId != kInvalidModelId) {
            rows += modelAllocations[modelId].nodes.count;
        }
    }
    nodeInstanceCount = rows;
    if (rows <= nodeInstanceCapacity && nodeInstanceBuffers[0].memory != nullptr) {
        return;
    }
    // callers hold the GPU idle, so every frame slot can be replaced
    nodeInstanceCapacity = std::max({rows, nodeInstanceCapacity * 2, 1u});
    logInfo(std::format("node instances: {} rows, capacity {}", rows, nodeInstanceCapacity), "ResourceManager");
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        destroyHostBuffer(nodeInstanceBuffers[i], kNodeInstanceTracyName);
        createHostBuffer(nodeInstanceBuffers[i], sizeof(GpuNodeInstance) * static_cast<vk::DeviceSize>(nodeInstanceCapacity),
                         std::format("NodeInstances_{}", i), kNodeInstanceTracyName);
    }
}

void ResourceManager::writeNodeInstances(uint32_t frameSlot)
{
    ZoneScopedN("ResourceManager::writeNodeInstances");
    // write-combined memory: whole rows, never read back
    auto* rows = static_cast<GpuNodeInstance*>(nodeInstanceBuffers[frameSlot].mapped);
    const auto entityCount = static_cast<EntityId>(entityNodeInstanceFirst.size());
    for (EntityId id = 0; id < entityCount; ++id) {
        const ModelId modelId = objectStorage.modelIds[id];
        if (modelId == kInvalidModelId || (objectStorage.flags[id] & EntityFlag::Active) == 0) {
            continue;
        }
        const glm::mat4 entityMatrix = computeModelMatrix(objectStorage.transforms[id]);
        const glm::mat4& prevEntityMatrix = objectStorage.prevModelMatrices[id];
        const ElementRange nodes = modelAllocations[modelId].nodes;
        for (uint32_t k = 0; k < nodes.count; ++k) {
            const NodeDraw& node = geometryStore.nodeDraws[nodes.first + k];
            const glm::mat4 world = entityMatrix * node.localToModel;
            // the entity transform can mirror too, so winding follows the combined matrix
            const bool isMirrored = glm::determinant(glm::mat3(world)) < 0.0f;
            rows[entityNodeInstanceFirst[id] + k] = GpuNodeInstance{
                .worldMatrix = world,
                .prevWorldMatrix = prevEntityMatrix * node.localToModel,
                .boundingSphere = transformBoundingSphere(entityMatrix, node.boundingSphere),
                .entityIndex = id,
                .modelIndex = modelId,
                .nodeIndex = k,
                .flags = (node.flags & ~NodeFlag::FlipWinding) | (isMirrored ? NodeFlag::FlipWinding : 0u),
            };
        }
    }
}

GpuDrawRecord* ResourceManager::mapDrawRecords(uint32_t frameSlot, uint32_t count)
{
    if (count > drawRecordCapacity[frameSlot] || drawRecordBuffers[frameSlot].memory == nullptr) {
        // only this slot is replaced; its previous frame finished before recording started
        drawRecordCapacity[frameSlot] = std::max({count, drawRecordCapacity[frameSlot] * 2, 64u});
        destroyHostBuffer(drawRecordBuffers[frameSlot], kDrawRecordTracyName);
        createHostBuffer(drawRecordBuffers[frameSlot],
                         sizeof(GpuDrawRecord) * static_cast<vk::DeviceSize>(drawRecordCapacity[frameSlot]),
                         std::format("DrawRecords_{}", frameSlot), kDrawRecordTracyName);
    }
    drawRecordCount[frameSlot] = count;
    return static_cast<GpuDrawRecord*>(drawRecordBuffers[frameSlot].mapped);
}

std::vector<GpuLight> ResourceManager::packScratchLights() const
{
    ZoneScopedN("ResourceManager::packScratchLights");
    std::vector<GpuLight> packed;
    packed.reserve(lightStore.instances.size());
    logInfo(std::format("pack lights: {} defs {} instances (gpu already {})", lightStore.defs.size(),
                         lightStore.instances.size(), lightStore.uploadedCount()),
             "ResourceManager");
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

void ResourceManager::createCameraBuffers(Camera& camera)
{
    logInfo("createCameraBuffers() started", "ResourceManager");
    camera.allocator = allocator.allocator;
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        constexpr vk::DeviceSize bufferSize = sizeof(GpuCameraData);
        vk::raii::Buffer buffer({});
        VmaAllocation bufferMem = nullptr;
        createBuffer(bufferSize,
                     vk::BufferUsageFlagBits2::eUniformBuffer | vk::BufferUsageFlagBits2::eStorageBuffer |
                         vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                     vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, buffer,
                     bufferMem, allocator.allocator, device, queueFamilyIndices,
                     std::format("CameraUniformBuffer_{}", i));
        camera.cameraBuffers[i] = std::move(buffer);
        camera.cameraBuffersMemory[i] = bufferMem;
        void* data = nullptr;
        vmaMapMemory(allocator.allocator, bufferMem, &data);
        camera.cameraBuffersMapped[i] = data;
        camera.cameraBufferAddresses[i] = device.getBufferAddress({.buffer = *camera.cameraBuffers[i]});
        cameraAddresses[i] = camera.cameraBufferAddresses[i];
        tracyResourceAlloc(static_cast<VkBuffer>(*camera.cameraBuffers[i]), static_cast<size_t>(bufferSize),
                           "GPU/CameraUBO");
    }
}

void ResourceManager::ensureInstanceCapacity(uint32_t entityCount)
{
    if (entityCount <= instanceCapacity)
    {
        return;
    }

    ZoneScopedN("ResourceManager::ensureInstanceCapacity");
    // headroom so interactive loads do not reallocate every time
    const uint32_t newCapacity = std::max(entityCount, instanceCapacity * 2);
    logInfo(std::format("Growing instance GpuObjectUB capacity {} -> {}", instanceCapacity, newCapacity),
             "ResourceManager");

    destroyInstanceUboBuffers();
    instanceCapacity = newCapacity;

    const vk::DeviceSize bufferSize = sizeof(GpuObjectUB) * static_cast<vk::DeviceSize>(instanceCapacity);
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
    {
        createHostBuffer(instanceUbos[i], bufferSize, std::format("InstanceObjectUB_{}", i), kInstanceUboTracyName);
    }
}

void ResourceManager::createUniformBuffers()
{
    logInfo("createUniformBuffers() started", "ResourceManager");
    ensureInstanceCapacity(std::max(objectStorage.size(), 1u));
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        createHostBuffer(frameRoots[i], sizeof(GpuFrameRoot), std::format("FrameRoot_{}", i), kFrameRootTracyName);
    }
    rebuildModelTable();
    rebuildNodeInstanceLayout();
}

void ResourceManager::flushGpuAssets()
{
    ZoneScopedN("ResourceManager::flushGpuAssets");
    logInfo("flushGpuAssets() started", "ResourceManager");
    logInfo(std::format("cpu scratch: verts={} meshlets={} meshletVerts={} triCorners={} indices={} "
                         "normals={} tangents={} uv1={} joints={} weights={} morphPos/Nrm/Tan={}/{}/{} "
                         "new materials={} lights={}/{} prims={} (flushed {})",
                         geometryStore.vertices.size(), geometryStore.meshlets.size(),
                         geometryStore.meshletVertices.size(), geometryStore.meshletTriangles.size(),
                         geometryStore.indices.size(), geometryStore.normals.size(), geometryStore.tangents.size(),
                         geometryStore.uv1.size(), geometryStore.joints0.size(), geometryStore.weights0.size(),
                         geometryStore.morphPos.size(), geometryStore.morphNrm.size(), geometryStore.morphTan.size(),
                         materialStore.dirtyRows.size(), lightStore.defs.size(), lightStore.instances.size(), geometryStore.primitiveDraws.size(),
                         geometryStore.flushedPrimitiveCount),
             "ResourceManager");

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
        .morphTargets = {geometryStore.flushedMorphTargetCount,
                         static_cast<uint32_t>(geometryStore.morphTargets.size()) -
                             geometryStore.flushedMorphTargetCount},
        .morphWeights = {geometryStore.flushedMorphWeightCount,
                         static_cast<uint32_t>(geometryStore.morphWeights.size()) -
                             geometryStore.flushedMorphWeightCount},
        .auxBlobs = {geometryStore.flushedAuxBlobCount,
                     static_cast<uint32_t>(geometryStore.auxBlobs.size()) - geometryStore.flushedAuxBlobCount},
        .nodes = {geometryStore.flushedNodeCount,
                  static_cast<uint32_t>(geometryStore.nodeDraws.size()) - geometryStore.flushedNodeCount},
        .key = objectStorage.names[entity],
        .meshletDraw = {},
        .localBounds = glm::vec4{0.0f},
        .lights = packScratchLights(),
        .materialRefs = {},
        .textureRefs = std::move(geometryStore.textureRefs),
        .refCount = 0,
    };

    for (uint32_t n = allocation.nodes.first; n < allocation.nodes.end(); ++n) {
        allocation.localBounds = mergeBoundingSpheres(allocation.localBounds, geometryStore.nodeDraws[n].boundingSphere);
    }
    PackedScratch packed{
        .primitives = packScratchPrimitives(),
        .nodes = packScratchNodes(),
        .morphTargets =
            packMorphTargets(std::span(geometryStore.morphTargets).subspan(geometryStore.flushedMorphTargetCount)),
        .materialRows = {},
        .materials = {},
        .extensions = {},
    };
    remapScratchOffsets();

    logInfo(std::format("material cache: rows={} live={} new={} hits={} misses={}", materialStore.size(),
                         materialStore.liveCount(), materialStore.dirtyRows.size(), materialStore.cacheHits,
                         materialStore.cacheMisses),
             "ResourceManager");

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
    // remapped to GPU-absolute above
    allocation.meshletDraw = objectStorage.meshletDraws[entity];
    allocation.materialRefs = materialStore.commitModel();
    modelAllocations.push_back(std::move(allocation));
    attachEntity(entity, modelId);
    uploadLights();
    rebuildNodeInstanceLayout();

    geometryStore.clearScratch();
    lightStore.clearScratch();
    rebuildModelTable();
#if ENGINE_USE_MIMALLOC
    // load scratch (decoded images, geometry, file buffers) is freed by now; return those pages to the OS
    mi_collect(true);
#endif

    logInfo(std::format("gpu catalog: verts={} meshlets={} meshletVerts={} triCorners={} indices={} "
                         "morphPos/Nrm/Tan={}/{}/{} materials={} lights={} prims={} morphTargets={}",
                         uploadedCount(AssetBuffer::Vertices), uploadedCount(AssetBuffer::Meshlets),
                         uploadedCount(AssetBuffer::MeshletVertices), uploadedCount(AssetBuffer::MeshletTriangles),
                         uploadedCount(AssetBuffer::Indices), uploadedCount(AssetBuffer::MorphPos),
                         uploadedCount(AssetBuffer::MorphNrm), uploadedCount(AssetBuffer::MorphTan),
                         materialStore.liveCount(), lightStore.uploadedCount(), geometryStore.primitiveDraws.size(),
                         geometryStore.morphTargets.size()),
             "ResourceManager");
    const auto [gpuAssetBytes, gpuAssetCapacity] = assetTotals();
    logInfo(std::format("gpu ssbo total: used {} / cap {}", formatBytes(gpuAssetBytes),
                         formatBytes(gpuAssetCapacity)),
             "ResourceManager");
    logInfo(std::format("cpu after flush: scratch verts={} meshlets={} | material cache={} lights scratch={}",
                         geometryStore.vertices.size(), geometryStore.meshlets.size(), materialStore.size(),
                         lightStore.instances.size()),
             "ResourceManager");
#ifdef TRACY_ENABLE
    const std::string tracyMsg = std::format(
        "flush gpu assets total={} verts={} meshlets={} materials={} lights={}", formatBytes(gpuAssetBytes),
        uploadedCount(AssetBuffer::Vertices), uploadedCount(AssetBuffer::Meshlets), materialStore.liveCount(),
        lightStore.uploadedCount());
    TracyMessage(tracyMsg.c_str(), tracyMsg.size());
#endif
    tracyPlotResources();
}

void ResourceManager::discardScratch()
{
    logInfo("flushGpuAssets: no new model, discarding load scratch", "ResourceManager");
    geometryStore.discardScratch();
    lightStore.clearScratch();
    materialStore.discardModel();
#if ENGINE_USE_MIMALLOC
    mi_collect(true);
#endif
}

void ResourceManager::appendMaterialUploads(PackedScratch& packed, std::vector<AssetUpload>& uploads) const
{
    packed.materialRows = materialStore.dirtyRows;
    std::ranges::sort(packed.materialRows);
    std::vector<MaterialPbrExtension> extensions;
    extensions.reserve(packed.materialRows.size());
    for (const uint32_t id : packed.materialRows) {
        packed.materials.push_back(materialStore.infos[id].material);
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

void ResourceManager::attachEntity(EntityId entity, ModelId modelId)
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

void ResourceManager::uploadLights()
{
    const std::span<const std::byte> bytes = std::as_bytes(std::span(lightStore.uploaded));
    if (!bytes.empty()) {
        const std::array uploads{AssetUpload{.id = AssetBuffer::Lights, .bytes = bytes, .dstOffset = 0}};
        uploadAssetBuffers(uploads);
    }
    // the mirror may have shrunk; rows past it are stale and GpuFrameRoot::lightCount stops before them
    assetBuffers[std::to_underlying(AssetBuffer::Lights)].usedBytes = bytes.size_bytes();
}

EntityId ResourceManager::instantiateModel(ModelId modelId, const Transform& transform)
{
    ZoneScopedN("ResourceManager::instantiateModel");
    const ModelAllocation& model = modelAllocations.at(modelId);
    const EntityId entity = objectStorage.create(transform, model.meshletDraw, model.primitives.first,
                                                 model.primitives.count, model.key);
    attachEntity(entity, modelId);
    uploadLights();
    rebuildNodeInstanceLayout();
    logInfo(std::format("instance entity {} of model {} '{}': {} entities share {} on GPU", entity, modelId, model.key,
                         modelAllocations[modelId].refCount, formatBytes(modelGpuBytes(modelId))),
             "ResourceManager");
    tracyPlotResources();
    return entity;
}

std::vector<uint32_t> ResourceManager::eraseEntity(EntityId id)
{
    ZoneScopedN("ResourceManager::eraseEntity");
    if (id >= objectStorage.size()) {
        throw std::out_of_range(std::format("eraseEntity: no entity {}", id));
    }
    const ModelId modelId = objectStorage.modelIds[id];
    logInfo(std::format("erase entity {} '{}' (model {})", id, objectStorage.names[id], modelId), "ResourceManager");

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
    rebuildNodeInstanceLayout();
    if (objectStorage.empty()) {
        geometryStore.extensionsUsed.clear();
        geometryStore.extensionsRequired.clear();
    }

    const auto [gpuAssetBytes, gpuAssetCapacity] = assetTotals();
    logInfo(std::format("gpu ssbo total after erase: used {} / cap {}", formatBytes(gpuAssetBytes),
                         formatBytes(gpuAssetCapacity)),
             "ResourceManager");
    tracyPlotResources();
    return releasedTextures;
}

std::vector<uint32_t> ResourceManager::eraseModel(ModelId modelId)
{
    ZoneScopedN("ResourceManager::eraseModel");
    logInfo(std::format("erase model {} '{}': {} on GPU, {} primitives, {} materials, {} textures", modelId,
                         modelAllocations[modelId].key, formatBytes(modelGpuBytes(modelId)),
                         modelAllocations[modelId].primitives.count, modelAllocations[modelId].materialRefs.size(),
                         modelAllocations[modelId].textureRefs.size()),
             "ResourceManager");
    ModelAllocation removed = std::move(modelAllocations[modelId]);
    compactAssetBuffers(removed);
    rebaseCatalog(modelId, removed);
    materialStore.release(removed.materialRefs);
    rebuildModelTable();
    return std::move(removed.textureRefs);
}

void ResourceManager::compactAssetBuffers(const ModelAllocation& removed)
{
    ZoneScopedN("ResourceManager::compactAssetBuffers");
    struct Compacted {
        AssetBuffer id;
        vk::raii::Buffer buffer;
        VmaAllocation memory;
        vk::DeviceSize usedBytes;
    };
    std::vector<Compacted> compacted;
    vk::raii::CommandBuffer& cmd = oneTimeTransferCmd();
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
        submitAndWait(cmd, oneTimeTransferQueue());
    }
    for (Compacted& next : compacted) {
        installAssetBuffer(next.id, std::move(next.buffer), next.memory, next.usedBytes, next.usedBytes);
    }
}

void ResourceManager::rebaseCatalog(ModelId modelId, const ModelAllocation& removed)
{
    ZoneScopedN("ResourceManager::rebaseCatalog");
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
    eraseSlice(geometry.morphTargets, removed.morphTargets);
    eraseSlice(geometry.morphWeights, removed.morphWeights);
    eraseSlice(geometry.auxBlobs, removed.auxBlobs);
    eraseSlice(geometry.nodeDraws, removed.nodes);
    geometry.flushedNodeCount -= removed.nodes.count;
    geometry.flushedPrimitiveCount -= removed.primitives.count;
    geometry.flushedMorphTargetCount -= removed.morphTargets.count;
    geometry.flushedMorphWeightCount -= removed.morphWeights.count;
    geometry.flushedAuxBlobCount -= removed.auxBlobs.count;

    // material ids are shared rows that never move
    for (PrimitiveDraw& draw : geometry.primitiveDraws) {
        shift(draw.meshlets.firstMeshlet, gpuRange(AssetBuffer::Meshlets));
        shift(draw.firstVertex, gpuRange(AssetBuffer::Vertices));
        shift(draw.firstIndex, gpuRange(AssetBuffer::Indices));
        shift(draw.morphFirst, removed.morphTargets);
        shift(draw.morphWeightFirst, removed.morphWeights);
    }
    for (NodeDraw& node : geometry.nodeDraws) {
        shift(node.firstPrimitive, removed.primitives);
    }
    for (MorphTarget& target : geometry.morphTargets) {
        shift(target.posOffset, gpuRange(AssetBuffer::MorphPos));
        shift(target.nrmOffset, gpuRange(AssetBuffer::MorphNrm));
        shift(target.tanOffset, gpuRange(AssetBuffer::MorphTan));
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
        shift(allocation.morphTargets.first, removed.morphTargets);
        shift(allocation.morphWeights.first, removed.morphWeights);
        shift(allocation.auxBlobs.first, removed.auxBlobs);
        shift(allocation.nodes.first, removed.nodes);
        shift(allocation.meshletDraw.firstMeshlet, gpuRange(AssetBuffer::Meshlets));
    }
}

void ResourceManager::tracyPlotResources() const
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
    TracyPlot("Vulkan/MorphPosCount", static_cast<double>(uploadedCount(AssetBuffer::MorphPos)));
    TracyPlot("Vulkan/MorphNrmCount", static_cast<double>(uploadedCount(AssetBuffer::MorphNrm)));
    TracyPlot("Vulkan/MorphTanCount", static_cast<double>(uploadedCount(AssetBuffer::MorphTan)));
    for (size_t i = 0; i < kAssetBufferCount; ++i) {
        TracyPlot(kAssetBufferInfo[i].plotUsed, static_cast<double>(assetBuffers[i].usedBytes));
        TracyPlot(kAssetBufferInfo[i].plotCapacity, static_cast<double>(assetBuffers[i].capacityBytes));
    }
    const auto [gpuAssetBytes, gpuAssetCapacity] = assetTotals();
    TracyPlot("Vulkan/GpuAssetBytes", static_cast<double>(gpuAssetBytes));
    TracyPlot("Vulkan/GpuAssetCapacityBytes", static_cast<double>(gpuAssetCapacity));
    TracyPlot("Vulkan/CpuScratchVertices", static_cast<double>(geometryStore.vertices.size()));
    TracyPlot("Vulkan/CpuScratchMeshlets", static_cast<double>(geometryStore.meshlets.size()));
    TracyPlot("Vulkan/InstanceCapacity", static_cast<double>(instanceCapacity));
    TracyPlot("Vulkan/NodeInstanceCount", static_cast<double>(nodeInstanceCount));
    TracyPlot("Vulkan/NodeCount", static_cast<double>(geometryStore.nodeDraws.size()));
    TracyPlot("Vulkan/InstanceUboBytes", static_cast<double>(sizeof(GpuObjectUB) * instanceCapacity));
    TracyPlot("Vulkan/MeshBdaReady",
              static_cast<double>(asset(AssetBuffer::Vertices).address != 0 &&
                                  asset(AssetBuffer::Meshlets).address != 0 &&
                                  asset(AssetBuffer::MeshletVertices).address != 0 &&
                                  asset(AssetBuffer::MeshletTriangles).address != 0));
#endif
}

vk::Format ResourceManager::findSupportedFormat(const std::vector<vk::Format>& candidates, vk::ImageTiling tiling,
                                                vk::FormatFeatureFlags features)
{
    for (const auto format : candidates) {
        vk::FormatProperties props = physicalDevice.getFormatProperties2(format).formatProperties;

        if (tiling == vk::ImageTiling::eLinear && (props.linearTilingFeatures & features) == features) {
            return format;
        }
        if (tiling == vk::ImageTiling::eOptimal && (props.optimalTilingFeatures & features) == features) {
            return format;
        }
    }
    throw std::runtime_error("failed to find supported format!");
}

vk::raii::ImageView ResourceManager::createImageView(vk::raii::Image& image, vk::Format format,
                                                     vk::ImageAspectFlags aspectFlags, uint32_t mipLevels)
{
    vk::ImageViewCreateInfo viewInfo{.image = image,
                                     .viewType = vk::ImageViewType::e2D,
                                     .format = format,
                                     .subresourceRange = {.aspectMask = aspectFlags,
                                                          .baseMipLevel = 0,
                                                          .levelCount = mipLevels,
                                                          .baseArrayLayer = 0,
                                                          .layerCount = 1}};
    return vk::raii::ImageView(device, viewInfo);
}

void ResourceManager::destroyAttachment(AttachmentImage& target, const char* tracyName)
{
    // view references the image, release it first
    target.view = nullptr;
    destroyVmaImage(allocator.allocator, target.image, target.memory, tracyName);
}

void ResourceManager::createAttachment(AttachmentImage& target, const AttachmentDesc& desc)
{
    destroyAttachment(target, desc.tracyName);
    allocator.createImage2D(swapChainExtent.width, swapChainExtent.height, 1, msaaSamples, desc.format, desc.usage,
                            target.image, target.memory, std::format("{}Memory", desc.debugName));
    setDebugName(device, target.image, desc.debugName);
    // approx footprint: 4 B/pixel * samples
    const vk::DeviceSize approxBytes = static_cast<vk::DeviceSize>(swapChainExtent.width) * swapChainExtent.height *
        static_cast<uint32_t>(msaaSamples) * 4u;
    tracyResourceAlloc(static_cast<VkImage>(*target.image), static_cast<size_t>(approxBytes), desc.tracyName);

    vk::raii::CommandBuffer& cmd = commandBuffers[0];
    cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    transitionImageLayout(&cmd, target.image, vk::ImageLayout::eUndefined, desc.layout,
                          {.aspectMask = desc.barrierAspect,
                           .baseMipLevel = 0,
                           .levelCount = 1,
                           .baseArrayLayer = 0,
                           .layerCount = 1});
    submitAndWait(cmd, graphicsQueue);
    target.view = createImageView(target.image, desc.format, desc.viewAspect, 1);
}

void ResourceManager::createColorResources()
{
    logInfo("createColorResources() started", "ResourceManager");
    if (swapChainImageFormat == vk::Format::eUndefined) {
        return;
    }
    createAttachment(colorAttachment,
                     {.format = swapChainImageFormat,
                      .usage = vk::ImageUsageFlagBits::eTransientAttachment | vk::ImageUsageFlagBits::eColorAttachment,
                      .viewAspect = vk::ImageAspectFlagBits::eColor,
                      .barrierAspect = vk::ImageAspectFlagBits::eColor,
                      .layout = vk::ImageLayout::eColorAttachmentOptimal,
                      .debugName = "ColorImage",
                      .tracyName = kColorTracyName});
}

vk::Format ResourceManager::findDepthFormat()
{
    logInfo("findDepthFormat() started", "ResourceManager");
    // D32 last (BestPractices-NVIDIA-CreateImage-Depth32Format)
    return findSupportedFormat(
        {vk::Format::eD24UnormS8Uint, vk::Format::eD16Unorm, vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint},
        vk::ImageTiling::eOptimal, vk::FormatFeatureFlagBits::eDepthStencilAttachment);
}

void ResourceManager::updateSwapChainExtent(const vk::Extent2D newExtent)
{
    swapChainExtent = newExtent;
}

void ResourceManager::createDepthResources()
{
    logInfo("createDepthResources() started", "ResourceManager");
    const vk::Format depthFormat = findDepthFormat();
    logInfo(std::format("Depth format selected: {}", vk::to_string(depthFormat)), "ResourceManager");

    // view stays depth-only; barriers cover stencil too when separateDepthStencilLayouts is off
    vk::ImageAspectFlags barrierAspect = vk::ImageAspectFlagBits::eDepth;
    if (hasStencilComponent(depthFormat)) {
        barrierAspect |= vk::ImageAspectFlagBits::eStencil;
    }
    createAttachment(depthAttachment,
                     {.format = depthFormat,
                      .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment,
                      .viewAspect = vk::ImageAspectFlagBits::eDepth,
                      .barrierAspect = barrierAspect,
                      .layout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
                      .debugName = "DepthImage",
                      .tracyName = kDepthTracyName});
}

bool ResourceManager::hasStencilComponent(vk::Format format)
{
    return format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint ||
           format == vk::Format::eD16UnormS8Uint;
}
