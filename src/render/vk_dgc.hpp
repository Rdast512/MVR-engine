#pragma once

#include "../Constants.h"
#include "core/vk_device.hpp"
#include "core/vk_resource_manager.hpp"
#include "vk_shader_objects.hpp"

#include <array>
#include <vulkan/vulkan_raii.hpp>

class DescriptorManager;

// GPU-driven mesh draws: PUSH_DATA + DRAW_MESH_TASKS via VK_EXT_device_generated_commands.
// The meshlet cull pass writes the sequences and their counts; one execute per (CullPhase, RasterBin).
// Each pair owns a sequence region and a preprocess region.
class DeviceGeneratedCommands
{
public:
    DeviceGeneratedCommands(Device& device, ResourceManager& resourceManager, DescriptorManager& descriptorManager,
                            ShaderObjects& shaderObjects);
    ~DeviceGeneratedCommands();

    DeviceGeneratedCommands(const DeviceGeneratedCommands&) = delete;
    DeviceGeneratedCommands& operator=(const DeviceGeneratedCommands&) = delete;
    DeviceGeneratedCommands(DeviceGeneratedCommands&&) = delete;
    DeviceGeneratedCommands& operator=(DeviceGeneratedCommands&&) = delete;

    void init();

    // sizes this slot's buffers for maxSequences per group in every phase (the slot must not be in flight); counts
    // are read from GpuCullCounters::sequenceCount of each phase, the first phase's counters at countersAddress
    void prepareFrame(uint32_t frameSlot, const std::array<uint32_t, kRasterBinCount>& maxSequences,
                      vk::DeviceAddress countersAddress);

    // valid after prepareFrame: where the cull pass writes each group's sequences
    [[nodiscard]] vk::DeviceAddress sequenceAddress() const { return sequenceAddresses[frame]; }
    [[nodiscard]] uint32_t binSequenceOffset(CullPhase phase, RasterBin group) const
    {
        return std::to_underlying(phase) * phaseSequences + groupOffsets[std::to_underlying(group)];
    }

    // preprocess snapshots the state command buffer's shaders and state; it runs outside the render pass, after the
    // cull pass is made visible to it
    void recordPreprocess(vk::raii::CommandBuffer& cmd, CullPhase phase) const;
    // binds the mesh state before each bin's execute
    void recordExecute(vk::raii::CommandBuffer& cmd, CullPhase phase) const;

private:
    void createLayout();
    void createStateCommandBuffer();
    void ensureFrameCapacity(uint32_t frameSlot, uint32_t minSequences);
    void destroyFrameResources(uint32_t frameSlot);
    void allocateSequenceBuffer(uint32_t frameSlot, uint32_t capacity);
    void allocatePreprocessBuffer(uint32_t frameSlot, uint32_t capacity);
    [[nodiscard]] vk::DeviceSize queryPreprocessSize(uint32_t capacity) const;
    [[nodiscard]] vk::GeneratedCommandsInfoEXT generatedCommandsInfo(CullPhase phase, RasterBin group) const;

    Device& device;
    ResourceManager& resourceManager;
    DescriptorManager& descriptorManager;
    ShaderObjects& shaderObjects;

    vk::raii::IndirectCommandsLayoutEXT layout = nullptr;
    // one handle per stage of kDgcMeshStages, pointed to by generatedShaderInfo
    std::array<vk::ShaderEXT, 2> generatedShaders{};
    vk::GeneratedCommandsShaderInfoEXT generatedShaderInfo{};
    vk::ShaderStageFlags shaderStages{};

    // stays in recording state; pool declared first so the buffers are freed before it
    vk::raii::CommandPool statePool = nullptr;
    // execute must match the preprocessed graphics state exactly; every bin shares it until M6
    vk::raii::CommandBuffer stateCommandBuffer = nullptr;

    // frame recorded by the last prepareFrame
    uint32_t frame = 0;
    std::array<uint32_t, kRasterBinCount> binMaxSequences{};
    // within a phase
    std::array<uint32_t, kRasterBinCount> groupOffsets{};
    // sequences of all groups of one phase
    uint32_t phaseSequences = 0;
    vk::DeviceAddress counters = 0;

    // device-local, written by buildMeshSequences
    std::array<vk::raii::Buffer, MAX_FRAMES_IN_FLIGHT> sequenceBuffers = {nullptr, nullptr};
    std::array<VmaAllocation, MAX_FRAMES_IN_FLIGHT> sequenceMemory = {nullptr, nullptr};
    std::array<vk::DeviceAddress, MAX_FRAMES_IN_FLIGHT> sequenceAddresses = {0, 0};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> sequenceCapacity = {0, 0};

    std::array<vk::raii::Buffer, MAX_FRAMES_IN_FLIGHT> preprocessBuffers = {nullptr, nullptr};
    std::array<VmaAllocation, MAX_FRAMES_IN_FLIGHT> preprocessMemory = {nullptr, nullptr};
    std::array<vk::DeviceAddress, MAX_FRAMES_IN_FLIGHT> preprocessAddresses = {0, 0};
    // byte stride between per-(phase, group) regions, each sized for the whole sequence capacity
    std::array<vk::DeviceSize, MAX_FRAMES_IN_FLIGHT> preprocessGroupStrides = {0, 0};
};
