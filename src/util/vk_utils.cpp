#include "vk_utils.hpp"

#include "vk_tracy.hpp"
#include "../static_headers/logger.hpp"

#include <atomic>
#include <format>
#include <stdexcept>


void transitionImageLayout(
    vk::raii::CommandBuffer* commandBuffer,
    vk::Image image,
    uint32_t mipLevels,
    vk::ImageLayout oldLayout,
    vk::ImageLayout newLayout,
    const vk::ImageSubresourceRange& subresourceRange,
    uint32_t srcQueueFamily,
    uint32_t dstQueueFamily,
    std::optional<vk::PipelineStageFlags2> srcStageMaskOverride,
    std::optional<vk::PipelineStageFlags2> dstStageMaskOverride,
    std::optional<vk::AccessFlags2> srcAccessMaskOverride,
    std::optional<vk::AccessFlags2> dstAccessMaskOverride)
{
    ZoneScopedN("transitionImageLayout started");
    vk::ImageMemoryBarrier2 barrier{.srcQueueFamilyIndex = srcQueueFamily,
                                    .dstQueueFamilyIndex = dstQueueFamily,
                                    .image = image,
                                    .subresourceRange = subresourceRange};
    vk::PipelineStageFlags2 srcStage = srcStageMaskOverride.value_or(vk::PipelineStageFlagBits2::eNone);
    vk::PipelineStageFlags2 dstStage = dstStageMaskOverride.value_or(vk::PipelineStageFlagBits2::eNone);
    vk::AccessFlags2 srcAccess = srcAccessMaskOverride.value_or(vk::AccessFlagBits2::eNone);
    vk::AccessFlags2 dstAccess = dstAccessMaskOverride.value_or(vk::AccessFlagBits2::eNone);
    if (srcStage == vk::PipelineStageFlagBits2::eNone && dstStage == vk::PipelineStageFlagBits2::eNone) {
        if (oldLayout == vk::ImageLayout::eUndefined && newLayout == vk::ImageLayout::eTransferDstOptimal) {
            srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
            dstStage = vk::PipelineStageFlagBits2::eTransfer;
            srcAccess = vk::AccessFlagBits2::eNone;
            dstAccess = vk::AccessFlagBits2::eTransferWrite;
        } else if (oldLayout == vk::ImageLayout::eTransferDstOptimal &&
                   newLayout == vk::ImageLayout::eShaderReadOnlyOptimal) {
            srcStage = vk::PipelineStageFlagBits2::eTransfer;
            dstStage = vk::PipelineStageFlagBits2::eFragmentShader;
            srcAccess = vk::AccessFlagBits2::eTransferWrite;
            dstAccess = vk::AccessFlagBits2::eShaderRead;
                   } else if (oldLayout == vk::ImageLayout::eUndefined &&
                              newLayout == vk::ImageLayout::eDepthStencilAttachmentOptimal) {
                       srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
                       dstStage = vk::PipelineStageFlagBits2::eEarlyFragmentTests;
                       srcAccess = vk::AccessFlagBits2::eNone;
                       dstAccess =
                           vk::AccessFlagBits2::eDepthStencilAttachmentWrite | vk::AccessFlagBits2::eDepthStencilAttachmentRead;
                              } else if (oldLayout == vk::ImageLayout::eUndefined && newLayout == vk::ImageLayout::eColorAttachmentOptimal) {
                                  srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
                                  dstStage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
                                  srcAccess = vk::AccessFlagBits2::eNone;
                                  dstAccess = vk::AccessFlagBits2::eColorAttachmentWrite;
                              } else {
                                  throw std::invalid_argument("unsupported layout transition");
                              }
    }
    barrier.srcStageMask = srcStage;
    barrier.dstStageMask = dstStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.subresourceRange.levelCount = mipLevels;
    vk::DependencyInfo dependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
    commandBuffer->pipelineBarrier2(dependencyInfo);
}

void createBuffer(vk::DeviceSize size, vk::BufferUsageFlags2 usage, vk::MemoryPropertyFlags properties,
                  vk::raii::Buffer& buffer, VmaAllocation& bufferMemory, VmaAllocator allocator,
                  const vk::raii::Device& device, const std::vector<uint32_t>& queueFamilyIndices,
                  std::string_view memoryDebugBaseName, VmaAllocationCreateFlags extraAllocationFlags)
{
    ZoneScopedN("createBuffer");
    log_info("createBuffer() started", "Utils");

    // maintenance5: usage lives in BufferUsageFlags2CreateInfo; VkBufferCreateInfo::usage is ignored.
    const vk::BufferUsageFlags2CreateInfo usage2{.usage = usage};
    vk::BufferCreateInfo bufferInfo{.pNext = &usage2,
                                    .size = size,
                                    .sharingMode = vk::SharingMode::eConcurrent,
                                    .queueFamilyIndexCount = static_cast<uint32_t>(queueFamilyIndices.size()),
                                    .pQueueFamilyIndices = queueFamilyIndices.data()};

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.preferredFlags = static_cast<VkMemoryPropertyFlags>(properties);

    if (properties & vk::MemoryPropertyFlagBits::eHostVisible) {
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        allocInfo.priority = 0.25f; // staging / CPU-visible (NVIDIA memory priority)
    } else {
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (properties & vk::MemoryPropertyFlagBits::eDeviceLocal) {
            allocInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        }
        allocInfo.priority = 0.75f;
    }
    allocInfo.flags |= extraAllocationFlags;
    // BestPractices-vkBindBufferMemory-small-dedicated-allocation: do not force
    // dedicated blocks for small buffers (validation threshold is typically 1 MiB).
    constexpr vk::DeviceSize kMinDedicatedAllocationBytes = 1024 * 1024;
    if ((allocInfo.flags & VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT) != 0 && size < kMinDedicatedAllocationBytes) {
        allocInfo.flags &= ~VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    }

    VkBuffer rawBuffer{};
    if (vmaCreateBuffer(allocator, &static_cast<const VkBufferCreateInfo&>(bufferInfo), &allocInfo, &rawBuffer,
                        &bufferMemory, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate buffer with VMA");
    }
    buffer = vk::raii::Buffer(device, rawBuffer);

    static std::atomic<uint64_t> allocationCounter = 0;
    const uint64_t id = allocationCounter.fetch_add(1, std::memory_order_relaxed);
    const std::string uniqueName =
        std::format("{}_{}", memoryDebugBaseName.empty() ? "DeviceMemory" : memoryDebugBaseName, id);
    vmaSetAllocationName(allocator, bufferMemory, uniqueName.c_str());
}
