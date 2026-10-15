#pragma once

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include "../util/debug.hpp"
#include "vk_device.hpp"

class VkAllocator
{
public:
    VmaAllocator allocator;
    const vk::raii::PhysicalDevice& physicalDevice;
    const vk::raii::Device& device;
    const vk::raii::Instance& instance;

    explicit VkAllocator(Device& deviceWrapper) :
        physicalDevice(deviceWrapper.physicalDevice), device(deviceWrapper.vkdevice),
        instance(deviceWrapper.instance)
    {
        VmaVulkanFunctions vulkanFunctions = {};
        vulkanFunctions.vkGetInstanceProcAddr = &vkGetInstanceProcAddr;
        vulkanFunctions.vkGetDeviceProcAddr = &vkGetDeviceProcAddr;

        VmaAllocatorCreateInfo allocatorInfo = {};
        // EXT_memory_priority: VMA calls vkSetDeviceMemoryPriorityEXT (NVIDIA best practice).
        // Prefer suballocation; only force dedicated via per-allocation flags when size warrants it.
        allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT |
            VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT | VMA_ALLOCATOR_CREATE_KHR_BIND_MEMORY2_BIT |
            VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT | VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT |
            VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT | VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT;

        allocatorInfo.pVulkanFunctions = &vulkanFunctions;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_4;
        allocatorInfo.physicalDevice = *physicalDevice;
        allocatorInfo.device = *device;
        allocatorInfo.instance = *instance;

        if (vmaCreateAllocator(&allocatorInfo, &allocator) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create VMA allocator");
        }
    }

    ~VkAllocator() { vmaDestroyAllocator(allocator); }

    // dedicated allocs below this trigger BestPractices-vkBindBufferMemory-small-dedicated-allocation
    static constexpr vk::DeviceSize kMinDedicatedAllocationBytes = 1024 * 1024;

    void allocateImage(const vk::ImageCreateInfo& imageInfo, const VmaAllocationCreateInfo& allocInfo,
                       vk::raii::Image& image, VmaAllocation& allocation,
                       std::string_view allocationDebugBaseName = "ImageMemory") const
    {
        // mip 0 only, conservative bytes per texel
        constexpr vk::DeviceSize kApproxBytesPerTexel = 16;
        const vk::DeviceSize approxBytes = static_cast<vk::DeviceSize>(imageInfo.extent.width) *
            imageInfo.extent.height * std::max(imageInfo.arrayLayers, 1u) * kApproxBytesPerTexel;
        VmaAllocationCreateInfo resolved = resolveAllocationCreateInfo(allocInfo, approxBytes, true);
        VkImage rawImage{};
        if (vmaCreateImage(allocator, imageInfo, &resolved, &rawImage, &allocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate image with VMA");
        }
        image = vk::raii::Image(device, rawImage);
        nameAllocation(allocation, allocationDebugBaseName);
    }

    // optimal-tiling single-layer 2D image, exclusive sharing (only the graphics queue touches images)
    void createImage2D(uint32_t width, uint32_t height, uint32_t mipLevels, vk::SampleCountFlagBits samples,
                       vk::Format format, vk::ImageUsageFlags usage, vk::raii::Image& image,
                       VmaAllocation& allocation, std::string_view allocationDebugBaseName) const
    {
        const vk::ImageCreateInfo imageInfo{.imageType = vk::ImageType::e2D,
                                            .format = format,
                                            .extent = {width, height, 1},
                                            .mipLevels = mipLevels,
                                            .arrayLayers = 1,
                                            .samples = samples,
                                            .tiling = vk::ImageTiling::eOptimal,
                                            .usage = usage,
                                            .sharingMode = vk::SharingMode::eExclusive};
        constexpr vk::ImageUsageFlags kAttachmentUsage = vk::ImageUsageFlagBits::eColorAttachment |
            vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eTransientAttachment;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        // attachments demote last under memory pressure (NVIDIA memory priority best practice)
        allocInfo.priority = (usage & kAttachmentUsage) ? 1.0f : 0.9f;
        allocateImage(imageInfo, allocInfo, image, allocation, allocationDebugBaseName);
    }

    VkAllocator(const VkAllocator&) = delete;
    VkAllocator(VkAllocator&&) = delete;
    VkAllocator& operator=(const VkAllocator&) = delete;
    VkAllocator& operator=(VkAllocator&&) = delete;

private:
    // tiny dedicated requests sub-allocate instead; always set a priority for VK_EXT_memory_priority
    static VmaAllocationCreateInfo resolveAllocationCreateInfo(const VmaAllocationCreateInfo& in,
                                                               vk::DeviceSize sizeBytes,
                                                               bool isImage)
    {
        VmaAllocationCreateInfo out = in;
        if ((out.flags & VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT) != 0 &&
            sizeBytes < kMinDedicatedAllocationBytes) {
            out.flags &= ~VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        }
        // images demote last under memory pressure
        if (out.priority <= 0.0f) {
            if (isImage) {
                out.priority = 1.0f;
            } else if ((out.flags & VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) != 0 ||
                       (out.flags & VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT) != 0) {
                out.priority = 0.25f; // staging / host-visible
            } else {
                out.priority = 0.75f; // device-local buffers
            }
        }
        return out;
    }

    void nameAllocation(VmaAllocation allocation, std::string_view baseName) const
    {
        if (allocation == nullptr) {
            return;
        }

        static std::atomic<uint64_t> allocationCounter = 0;
        const uint64_t id = allocationCounter.fetch_add(1, std::memory_order_relaxed);
        const std::string uniqueName = std::format("{}_{}", baseName.empty() ? "DeviceMemory" : baseName, id);

        vmaSetAllocationName(allocator, allocation, uniqueName.c_str());

        VmaAllocationInfo allocInfo{};
        vmaGetAllocationInfo(allocator, allocation, &allocInfo);
        if (allocInfo.deviceMemory != VK_NULL_HANDLE) {
            const uint64_t memoryHandle = reinterpret_cast<uint64_t>(allocInfo.deviceMemory);
            static std::mutex namedMemoryHandlesMutex;
            static std::unordered_set<uint64_t> namedMemoryHandles;

            bool shouldNameMemory = false;
            {
                const std::lock_guard<std::mutex> lock(namedMemoryHandlesMutex);
                shouldNameMemory = namedMemoryHandles.insert(memoryHandle).second;
            }

            if (shouldNameMemory) {
                const std::string memoryName = std::format("{}_DeviceMemory", uniqueName);
                setDebugName(device, vk::DeviceMemory(allocInfo.deviceMemory), memoryName);
            }
        }
    }
};
