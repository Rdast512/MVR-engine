//
// Created by user on 31.12.2025.
//

#include "debug.hpp"

#include <string>
#include <string_view>

namespace {
    void setDebugNameHandle(const vk::raii::Device &device, uint64_t handle, vk::ObjectType type, std::string_view name) {
	const auto setNameFn = device.getDispatcher()->vkSetDebugUtilsObjectNameEXT;
	if (!setNameFn || handle == 0) {
		return; // Debug utils extension not enabled or null handle
	}
	std::string nameCopy{name};
	vk::DebugUtilsObjectNameInfoEXT info{
		.objectType = type,
		.objectHandle = handle,
		.pObjectName = nameCopy.c_str()
	};
	device.setDebugUtilsObjectNameEXT(info);
}

template <typename Handle>
void setDebugNameImpl(const vk::raii::Device &device, const Handle &handle, std::string_view name, vk::ObjectType type) {
	setDebugNameHandle(device, reinterpret_cast<uint64_t>(static_cast<typename Handle::CType>(*handle)), type, name);
}
} // namespace

void setDebugName(const vk::raii::Device &device, const vk::raii::Image &image, std::string_view name) {
	setDebugNameImpl(device, image, name, vk::ObjectType::eImage);
}

void setDebugName(const vk::raii::Device &device, const vk::raii::Buffer &buffer, std::string_view name) {
	setDebugNameImpl(device, buffer, name, vk::ObjectType::eBuffer);
}

void setDebugName(const vk::raii::Device &device, const vk::raii::ImageView &imageView, std::string_view name) {
	setDebugNameImpl(device, imageView, name, vk::ObjectType::eImageView);
}

void setDebugName(const vk::raii::Device &device, vk::Image image, std::string_view name) {
	setDebugNameHandle(device, reinterpret_cast<uint64_t>(static_cast<VkImage>(image)), vk::ObjectType::eImage, name);
}

void setDebugName(const vk::raii::Device &device, vk::ImageView imageView, std::string_view name) {
	setDebugNameHandle(device, reinterpret_cast<uint64_t>(static_cast<VkImageView>(imageView)), vk::ObjectType::eImageView, name);
}

void setDebugName(const vk::raii::Device &device, vk::DeviceMemory memory, std::string_view name) {
	setDebugNameHandle(device, reinterpret_cast<uint64_t>(static_cast<VkDeviceMemory>(memory)), vk::ObjectType::eDeviceMemory, name);
}

void setDebugName(const vk::raii::Device &device, const vk::raii::Device &object, std::string_view name) {
	setDebugNameImpl(device, object, name, vk::ObjectType::eDevice);
}

void setDebugName(const vk::raii::Device &device, const vk::raii::PhysicalDevice &object, std::string_view name) {
	setDebugNameImpl(device, object, name, vk::ObjectType::ePhysicalDevice);
}

void setDebugName(const vk::raii::Device &device, const vk::raii::Queue &queue, std::string_view name) {
	setDebugNameImpl(device, queue, name, vk::ObjectType::eQueue);
}

void setDebugName(const vk::raii::Device &device, const vk::raii::CommandPool &pool, std::string_view name) {
	setDebugNameImpl(device, pool, name, vk::ObjectType::eCommandPool);
}

void setDebugName(const vk::raii::Device &device, const vk::raii::CommandBuffer &buffer, std::string_view name) {
	setDebugNameImpl(device, buffer, name, vk::ObjectType::eCommandBuffer);
}

void setDebugName(const vk::raii::Device &device, const vk::raii::SurfaceKHR &surface, std::string_view name) {
	setDebugNameImpl(device, surface, name, vk::ObjectType::eSurfaceKHR);
}

void setDebugName(const vk::raii::Device &device, const vk::raii::Instance &instance, std::string_view name) {
	setDebugNameImpl(device, instance, name, vk::ObjectType::eInstance);
}

void setDebugName(const vk::raii::Device &device, const vk::raii::DescriptorSetLayout &layout, std::string_view name) {
	setDebugNameImpl(device, layout, name, vk::ObjectType::eDescriptorSetLayout);
}

void setDebugName(const vk::raii::Device &device, const vk::raii::DescriptorPool &pool, std::string_view name) {
	setDebugNameImpl(device, pool, name, vk::ObjectType::eDescriptorPool);
}

void setDebugName(const vk::raii::Device &device, const vk::raii::DescriptorSet &set, std::string_view name) {
	setDebugNameImpl(device, set, name, vk::ObjectType::eDescriptorSet);
}

void setDebugName(const vk::raii::Device &device, const vk::raii::PipelineLayout &layout, std::string_view name) {
	setDebugNameImpl(device, layout, name, vk::ObjectType::ePipelineLayout);
}

void setDebugName(const vk::raii::Device &device, const vk::raii::Pipeline &pipeline, std::string_view name) {
	setDebugNameImpl(device, pipeline, name, vk::ObjectType::ePipeline);
}
