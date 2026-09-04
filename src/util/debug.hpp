#pragma once

#include <vulkan/vulkan_raii.hpp>
#include <string_view>

// Sets a debug name via VK_EXT_debug_utils if available (no-op otherwise).
void setDebugName(const vk::raii::Device &device, const vk::raii::Image &image, std::string_view name);
void setDebugName(const vk::raii::Device &device, const vk::raii::Buffer &buffer, std::string_view name);
void setDebugName(const vk::raii::Device &device, const vk::raii::ImageView &imageView, std::string_view name);
void setDebugName(const vk::raii::Device &device, vk::Image image, std::string_view name);
void setDebugName(const vk::raii::Device &device, vk::ImageView imageView, std::string_view name);
void setDebugName(const vk::raii::Device &device, vk::DeviceMemory memory, std::string_view name);
void setDebugName(const vk::raii::Device &device, const vk::raii::Device &object, std::string_view name);
void setDebugName(const vk::raii::Device &device, const vk::raii::PhysicalDevice &object, std::string_view name);
void setDebugName(const vk::raii::Device &device, const vk::raii::Queue &queue, std::string_view name);
void setDebugName(const vk::raii::Device &device, const vk::raii::CommandPool &pool, std::string_view name);
void setDebugName(const vk::raii::Device &device, const vk::raii::CommandBuffer &buffer, std::string_view name);
void setDebugName(const vk::raii::Device &device, const vk::raii::SurfaceKHR &surface, std::string_view name);
void setDebugName(const vk::raii::Device &device, const vk::raii::Instance &instance, std::string_view name);
void setDebugName(const vk::raii::Device &device, const vk::raii::DescriptorSetLayout &layout, std::string_view name);
void setDebugName(const vk::raii::Device &device, const vk::raii::DescriptorPool &pool, std::string_view name);
void setDebugName(const vk::raii::Device &device, const vk::raii::DescriptorSet &set, std::string_view name);
void setDebugName(const vk::raii::Device &device, const vk::raii::PipelineLayout &layout, std::string_view name);
void setDebugName(const vk::raii::Device &device, const vk::raii::Pipeline &pipeline, std::string_view name);
