#include "vk_utils.hpp"

#include "debug.hpp"
#include "vk_tracy.hpp"
#include "../static_headers/logger.hpp"

#include <atomic>
#include <format>
#include <string>
#include <stdexcept>

void transitionImageLayout(
    vk::raii::CommandBuffer* commandBuffer,
    vk::Image image,
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
    const vk::DependencyInfo dependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
    commandBuffer->pipelineBarrier2(dependencyInfo);
}

void createBuffer(vk::DeviceSize size, vk::BufferUsageFlags2 usage, vk::MemoryPropertyFlags properties,
                  vk::raii::Buffer& buffer, VmaAllocation& bufferMemory, VmaAllocator allocator,
                  const vk::raii::Device& device, const std::vector<uint32_t>& queueFamilyIndices,
                  std::string_view debugName, VmaAllocationCreateFlags extraAllocationFlags,
                  vk::DeviceSize minAlignment)
{
    ZoneScopedN("createBuffer");
    logInfo("createBuffer() started", "Utils");

    // maintenance5: usage lives in BufferUsageFlags2CreateInfo
    const vk::BufferUsageFlags2CreateInfo usage2{.usage = usage};
    // concurrent sharing needs at least two distinct families (VUID-VkBufferCreateInfo-sharingMode-00914)
    const bool isShared = queueFamilyIndices.size() > 1;
    vk::BufferCreateInfo bufferInfo{
        .pNext = &usage2,
        .size = size,
        .sharingMode = isShared ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive,
        .queueFamilyIndexCount = isShared ? static_cast<uint32_t>(queueFamilyIndices.size()) : 0u,
        .pQueueFamilyIndices = isShared ? queueFamilyIndices.data() : nullptr};

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.preferredFlags = static_cast<VkMemoryPropertyFlags>(properties);

    if (properties & vk::MemoryPropertyFlagBits::eHostVisible) {
        const bool isHostCached = static_cast<bool>(properties & vk::MemoryPropertyFlagBits::eHostCached);
        allocInfo.usage = isHostCached ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO;
        allocInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        if (properties & vk::MemoryPropertyFlagBits::eHostCoherent) {
            allocInfo.requiredFlags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        }
        // cached readback buffers must avoid uncached ReBAR write-combined VRAM (100x slower CPU reads)
        allocInfo.flags = isHostCached ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                                       : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
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

    // bind after naming so bind-time validation messages carry the name
    allocInfo.flags |= VMA_ALLOCATION_CREATE_DONT_BIND_BIT;
    VkBuffer rawBuffer{};
    if (vmaCreateBufferWithAlignment(allocator, &static_cast<const VkBufferCreateInfo&>(bufferInfo), &allocInfo,
                                     minAlignment, &rawBuffer, &bufferMemory, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate buffer with VMA");
    }
    buffer = vk::raii::Buffer(device, rawBuffer);
    setDebugName(device, buffer, debugName);
    if (vmaBindBufferMemory(allocator, bufferMemory, rawBuffer) != VK_SUCCESS) {
        buffer = nullptr;
        vmaFreeMemory(allocator, bufferMemory);
        bufferMemory = nullptr;
        throw std::runtime_error(std::format("Failed to bind memory for buffer {}", debugName));
    }

    static std::atomic<uint64_t> allocationCounter = 0;
    const uint64_t id = allocationCounter.fetch_add(1, std::memory_order_relaxed);
    const std::string uniqueName = std::format("{}Memory_{}", debugName, id);
    vmaSetAllocationName(allocator, bufferMemory, uniqueName.c_str());
}

void submitAndWait(vk::raii::CommandBuffer& commandBuffer, const vk::raii::Queue& queue)
{
    ZoneScopedN("submitAndWait");
    commandBuffer.end();
    // sync2 submit avoids WARNING-deprecation-sync2 / legacy QueueSubmit
    const vk::CommandBufferSubmitInfo commandBufferInfo{.commandBuffer = *commandBuffer};
    const vk::SubmitInfo2 submitInfo{.commandBufferInfoCount = 1, .pCommandBufferInfos = &commandBufferInfo};
    queue.submit2(submitInfo, nullptr);
    queue.waitIdle();
}

HostBuffer createHostBuffer(vk::DeviceSize size, VmaAllocator allocator, const vk::raii::Device& device,
                            const std::vector<uint32_t>& queueFamilyIndices, std::string_view debugName,
                            const char* tracyName, vk::BufferUsageFlags2 extraUsage)
{
    HostBuffer target;
    createBuffer(size,
                 vk::BufferUsageFlagBits2::eUniformBuffer | vk::BufferUsageFlagBits2::eStorageBuffer |
                     vk::BufferUsageFlagBits2::eShaderDeviceAddress | extraUsage,
                 vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, target.buffer,
                 target.memory, allocator, device, queueFamilyIndices, debugName, VMA_ALLOCATION_CREATE_MAPPED_BIT);
    VmaAllocationInfo allocationInfo{};
    vmaGetAllocationInfo(allocator, target.memory, &allocationInfo);
    target.mapped = allocationInfo.pMappedData;
    target.address = device.getBufferAddress({.buffer = *target.buffer});
    tracyResourceAlloc(static_cast<VkBuffer>(*target.buffer), static_cast<size_t>(size), tracyName);
    return target;
}

void destroyHostBuffer(VmaAllocator allocator, HostBuffer& target, const char* tracyName)
{
    // persistently mapped (MAPPED_BIT): VMA unmaps on destroy
    destroyVmaBuffer(allocator, target.buffer, target.memory, tracyName);
    target.mapped = nullptr;
    target.address = 0;
}

vk::Format findSupportedFormat(const vk::raii::PhysicalDevice& physicalDevice, std::span<const vk::Format> candidates,
                               vk::ImageTiling tiling, vk::FormatFeatureFlags features)
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

vk::raii::ImageView createImageView(const vk::raii::Device& device, vk::Image image, vk::Format format,
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

void destroyVmaBuffer(VmaAllocator allocator, vk::raii::Buffer& buffer, VmaAllocation& allocation,
                      const char* tracyName)
{
    if (allocation == nullptr) {
        return;
    }
    // release first: raii destructor would vkDestroyBuffer the handle VMA frees
    VkBuffer raw = buffer.release();
    if (tracyName != nullptr) {
        tracyResourceFree(raw, tracyName);
    }
    vmaDestroyBuffer(allocator, raw, allocation);
    allocation = nullptr;
}

void destroyVmaImage(VmaAllocator allocator, vk::raii::Image& image, VmaAllocation& allocation,
                     const char* tracyName)
{
    if (allocation == nullptr) {
        return;
    }
    VkImage raw = image.release();
    if (tracyName != nullptr) {
        tracyResourceFree(raw, tracyName);
    }
    vmaDestroyImage(allocator, raw, allocation);
    allocation = nullptr;
}
