#pragma once
#include <vulkan/vulkan_raii.hpp>
#include "vma/vk_mem_alloc.h"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Persistently mapped host-visible buffer (VMA_ALLOCATION_CREATE_MAPPED_BIT) with a device address
struct HostBuffer {
    vk::raii::Buffer buffer = nullptr;
    VmaAllocation memory = nullptr;
    void* mapped = nullptr;
    vk::DeviceAddress address = 0;
};

// Inserts a pipeline barrier to transition an image between layouts.
// Pure utility — does not depend on any manager class.
void transitionImageLayout(
    vk::raii::CommandBuffer* commandBuffer,
    vk::Image image,
    vk::ImageLayout oldLayout,
    vk::ImageLayout newLayout,
    const vk::ImageSubresourceRange& subresourceRange = {
        vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1
    },
    uint32_t srcQueueFamily = VK_QUEUE_FAMILY_IGNORED,
    uint32_t dstQueueFamily = VK_QUEUE_FAMILY_IGNORED,
    std::optional<vk::PipelineStageFlags2> srcStageMaskOverride = std::nullopt,
    std::optional<vk::PipelineStageFlags2> dstStageMaskOverride = std::nullopt,
    std::optional<vk::AccessFlags2> srcAccessMaskOverride = std::nullopt,
    std::optional<vk::AccessFlags2> dstAccessMaskOverride = std::nullopt);

// Creates a VkBuffer with VMA allocation, named before its memory is bound.  Pure utility — does not depend on any
// manager class. minAlignment: lower bound on the buffer's start, on top of its memory requirements; 0 for none
void createBuffer(
    vk::DeviceSize size,
    vk::BufferUsageFlags2 usage,
    vk::MemoryPropertyFlags properties,
    vk::raii::Buffer& buffer,
    VmaAllocation& bufferMemory,
    VmaAllocator allocator,
    const vk::raii::Device& device,
    const std::vector<uint32_t>& queueFamilyIndices,
    std::string_view debugName,
    VmaAllocationCreateFlags extraAllocationFlags = 0,
    vk::DeviceSize minAlignment = 0);

// Ends a one-time command buffer, submits it and blocks until the queue drains.
void submitAndWait(vk::raii::CommandBuffer& commandBuffer, const vk::raii::Queue& queue);

// Frees a VMA-backed buffer / image; no-op when allocation is null.
// Explicit vmaMapMemory mappings must be unmapped first (VMA asserts on m_MapCount).
// tracyName must match the pool used at alloc time; nullptr skips Tracy tracking.
void destroyVmaBuffer(VmaAllocator allocator, vk::raii::Buffer& buffer, VmaAllocation& allocation,
                      const char* tracyName = nullptr);
void destroyVmaImage(VmaAllocator allocator, vk::raii::Image& image, VmaAllocation& allocation,
                     const char* tracyName = nullptr);

// uniform + storage usage, host-visible coherent; tracyName nullptr skips Tracy tracking
// extraUsage adds to uniform | storage | device address, e.g. acceleration-structure build input
[[nodiscard]] HostBuffer createHostBuffer(vk::DeviceSize size, VmaAllocator allocator, const vk::raii::Device& device,
                                          const std::vector<uint32_t>& queueFamilyIndices, std::string_view debugName,
                                          const char* tracyName, vk::BufferUsageFlags2 extraUsage = {});
// VMA unmaps on destroy; no-op when never created
void destroyHostBuffer(VmaAllocator allocator, HostBuffer& target, const char* tracyName);

// GPU-written counters the host reads after the slot's fence. Shaders write the device-local buffer at address; one
// copy per frame lands in host-cached readback. GPU atomics into host-cached (system) memory cross PCIe
// (considerations C-M85-1)
struct CounterReadback {
    vk::raii::Buffer counters = nullptr;
    VmaAllocation countersMemory = nullptr;
    vk::DeviceAddress address = 0;
    vk::DeviceSize size = 0;
    HostBuffer readback;
};

[[nodiscard]] CounterReadback createCounterReadback(vk::DeviceSize size, VmaAllocator allocator,
                                                    const vk::raii::Device& device,
                                                    const std::vector<uint32_t>& queueFamilyIndices,
                                                    std::string_view debugName);
void destroyCounterReadback(VmaAllocator allocator, CounterReadback& target);
// zeroes the counters, ordered before this frame's compute writers
void recordCounterReset(const vk::raii::CommandBuffer& cmd, const CounterReadback& target);
// after the compute writers: copies the counters into readback, visible to the host after the slot's fence
void recordCounterReadback(const vk::raii::CommandBuffer& cmd, const CounterReadback& target);

// first candidate whose tiling features include features; throws when none does
[[nodiscard]] vk::Format findSupportedFormat(const vk::raii::PhysicalDevice& physicalDevice,
                                             std::span<const vk::Format> candidates, vk::ImageTiling tiling,
                                             vk::FormatFeatureFlags features);
[[nodiscard]] vk::raii::ImageView createImageView(const vk::raii::Device& device, vk::Image image, vk::Format format,
                                                  vk::ImageAspectFlags aspectFlags, uint32_t mipLevels);
