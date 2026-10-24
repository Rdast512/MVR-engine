#pragma once

#include "../Constants.h"
#include "core/vk_device.hpp"
#include "core/vk_resource_manager.hpp"
#include "vk_shader_objects.hpp"

#include <array>
#include <vulkan/vulkan_raii.hpp>

class DescriptorManager;

// GPU-driven mesh draws: PUSH_DATA + DRAW_MESH_TASKS via VK_EXT_device_generated_commands.
// Sequences are CPU-filled until a cull compute pass exists.
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

    void updateSequences(uint32_t frameSlot);

    // both get the same variant within a frame: preprocess snapshots its shaders and state
    void recordPreprocess(vk::raii::CommandBuffer& cmd, RasterVariant variant) const;
    void recordExecute(vk::raii::CommandBuffer& cmd, RasterVariant variant) const;

private:
    void createLayout();
    void createStateCommandBuffers();
    void ensureFrameCapacity(uint32_t frameSlot, uint32_t minSequences);
    void destroyFrameResources(uint32_t frameSlot);
    void allocateSequenceBuffer(uint32_t frameSlot, uint32_t capacity);
    void allocatePreprocessBuffer(uint32_t frameSlot, uint32_t capacity);
    [[nodiscard]] vk::DeviceSize queryPreprocessSize(uint32_t capacity) const;
    void fillGeneratedCommandsInfo(vk::GeneratedCommandsInfoEXT& info, uint32_t frameSlot, uint32_t chunk,
                                   RasterVariant variant) const;
    [[nodiscard]] uint32_t chunkCount() const;

    Device& device;
    ResourceManager& resourceManager;
    DescriptorManager& descriptorManager;
    ShaderObjects& shaderObjects;

    vk::raii::IndirectCommandsLayoutEXT layout = nullptr;
    // per RasterVariant: one handle per stage of kDgcMeshStages, pointed to by the matching generatedShaderInfos row
    std::array<std::array<vk::ShaderEXT, 2>, kRasterVariantCount> generatedShaders{};
    std::array<vk::GeneratedCommandsShaderInfoEXT, kRasterVariantCount> generatedShaderInfos{};
    vk::ShaderStageFlags shaderStages{};
    bool explicitPreprocess = false;

    // stays in recording state; pool declared first so the buffer is freed before it
    vk::raii::CommandPool statePool = nullptr;
    // per RasterVariant
    std::array<vk::raii::CommandBuffer, kRasterVariantCount> stateCommandBuffers = {nullptr, nullptr};

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
    // byte stride between per-execute chunk regions inside the preprocess buffer
    std::array<vk::DeviceSize, MAX_FRAMES_IN_FLIGHT> preprocessChunkStrides = {0, 0};
};
