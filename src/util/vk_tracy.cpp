#include "vk_tracy.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

void tracyResourceAlloc([[maybe_unused]] const void* ptr, [[maybe_unused]] size_t size,
                        [[maybe_unused]] const char* poolName)
{
#ifdef TRACY_ENABLE
	if (ptr == nullptr || size == 0) {
		return;
	}
	TracyAllocN(ptr, size, poolName != nullptr ? poolName : "GPU/Unknown");
#endif
}

void tracyResourceFree([[maybe_unused]] const void* ptr, [[maybe_unused]] const char* poolName)
{
#ifdef TRACY_ENABLE
	if (ptr == nullptr) {
		return;
	}
	TracyFreeN(ptr, poolName != nullptr ? poolName : "GPU/Unknown");
#endif
}

VkTracyContext::~VkTracyContext()
{
	shutdown();
}

void VkTracyContext::init([[maybe_unused]] const vk::raii::Instance& instance,
						 [[maybe_unused]] const vk::raii::PhysicalDevice& physicalDevice,
						 [[maybe_unused]] const vk::raii::Device& device,
						 [[maybe_unused]] const vk::raii::Queue& queue,
						 [[maybe_unused]] const vk::raii::CommandBuffer& setupCommandBuffer,
						 [[maybe_unused]] const char* contextName)
{
#ifdef TRACY_ENABLE
	if (context)
	{
		return;
	}

#if defined(TRACY_VK_USE_SYMBOL_TABLE)
	context = TracyVkContext(static_cast<VkInstance>(*instance), static_cast<VkPhysicalDevice>(*physicalDevice),
							  static_cast<VkDevice>(*device), static_cast<VkQueue>(*queue),
							  static_cast<VkCommandBuffer>(*setupCommandBuffer), nullptr, nullptr);
#else
	context = TracyVkContext(static_cast<VkPhysicalDevice>(*physicalDevice), static_cast<VkDevice>(*device),
							  static_cast<VkQueue>(*queue), static_cast<VkCommandBuffer>(*setupCommandBuffer));
#endif

	if (context && contextName)
	{
		const auto len = static_cast<uint16_t>(std::min<size_t>(std::strlen(contextName), UINT16_MAX));
		TracyVkContextName(context, contextName, len);
	}
#endif
}

void VkTracyContext::collect([[maybe_unused]] const vk::raii::CommandBuffer& commandBuffer) const
{
#ifdef TRACY_ENABLE
	if (!context)
	{
		return;
	}

	TracyVkCollect(context, static_cast<VkCommandBuffer>(*commandBuffer));
#endif
}

void VkTracyContext::shutdown()
{
#ifdef TRACY_ENABLE
	if (!context)
	{
		return;
	}

	TracyVkDestroy(context);
	context = nullptr;
#endif
}

bool VkTracyContext::isActive() const noexcept
{
#ifdef TRACY_ENABLE
	return context != nullptr;
#else
	return false;
#endif
}
