#include "vk_dgc.hpp"

#include "../static_headers/logger.hpp"
#include "core/vk_descriptors.hpp"
#include "push_data.hpp"
#include "util/vk_tracy.hpp"
#include "util/debug.hpp"
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

    constexpr std::array<RasterBin, kRasterBinCount> kRasterBins = {RasterBin::Opaque,
                                                                                   RasterBin::AlphaTested};
} // namespace

DeviceGeneratedCommands::DeviceGeneratedCommands(Device& device, const VkAllocator& allocator,
                                                 DescriptorManager& descriptorManager, ShaderObjects& shaderObjects) :
    device(device), allocator(allocator), descriptorManager(descriptorManager),
    shaderObjects(shaderObjects)
{
}

DeviceGeneratedCommands::~DeviceGeneratedCommands()
{
    for (uint32_t frameSlot = 0; frameSlot < MAX_FRAMES_IN_FLIGHT; ++frameSlot) {
        destroyFrameResources(frameSlot);
    }
    destroyVmaBuffer(allocator.allocator, resolvePreprocessBuffer, resolvePreprocessMemory);
}

void DeviceGeneratedCommands::init()
{
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
    for (const RasterBin group : kRasterBins) {
        const uint32_t index = std::to_underlying(group);
        const ShaderObjects::MeshShaderPair& shaders = shaderObjects.meshShaders(group);
        generatedShaders[index] = {*shaders.mesh, *shaders.fragment};
        generatedShaderInfos[index] = vk::GeneratedCommandsShaderInfoEXT{
            .shaderCount = static_cast<uint32_t>(generatedShaders[index].size()),
            .pShaders = generatedShaders[index].data(),
        };
    }
    createLayout();
    createStateCommandBuffer();
    createResolveCommands();

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

// separate state buffer as in nvpro vk_device_generated_cmds; Nsight 2026.3.1 replay faults when
// stateCommandBuffer is the recording command buffer itself
void DeviceGeneratedCommands::createStateCommandBuffer()
{
    statePool = vk::raii::CommandPool(device.vkdevice, {.queueFamilyIndex = device.graphicsIndex});
    setDebugName(device.vkdevice, statePool, "DgcStateCommandPool");

    const vk::CommandBufferAllocateInfo allocInfo{
        .commandPool = *statePool,
        .level = vk::CommandBufferLevel::eSecondary,
        .commandBufferCount = kRasterBinCount,
    };
    vk::raii::CommandBuffers buffers(device.vkdevice, allocInfo);

    // shaders and heaps are created once before init, so recording once is enough
    const vk::CommandBufferInheritanceInfo inheritance{};
    for (const RasterBin group : kRasterBins) {
        const uint32_t index = std::to_underlying(group);
        vk::raii::CommandBuffer& state = stateCommandBuffers[index];
        state = std::move(buffers[index]);
        setDebugName(device.vkdevice, state, std::format("DgcStateCommandBuffer_{}", index));
        state.begin({.pInheritanceInfo = &inheritance});
        shaderObjects.bindMeshState(state, group);
        state.bindResourceHeapEXT(descriptorManager.resourceHeapInfo);
        state.bindSamplerHeapEXT(descriptorManager.samplerHeapInfo);
    }
}

// the resolve shaders were created indirect-bindable; the execution set starts with MaterialBin::Background and gets
// the other bins by index, as every initial shader must have a distinct stage
void DeviceGeneratedCommands::createResolveCommands()
{
    const auto& props = device.capabilities.deviceGeneratedCommands;
    constexpr uint32_t kResolveTokenCount = 3;
    if (!(props.supportedIndirectCommandsShaderStages & vk::ShaderStageFlagBits::eCompute) ||
        props.maxIndirectCommandsTokenCount < kResolveTokenCount ||
        props.maxIndirectCommandsTokenOffset < offsetof(ResolveDgcSequence, dispatch) ||
        props.maxIndirectCommandsIndirectStride < sizeof(ResolveDgcSequence)) {
        throw std::runtime_error("DGC limits below the material-binned resolve layout");
    }

    const auto& shaders = shaderObjects.resolveShaders;
    const vk::raii::ShaderEXT& initialShader = shaders[std::to_underlying(MaterialBin::Background)];
    const vk::ShaderEXT initialHandle = *initialShader;
    // heap shaders: no set layouts (VUID-VkIndirectExecutionSetShaderInfoEXT-pInitialShaders-11323), push data only
    const vk::IndirectExecutionSetShaderInfoEXT shaderInfo{
        .shaderCount = 1,
        .pInitialShaders = &initialHandle,
        .maxShaderCount = kMaterialBinCount,
    };
    resolveExecutionSet = vk::raii::IndirectExecutionSetEXT(
        device.vkdevice,
        vk::IndirectExecutionSetCreateInfoEXT{.type = vk::IndirectExecutionSetInfoTypeEXT::eShaderObjects,
                                              .info = &shaderInfo});
    setDebugName(device.vkdevice, resolveExecutionSet, "DgcResolveExecutionSet");
    std::array<vk::WriteIndirectExecutionSetShaderEXT, kMaterialBinCount - 1> writes{};
    for (uint32_t bin = 1; bin < kMaterialBinCount; ++bin) {
        writes[bin - 1] = vk::WriteIndirectExecutionSetShaderEXT{.index = bin, .shader = *shaders[bin]};
    }
    resolveExecutionSet.updateShader(writes);

    const vk::IndirectCommandsExecutionSetTokenEXT executionSetToken{
        .type = vk::IndirectExecutionSetInfoTypeEXT::eShaderObjects,
        .shaderStages = vk::ShaderStageFlagBits::eCompute,
    };
    const vk::IndirectCommandsPushConstantTokenEXT pushToken{
        .updateRange =
            {
                // VUID-VkIndirectCommandsLayoutTokenEXT-type-11333: PUSH_DATA tokens take every stage
                .stageFlags = vk::ShaderStageFlagBits::eAll,
                .offset = 0,
                .size = static_cast<uint32_t>(sizeof(ResolvePushData)),
            },
    };
    // the execution-set token comes first (VUID-VkIndirectCommandsLayoutCreateInfoEXT-pTokens-11139)
    const std::array<vk::IndirectCommandsLayoutTokenEXT, kResolveTokenCount> tokens = {
        vk::IndirectCommandsLayoutTokenEXT{
            .type = vk::IndirectCommandsTokenTypeEXT::eExecutionSet,
            .data = vk::IndirectCommandsTokenDataEXT{&executionSetToken},
            .offset = static_cast<uint32_t>(offsetof(ResolveDgcSequence, executionSetIndex)),
        },
        vk::IndirectCommandsLayoutTokenEXT{
            .type = vk::IndirectCommandsTokenTypeEXT::ePushData,
            .data = vk::IndirectCommandsTokenDataEXT{&pushToken},
            .offset = static_cast<uint32_t>(offsetof(ResolveDgcSequence, pushData)),
        },
        vk::IndirectCommandsLayoutTokenEXT{
            .type = vk::IndirectCommandsTokenTypeEXT::eDispatch,
            .offset = static_cast<uint32_t>(offsetof(ResolveDgcSequence, dispatch)),
        },
    };
    resolveLayout = vk::raii::IndirectCommandsLayoutEXT(
        device.vkdevice, vk::IndirectCommandsLayoutCreateInfoEXT{
                             .flags = vk::IndirectCommandsLayoutUsageFlagBitsEXT::eExplicitPreprocess,
                             .shaderStages = vk::ShaderStageFlagBits::eCompute,
                             .indirectStride = static_cast<uint32_t>(sizeof(ResolveDgcSequence)),
                             .pipelineLayout = nullptr,
                             .tokenCount = static_cast<uint32_t>(tokens.size()),
                             .pTokens = tokens.data(),
                         });
    setDebugName(device.vkdevice, resolveLayout, "DgcResolveLayout");

    vk::raii::CommandBuffers buffers(device.vkdevice, vk::CommandBufferAllocateInfo{
                                                          .commandPool = *statePool,
                                                          .level = vk::CommandBufferLevel::eSecondary,
                                                          .commandBufferCount = 1,
                                                      });
    resolveStateCommandBuffer = std::move(buffers.front());
    setDebugName(device.vkdevice, resolveStateCommandBuffer, "DgcResolveStateCommandBuffer");
    const vk::CommandBufferInheritanceInfo inheritance{};
    resolveStateCommandBuffer.begin({.pInheritanceInfo = &inheritance});
    ShaderObjects::bindComputeShader(resolveStateCommandBuffer, initialShader);
    resolveStateCommandBuffer.bindResourceHeapEXT(descriptorManager.resourceHeapInfo);
    resolveStateCommandBuffer.bindSamplerHeapEXT(descriptorManager.samplerHeapInfo);

    // sized for every bin at once, the most binArgsMain writes
    const vk::GeneratedCommandsMemoryRequirementsInfoEXT memInfo{
        .indirectExecutionSet = *resolveExecutionSet,
        .indirectCommandsLayout = *resolveLayout,
        .maxSequenceCount = kMaterialBinCount,
        .maxDrawCount = 1,
    };
    const vk::DeviceSize preprocessSize =
        device.vkdevice.getGeneratedCommandsMemoryRequirementsEXT(memInfo).memoryRequirements.size;
    if (preprocessSize == 0) {
        return;
    }
    resolvePreprocessSize = alignUp(preprocessSize, kPreprocessRegionAlignment);
    createBuffer(resolvePreprocessSize,
                 vk::BufferUsageFlagBits2::ePreprocessBufferEXT | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eDeviceLocal, resolvePreprocessBuffer, resolvePreprocessMemory,
                 allocator.allocator, device.vkdevice, device.queueFamilyIndices, "DgcResolvePreprocess", 0,
                 kPreprocessRegionAlignment);
    resolvePreprocessAddress = device.vkdevice.getBufferAddress({.buffer = *resolvePreprocessBuffer});
}

// the largest bin's: every (phase, bin) region has the same stride
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
        size = std::max(size,
                        device.vkdevice.getGeneratedCommandsMemoryRequirementsEXT(memInfo).memoryRequirements.size);
    }
    return size;
}

void DeviceGeneratedCommands::prepareFrame(uint32_t frameSlot,
                                           const std::array<uint32_t, kRasterBinCount>& maxSequences,
                                           vk::DeviceAddress countersAddress)
{
    frame = frameSlot;
    binMaxSequences = maxSequences;
    counters = countersAddress;
    std::exclusive_scan(maxSequences.begin(), maxSequences.end(), groupOffsets.begin(), 0u);
    phaseSequences = groupOffsets.back() + maxSequences.back();
    if (phaseSequences > 0) {
        ensureFrameCapacity(frameSlot, phaseSequences * kCullPhaseCount);
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
                 vk::MemoryPropertyFlagBits::eDeviceLocal, buffer, memory, allocator.allocator,
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
    const vk::DeviceSize preprocessSize = regionStride * kCullPhaseCount * kRasterBinCount;

    vk::raii::Buffer buffer({});
    VmaAllocation memory = nullptr;
    // suballocated at a page-aligned start, so every region is 4 KiB aligned
    createBuffer(preprocessSize,
                 vk::BufferUsageFlagBits2::ePreprocessBufferEXT | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eDeviceLocal, buffer, memory, allocator.allocator, device.vkdevice,
                 device.queueFamilyIndices, std::format("DgcPreprocess_{}", frameSlot), 0, kPreprocessRegionAlignment);

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
    destroyVmaBuffer(allocator.allocator, sequenceBuffers[frameSlot], sequenceMemory[frameSlot],
                     "GPU/DgcSequences");
    sequenceAddresses[frameSlot] = 0;
    sequenceCapacity[frameSlot] = 0;

    destroyVmaBuffer(allocator.allocator, preprocessBuffers[frameSlot], preprocessMemory[frameSlot],
                     "GPU/DgcPreprocess");
    preprocessAddresses[frameSlot] = 0;
    preprocessGroupStrides[frameSlot] = 0;
}

vk::GeneratedCommandsInfoEXT DeviceGeneratedCommands::generatedCommandsInfo(CullPhase phase, RasterBin group) const
{
    const uint32_t index = std::to_underlying(group);
    const uint32_t region = std::to_underlying(phase) * kRasterBinCount + index;
    const vk::DeviceAddress preprocessBase = preprocessAddresses[frame];
    const vk::DeviceAddress phaseCounters = counters + std::to_underlying(phase) * sizeof(GpuCullCounters);
    return vk::GeneratedCommandsInfoEXT{
        .pNext = &generatedShaderInfos[index],
        .shaderStages = shaderStages,
        .indirectExecutionSet = nullptr,
        .indirectCommandsLayout = *layout,
        .indirectAddress = sequenceAddresses[frame] + binSequenceOffset(phase, group) * sizeof(MeshDgcSequence),
        .indirectAddressSize = static_cast<vk::DeviceSize>(binMaxSequences[index]) * sizeof(MeshDgcSequence),
        .preprocessAddress = preprocessBase == 0 ? 0 : preprocessBase + region * preprocessGroupStrides[frame],
        .preprocessSize = preprocessGroupStrides[frame],
        .maxSequenceCount = binMaxSequences[index],
        .sequenceCountAddress = phaseCounters + offsetof(GpuCullCounters, sequenceCount) + index * sizeof(uint32_t),
        .maxDrawCount = 1,
    };
}

void DeviceGeneratedCommands::recordPreprocess(vk::raii::CommandBuffer& cmd, CullPhase phase) const
{
    ZoneScopedN("DeviceGeneratedCommands::recordPreprocess");
    bool hasWork = false;
    // groups use disjoint preprocess regions, so no barrier is needed between them
    for (const RasterBin group : kRasterBins) {
        if (binMaxSequences[std::to_underlying(group)] == 0) {
            continue;
        }
        cmd.preprocessGeneratedCommandsEXT(generatedCommandsInfo(phase, group),
                                           *stateCommandBuffers[std::to_underlying(group)]);
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

void DeviceGeneratedCommands::recordExecute(vk::raii::CommandBuffer& cmd, CullPhase phase, RasterBin group) const
{
    ZoneScopedN("DeviceGeneratedCommands::recordExecute");
    if (binMaxSequences[std::to_underlying(group)] == 0) {
        return;
    }
    // execute leaves the bound graphics state undefined; rebind exactly what preprocess saw
    shaderObjects.bindMeshState(cmd, group);
    cmd.executeGeneratedCommandsEXT(vk::True, generatedCommandsInfo(phase, group));
}

vk::GeneratedCommandsInfoEXT DeviceGeneratedCommands::resolveCommandsInfo(vk::DeviceAddress materialBins) const
{
    return vk::GeneratedCommandsInfoEXT{
        .shaderStages = vk::ShaderStageFlagBits::eCompute,
        .indirectExecutionSet = *resolveExecutionSet,
        .indirectCommandsLayout = *resolveLayout,
        .indirectAddress = materialBins + offsetof(GpuMaterialBins, sequences),
        .indirectAddressSize = sizeof(GpuMaterialBins::sequences),
        .preprocessAddress = resolvePreprocessAddress,
        .preprocessSize = resolvePreprocessSize,
        .maxSequenceCount = kMaterialBinCount,
        .sequenceCountAddress = materialBins + offsetof(GpuMaterialBins, sequenceCount),
        .maxDrawCount = 1,
    };
}

void DeviceGeneratedCommands::recordResolvePreprocess(const vk::raii::CommandBuffer& cmd,
                                                      vk::DeviceAddress materialBins) const
{
    ZoneScopedN("DeviceGeneratedCommands::recordResolvePreprocess");
    cmd.preprocessGeneratedCommandsEXT(resolveCommandsInfo(materialBins), *resolveStateCommandBuffer);
    const vk::MemoryBarrier2 preprocessBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eCommandPreprocessEXT,
        .srcAccessMask = vk::AccessFlagBits2::eCommandPreprocessWriteEXT,
        .dstStageMask = vk::PipelineStageFlagBits2::eCommandPreprocessEXT | vk::PipelineStageFlagBits2::eDrawIndirect,
        .dstAccessMask = vk::AccessFlagBits2::eCommandPreprocessReadEXT | vk::AccessFlagBits2::eIndirectCommandRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &preprocessBarrier});
}

void DeviceGeneratedCommands::recordResolveExecute(const vk::raii::CommandBuffer& cmd,
                                                   vk::DeviceAddress materialBins) const
{
    ZoneScopedN("DeviceGeneratedCommands::recordResolveExecute");
    // the state the preprocess saw; execute leaves the bound compute shader undefined afterwards
    ShaderObjects::bindComputeShader(cmd, shaderObjects.resolveShaders[std::to_underlying(MaterialBin::Background)]);
    cmd.executeGeneratedCommandsEXT(vk::True, resolveCommandsInfo(materialBins));
}
