#include "vk_meshlet_culling.hpp"

#include "../static_headers/logger.hpp"
#include "push_data.hpp"
#include "util/vk_tracy.hpp"
#include "util/vk_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <numeric>
#include <stdexcept>

namespace
{
    constexpr const char* kCullViewTracyName = "GPU/CullView";
    constexpr const char* kCullCountersTracyName = "GPU/CullCounters";
    constexpr const char* kInstanceItemsTracyName = "GPU/InstanceCullItems";
    constexpr const char* kDrawRecordTemplatesTracyName = "GPU/DrawRecordTemplates";
    constexpr const char* kDrawRecordsTracyName = "GPU/DrawRecords";
    constexpr const char* kVisibleMeshletsTracyName = "GPU/VisibleMeshlets";
    // first allocation of the growable buffers, in elements
    constexpr uint32_t kMinCapacity = 64;
    constexpr uint32_t kMinVisibleCapacity = 1024;

    // Gribb-Hartmann on a [0, 1] depth projection; normalized so distances are in world units
    std::array<glm::vec4, 6> frustumPlanes(const glm::mat4& viewProj)
    {
        const glm::mat4 rows = glm::transpose(viewProj);
        std::array<glm::vec4, 6> planes{
            rows[3] + rows[0], rows[3] - rows[0], rows[3] + rows[1],
            rows[3] - rows[1], rows[2],           rows[3] - rows[2],
        };
        for (glm::vec4& plane : planes) {
            plane /= glm::length(glm::vec3(plane));
        }
        return planes;
    }

    void createMappedBuffer(HostBuffer& target, vk::DeviceSize size, vk::BufferUsageFlags2 usage,
                            vk::MemoryPropertyFlags properties, const Device& device, VmaAllocator allocator,
                            std::string_view debugName, const char* tracyName)
    {
        createBuffer(size, usage | vk::BufferUsageFlagBits2::eShaderDeviceAddress, properties, target.buffer,
                     target.memory, allocator, device.vkdevice, device.queueFamilyIndices, debugName,
                     VMA_ALLOCATION_CREATE_MAPPED_BIT);
        VmaAllocationInfo allocationInfo{};
        vmaGetAllocationInfo(allocator, target.memory, &allocationInfo);
        target.mapped = allocationInfo.pMappedData;
        target.address = device.vkdevice.getBufferAddress({.buffer = *target.buffer});
        tracyResourceAlloc(static_cast<VkBuffer>(*target.buffer), static_cast<size_t>(size), tracyName);
    }

    void createDeviceBuffer(DeviceBuffer& target, vk::DeviceSize size, const Device& device, VmaAllocator allocator,
                            std::string_view debugName, const char* tracyName)
    {
        createBuffer(size, vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                     vk::MemoryPropertyFlagBits::eDeviceLocal, target.buffer, target.memory, allocator,
                     device.vkdevice, device.queueFamilyIndices, debugName);
        target.address = device.vkdevice.getBufferAddress({.buffer = *target.buffer});
        target.capacityBytes = size;
        tracyResourceAlloc(static_cast<VkBuffer>(*target.buffer), static_cast<size_t>(size), tracyName);
    }

    // doubling growth; capacity is in elements
    uint32_t grownCapacity(uint32_t needed, uint32_t current, uint32_t minimum)
    {
        return std::max({needed, current * 2, minimum});
    }

    constexpr uint32_t divideRoundUp(uint32_t value, uint32_t divisor) { return (value + divisor - 1) / divisor; }
} // namespace

MeshletCulling::MeshletCulling(Device& device, ResourceManager& resourceManager, ShaderObjects& shaderObjects,
                               DeviceGeneratedCommands& dgc) :
    device(device), resourceManager(resourceManager), shaderObjects(shaderObjects), dgc(dgc)
{
    createFrameBuffers();
}

MeshletCulling::~MeshletCulling() { destroyFrameBuffers(); }

void MeshletCulling::createFrameBuffers()
{
    const VmaAllocator allocator = resourceManager.allocator.allocator;
    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        createMappedBuffer(frames[slot].cullView, sizeof(GpuCullView), vk::BufferUsageFlagBits2::eStorageBuffer,
                           vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
                           device, allocator, std::format("CullView_{}", slot), kCullViewTracyName);
        // indirect: the meshlet dispatch and the DGC sequence counts are read from here
        createMappedBuffer(frames[slot].counters, sizeof(GpuCullCounters),
                           vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eIndirectBuffer |
                               vk::BufferUsageFlagBits2::eTransferDst,
                           vk::MemoryPropertyFlagBits::eDeviceLocal | vk::MemoryPropertyFlagBits::eHostVisible |
                               vk::MemoryPropertyFlagBits::eHostCoherent,
                           device, allocator, std::format("CullCounters_{}", slot), kCullCountersTracyName);
    }
}

void MeshletCulling::destroyFrameBuffers()
{
    const VmaAllocator allocator = resourceManager.allocator.allocator;
    for (FrameBuffers& buffers : frames) {
        // persistently mapped (MAPPED_BIT): VMA unmaps on destroy
        destroyVmaBuffer(allocator, buffers.cullView.buffer, buffers.cullView.memory, kCullViewTracyName);
        destroyVmaBuffer(allocator, buffers.counters.buffer, buffers.counters.memory, kCullCountersTracyName);
        destroyVmaBuffer(allocator, buffers.instanceItems.buffer, buffers.instanceItems.memory,
                         kInstanceItemsTracyName);
        destroyVmaBuffer(allocator, buffers.drawRecordTemplates.buffer, buffers.drawRecordTemplates.memory,
                         kDrawRecordTemplatesTracyName);
        destroyVmaBuffer(allocator, buffers.drawRecords.buffer, buffers.drawRecords.memory, kDrawRecordsTracyName);
        destroyVmaBuffer(allocator, buffers.visibleMeshlets.buffer, buffers.visibleMeshlets.memory,
                         kVisibleMeshletsTracyName);
    }
}

void MeshletCulling::prepareFrame(uint32_t frameSlot, const Camera& camera)
{
    ZoneScopedN("MeshletCulling::prepareFrame");
    const auto start = std::chrono::steady_clock::now();
    frame = frameSlot;
    readBackCounters(frameSlot);

    if (templates.generation != resourceManager.nodeInstanceLayoutGeneration) {
        buildTemplates();
    }
    if (frames[frameSlot].generation != templates.generation) {
        uploadTemplates(frameSlot);
    }
    resourceManager.drawRecordAddresses[frameSlot] = frames[frameSlot].drawRecords.address;
    resourceManager.visibleMeshletAddresses[frameSlot] = frames[frameSlot].visibleMeshlets.address;

    // every template may survive, so the per-group meshlet totals bound the visible lists and sequences
    const uint32_t totalMeshlets = std::reduce(templates.groupMeshlets.begin(), templates.groupMeshlets.end(), 0u);
    const uint32_t perSequence = meshletsPerSequence(totalMeshlets);
    std::array<uint32_t, kMeshStateGroupCount> maxSequences{};
    std::ranges::transform(templates.groupMeshlets, maxSequences.begin(),
                           [perSequence](uint32_t meshlets) { return divideRoundUp(meshlets, perSequence); });
    dgc.prepareFrame(frameSlot, maxSequences, frames[frameSlot].counters.address);
    writeCullView(frameSlot, camera, perSequence);

    recorded[frameSlot] = RecordedFrame{
        .totals =
            {
                .instances = static_cast<uint32_t>(templates.instances.size()),
                .drawRecords = static_cast<uint32_t>(templates.drawRecords.size()),
                .totalMeshlets = totalMeshlets,
                .meshletsPerSequence = perSequence,
                .groupMeshlets = templates.groupMeshlets,
                .cpuPrepareMs =
                    std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count(),
            },
        .hasResults = true,
    };
}

void MeshletCulling::readBackCounters(uint32_t frameSlot)
{
    const RecordedFrame& record = recorded[frameSlot];
    if (!record.hasResults) {
        return;
    }
    // the slot's fence has signaled, so its counters are final
    const GpuCullCounters counters = *static_cast<const GpuCullCounters*>(frames[frameSlot].counters.mapped);
    stats = record.totals;
    stats.visibleDrawRecords = counters.meshletDispatch[0];
    stats.visibleMeshlets = counters.visibleMeshlets;
    stats.sequences = counters.sequenceCount;
    stats.instancesCulled = counters.instancesCulled;
    stats.testedMeshlets = counters.testedMeshlets;
    stats.frustumCulled = counters.frustumCulled;
    stats.coneCulled = counters.coneCulled;
    stats.unculledMeshlets = counters.unculledMeshlets;

    TracyPlot("Culling/VisibleMeshlets",
              static_cast<double>(std::reduce(stats.visibleMeshlets.begin(), stats.visibleMeshlets.end(), 0u)));
    TracyPlot("Culling/TotalMeshlets", static_cast<double>(stats.totalMeshlets));
    TracyPlot("Culling/InstancesCulled", static_cast<double>(stats.instancesCulled));
    TracyPlot("Culling/VisibleDrawRecords", static_cast<double>(stats.visibleDrawRecords));
    TracyPlot("Culling/FrustumCulled", static_cast<double>(stats.frustumCulled));
    TracyPlot("Culling/ConeCulled", static_cast<double>(stats.coneCulled));
}

MeshStateGroup MeshletCulling::stateGroupOf(const PrimitiveDraw& primitive) const
{
    const MaterialStore& materials = resourceManager.materialStore;
    const uint32_t materialId = primitive.materialId == kNoneIndex ? materials.defaultMaterialId() : primitive.materialId;
    const bool isDoubleSided = (materials.infos[materialId].material.flags & GpuMaterialFlag::DoubleSided) != 0;
    return isDoubleSided ? MeshStateGroup::DoubleSided : MeshStateGroup::SingleSided;
}

// one item per node instance with meshlets, its draw records contiguous behind firstDrawRecord
void MeshletCulling::buildTemplates()
{
    ZoneScopedN("MeshletCulling::buildTemplates");
    const auto& storage = resourceManager.objectStorage;
    const auto& geometry = resourceManager.geometryStore;
    templates.instances.clear();
    templates.drawRecords.clear();
    templates.groupMeshlets = {};
    templates.generation = resourceManager.nodeInstanceLayoutGeneration;

    const uint32_t entityCount = resourceManager.asset(AssetBuffer::Primitives).address == 0 ? 0 : storage.size();
    for (EntityId id = 0; id < entityCount; ++id) {
        // an entity whose load is not flushed yet has no GPU model and no node-instance rows
        if ((storage.flags[id] & EntityFlag::Active) == 0 || storage.modelIds[id] == kInvalidModelId) {
            continue;
        }
        const ElementRange nodes = resourceManager.modelAllocations[storage.modelIds[id]].nodes;
        for (uint32_t k = 0; k < nodes.count; ++k) {
            const NodeDraw& node = geometry.nodeDraws[nodes.first + k];
            GpuInstanceCullItem item{
                .nodeInstanceIndex = resourceManager.entityNodeInstanceFirst[id] + k,
                .firstDrawRecord = static_cast<uint32_t>(templates.drawRecords.size()),
            };
            for (uint32_t p = node.firstPrimitive; p < node.firstPrimitive + node.primitiveCount; ++p) {
                const PrimitiveDraw& primitive = geometry.primitiveDraws[p];
                const MeshStateGroup group = stateGroupOf(primitive);
                // bounds are built from base positions, so morphed meshlets always draw
                const uint32_t flags =
                    std::to_underlying(group) | (primitive.morphCount > 0 ? DrawRecordFlag::SkipCulling : 0u);
                item.flags |= flags & DrawRecordFlag::SkipCulling;
                const uint32_t meshletCount = primitive.meshlets.meshletCount;
                templates.groupMeshlets[std::to_underlying(group)] += meshletCount;
                for (uint32_t offset = 0; offset < meshletCount; offset += kCullGroupSize) {
                    templates.drawRecords.push_back(GpuDrawRecord{
                        .nodeInstanceIndex = item.nodeInstanceIndex,
                        .primitiveIndex = p,
                        .meshletOffset = offset,
                        .flags = flags,
                    });
                }
            }
            item.drawRecordCount = static_cast<uint32_t>(templates.drawRecords.size()) - item.firstDrawRecord;
            if (item.drawRecordCount > 0) {
                templates.instances.push_back(item);
            }
        }
    }

    const uint32_t maxGroups = device.capabilities.properties2.properties.limits.maxComputeWorkGroupCount[0];
    if (templates.instances.size() > maxGroups || templates.drawRecords.size() > maxGroups) {
        throw std::runtime_error(std::format("cull: {} instances / {} draw records exceed maxComputeWorkGroupCount {}",
                                             templates.instances.size(), templates.drawRecords.size(), maxGroups));
    }
    logInfo(std::format("cull templates: {} instances, {} draw records, {} + {} meshlets", templates.instances.size(),
                        templates.drawRecords.size(), templates.groupMeshlets[0], templates.groupMeshlets[1]),
            "Culling");
}

// the slot is not in flight, so its buffers can be replaced and rewritten in place
void MeshletCulling::uploadTemplates(uint32_t frameSlot)
{
    ZoneScopedN("MeshletCulling::uploadTemplates");
    FrameBuffers& buffers = frames[frameSlot];
    const VmaAllocator allocator = resourceManager.allocator.allocator;
    constexpr vk::MemoryPropertyFlags kHostProperties =
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;

    const auto instanceCount = static_cast<uint32_t>(templates.instances.size());
    if (instanceCount > buffers.instanceCapacity || buffers.instanceItems.memory == nullptr) {
        buffers.instanceCapacity = grownCapacity(instanceCount, buffers.instanceCapacity, kMinCapacity);
        destroyVmaBuffer(allocator, buffers.instanceItems.buffer, buffers.instanceItems.memory,
                         kInstanceItemsTracyName);
        createMappedBuffer(buffers.instanceItems, sizeof(GpuInstanceCullItem) * vk::DeviceSize{buffers.instanceCapacity},
                           vk::BufferUsageFlagBits2::eStorageBuffer, kHostProperties, device, allocator,
                           std::format("InstanceCullItems_{}", frameSlot), kInstanceItemsTracyName);
    }

    const auto recordCount = static_cast<uint32_t>(templates.drawRecords.size());
    if (recordCount > buffers.drawRecordCapacity || buffers.drawRecordTemplates.memory == nullptr) {
        buffers.drawRecordCapacity = grownCapacity(recordCount, buffers.drawRecordCapacity, kMinCapacity);
        const vk::DeviceSize size = sizeof(GpuDrawRecord) * vk::DeviceSize{buffers.drawRecordCapacity};
        destroyVmaBuffer(allocator, buffers.drawRecordTemplates.buffer, buffers.drawRecordTemplates.memory,
                         kDrawRecordTemplatesTracyName);
        destroyVmaBuffer(allocator, buffers.drawRecords.buffer, buffers.drawRecords.memory, kDrawRecordsTracyName);
        createMappedBuffer(buffers.drawRecordTemplates, size, vk::BufferUsageFlagBits2::eStorageBuffer, kHostProperties,
                           device, allocator, std::format("DrawRecordTemplates_{}", frameSlot),
                           kDrawRecordTemplatesTracyName);
        createDeviceBuffer(buffers.drawRecords, size, device, allocator, std::format("DrawRecords_{}", frameSlot),
                           kDrawRecordsTracyName);
    }

    const uint32_t totalMeshlets = std::reduce(templates.groupMeshlets.begin(), templates.groupMeshlets.end(), 0u);
    if (totalMeshlets > buffers.visibleCapacity || buffers.visibleMeshlets.memory == nullptr) {
        buffers.visibleCapacity = grownCapacity(totalMeshlets, buffers.visibleCapacity, kMinVisibleCapacity);
        destroyVmaBuffer(allocator, buffers.visibleMeshlets.buffer, buffers.visibleMeshlets.memory,
                         kVisibleMeshletsTracyName);
        createDeviceBuffer(buffers.visibleMeshlets, sizeof(uint32_t) * vk::DeviceSize{buffers.visibleCapacity}, device,
                           allocator, std::format("VisibleMeshlets_{}", frameSlot), kVisibleMeshletsTracyName);
    }

    // write-combined memory: one sequential copy each, never read back
    std::memcpy(buffers.instanceItems.mapped, templates.instances.data(),
                templates.instances.size() * sizeof(GpuInstanceCullItem));
    std::memcpy(buffers.drawRecordTemplates.mapped, templates.drawRecords.data(),
                templates.drawRecords.size() * sizeof(GpuDrawRecord));
    buffers.generation = templates.generation;
}

uint32_t MeshletCulling::meshletsPerSequence(uint32_t totalMeshlets) const
{
    const auto& meshShader = device.capabilities.meshShader;
    uint32_t perSequence = std::max(meshShader.maxMeshWorkGroupCount[0], 1u);
    if (meshShader.maxMeshWorkGroupTotalCount != 0) {
        perSequence = std::min(perSequence, meshShader.maxMeshWorkGroupTotalCount);
    }
    if (settings.maxMeshletsPerSequence != 0) {
        perSequence = std::min(perSequence, settings.maxMeshletsPerSequence);
    }
    // a small UI value must not push a group past maxIndirectSequenceCount
    const uint32_t maxSequences = device.capabilities.deviceGeneratedCommands.maxIndirectSequenceCount;
    return std::max(perSequence, divideRoundUp(totalMeshlets, maxSequences));
}

void MeshletCulling::writeCullView(uint32_t frameSlot, const Camera& camera, uint32_t perSequence)
{
    if (settings.isCameraFrozen && !wasCameraFrozen) {
        frozenViewProj = camera.cameraData.viewProj;
        frozenPosition = camera.cameraData.cameraPos;
    }
    wasCameraFrozen = settings.isCameraFrozen;
    const glm::mat4& viewProj = settings.isCameraFrozen ? frozenViewProj : camera.cameraData.viewProj;
    const glm::vec3& position = settings.isCameraFrozen ? frozenPosition : camera.cameraData.cameraPos;

    const FrameBuffers& buffers = frames[frameSlot];
    GpuCullView view{
        .frustumPlanes = frustumPlanes(viewProj),
        .cameraPos = position,
        .flags = (settings.isFrustumEnabled ? CullFlag::Frustum : 0u) | (settings.isConeEnabled ? CullFlag::Cone : 0u) |
                 (settings.isStatsEnabled ? CullFlag::Stats : 0u) |
                 (settings.isInstanceFrustumEnabled ? CullFlag::InstanceFrustum : 0u),
        .visibleMeshlets = buffers.visibleMeshlets.address,
        .counters = buffers.counters.address,
        .sequences = dgc.sequenceAddress(),
        .instanceItems = buffers.instanceItems.address,
        .drawRecordTemplates = buffers.drawRecordTemplates.address,
        .instanceCount = static_cast<uint32_t>(templates.instances.size()),
        .maxMeshletsPerSequence = perSequence,
    };
    std::exclusive_scan(templates.groupMeshlets.begin(), templates.groupMeshlets.end(),
                        view.groupVisibleOffset.begin(), 0u);
    for (uint32_t group = 0; group < kMeshStateGroupCount; ++group) {
        view.groupSequenceOffset[group] = dgc.groupSequenceOffset(static_cast<MeshStateGroup>(group));
    }
    *static_cast<GpuCullView*>(buffers.cullView.mapped) = view;
}

void MeshletCulling::recordCull(const vk::raii::CommandBuffer& cmd) const
{
    ZoneScopedN("MeshletCulling::recordCull");
    const FrameBuffers& buffers = frames[frame];
    // one write: zeroes plus the y = z = 1 of the meshlet dispatch
    cmd.updateBuffer<GpuCullCounters>(*buffers.counters.buffer, 0, kInitialCullCounters);

    const vk::MemoryBarrier2 resetBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &resetBarrier});

    if (!templates.instances.empty()) {
        const CullPushData push{
            .frameRoot = resourceManager.frameRootAddress(frame),
            .cullView = buffers.cullView.address,
        };
        cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});

        ShaderObjects::bindComputeShader(cmd, shaderObjects.cullShaders.cullInstances);
        cmd.dispatch(static_cast<uint32_t>(templates.instances.size()), 1, 1);

        // draw records feed the meshlet cull, their count its indirect dispatch
        const vk::MemoryBarrier2 instanceBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect | vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead | vk::AccessFlagBits2::eShaderStorageRead |
                             vk::AccessFlagBits2::eShaderStorageWrite,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &instanceBarrier});

        ShaderObjects::bindComputeShader(cmd, shaderObjects.cullShaders.cullMeshlets);
        cmd.dispatchIndirect(*buffers.counters.buffer, offsetof(GpuCullCounters, meshletDispatch));

        const vk::MemoryBarrier2 cullBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &cullBarrier});

        ShaderObjects::bindComputeShader(cmd, shaderObjects.cullShaders.buildMeshSequences);
        cmd.dispatch(1, 1, 1);
    }

    // sequences + counts feed preprocess and execute; the visible list and draw records feed the mesh shader
    const vk::MemoryBarrier2 outputBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eCommandPreprocessEXT | vk::PipelineStageFlagBits2::eDrawIndirect |
                        vk::PipelineStageFlagBits2::eMeshShaderEXT,
        .dstAccessMask = vk::AccessFlagBits2::eCommandPreprocessReadEXT | vk::AccessFlagBits2::eIndirectCommandRead |
                         vk::AccessFlagBits2::eShaderStorageRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &outputBarrier});
}
