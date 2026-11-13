#include "vk_frame_sync.hpp"
#include "Constants.h"
#include "static_headers/logger.hpp"
#include "util/debug.hpp"

#include <format>

FrameSync::FrameSync(const Device& device) : device(device)
{
    createCommandPool();
    createCommandBuffers();
}

void FrameSync::recreate(uint32_t swapChainImageCount)
{
    logInfo("recreate() started", "FrameSync");
    const vk::raii::Device& vkDevice = device.vkdevice;
    presentCompleteSemaphore.clear();
    renderFinishedSemaphore.clear();
    inFlightFences.clear();

    // acquire semaphore and fence per frame-in-flight
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        presentCompleteSemaphore.emplace_back(vkDevice, vk::SemaphoreCreateInfo());
        inFlightFences.emplace_back(vkDevice, vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled});
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
    logInfo(std::format("Command buffers allocated: {}", commandBuffers.size()), "FrameSync");
    logInfo(std::format("Transfer command buffers allocated: {}", transferCommandBuffer.size()), "FrameSync");
}
