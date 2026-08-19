#pragma once
#include <vulkan/vulkan_raii.hpp>
#include "vma/vk_mem_alloc.h"

#include <optional>
#include <string_view>
#include <vector>

// Inserts a pipeline barrier to transition an image between layouts.
// Pure utility — does not depend on any manager class.
void transitionImageLayout(
    vk::raii::CommandBuffer* commandBuffer,
    vk::Image image,
    uint32_t mipLevels,
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

// Creates a VkBuffer with VMA allocation.  Pure utility — does not depend on any manager class.
void createBuffer(
    vk::DeviceSize size,
    vk::BufferUsageFlags2 usage,
    vk::MemoryPropertyFlags properties,
    vk::raii::Buffer& buffer,
    VmaAllocation& bufferMemory,
    VmaAllocator allocator,
    const vk::raii::Device& device,
    const std::vector<uint32_t>& queueFamilyIndices,
    std::string_view memoryDebugBaseName = "ResourceBufferMemory",
    VmaAllocationCreateFlags extraAllocationFlags = 0);
