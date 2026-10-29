#include "vk_meshlet_culling.hpp"

#include "../static_headers/logger.hpp"
#include "push_data.hpp"
#include "util/vk_tracy.hpp"
#include "util/vk_utils.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <numeric>
#include <stdexcept>

namespace
{
    constexpr const char* kCullViewTracyName = "GPU/CullView";
    constexpr const char* kCullCountersTracyName = "GPU/CullCounters";
    constexpr const char* kVisibleMeshletsTracyName = "GPU/VisibleMeshlets";
    // first visible-list allocation, in meshlets
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
        // indirect: DGC reads the sequence counts from here
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

    const GroupTotals totals = writeDrawRecords(frameSlot);
    drawRecordCount = totals.drawRecords;
    const uint32_t totalMeshlets = std::reduce(totals.meshlets.begin(), totals.meshlets.end(), 0u);
    ensureVisibleCapacity(frameSlot, totalMeshlets);
    resourceManager.visibleMeshletAddresses[frameSlot] = frames[frameSlot].visibleMeshlets.address;

    const uint32_t perSequence = meshletsPerSequence(totalMeshlets);
    std::array<uint32_t, kMeshStateGroupCount> maxSequences{};
    std::ranges::transform(totals.meshlets, maxSequences.begin(),
                           [perSequence](uint32_t meshlets) { return divideRoundUp(meshlets, perSequence); });
    dgc.prepareFrame(frameSlot, maxSequences, frames[frameSlot].counters.address);
    writeCullView(frameSlot, camera, totals, perSequence);

    recorded[frameSlot] = RecordedFrame{
        .totals =
            {
                .drawRecords = totals.drawRecords,
                .totalMeshlets = totalMeshlets,
                .meshletsPerSequence = perSequence,
                .groupMeshlets = totals.meshlets,
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
    stats.visibleMeshlets = counters.visibleMeshlets;
    stats.sequences = counters.sequenceCount;
    stats.testedMeshlets = counters.testedMeshlets;
    stats.frustumCulled = counters.frustumCulled;
    stats.coneCulled = counters.coneCulled;
    stats.unculledMeshlets = counters.unculledMeshlets;

    TracyPlot("Culling/VisibleMeshlets",
              static_cast<double>(std::reduce(stats.visibleMeshlets.begin(), stats.visibleMeshlets.end(), 0u)));
    TracyPlot("Culling/TotalMeshlets", static_cast<double>(stats.totalMeshlets));
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

MeshletCulling::GroupTotals MeshletCulling::writeDrawRecords(uint32_t frameSlot) const
{
    ZoneScopedN("MeshletCulling::writeDrawRecords");
    const auto& storage = resourceManager.objectStorage;
    const auto& geometry = resourceManager.geometryStore;
    const uint32_t entityCount = resourceManager.asset(AssetBuffer::Primitives).address == 0 ? 0 : storage.size();

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
                    if (geometry.primitiveDraws[p].meshlets.meshletCount > 0) {
                        visit(nodeInstance, p, geometry.primitiveDraws[p]);
                    }
                }
            }
        }
    };

    GroupTotals totals;
    forEachPrimitive([&](uint32_t, uint32_t, const PrimitiveDraw& primitive) {
        totals.drawRecords += divideRoundUp(primitive.meshlets.meshletCount, kCullGroupSize);
    });
    // the frame root reports 0 records when nothing draws
    GpuDrawRecord* records = resourceManager.mapDrawRecords(frameSlot, totals.drawRecords);

    uint32_t written = 0;
    forEachPrimitive([&](uint32_t nodeInstance, uint32_t primitiveIndex, const PrimitiveDraw& primitive) {
        const MeshStateGroup group = stateGroupOf(primitive);
        // bounds are built from base positions, so morphed meshlets always draw
        const uint32_t flags = std::to_underlying(group) | (primitive.morphCount > 0 ? DrawRecordFlag::SkipCulling : 0u);
        const uint32_t meshletCount = primitive.meshlets.meshletCount;
        totals.meshlets[std::to_underlying(group)] += meshletCount;
        // write-combined memory: whole rows, never read back
        for (uint32_t offset = 0; offset < meshletCount; offset += kCullGroupSize) {
            records[written++] = GpuDrawRecord{
                .nodeInstanceIndex = nodeInstance,
                .primitiveIndex = primitiveIndex,
                .meshletOffset = offset,
                .flags = flags,
            };
        }
    });

    const uint32_t maxGroups = device.capabilities.properties2.properties.limits.maxComputeWorkGroupCount[0];
    if (totals.drawRecords > maxGroups) {
        throw std::runtime_error(std::format("meshlet cull: {} draw records exceed maxComputeWorkGroupCount {}",
                                             totals.drawRecords, maxGroups));
    }
    return totals;
}

void MeshletCulling::ensureVisibleCapacity(uint32_t frameSlot, uint32_t meshletCount)
{
    FrameBuffers& buffers = frames[frameSlot];
    if (meshletCount <= buffers.visibleCapacity && buffers.visibleMeshlets.memory != nullptr) {
        return;
    }
    ZoneScopedN("MeshletCulling::ensureVisibleCapacity");
    // only this slot is replaced; its previous frame finished before recording started
    const uint32_t capacity = std::max({meshletCount, buffers.visibleCapacity * 2, kMinVisibleCapacity});
    const vk::DeviceSize size = sizeof(uint32_t) * static_cast<vk::DeviceSize>(capacity);
    DeviceBuffer& visible = buffers.visibleMeshlets;
    destroyVmaBuffer(resourceManager.allocator.allocator, visible.buffer, visible.memory, kVisibleMeshletsTracyName);
    createBuffer(size, vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eDeviceLocal, visible.buffer, visible.memory,
                 resourceManager.allocator.allocator, device.vkdevice, device.queueFamilyIndices,
                 std::format("VisibleMeshlets_{}", frameSlot));
    visible.address = device.vkdevice.getBufferAddress({.buffer = *visible.buffer});
    visible.capacityBytes = size;
    buffers.visibleCapacity = capacity;
    tracyResourceAlloc(static_cast<VkBuffer>(*visible.buffer), static_cast<size_t>(size), kVisibleMeshletsTracyName);
    logInfo(std::format("visible meshlets frame {}: capacity {}", frameSlot, capacity), "Culling");
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

void MeshletCulling::writeCullView(uint32_t frameSlot, const Camera& camera, const GroupTotals& totals,
                                   uint32_t perSequence)
{
    if (settings.isCameraFrozen && !wasCameraFrozen) {
        frozenViewProj = camera.cameraData.viewProj;
        frozenPosition = camera.cameraData.cameraPos;
    }
    wasCameraFrozen = settings.isCameraFrozen;
    const glm::mat4& viewProj = settings.isCameraFrozen ? frozenViewProj : camera.cameraData.viewProj;
    const glm::vec3& position = settings.isCameraFrozen ? frozenPosition : camera.cameraData.cameraPos;

    GpuCullView view{
        .frustumPlanes = frustumPlanes(viewProj),
        .cameraPos = position,
        .flags = (settings.isFrustumEnabled ? CullFlag::Frustum : 0u) | (settings.isConeEnabled ? CullFlag::Cone : 0u) |
                 (settings.isStatsEnabled ? CullFlag::Stats : 0u),
        .visibleMeshlets = frames[frameSlot].visibleMeshlets.address,
        .counters = frames[frameSlot].counters.address,
        .sequences = dgc.sequenceAddress(),
        .drawRecordCount = totals.drawRecords,
        .maxMeshletsPerSequence = perSequence,
    };
    std::exclusive_scan(totals.meshlets.begin(), totals.meshlets.end(), view.groupVisibleOffset.begin(), 0u);
    for (uint32_t group = 0; group < kMeshStateGroupCount; ++group) {
        view.groupSequenceOffset[group] = dgc.groupSequenceOffset(static_cast<MeshStateGroup>(group));
    }
    *static_cast<GpuCullView*>(frames[frameSlot].cullView.mapped) = view;
}

void MeshletCulling::recordCull(const vk::raii::CommandBuffer& cmd) const
{
    ZoneScopedN("MeshletCulling::recordCull");
    const FrameBuffers& buffers = frames[frame];
    cmd.fillBuffer(*buffers.counters.buffer, 0, sizeof(GpuCullCounters), 0);

    const vk::MemoryBarrier2 clearBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &clearBarrier});

    if (drawRecordCount > 0) {
        const CullPushData push{
            .frameRoot = resourceManager.frameRootAddress(frame),
            .cullView = buffers.cullView.address,
        };
        cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});

        ShaderObjects::bindComputeShader(cmd, shaderObjects.cullShaders.cullMeshlets);
        cmd.dispatch(drawRecordCount, 1, 1);

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

    // sequences + counts feed preprocess and execute; the visible list feeds the mesh shader
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
