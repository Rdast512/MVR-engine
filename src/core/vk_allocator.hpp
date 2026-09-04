#pragma once

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

        // Get the dynamic dispatcher for proper Vulkan function access
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

    // Best-practices threshold: dedicated allocs below this size trigger
    // BestPractices-vkBindBufferMemory-small-dedicated-allocation (typically 1 MiB).
    static constexpr vk::DeviceSize kMinDedicatedAllocationBytes = 1024 * 1024;

    void alocateBuffer(const vk::BufferCreateInfo& bufferInfo, const VmaAllocationCreateInfo& allocInfo,
                       vk::raii::Buffer& buffer, VmaAllocation& allocation,
                       std::string_view allocationDebugBaseName = "BufferMemory") const
    {
        VmaAllocationCreateInfo resolved = resolveAllocationCreateInfo(allocInfo, bufferInfo.size, false);
        VkBuffer rawBuffer{};
        if (vmaCreateBuffer(allocator, bufferInfo, &resolved, &rawBuffer, &allocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate buffer with VMA");
        }
        buffer = vk::raii::Buffer(device, rawBuffer);
        nameAllocation(allocation, allocationDebugBaseName);
    }

    void alocateImage(const vk::ImageCreateInfo& imageInfo, const VmaAllocationCreateInfo& allocInfo,
                      vk::raii::Image& image, VmaAllocation& allocation,
                      std::string_view allocationDebugBaseName = "ImageMemory") const
    {
        // Approximate image size for dedicated-threshold policy (mip 0 only; conservative).
        const vk::DeviceSize approxBytes = static_cast<vk::DeviceSize>(imageInfo.extent.width) *
            static_cast<vk::DeviceSize>(imageInfo.extent.height) *
            static_cast<vk::DeviceSize>(std::max(imageInfo.arrayLayers, 1u)) * 16u;
        VmaAllocationCreateInfo resolved = resolveAllocationCreateInfo(allocInfo, approxBytes, true);
        VkImage rawImage{};
        if (vmaCreateImage(allocator, imageInfo, &resolved, &rawImage, &allocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate image with VMA");
        }
        image = vk::raii::Image(device, rawImage);
        nameAllocation(allocation, allocationDebugBaseName);
    }

    // Non-copyable, non-movable: manages a unique VMA allocator handle.
    VkAllocator(const VkAllocator&) = delete;
    VkAllocator(VkAllocator&&) = delete;
    VkAllocator& operator=(const VkAllocator&) = delete;
    VkAllocator& operator=(VkAllocator&&) = delete;

private:
    // Drop tiny dedicated requests (sub-allocate instead) and ensure a priority is set
    // so VK_EXT_memory_priority / NVIDIA best practices are satisfied.
    static VmaAllocationCreateInfo resolveAllocationCreateInfo(const VmaAllocationCreateInfo& in,
                                                               vk::DeviceSize sizeBytes,
                                                               bool isImage)
    {
        VmaAllocationCreateInfo out = in;
        if ((out.flags & VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT) != 0 &&
            sizeBytes < kMinDedicatedAllocationBytes) {
            out.flags &= ~VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        }
        // VMA default priority is 0.5; still set explicitly so priorities are intentional.
        // Attachments / GPU-written images keep highest priority for OS demotion order.
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
