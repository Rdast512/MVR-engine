#include "vk_driver_heap.hpp"
#include "util/debug.hpp"

#include <format>

DriverDescriptorHeap::DriverDescriptorHeap(const Device& device, std::string_view debugName)
{
    setLayout = vk::raii::DescriptorSetLayout(device.vkdevice, vk::DescriptorSetLayoutCreateInfo{});
    const vk::DescriptorSetLayout layout = *setLayout;
    pipelineLayout = vk::raii::PipelineLayout(device.vkdevice,
                                              vk::PipelineLayoutCreateInfo{.setLayoutCount = 1, .pSetLayouts = &layout});
    // a pool needs at least one size entry even though the set holds no descriptors
    constexpr vk::DescriptorPoolSize kPoolSize{.type = vk::DescriptorType::eSampler, .descriptorCount = 1};
    pool = vk::raii::DescriptorPool(device.vkdevice,
                                    vk::DescriptorPoolCreateInfo{.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
                                                                 .maxSets = 1,
                                                                 .poolSizeCount = 1,
                                                                 .pPoolSizes = &kPoolSize});
    set = std::move(vk::raii::DescriptorSets(device.vkdevice, vk::DescriptorSetAllocateInfo{.descriptorPool = *pool,
                                                                                            .descriptorSetCount = 1,
                                                                                            .pSetLayouts = &layout})
                        .front());
    setDebugName(device.vkdevice, setLayout, std::format("{}EmptySetLayout", debugName));
    setDebugName(device.vkdevice, pool, std::format("{}EmptySetPool", debugName));
    setDebugName(device.vkdevice, set, std::format("{}EmptySet", debugName));
}

void DriverDescriptorHeap::bind(const vk::raii::CommandBuffer& cmd) const
{
    const vk::DescriptorSet emptySet = *set;
    cmd.bindDescriptorSets2(vk::BindDescriptorSetsInfo{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                                       .layout = *pipelineLayout,
                                                       .descriptorSetCount = 1,
                                                       .pDescriptorSets = &emptySet});
}
