#include "vk_resource_manager.hpp"
#include <algorithm>
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
               const std::vector<Vertex> &verticesIn,
               const std::vector<MeshletDesc>& meshletsIn,
               const std::vector<uint32_t>& meshletVerticesIn,
               const std::vector<uint8_t>& meshletTrianglesIn,
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
      graphicsIndex(deviceWrapper.graphicsIndex),
      transferIndex(deviceWrapper.transferIndex),
      msaaSamples(deviceWrapper.msaaSamples),
      vertices(verticesIn),
      meshlets(meshletsIn),
      meshletVertices(meshletVerticesIn),
      meshletTriangles(meshletTrianglesIn)
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

ResourceManager::~ResourceManager()
{
    ZoneScopedN("ResourceManager::~ResourceManager");
    log_info("Destructor called", "ResourceManager");
    destroyInstanceUboBuffers();
    {
        if (vertexBufferMemory) {
            VkBuffer raw = vertexBuffer.release();
            tracyResourceFree(raw, "GPU/Vertices");
            vmaDestroyBuffer(allocator.allocator, raw, vertexBufferMemory);
            trackedVertexBytes = 0;
        }
        if (meshletBufferMemory) {
            VkBuffer raw = meshletBuffer.release();
            tracyResourceFree(raw, "GPU/Meshlets");
            vmaDestroyBuffer(allocator.allocator, raw, meshletBufferMemory);
            meshletBufferMemory = nullptr;
            meshletBufferAddress = 0;
            trackedMeshletBytes = 0;
        }
        if (meshletVertexBufferMemory) {
            VkBuffer raw = meshletVertexBuffer.release();
            tracyResourceFree(raw, "GPU/MeshletVertices");
            vmaDestroyBuffer(allocator.allocator, raw, meshletVertexBufferMemory);
            meshletVertexBufferMemory = nullptr;
            meshletVertexBufferAddress = 0;
            trackedMeshletVertexBytes = 0;
        }
        if (meshletTriangleBufferMemory) {
            VkBuffer raw = meshletTriangleBuffer.release();
            tracyResourceFree(raw, "GPU/MeshletTriangles");
            vmaDestroyBuffer(allocator.allocator, raw, meshletTriangleBufferMemory);
            meshletTriangleBufferMemory = nullptr;
            meshletTriangleBufferAddress = 0;
            trackedMeshletTriangleBytes = 0;
        }
        if (indirectBufferMemory) {
            VkBuffer raw = indirectBuffer.release();
            tracyResourceFree(raw, "GPU/IndirectCopyCommand");
            vmaDestroyBuffer(allocator.allocator, raw, indirectBufferMemory);
            indirectBufferMemory = nullptr;
            indirectBufferAddress = 0;
        }
        vertexBufferAddress = 0;
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
    createVertexBuffer();
    // Meshlet tables + BDAs for mesh shaders (static geometry; single addresses).
    createMeshBuffers();
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

    applyYawSpin(objectStorage.transforms, 0.01f);

    const glm::mat4 meshPreRotation =
        glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));

    auto* mapped = static_cast<ObjectUB*>(instanceUboMapped[currentImage]);
    writeObjectUbs(objectStorage, std::span(mapped, objectStorage.size()), meshPreRotation);
}

vk::DeviceAddress ResourceManager::instanceUboAddress(uint32_t frameSlot, EntityId entityId) const noexcept
{
    return instanceUboBaseAddresses[frameSlot] + static_cast<vk::DeviceAddress>(entityId) * sizeof(ObjectUB);
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
    transferCommandBuffer[0].begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    transferCommandBuffer[0].copyBuffer(srcBuffer, dstBuffer, vk::BufferCopy(0, 0, size));
    transferCommandBuffer[0].end();
    vk::CommandBufferSubmitInfo commandBufferInfo = {.commandBuffer = *transferCommandBuffer[0]};
    const vk::SubmitInfo2 submitInfo{.commandBufferInfoCount = 1, .pCommandBufferInfos = &commandBufferInfo};
    transferQueue.submit2(submitInfo, nullptr);
    transferQueue.waitIdle();
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

void ResourceManager::createVertexBuffer()
{
    ZoneScopedN("ResourceManager::createVertexBuffer");
    log_info("createVertexBuffer() started", "ResourceManager");
    log_info(std::format("Creating vertex buffer with {} vertices", vertices.size()), "ResourceManager");

    if (vertices.empty()) {
        log_info("No vertices present, skipping vertex buffer creation", "ResourceManager");
        return;
    }

    vk::DeviceSize bufferSize = sizeof(vertices[0]) * vertices.size();


    createBuffer(bufferSize,
                 vk::BufferUsageFlagBits2::eTransferSrc | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, stagingBuffer,
                 stagingBufferMemory, allocator.allocator, device, queueFamilyIndices, "VertexStagingBufferMemory");
    setDebugName(device, stagingBuffer, "VertexStagingBuffer");

    void* dataStaging = nullptr;
    vmaMapMemory(allocator.allocator, stagingBufferMemory, &dataStaging);
    memcpy(dataStaging, vertices.data(), bufferSize);
    vmaUnmapMemory(allocator.allocator, stagingBufferMemory);

    if (vertexBufferMemory != nullptr) {
        VkBuffer rawVertex = vertexBuffer.release();
        tracyResourceFree(rawVertex, "GPU/Vertices");
        vmaDestroyBuffer(allocator.allocator, rawVertex, vertexBufferMemory);
        vertexBufferMemory = nullptr;
        trackedVertexBytes = 0;
    }

    // eStorageBuffer | eShaderDeviceAddress: mesh shader BDA loads (MeshPushData.vertices).
    createBuffer(bufferSize,
                 vk::BufferUsageFlagBits2::eTransferDst | vk::BufferUsageFlagBits2::eStorageBuffer |
                     vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eDeviceLocal, vertexBuffer, vertexBufferMemory, allocator.allocator, device, queueFamilyIndices, "VertexBufferMemory");
    setDebugName(device, vertexBuffer, "VertexBuffer");
    tracyResourceAlloc(static_cast<VkBuffer>(*vertexBuffer), static_cast<size_t>(bufferSize), "GPU/Vertices");
    trackedVertexBytes = bufferSize;

    copyBuffer(stagingBuffer, vertexBuffer, bufferSize);

    // Free staging buffer after use to avoid leaking allocations
    if (stagingBufferMemory != nullptr) {
        VkBuffer rawStaging = stagingBuffer.release();
        vmaDestroyBuffer(allocator.allocator, rawStaging, stagingBufferMemory);
        stagingBufferMemory = nullptr;
    }

    vertexBufferAddress = device.getBufferAddress({.buffer = *vertexBuffer});
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

void ResourceManager::createMeshBuffers()
{
    ZoneScopedN("ResourceManager::createMeshBuffers");
    log_info("createMeshBuffers() started", "ResourceManager");
    // Create meshlet descriptor buffer (MeshletDesc[])
    log_info(std::format("Creating Meshlet buffer with {} entries", meshlets.size()), "ResourceManager");
    if (meshlets.empty()) {
        log_info("No meshlets present, skipping meshlet buffer creation", "ResourceManager");
    } else {
        vk::DeviceSize bufferSize = sizeof(MeshletDesc) * meshlets.size();

        createBuffer(bufferSize,
                     vk::BufferUsageFlagBits2::eTransferSrc | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                     vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, stagingBuffer,
                     stagingBufferMemory, allocator.allocator, device, queueFamilyIndices, "MeshletStagingBufferMemory");
        setDebugName(device, stagingBuffer, "MeshletStagingBuffer");

        void* data = nullptr;
        vmaMapMemory(allocator.allocator, stagingBufferMemory, &data);
        memcpy(data, meshlets.data(), bufferSize);
        vmaUnmapMemory(allocator.allocator, stagingBufferMemory);

        if (meshletBufferMemory != nullptr) {
            VkBuffer rawMeshlet = meshletBuffer.release();
            tracyResourceFree(rawMeshlet, "GPU/Meshlets");
            vmaDestroyBuffer(allocator.allocator, rawMeshlet, meshletBufferMemory);
            meshletBufferMemory = nullptr;
            trackedMeshletBytes = 0;
        }

        createBuffer(bufferSize,
                     vk::BufferUsageFlagBits2::eTransferDst | vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                     vk::MemoryPropertyFlagBits::eDeviceLocal, meshletBuffer, meshletBufferMemory, allocator.allocator, device, queueFamilyIndices,
                     "MeshletBufferMemory");
        setDebugName(device, meshletBuffer, "MeshletBuffer");
        tracyResourceAlloc(static_cast<VkBuffer>(*meshletBuffer), static_cast<size_t>(bufferSize), "GPU/Meshlets");
        trackedMeshletBytes = bufferSize;

        copyBuffer(stagingBuffer, meshletBuffer, bufferSize);

        // Free staging buffer after use to avoid leaking allocations
        if (stagingBufferMemory != nullptr) {
            VkBuffer rawStaging = stagingBuffer.release();
            vmaDestroyBuffer(allocator.allocator, rawStaging, stagingBufferMemory);
            stagingBufferMemory = nullptr;
        }

        meshletBufferAddress = device.getBufferAddress({.buffer = *meshletBuffer});
    }

    // Create meshlet vertex remap buffer (uint32_t[])
    log_info(std::format("Creating meshletVertexBuffer buffer with {} entries", meshletVertices.size()), "ResourceManager");
    if (meshletVertices.empty()) {
        log_info("No meshlet vertex remap data, skipping meshletVertexBuffer creation", "ResourceManager");
    } else {
        vk::DeviceSize vertexBufferSize = sizeof(meshletVertices[0]) * meshletVertices.size();

        createBuffer(vertexBufferSize,
                     vk::BufferUsageFlagBits2::eTransferSrc | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                     vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, stagingBuffer,
                     stagingBufferMemory, allocator.allocator, device, queueFamilyIndices, "MeshletVertexStagingBufferMemory");
        setDebugName(device, stagingBuffer, "MeshletVertexStagingBuffer");

        void* vdata = nullptr;
        vmaMapMemory(allocator.allocator, stagingBufferMemory, &vdata);
        memcpy(vdata, meshletVertices.data(), vertexBufferSize);
        vmaUnmapMemory(allocator.allocator, stagingBufferMemory);

        if (meshletVertexBufferMemory != nullptr) {
            VkBuffer raw = meshletVertexBuffer.release();
            tracyResourceFree(raw, "GPU/MeshletVertices");
            vmaDestroyBuffer(allocator.allocator, raw, meshletVertexBufferMemory);
            meshletVertexBufferMemory = nullptr;
            trackedMeshletVertexBytes = 0;
        }

        // Remap table is SSBO-style BDA traffic in the mesh shader (uint[]), not a vertex binding.
        createBuffer(vertexBufferSize,
                     vk::BufferUsageFlagBits2::eTransferDst | vk::BufferUsageFlagBits2::eStorageBuffer |
                         vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                     vk::MemoryPropertyFlagBits::eDeviceLocal, meshletVertexBuffer, meshletVertexBufferMemory, allocator.allocator,
                     device, queueFamilyIndices, "MeshletVertexBufferMemory");
        setDebugName(device, meshletVertexBuffer, "MeshletVertexBuffer");
        tracyResourceAlloc(static_cast<VkBuffer>(*meshletVertexBuffer), static_cast<size_t>(vertexBufferSize),
                           "GPU/MeshletVertices");
        trackedMeshletVertexBytes = vertexBufferSize;

        copyBuffer(stagingBuffer, meshletVertexBuffer, vertexBufferSize);

        if (stagingBufferMemory != nullptr) {
            VkBuffer rawStaging = stagingBuffer.release();
            vmaDestroyBuffer(allocator.allocator, rawStaging, stagingBufferMemory);
            stagingBufferMemory = nullptr;
        }

        meshletVertexBufferAddress = device.getBufferAddress({.buffer = *meshletVertexBuffer});
    }

    // Create meshlet triangle local-corner buffer (uint8_t[])
    log_info(std::format("Creating meshletTriangleBuffer buffer with {} entries", meshletTriangles.size()), "ResourceManager");
    if (meshletTriangles.empty()) {
        log_info("No meshlet triangle data, skipping meshletTriangleBuffer creation", "ResourceManager");
    } else {
        vk::DeviceSize triBufferSize = sizeof(meshletTriangles[0]) * meshletTriangles.size();

        createBuffer(triBufferSize,
                     vk::BufferUsageFlagBits2::eTransferSrc | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                     vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, stagingBuffer,
                     stagingBufferMemory, allocator.allocator, device, queueFamilyIndices, "MeshletTriangleStagingBufferMemory");
        setDebugName(device, stagingBuffer, "MeshletTriangleStagingBuffer");

        void* tdata = nullptr;
        vmaMapMemory(allocator.allocator, stagingBufferMemory, &tdata);
        memcpy(tdata, meshletTriangles.data(), triBufferSize);
        vmaUnmapMemory(allocator.allocator, stagingBufferMemory);

        if (meshletTriangleBufferMemory != nullptr) {
            VkBuffer raw = meshletTriangleBuffer.release();
            tracyResourceFree(raw, "GPU/MeshletTriangles");
            vmaDestroyBuffer(allocator.allocator, raw, meshletTriangleBufferMemory);
            meshletTriangleBufferMemory = nullptr;
            trackedMeshletTriangleBytes = 0;
        }

        createBuffer(triBufferSize,
                     vk::BufferUsageFlagBits2::eTransferDst | vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                     vk::MemoryPropertyFlagBits::eDeviceLocal, meshletTriangleBuffer, meshletTriangleBufferMemory, allocator.allocator,
                     device, queueFamilyIndices, "MeshletTriangleBufferMemory");
        setDebugName(device, meshletTriangleBuffer, "MeshletTriangleBuffer");
        tracyResourceAlloc(static_cast<VkBuffer>(*meshletTriangleBuffer), static_cast<size_t>(triBufferSize),
                           "GPU/MeshletTriangles");
        trackedMeshletTriangleBytes = triBufferSize;

        copyBuffer(stagingBuffer, meshletTriangleBuffer, triBufferSize);

        if (stagingBufferMemory != nullptr) {
            VkBuffer rawStaging = stagingBuffer.release();
            vmaDestroyBuffer(allocator.allocator, rawStaging, stagingBufferMemory);
            stagingBufferMemory = nullptr;
        }

        meshletTriangleBufferAddress = device.getBufferAddress({.buffer = *meshletTriangleBuffer});
    }
}



void ResourceManager::createCameraBuffers(Camera& camera)
{
    ZoneScopedN("ResourceManager::createCameraBuffers");
    log_info("createCameraBuffers() started", "ResourceManager");
    camera.allocator = allocator.allocator;
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vk::DeviceSize bufferSize = sizeof(CameraData);
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
    log_info(std::format("Growing instance ObjectUB capacity {} -> {}", instanceCapacity, newCapacity),
             "ResourceManager");

    destroyInstanceUboBuffers();
    instanceCapacity = newCapacity;

    const vk::DeviceSize bufferSize = sizeof(ObjectUB) * static_cast<vk::DeviceSize>(instanceCapacity);
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

void ResourceManager::recreateObjectsBuffers()
{
    ZoneScopedN("ResourceManager::recreateObjectsBuffers");
    log_info("recreateObjectsBuffers() started", "ResourceManager");
    createVertexBuffer();
    createMeshBuffers();
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
    TracyPlot("Vulkan/MeshletCount", static_cast<double>(meshlets.size()));
    TracyPlot("Vulkan/MeshletVertexCount", static_cast<double>(meshletVertices.size()));
    TracyPlot("Vulkan/MeshletTriangleCorners", static_cast<double>(meshletTriangles.size()));
    TracyPlot("Vulkan/VerticesInUse", static_cast<double>(vertices.size()));
    TracyPlot("Vulkan/VertexBytesInUse", static_cast<double>(trackedVertexBytes));
    TracyPlot("Vulkan/MeshletBytes", static_cast<double>(trackedMeshletBytes));
    TracyPlot("Vulkan/MeshletVertexBytes", static_cast<double>(trackedMeshletVertexBytes));
    TracyPlot("Vulkan/MeshletTriangleBytes", static_cast<double>(trackedMeshletTriangleBytes));
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
