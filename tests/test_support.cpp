#include "test_support.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace test
{
    namespace
    {
        int failures = 0;

        bool hasExtension(VkPhysicalDevice gpu, const char* name)
        {
            uint32_t count = 0;
            vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr);
            std::vector<VkExtensionProperties> props(count);
            vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, props.data());
            for (const VkExtensionProperties& prop : props) {
                if (std::strcmp(prop.extensionName, name) == 0) {
                    return true;
                }
            }
            return false;
        }

        VkPhysicalDevice findGpu(VkInstance instance, uint32_t vendorId)
        {
            uint32_t count = 0;
            vkEnumeratePhysicalDevices(instance, &count, nullptr);
            std::vector<VkPhysicalDevice> gpus(count);
            vkEnumeratePhysicalDevices(instance, &count, gpus.data());
            for (VkPhysicalDevice gpu : gpus) {
                VkPhysicalDeviceProperties props{};
                vkGetPhysicalDeviceProperties(gpu, &props);
                if (vendorId == 0 || props.vendorID == vendorId) {
                    return gpu;
                }
            }
            return VK_NULL_HANDLE;
        }

        uint32_t findQueueFamily(VkPhysicalDevice gpu)
        {
            uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families.data());
            for (uint32_t i = 0; i < count; ++i) {
                if (families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
                    return i;
                }
            }
            return 0;
        }
    }

    bool check(bool condition, const char* what)
    {
        std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", what);
        if (!condition) {
            ++failures;
        }
        return condition;
    }

    int finish()
    {
        std::printf("%s (%d failures)\n", failures == 0 ? "OK" : "FAILED", failures);
        return failures == 0 ? 0 : 1;
    }

    VulkanContext::~VulkanContext()
    {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            vkDestroyCommandPool(device, commandPool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
        }
    }

    ContextStatus VulkanContext::create(std::span<const char* const> instanceExtensions,
                                        std::span<const char* const> deviceExtensions, uint32_t vendorId)
    {
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.apiVersion = VK_API_VERSION_1_4;
        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &appInfo;
        instanceInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
        instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();
        if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) {
            std::printf("[SKIP] no Vulkan instance\n");
            return ContextStatus::Unavailable;
        }

        gpu = findGpu(instance, vendorId);
        if (gpu == VK_NULL_HANDLE) {
            std::printf("[SKIP] no matching GPU\n");
            return ContextStatus::Unavailable;
        }
        bool hasAllExtensions = true;
        for (const char* name : deviceExtensions) {
            hasAllExtensions &= check(hasExtension(gpu, name), name);
        }
        if (!hasAllExtensions) {
            return ContextStatus::Failed;
        }

        VkPhysicalDeviceVulkan14Features features14{};
        features14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features13.pNext = &features14;
        VkPhysicalDeviceVulkan12Features features12{};
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        features12.pNext = &features13;
        VkPhysicalDeviceVulkan11Features features11{};
        features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        features11.pNext = &features12;
        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &features11;
        vkGetPhysicalDeviceFeatures2(gpu, &features);

        queueFamily = findQueueFamily(gpu);
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.pNext = &features;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
        deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
        if (!check(vkCreateDevice(gpu, &deviceInfo, nullptr, &device) == VK_SUCCESS, "Vulkan device created")) {
            device = VK_NULL_HANDLE;
            return ContextStatus::Failed;
        }
        vkGetDeviceQueue(device, queueFamily, 0, &queue);

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = queueFamily;
        vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);
        return ContextStatus::Ready;
    }

    VkCommandBuffer VulkanContext::beginCommands() const
    {
        VkCommandBufferAllocateInfo cmdInfo{};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdInfo.commandPool = commandPool;
        cmdInfo.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(device, &cmdInfo, &cmd);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);
        return cmd;
    }

    bool VulkanContext::submit(VkCommandBuffer cmd) const
    {
        vkEndCommandBuffer(cmd);
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        return vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE) == VK_SUCCESS;
    }

    void VulkanContext::waitIdle() const
    {
        vkQueueWaitIdle(queue);
    }
}
