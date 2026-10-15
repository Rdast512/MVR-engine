#include "vk_dgc.hpp"

#include "../static_headers/logger.hpp"
#include "core/vk_descriptors.hpp"
#include "push_data.hpp"
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
    // packed size is 36 (24-byte push + 12-byte draw); 16-byte stride keeps the
    // frame root BDA in pushData 8-byte aligned for every sequence
    struct alignas(16) MeshDgcSequence
    {
        MeshPushData pushData;
        vk::DrawMeshTasksIndirectCommandEXT draw;
    };

    static_assert(std::is_trivially_copyable_v<MeshDgcSequence>);
    static_assert(offsetof(MeshDgcSequence, pushData) == 0);
    static_assert(offsetof(MeshDgcSequence, draw) == sizeof(MeshPushData));
    static_assert(sizeof(MeshDgcSequence) % 16 == 0);
    static_assert(sizeof(MeshDgcSequence) >= sizeof(MeshPushData) + sizeof(vk::DrawMeshTasksIndirectCommandEXT));

    constexpr vk::ShaderStageFlags kDgcMeshStages =
        vk::ShaderStageFlagBits::eMeshEXT | vk::ShaderStageFlagBits::eFragment;

    uint32_t maxMeshGroupsX(const HardwareCapabilities& capabilities)
    {
        const uint32_t countX = capabilities.meshShader.maxMeshWorkGroupCount[0];
        const uint32_t total = capabilities.meshShader.maxMeshWorkGroupTotalCount;
        uint32_t cap = countX == 0 ? 1u : countX;
        if (total != 0) {
            cap = std::min(cap, total);
        }
        return std::max(1u, cap);
    }

    uint32_t sequenceCountForMeshlets(uint32_t meshletCount, uint32_t maxGroupsX)
    {
        if (meshletCount == 0) {
            return 0;
        }
        return (meshletCount + maxGroupsX - 1) / maxGroupsX;
    }
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
        throw std::runtime_error("DGC requires descriptor heaps for PUSH_DATA tokens");
    }

    const auto& props = device.capabilities.deviceGeneratedCommands;
    if (props.maxIndirectSequenceCount == 0 || props.maxIndirectCommandsTokenCount < 2) {
        throw std::runtime_error("DGC sequence/token limits too small");
    }
    if ((props.supportedIndirectCommandsShaderStages & kDgcMeshStages) != kDgcMeshStages) {
        throw std::runtime_error("DGC mesh+fragment stages not in supportedIndirectCommandsShaderStages");
    }
    if (props.maxIndirectCommandsTokenOffset < offsetof(MeshDgcSequence, draw)) {
        throw std::runtime_error("DGC maxIndirectCommandsTokenOffset below mesh draw token");
    }
    if (props.maxIndirectCommandsIndirectStride < sizeof(MeshDgcSequence)) {
        throw std::runtime_error("DGC maxIndirectCommandsIndirectStride below sequence stride");
    }

    shaderStages = kDgcMeshStages;
    createLayout();

    const uint32_t initialCapacity =
        std::min(std::max(resourceManager.objectStorage.size(), 1u), props.maxIndirectSequenceCount);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        ensureFrameCapacity(frame, initialCapacity);
    }

    logInfo(std::format("DGC ready: stride={} explicitPreprocess={} initialSequences={}", sizeof(MeshDgcSequence),
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

    const std::array tokens = {
        vk::IndirectCommandsLayoutTokenEXT{
            .type = vk::IndirectCommandsTokenTypeEXT::ePushData,
            .data = vk::IndirectCommandsTokenDataEXT{&pushToken},
            .offset = 0,
        },
        vk::IndirectCommandsLayoutTokenEXT{
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
        .tokenCount = static_cast<uint32_t>(tokens.size()),
        .pTokens = tokens.data(),
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

    logInfo(std::format("DGC frame {} capacity {} -> {} (need {}, stride={})", frameSlot,
                         sequenceCapacity[frameSlot], newCapacity, minSequences, sizeof(MeshDgcSequence)),
             "DGC");
    destroyFrameResources(frameSlot);
    allocateSequenceBuffer(frameSlot, newCapacity);
    allocatePreprocessBuffer(frameSlot, newCapacity);
}

void DeviceGeneratedCommands::allocateSequenceBuffer(uint32_t frameSlot, uint32_t capacity)
{
    const vk::DeviceSize bufferSize = static_cast<vk::DeviceSize>(capacity) * sizeof(MeshDgcSequence);
    vk::raii::Buffer buffer({});
    VmaAllocation memory = nullptr;
    createBuffer(bufferSize, vk::BufferUsageFlagBits2::eIndirectBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
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

    vk::raii::Buffer buffer({});
    VmaAllocation memory = nullptr;
    // NVIDIA 616.92 reports 256 B alignment, but a suballocated preprocess buffer at a non-4 KiB
    // offset faults (WriteInvalid @ VA 0) once a frame exceeds 96 sequences; own block = offset 0
    createBuffer(preprocessSize,
                 vk::BufferUsageFlagBits2::ePreprocessBufferEXT | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eDeviceLocal, buffer, memory, resourceManager.allocator.allocator,
                 device.vkdevice, device.queueFamilyIndices, std::format("DgcPreprocessMemory_{}", frameSlot),
                 VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);

    preprocessBuffers[frameSlot] = std::move(buffer);
    preprocessMemory[frameSlot] = memory;
    preprocessAddresses[frameSlot] = device.vkdevice.getBufferAddress({.buffer = *preprocessBuffers[frameSlot]});
    preprocessSizes[frameSlot] = preprocessSize;
    setDebugName(device.vkdevice, preprocessBuffers[frameSlot], std::format("DgcPreprocess_{}", frameSlot));
    tracyResourceAlloc(static_cast<VkBuffer>(*preprocessBuffers[frameSlot]), static_cast<size_t>(preprocessSize),
                       "GPU/DgcPreprocess");
}

void DeviceGeneratedCommands::destroyFrameResources(uint32_t frameSlot)
{
    if (sequenceMapped[frameSlot] != nullptr && sequenceMemory[frameSlot] != nullptr) {
        vmaUnmapMemory(resourceManager.allocator.allocator, sequenceMemory[frameSlot]);
        sequenceMapped[frameSlot] = nullptr;
    }
    destroyVmaBuffer(resourceManager.allocator.allocator, sequenceBuffers[frameSlot], sequenceMemory[frameSlot],
                     "GPU/DgcSequences");
    sequenceAddresses[frameSlot] = 0;
    sequenceCapacity[frameSlot] = 0;

    destroyVmaBuffer(resourceManager.allocator.allocator, preprocessBuffers[frameSlot], preprocessMemory[frameSlot],
                     "GPU/DgcPreprocess");
    preprocessAddresses[frameSlot] = 0;
    preprocessSizes[frameSlot] = 0;
}

void DeviceGeneratedCommands::updateSequences(uint32_t frameSlot)
{
    ZoneScopedN("DeviceGeneratedCommands::updateSequences");
    sequenceCount = 0;
    recordedFrame = frameSlot;
    if (resourceManager.asset(AssetBuffer::Primitives).address == 0) {
        return;
    }

    const auto& storage = resourceManager.objectStorage;
    // entities not flushed yet have no GPU slice
    const auto entityCount =
        std::min(storage.size(), static_cast<uint32_t>(resourceManager.modelAllocations.size()));
    if (entityCount == 0) {
        return;
    }

    const auto& geometry = resourceManager.geometryStore;
    const uint32_t maxGroupsX = maxMeshGroupsX(device.capabilities);

    // visits every primitive with meshlets; the shaders resolve everything else from the frame root
    auto forEachPrimitive = [&](auto&& visit) {
        for (EntityId id = 0; id < entityCount; ++id) {
            if ((storage.flags[id] & EntityFlag::Active) == 0) {
                continue;
            }
            const uint32_t primFirst = storage.firstPrimitives[id];
            for (uint32_t p = primFirst; p < primFirst + storage.primitiveCounts[id]; ++p) {
                const uint32_t meshletCount = geometry.primitiveDraws[p].meshlets.meshletCount;
                if (meshletCount > 0) {
                    visit(id, p, meshletCount);
                }
            }
        }
    };

    uint32_t needed = 0;
    forEachPrimitive([&](EntityId, uint32_t, uint32_t meshletCount) {
        needed += sequenceCountForMeshlets(meshletCount, maxGroupsX);
    });
    if (needed == 0) {
        return;
    }

    ensureFrameCapacity(frameSlot, needed);
    auto* sequences = static_cast<MeshDgcSequence*>(sequenceMapped[frameSlot]);

    const vk::DeviceAddress frameRoot = resourceManager.frameRootAddress(frameSlot);
    auto fillSequences = [&](EntityId id, uint32_t primitive, uint32_t meshletCount) {
        // meshlet batches past maxMeshWorkGroupCount split into more sequences
        for (uint32_t offset = 0; offset < meshletCount; offset += maxGroupsX) {
            const uint32_t batch = std::min(meshletCount - offset, maxGroupsX);
            sequences[sequenceCount++] = MeshDgcSequence{
                .pushData =
                    {
                        .frameRoot = frameRoot,
                        .instanceIndex = id,
                        .primitiveIndex = primitive,
                        .meshletOffset = offset,
                        .meshletCount = batch,
                    },
                .draw = {.groupCountX = batch, .groupCountY = 1, .groupCountZ = 1},
            };
        }
    };

    forEachPrimitive(fillSequences);

    TracyPlot("Vulkan/DgcSequenceCount", static_cast<double>(sequenceCount));
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
    if (sequenceCount == 0 || !explicitPreprocess) {
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
    if (sequenceCount == 0) {
        return;
    }

    vk::GeneratedCommandsPipelineInfoEXT pipelineInfo{};
    vk::GeneratedCommandsInfoEXT info{};
    fillGeneratedCommandsInfo(info, pipelineInfo, recordedFrame);
    cmd.executeGeneratedCommandsEXT(explicitPreprocess ? vk::True : vk::False, info);
}
