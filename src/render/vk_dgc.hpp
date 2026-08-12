#pragma once

#include "../Constants.h"
#include "core/vk_device.hpp"
#include "core/vk_resource_manager.hpp"
#include "vk_pipeline.hpp"

#include <array>
#include <vulkan/vulkan_raii.hpp>

class DescriptorManager;
class Camera;

// GPU-driven mesh draws: PUSH_DATA + DRAW_MESH_TASKS via VK_EXT_device_generated_commands.
// Sequences are CPU-filled until a cull compute pass exists.
class DeviceGeneratedCommands
{
public:
    DeviceGeneratedCommands(Device& device, ResourceManager& resourceManager, DescriptorManager& descriptorManager,
                            Pipeline& pipeline);
    ~DeviceGeneratedCommands();

    DeviceGeneratedCommands(const DeviceGeneratedCommands&) = delete;
    DeviceGeneratedCommands& operator=(const DeviceGeneratedCommands&) = delete;
    DeviceGeneratedCommands(DeviceGeneratedCommands&&) = delete;
    DeviceGeneratedCommands& operator=(DeviceGeneratedCommands&&) = delete;

    void init();

    void updateSequences(uint32_t frameSlot, const Camera& camera);

    void recordPreprocess(vk::raii::CommandBuffer& cmd) const;
    void recordExecute(vk::raii::CommandBuffer& cmd) const;

private:
    void createLayout();
    void ensureFrameCapacity(uint32_t frameSlot, uint32_t minSequences);
    void destroyFrameResources(uint32_t frameSlot);
    void allocateSequenceBuffer(uint32_t frameSlot, uint32_t capacity);
    void allocatePreprocessBuffer(uint32_t frameSlot, uint32_t capacity);
    [[nodiscard]] vk::DeviceSize queryPreprocessSize(uint32_t capacity) const;
    void fillGeneratedCommandsInfo(vk::GeneratedCommandsInfoEXT& info,
                                   vk::GeneratedCommandsPipelineInfoEXT& pipelineInfo, uint32_t frameSlot) const;

    Device& device;
    ResourceManager& resourceManager;
    DescriptorManager& descriptorManager;
    Pipeline& pipeline;

    vk::raii::IndirectCommandsLayoutEXT layout = nullptr;
    vk::ShaderStageFlags shaderStages{};
    bool explicitPreprocess = false;

    uint32_t sequenceCount = 0;
    uint32_t recordedFrame = 0;

    std::array<vk::raii::Buffer, MAX_FRAMES_IN_FLIGHT> sequenceBuffers = {nullptr, nullptr};
    std::array<VmaAllocation, MAX_FRAMES_IN_FLIGHT> sequenceMemory = {nullptr, nullptr};
    std::array<void*, MAX_FRAMES_IN_FLIGHT> sequenceMapped = {nullptr, nullptr};
    std::array<vk::DeviceAddress, MAX_FRAMES_IN_FLIGHT> sequenceAddresses = {0, 0};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> sequenceCapacity = {0, 0};

    std::array<vk::raii::Buffer, MAX_FRAMES_IN_FLIGHT> preprocessBuffers = {nullptr, nullptr};
    std::array<VmaAllocation, MAX_FRAMES_IN_FLIGHT> preprocessMemory = {nullptr, nullptr};
    std::array<vk::DeviceAddress, MAX_FRAMES_IN_FLIGHT> preprocessAddresses = {0, 0};
    std::array<vk::DeviceSize, MAX_FRAMES_IN_FLIGHT> preprocessSizes = {0, 0};
};
