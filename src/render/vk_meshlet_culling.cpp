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
    constexpr const char* kInstanceCandidatesTracyName = "GPU/InstanceCandidates";
    constexpr const char* kMeshletCandidatesTracyName = "GPU/MeshletCandidates";
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
        createMappedBuffer(frames[slot].cullView, sizeof(PhaseCullViews), vk::BufferUsageFlagBits2::eStorageBuffer,
                           vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
                           device, allocator, std::format("CullView_{}", slot), kCullViewTracyName);
        // indirect: the meshlet dispatch and the DGC sequence counts are read from here
        createMappedBuffer(frames[slot].counters, sizeof(kInitialCullCounters),
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
        destroyVmaBuffer(allocator, buffers.instanceCandidates.buffer, buffers.instanceCandidates.memory,
                         kInstanceCandidatesTracyName);
        destroyVmaBuffer(allocator, buffers.drawRecordTemplates.buffer, buffers.drawRecordTemplates.memory,
                         kDrawRecordTemplatesTracyName);
        destroyVmaBuffer(allocator, buffers.drawRecords.buffer, buffers.drawRecords.memory, kDrawRecordsTracyName);
        destroyVmaBuffer(allocator, buffers.visibleMeshlets.buffer, buffers.visibleMeshlets.memory,
                         kVisibleMeshletsTracyName);
        destroyVmaBuffer(allocator, buffers.meshletCandidates.buffer, buffers.meshletCandidates.memory,
                         kMeshletCandidatesTracyName);
    }
}

void MeshletCulling::prepareFrame(uint32_t frameSlot, const Camera& camera, const HiZPyramid& hiz)
{
    ZoneScopedN("MeshletCulling::prepareFrame");
    const auto start = std::chrono::steady_clock::now();
    frame = frameSlot;
    readBackCounters(frameSlot);

    const bool isLayoutChanged = templates.generation != resourceManager.nodeInstanceLayoutGeneration;
    if (isLayoutChanged) {
        buildTemplates();
    }
    const OcclusionHistory history = occlusionHistory(camera, hiz, isLayoutChanged);
    if (history != OcclusionHistory::Valid && history != OcclusionHistory::Disabled) {
        ++fallbackFrames;
    }
    if (frames[frameSlot].generation != templates.generation) {
        uploadTemplates(frameSlot);
    }
    resourceManager.drawRecordAddresses[frameSlot] = frames[frameSlot].drawRecords.address;
    resourceManager.visibleMeshletAddresses[frameSlot] = frames[frameSlot].visibleMeshlets.address;

    // every template may survive, so the per-group meshlet totals bound the visible lists and sequences
    const uint32_t totalMeshlets = std::reduce(templates.groupMeshlets.begin(), templates.groupMeshlets.end(), 0u);
    // one visible region of totalMeshlets rows per phase
    resourceManager.lateVisibleBases[frameSlot] = totalMeshlets * std::to_underlying(CullPhase::Late);
    const uint32_t perSequence = meshletsPerSequence(totalMeshlets);
    std::array<uint32_t, kMeshStateGroupCount> maxSequences{};
    std::ranges::transform(templates.groupMeshlets, maxSequences.begin(),
                           [perSequence](uint32_t meshlets) { return divideRoundUp(meshlets, perSequence); });
    dgc.prepareFrame(frameSlot, maxSequences, frames[frameSlot].counters.address);
    writeCullView(frameSlot, camera, hiz, history, perSequence);

    recorded[frameSlot] = RecordedFrame{
        .totals =
            {
                .instances = static_cast<uint32_t>(templates.instances.size()),
                .drawRecords = static_cast<uint32_t>(templates.drawRecords.size()),
                .totalMeshlets = totalMeshlets,
                .meshletsPerSequence = perSequence,
                .groupMeshlets = templates.groupMeshlets,
                .history = history,
                .fallbackFrames = fallbackFrames,
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
    stats = record.totals;
    stats.phases = *static_cast<const decltype(kInitialCullCounters)*>(frames[frameSlot].counters.mapped);

    [[maybe_unused]] const GpuCullCounters& early = stats.phase(CullPhase::Early);
    [[maybe_unused]] const GpuCullCounters& late = stats.phase(CullPhase::Late);
    TracyPlot("Culling/TotalMeshlets", static_cast<double>(stats.totalMeshlets));
    TracyPlot("Culling/EarlyVisibleMeshlets", static_cast<double>(visibleMeshletCount(early)));
    TracyPlot("Culling/LateVisibleMeshlets", static_cast<double>(visibleMeshletCount(late)));
    TracyPlot("Culling/MeshletCandidates", static_cast<double>(late.meshletCandidateCount));
    TracyPlot("Culling/InstanceCandidates", static_cast<double>(late.instanceDispatch[0]));
    TracyPlot("Culling/FrustumCulled", static_cast<double>(early.frustumCulled + late.frustumCulled));
    TracyPlot("Culling/ConeCulled", static_cast<double>(early.coneCulled + late.coneCulled));
    // late is the final verdict: early-occluded meshlets are re-tested there
    TracyPlot("Culling/MeshletsOccluded", static_cast<double>(late.occlusionCulled));
    TracyPlot("Culling/HistoryValid", stats.history == OcclusionHistory::Valid ? 1.0 : 0.0);
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
        destroyVmaBuffer(allocator, buffers.instanceCandidates.buffer, buffers.instanceCandidates.memory,
                         kInstanceCandidatesTracyName);
        createDeviceBuffer(buffers.instanceCandidates, sizeof(uint32_t) * vk::DeviceSize{buffers.instanceCapacity},
                           device, allocator, std::format("InstanceCandidates_{}", frameSlot),
                           kInstanceCandidatesTracyName);
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
        // one region per phase: an instance draws in at most one of them
        createDeviceBuffer(buffers.drawRecords, size * kCullPhaseCount, device, allocator,
                           std::format("DrawRecords_{}", frameSlot), kDrawRecordsTracyName);
    }

    // one region per phase
    const uint32_t totalMeshlets = std::reduce(templates.groupMeshlets.begin(), templates.groupMeshlets.end(), 0u);
    const uint32_t visibleRows = totalMeshlets * kCullPhaseCount;
    if (visibleRows > buffers.visibleCapacity || buffers.visibleMeshlets.memory == nullptr) {
        buffers.visibleCapacity = grownCapacity(visibleRows, buffers.visibleCapacity, kMinVisibleCapacity);
        destroyVmaBuffer(allocator, buffers.visibleMeshlets.buffer, buffers.visibleMeshlets.memory,
                         kVisibleMeshletsTracyName);
        createDeviceBuffer(buffers.visibleMeshlets, sizeof(uint32_t) * vk::DeviceSize{buffers.visibleCapacity}, device,
                           allocator, std::format("VisibleMeshlets_{}", frameSlot), kVisibleMeshletsTracyName);
    }
    if (totalMeshlets > buffers.meshletCandidateCapacity || buffers.meshletCandidates.memory == nullptr) {
        buffers.meshletCandidateCapacity =
            grownCapacity(totalMeshlets, buffers.meshletCandidateCapacity, kMinVisibleCapacity);
        destroyVmaBuffer(allocator, buffers.meshletCandidates.buffer, buffers.meshletCandidates.memory,
                         kMeshletCandidatesTracyName);
        createDeviceBuffer(buffers.meshletCandidates,
                           sizeof(uint32_t) * vk::DeviceSize{buffers.meshletCandidateCapacity}, device, allocator,
                           std::format("MeshletCandidates_{}", frameSlot), kMeshletCandidatesTracyName);
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

// first match wins, so the reported reason is the most fundamental one
OcclusionHistory MeshletCulling::occlusionHistory(const Camera& camera, const HiZPyramid& hiz,
                                                  bool isLayoutChanged) const
{
    if (!settings.isOcclusionEnabled) {
        return OcclusionHistory::Disabled;
    }
    if (!hiz.hasContent()) {
        return OcclusionHistory::NoPyramid;
    }
    if (camera.isCut()) {
        return OcclusionHistory::CameraCut;
    }
    if (isLayoutChanged) {
        return OcclusionHistory::SceneChanged;
    }
    if (settings.isCameraFrozen) {
        return OcclusionHistory::CameraFrozen;
    }
    return OcclusionHistory::Valid;
}

void MeshletCulling::writeCullView(uint32_t frameSlot, const Camera& camera, const HiZPyramid& hiz,
                                   OcclusionHistory history, uint32_t perSequence)
{
    if (settings.isCameraFrozen && !wasCameraFrozen) {
        frozenViewProj = camera.cameraData.viewProj;
        frozenPosition = camera.cameraData.cameraPos;
    }
    wasCameraFrozen = settings.isCameraFrozen;
    const glm::mat4& viewProj = settings.isCameraFrozen ? frozenViewProj : camera.cameraData.viewProj;
    const glm::vec3& position = settings.isCameraFrozen ? frozenPosition : camera.cameraData.cameraPos;

    const FrameBuffers& buffers = frames[frameSlot];
    const GpuCullView view{
        .frustumPlanes = frustumPlanes(viewProj),
        .cameraPos = position,
        .flags = (settings.isFrustumEnabled ? CullFlag::Frustum : 0u) | (settings.isConeEnabled ? CullFlag::Cone : 0u) |
                 (settings.isStatsEnabled ? CullFlag::Stats : 0u) |
                 (settings.isInstanceFrustumEnabled ? CullFlag::InstanceFrustum : 0u) |
                 (settings.isOcclusionEnabled ? CullFlag::Occlusion : 0u),
        .visibleMeshlets = buffers.visibleMeshlets.address,
        .counters = buffers.counters.address,
        .sequences = dgc.sequenceAddress(),
        .instanceItems = buffers.instanceItems.address,
        .drawRecordTemplates = buffers.drawRecordTemplates.address,
        .instanceCount = static_cast<uint32_t>(templates.instances.size()),
        .maxMeshletsPerSequence = perSequence,
        .hizPyramid = hiz.pyramidHeapIndex(),
        .hizMaxSampler = hiz.maxSamplerHeapIndex(),
        .hizSize = hiz.size(),
        .instanceCandidates = buffers.instanceCandidates.address,
        .meshletCandidates = buffers.meshletCandidates.address,
    };
    const uint32_t phaseVisibleRows =
        std::reduce(templates.groupMeshlets.begin(), templates.groupMeshlets.end(), 0u);
    PhaseCullViews views{};
    for (uint32_t phase = 0; phase < kCullPhaseCount; ++phase) {
        GpuCullView& phaseView = views[phase];
        phaseView = view;
        const bool isLate = static_cast<CullPhase>(phase) == CullPhase::Late;
        phaseView.counters += phase * sizeof(GpuCullCounters);
        phaseView.flags |= isLate ? CullFlag::LatePhase : 0u;
        // without history the early phase draws everything that survives frustum + cone; no candidates follow
        if (!isLate && history != OcclusionHistory::Valid) {
            phaseView.flags &= ~CullFlag::Occlusion;
        }
        phaseView.lateCounters =
            isLate ? 0 : buffers.counters.address + std::to_underlying(CullPhase::Late) * sizeof(GpuCullCounters);
        phaseView.drawRecordBase = phase * static_cast<uint32_t>(templates.drawRecords.size());
        // the pyramid follows the render camera, not a frozen cull camera
        phaseView.occlusionViewProj = isLate ? camera.cameraData.viewProj : camera.cameraData.prevViewProj;
        std::exclusive_scan(templates.groupMeshlets.begin(), templates.groupMeshlets.end(),
                            phaseView.groupVisibleOffset.begin(), phase * phaseVisibleRows);
        for (uint32_t group = 0; group < kMeshStateGroupCount; ++group) {
            phaseView.groupSequenceOffset[group] =
                dgc.groupSequenceOffset(static_cast<CullPhase>(phase), static_cast<MeshStateGroup>(group));
        }
    }
    *static_cast<PhaseCullViews*>(buffers.cullView.mapped) = views;
}

void MeshletCulling::recordCull(const vk::raii::CommandBuffer& cmd, CullPhase phase) const
{
    ZoneScopedN("MeshletCulling::recordCull");
    const FrameBuffers& buffers = frames[frame];
    const bool isEarly = phase == CullPhase::Early;
    const vk::DeviceSize countersOffset = std::to_underlying(phase) * sizeof(GpuCullCounters);
    if (isEarly) {
        // one write for both phases: zeroes plus the y = z = 1 of each indirect dispatch
        cmd.updateBuffer<std::array<GpuCullCounters, kCullPhaseCount>>(*buffers.counters.buffer, 0,
                                                                        kInitialCullCounters);
    }
    // early: counter reset and the previous frame's late pyramid. late: candidates and their count from the early
    // instance cull, and the early pyramid
    const vk::MemoryBarrier2 inputBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer | vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect | vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead | vk::AccessFlagBits2::eShaderStorageRead |
                         vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eShaderSampledRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &inputBarrier});

    if (!templates.instances.empty()) {
        const CullPushData push{
            .frameRoot = resourceManager.frameRootAddress(frame),
            .cullView = buffers.cullView.address + std::to_underlying(phase) * sizeof(GpuCullView),
        };
        cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});

        ShaderObjects::bindComputeShader(cmd, shaderObjects.cullShaders.cullInstances);
        if (isEarly) {
            cmd.dispatch(static_cast<uint32_t>(templates.instances.size()), 1, 1);
        } else {
            cmd.dispatchIndirect(*buffers.counters.buffer, countersOffset + offsetof(GpuCullCounters, instanceDispatch));
        }

        // draw records feed the meshlet cull, their count its indirect dispatch
        const vk::MemoryBarrier2 instanceBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect | vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead | vk::AccessFlagBits2::eShaderStorageRead |
                             vk::AccessFlagBits2::eShaderStorageWrite,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &instanceBarrier});

        // visible appends of the late meshlet passes, then the counts read by buildMeshSequences
        const vk::MemoryBarrier2 cullBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
        };
        ShaderObjects::bindComputeShader(cmd, shaderObjects.cullShaders.cullMeshlets);
        cmd.dispatchIndirect(*buffers.counters.buffer, countersOffset + offsetof(GpuCullCounters, meshletDispatch));
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &cullBarrier});

        // the early meshlet cull wrote the candidates and their dispatch; inputBarrier made them visible
        if (!isEarly) {
            ShaderObjects::bindComputeShader(cmd, shaderObjects.cullShaders.cullMeshletCandidates);
            cmd.dispatchIndirect(*buffers.counters.buffer,
                                 countersOffset + offsetof(GpuCullCounters, meshletCandidateDispatch));
            cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &cullBarrier});
        }

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
