#include "vk_dgc.hpp"

#include "../static_headers/logger.hpp"
#include "core/vk_descriptors.hpp"
#include "push_data.hpp"
#include "util/vk_tracy.hpp"
#include "util/vk_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <numeric>
#include <stdexcept>

namespace
{
    constexpr vk::ShaderStageFlags kDgcMeshStages =
        vk::ShaderStageFlagBits::eMeshEXT | vk::ShaderStageFlagBits::eFragment;

    // each group's preprocess region starts page-aligned; NVIDIA 616.92 faulted with the reported 256 B alignment
    constexpr vk::DeviceSize kPreprocessRegionAlignment = 4096;

    constexpr vk::DeviceSize alignUp(vk::DeviceSize size, vk::DeviceSize alignment)
    {
        return (size + alignment - 1) / alignment * alignment;
    }

    constexpr std::array<MeshStateGroup, kMeshStateGroupCount> kMeshStateGroups = {MeshStateGroup::SingleSided,
                                                                                   MeshStateGroup::DoubleSided};
} // namespace

DeviceGeneratedCommands::DeviceGeneratedCommands(Device& device, ResourceManager& resourceManager,
                                                 DescriptorManager& descriptorManager, ShaderObjects& shaderObjects) :
    device(device), resourceManager(resourceManager), descriptorManager(descriptorManager),
    shaderObjects(shaderObjects)
{
}

DeviceGeneratedCommands::~DeviceGeneratedCommands()
{
    for (uint32_t frameSlot = 0; frameSlot < MAX_FRAMES_IN_FLIGHT; ++frameSlot) {
        destroyFrameResources(frameSlot);
    }
}

void DeviceGeneratedCommands::init()
{
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
    for (size_t variant = 0; variant < kRasterVariantCount; ++variant) {
        const ShaderObjects::MeshShaderPair& shaderPair = shaderObjects.meshShaders[variant];
        generatedShaders[variant] = {*shaderPair.mesh, *shaderPair.fragment};
        generatedShaderInfos[variant] = vk::GeneratedCommandsShaderInfoEXT{
            .shaderCount = static_cast<uint32_t>(generatedShaders[variant].size()),
            .pShaders = generatedShaders[variant].data(),
        };
    }
    createLayout();
    createStateCommandBuffers();

    logInfo(std::format("DGC ready: stride={} maxSequences={}", sizeof(MeshDgcSequence),
                        props.maxIndirectSequenceCount),
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
}

// separate state buffers as in nvpro vk_device_generated_cmds; Nsight 2026.3.1 replay faults when
// stateCommandBuffer is the recording command buffer itself
void DeviceGeneratedCommands::createStateCommandBuffers()
{
    statePool = vk::raii::CommandPool(device.vkdevice, {.queueFamilyIndex = device.graphicsIndex});
    setDebugName(device.vkdevice, statePool, "DgcStateCommandPool");

    const vk::CommandBufferAllocateInfo allocInfo{
        .commandPool = *statePool,
        .level = vk::CommandBufferLevel::eSecondary,
        .commandBufferCount = static_cast<uint32_t>(kRasterVariantCount * kMeshStateGroupCount),
    };
    stateCommandBuffers = vk::raii::CommandBuffers(device.vkdevice, allocInfo);

    // shaders and heaps are created once before init, so recording once is enough
    const vk::CommandBufferInheritanceInfo inheritance{};
    for (size_t variant = 0; variant < kRasterVariantCount; ++variant) {
        const auto rasterVariant = static_cast<RasterVariant>(variant);
        for (const MeshStateGroup group : kMeshStateGroups) {
            const vk::raii::CommandBuffer& state = stateCommandBuffer(rasterVariant, group);
            setDebugName(device.vkdevice, state,
                         std::format("DgcStateCommandBuffer{}_{}", variant, std::to_underlying(group)));
            state.begin({.pInheritanceInfo = &inheritance});
            shaderObjects.bindMeshState(state, rasterVariant, group);
            state.bindResourceHeapEXT(descriptorManager.resourceHeapInfo);
            state.bindSamplerHeapEXT(descriptorManager.samplerHeapInfo);
        }
    }
}

const vk::raii::CommandBuffer& DeviceGeneratedCommands::stateCommandBuffer(RasterVariant variant,
                                                                         MeshStateGroup group) const
{
    return stateCommandBuffers[std::to_underlying(variant) * kMeshStateGroupCount + std::to_underlying(group)];
}

// requirements depend on the shader set; one buffer serves every variant
vk::DeviceSize DeviceGeneratedCommands::queryPreprocessSize(uint32_t capacity) const
{
    vk::DeviceSize size = 0;
    for (const vk::GeneratedCommandsShaderInfoEXT& shaderInfo : generatedShaderInfos) {
        const vk::GeneratedCommandsMemoryRequirementsInfoEXT memInfo{
            .pNext = &shaderInfo,
            .indirectCommandsLayout = *layout,
            .maxSequenceCount = capacity,
            .maxDrawCount = 1,
        };
        const vk::MemoryRequirements2 requirements = device.vkdevice.getGeneratedCommandsMemoryRequirementsEXT(memInfo);
        size = std::max(size, requirements.memoryRequirements.size);
    }
    return size;
}

void DeviceGeneratedCommands::prepareFrame(uint32_t frameSlot,
                                           const std::array<uint32_t, kMeshStateGroupCount>& maxSequences,
                                           vk::DeviceAddress countersAddress)
{
    frame = frameSlot;
    groupMaxSequences = maxSequences;
    counters = countersAddress;
    std::exclusive_scan(maxSequences.begin(), maxSequences.end(), groupOffsets.begin(), 0u);
    const uint32_t total = groupOffsets.back() + maxSequences.back();
    if (total > 0) {
        ensureFrameCapacity(frameSlot, total);
    }
}

void DeviceGeneratedCommands::ensureFrameCapacity(uint32_t frameSlot, uint32_t minSequences)
{
    if (minSequences <= sequenceCapacity[frameSlot]) {
        return;
    }
    // zone only on growth: reallocation stalls the frame
    ZoneScopedN("DeviceGeneratedCommands::ensureFrameCapacity");

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
    ZoneScopedN("DeviceGeneratedCommands::allocateSequenceBuffer");
    const vk::DeviceSize bufferSize = static_cast<vk::DeviceSize>(capacity) * sizeof(MeshDgcSequence);
    vk::raii::Buffer buffer({});
    VmaAllocation memory = nullptr;
    createBuffer(bufferSize, vk::BufferUsageFlagBits2::eIndirectBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eDeviceLocal, buffer, memory, resourceManager.allocator.allocator,
                 device.vkdevice, device.queueFamilyIndices, std::format("DgcSequences_{}", frameSlot));

    sequenceBuffers[frameSlot] = std::move(buffer);
    sequenceMemory[frameSlot] = memory;
    sequenceAddresses[frameSlot] = device.vkdevice.getBufferAddress({.buffer = *sequenceBuffers[frameSlot]});
    sequenceCapacity[frameSlot] = capacity;
    tracyResourceAlloc(static_cast<VkBuffer>(*sequenceBuffers[frameSlot]), static_cast<size_t>(bufferSize),
                       "GPU/DgcSequences");
}

void DeviceGeneratedCommands::allocatePreprocessBuffer(uint32_t frameSlot, uint32_t capacity)
{
    ZoneScopedN("DeviceGeneratedCommands::allocatePreprocessBuffer");
    // each region fits the whole capacity, so any split between groups fits without requerying
    const vk::DeviceSize regionSize = queryPreprocessSize(capacity);
    if (regionSize == 0) {
        preprocessGroupStrides[frameSlot] = 0;
        preprocessAddresses[frameSlot] = 0;
        return;
    }
    const vk::DeviceSize regionStride = alignUp(regionSize, kPreprocessRegionAlignment);
    const vk::DeviceSize preprocessSize = regionStride * kMeshStateGroupCount;

    vk::raii::Buffer buffer({});
    VmaAllocation memory = nullptr;
    // own block puts the buffer at offset 0, so every region is 4 KiB aligned
    createBuffer(preprocessSize,
                 vk::BufferUsageFlagBits2::ePreprocessBufferEXT | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eDeviceLocal, buffer, memory, resourceManager.allocator.allocator,
                 device.vkdevice, device.queueFamilyIndices, std::format("DgcPreprocess_{}", frameSlot),
                 VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);

    preprocessBuffers[frameSlot] = std::move(buffer);
    preprocessMemory[frameSlot] = memory;
    preprocessAddresses[frameSlot] = device.vkdevice.getBufferAddress({.buffer = *preprocessBuffers[frameSlot]});
    preprocessGroupStrides[frameSlot] = regionStride;
    tracyResourceAlloc(static_cast<VkBuffer>(*preprocessBuffers[frameSlot]), static_cast<size_t>(preprocessSize),
                       "GPU/DgcPreprocess");
}

void DeviceGeneratedCommands::destroyFrameResources(uint32_t frameSlot)
{
    ZoneScopedN("DeviceGeneratedCommands::destroyFrameResources");
    destroyVmaBuffer(resourceManager.allocator.allocator, sequenceBuffers[frameSlot], sequenceMemory[frameSlot],
                     "GPU/DgcSequences");
    sequenceAddresses[frameSlot] = 0;
    sequenceCapacity[frameSlot] = 0;

    destroyVmaBuffer(resourceManager.allocator.allocator, preprocessBuffers[frameSlot], preprocessMemory[frameSlot],
                     "GPU/DgcPreprocess");
    preprocessAddresses[frameSlot] = 0;
    preprocessGroupStrides[frameSlot] = 0;
}

vk::GeneratedCommandsInfoEXT DeviceGeneratedCommands::generatedCommandsInfo(MeshStateGroup group,
                                                                            RasterVariant variant) const
{
    const uint32_t index = std::to_underlying(group);
    const vk::DeviceAddress preprocessBase = preprocessAddresses[frame];
    return vk::GeneratedCommandsInfoEXT{
        .pNext = &generatedShaderInfos[std::to_underlying(variant)],
        .shaderStages = shaderStages,
        .indirectExecutionSet = nullptr,
        .indirectCommandsLayout = *layout,
        .indirectAddress = sequenceAddresses[frame] + groupOffsets[index] * sizeof(MeshDgcSequence),
        .indirectAddressSize = static_cast<vk::DeviceSize>(groupMaxSequences[index]) * sizeof(MeshDgcSequence),
        .preprocessAddress = preprocessBase == 0 ? 0 : preprocessBase + index * preprocessGroupStrides[frame],
        .preprocessSize = preprocessGroupStrides[frame],
        .maxSequenceCount = groupMaxSequences[index],
        .sequenceCountAddress = counters + offsetof(GpuCullCounters, sequenceCount) + index * sizeof(uint32_t),
        .maxDrawCount = 1,
    };
}

void DeviceGeneratedCommands::recordPreprocess(vk::raii::CommandBuffer& cmd, RasterVariant variant) const
{
    ZoneScopedN("DeviceGeneratedCommands::recordPreprocess");
    bool hasWork = false;
    // groups use disjoint preprocess regions, so no barrier is needed between them
    for (const MeshStateGroup group : kMeshStateGroups) {
        if (groupMaxSequences[std::to_underlying(group)] == 0) {
            continue;
        }
        cmd.preprocessGeneratedCommandsEXT(generatedCommandsInfo(group, variant), *stateCommandBuffer(variant, group));
        hasWork = true;
    }
    if (!hasWork) {
        return;
    }

    const vk::MemoryBarrier2 preprocessBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eCommandPreprocessEXT,
        .srcAccessMask = vk::AccessFlagBits2::eCommandPreprocessWriteEXT,
        .dstStageMask = vk::PipelineStageFlagBits2::eCommandPreprocessEXT | vk::PipelineStageFlagBits2::eDrawIndirect,
        .dstAccessMask = vk::AccessFlagBits2::eCommandPreprocessReadEXT | vk::AccessFlagBits2::eIndirectCommandRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &preprocessBarrier});
}

void DeviceGeneratedCommands::recordExecute(vk::raii::CommandBuffer& cmd, RasterVariant variant) const
{
    ZoneScopedN("DeviceGeneratedCommands::recordExecute");
    for (const MeshStateGroup group : kMeshStateGroups) {
        if (groupMaxSequences[std::to_underlying(group)] == 0) {
            continue;
        }
        shaderObjects.bindMeshState(cmd, variant, group);
        cmd.executeGeneratedCommandsEXT(vk::True, generatedCommandsInfo(group, variant));
    }
}
