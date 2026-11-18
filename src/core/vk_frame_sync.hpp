#pragma once
#include "vk_device.hpp"

#include <cstdint>
#include <span>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

// fences, semaphores and command buffers of the frame slots, plus the command buffer for load-time work
class FrameSync {
public:
    // ray segments per frame slot: four engine segments around NRC BeginFrame, NRC QueryAndTrain and DLSS-RR
    static constexpr uint32_t kRaySegmentsPerFrame = 7;

    // command buffer and the queue it is submitted to
    struct OneTimeCommands {
        vk::raii::CommandBuffer& cmd;
        const vk::raii::Queue& queue;
    };

    explicit FrameSync(const Device& device);

    FrameSync(const FrameSync&) = delete;
    FrameSync& operator=(const FrameSync&) = delete;
    FrameSync(FrameSync&&) = delete;
    FrameSync& operator=(FrameSync&&) = delete;

    // device idle: recreates every binary semaphore; one present signal per swapchain image. Fences live as long as
    // FrameSync
    void recreate(uint32_t swapChainImageCount);
    // load-time copies: the transfer pool when a transfer family exists, frame slot 0's buffer otherwise
    [[nodiscard]] OneTimeCommands oneTimeCommands();
    // load-time acceleration-structure builds: frame slot 0's buffer on the graphics queue (transfer cannot build)
    [[nodiscard]] OneTimeCommands computeCommands();
    [[nodiscard]] std::span<vk::raii::CommandBuffer> raySegments(uint32_t frameSlot);

    // Acquire: one per frame-in-flight. Present signal: one per swapchain image.
    std::vector<vk::raii::Semaphore> presentCompleteSemaphore;
    std::vector<vk::raii::Semaphore> renderFinishedSemaphore;
    std::vector<vk::raii::Fence> inFlightFences;
    // async compute work of the frame slots signals frame numbers; not recreated with the swapchain
    vk::raii::Semaphore computeTimeline = nullptr;
    // the ray passes of the frame slots signal frame numbers: the next in-place PTLAS update waits for them
    vk::raii::Semaphore rayTimeline = nullptr;
    // declared before the buffers: destroyed after them
    vk::raii::CommandPool commandPool = nullptr;
    vk::raii::CommandPool transferCommandPool = nullptr;
    vk::raii::CommandPool computeCommandPool = nullptr;
    // per frame slot: the frame's work, submitted before the swapchain image is acquired
    std::vector<vk::raii::CommandBuffer> commandBuffers;
    // kRaySegmentsPerFrame per frame slot, slot-major: the passes that trace rays, submitted in order after
    // commandBuffers with a wait on this frame's computeTimeline value (the TLAS build)
    std::vector<vk::raii::CommandBuffer> rayCommandBuffers;
    // per frame slot: present pass + UI, the only work that waits for the acquired image
    std::vector<vk::raii::CommandBuffer> presentCommandBuffers;
    std::vector<vk::raii::CommandBuffer> transferCommandBuffer;
    // per frame slot: async compute work (RT instances + TLAS), submitted to Device::computeQueue
    std::vector<vk::raii::CommandBuffer> computeCommandBuffers;

private:
    void createCommandPool();
    void createCommandBuffers();

    const Device& device;
};
