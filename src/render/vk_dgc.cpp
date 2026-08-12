#include "vk_dgc.hpp"

#include "../static_headers/logger.hpp"
#include "core/vk_descriptors.hpp"
#include "push_data.hpp"
#include "scene/vk_camera.hpp"
#include "util/debug.hpp"
#include "util/vk_tracy.hpp"
#include "util/vk_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <stdexcept>
#include <type_traits>

namespace
{
    struct MeshDgcSequence
    {
        MeshPushData pushData;
        vk::DrawMeshTasksIndirectCommandEXT draw;
    };

    static_assert(std::is_trivially_copyable_v<MeshDgcSequence>);
    static_assert(offsetof(MeshDgcSequence, pushData) == 0);
    static_assert(offsetof(MeshDgcSequence, draw) == sizeof(MeshPushData));

    constexpr vk::ShaderStageFlags kDgcMeshStages =
        vk::ShaderStageFlagBits::eMeshEXT | vk::ShaderStageFlagBits::eFragment;
} // namespace

DeviceGeneratedCommands::DeviceGeneratedCommands(Device& device, ResourceManager& resourceManager,
                                                 DescriptorManager& descriptorManager, Pipeline& pipeline) :
    device(device), resourceManager(resourceManager), descriptorManager(descriptorManager), pipeline(pipeline)
{
}

DeviceGeneratedCommands::~DeviceGeneratedCommands()
{
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        destroyFrameResources(frame);
    }
}

void DeviceGeneratedCommands::init()
{
    ZoneScopedN("DeviceGeneratedCommands::init");

    if (descriptorManager.descriptorBindingMode != DescriptorBindingMode::DescriptorHeaps) {
        log_info("DGC skipped: descriptor heaps required for PUSH_DATA tokens", "DGC");
        return;
    }

    const auto& props = device.capabilities.deviceGeneratedCommands;
    if (props.maxIndirectSequenceCount == 0 || props.maxIndirectCommandsTokenCount < 2) {
        log_info("DGC skipped: sequence/token limits too small", "DGC");
        return;
    }
    if ((props.supportedIndirectCommandsShaderStages & kDgcMeshStages) != kDgcMeshStages) {
        log_info("DGC skipped: mesh+fragment stages not in supportedIndirectCommandsShaderStages", "DGC");
        return;
    }
    if (props.maxIndirectCommandsTokenOffset < offsetof(MeshDgcSequence, draw)) {
        log_info("DGC skipped: maxIndirectCommandsTokenOffset below mesh draw token", "DGC");
        return;
    }
    if (props.maxIndirectCommandsIndirectStride < sizeof(MeshDgcSequence)) {
        log_info("DGC skipped: maxIndirectCommandsIndirectStride below sequence stride", "DGC");
        return;
    }

    shaderStages = kDgcMeshStages;
    createLayout();

    const uint32_t initialCapacity =
        std::min(std::max(resourceManager.objectStorage.size(), 1u), props.maxIndirectSequenceCount);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        ensureFrameCapacity(frame, initialCapacity);
    }

    available = true;
    log_info(std::format("DGC ready: stride={} explicitPreprocess={} initialSequences={}", sizeof(MeshDgcSequence),
                         explicitPreprocess, initialCapacity),
             "DGC");
}

void DeviceGeneratedCommands::createLayout()
{
    const vk::IndirectCommandsPushConstantTokenEXT pushToken{
        .updateRange =
            {
                .stageFlags = vk::ShaderStageFlagBits::eAll,
                .offset = 0,
                .size = static_cast<uint32_t>(sizeof(MeshPushData)),
            },
    };

    const vk::IndirectCommandsLayoutTokenEXT tokens[] = {
        {
            .type = vk::IndirectCommandsTokenTypeEXT::ePushData,
            .data = vk::IndirectCommandsTokenDataEXT{&pushToken},
            .offset = 0,
        },
        {
            .type = vk::IndirectCommandsTokenTypeEXT::eDrawMeshTasks,
            .offset = static_cast<uint32_t>(offsetof(MeshDgcSequence, draw)),
        },
    };

    // explicit preprocess so execute stays inside the render pass
    const vk::IndirectCommandsLayoutCreateInfoEXT createInfo{
        .flags = vk::IndirectCommandsLayoutUsageFlagBitsEXT::eExplicitPreprocess,
        .shaderStages = shaderStages,
        .indirectStride = static_cast<uint32_t>(sizeof(MeshDgcSequence)),
        .pipelineLayout = nullptr,
        .tokenCount = 2,
        .pTokens = tokens,
    };

    layout = vk::raii::IndirectCommandsLayoutEXT(device.vkdevice, createInfo);
    explicitPreprocess = true;
}

vk::DeviceSize DeviceGeneratedCommands::queryPreprocessSize(uint32_t capacity) const
{
    vk::GeneratedCommandsPipelineInfoEXT pipelineInfo{
        .pipeline = *pipeline.pipeline,
    };
    const vk::GeneratedCommandsMemoryRequirementsInfoEXT memInfo{
        .pNext = &pipelineInfo,
        .indirectCommandsLayout = *layout,
        .maxSequenceCount = capacity,
        .maxDrawCount = 1,
    };
    const vk::MemoryRequirements2 requirements = device.vkdevice.getGeneratedCommandsMemoryRequirementsEXT(memInfo);
    return requirements.memoryRequirements.size;
}

void DeviceGeneratedCommands::ensureFrameCapacity(uint32_t frameSlot, uint32_t minSequences)
{
    if (minSequences <= sequenceCapacity[frameSlot]) {
        return;
    }

    const auto& props = device.capabilities.deviceGeneratedCommands;
    uint32_t newCapacity = sequenceCapacity[frameSlot] == 0 ? minSequences : sequenceCapacity[frameSlot];
    while (newCapacity < minSequences) {
        newCapacity *= 2;
    }
    newCapacity = std::min(newCapacity, props.maxIndirectSequenceCount);
    if (newCapacity < minSequences) {
        throw std::runtime_error("DGC sequence count exceeds maxIndirectSequenceCount");
    }

    destroyFrameResources(frameSlot);
    allocateSequenceBuffer(frameSlot, newCapacity);
    allocatePreprocessBuffer(frameSlot, newCapacity);
}

void DeviceGeneratedCommands::allocateSequenceBuffer(uint32_t frameSlot, uint32_t capacity)
{
    const vk::DeviceSize bufferSize = static_cast<vk::DeviceSize>(capacity) * sizeof(MeshDgcSequence);
    vk::raii::Buffer buffer({});
    VmaAllocation memory = nullptr;
    createBuffer(bufferSize, vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, buffer, memory,
                 resourceManager.allocator.allocator, device.vkdevice, device.queueFamilyIndices,
                 std::format("DgcSequenceMemory_{}", frameSlot));

    sequenceBuffers[frameSlot] = std::move(buffer);
    sequenceMemory[frameSlot] = memory;
    void* mapped = nullptr;
    vmaMapMemory(resourceManager.allocator.allocator, memory, &mapped);
    sequenceMapped[frameSlot] = mapped;
    sequenceAddresses[frameSlot] = device.vkdevice.getBufferAddress({.buffer = *sequenceBuffers[frameSlot]});
    sequenceCapacity[frameSlot] = capacity;
    setDebugName(device.vkdevice, sequenceBuffers[frameSlot], std::format("DgcSequences_{}", frameSlot));
    tracyResourceAlloc(static_cast<VkBuffer>(*sequenceBuffers[frameSlot]), static_cast<size_t>(bufferSize),
                       "GPU/DgcSequences");
}

void DeviceGeneratedCommands::allocatePreprocessBuffer(uint32_t frameSlot, uint32_t capacity)
{
    const vk::DeviceSize preprocessSize = queryPreprocessSize(capacity);
    if (preprocessSize == 0) {
        preprocessSizes[frameSlot] = 0;
        preprocessAddresses[frameSlot] = 0;
        return;
    }

    const vk::BufferUsageFlags2CreateInfo usage2{
        .usage = vk::BufferUsageFlagBits2::ePreprocessBufferEXT | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
    };
    const vk::BufferCreateInfo bufferInfo{
        .pNext = &usage2,
        .size = preprocessSize,
        .sharingMode = vk::SharingMode::eConcurrent,
        .queueFamilyIndexCount = static_cast<uint32_t>(device.queueFamilyIndices.size()),
        .pQueueFamilyIndices = device.queueFamilyIndices.data(),
    };

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    allocInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    allocInfo.priority = 0.75f;

    VkBuffer rawBuffer{};
    VmaAllocation memory = nullptr;
    if (vmaCreateBuffer(resourceManager.allocator.allocator, &static_cast<const VkBufferCreateInfo&>(bufferInfo),
                        &allocInfo, &rawBuffer, &memory, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate DGC preprocess buffer");
    }

    preprocessBuffers[frameSlot] = vk::raii::Buffer(device.vkdevice, rawBuffer);
    preprocessMemory[frameSlot] = memory;
    preprocessAddresses[frameSlot] = device.vkdevice.getBufferAddress({.buffer = *preprocessBuffers[frameSlot]});
    preprocessSizes[frameSlot] = preprocessSize;
    vmaSetAllocationName(resourceManager.allocator.allocator, memory,
                         std::format("DgcPreprocessMemory_{}", frameSlot).c_str());
    setDebugName(device.vkdevice, preprocessBuffers[frameSlot], std::format("DgcPreprocess_{}", frameSlot));
    tracyResourceAlloc(rawBuffer, static_cast<size_t>(preprocessSize), "GPU/DgcPreprocess");
}

void DeviceGeneratedCommands::destroyFrameResources(uint32_t frameSlot)
{
    if (sequenceMapped[frameSlot] != nullptr && sequenceMemory[frameSlot] != nullptr) {
        vmaUnmapMemory(resourceManager.allocator.allocator, sequenceMemory[frameSlot]);
        sequenceMapped[frameSlot] = nullptr;
    }
    if (sequenceMemory[frameSlot] != nullptr) {
        VkBuffer raw = sequenceBuffers[frameSlot].release();
        tracyResourceFree(raw, "GPU/DgcSequences");
        vmaDestroyBuffer(resourceManager.allocator.allocator, raw, sequenceMemory[frameSlot]);
        sequenceMemory[frameSlot] = nullptr;
    }
    sequenceAddresses[frameSlot] = 0;
    sequenceCapacity[frameSlot] = 0;

    if (preprocessMemory[frameSlot] != nullptr) {
        VkBuffer raw = preprocessBuffers[frameSlot].release();
        tracyResourceFree(raw, "GPU/DgcPreprocess");
        vmaDestroyBuffer(resourceManager.allocator.allocator, raw, preprocessMemory[frameSlot]);
        preprocessMemory[frameSlot] = nullptr;
    }
    preprocessAddresses[frameSlot] = 0;
    preprocessSizes[frameSlot] = 0;
}

bool DeviceGeneratedCommands::updateSequences(uint32_t frameSlot, const Camera& camera)
{
    ZoneScopedN("DeviceGeneratedCommands::updateSequences");
    sequenceCount = 0;
    recordedFrame = frameSlot;
    if (!available) {
        return false;
    }
    if (resourceManager.vertexBufferAddress == 0 || resourceManager.meshletBufferAddress == 0 ||
        resourceManager.meshletVertexBufferAddress == 0 || resourceManager.meshletTriangleBufferAddress == 0) {
        return false;
    }

    const auto& storage = resourceManager.objectStorage;
    const uint32_t entityCount = storage.size();
    if (entityCount == 0) {
        return false;
    }

    ensureFrameCapacity(frameSlot, entityCount);
    auto* sequences = static_cast<MeshDgcSequence*>(sequenceMapped[frameSlot]);

    for (EntityId id = 0; id < entityCount; ++id) {
        if ((storage.flags[id] & EntityFlag::Active) == 0) {
            continue;
        }

        const MeshletDraw& meshletDraw = storage.meshletDraws[id];
        if (meshletDraw.meshletCount == 0) {
            continue;
        }

        MeshDgcSequence& sequence = sequences[sequenceCount];
        sequence.pushData.cameraAddress = camera.cameraBufferAddresses[frameSlot];
        sequence.pushData.objectUbAddress = resourceManager.instanceUboAddress(frameSlot, id);
        sequence.pushData.vertices = resourceManager.vertexBufferAddress;
        sequence.pushData.meshlets = resourceManager.meshletBufferAddress;
        sequence.pushData.meshletVertices = resourceManager.meshletVertexBufferAddress;
        sequence.pushData.meshletTriangles = resourceManager.meshletTriangleBufferAddress;
        sequence.pushData.firstMeshlet = meshletDraw.firstMeshlet;
        sequence.pushData.meshletCount = meshletDraw.meshletCount;
        sequence.pushData.texture = {
            .resourceIndex = storage.materials[id].textureIndex,
            .samplerIndex = 0,
        };
        sequence.pushData.samplerHandle = {
            .resourceIndex = descriptorManager.getSamplerDescriptorIndex(),
            .samplerIndex = 0,
        };
        sequence.draw.groupCountX = meshletDraw.meshletCount;
        sequence.draw.groupCountY = 1;
        sequence.draw.groupCountZ = 1;
        ++sequenceCount;
    }

    TracyPlot("Vulkan/DgcSequenceCount", static_cast<double>(sequenceCount));
    return sequenceCount > 0;
}

void DeviceGeneratedCommands::fillGeneratedCommandsInfo(vk::GeneratedCommandsInfoEXT& info,
                                                        vk::GeneratedCommandsPipelineInfoEXT& pipelineInfo,
                                                        uint32_t frameSlot) const
{
    pipelineInfo.pipeline = *pipeline.pipeline;
    info.pNext = &pipelineInfo;
    info.shaderStages = shaderStages;
    info.indirectExecutionSet = nullptr;
    info.indirectCommandsLayout = *layout;
    info.indirectAddress = sequenceAddresses[frameSlot];
    info.indirectAddressSize = static_cast<vk::DeviceSize>(sequenceCapacity[frameSlot]) * sizeof(MeshDgcSequence);
    info.preprocessAddress = preprocessAddresses[frameSlot];
    info.preprocessSize = preprocessSizes[frameSlot];
    info.maxSequenceCount = sequenceCount;
    info.sequenceCountAddress = 0;
    info.maxDrawCount = 1;
}

void DeviceGeneratedCommands::recordPreprocess(vk::raii::CommandBuffer& cmd) const
{
    ZoneScopedN("DeviceGeneratedCommands::recordPreprocess");
    if (!available || sequenceCount == 0 || !explicitPreprocess) {
        return;
    }

    const vk::MemoryBarrier2 hostBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eHost,
        .srcAccessMask = vk::AccessFlagBits2::eHostWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eCommandPreprocessEXT,
        .dstAccessMask = vk::AccessFlagBits2::eCommandPreprocessReadEXT,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &hostBarrier});

    vk::GeneratedCommandsPipelineInfoEXT pipelineInfo{};
    vk::GeneratedCommandsInfoEXT info{};
    fillGeneratedCommandsInfo(info, pipelineInfo, recordedFrame);
    cmd.preprocessGeneratedCommandsEXT(info, *cmd);

    const vk::MemoryBarrier2 preprocessBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eCommandPreprocessEXT,
        .srcAccessMask = vk::AccessFlagBits2::eCommandPreprocessWriteEXT,
        .dstStageMask = vk::PipelineStageFlagBits2::eCommandPreprocessEXT | vk::PipelineStageFlagBits2::eDrawIndirect,
        .dstAccessMask = vk::AccessFlagBits2::eCommandPreprocessReadEXT | vk::AccessFlagBits2::eIndirectCommandRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &preprocessBarrier});
}

void DeviceGeneratedCommands::recordExecute(vk::raii::CommandBuffer& cmd) const
{
    ZoneScopedN("DeviceGeneratedCommands::recordExecute");
    if (!available || sequenceCount == 0) {
        return;
    }

    vk::GeneratedCommandsPipelineInfoEXT pipelineInfo{};
    vk::GeneratedCommandsInfoEXT info{};
    fillGeneratedCommandsInfo(info, pipelineInfo, recordedFrame);
    cmd.executeGeneratedCommandsEXT(explicitPreprocess ? vk::True : vk::False, info);
}
