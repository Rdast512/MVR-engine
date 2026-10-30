#pragma once

#include "../Constants.h"
#include "core/vk_device.hpp"
#include "core/vk_resource_manager.hpp"
#include "vk_shader_objects.hpp"

#include <array>
#include <vulkan/vulkan_raii.hpp>

class DescriptorManager;

// GPU-driven mesh draws: PUSH_DATA + DRAW_MESH_TASKS via VK_EXT_device_generated_commands.
// The meshlet cull pass writes the sequences and their counts; one execute per (CullPhase, MeshStateGroup).
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
    void prepareFrame(uint32_t frameSlot, const std::array<uint32_t, kMeshStateGroupCount>& maxSequences,
                      vk::DeviceAddress countersAddress);

    // valid after prepareFrame: where the cull pass writes each group's sequences
    [[nodiscard]] vk::DeviceAddress sequenceAddress() const { return sequenceAddresses[frame]; }
    [[nodiscard]] uint32_t groupSequenceOffset(CullPhase phase, MeshStateGroup group) const
    {
        return std::to_underlying(phase) * phaseSequences + groupOffsets[std::to_underlying(group)];
    }

    // both get the same variant within a frame: preprocess snapshots its shaders and state.
    // preprocess runs outside the render pass, after the cull pass is made visible to it
    void recordPreprocess(vk::raii::CommandBuffer& cmd, RasterVariant variant, CullPhase phase) const;
    // binds each group's mesh state before its execute
    void recordExecute(vk::raii::CommandBuffer& cmd, RasterVariant variant, CullPhase phase) const;

private:
    void createLayout();
    void createStateCommandBuffers();
    void ensureFrameCapacity(uint32_t frameSlot, uint32_t minSequences);
    void destroyFrameResources(uint32_t frameSlot);
    void allocateSequenceBuffer(uint32_t frameSlot, uint32_t capacity);
    void allocatePreprocessBuffer(uint32_t frameSlot, uint32_t capacity);
    [[nodiscard]] vk::DeviceSize queryPreprocessSize(uint32_t capacity) const;
    [[nodiscard]] vk::GeneratedCommandsInfoEXT generatedCommandsInfo(CullPhase phase, MeshStateGroup group,
                                                                     RasterVariant variant) const;
    [[nodiscard]] const vk::raii::CommandBuffer& stateCommandBuffer(RasterVariant variant,
                                                                    MeshStateGroup group) const;

    Device& device;
    ResourceManager& resourceManager;
    DescriptorManager& descriptorManager;
    ShaderObjects& shaderObjects;

    vk::raii::IndirectCommandsLayoutEXT layout = nullptr;
    // per RasterVariant: one handle per stage of kDgcMeshStages, pointed to by the matching generatedShaderInfos row
    std::array<std::array<vk::ShaderEXT, 2>, kRasterVariantCount> generatedShaders{};
    std::array<vk::GeneratedCommandsShaderInfoEXT, kRasterVariantCount> generatedShaderInfos{};
    vk::ShaderStageFlags shaderStages{};

    // stays in recording state; pool declared first so the buffers are freed before it
    vk::raii::CommandPool statePool = nullptr;
    // [variant * kMeshStateGroupCount + group]: execute must match the preprocessed graphics state exactly
    std::vector<vk::raii::CommandBuffer> stateCommandBuffers;

    // frame recorded by the last prepareFrame
    uint32_t frame = 0;
    std::array<uint32_t, kMeshStateGroupCount> groupMaxSequences{};
    // within a phase
    std::array<uint32_t, kMeshStateGroupCount> groupOffsets{};
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
