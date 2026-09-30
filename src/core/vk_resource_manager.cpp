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
    }};

    constexpr const char* kColorTracyName = "GPU/ColorMSAA";
    constexpr const char* kDepthTracyName = "GPU/Depth";
    constexpr const char* kInstanceUboTracyName = "GPU/InstanceUBO";

    [[nodiscard]] std::string formatBytes(vk::DeviceSize bytes)
    {
        if (bytes < 1024) {
            return std::format("{} B", bytes);
        }
        if (bytes < 1024ull * 1024ull) {
            return std::format("{:.1f} KiB", static_cast<double>(bytes) / 1024.0);
        }
        return std::format("{:.2f} MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
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
    log_info("Initialized", "ResourceManager");
}

void ResourceManager::destroyInstanceUboBuffers()
{
    ZoneScopedN("ResourceManager::destroyInstanceUboBuffers");
    for (HostBuffer& ubo : instanceUbos)
    {
        // persistently mapped (MAPPED_BIT): VMA unmaps on destroy
        destroyVmaBuffer(allocator.allocator, ubo.buffer, ubo.memory, kInstanceUboTracyName);
        ubo.mapped = nullptr;
        ubo.address = 0;
    }
    instanceCapacity = 0;
}

void ResourceManager::destroyAssetBuffer(AssetBuffer id)
{
    DeviceBuffer& target = assetBuffers[std::to_underlying(id)];
    if (target.memory == nullptr) {
        return;
    }
    const AssetBufferInfo& info = kAssetBufferInfo[std::to_underlying(id)];
    log_info(std::format("destroy {} (used {} / cap {} bda=0x{:x})", info.tracyName, formatBytes(target.usedBytes),
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
    log_info("Destructor called", "ResourceManager");
    destroyInstanceUboBuffers();
    for (size_t i = 0; i < kAssetBufferCount; ++i) {
        destroyAssetBuffer(static_cast<AssetBuffer>(i));
    }
    destroyAttachment(colorAttachment, kColorTracyName);
    destroyAttachment(depthAttachment, kDepthTracyName);
}

void ResourceManager::init()
{
    ZoneScopedN("ResourceManager::init");
    log_info("init() started", "ResourceManager");
    createCommandPool();
    createCommandBuffers();
    createUniformBuffers();
    flushGpuAssets();
}

void ResourceManager::createSyncObjects()
{
    ZoneScopedN("ResourceManager::createSyncObjects");
    log_info("createSyncObjects() started", "ResourceManager");
    presentCompleteSemaphore.clear();
    renderFinishedSemaphore.clear();
    inFlightFences.clear();

    // Acquire semaphore: one per frame-in-flight (indexed by currentFrame).
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        presentCompleteSemaphore.emplace_back(device, vk::SemaphoreCreateInfo());
        inFlightFences.emplace_back(device, vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled});
    }
    // Present-complete signal: one per swapchain image (indexed by imageIndex).
    for (size_t i = 0; i < swapChainImageCount; i++) {
        renderFinishedSemaphore.emplace_back(device, vk::SemaphoreCreateInfo());
    }
}

void ResourceManager::updateUniformBuffers(uint32_t currentImage)
{
    ZoneScopedN("ResourceManager::updateUniformBuffer");
    if (objectStorage.empty())
    {
        return;
    }

    ensureInstanceCapacity(objectStorage.size());

    const glm::mat4 meshPreRotation =
        glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));

    auto* mapped = static_cast<GpuObjectUB*>(instanceUbos[currentImage].mapped);
    writeObjectUbs(objectStorage, std::span(mapped, objectStorage.size()), meshPreRotation);
}

vk::DeviceAddress ResourceManager::instanceUboAddress(uint32_t frameSlot, EntityId entityId) const noexcept
{
    return instanceUbos[frameSlot].address + static_cast<vk::DeviceAddress>(entityId) * sizeof(GpuObjectUB);
}

void ResourceManager::createCommandPool()
{
    ZoneScopedN("ResourceManager::createCommandPool");
    log_info("createCommandPool() started", "ResourceManager");
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
    log_info("createCommandBuffers() started", "ResourceManager");
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
    log_info(std::format("Command buffers allocated: {}", commandBuffers.size()), "ResourceManager");
    log_info(std::format("Transfer command buffers allocated: {}", transferCommandBuffer.size()), "ResourceManager");
}

void ResourceManager::appendDeviceLocal(AssetBuffer id, std::span<const std::byte> src)
{
    DeviceBuffer& dst = assetBuffers[std::to_underlying(id)];
    const AssetBufferInfo& info = kAssetBufferInfo[std::to_underlying(id)];
    const std::string_view debugName = info.debugName;
    if (src.empty()) {
        log_info(std::format("skip {}: empty scratch (used {} / cap {})", debugName, formatBytes(dst.usedBytes),
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
                 std::format("{}StagingMemory", debugName));
    vmaCopyMemoryToAllocation(allocator.allocator, src.data(), stagingMemory, 0, srcBytes);

    const vk::DeviceSize oldUsed = dst.usedBytes;
    const vk::DeviceSize newUsed = oldUsed + srcBytes;
    vk::raii::CommandBuffer& cmd = oneTimeTransferCmd();

    if (dst.memory != nullptr && newUsed <= dst.capacityBytes) {
        log_info(std::format("cache-hit {} : used {} + {} -> {} (cap {})", debugName, formatBytes(oldUsed),
                             formatBytes(srcBytes), formatBytes(newUsed), formatBytes(dst.capacityBytes)),
                 "ResourceManager");
        cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        cmd.copyBuffer(staging, dst.buffer, vk::BufferCopy(0, oldUsed, srcBytes));
        submitAndWait(cmd, oneTimeTransferQueue());
        dst.usedBytes = newUsed;
    } else {
        const vk::DeviceSize newCapacity = nextCapacity(dst.capacityBytes, newUsed);
        log_info(std::format("{} {}: used {} + {} -> {} (cap {} -> {})", oldUsed == 0 ? "create" : "grow", debugName,
                             formatBytes(oldUsed), formatBytes(srcBytes), formatBytes(newUsed),
                             formatBytes(dst.capacityBytes), formatBytes(newCapacity)),
                 "ResourceManager");

        vk::raii::Buffer grown({});
        VmaAllocation grownMemory = nullptr;
        createBuffer(newCapacity,
                     vk::BufferUsageFlagBits2::eTransferSrc | vk::BufferUsageFlagBits2::eTransferDst |
                         vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                     vk::MemoryPropertyFlagBits::eDeviceLocal, grown, grownMemory, allocator.allocator, device,
                     queueFamilyIndices, std::format("{}Memory", debugName));

        cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        if (oldUsed > 0 && dst.memory != nullptr) {
            cmd.copyBuffer(dst.buffer, grown, vk::BufferCopy(0, 0, oldUsed));
        }
        cmd.copyBuffer(staging, grown, vk::BufferCopy(0, oldUsed, srcBytes));
        submitAndWait(cmd, oneTimeTransferQueue());

        destroyAssetBuffer(id);

        dst.buffer = std::move(grown);
        dst.memory = grownMemory;
        dst.usedBytes = newUsed;
        dst.capacityBytes = newCapacity;
        dst.address = device.getBufferAddress({.buffer = *dst.buffer});
        setDebugName(device, dst.buffer, debugName);
        tracyResourceAlloc(static_cast<VkBuffer>(*dst.buffer), static_cast<size_t>(newCapacity), info.tracyName);
    }

    destroyVmaBuffer(allocator.allocator, staging, stagingMemory);

    log_info(std::format("{} ready used {} / cap {} bda=0x{:x}", debugName, formatBytes(dst.usedBytes),
                         formatBytes(dst.capacityBytes), static_cast<uint64_t>(dst.address)),
             "ResourceManager");
#ifdef TRACY_ENABLE
    const std::string tracyMsg =
        std::format("{} used {} / cap {} bda=0x{:x}", debugName, formatBytes(dst.usedBytes),
                    formatBytes(dst.capacityBytes), static_cast<uint64_t>(dst.address));
    TracyMessage(tracyMsg.c_str(), tracyMsg.size());
#endif
}

std::span<const std::byte> ResourceManager::scratchBytes(AssetBuffer id, std::span<const GpuLight> lights) const
{
    // material rows are a persistent cache; only the pending tail is new
    const uint32_t materialBase = materialStore.uploadedCount;
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
    case AssetBuffer::Materials:
        return std::as_bytes(std::span(materialStore.gpuMaterials).subspan(materialBase));
    case AssetBuffer::PbrExt:
        return std::as_bytes(std::span(materialStore.pbrExtensions).subspan(materialBase));
    case AssetBuffer::Lights:           return std::as_bytes(lights);
    case AssetBuffer::Count:            break;
    }
    return {};
}

void ResourceManager::remapScratchOffsets()
{
    ZoneScopedN("ResourceManager::remapScratchOffsets");
    GeometryStore& geometry = geometryStore;
    const uint32_t vertexBase = uploadedCount(AssetBuffer::Vertices);
    const uint32_t meshletBase = uploadedCount(AssetBuffer::Meshlets);
    const uint32_t meshletVertexBase = uploadedCount(AssetBuffer::MeshletVertices);
    const uint32_t meshletTriangleBase = uploadedCount(AssetBuffer::MeshletTriangles);
    const uint32_t indexBase = uploadedCount(AssetBuffer::Indices);
    const uint32_t newPrimitives =
        static_cast<uint32_t>(geometry.primitiveDraws.size()) - geometry.flushedPrimitiveCount;
    const uint32_t newMorphTargets =
        static_cast<uint32_t>(geometry.morphTargets.size()) - geometry.flushedMorphTargetCount;
    log_info(std::format("remap scratch: +{} prims +{} morphTargets onto gpu verts={} meshlets={} indices={}",
                         newPrimitives, newMorphTargets, vertexBase, meshletBase, indexBase),
             "ResourceManager");
    for (uint32_t& vertexIndex : geometry.meshletVertices) {
        vertexIndex += vertexBase;
    }
    for (uint32_t& index : geometry.indices) {
        index += vertexBase;
    }
    for (GpuMeshletDesc& meshlet : geometry.meshlets) {
        meshlet.vertexOffset += meshletVertexBase;
        meshlet.triangleOffset += meshletTriangleBase;
    }
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

std::vector<GpuLight> ResourceManager::packScratchLights() const
{
    ZoneScopedN("ResourceManager::packScratchLights");
    std::vector<GpuLight> packed;
    packed.reserve(lightStore.instances.size());
    log_info(std::format("pack lights: {} defs {} instances (gpu already {})", lightStore.defs.size(),
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
    log_info("createCameraBuffers() started", "ResourceManager");
    camera.allocator = allocator.allocator;
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vk::DeviceSize bufferSize = sizeof(GpuCameraData);
        vk::raii::Buffer buffer({});
        VmaAllocation bufferMem = nullptr;
        createBuffer(bufferSize,
                     vk::BufferUsageFlagBits2::eUniformBuffer | vk::BufferUsageFlagBits2::eStorageBuffer |
                         vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                     vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, buffer,
                     bufferMem, allocator.allocator, device, queueFamilyIndices,
                     std::format("CameraUniformBufferMemory_{}", i));
        camera.cameraBuffers[i] = std::move(buffer);
        camera.cameraBuffersMemory[i] = bufferMem;
        void* data = nullptr;
        vmaMapMemory(allocator.allocator, bufferMem, &data);
        camera.cameraBuffersMapped[i] = data;
        camera.cameraBufferAddresses[i] = device.getBufferAddress({.buffer = *camera.cameraBuffers[i]});
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
    // Grow with headroom so interactive loads do not reallocate every time.
    const uint32_t newCapacity = std::max(entityCount, instanceCapacity * 2);
    log_info(std::format("Growing instance GpuObjectUB capacity {} -> {}", instanceCapacity, newCapacity),
             "ResourceManager");

    destroyInstanceUboBuffers();
    instanceCapacity = newCapacity;

    const vk::DeviceSize bufferSize = sizeof(GpuObjectUB) * static_cast<vk::DeviceSize>(instanceCapacity);
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
    {
        HostBuffer& ubo = instanceUbos[i];
        createBuffer(bufferSize,
                     vk::BufferUsageFlagBits2::eUniformBuffer | vk::BufferUsageFlagBits2::eStorageBuffer |
                         vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                     vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, ubo.buffer,
                     ubo.memory, allocator.allocator, device, queueFamilyIndices,
                     std::format("InstanceObjectUBMemory_{}", i), VMA_ALLOCATION_CREATE_MAPPED_BIT);
        VmaAllocationInfo allocationInfo{};
        vmaGetAllocationInfo(allocator.allocator, ubo.memory, &allocationInfo);
        ubo.mapped = allocationInfo.pMappedData;
        ubo.address = device.getBufferAddress({.buffer = *ubo.buffer});
        setDebugName(device, ubo.buffer, std::format("InstanceObjectUB_{}", i));
        tracyResourceAlloc(static_cast<VkBuffer>(*ubo.buffer), static_cast<size_t>(bufferSize),
                           kInstanceUboTracyName);
    }
}

void ResourceManager::createUniformBuffers()
{
    ZoneScopedN("ResourceManager::createUniformBuffers");
    log_info("createUniformBuffers() started", "ResourceManager");
    ensureInstanceCapacity(std::max(objectStorage.size(), 1u));
}

void ResourceManager::flushGpuAssets()
{
    ZoneScopedN("ResourceManager::flushGpuAssets");
    log_info("flushGpuAssets() started", "ResourceManager");
    log_info(std::format("cpu scratch: verts={} meshlets={} meshletVerts={} triCorners={} indices={} "
                         "normals={} tangents={} uv1={} joints={} weights={} morphPos/Nrm/Tan={}/{}/{} "
                         "materials={} pbrExt={} lights={}/{} prims={} (flushed {})",
                         geometryStore.vertices.size(), geometryStore.meshlets.size(),
                         geometryStore.meshletVertices.size(), geometryStore.meshletTriangles.size(),
                         geometryStore.indices.size(), geometryStore.normals.size(), geometryStore.tangents.size(),
                         geometryStore.uv1.size(), geometryStore.joints0.size(), geometryStore.weights0.size(),
                         geometryStore.morphPos.size(), geometryStore.morphNrm.size(), geometryStore.morphTan.size(),
                         materialStore.gpuMaterials.size(), materialStore.pbrExtensions.size(),
                         lightStore.defs.size(), lightStore.instances.size(), geometryStore.primitiveDraws.size(),
                         geometryStore.flushedPrimitiveCount),
             "ResourceManager");

    remapScratchOffsets();

    log_info(std::format("material cache: total={} uploaded={} pending={} hits={} misses={}", materialStore.size(),
                         materialStore.uploadedCount, materialStore.pendingCount(), materialStore.cacheHits,
                         materialStore.cacheMisses),
             "ResourceManager");

    const std::vector<GpuLight> packedLights = packScratchLights();
    for (size_t i = 0; i < kAssetBufferCount; ++i) {
        const auto id = static_cast<AssetBuffer>(i);
        appendDeviceLocal(id, scratchBytes(id, packedLights));
    }

    materialStore.markUploaded();
    lightStore.uploadedCount += static_cast<uint32_t>(packedLights.size());

    geometryStore.clearScratch();
    lightStore.clearScratch();

    log_info(std::format("gpu catalog: verts={} meshlets={} meshletVerts={} triCorners={} indices={} "
                         "morphPos/Nrm/Tan={}/{}/{} materials={} lights={} prims={} morphTargets={}",
                         uploadedCount(AssetBuffer::Vertices), uploadedCount(AssetBuffer::Meshlets),
                         uploadedCount(AssetBuffer::MeshletVertices), uploadedCount(AssetBuffer::MeshletTriangles),
                         uploadedCount(AssetBuffer::Indices), uploadedCount(AssetBuffer::MorphPos),
                         uploadedCount(AssetBuffer::MorphNrm), uploadedCount(AssetBuffer::MorphTan),
                         materialStore.uploadedCount, lightStore.uploadedCount, geometryStore.primitiveDraws.size(),
                         geometryStore.morphTargets.size()),
             "ResourceManager");
    const auto [gpuAssetBytes, gpuAssetCapacity] = assetTotals();
    log_info(std::format("gpu ssbo total: used {} / cap {}", formatBytes(gpuAssetBytes),
                         formatBytes(gpuAssetCapacity)),
             "ResourceManager");
    log_info(std::format("cpu after flush: scratch verts={} meshlets={} | material cache={} lights scratch={}",
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
    // approximate footprint: 4 B/pixel * samples
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
    log_info("createColorResources() started", "ResourceManager");
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
    log_info("findDepthFormat() started", "ResourceManager");
    // Prefer D24/D16 over D32 (BestPractices-NVIDIA-CreateImage-Depth32Format).
    // Fall back to D32 only if the preferred formats are unsupported.
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
    log_info("createDepthResources() started", "ResourceManager");
    const vk::Format depthFormat = findDepthFormat();
    log_info(std::format("Depth format selected: {}", vk::to_string(depthFormat)), "ResourceManager");

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
