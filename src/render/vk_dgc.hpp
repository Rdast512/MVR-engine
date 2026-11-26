#pragma once

#include "../Constants.h"
#include "core/vk_device.hpp"
#include "core/vk_allocator.hpp"
#include "vk_shader_objects.hpp"

#include <array>
#include <string_view>
#include <vulkan/vulkan_raii.hpp>

class DescriptorManager;
struct MeshPushData;

// GPU-driven mesh draws: PUSH_DATA + DRAW_MESH_TASKS via VK_EXT_device_generated_commands.
// The meshlet cull pass writes the sequences and their counts; one execute per (CullPhase, RasterBin).
// Each pair owns a sequence region and a preprocess region.
// The material-binned resolve (spec/m7/e1_material_binning.md) is one compute execute: EXECUTION_SET + PUSH_DATA +
// DISPATCH over the sequences passes/classify.slang writes, one per non-empty MaterialBin.
// without ENGINE_DGC=1 (default, spec/features/planned/dgc_toggle.md): no DGC object; only the sequence buffers
// remain, drawn by recordIndirectDraw, and the renderer dispatches the resolve per bin
class DeviceGeneratedCommands
{
public:
    DeviceGeneratedCommands(Device& device, const VkAllocator& allocator, DescriptorManager& descriptorManager,
                            ShaderObjects& shaderObjects);
    ~DeviceGeneratedCommands();

    DeviceGeneratedCommands(const DeviceGeneratedCommands&) = delete;
    DeviceGeneratedCommands& operator=(const DeviceGeneratedCommands&) = delete;
    DeviceGeneratedCommands(DeviceGeneratedCommands&&) = delete;
    DeviceGeneratedCommands& operator=(DeviceGeneratedCommands&&) = delete;

    void init();

    // sizes this slot's buffers for maxSequences per group in every phase (the slot must not be in flight); counts
    // are read from GpuCullCounters::sequenceCount of each phase, the first phase's counters at the start of
    // countersBuffer (countersAddress)
    void prepareFrame(uint32_t frameSlot, const std::array<uint32_t, kRasterBinCount>& maxSequences,
                      vk::Buffer countersBuffer, vk::DeviceAddress countersAddress);

    // valid after prepareFrame: where the cull pass writes each group's sequences
    [[nodiscard]] vk::DeviceAddress sequenceAddress() const { return sequenceAddresses[frame]; }
    [[nodiscard]] uint32_t binSequenceOffset(CullPhase phase, RasterBin group) const
    {
        return std::to_underlying(phase) * phaseSequences + groupOffsets[std::to_underlying(group)];
    }

    // preprocess snapshots the state command buffer's shaders and state; it runs outside the render pass, after the
    // cull pass is made visible to it
    void recordPreprocess(vk::raii::CommandBuffer& cmd, CullPhase phase) const;
    // binds the bin's mesh state, then executes its sequences; nothing when the bin holds no meshlet
    void recordExecute(vk::raii::CommandBuffer& cmd, CullPhase phase, RasterBin group) const;
    // without DGC: the same sequences as one vkCmdDrawMeshTasksIndirectCountEXT, push from
    // MeshletCulling::indirectDrawPush; nothing when the bin holds no meshlet
    void recordIndirectDraw(const vk::raii::CommandBuffer& cmd, CullPhase phase, RasterBin group,
                            const MeshPushData& push) const;

    // materialBins: RenderTargets' GpuMaterialBins, its sequences made visible to the preprocess stage
    void recordResolvePreprocess(const vk::raii::CommandBuffer& cmd, vk::DeviceAddress materialBins) const;
    // binds the execution set's initial shader, as the state command buffer does, then executes
    void recordResolveExecute(const vk::raii::CommandBuffer& cmd, vk::DeviceAddress materialBins) const;

private:
    void createLayout();
    void createStateCommandBuffer();
    void ensureFrameCapacity(uint32_t frameSlot, uint32_t minSequences);
    void destroyFrameResources(uint32_t frameSlot);
    void allocateSequenceBuffer(uint32_t frameSlot, uint32_t capacity);
    void allocatePreprocessBuffer(uint32_t frameSlot, uint32_t capacity);
    [[nodiscard]] vk::DeviceSize queryPreprocessSize(uint32_t capacity) const;
    [[nodiscard]] vk::GeneratedCommandsInfoEXT generatedCommandsInfo(CullPhase phase, RasterBin group) const;
    void createResolveCommands();
    [[nodiscard]] vk::GeneratedCommandsInfoEXT resolveCommandsInfo(vk::DeviceAddress materialBins) const;
    // a DGC record call without DGC is a renderer bug
    void requireEnabled(std::string_view caller) const;

    Device& device;
    const VkAllocator& allocator;
    DescriptorManager& descriptorManager;
    ShaderObjects& shaderObjects;

    vk::raii::IndirectCommandsLayoutEXT layout = nullptr;
    // per bin: one handle per stage of kDgcMeshStages, pointed to by the bin's generatedShaderInfos row
    std::array<std::array<vk::ShaderEXT, 2>, kRasterBinCount> generatedShaders{};
    std::array<vk::GeneratedCommandsShaderInfoEXT, kRasterBinCount> generatedShaderInfos{};
    vk::ShaderStageFlags shaderStages{};

    // stays in recording state; pool declared first so the buffers are freed before it
    vk::raii::CommandPool statePool = nullptr;
    // one per bin: execute must match the preprocessed graphics state exactly, shaders included
    std::array<vk::raii::CommandBuffer, kRasterBinCount> stateCommandBuffers = {nullptr, nullptr};
    // binds the resolve execution set's initial shader (MaterialBin::Background)
    vk::raii::CommandBuffer resolveStateCommandBuffer = nullptr;

    vk::raii::IndirectCommandsLayoutEXT resolveLayout = nullptr;
    // MaterialBin-indexed resolve shaders
    vk::raii::IndirectExecutionSetEXT resolveExecutionSet = nullptr;
    // one region for both frame slots: frames are ordered on the graphics queue by the resolve's barriers
    vk::raii::Buffer resolvePreprocessBuffer = nullptr;
    VmaAllocation resolvePreprocessMemory = nullptr;
    vk::DeviceAddress resolvePreprocessAddress = 0;
    vk::DeviceSize resolvePreprocessSize = 0;

    // frame recorded by the last prepareFrame
    uint32_t frame = 0;
    std::array<uint32_t, kRasterBinCount> binMaxSequences{};
    // within a phase
    std::array<uint32_t, kRasterBinCount> groupOffsets{};
    // sequences of all groups of one phase
    uint32_t phaseSequences = 0;
    vk::DeviceAddress counters = 0;
    vk::Buffer sequenceCountBuffer;

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
