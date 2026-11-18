#include "vk_frame_sync.hpp"
#include "Constants.h"
#include "static_headers/logger.hpp"
#include "util/debug.hpp"

#include <format>

FrameSync::FrameSync(const Device& device) : device(device)
{
    createCommandPool();
    createCommandBuffers();
    const vk::SemaphoreTypeCreateInfo timelineInfo{.semaphoreType = vk::SemaphoreType::eTimeline, .initialValue = 0};
    computeTimeline = vk::raii::Semaphore(device.vkdevice, vk::SemaphoreCreateInfo{.pNext = &timelineInfo});
    setDebugName(device.vkdevice, computeTimeline, "ComputeTimeline");
    rayTimeline = vk::raii::Semaphore(device.vkdevice, vk::SemaphoreCreateInfo{.pNext = &timelineInfo});
    setDebugName(device.vkdevice, rayTimeline, "RayTimeline");
    // every frame path ends in a submit that signals its slot's fence, so a resize never needs new ones
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        inFlightFences.emplace_back(device.vkdevice, vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled});
        setDebugName(device.vkdevice, inFlightFences.back(), std::format("InFlightFence_{}", i));
    }
}

void FrameSync::recreate(uint32_t swapChainImageCount)
{
    logInfo("recreate() started", "FrameSync");
    const vk::raii::Device& vkDevice = device.vkdevice;
    presentCompleteSemaphore.clear();
    renderFinishedSemaphore.clear();

    // acquire semaphore per frame-in-flight
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        presentCompleteSemaphore.emplace_back(vkDevice, vk::SemaphoreCreateInfo());
    }
    // present signal per swapchain image
    for (size_t i = 0; i < swapChainImageCount; i++) {
        renderFinishedSemaphore.emplace_back(vkDevice, vk::SemaphoreCreateInfo());
    }
}

FrameSync::OneTimeCommands FrameSync::oneTimeCommands()
{
    if (transferCommandBuffer.empty()) {
        return {.cmd = commandBuffers[0], .queue = device.graphicsQueue};
    }
    return {.cmd = transferCommandBuffer[0], .queue = device.transferQueue};
}

FrameSync::OneTimeCommands FrameSync::computeCommands()
{
    return {.cmd = commandBuffers[0], .queue = device.graphicsQueue};
}

void FrameSync::createCommandPool()
{
    logInfo("createCommandPool() started", "FrameSync");
    const vk::raii::Device& vkDevice = device.vkdevice;
    vk::CommandPoolCreateInfo poolInfo{.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                       .queueFamilyIndex = device.graphicsIndex};
    commandPool = vk::raii::CommandPool(vkDevice, poolInfo);
    setDebugName(vkDevice, commandPool, "GraphicsCommandPool");
    if (device.hasDedicatedTransferQueue()) {
        vk::CommandPoolCreateInfo transferPoolInfo{.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                                   .queueFamilyIndex = device.transferIndex};
        transferCommandPool = vk::raii::CommandPool(vkDevice, transferPoolInfo);
        setDebugName(vkDevice, transferCommandPool, "TransferCommandPool");
    }
    // the compute index falls back to the graphics family when no other family computes
    computeCommandPool = vk::raii::CommandPool(
        vkDevice, vk::CommandPoolCreateInfo{.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                            .queueFamilyIndex = device.computeIndex});
    setDebugName(vkDevice, computeCommandPool, "ComputeCommandPool");
}

void FrameSync::createCommandBuffers()
{
    logInfo("createCommandBuffers() started", "FrameSync");
    const vk::raii::Device& vkDevice = device.vkdevice;
    commandBuffers.clear();
    vk::CommandBufferAllocateInfo allocInfo{.commandPool = commandPool,
                                            .level = vk::CommandBufferLevel::ePrimary,
                                            .commandBufferCount = MAX_FRAMES_IN_FLIGHT};
    commandBuffers = vk::raii::CommandBuffers(vkDevice, allocInfo);
    for (size_t i = 0; i < commandBuffers.size(); ++i) {
        setDebugName(vkDevice, commandBuffers[i], std::format("GraphicsCommandBuffer_{}", i));
    }
    rayCommandBuffers.clear();
    rayCommandBuffers = vk::raii::CommandBuffers(vkDevice, allocInfo);
    for (size_t i = 0; i < rayCommandBuffers.size(); ++i) {
        setDebugName(vkDevice, rayCommandBuffers[i], std::format("RayCommandBuffer_{}", i));
    }
    presentCommandBuffers.clear();
    presentCommandBuffers = vk::raii::CommandBuffers(vkDevice, allocInfo);
    for (size_t i = 0; i < presentCommandBuffers.size(); ++i) {
        setDebugName(vkDevice, presentCommandBuffers[i], std::format("PresentCommandBuffer_{}", i));
    }
    if (device.hasDedicatedTransferQueue()) {
        vk::CommandBufferAllocateInfo transferAllocInfo{.commandPool = transferCommandPool,
                                                        .level = vk::CommandBufferLevel::ePrimary,
                                                        .commandBufferCount = MAX_FRAMES_IN_FLIGHT};
        transferCommandBuffer = vk::raii::CommandBuffers(vkDevice, transferAllocInfo);
        for (size_t i = 0; i < transferCommandBuffer.size(); ++i) {
            setDebugName(vkDevice, transferCommandBuffer[i], std::format("TransferCommandBuffer_{}", i));
        }
    }
    computeCommandBuffers = vk::raii::CommandBuffers(
        vkDevice, vk::CommandBufferAllocateInfo{.commandPool = computeCommandPool,
                                                .level = vk::CommandBufferLevel::ePrimary,
                                                .commandBufferCount = MAX_FRAMES_IN_FLIGHT});
    for (size_t i = 0; i < computeCommandBuffers.size(); ++i) {
        setDebugName(vkDevice, computeCommandBuffers[i], std::format("ComputeCommandBuffer_{}", i));
    }
    logInfo(std::format("Command buffers allocated: {}", commandBuffers.size()), "FrameSync");
    logInfo(std::format("Transfer command buffers allocated: {}", transferCommandBuffer.size()), "FrameSync");
}
