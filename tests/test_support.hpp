#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>

namespace test
{
    // ctest SKIP_RETURN_CODE
    constexpr int kSkipExitCode = 77;

    // prints [PASS]/[FAIL] and counts failures; returns condition
    bool check(bool condition, const char* what);
    // prints the summary; returns the process exit code
    int finish();

    enum class ContextStatus
    {
        Ready,
        Unavailable, // no instance or no matching GPU: skip
        Failed,
    };

    // instance + device with every supported core 1.1-1.4 feature and one graphics/compute queue
    struct VulkanContext
    {
        VulkanContext() = default;
        ~VulkanContext();

        VulkanContext(const VulkanContext&) = delete;
        VulkanContext& operator=(const VulkanContext&) = delete;
        VulkanContext(VulkanContext&&) = delete;
        VulkanContext& operator=(VulkanContext&&) = delete;

        // vendorId 0 accepts any GPU
        [[nodiscard]] ContextStatus create(std::span<const char* const> instanceExtensions,
                                           std::span<const char* const> deviceExtensions, uint32_t vendorId = 0);

        // one-time command buffer from the context pool, already begun
        [[nodiscard]] VkCommandBuffer beginCommands() const;
        // ends and submits without waiting
        [[nodiscard]] bool submit(VkCommandBuffer cmd) const;
        void waitIdle() const;

        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice gpu = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t queueFamily = 0;
        VkCommandPool commandPool = VK_NULL_HANDLE;
    };
}
