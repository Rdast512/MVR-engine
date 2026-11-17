#pragma once
#include "core/vk_device.hpp"

#include <string_view>

#include <vulkan/vulkan_raii.hpp>

// CUDA kernels launched through VK_NVX_binary_import (NGX, NRC) address resources by VK_NVX_image_view_handle handles
// into the driver-managed descriptor heap. While an application heap is bound they index that heap instead, even from
// a new command buffer. Binding legacy descriptor state is the only way back to the driver heap (DXVK's
// DxvkContext::launchCuKernelNVX binds an empty set the same way; its bind without a layout crashes driver 621.01).
// The caller rebinds the engine heaps after the vendor call (spec/considerations.md C-H1-1)
class DriverDescriptorHeap
{
public:
    DriverDescriptorHeap(const Device& device, std::string_view debugName);

    void bind(const vk::raii::CommandBuffer& cmd) const;

private:
    vk::raii::DescriptorSetLayout setLayout = nullptr;
    vk::raii::PipelineLayout pipelineLayout = nullptr;
    vk::raii::DescriptorPool pool = nullptr;
    vk::raii::DescriptorSet set = nullptr;
};
