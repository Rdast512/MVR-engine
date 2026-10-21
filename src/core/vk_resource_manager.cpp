#include "vk_resource_manager.hpp"
#include <algorithm>
#include <array>
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
        {"MaterialBuffer", "GPU/Materials", "Vulkan/MaterialBytes", "Vulkan/MaterialCapacityBytes",
         sizeof(GpuMaterial)},
        {"PbrExtBuffer", "GPU/PbrExtensions", "Vulkan/PbrExtBytes", "Vulkan/PbrExtCapacityBytes",
         sizeof(MaterialPbrExtension)},
        {"LightBuffer", "GPU/Lights", "Vulkan/LightBytes", "Vulkan/LightCapacityBytes", sizeof(GpuLight)},
        {"PrimitiveBuffer", "GPU/Primitives", "Vulkan/PrimitiveBytes", "Vulkan/PrimitiveCapacityBytes",
         sizeof(GpuPrimitive)},
    }};

    constexpr const char* kColorTracyName = "GPU/ColorMSAA";
    constexpr const char* kDepthTracyName = "GPU/Depth";
    constexpr const char* kInstanceUboTracyName = "GPU/InstanceUBO";
    constexpr const char* kFrameRootTracyName = "GPU/FrameRoot";
    constexpr const char* kModelTableTracyName = "GPU/ModelTable";

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
    destroyHostBuffer(modelTable, kModelTableTracyName);
    const auto modelCount = static_cast<uint32_t>(modelAllocations.size());
    createHostBuffer(modelTable, sizeof(GpuModel) * std::max<vk::DeviceSize>(modelCount, 1), "ModelTable",
                     kModelTableTracyName);

    auto* rows = static_cast<GpuModel*>(modelTable.mapped);
    for (EntityId id = 0; id < modelCount; ++id) {
        // a stream the model does not have stays 0
        const auto slice = [&](AssetBuffer buffer) -> vk::DeviceAddress {
            return modelAllocations[id].gpu[std::to_underlying(buffer)].count > 0 ? modelAssetAddress(buffer, id) : 0;
        };
        GpuModel model{
            .vertices = slice(AssetBuffer::Vertices),
            .meshlets = slice(AssetBuffer::Meshlets),
            .meshletVertices = slice(AssetBuffer::MeshletVertices),
            .meshletTriangles = slice(AssetBuffer::MeshletTriangles),
            .materials = slice(AssetBuffer::Materials),
            .pbrExtensions = slice(AssetBuffer::PbrExt),
            .normals = slice(AssetBuffer::Normals),
            .tangents = slice(AssetBuffer::Tangents),
            .uv1 = slice(AssetBuffer::Uv1),
            .joints = slice(AssetBuffer::Joints),
            .weights = slice(AssetBuffer::Weights),
            .indices = slice(AssetBuffer::Indices),
        };
        const std::array<std::pair<vk::DeviceAddress, uint32_t>, 6> streams{{
            {model.normals, GpuModelAttribute::Normals},
            {model.tangents, GpuModelAttribute::Tangents},
            {model.uv1, GpuModelAttribute::Uv1},
            {model.joints, GpuModelAttribute::Joints},
            {model.weights, GpuModelAttribute::Weights},
            {model.indices, GpuModelAttribute::Indices},
        }};
        for (const auto& [address, bit] : streams) {
            if (address != 0) {
                model.attributeMask |= bit;
            }
        }
        rows[id] = model;
    }
    logInfo(std::format("model table: {} rows bda=0x{:x}", modelCount, static_cast<uint64_t>(modelTable.address)),
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
        .lightCount = lightStore.uploadedCount,
        .instanceCount = objectStorage.size(),
        .primitiveCount = uploadedCount(AssetBuffer::Primitives),
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

vk::DeviceAddress ResourceManager::modelAssetAddress(AssetBuffer id, EntityId entityId) const noexcept
{
    const vk::DeviceSize stride = kAssetBufferInfo[std::to_underlying(id)].stride;
    return asset(id).address + modelAllocations[entityId].gpu[std::to_underlying(id)].first * stride;
}

vk::DeviceSize ResourceManager::modelGpuBytes(EntityId entityId) const noexcept
{
    vk::DeviceSize bytes = 0;
    for (size_t i = 0; i < kAssetBufferCount; ++i) {
        bytes += modelAllocations[entityId].gpu[i].count * kAssetBufferInfo[i].stride;
    }
    return bytes;
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
    ZoneScopedN("ResourceManager::~ResourceManager");
    logInfo("Destructor called", "ResourceManager");
    destroyInstanceUboBuffers();
    for (HostBuffer& root : frameRoots) {
        destroyHostBuffer(root, kFrameRootTracyName);
    }
    destroyHostBuffer(modelTable, kModelTableTracyName);
    for (size_t i = 0; i < kAssetBufferCount; ++i) {
        destroyAssetBuffer(static_cast<AssetBuffer>(i));
    }
    destroyAttachment(colorAttachment, kColorTracyName);
    destroyAttachment(depthAttachment, kDepthTracyName);
}

void ResourceManager::init()
{
    ZoneScopedN("ResourceManager::init");
    logInfo("init() started", "ResourceManager");
    createCommandPool();
    createCommandBuffers();
    createUniformBuffers();
    flushGpuAssets();
}

void ResourceManager::createSyncObjects()
{
    ZoneScopedN("ResourceManager::createSyncObjects");
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

    const glm::mat4 meshPreRotation =
        glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    // write-combined memory: whole rows, never read back
    auto* instances = static_cast<GpuObjectUB*>(instanceUbos[currentImage].mapped);
    for (EntityId id = 0; id < objectStorage.size(); ++id) {
        if ((objectStorage.flags[id] & EntityFlag::Active) == 0) {
            continue;
        }
        const glm::mat4 model = computeModelMatrix(objectStorage.transforms[id]) * meshPreRotation;
        instances[id] = GpuObjectUB{
            .modelMatrix = model,
            .prevModelMatrix = objectStorage.prevModelMatrices[id],
            .boundingSphere = glm::vec4{0.0f},
            // one model per entity
            .modelIndex = id,
            .instanceFlags = objectStorage.flags[id],
            .pad0 = 0,
            .pad1 = 0,
        };
        objectStorage.prevModelMatrices[id] = model;
    }
}

void ResourceManager::createCommandPool()
{
    ZoneScopedN("ResourceManager::createCommandPool");
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
    ZoneScopedN("ResourceManager::createCommandBuffers");
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

void ResourceManager::appendDeviceLocal(AssetBuffer id, std::span<const std::byte> src)
{
    DeviceBuffer& dst = assetBuffers[std::to_underlying(id)];
    const AssetBufferInfo& info = kAssetBufferInfo[std::to_underlying(id)];
    const std::string_view debugName = info.debugName;
    if (src.empty()) {
        logInfo(std::format("skip {}: empty scratch (used {} / cap {})", debugName, formatBytes(dst.usedBytes),
                             formatBytes(dst.capacityBytes)),
                 "ResourceManager");
        return;
    }
    const vk::DeviceSize srcBytes = src.size_bytes();

    ZoneScopedN("ResourceManager::appendDeviceLocal");
    ZoneText(debugName.data(), debugName.size());
    ZoneValue(static_cast<uint64_t>(srcBytes));

    vk::raii::Buffer staging({});
    VmaAllocation stagingMemory = nullptr;
    createBuffer(srcBytes,
                 vk::BufferUsageFlagBits2::eTransferSrc | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, staging,
                 stagingMemory, allocator.allocator, device, queueFamilyIndices,
                 std::format("{}Staging", debugName));
    vmaCopyMemoryToAllocation(allocator.allocator, src.data(), stagingMemory, 0, srcBytes);

    const vk::DeviceSize oldUsed = dst.usedBytes;
    const vk::DeviceSize newUsed = oldUsed + srcBytes;
    vk::raii::CommandBuffer& cmd = oneTimeTransferCmd();

    if (dst.memory != nullptr && newUsed <= dst.capacityBytes) {
        logInfo(std::format("cache-hit {} : used {} + {} -> {} (cap {})", debugName, formatBytes(oldUsed),
                             formatBytes(srcBytes), formatBytes(newUsed), formatBytes(dst.capacityBytes)),
                 "ResourceManager");
        cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        cmd.copyBuffer(staging, dst.buffer, vk::BufferCopy(0, oldUsed, srcBytes));
        submitAndWait(cmd, oneTimeTransferQueue());
        dst.usedBytes = newUsed;
    } else {
        const vk::DeviceSize newCapacity = nextCapacity(dst.capacityBytes, newUsed);
        logInfo(std::format("{} {}: used {} + {} -> {} (cap {} -> {})", oldUsed == 0 ? "create" : "grow", debugName,
                             formatBytes(oldUsed), formatBytes(srcBytes), formatBytes(newUsed),
                             formatBytes(dst.capacityBytes), formatBytes(newCapacity)),
                 "ResourceManager");

        vk::raii::Buffer grown({});
        VmaAllocation grownMemory = nullptr;
        createAssetBuffer(id, newCapacity, grown, grownMemory);

        cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        if (oldUsed > 0 && dst.memory != nullptr) {
            cmd.copyBuffer(dst.buffer, grown, vk::BufferCopy(0, 0, oldUsed));
        }
        cmd.copyBuffer(staging, grown, vk::BufferCopy(0, oldUsed, srcBytes));
        submitAndWait(cmd, oneTimeTransferQueue());

        installAssetBuffer(id, std::move(grown), grownMemory, newUsed, newCapacity);
    }

    destroyVmaBuffer(allocator.allocator, staging, stagingMemory);

    logInfo(std::format("{} ready used {} / cap {} bda=0x{:x}", debugName, formatBytes(dst.usedBytes),
                         formatBytes(dst.capacityBytes), static_cast<uint64_t>(dst.address)),
             "ResourceManager");
#ifdef TRACY_ENABLE
    const std::string tracyMsg =
        std::format("{} used {} / cap {} bda=0x{:x}", debugName, formatBytes(dst.usedBytes),
                    formatBytes(dst.capacityBytes), static_cast<uint64_t>(dst.address));
    TracyMessage(tracyMsg.c_str(), tracyMsg.size());
#endif
}

std::span<const std::byte> ResourceManager::scratchBytes(AssetBuffer id, std::span<const GpuLight> lights,
                                                         std::span<const GpuPrimitive> primitives) const
{
    switch (id) {
    case AssetBuffer::Vertices:         return std::as_bytes(std::span(geometryStore.vertices));
    case AssetBuffer::Meshlets:         return std::as_bytes(std::span(geometryStore.meshlets));
    case AssetBuffer::MeshletVertices:  return std::as_bytes(std::span(geometryStore.meshletVertices));
    case AssetBuffer::MeshletTriangles: return std::as_bytes(std::span(geometryStore.meshletTriangles));
    case AssetBuffer::Normals:          return std::as_bytes(std::span(geometryStore.normals));
    case AssetBuffer::Tangents:         return std::as_bytes(std::span(geometryStore.tangents));
    case AssetBuffer::Uv1:              return std::as_bytes(std::span(geometryStore.uv1));
    case AssetBuffer::Joints:           return std::as_bytes(std::span(geometryStore.joints0));
    case AssetBuffer::Weights:          return std::as_bytes(std::span(geometryStore.weights0));
    case AssetBuffer::Indices:          return std::as_bytes(std::span(geometryStore.indices));
    case AssetBuffer::MorphPos:         return std::as_bytes(std::span(geometryStore.morphPos));
    case AssetBuffer::MorphNrm:         return std::as_bytes(std::span(geometryStore.morphNrm));
    case AssetBuffer::MorphTan:         return std::as_bytes(std::span(geometryStore.morphTan));
    case AssetBuffer::Materials:        return std::as_bytes(std::span(materialStore.pendingMaterials));
    case AssetBuffer::PbrExt:           return std::as_bytes(std::span(materialStore.pendingExtensions));
    case AssetBuffer::Lights:           return std::as_bytes(lights);
    case AssetBuffer::Primitives:       return std::as_bytes(primitives);
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
    // the model's material slice starts after the default row
    const uint32_t materialFirst =
        std::max(uploadedCount(AssetBuffer::Materials), materialStore.defaultMaterialId() + 1);
    std::vector<GpuPrimitive> packed;
    packed.reserve(geometryStore.primitiveDraws.size() - geometryStore.flushedPrimitiveCount);
    for (uint32_t p = geometryStore.flushedPrimitiveCount; p < geometryStore.primitiveDraws.size(); ++p) {
        const PrimitiveDraw& draw = geometryStore.primitiveDraws[p];
        const bool isDefaultMaterial =
            draw.materialId == kNoneIndex || draw.materialId == materialStore.defaultMaterialId();
        packed.push_back(GpuPrimitive{
            .firstMeshlet = draw.meshlets.firstMeshlet,
            .meshletCount = draw.meshlets.meshletCount,
            .materialId = isDefaultMaterial ? kNoneIndex : draw.materialId - materialFirst,
            .firstVertex = draw.firstVertex,
            .vertexCount = draw.vertexCount,
            .firstIndex = draw.firstIndex,
            .indexCount = draw.indexCount,
            .pad0 = 0,
        });
    }
    return packed;
}

std::vector<GpuLight> ResourceManager::packScratchLights() const
{
    ZoneScopedN("ResourceManager::packScratchLights");
    std::vector<GpuLight> packed;
    packed.reserve(lightStore.instances.size());
    logInfo(std::format("pack lights: {} defs {} instances (gpu already {})", lightStore.defs.size(),
                         lightStore.instances.size(), lightStore.uploadedCount),
             "ResourceManager");
    for (const LightInstance& instance : lightStore.instances) {
        GpuLight light{};
        light.worldPos = instance.worldPos;
        light.worldDir = instance.worldDir;
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
    ZoneScopedN("ResourceManager::createCameraBuffers");
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
    ZoneScopedN("ResourceManager::createUniformBuffers");
    logInfo("createUniformBuffers() started", "ResourceManager");
    ensureInstanceCapacity(std::max(objectStorage.size(), 1u));
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        createHostBuffer(frameRoots[i], sizeof(GpuFrameRoot), std::format("FrameRoot_{}", i), kFrameRootTracyName);
    }
    rebuildModelTable();
}

void ResourceManager::flushGpuAssets()
{
    ZoneScopedN("ResourceManager::flushGpuAssets");
    logInfo("flushGpuAssets() started", "ResourceManager");
    logInfo(std::format("cpu scratch: verts={} meshlets={} meshletVerts={} triCorners={} indices={} "
                         "normals={} tangents={} uv1={} joints={} weights={} morphPos/Nrm/Tan={}/{}/{} "
                         "materials={} pbrExt={} lights={}/{} prims={} (flushed {})",
                         geometryStore.vertices.size(), geometryStore.meshlets.size(),
                         geometryStore.meshletVertices.size(), geometryStore.meshletTriangles.size(),
                         geometryStore.indices.size(), geometryStore.normals.size(), geometryStore.tangents.size(),
                         geometryStore.uv1.size(), geometryStore.joints0.size(), geometryStore.weights0.size(),
                         geometryStore.morphPos.size(), geometryStore.morphNrm.size(), geometryStore.morphTan.size(),
                         materialStore.pendingMaterials.size(), materialStore.pendingExtensions.size(),
                         lightStore.defs.size(), lightStore.instances.size(), geometryStore.primitiveDraws.size(),
                         geometryStore.flushedPrimitiveCount),
             "ResourceManager");

    // one model per flush keeps every slice contiguous and its GPU data model-relative
    const auto pendingEntities = static_cast<uint32_t>(objectStorage.size() - modelAllocations.size());
    if (pendingEntities > 1) {
        throw std::logic_error(std::format("flushGpuAssets: {} models pending, expected at most 1", pendingEntities));
    }
    if (pendingEntities == 0) {
        discardScratch();
        return;
    }

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
    };

    const std::vector<GpuPrimitive> packedPrimitives = packScratchPrimitives();
    remapScratchOffsets();

    logInfo(std::format("material cache: total={} uploaded={} pending={} hits={} misses={}", materialStore.size(),
                         materialStore.uploadedCount, materialStore.pendingCount(), materialStore.cacheHits,
                         materialStore.cacheMisses),
             "ResourceManager");

    const std::vector<GpuLight> packedLights = packScratchLights();
    for (size_t i = 0; i < kAssetBufferCount; ++i) {
        const auto id = static_cast<AssetBuffer>(i);
        // the default material row rides along with the first flush but belongs to no model
        const bool isMaterialRow = id == AssetBuffer::Materials || id == AssetBuffer::PbrExt;
        const uint32_t first = std::max(uploadedCount(id), isMaterialRow ? materialStore.defaultMaterialId() + 1 : 0u);
        appendDeviceLocal(id, scratchBytes(id, packedLights, packedPrimitives));
        allocation.gpu[i] = {first, uploadedCount(id) - first};
    }
    modelAllocations.push_back(allocation);

    materialStore.markUploaded();
    lightStore.uploadedCount += static_cast<uint32_t>(packedLights.size());

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
                         materialStore.uploadedCount, lightStore.uploadedCount, geometryStore.primitiveDraws.size(),
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
        uploadedCount(AssetBuffer::Vertices), uploadedCount(AssetBuffer::Meshlets), materialStore.uploadedCount,
        lightStore.uploadedCount);
    TracyMessage(tracyMsg.c_str(), tracyMsg.size());
#endif
    tracyPlotResources();
}

void ResourceManager::discardScratch()
{
    logInfo("flushGpuAssets: no new model, discarding load scratch", "ResourceManager");
    geometryStore.discardScratch();
    lightStore.clearScratch();
    materialStore.discardPending();
#if ENGINE_USE_MIMALLOC
    mi_collect(true);
#endif
}

void ResourceManager::eraseModel(EntityId id)
{
    ZoneScopedN("ResourceManager::eraseModel");
    if (id >= modelAllocations.size()) {
        throw std::out_of_range(std::format("eraseModel: entity {} has no GPU allocation", id));
    }
    const ModelAllocation removed = modelAllocations[id];
    logInfo(std::format("erase model entity {} '{}': {} on GPU, {} primitives", id, objectStorage.names[id],
                         formatBytes(modelGpuBytes(id)), removed.primitives.count),
             "ResourceManager");

    compactAssetBuffers(removed);
    rebaseCatalog(id, removed);

    const ElementRange materials = removed.gpu[std::to_underlying(AssetBuffer::Materials)];
    if (materials.count > 0) {
        materialStore.erase(materials.first, materials.count);
    }
    lightStore.uploadedCount -= removed.gpu[std::to_underlying(AssetBuffer::Lights)].count;
    if (objectStorage.empty()) {
        geometryStore.extensionsUsed.clear();
        geometryStore.extensionsRequired.clear();
    }
    rebuildModelTable();

    const auto [gpuAssetBytes, gpuAssetCapacity] = assetTotals();
    logInfo(std::format("gpu ssbo total after erase: used {} / cap {}", formatBytes(gpuAssetBytes),
                         formatBytes(gpuAssetCapacity)),
             "ResourceManager");
    tracyPlotResources();
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

void ResourceManager::rebaseCatalog(EntityId id, const ModelAllocation& removed)
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
    geometry.flushedPrimitiveCount -= removed.primitives.count;
    geometry.flushedMorphTargetCount -= removed.morphTargets.count;
    geometry.flushedMorphWeightCount -= removed.morphWeights.count;
    geometry.flushedAuxBlobCount -= removed.auxBlobs.count;

    for (PrimitiveDraw& draw : geometry.primitiveDraws) {
        shift(draw.meshlets.firstMeshlet, gpuRange(AssetBuffer::Meshlets));
        shift(draw.firstVertex, gpuRange(AssetBuffer::Vertices));
        shift(draw.firstIndex, gpuRange(AssetBuffer::Indices));
        shift(draw.materialId, gpuRange(AssetBuffer::Materials));
        shift(draw.morphFirst, removed.morphTargets);
        shift(draw.morphWeightFirst, removed.morphWeights);
    }
    for (MorphTarget& target : geometry.morphTargets) {
        shift(target.posOffset, gpuRange(AssetBuffer::MorphPos));
        shift(target.nrmOffset, gpuRange(AssetBuffer::MorphNrm));
        shift(target.tanOffset, gpuRange(AssetBuffer::MorphTan));
    }

    objectStorage.erase(id);
    modelAllocations.erase(modelAllocations.begin() + id);
    for (EntityId other = 0; other < objectStorage.size(); ++other) {
        shift(objectStorage.firstPrimitives[other], removed.primitives);
        shift(objectStorage.meshletDraws[other].firstMeshlet, gpuRange(AssetBuffer::Meshlets));
    }
    for (ModelAllocation& allocation : modelAllocations) {
        for (size_t i = 0; i < kAssetBufferCount; ++i) {
            shift(allocation.gpu[i].first, removed.gpu[i]);
        }
        shift(allocation.primitives.first, removed.primitives);
        shift(allocation.morphTargets.first, removed.morphTargets);
        shift(allocation.morphWeights.first, removed.morphWeights);
        shift(allocation.auxBlobs.first, removed.auxBlobs);
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
    TracyPlot("Vulkan/MaterialCount", static_cast<double>(materialStore.uploadedCount));
    TracyPlot("Vulkan/MaterialCacheSize", static_cast<double>(materialStore.size()));
    TracyPlot("Vulkan/MaterialCacheHits", static_cast<double>(materialStore.cacheHits));
    TracyPlot("Vulkan/MaterialCacheMisses", static_cast<double>(materialStore.cacheMisses));
    TracyPlot("Vulkan/LightCount", static_cast<double>(lightStore.uploadedCount));
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
    TracyPlot("Vulkan/InstanceUboBytes", static_cast<double>(sizeof(GpuObjectUB) * instanceCapacity));
    TracyPlot("Vulkan/CommandBuffersInUse", static_cast<double>(commandBuffers.size()));
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
    ZoneScopedN("ResourceManager::createColorResources");
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
    ZoneScopedN("ResourceManager::createDepthResources");
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
