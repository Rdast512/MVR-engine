#pragma once

#include <cstddef>
#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_raii.hpp>

// tracy memory tracking; requires matched pointers for alloc/free
void tracyResourceAlloc(const void* ptr, size_t size, const char* poolName);
void tracyResourceFree(const void* ptr, const char* poolName);

class VkTracyContext
{
public:
    VkTracyContext() = default;
    ~VkTracyContext();

    VkTracyContext(const VkTracyContext&) = delete;
    VkTracyContext& operator=(const VkTracyContext&) = delete;

    void init(const vk::raii::Instance& instance, const vk::raii::PhysicalDevice& physicalDevice,
              const vk::raii::Device& device, const vk::raii::Queue& queue,
              const vk::raii::CommandBuffer& setupCommandBuffer, const char* contextName = "Graphics Queue");

    void collect(const vk::raii::CommandBuffer& commandBuffer) const;
    void shutdown();

    [[nodiscard]] bool active() const noexcept;

#ifdef TRACY_ENABLE
    [[nodiscard]] TracyVkCtx handle() const noexcept { return context; }
#endif

private:
#ifdef TRACY_ENABLE
    TracyVkCtx context = nullptr;
#endif
};
