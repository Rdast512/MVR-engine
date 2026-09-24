#include "vk_resource_manager.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <span>
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>
#include "Constants.h"
#include "static_headers/logger.hpp"
#include "util/debug.hpp"
#include "util/vk_tracy.hpp"
#include "util/vk_utils.hpp"

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
      hardwareCapabilities(deviceWrapper.capabilities),
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
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
    {
        if (instanceUboMapped[i] != nullptr && instanceUboMemory[i] != nullptr)
        {
            vmaUnmapMemory(allocator.allocator, instanceUboMemory[i]);
            instanceUboMapped[i] = nullptr;
        }
        if (instanceUboMemory[i] != nullptr)
        {
            VkBuffer raw = instanceUboBuffers[i].release();
            tracyResourceFree(raw, "GPU/InstanceUBO");
            vmaDestroyBuffer(allocator.allocator, raw, instanceUboMemory[i]);
            instanceUboMemory[i] = nullptr;
            trackedInstanceUboBytes[i] = 0;
        }
        instanceUboBaseAddresses[i] = 0;
    }
    instanceCapacity = 0;
}

namespace
{
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

void ResourceManager::destroyDeviceBuffer(vk::raii::Buffer& buffer, VmaAllocation& memory, vk::DeviceAddress& address,
                                          vk::DeviceSize& usedBytes, vk::DeviceSize& capacityBytes,
                                          const char* tracyName)
{
    if (memory == nullptr) {
        return;
    }
    log_info(std::format("destroy {} (used {} / cap {} bda=0x{:x})", tracyName, formatBytes(usedBytes),
                         formatBytes(capacityBytes), static_cast<uint64_t>(address)),
             "ResourceManager");
    VkBuffer raw = buffer.release();
    tracyResourceFree(raw, tracyName);
    vmaDestroyBuffer(allocator.allocator, raw, memory);
    memory = nullptr;
    address = 0;
    usedBytes = 0;
    capacityBytes = 0;
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
    {
        destroyDeviceBuffer(vertexBuffer, vertexBufferMemory, vertexBufferAddress, trackedVertexBytes,
                            capacityVertexBytes, "GPU/Vertices");
        destroyDeviceBuffer(meshletBuffer, meshletBufferMemory, meshletBufferAddress, trackedMeshletBytes,
                            capacityMeshletBytes, "GPU/Meshlets");
        destroyDeviceBuffer(meshletVertexBuffer, meshletVertexBufferMemory, meshletVertexBufferAddress,
                            trackedMeshletVertexBytes, capacityMeshletVertexBytes, "GPU/MeshletVertices");
        destroyDeviceBuffer(meshletTriangleBuffer, meshletTriangleBufferMemory, meshletTriangleBufferAddress,
                            trackedMeshletTriangleBytes, capacityMeshletTriangleBytes, "GPU/MeshletTriangles");
        destroyDeviceBuffer(materialBuffer, materialBufferMemory, materialBufferAddress, trackedMaterialBytes,
                            capacityMaterialBytes, "GPU/Materials");
        destroyDeviceBuffer(pbrExtBuffer, pbrExtBufferMemory, pbrExtBufferAddress, trackedPbrExtBytes,
                            capacityPbrExtBytes, "GPU/PbrExtensions");
        destroyDeviceBuffer(lightBuffer, lightBufferMemory, lightBufferAddress, trackedLightBytes, capacityLightBytes,
                            "GPU/Lights");
        destroyDeviceBuffer(normalBuffer, normalBufferMemory, normalBufferAddress, trackedNormalBytes,
                            capacityNormalBytes, "GPU/Normals");
        destroyDeviceBuffer(tangentBuffer, tangentBufferMemory, tangentBufferAddress, trackedTangentBytes,
                            capacityTangentBytes, "GPU/Tangents");
        destroyDeviceBuffer(uv1Buffer, uv1BufferMemory, uv1BufferAddress, trackedUv1Bytes, capacityUv1Bytes, "GPU/Uv1");
        destroyDeviceBuffer(jointBuffer, jointBufferMemory, jointBufferAddress, trackedJointBytes, capacityJointBytes,
                            "GPU/Joints");
        destroyDeviceBuffer(weightBuffer, weightBufferMemory, weightBufferAddress, trackedWeightBytes,
                            capacityWeightBytes, "GPU/Weights");
        destroyDeviceBuffer(indexBuffer, indexBufferMemory, indexBufferAddress, trackedIndexBytes, capacityIndexBytes,
                            "GPU/Indices");
        destroyDeviceBuffer(morphPosBuffer, morphPosBufferMemory, morphPosBufferAddress, trackedMorphPosBytes,
                            capacityMorphPosBytes, "GPU/MorphPos");
        destroyDeviceBuffer(morphNrmBuffer, morphNrmBufferMemory, morphNrmBufferAddress, trackedMorphNrmBytes,
                            capacityMorphNrmBytes, "GPU/MorphNrm");
        destroyDeviceBuffer(morphTanBuffer, morphTanBufferMemory, morphTanBufferAddress, trackedMorphTanBytes,
                            capacityMorphTanBytes, "GPU/MorphTan");
        if (indirectBufferMemory) {
            VkBuffer raw = indirectBuffer.release();
            tracyResourceFree(raw, "GPU/IndirectCopyCommand");
            vmaDestroyBuffer(allocator.allocator, raw, indirectBufferMemory);
            indirectBufferMemory = nullptr;
            indirectBufferAddress = 0;
        }
        if (colorImageMemory) {
            VkImage raw = colorImage.release();
            tracyResourceFree(raw, "GPU/ColorMSAA");
            vmaDestroyImage(allocator.allocator, raw, colorImageMemory);
            trackedColorBytes = 0;
        }
        if (depthImageMemory) {
            VkImage raw = depthImage.release();
            tracyResourceFree(raw, "GPU/Depth");
            vmaDestroyImage(allocator.allocator, raw, depthImageMemory);
            trackedDepthBytes = 0;
        }
    }
}

void ResourceManager::init()
{
    ZoneScopedN("ResourceManager::init");
    log_info("init() started", "ResourceManager");
    createCommandPool();
    createCommandBuffers();
    createUniformBuffers();
    // Host-visible CopyMemoryIndirectCommandKHR buffer; copyBuffer needs it first. NOTE: disabled since not sure if its even better if no streaming is implomented
    // createIndirectBuffer();
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

    auto* mapped = static_cast<GpuObjectUB*>(instanceUboMapped[currentImage]);
    writeObjectUbs(objectStorage, std::span(mapped, objectStorage.size()), meshPreRotation);
}

vk::DeviceAddress ResourceManager::instanceUboAddress(uint32_t frameSlot, EntityId entityId) const noexcept
{
    return instanceUboBaseAddresses[frameSlot] + static_cast<vk::DeviceAddress>(entityId) * sizeof(GpuObjectUB);
}



void ResourceManager::createCommandPool()
{
    ZoneScopedN("ResourceManager::createCommandPool");
    log_info("createCommandPool() started", "ResourceManager");
    vk::CommandPoolCreateInfo poolInfo{.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                       .queueFamilyIndex = graphicsIndex};
    commandPool = vk::raii::CommandPool(device, poolInfo);
    setDebugName(device, commandPool, "GraphicsCommandPool");
    vk::CommandPoolCreateInfo transferPoolInfo{.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                               .queueFamilyIndex = transferIndex};
    if (transferIndex != graphicsIndex && transferIndex != UINT32_MAX) {
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
    if (transferIndex != UINT32_MAX && transferIndex != graphicsIndex) {
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

void ResourceManager::copyBuffer(vk::raii::Buffer& srcBuffer, vk::raii::Buffer& dstBuffer, vk::DeviceSize size)
{
    ZoneScopedN("ResourceManager::copyBuffer");
    // NOTE: potentionnaly not needed or even worse on perf since if using a transfer queue these copies are already fast and cpu overhead is low
    // const auto srcAddress =
    //     device.getBufferAddress({
    //         .buffer = *srcBuffer
    //     });
    //
    // const auto dstAddress =
    //     device.getBufferAddress({
    //         .buffer = *dstBuffer
    //     });
    //
    // // This structure must live in GPU-visible memory.
    // const vk::CopyMemoryIndirectCommandKHR indirectCommand{
    //     .srcAddress = srcAddress,
    //     .dstAddress = dstAddress,
    //     .size       = size
    // };
    //
    // void* dataStaging = nullptr;
    // vmaMapMemory(allocator.allocator, indirectBufferMemory, &dataStaging);
    // memcpy(dataStaging, &indirectCommand, sizeof(vk::CopyMemoryIndirectCommandKHR));
    // vmaUnmapMemory(allocator.allocator, indirectBufferMemory);
    //
    // // Submit on the family that allocated the command buffer (VUID-vkQueueSubmit2-commandBuffer-03878).
    // vk::raii::CommandBuffer const& cmd = commandBuffers[0];
    // const vk::raii::Queue& queue = graphicsQueue;
    //
    // cmd.begin({
    //     .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit
    // });
    //
    // cmd.copyMemoryIndirectKHR({
    //     .srcCopyFlags = {},
    //     .dstCopyFlags = {},
    //     .copyCount = 1,
    //     .copyAddressRange = {
    //         .address = indirectBufferAddress,
    //         .size = sizeof(vk::CopyMemoryIndirectCommandKHR),
    //         .stride = sizeof(vk::CopyMemoryIndirectCommandKHR)
    //     }
    // });
    //
    // cmd.end();
    //
    // vk::CommandBufferSubmitInfo const commandBufferInfo{
    //     .commandBuffer = *cmd,
    // };
    //
    // const vk::SubmitInfo2 submitInfo{
    //     .commandBufferInfoCount = 1,
    //     .pCommandBufferInfos = &commandBufferInfo,
    // };
    // queue.submit2(submitInfo, nullptr);
    // queue.waitIdle();

    log_info("copyBuffer() started", "ResourceManager");
    vk::raii::CommandBuffer& cmd = oneTimeTransferCmd();
    cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    cmd.copyBuffer(srcBuffer, dstBuffer, vk::BufferCopy(0, 0, size));
    cmd.end();
    vk::CommandBufferSubmitInfo commandBufferInfo = {.commandBuffer = *cmd};
    const vk::SubmitInfo2 submitInfo{.commandBufferInfoCount = 1, .pCommandBufferInfos = &commandBufferInfo};
    oneTimeTransferQueue().submit2(submitInfo, nullptr);
    oneTimeTransferQueue().waitIdle();
}


void ResourceManager::endCommandBuffer(vk::raii::CommandBuffer& commandBuffer, const vk::raii::Queue& queue)
{
    ZoneScopedN("ResourceManager::endCommandBuffer");
    log_info("endCommandBuffer() started", "ResourceManager");
    commandBuffer.end();
    // Prefer synchronization2 submit (avoids WARNING-deprecation-sync2 / legacy QueueSubmit).
    const vk::CommandBufferSubmitInfo commandBufferInfo{.commandBuffer = *commandBuffer};
    const vk::SubmitInfo2 submitInfo{.commandBufferInfoCount = 1, .pCommandBufferInfos = &commandBufferInfo};
    queue.submit2(submitInfo, nullptr);
    queue.waitIdle();
}

void ResourceManager::appendDeviceLocal(vk::raii::Buffer& dst, VmaAllocation& dstMemory, vk::DeviceAddress& dstAddress,
                                        vk::DeviceSize& usedBytes, vk::DeviceSize& capacityBytes, const void* src,
                                        vk::DeviceSize srcBytes, std::string_view debugName, const char* tracyName)
{
    if (src == nullptr || srcBytes == 0) {
        log_info(std::format("skip {}: empty scratch (used {} / cap {})", debugName, formatBytes(usedBytes),
                             formatBytes(capacityBytes)),
                 "ResourceManager");
        return;
    }

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

    void* mapped = nullptr;
    vmaMapMemory(allocator.allocator, stagingMemory, &mapped);
    std::memcpy(mapped, src, static_cast<size_t>(srcBytes));
    vmaUnmapMemory(allocator.allocator, stagingMemory);

    const vk::DeviceSize oldUsed = usedBytes;
    const vk::DeviceSize newUsed = oldUsed + srcBytes;
    const bool fits = dstMemory != nullptr && newUsed <= capacityBytes;

    auto submitCopies = [this](vk::raii::CommandBuffer& cmd) {
        cmd.end();
        vk::CommandBufferSubmitInfo commandBufferInfo{.commandBuffer = *cmd};
        const vk::SubmitInfo2 submitInfo{.commandBufferInfoCount = 1, .pCommandBufferInfos = &commandBufferInfo};
        oneTimeTransferQueue().submit2(submitInfo, nullptr);
        oneTimeTransferQueue().waitIdle();
    };

    if (fits) {
        log_info(std::format("cache-hit {} : used {} + {} -> {} (cap {})", debugName, formatBytes(oldUsed),
                             formatBytes(srcBytes), formatBytes(newUsed), formatBytes(capacityBytes)),
                 "ResourceManager");
        vk::raii::CommandBuffer& cmd = oneTimeTransferCmd();
        cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        cmd.copyBuffer(staging, dst, vk::BufferCopy(0, oldUsed, srcBytes));
        submitCopies(cmd);
        usedBytes = newUsed;
    } else {
        const vk::DeviceSize newCapacity = nextCapacity(capacityBytes, newUsed);
        log_info(std::format("{} {}: used {} + {} -> {} (cap {} -> {})", oldUsed == 0 ? "create" : "grow", debugName,
                             formatBytes(oldUsed), formatBytes(srcBytes), formatBytes(newUsed),
                             formatBytes(capacityBytes), formatBytes(newCapacity)),
                 "ResourceManager");

        vk::raii::Buffer grown({});
        VmaAllocation grownMemory = nullptr;
        createBuffer(newCapacity,
                     vk::BufferUsageFlagBits2::eTransferSrc | vk::BufferUsageFlagBits2::eTransferDst |
                         vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                     vk::MemoryPropertyFlagBits::eDeviceLocal, grown, grownMemory, allocator.allocator, device,
                     queueFamilyIndices, std::format("{}Memory", debugName));

        vk::raii::CommandBuffer& cmd = oneTimeTransferCmd();
        cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        if (oldUsed > 0 && dstMemory != nullptr) {
            cmd.copyBuffer(dst, grown, vk::BufferCopy(0, 0, oldUsed));
        }
        cmd.copyBuffer(staging, grown, vk::BufferCopy(0, oldUsed, srcBytes));
        submitCopies(cmd);

        destroyDeviceBuffer(dst, dstMemory, dstAddress, usedBytes, capacityBytes, tracyName);

        dst = std::move(grown);
        dstMemory = grownMemory;
        usedBytes = newUsed;
        capacityBytes = newCapacity;
        dstAddress = device.getBufferAddress({.buffer = *dst});
        setDebugName(device, dst, debugName);
        tracyResourceAlloc(static_cast<VkBuffer>(*dst), static_cast<size_t>(newCapacity), tracyName);
    }

    {
        VkBuffer rawStaging = staging.release();
        vmaDestroyBuffer(allocator.allocator, rawStaging, stagingMemory);
    }

    log_info(std::format("{} ready used {} / cap {} bda=0x{:x}", debugName, formatBytes(usedBytes),
                         formatBytes(capacityBytes), static_cast<uint64_t>(dstAddress)),
             "ResourceManager");
#ifdef TRACY_ENABLE
    const std::string tracyMsg =
        std::format("{} used {} / cap {} bda=0x{:x}", debugName, formatBytes(usedBytes), formatBytes(capacityBytes),
                    static_cast<uint64_t>(dstAddress));
    TracyMessage(tracyMsg.c_str(), tracyMsg.size());
#endif
}

void ResourceManager::remapScratchOffsets()
{
    ZoneScopedN("ResourceManager::remapScratchOffsets");
    GeometryStore& geometry = geometryStore;
    const uint32_t newPrimitives =
        static_cast<uint32_t>(geometry.primitiveDraws.size()) - geometry.flushedPrimitiveCount;
    const uint32_t newMorphTargets =
        static_cast<uint32_t>(geometry.morphTargets.size()) - geometry.flushedMorphTargetCount;
    log_info(std::format("remap scratch: +{} prims +{} morphTargets onto gpu verts={} meshlets={} indices={}",
                         newPrimitives, newMorphTargets, uploadedVertexCount, uploadedMeshletCount,
                         uploadedIndexCount),
             "ResourceManager");
    for (uint32_t& vertexIndex : geometry.meshletVertices) {
        vertexIndex += uploadedVertexCount;
    }
    for (uint32_t& index : geometry.indices) {
        index += uploadedVertexCount;
    }
    for (GpuMeshletDesc& meshlet : geometry.meshlets) {
        meshlet.vertexOffset += uploadedMeshletVertexCount;
        meshlet.triangleOffset += uploadedMeshletTriangleCount;
    }
    for (uint32_t p = geometry.flushedPrimitiveCount; p < geometry.primitiveDraws.size(); ++p) {
        PrimitiveDraw& draw = geometry.primitiveDraws[p];
        draw.firstVertex += uploadedVertexCount;
        draw.firstIndex += uploadedIndexCount;
        draw.meshlets.firstMeshlet += uploadedMeshletCount;
    }
    auto bump = [](uint32_t& value, uint32_t delta) {
        if (value != kNoneIndex) {
            value += delta;
        }
    };
    for (uint32_t t = geometry.flushedMorphTargetCount; t < geometry.morphTargets.size(); ++t) {
        MorphTarget& target = geometry.morphTargets[t];
        bump(target.posOffset, uploadedMorphPosCount);
        bump(target.nrmOffset, uploadedMorphNrmCount);
        bump(target.tanOffset, uploadedMorphTanCount);
    }
    for (EntityId id = 0; id < objectStorage.size(); ++id) {
        if (objectStorage.firstPrimitives[id] >= geometry.flushedPrimitiveCount) {
            objectStorage.meshletDraws[id].firstMeshlet += uploadedMeshletCount;
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

void ResourceManager::createIndirectBuffer()
{
    ZoneScopedN("ResourceManager::createIndirectBuffer");
    log_info("createIndirectBuffer() started", "ResourceManager");
    if (indirectBufferMemory != nullptr) {
        VkBuffer raw = indirectBuffer.release();
        tracyResourceFree(raw, "GPU/IndirectCopyCommand");
        vmaDestroyBuffer(allocator.allocator, raw, indirectBufferMemory);
        indirectBufferMemory = nullptr;
        indirectBufferAddress = 0;
    }
    // Host-visible: copyBuffer memcpy's VkCopyMemoryIndirectCommandKHR here.
    // Device-local cannot be mapped; SDA required for copyAddressRange.
    const vk::DeviceSize bufferSize = sizeof(vk::CopyMemoryIndirectCommandKHR);
    createBuffer(bufferSize, vk::BufferUsageFlagBits2::eShaderDeviceAddress | vk::BufferUsageFlagBits2::eIndirectBuffer,
                 vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, indirectBuffer,
                 indirectBufferMemory, allocator.allocator, device, queueFamilyIndices, "IndirectBufferMemory");
    setDebugName(device, indirectBuffer, "IndirectCopyCommandBuffer");
    indirectBufferAddress = device.getBufferAddress({.buffer = *indirectBuffer});
    tracyResourceAlloc(static_cast<VkBuffer>(*indirectBuffer), static_cast<size_t>(bufferSize),
                       "GPU/IndirectCopyCommand");
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
    if (entityCount == 0)
    {
        return;
    }
    if (entityCount <= instanceCapacity)
    {
        return;
    }

    ZoneScopedN("ResourceManager::ensureInstanceCapacity");
    // Grow with headroom so interactive loads do not reallocate every time.
    const uint32_t newCapacity = std::max(entityCount, instanceCapacity == 0 ? entityCount : instanceCapacity * 2);
    log_info(std::format("Growing instance GpuObjectUB capacity {} -> {}", instanceCapacity, newCapacity),
             "ResourceManager");

    destroyInstanceUboBuffers();
    instanceCapacity = newCapacity;

    const vk::DeviceSize bufferSize = sizeof(GpuObjectUB) * static_cast<vk::DeviceSize>(instanceCapacity);
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
    {
        vk::raii::Buffer buffer({});
        VmaAllocation bufferMem = nullptr;
        createBuffer(bufferSize,
                     vk::BufferUsageFlagBits2::eUniformBuffer | vk::BufferUsageFlagBits2::eStorageBuffer |
                         vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                     vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, buffer,
                     bufferMem, allocator.allocator, device, queueFamilyIndices,
                     std::format("InstanceObjectUBMemory_{}", i));
        instanceUboBuffers[i] = std::move(buffer);
        instanceUboMemory[i] = bufferMem;
        void* data = nullptr;
        vmaMapMemory(allocator.allocator, bufferMem, &data);
        instanceUboMapped[i] = data;
        instanceUboBaseAddresses[i] = device.getBufferAddress({.buffer = *instanceUboBuffers[i]});
        setDebugName(device, instanceUboBuffers[i], std::format("InstanceObjectUB_{}", i));
        tracyResourceAlloc(static_cast<VkBuffer>(*instanceUboBuffers[i]), static_cast<size_t>(bufferSize),
                           "GPU/InstanceUBO");
        trackedInstanceUboBytes[i] = bufferSize;
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

    const uint32_t newVertexCount = static_cast<uint32_t>(geometryStore.vertices.size());
    const uint32_t newMeshletCount = static_cast<uint32_t>(geometryStore.meshlets.size());
    const uint32_t newMeshletVertexCount = static_cast<uint32_t>(geometryStore.meshletVertices.size());
    const uint32_t newMeshletTriangleCount = static_cast<uint32_t>(geometryStore.meshletTriangles.size());
    const uint32_t newIndexCount = static_cast<uint32_t>(geometryStore.indices.size());
    const uint32_t newMorphPosCount = static_cast<uint32_t>(geometryStore.morphPos.size());
    const uint32_t newMorphNrmCount = static_cast<uint32_t>(geometryStore.morphNrm.size());
    const uint32_t newMorphTanCount = static_cast<uint32_t>(geometryStore.morphTan.size());
    const uint32_t pendingMaterials = materialStore.pendingCount();
    log_info(std::format("material cache: total={} uploaded={} pending={} hits={} misses={}", materialStore.size(),
                         materialStore.uploadedCount, pendingMaterials, materialStore.cacheHits,
                         materialStore.cacheMisses),
             "ResourceManager");

    appendDeviceLocal(vertexBuffer, vertexBufferMemory, vertexBufferAddress, trackedVertexBytes, capacityVertexBytes,
                      geometryStore.vertices.data(),
                      static_cast<vk::DeviceSize>(newVertexCount) * sizeof(GpuVertex), "VertexBuffer",
                      "GPU/Vertices");
    appendDeviceLocal(meshletBuffer, meshletBufferMemory, meshletBufferAddress, trackedMeshletBytes,
                      capacityMeshletBytes, geometryStore.meshlets.data(),
                      static_cast<vk::DeviceSize>(newMeshletCount) * sizeof(GpuMeshletDesc), "MeshletBuffer",
                      "GPU/Meshlets");
    appendDeviceLocal(meshletVertexBuffer, meshletVertexBufferMemory, meshletVertexBufferAddress,
                      trackedMeshletVertexBytes, capacityMeshletVertexBytes, geometryStore.meshletVertices.data(),
                      static_cast<vk::DeviceSize>(newMeshletVertexCount) * sizeof(uint32_t), "MeshletVertexBuffer",
                      "GPU/MeshletVertices");
    appendDeviceLocal(meshletTriangleBuffer, meshletTriangleBufferMemory, meshletTriangleBufferAddress,
                      trackedMeshletTriangleBytes, capacityMeshletTriangleBytes, geometryStore.meshletTriangles.data(),
                      static_cast<vk::DeviceSize>(newMeshletTriangleCount) * sizeof(uint8_t), "MeshletTriangleBuffer",
                      "GPU/MeshletTriangles");
    appendDeviceLocal(normalBuffer, normalBufferMemory, normalBufferAddress, trackedNormalBytes, capacityNormalBytes,
                      geometryStore.normals.data(),
                      static_cast<vk::DeviceSize>(geometryStore.normals.size()) * sizeof(glm::vec3), "NormalBuffer",
                      "GPU/Normals");
    appendDeviceLocal(tangentBuffer, tangentBufferMemory, tangentBufferAddress, trackedTangentBytes,
                      capacityTangentBytes, geometryStore.tangents.data(),
                      static_cast<vk::DeviceSize>(geometryStore.tangents.size()) * sizeof(glm::vec4), "TangentBuffer",
                      "GPU/Tangents");
    appendDeviceLocal(uv1Buffer, uv1BufferMemory, uv1BufferAddress, trackedUv1Bytes, capacityUv1Bytes,
                      geometryStore.uv1.data(), static_cast<vk::DeviceSize>(geometryStore.uv1.size()) * sizeof(glm::vec2),
                      "Uv1Buffer", "GPU/Uv1");
    appendDeviceLocal(jointBuffer, jointBufferMemory, jointBufferAddress, trackedJointBytes, capacityJointBytes,
                      geometryStore.joints0.data(),
                      static_cast<vk::DeviceSize>(geometryStore.joints0.size()) * sizeof(std::array<uint16_t, 4>),
                      "JointBuffer", "GPU/Joints");
    appendDeviceLocal(weightBuffer, weightBufferMemory, weightBufferAddress, trackedWeightBytes, capacityWeightBytes,
                      geometryStore.weights0.data(),
                      static_cast<vk::DeviceSize>(geometryStore.weights0.size()) * sizeof(glm::vec4), "WeightBuffer",
                      "GPU/Weights");
    appendDeviceLocal(indexBuffer, indexBufferMemory, indexBufferAddress, trackedIndexBytes, capacityIndexBytes,
                      geometryStore.indices.data(), static_cast<vk::DeviceSize>(newIndexCount) * sizeof(uint32_t),
                      "IndexBuffer", "GPU/Indices");
    appendDeviceLocal(morphPosBuffer, morphPosBufferMemory, morphPosBufferAddress, trackedMorphPosBytes,
                      capacityMorphPosBytes, geometryStore.morphPos.data(),
                      static_cast<vk::DeviceSize>(newMorphPosCount) * sizeof(glm::vec3), "MorphPosBuffer",
                      "GPU/MorphPos");
    appendDeviceLocal(morphNrmBuffer, morphNrmBufferMemory, morphNrmBufferAddress, trackedMorphNrmBytes,
                      capacityMorphNrmBytes, geometryStore.morphNrm.data(),
                      static_cast<vk::DeviceSize>(newMorphNrmCount) * sizeof(glm::vec3), "MorphNrmBuffer",
                      "GPU/MorphNrm");
    appendDeviceLocal(morphTanBuffer, morphTanBufferMemory, morphTanBufferAddress, trackedMorphTanBytes,
                      capacityMorphTanBytes, geometryStore.morphTan.data(),
                      static_cast<vk::DeviceSize>(newMorphTanCount) * sizeof(glm::vec4), "MorphTanBuffer",
                      "GPU/MorphTan");
    appendDeviceLocal(materialBuffer, materialBufferMemory, materialBufferAddress, trackedMaterialBytes,
                      capacityMaterialBytes, materialStore.gpuMaterials.data() + materialStore.uploadedCount,
                      static_cast<vk::DeviceSize>(pendingMaterials) * sizeof(GpuMaterial), "MaterialBuffer",
                      "GPU/Materials");
    appendDeviceLocal(pbrExtBuffer, pbrExtBufferMemory, pbrExtBufferAddress, trackedPbrExtBytes, capacityPbrExtBytes,
                      materialStore.pbrExtensions.data() + materialStore.uploadedCount,
                      static_cast<vk::DeviceSize>(pendingMaterials) * sizeof(MaterialPbrExtension), "PbrExtBuffer",
                      "GPU/PbrExtensions");

    const std::vector<GpuLight> packedLights = packScratchLights();
    appendDeviceLocal(lightBuffer, lightBufferMemory, lightBufferAddress, trackedLightBytes, capacityLightBytes,
                      packedLights.data(), static_cast<vk::DeviceSize>(packedLights.size()) * sizeof(GpuLight),
                      "LightBuffer", "GPU/Lights");

    uploadedVertexCount += newVertexCount;
    uploadedMeshletCount += newMeshletCount;
    uploadedMeshletVertexCount += newMeshletVertexCount;
    uploadedMeshletTriangleCount += newMeshletTriangleCount;
    uploadedIndexCount += newIndexCount;
    uploadedMorphPosCount += newMorphPosCount;
    uploadedMorphNrmCount += newMorphNrmCount;
    uploadedMorphTanCount += newMorphTanCount;
    materialStore.markUploaded();
    lightStore.uploadedCount += static_cast<uint32_t>(packedLights.size());

    geometryStore.clearScratch();
    lightStore.clearScratch();

    const vk::DeviceSize gpuAssetBytes = trackedVertexBytes + trackedMeshletBytes + trackedMeshletVertexBytes +
        trackedMeshletTriangleBytes + trackedNormalBytes + trackedTangentBytes + trackedUv1Bytes + trackedJointBytes +
        trackedWeightBytes + trackedIndexBytes + trackedMorphPosBytes + trackedMorphNrmBytes + trackedMorphTanBytes +
        trackedMaterialBytes + trackedPbrExtBytes + trackedLightBytes;

    log_info(std::format("gpu catalog: verts={} meshlets={} meshletVerts={} triCorners={} indices={} "
                         "morphPos/Nrm/Tan={}/{}/{} materials={} lights={} prims={} morphTargets={}",
                         uploadedVertexCount, uploadedMeshletCount, uploadedMeshletVertexCount,
                         uploadedMeshletTriangleCount, uploadedIndexCount, uploadedMorphPosCount,
                         uploadedMorphNrmCount, uploadedMorphTanCount, materialStore.uploadedCount,
                         lightStore.uploadedCount, geometryStore.primitiveDraws.size(),
                         geometryStore.morphTargets.size()),
             "ResourceManager");
    const vk::DeviceSize gpuAssetCapacity = capacityVertexBytes + capacityMeshletBytes + capacityMeshletVertexBytes +
        capacityMeshletTriangleBytes + capacityNormalBytes + capacityTangentBytes + capacityUv1Bytes +
        capacityJointBytes + capacityWeightBytes + capacityIndexBytes + capacityMorphPosBytes + capacityMorphNrmBytes +
        capacityMorphTanBytes + capacityMaterialBytes + capacityPbrExtBytes + capacityLightBytes;
    log_info(std::format("gpu ssbo used: verts={} meshlets={} meshletVerts={} tris={} normals={} tangents={} "
                         "uv1={} joints={} weights={} indices={} morph={}/{}/{} materials={} pbrExt={} lights={} "
                         "total={}",
                         formatBytes(trackedVertexBytes), formatBytes(trackedMeshletBytes),
                         formatBytes(trackedMeshletVertexBytes), formatBytes(trackedMeshletTriangleBytes),
                         formatBytes(trackedNormalBytes), formatBytes(trackedTangentBytes), formatBytes(trackedUv1Bytes),
                         formatBytes(trackedJointBytes), formatBytes(trackedWeightBytes), formatBytes(trackedIndexBytes),
                         formatBytes(trackedMorphPosBytes), formatBytes(trackedMorphNrmBytes),
                         formatBytes(trackedMorphTanBytes), formatBytes(trackedMaterialBytes),
                         formatBytes(trackedPbrExtBytes), formatBytes(trackedLightBytes), formatBytes(gpuAssetBytes)),
             "ResourceManager");
    log_info(std::format("gpu ssbo cap: verts={} meshlets={} materials={} pbrExt={} lights={} total={}",
                         formatBytes(capacityVertexBytes), formatBytes(capacityMeshletBytes),
                         formatBytes(capacityMaterialBytes), formatBytes(capacityPbrExtBytes),
                         formatBytes(capacityLightBytes), formatBytes(gpuAssetCapacity)),
             "ResourceManager");
    log_info(std::format("cpu after flush: scratch verts={} meshlets={} | material cache={} lights scratch={}",
                         geometryStore.vertices.size(), geometryStore.meshlets.size(), materialStore.size(),
                         lightStore.instances.size()),
             "ResourceManager");
#ifdef TRACY_ENABLE
    const std::string tracyMsg = std::format(
        "flush gpu assets total={} verts={} meshlets={} materials={} lights={}", formatBytes(gpuAssetBytes),
        uploadedVertexCount, uploadedMeshletCount, materialStore.uploadedCount, lightStore.uploadedCount);
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

    const vk::DeviceSize gpuAssetBytes = trackedVertexBytes + trackedMeshletBytes + trackedMeshletVertexBytes +
        trackedMeshletTriangleBytes + trackedNormalBytes + trackedTangentBytes + trackedUv1Bytes + trackedJointBytes +
        trackedWeightBytes + trackedIndexBytes + trackedMorphPosBytes + trackedMorphNrmBytes + trackedMorphTanBytes +
        trackedMaterialBytes + trackedPbrExtBytes + trackedLightBytes;

    TracyPlot("Vulkan/EntityCount", static_cast<double>(objectStorage.size()));
    TracyPlot("Vulkan/ActiveEntities", static_cast<double>(activeEntities));
    TracyPlot("Vulkan/ActiveMeshlets", static_cast<double>(activeMeshlets));
    TracyPlot("Vulkan/PrimitiveCount", static_cast<double>(geometryStore.primitiveDraws.size()));
    TracyPlot("Vulkan/MeshletCount", static_cast<double>(uploadedMeshletCount));
    TracyPlot("Vulkan/MeshletVertexCount", static_cast<double>(uploadedMeshletVertexCount));
    TracyPlot("Vulkan/MeshletTriangleCorners", static_cast<double>(uploadedMeshletTriangleCount));
    TracyPlot("Vulkan/VerticesInUse", static_cast<double>(uploadedVertexCount));
    TracyPlot("Vulkan/IndexCount", static_cast<double>(uploadedIndexCount));
    TracyPlot("Vulkan/MaterialCount", static_cast<double>(materialStore.uploadedCount));
    TracyPlot("Vulkan/MaterialCacheSize", static_cast<double>(materialStore.size()));
    TracyPlot("Vulkan/MaterialCacheHits", static_cast<double>(materialStore.cacheHits));
    TracyPlot("Vulkan/MaterialCacheMisses", static_cast<double>(materialStore.cacheMisses));
    TracyPlot("Vulkan/LightCount", static_cast<double>(lightStore.uploadedCount));
    TracyPlot("Vulkan/MorphPosCount", static_cast<double>(uploadedMorphPosCount));
    TracyPlot("Vulkan/MorphNrmCount", static_cast<double>(uploadedMorphNrmCount));
    TracyPlot("Vulkan/MorphTanCount", static_cast<double>(uploadedMorphTanCount));
    TracyPlot("Vulkan/VertexBytes", static_cast<double>(trackedVertexBytes));
    TracyPlot("Vulkan/MeshletBytes", static_cast<double>(trackedMeshletBytes));
    TracyPlot("Vulkan/MeshletVertexBytes", static_cast<double>(trackedMeshletVertexBytes));
    TracyPlot("Vulkan/MeshletTriangleBytes", static_cast<double>(trackedMeshletTriangleBytes));
    TracyPlot("Vulkan/NormalBytes", static_cast<double>(trackedNormalBytes));
    TracyPlot("Vulkan/TangentBytes", static_cast<double>(trackedTangentBytes));
    TracyPlot("Vulkan/Uv1Bytes", static_cast<double>(trackedUv1Bytes));
    TracyPlot("Vulkan/JointBytes", static_cast<double>(trackedJointBytes));
    TracyPlot("Vulkan/WeightBytes", static_cast<double>(trackedWeightBytes));
    TracyPlot("Vulkan/IndexBytes", static_cast<double>(trackedIndexBytes));
    TracyPlot("Vulkan/MorphPosBytes", static_cast<double>(trackedMorphPosBytes));
    TracyPlot("Vulkan/MorphNrmBytes", static_cast<double>(trackedMorphNrmBytes));
    TracyPlot("Vulkan/MorphTanBytes", static_cast<double>(trackedMorphTanBytes));
    TracyPlot("Vulkan/MaterialBytes", static_cast<double>(trackedMaterialBytes));
    TracyPlot("Vulkan/PbrExtBytes", static_cast<double>(trackedPbrExtBytes));
    TracyPlot("Vulkan/LightBytes", static_cast<double>(trackedLightBytes));
    TracyPlot("Vulkan/VertexCapacityBytes", static_cast<double>(capacityVertexBytes));
    TracyPlot("Vulkan/MeshletCapacityBytes", static_cast<double>(capacityMeshletBytes));
    TracyPlot("Vulkan/MeshletVertexCapacityBytes", static_cast<double>(capacityMeshletVertexBytes));
    TracyPlot("Vulkan/MeshletTriangleCapacityBytes", static_cast<double>(capacityMeshletTriangleBytes));
    TracyPlot("Vulkan/NormalCapacityBytes", static_cast<double>(capacityNormalBytes));
    TracyPlot("Vulkan/TangentCapacityBytes", static_cast<double>(capacityTangentBytes));
    TracyPlot("Vulkan/Uv1CapacityBytes", static_cast<double>(capacityUv1Bytes));
    TracyPlot("Vulkan/JointCapacityBytes", static_cast<double>(capacityJointBytes));
    TracyPlot("Vulkan/WeightCapacityBytes", static_cast<double>(capacityWeightBytes));
    TracyPlot("Vulkan/IndexCapacityBytes", static_cast<double>(capacityIndexBytes));
    TracyPlot("Vulkan/MorphPosCapacityBytes", static_cast<double>(capacityMorphPosBytes));
    TracyPlot("Vulkan/MorphNrmCapacityBytes", static_cast<double>(capacityMorphNrmBytes));
    TracyPlot("Vulkan/MorphTanCapacityBytes", static_cast<double>(capacityMorphTanBytes));
    TracyPlot("Vulkan/MaterialCapacityBytes", static_cast<double>(capacityMaterialBytes));
    TracyPlot("Vulkan/PbrExtCapacityBytes", static_cast<double>(capacityPbrExtBytes));
    TracyPlot("Vulkan/LightCapacityBytes", static_cast<double>(capacityLightBytes));
    const vk::DeviceSize gpuAssetCapacity = capacityVertexBytes + capacityMeshletBytes + capacityMeshletVertexBytes +
        capacityMeshletTriangleBytes + capacityNormalBytes + capacityTangentBytes + capacityUv1Bytes +
        capacityJointBytes + capacityWeightBytes + capacityIndexBytes + capacityMorphPosBytes + capacityMorphNrmBytes +
        capacityMorphTanBytes + capacityMaterialBytes + capacityPbrExtBytes + capacityLightBytes;
    TracyPlot("Vulkan/GpuAssetBytes", static_cast<double>(gpuAssetBytes));
    TracyPlot("Vulkan/GpuAssetCapacityBytes", static_cast<double>(gpuAssetCapacity));
    TracyPlot("Vulkan/CpuScratchVertices", static_cast<double>(geometryStore.vertices.size()));
    TracyPlot("Vulkan/CpuScratchMeshlets", static_cast<double>(geometryStore.meshlets.size()));
    TracyPlot("Vulkan/InstanceCapacity", static_cast<double>(instanceCapacity));
    TracyPlot("Vulkan/InstanceUboBytes", static_cast<double>(trackedInstanceUboBytes[0]));
    TracyPlot("Vulkan/CommandBuffersInUse", static_cast<double>(commandBuffers.size()));
    TracyPlot("Vulkan/MeshBdaReady",
              static_cast<double>(vertexBufferAddress != 0 && meshletBufferAddress != 0 &&
                                  meshletVertexBufferAddress != 0 && meshletTriangleBufferAddress != 0));
#endif
}

vk::Format ResourceManager::findSupportedFormat(const std::vector<vk::Format>& candidates, vk::ImageTiling tiling,
                                                vk::FormatFeatureFlags features)
{
    log_info("findSupportedFormat() started", "ResourceManager");
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

uint32_t ResourceManager::findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties)
{
    log_info("findMemoryType() started", "ResourceManager");
    vk::PhysicalDeviceMemoryProperties memProperties = physicalDevice.getMemoryProperties2().memoryProperties;
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("failed to find suitable memory type!");
}

vk::raii::ImageView ResourceManager::createImageView(vk::raii::Image& image, vk::Format format,
                                                     vk::ImageAspectFlags aspectFlags, uint32_t mipLevels)
{
    log_info("createImageView() started", "ResourceManager");
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

void ResourceManager::createColorResources()
{
    ZoneScopedN("ResourceManager::createColorResources");
    log_info("createColorResources() started", "ResourceManager");
    if (swapChainImageFormat == vk::Format::eUndefined) {
        return;
    }

    // Destroy previous color resources before recreating
    if (colorImageMemory != nullptr) {
        VkImage raw = colorImage.release();
        tracyResourceFree(raw, "GPU/ColorMSAA");
        vmaDestroyImage(allocator.allocator, raw, colorImageMemory);
        colorImageMemory = nullptr;
        colorImageView = nullptr;
        trackedColorBytes = 0;
    }
    vk::Format colorFormat = swapChainImageFormat;

    createImage(swapChainExtent.width, swapChainExtent.height, 1, msaaSamples, colorFormat, vk::ImageTiling::eOptimal,
                vk::ImageUsageFlagBits::eTransientAttachment | vk::ImageUsageFlagBits::eColorAttachment,
                vk::MemoryPropertyFlagBits::eDeviceLocal, colorImage, colorImageMemory, "ColorImageMemory");
    setDebugName(device, colorImage, "ColorImage");
    // Approximate MSAA color footprint (4 B/pixel * samples).
    trackedColorBytes = static_cast<vk::DeviceSize>(swapChainExtent.width) * swapChainExtent.height *
        static_cast<uint32_t>(msaaSamples) * 4u;
    tracyResourceAlloc(static_cast<VkImage>(*colorImage), static_cast<size_t>(trackedColorBytes), "GPU/ColorMSAA");
    commandBuffers[0].begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    transitionImageLayout(&commandBuffers[0], colorImage, 1, vk::ImageLayout::eUndefined,
                          vk::ImageLayout::eColorAttachmentOptimal,
                          {.aspectMask = vk::ImageAspectFlagBits::eColor,
                           .baseMipLevel = 0,
                           .levelCount = 1,
                           .baseArrayLayer = 0,
                           .layerCount = 1});
    endCommandBuffer(commandBuffers[0], graphicsQueue);
    colorImageView = createImageView(colorImage, colorFormat, vk::ImageAspectFlagBits::eColor, 1);
}

void ResourceManager::createImage(uint32_t width, uint32_t height, uint32_t mipLevels, vk::SampleCountFlagBits Samples,
                                  vk::Format format, vk::ImageTiling tiling, vk::ImageUsageFlags usage,
                                  vk::MemoryPropertyFlags properties, vk::raii::Image& image,
                                  VmaAllocation& imageMemory, std::string_view memoryDebugBaseName)
{
    ZoneScopedN("ResourceManager::createImage");
    log_info("createImage() started", "ResourceManager");
    // Determine sharing mode based on usage
    vk::SharingMode sharingMode = vk::SharingMode::eExclusive;
    std::vector<uint32_t> queueIndices;

    // Only use concurrent sharing for transfer operations between different queue families
    if ((usage & vk::ImageUsageFlagBits::eTransferSrc || usage & vk::ImageUsageFlagBits::eTransferDst) &&
        transferIndex != UINT32_MAX && transferIndex != graphicsIndex) {
        sharingMode = vk::SharingMode::eConcurrent;
        queueIndices = queueFamilyIndices;
    }

    vk::ImageCreateInfo const imageInfo{.imageType = vk::ImageType::e2D,
                                        .format = format,
                                        .extent = {.width = width, .height = height, .depth = 1},
                                        .mipLevels = mipLevels,
                                        .arrayLayers = 1,
                                        .samples = Samples,
                                        .tiling = tiling,
                                        .usage = usage,
                                        .sharingMode = sharingMode,
                                        .queueFamilyIndexCount = static_cast<uint32_t>(queueIndices.size()),
                                        .pQueueFamilyIndices = queueIndices.data()};

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (properties & vk::MemoryPropertyFlagBits::eHostVisible) {
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        allocInfo.priority = 0.25f;
    } else if (usage & (vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eDepthStencilAttachment |
                        vk::ImageUsageFlagBits::eTransientAttachment)) {
        // Color/depth attachments should demote last (NVIDIA memory priority best practice).
        allocInfo.priority = 1.0f;
    } else {
        allocInfo.priority = 0.9f;
    }

    allocator.alocateImage(imageInfo, allocInfo, image, imageMemory, memoryDebugBaseName);
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
    log_info("updateSwapChainExtent() started", "ResourceManager");
    swapChainExtent = newExtent;
}

void ResourceManager::createDepthResources()
{
    ZoneScopedN("ResourceManager::createDepthResources");
    log_info("createDepthResources() started", "ResourceManager");
    vk::Format depthFormat = findDepthFormat();
    log_info(std::format("Depth format selected: {}", vk::to_string(depthFormat)), "ResourceManager");

    // Destroy previous depth resources before recreating
    if (depthImageMemory != nullptr) {
        VkImage raw = depthImage.release();
        tracyResourceFree(raw, "GPU/Depth");
        vmaDestroyImage(allocator.allocator, raw, depthImageMemory);
        depthImageMemory = nullptr;
        depthImageView = nullptr;
        trackedDepthBytes = 0;
    }
    createImage(swapChainExtent.width, swapChainExtent.height, 1, msaaSamples, depthFormat, vk::ImageTiling::eOptimal,
                vk::ImageUsageFlagBits::eDepthStencilAttachment, vk::MemoryPropertyFlagBits::eDeviceLocal, depthImage,
                depthImageMemory, "DepthImageMemory");
    setDebugName(device, depthImage, "DepthImage");
    trackedDepthBytes = static_cast<vk::DeviceSize>(swapChainExtent.width) * swapChainExtent.height *
        static_cast<uint32_t>(msaaSamples) * 4u;
    tracyResourceAlloc(static_cast<VkImage>(*depthImage), static_cast<size_t>(trackedDepthBytes), "GPU/Depth");
    // View can be depth-only for the attachment; barriers must still cover both aspects
    // when the format is packed depth/stencil and separateDepthStencilLayouts is off
    // (VUID-VkImageMemoryBarrier2-image-03320).
    depthImageView = createImageView(depthImage, depthFormat, vk::ImageAspectFlagBits::eDepth, 1);
    vk::ImageAspectFlags barrierAspects = vk::ImageAspectFlagBits::eDepth;
    if (hasStencilComponent(depthFormat)) {
        barrierAspects |= vk::ImageAspectFlagBits::eStencil;
    }
    commandBuffers[0].begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    transitionImageLayout(&commandBuffers[0], depthImage, 1, vk::ImageLayout::eUndefined,
                          vk::ImageLayout::eDepthStencilAttachmentOptimal,
                          {.aspectMask = barrierAspects,
                           .baseMipLevel = 0,
                           .levelCount = 1,
                           .baseArrayLayer = 0,
                           .layerCount = 1});
    endCommandBuffer(commandBuffers[0], graphicsQueue);
}

bool ResourceManager::hasStencilComponent(vk::Format format)
{
    log_info("hasStencilComponent() started", "ResourceManager");
    return format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint ||
           format == vk::Format::eD16UnormS8Uint;
}

void ResourceManager::copyBufferToImage(const vk::raii::Buffer& buffer, vk::raii::Image& image, uint32_t width,
                                        uint32_t height)
{
    ZoneScopedN("ResourceManager::copyBufferToImage");
    log_info("copyBufferToImage() started", "ResourceManager");

    vk::BufferImageCopy const region{.bufferOffset = 0,
                               .bufferRowLength = 0,
                               .bufferImageHeight = 0,
                               .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                               .imageOffset = {0, 0, 0},
                               .imageExtent = {width, height, 1}};
    commandBuffers[0].copyBufferToImage(buffer, image, vk::ImageLayout::eTransferDstOptimal, {region});
}

void ResourceManager::generateMipmaps(vk::raii::Image& image, vk::Format imageFormat, int32_t texWidth,
                                      int32_t texHeight, uint32_t mipLevels)
{
    ZoneScopedN("ResourceManager::generateMipmaps");
    log_info("generateMipmaps() started", "ResourceManager");
    // Check for blit support
    vk::FormatProperties formatProperties = physicalDevice.getFormatProperties2(imageFormat).formatProperties;
    if (!(formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImageFilterLinear)) {
        throw std::runtime_error("Texture image format does not support linear blitting!");
    }

    auto& graphicsCmd = commandBuffers[0];
    graphicsCmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});


    int32_t mipWidth = texWidth;
    int32_t mipHeight = texHeight;

    for (uint32_t i = 1; i < mipLevels; i++) {
        vk::ImageMemoryBarrier2 barrier_to_src = {
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .image = *image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, i - 1, 1, 0, 1}};
        vk::DependencyInfo depInfoToSrc{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier_to_src};
        graphicsCmd.pipelineBarrier2(depInfoToSrc);

        vk::ImageBlit blit{};
        blit.srcSubresource = {
            .aspectMask = vk::ImageAspectFlagBits::eColor, .mipLevel = i - 1, .baseArrayLayer = 0, .layerCount = 1};
        blit.srcOffsets[0] = vk::Offset3D(0, 0, 0);
        blit.srcOffsets[1] = vk::Offset3D(mipWidth, mipHeight, 1);
        blit.dstSubresource = {
            .aspectMask = vk::ImageAspectFlagBits::eColor, .mipLevel = i, .baseArrayLayer = 0, .layerCount = 1};
        blit.dstOffsets[0] = vk::Offset3D(0, 0, 0);
        blit.dstOffsets[1] = vk::Offset3D(mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1);

        graphicsCmd.blitImage(*image, vk::ImageLayout::eTransferSrcOptimal, *image,
                              vk::ImageLayout::eTransferDstOptimal, {blit}, vk::Filter::eLinear);

        if (mipWidth > 1)
            mipWidth /= 2;
        if (mipHeight > 1)
            mipHeight /= 2;
    }

    vk::ImageMemoryBarrier2 barrier_last = {.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                                            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                                            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                                            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                                            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                                            .image = *image,
                                            .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                                                 .baseMipLevel = mipLevels - 1,
                                                                 .levelCount = 1,
                                                                 .baseArrayLayer = 0,
                                                                 .layerCount = 1}};
    vk::DependencyInfo depInfoLast{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier_last};
    graphicsCmd.pipelineBarrier2(depInfoLast);

    vk::ImageMemoryBarrier2 final_barrier = {.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                             .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
                                             .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                                             .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                                             .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                                             .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                             .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                                             .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                                             .image = *image,
                                             .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                                                  .baseMipLevel = 0,
                                                                  .levelCount = mipLevels,
                                                                  .baseArrayLayer = 0,
                                                                  .layerCount = 1}};
    vk::DependencyInfo depInfoFinal{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &final_barrier};
    graphicsCmd.pipelineBarrier2(depInfoFinal);

    endCommandBuffer(graphicsCmd, graphicsQueue);
}
