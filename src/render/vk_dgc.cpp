#include "vk_dgc.hpp"

#include "../static_headers/logger.hpp"
#include "core/vk_descriptors.hpp"
#include "push_data.hpp"
#include "util/vk_tracy.hpp"
#include "util/vk_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <stdexcept>
#include <type_traits>

namespace
{
    // packed size is 28 (16-byte push + 12-byte draw); 16-byte stride keeps the
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

    // NVIDIA 616.92: one execute with > 96 sequences drops/garbles sequences nondeterministically
    // (and faults with a non-4 KiB preprocess offset); split into chunks of at most 96
    constexpr uint32_t kMaxSequencesPerExecute = 95;
    // each chunk's preprocess region starts page-aligned; reported 256 B alignment is not enough
    constexpr vk::DeviceSize kPreprocessChunkAlignment = 4096;

    constexpr vk::DeviceSize alignUp(vk::DeviceSize size, vk::DeviceSize alignment)
    {
        return (size + alignment - 1) / alignment * alignment;
    }

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
                                                 DescriptorManager& descriptorManager, ShaderObjects& shaderObjects) :
    device(device), resourceManager(resourceManager), descriptorManager(descriptorManager),
    shaderObjects(shaderObjects)
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
    generatedShaders = {*shaderObjects.meshShader, *shaderObjects.fragmentShader};
    generatedShaderInfo = vk::GeneratedCommandsShaderInfoEXT{
        .shaderCount = static_cast<uint32_t>(generatedShaders.size()),
        .pShaders = generatedShaders.data(),
    };
    createLayout();
    createStateCommandBuffer();

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

// separate state buffer as in nvpro vk_device_generated_cmds; Nsight 2026.3.1 replay faults when
// stateCommandBuffer is the recording command buffer itself
void DeviceGeneratedCommands::createStateCommandBuffer()
{
    statePool = vk::raii::CommandPool(device.vkdevice, {.queueFamilyIndex = device.graphicsIndex});
    setDebugName(device.vkdevice, statePool, "DgcStateCommandPool");

    const vk::CommandBufferAllocateInfo allocInfo{
        .commandPool = *statePool,
        .level = vk::CommandBufferLevel::eSecondary,
        .commandBufferCount = 1,
    };
    stateCommandBuffer = std::move(vk::raii::CommandBuffers(device.vkdevice, allocInfo).front());
    setDebugName(device.vkdevice, stateCommandBuffer, "DgcStateCommandBuffer");

    // shaders and heaps are created once before init, so recording once is enough
    const vk::CommandBufferInheritanceInfo inheritance{};
    stateCommandBuffer.begin({.pInheritanceInfo = &inheritance});
    shaderObjects.bindMeshState(stateCommandBuffer);
    stateCommandBuffer.bindResourceHeapEXT(descriptorManager.resourceHeapInfo);
    stateCommandBuffer.bindSamplerHeapEXT(descriptorManager.samplerHeapInfo);
}

vk::DeviceSize DeviceGeneratedCommands::queryPreprocessSize(uint32_t capacity) const
{
    const vk::GeneratedCommandsMemoryRequirementsInfoEXT memInfo{
        .pNext = &generatedShaderInfo,
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
                 vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, buffer, memory,
                 resourceManager.allocator.allocator, device.vkdevice, device.queueFamilyIndices,
                 std::format("DgcSequences_{}", frameSlot));

    sequenceBuffers[frameSlot] = std::move(buffer);
    sequenceMemory[frameSlot] = memory;
    void* mapped = nullptr;
    vmaMapMemory(resourceManager.allocator.allocator, memory, &mapped);
    sequenceMapped[frameSlot] = mapped;
    sequenceAddresses[frameSlot] = device.vkdevice.getBufferAddress({.buffer = *sequenceBuffers[frameSlot]});
    sequenceCapacity[frameSlot] = capacity;
    tracyResourceAlloc(static_cast<VkBuffer>(*sequenceBuffers[frameSlot]), static_cast<size_t>(bufferSize),
                       "GPU/DgcSequences");
}

void DeviceGeneratedCommands::allocatePreprocessBuffer(uint32_t frameSlot, uint32_t capacity)
{
    ZoneScopedN("DeviceGeneratedCommands::allocatePreprocessBuffer");
    const vk::DeviceSize chunkSize = queryPreprocessSize(std::min(capacity, kMaxSequencesPerExecute));
    if (chunkSize == 0) {
        preprocessSizes[frameSlot] = 0;
        preprocessChunkStrides[frameSlot] = 0;
        preprocessAddresses[frameSlot] = 0;
        return;
    }
    const vk::DeviceSize chunkStride = alignUp(chunkSize, kPreprocessChunkAlignment);
    const uint32_t chunkCount = (capacity + kMaxSequencesPerExecute - 1) / kMaxSequencesPerExecute;
    const vk::DeviceSize preprocessSize = chunkStride * chunkCount;

    vk::raii::Buffer buffer({});
    VmaAllocation memory = nullptr;
    // own block puts the buffer at offset 0, so every chunk region is 4 KiB aligned
    createBuffer(preprocessSize,
                 vk::BufferUsageFlagBits2::ePreprocessBufferEXT | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eDeviceLocal, buffer, memory, resourceManager.allocator.allocator,
                 device.vkdevice, device.queueFamilyIndices, std::format("DgcPreprocess_{}", frameSlot),
                 VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);

    preprocessBuffers[frameSlot] = std::move(buffer);
    preprocessMemory[frameSlot] = memory;
    preprocessAddresses[frameSlot] = device.vkdevice.getBufferAddress({.buffer = *preprocessBuffers[frameSlot]});
    preprocessSizes[frameSlot] = preprocessSize;
    preprocessChunkStrides[frameSlot] = chunkStride;
    tracyResourceAlloc(static_cast<VkBuffer>(*preprocessBuffers[frameSlot]), static_cast<size_t>(preprocessSize),
                       "GPU/DgcPreprocess");
}

void DeviceGeneratedCommands::destroyFrameResources(uint32_t frameSlot)
{
    ZoneScopedN("DeviceGeneratedCommands::destroyFrameResources");
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
    preprocessChunkStrides[frameSlot] = 0;
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
    const uint32_t entityCount = storage.size();
    if (entityCount == 0) {
        return;
    }

    const auto& geometry = resourceManager.geometryStore;
    const uint32_t maxGroupsX = maxMeshGroupsX(device.capabilities);

    // visits every (node instance, primitive) with meshlets; a mesh placed by n nodes is visited n times
    auto forEachPrimitive = [&](auto&& visit) {
        for (EntityId id = 0; id < entityCount; ++id) {
            // an entity whose load is not flushed yet has no GPU model
            if ((storage.flags[id] & EntityFlag::Active) == 0 || storage.modelIds[id] == kInvalidModelId) {
                continue;
            }
            const ElementRange nodes = resourceManager.modelAllocations[storage.modelIds[id]].nodes;
            for (uint32_t k = 0; k < nodes.count; ++k) {
                const NodeDraw& node = geometry.nodeDraws[nodes.first + k];
                const uint32_t nodeInstance = resourceManager.entityNodeInstanceFirst[id] + k;
                for (uint32_t p = node.firstPrimitive; p < node.firstPrimitive + node.primitiveCount; ++p) {
                    const uint32_t meshletCount = geometry.primitiveDraws[p].meshlets.meshletCount;
                    if (meshletCount > 0) {
                        visit(nodeInstance, p, meshletCount);
                    }
                }
            }
        }
    };

    uint32_t needed = 0;
    {
        ZoneScopedN("DeviceGeneratedCommands::updateSequences::count");
        forEachPrimitive([&](uint32_t, uint32_t, uint32_t meshletCount) {
            needed += sequenceCountForMeshlets(meshletCount, maxGroupsX);
        });
    }
    // the frame root reports 0 records when nothing draws
    GpuDrawRecord* records = resourceManager.mapDrawRecords(frameSlot, needed);
    if (needed == 0) {
        return;
    }

    ensureFrameCapacity(frameSlot, needed);
    auto* sequences = static_cast<MeshDgcSequence*>(sequenceMapped[frameSlot]);

    const vk::DeviceAddress frameRoot = resourceManager.frameRootAddress(frameSlot);
    // one draw record per sequence, same index
    auto fillSequences = [&](uint32_t nodeInstance, uint32_t primitive, uint32_t meshletCount) {
        // meshlet batches past maxMeshWorkGroupCount split into more sequences
        for (uint32_t offset = 0; offset < meshletCount; offset += maxGroupsX) {
            const uint32_t batch = std::min(meshletCount - offset, maxGroupsX);
            records[sequenceCount] = GpuDrawRecord{
                .nodeInstanceIndex = nodeInstance,
                .primitiveIndex = primitive,
                .meshletOffset = offset,
                .pad0 = 0,
            };
            sequences[sequenceCount] = MeshDgcSequence{
                .pushData = {.frameRoot = frameRoot, .drawIndex = sequenceCount, .meshletCount = batch},
                .draw = {.groupCountX = batch, .groupCountY = 1, .groupCountZ = 1},
            };
            ++sequenceCount;
        }
    };

    {
        ZoneScopedN("DeviceGeneratedCommands::updateSequences::fill");
        forEachPrimitive(fillSequences);
    }

    TracyPlot("Vulkan/DgcSequenceCount", static_cast<double>(sequenceCount));
}

void DeviceGeneratedCommands::fillGeneratedCommandsInfo(vk::GeneratedCommandsInfoEXT& info, uint32_t frameSlot,
                                                        uint32_t chunk) const
{
    const uint32_t firstSequence = chunk * kMaxSequencesPerExecute;
    const uint32_t chunkSequences = std::min(sequenceCount - firstSequence, kMaxSequencesPerExecute);

    info.pNext = &generatedShaderInfo;
    info.shaderStages = shaderStages;
    info.indirectExecutionSet = nullptr;
    info.indirectCommandsLayout = *layout;
    info.indirectAddress = sequenceAddresses[frameSlot] + firstSequence * sizeof(MeshDgcSequence);
    info.indirectAddressSize = static_cast<vk::DeviceSize>(chunkSequences) * sizeof(MeshDgcSequence);
    info.preprocessAddress =
        preprocessAddresses[frameSlot] == 0 ? 0 : preprocessAddresses[frameSlot] + chunk * preprocessChunkStrides[frameSlot];
    info.preprocessSize = preprocessChunkStrides[frameSlot];
    info.maxSequenceCount = chunkSequences;
    info.sequenceCountAddress = 0;
    info.maxDrawCount = 1;
}

uint32_t DeviceGeneratedCommands::chunkCount() const
{
    return (sequenceCount + kMaxSequencesPerExecute - 1) / kMaxSequencesPerExecute;
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

    // chunks use disjoint preprocess regions, so no barrier is needed between them
    for (uint32_t chunk = 0; chunk < chunkCount(); ++chunk) {
        vk::GeneratedCommandsInfoEXT info{};
        fillGeneratedCommandsInfo(info, recordedFrame, chunk);
        cmd.preprocessGeneratedCommandsEXT(info, *stateCommandBuffer);
    }

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

    for (uint32_t chunk = 0; chunk < chunkCount(); ++chunk) {
        vk::GeneratedCommandsInfoEXT info{};
        fillGeneratedCommandsInfo(info, recordedFrame, chunk);
        cmd.executeGeneratedCommandsEXT(explicitPreprocess ? vk::True : vk::False, info);
    }
}
