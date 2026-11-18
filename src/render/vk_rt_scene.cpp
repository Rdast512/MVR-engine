#include "vk_rt_scene.hpp"
#include "../util/debug.hpp"
#include "../util/vk_tracy.hpp"
#include "push_data.hpp"
#include "static_headers/logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace
{
    constexpr const char* kRayInstancesTracyName = "GPU/RayInstances";
    constexpr const char* kRayBuildsTracyName = "GPU/RayInstanceBuilds";
    constexpr const char* kPtlasOperationTracyName = "GPU/PtlasOperation";
    // first allocation, in instances
    constexpr uint32_t kMinCapacity = 64;
    // matches shaders/passes/rt_instances.slang
    constexpr uint32_t kRtInstancesGroupSize = 64;
    // PtlasInstanceState in rt_instances.slang: the 3x4 transform the PTLAS holds
    constexpr vk::DeviceSize kPtlasStateRowBytes = 48;
    // VkPartitionedAccelerationStructureWriteInstanceDataNV::instanceID carries the primitive row
    constexpr uint32_t kCustomIndexLimit = 1u << 24;
    // VUID-vkCmdBuildPartitionedAccelerationStructuresNV-pBuildInfo-10542/-10544/-10545: 256-byte aligned
    // scratch, source and destination
    constexpr vk::DeviceSize kPtlasAlignment = 256;
    // srcInfos / srcInfosCount of the build: one write-instance op, then its count
    constexpr vk::DeviceSize kOpCountOffset = sizeof(vk::BuildPartitionedAccelerationStructureIndirectCommandNV);
    constexpr vk::DeviceSize kOperationBytes = kOpCountOffset + sizeof(uint32_t);

    [[nodiscard]] constexpr vk::DeviceSize alignUp(vk::DeviceSize size, vk::DeviceSize alignment)
    {
        return (size + alignment - 1) / alignment * alignment;
    }

    constexpr uint32_t divideRoundUp(uint32_t value, uint32_t divisor) { return (value + divisor - 1) / divisor; }

    // mesh-space box of a primitive's meshlet groups
    [[nodiscard]] BoundingBox primitiveBounds(const GeometryStore& geometry, const PrimitiveDraw& primitive)
    {
        BoundingBox box{.bboxMin = glm::vec3(std::numeric_limits<float>::max()),
                        .bboxMax = glm::vec3(std::numeric_limits<float>::lowest())};
        for (uint32_t g = primitive.firstGroup; g < primitive.firstGroup + primitive.groupCount; ++g) {
            box.bboxMin = glm::min(box.bboxMin, geometry.meshletGroups[g].bboxMin);
            box.bboxMax = glm::max(box.bboxMax, geometry.meshletGroups[g].bboxMax);
        }
        return box;
    }
} // namespace

RayTracingScene::RayTracingScene(const Device& device, const VkAllocator& allocator, const FrameSync& frameSync,
                                 const Scene& scene, const AssetBuffers& assets, const FrameData& frameData,
                                 const ShaderObjects& shaderObjects) :
    device(device), allocator(allocator), frameSync(frameSync), scene(scene), assets(assets), frameData(frameData),
    shaderObjects(shaderObjects)
{
    static_assert(sizeof(vk::PartitionedAccelerationStructureWriteInstanceDataNV) == 104);
    static_assert(sizeof(vk::BuildPartitionedAccelerationStructureIndirectCommandNV) == 24);
    const vk::PhysicalDeviceLimits& limits = device.capabilities.properties2.properties.limits;
    const auto queueFamilies = device.physicalDevice.getQueueFamilyProperties2();
    if (limits.timestampPeriod > 0.0f && queueFamilies[device.computeIndex].queueFamilyProperties.timestampValidBits != 0) {
        timestampPeriodNs = limits.timestampPeriod;
        timestamps = vk::raii::QueryPool(device.vkdevice, vk::QueryPoolCreateInfo{
                                                              .queryType = vk::QueryType::eTimestamp,
                                                              .queryCount = kQueriesPerFrame * MAX_FRAMES_IN_FLIGHT,
                                                          });
    }
}

RayTracingScene::~RayTracingScene()
{
    for (FrameBuffers& buffers : frames) {
        destroyFrameBuffers(buffers);
    }
    destroyPtlas();
}

void RayTracingScene::prepareFrame(uint32_t frameSlot, uint64_t lastSubmittedFrame)
{
    ZoneScopedN("RayTracingScene::prepareFrame");
    collectResults(frameSlot);
    if (templates.layoutGeneration != frameData.layoutGeneration() ||
        templates.instancesPerPartition != settings.instancesPerPartition) {
        buildTemplates();
    }
    const PtlasShape& shape = templates.shape;
    const PtlasShape& sized = ptlas.sizedFor;
    if (ptlas.storage.memory == nullptr || shape.instances > sized.instances || shape.partitions > sized.partitions ||
        shape.maxPerPartition > sized.maxPerPartition) {
        createPtlas(lastSubmittedFrame);
    }
    if (frames[frameSlot].version != templates.version) {
        uploadTemplates(frameSlot);
    }
    // the GPU appends to argCount; the slot's last build has completed
    auto* operation = static_cast<std::byte*>(frames[frameSlot].operation.mapped);
    const vk::BuildPartitionedAccelerationStructureIndirectCommandNV writeOp{
        .opType = vk::PartitionedAccelerationStructureOpTypeNV::eWriteInstance,
        .argCount = 0,
        .argData = {.startAddress = frames[frameSlot].writeInfos.address,
                    .strideInBytes = sizeof(vk::PartitionedAccelerationStructureWriteInstanceDataNV)},
    };
    constexpr uint32_t kOpCount = 1;
    std::memcpy(operation, &writeOp, sizeof(writeOp));
    std::memcpy(operation + kOpCountOffset, &kOpCount, sizeof(kOpCount));
}

// same entity filter as the cull templates (MeshletCulling::buildTemplates): one row per drawn primitive. Each row's
// partition is its cell in a uniform XZ grid over the primitives' world-space centers at layout time
void RayTracingScene::buildTemplates()
{
    ZoneScopedN("RayTracingScene::buildTemplates");
    templates.rows.clear();
    templates.builds.clear();
    templates.layoutGeneration = frameData.layoutGeneration();
    templates.instancesPerPartition = settings.instancesPerPartition;
    ++templates.version;
    templates.sceneBounds = BoundingBox{.bboxMin = glm::vec3(std::numeric_limits<float>::max()),
                                        .bboxMax = glm::vec3(std::numeric_limits<float>::lowest())};

    const ObjectStorage& storage = scene.objectStorage;
    const GeometryStore& geometry = scene.geometryStore;
    const uint32_t entityCount = assets.asset(AssetBuffer::Primitives).address == 0 ? 0 : storage.size();
    const std::span<const uint32_t> nodeInstanceFirsts = frameData.entityNodeInstanceFirst();
    std::vector<glm::vec2> centers;
    for (EntityId id = 0; id < entityCount; ++id) {
        // an entity whose load is not flushed yet has no GPU model and no node-instance rows
        if ((storage.flags[id] & EntityFlag::Active) == 0 || storage.modelIds[id] == kInvalidModelId) {
            continue;
        }
        const ModelId modelId = storage.modelIds[id];
        const ModelAllocation& model = assets.models()[modelId];
        // indexed by model-relative primitive row
        const std::span<const vk::DeviceAddress> blas = assets.rayGeometry().blasAddresses(modelId);
        const glm::mat4 entityMatrix = computeModelMatrix(storage.transforms[id]);
        for (uint32_t k = 0; k < model.nodes.count; ++k) {
            const NodeDraw& node = geometry.nodeDraws[model.nodes.first + k];
            const glm::mat4 world = entityMatrix * node.localToModel;
            for (uint32_t p = node.firstPrimitive; p < node.firstPrimitive + node.primitiveCount; ++p) {
                const vk::DeviceAddress address = blas[p - model.primitives.first];
                // a primitive without meshlets has no BLAS
                if (address == 0) {
                    continue;
                }
                templates.rows.push_back(GpuRayInstance{.nodeInstanceIndex = nodeInstanceFirsts[id] + k,
                                                        .primitiveRow = p});
                templates.builds.push_back(GpuRayInstanceBuild{.blas = address});
                const BoundingBox box = primitiveBounds(geometry, geometry.primitiveDraws[p]);
                const glm::vec4 center = world * glm::vec4(0.5f * (box.bboxMin + box.bboxMax), 1.0f);
                centers.emplace_back(center.x, center.z);
                for (uint32_t corner = 0; corner < 8; ++corner) {
                    const glm::vec3 local{(corner & 1u) != 0 ? box.bboxMax.x : box.bboxMin.x,
                                          (corner & 2u) != 0 ? box.bboxMax.y : box.bboxMin.y,
                                          (corner & 4u) != 0 ? box.bboxMax.z : box.bboxMin.z};
                    const glm::vec3 position = glm::vec3(world * glm::vec4(local, 1.0f));
                    templates.sceneBounds.bboxMin = glm::min(templates.sceneBounds.bboxMin, position);
                    templates.sceneBounds.bboxMax = glm::max(templates.sceneBounds.bboxMax, position);
                }
            }
        }
    }

    const auto count = static_cast<uint32_t>(templates.rows.size());
    const uint64_t maxInstances = device.capabilities.accelerationStructure.maxInstanceCount;
    if (count > maxInstances) {
        throw std::runtime_error(std::format("rt: {} instances exceed maxInstanceCount {}", count, maxInstances));
    }
    if (assets.uploadedCount(AssetBuffer::Primitives) > kCustomIndexLimit) {
        throw std::runtime_error(std::format("rt: {} primitive rows exceed the 24-bit instance custom index",
                                             assets.uploadedCount(AssetBuffer::Primitives)));
    }

    // no global partition: changed instances are rewritten in their grid partition, which measured faster (feature
    // spec §3.6)
    const uint32_t maxPartitions = device.capabilities.partitionedAccelerationStructure.maxPartitionCount;
    if (maxPartitions == 0) {
        throw std::runtime_error("rt: maxPartitionCount is 0");
    }
    const double cellInstances = std::max(templates.instancesPerPartition, 1u);
    auto axis = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(count) / cellInstances)));
    axis = std::max(axis, 1u);
    while (axis > 1 && axis * axis > maxPartitions) {
        --axis;
    }
    glm::vec2 lower(std::numeric_limits<float>::max());
    glm::vec2 upper(std::numeric_limits<float>::lowest());
    for (const glm::vec2& center : centers) {
        lower = glm::min(lower, center);
        upper = glm::max(upper, center);
    }
    const glm::vec2 extent = glm::max(upper - lower, glm::vec2(std::numeric_limits<float>::min()));
    std::vector<uint32_t> occupancy(static_cast<size_t>(axis) * axis, 0);
    for (uint32_t row = 0; row < count; ++row) {
        const glm::uvec2 cell =
            glm::min(glm::uvec2((centers[row] - lower) / extent * static_cast<float>(axis)), glm::uvec2(axis - 1));
        const uint32_t partition = cell.y * axis + cell.x;
        templates.builds[row].partition = partition;
        ++occupancy[partition];
    }
    const uint32_t partitions = axis * axis;
    templates.shape = PtlasShape{
        .instances = std::max(count, 1u),
        .partitions = partitions,
        .maxPerPartition = std::max(*std::ranges::max_element(occupancy), 1u),
    };
    logInfo(std::format("rt templates: {} instances, {}x{} partitions, at most {} per partition", count, axis, axis,
                        templates.shape.maxPerPartition),
            "RayTracingScene");
}

void RayTracingScene::uploadTemplates(uint32_t frameSlot)
{
    ZoneScopedN("RayTracingScene::uploadTemplates");
    FrameBuffers& buffers = frames[frameSlot];
    const auto count = static_cast<uint32_t>(templates.rows.size());
    if (count > buffers.capacity || buffers.rayInstances.memory == nullptr) {
        createFrameBuffers(frameSlot, std::max({count, buffers.capacity * 2, kMinCapacity}));
    }
    // write-combined memory: whole arrays, never read back
    std::memcpy(buffers.rayInstances.mapped, templates.rows.data(), std::span(templates.rows).size_bytes());
    std::memcpy(buffers.builds.mapped, templates.builds.data(), std::span(templates.builds).size_bytes());
    buffers.count = count;
    buffers.version = templates.version;
    TracyPlot("RT/Instances", static_cast<double>(count));
}

vk::PartitionedAccelerationStructureInstancesInputNV RayTracingScene::buildInput(const PtlasShape& shape) const
{
    return vk::PartitionedAccelerationStructureInstancesInputNV{
        .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
        .instanceCount = shape.instances,
        .maxInstancePerPartitionCount = shape.maxPerPartition,
        .partitionCount = shape.partitions,
        .maxInstanceInGlobalPartitionCount = 0,
    };
}

void RayTracingScene::createPtlas(uint64_t lastSubmittedFrame)
{
    ZoneScopedN("RayTracingScene::createPtlas");
    // frames in flight trace the PTLAS and update it in place: let the last submitted ones finish
    if (lastSubmittedFrame > 0) {
        const std::array semaphores{*frameSync.computeTimeline, *frameSync.rayTimeline};
        const std::array values{lastSubmittedFrame, lastSubmittedFrame};
        const vk::SemaphoreWaitInfo waitInfo{.semaphoreCount = static_cast<uint32_t>(semaphores.size()),
                                             .pSemaphores = semaphores.data(),
                                             .pValues = values.data()};
        while (vk::Result::eTimeout == device.vkdevice.waitSemaphores(waitInfo, UINT64_MAX))
            ;
    }
    const PtlasShape previous = ptlas.sizedFor;
    destroyPtlas();

    // monotonic, with headroom on the instance count, so loading models one by one does not reallocate each time
    const PtlasShape sized{
        .instances = std::max({templates.shape.instances, previous.instances * 2, kMinCapacity}),
        .partitions = std::max(templates.shape.partitions, previous.partitions),
        .maxPerPartition = std::max(templates.shape.maxPerPartition, previous.maxPerPartition),
    };
    const vk::AccelerationStructureBuildSizesInfoKHR sizes =
        device.vkdevice.getPartitionedAccelerationStructuresBuildSizesNV(buildInput(sized));
    ptlas.storage = createAlignedBuffer(sizes.accelerationStructureSize, kPtlasAlignment,
                                        vk::BufferUsageFlagBits2::eAccelerationStructureStorageKHR, "PtlasStorage");
    ptlas.scratch = createAlignedBuffer(sizes.buildScratchSize, kPtlasAlignment,
                                        vk::BufferUsageFlagBits2::eStorageBuffer, "PtlasScratch");
    ptlas.state = createAlignedBuffer(kPtlasStateRowBytes * sized.instances, alignof(glm::vec4),
                                      vk::BufferUsageFlagBits2::eStorageBuffer, "PtlasState");
    ptlas.sizedFor = sized;
    // the first build starts from nothing
    ptlas.version = ~0u;
    logInfo(std::format("PTLAS for {} instances, {} partitions, {} per partition: {} KiB, scratch {} KiB",
                        sized.instances, sized.partitions, sized.maxPerPartition,
                        divideRoundUp(static_cast<uint32_t>(sizes.accelerationStructureSize), 1024),
                        divideRoundUp(static_cast<uint32_t>(sizes.buildScratchSize), 1024)),
            "RayTracingScene");
}

void RayTracingScene::destroyPtlas()
{
    destroyAlignedBuffer(ptlas.storage);
    destroyAlignedBuffer(ptlas.scratch);
    destroyAlignedBuffer(ptlas.state);
    ptlas.sizedFor = {};
    ptlas.version = ~0u;
}

void RayTracingScene::createFrameBuffers(uint32_t frameSlot, uint32_t capacity)
{
    ZoneScopedN("RayTracingScene::createFrameBuffers");
    FrameBuffers& buffers = frames[frameSlot];
    destroyFrameBuffers(buffers);
    buffers.capacity = capacity;
    const VmaAllocator vma = allocator.allocator;

    buffers.rayInstances = createHostBuffer(sizeof(GpuRayInstance) * vk::DeviceSize{capacity}, vma, device.vkdevice,
                                            device.queueFamilyIndices, std::format("RayInstances_{}", frameSlot),
                                            kRayInstancesTracyName);
    buffers.builds = createHostBuffer(sizeof(GpuRayInstanceBuild) * vk::DeviceSize{capacity}, vma, device.vkdevice,
                                      device.queueFamilyIndices, std::format("RayInstanceBuilds_{}", frameSlot),
                                      kRayBuildsTracyName);
    // the build reads the op and its count as build input
    buffers.operation = createHostBuffer(kOperationBytes, vma, device.vkdevice, device.queueFamilyIndices,
                                         std::format("PtlasOperation_{}", frameSlot), kPtlasOperationTracyName,
                                         vk::BufferUsageFlagBits2::eAccelerationStructureBuildInputReadOnlyKHR);
    buffers.writeInfos =
        createAlignedBuffer(sizeof(vk::PartitionedAccelerationStructureWriteInstanceDataNV) * vk::DeviceSize{capacity},
                            alignof(vk::DeviceAddress),
                            vk::BufferUsageFlagBits2::eStorageBuffer |
                                vk::BufferUsageFlagBits2::eAccelerationStructureBuildInputReadOnlyKHR,
                            std::format("PtlasWriteInfos_{}", frameSlot));
    logInfo(std::format("slot {}: capacity {} instances", frameSlot, capacity), "RayTracingScene");
}

void RayTracingScene::destroyFrameBuffers(FrameBuffers& buffers)
{
    destroyHostBuffer(allocator.allocator, buffers.rayInstances, kRayInstancesTracyName);
    destroyHostBuffer(allocator.allocator, buffers.builds, kRayBuildsTracyName);
    destroyHostBuffer(allocator.allocator, buffers.operation, kPtlasOperationTracyName);
    destroyAlignedBuffer(buffers.writeInfos);
    buffers.capacity = 0;
    buffers.count = 0;
    buffers.version = ~0u;
    buffers.isSubmitted = false;
}

RayTracingScene::AlignedBuffer RayTracingScene::createAlignedBuffer(vk::DeviceSize bytes, vk::DeviceSize alignment,
                                                                    vk::BufferUsageFlags2 usage,
                                                                    std::string_view debugName) const
{
    AlignedBuffer target;
    // the slack lets the start move up to the alignment the build asks for
    createBuffer(bytes + alignment - 1, usage | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eDeviceLocal, target.buffer, target.memory, allocator.allocator,
                 device.vkdevice, device.queueFamilyIndices, debugName);
    target.address = alignUp(device.vkdevice.getBufferAddress({.buffer = *target.buffer}), alignment);
    return target;
}

void RayTracingScene::destroyAlignedBuffer(AlignedBuffer& target) const
{
    destroyVmaBuffer(allocator.allocator, target.buffer, target.memory);
    target.address = 0;
}

void RayTracingScene::recordBuild(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot)
{
    ZoneScopedN("RayTracingScene::recordBuild");
    FrameBuffers& buffers = frames[frameSlot];
    const uint32_t firstQuery = frameSlot * kQueriesPerFrame;
    buffers.isTimed = timestampPeriodNs > 0.0f;
    buffers.isSubmitted = true;
    if (buffers.isTimed) {
        cmd.resetQueryPool(*timestamps, firstQuery, kQueriesPerFrame);
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *timestamps, firstQuery);
    }

    // last frame's build on this queue wrote the PTLAS and read the write infos; last frame's instance pass wrote the
    // state rows this pass reads and rewrites
    const vk::MemoryBarrier2 reuseBarrier{
        .srcStageMask =
            vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR | vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR | vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask =
            vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
        .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR |
            vk::AccessFlagBits2::eAccelerationStructureWriteKHR | vk::AccessFlagBits2::eShaderStorageRead |
            vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &reuseBarrier});

    // a new layout or a new PTLAS: every row writes its instance into an empty PTLAS
    const bool isFullBuild = settings.isFullBuildForced || ptlas.version != templates.version;
    if (buffers.count > 0) {
        const RtInstancesPushData push{
            .frameRoot = frameData.frameRootAddress(frameSlot),
            .buildRows = buffers.builds.address,
            .writeInfos = buffers.writeInfos.address,
            .operation = buffers.operation.address,
            .ptlasState = ptlas.state.address,
            .count = buffers.count,
            .flags = isFullBuild ? RtInstancesFlag::FullBuild : 0u,
        };
        ShaderObjects::bindComputeShader(cmd, shaderObjects.rtShaders.writeInstances);
        cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
        cmd.dispatch(divideRoundUp(buffers.count, kRtInstancesGroupSize), 1, 1);
        // the write infos and the op's argument count are build input
        const vk::MemoryBarrier2 instanceBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &instanceBarrier});
    }
    if (buffers.isTimed) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *timestamps, firstQuery + 1);
    }

    // an empty scene still builds: the frame root always points at a valid PTLAS. In place after the first build
    // (VUID-...-pBuildInfo-10549 forbids overlap only between distinct source and destination); every build of one
    // PTLAS uses the input it was sized for
    cmd.buildPartitionedAccelerationStructuresNV(vk::BuildPartitionedAccelerationStructureInfoNV{
        .input = buildInput(ptlas.sizedFor),
        .srcAccelerationStructureData = isFullBuild ? 0 : ptlas.storage.address,
        .dstAccelerationStructureData = ptlas.storage.address,
        .scratchData = ptlas.scratch.address,
        .srcInfos = buffers.operation.address,
        .srcInfosCount = buffers.operation.address + kOpCountOffset,
    });
    ptlas.version = templates.version;
    if (buffers.isTimed) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *timestamps, firstQuery + 2);
    }
    // collectResults reads the op's argument count after the slot's timeline wait
    const vk::MemoryBarrier2 hostBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &hostBarrier});
}

void RayTracingScene::collectResults(uint32_t frameSlot)
{
    FrameBuffers& buffers = frames[frameSlot];
    if (buffers.isSubmitted) {
        buffers.isSubmitted = false;
        vk::BuildPartitionedAccelerationStructureIndirectCommandNV op{};
        std::memcpy(&op, buffers.operation.mapped, sizeof(op));
        lastWrittenInstances = op.argCount;
        TracyPlot("RT/PTLAS instance writes", static_cast<double>(op.argCount));
    }
    if (!buffers.isTimed) {
        return;
    }
    buffers.isTimed = false;
    const auto [result, ticks] =
        timestamps.getResults<uint64_t>(frameSlot * kQueriesPerFrame, kQueriesPerFrame,
                                        kQueriesPerFrame * sizeof(uint64_t), sizeof(uint64_t),
                                        vk::QueryResultFlagBits::e64);
    if (result != vk::Result::eSuccess) {
        return;
    }
    const auto toMs = [this](uint64_t begin, uint64_t end) {
        return static_cast<float>(end - begin) * timestampPeriodNs * 1e-6f;
    };
    histories[0][historyHead] = toMs(ticks[0], ticks[1]);
    histories[1][historyHead] = toMs(ticks[1], ticks[2]);
    TracyPlot("RT instance write (async)", static_cast<double>(histories[0][historyHead]));
    TracyPlot("PTLAS build (async)", static_cast<double>(histories[1][historyHead]));
    historyHead = (historyHead + 1) % kHistorySize;
    historyCount = std::min(historyCount + 1, kHistorySize);
}

RayTracingScene::Stats RayTracingScene::getStats() const noexcept
{
    // unfilled slots are zero, so summing the whole ring is exact
    const auto average = [this](const std::array<float, kHistorySize>& samples) {
        return historyCount == 0
            ? 0.0f
            : std::reduce(samples.begin(), samples.end(), 0.0f) / static_cast<float>(historyCount);
    };
    return Stats{
        .instances = static_cast<uint32_t>(templates.rows.size()),
        .capacity = ptlas.sizedFor.instances,
        .writtenInstances = lastWrittenInstances,
        .partitions = templates.shape.partitions,
        .instanceWriteMs = average(histories[0]),
        .tlasBuildMs = average(histories[1]),
    };
}
