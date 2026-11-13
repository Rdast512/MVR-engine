#include "vk_rt_scene.hpp"
#include "../util/debug.hpp"
#include "../util/vk_tracy.hpp"
#include "push_data.hpp"
#include "static_headers/logger.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace
{
    constexpr const char* kRayInstancesTracyName = "GPU/RayInstances";
    constexpr const char* kBlasAddressesTracyName = "GPU/RayBlasAddresses";
    // first allocation, in instances
    constexpr uint32_t kMinCapacity = 64;
    // matches shaders/passes/rt_instances.slang
    constexpr uint32_t kRtInstancesGroupSize = 64;
    // VkAccelerationStructureInstanceKHR::instanceCustomIndex holds the primitive row
    constexpr uint32_t kCustomIndexLimit = 1u << 24;
    // VkAccelerationStructureCreateInfoKHR::offset must be a multiple of 256
    constexpr vk::DeviceSize kTlasStorageAlignment = 256;
    // VkAccelerationStructureGeometryInstancesDataKHR::data
    constexpr vk::DeviceSize kInstanceDataAlignment = 16;

    [[nodiscard]] constexpr vk::DeviceSize alignUp(vk::DeviceSize size, vk::DeviceSize alignment)
    {
        return (size + alignment - 1) / alignment * alignment;
    }

    constexpr uint32_t divideRoundUp(uint32_t value, uint32_t divisor) { return (value + divisor - 1) / divisor; }

    // the geometry unions are not aggregates
    [[nodiscard]] vk::AccelerationStructureGeometryKHR instanceGeometry(vk::DeviceAddress instances)
    {
        vk::AccelerationStructureGeometryInstancesDataKHR instanceData{};
        instanceData.data.deviceAddress = instances;
        vk::AccelerationStructureGeometryKHR geometry{.geometryType = vk::GeometryTypeKHR::eInstances};
        // assigning the whole struct keeps its sType; a zeroed union member has none
        geometry.geometry.instances = instanceData;
        return geometry;
    }
} // namespace

RayTracingScene::RayTracingScene(const Device& device, const VkAllocator& allocator, const Scene& scene,
                                 const AssetBuffers& assets, const FrameData& frameData,
                                 const ShaderObjects& shaderObjects) :
    device(device), allocator(allocator), scene(scene), assets(assets), frameData(frameData),
    shaderObjects(shaderObjects)
{
    static_assert(sizeof(vk::AccelerationStructureInstanceKHR) == 64);
    const vk::PhysicalDeviceLimits& limits = device.capabilities.properties2.properties.limits;
    const auto queueFamilies = device.physicalDevice.getQueueFamilyProperties();
    if (limits.timestampPeriod > 0.0f && queueFamilies[device.computeIndex].timestampValidBits != 0) {
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
}

void RayTracingScene::prepareFrame(uint32_t frameSlot)
{
    ZoneScopedN("RayTracingScene::prepareFrame");
    collectTimestamps(frameSlot);
    if (templates.generation != frameData.layoutGeneration()) {
        buildTemplates();
    }
    if (frames[frameSlot].generation != templates.generation) {
        uploadTemplates(frameSlot);
    }
}

// same entity filter as the cull templates (MeshletCulling::buildTemplates): one row per drawn primitive
void RayTracingScene::buildTemplates()
{
    ZoneScopedN("RayTracingScene::buildTemplates");
    templates.rows.clear();
    templates.blasAddresses.clear();
    templates.generation = frameData.layoutGeneration();

    const ObjectStorage& storage = scene.objectStorage;
    const GeometryStore& geometry = scene.geometryStore;
    const uint32_t entityCount = assets.asset(AssetBuffer::Primitives).address == 0 ? 0 : storage.size();
    const std::span<const uint32_t> nodeInstanceFirsts = frameData.entityNodeInstanceFirst();
    for (EntityId id = 0; id < entityCount; ++id) {
        // an entity whose load is not flushed yet has no GPU model and no node-instance rows
        if ((storage.flags[id] & EntityFlag::Active) == 0 || storage.modelIds[id] == kInvalidModelId) {
            continue;
        }
        const ModelId modelId = storage.modelIds[id];
        const ModelAllocation& model = assets.models()[modelId];
        // indexed by model-relative primitive row
        const std::span<const vk::DeviceAddress> blas = assets.rayGeometry().blasAddresses(modelId);
        for (uint32_t k = 0; k < model.nodes.count; ++k) {
            const NodeDraw& node = geometry.nodeDraws[model.nodes.first + k];
            for (uint32_t p = node.firstPrimitive; p < node.firstPrimitive + node.primitiveCount; ++p) {
                const vk::DeviceAddress address = blas[p - model.primitives.first];
                // a primitive without meshlets has no BLAS
                if (address == 0) {
                    continue;
                }
                templates.rows.push_back(GpuRayInstance{.nodeInstanceIndex = nodeInstanceFirsts[id] + k,
                                                        .primitiveRow = p});
                templates.blasAddresses.push_back(address);
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
    logInfo(std::format("rt templates: {} instances", count), "RayTracingScene");
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
    std::memcpy(buffers.blasAddresses.mapped, templates.blasAddresses.data(),
                std::span(templates.blasAddresses).size_bytes());
    buffers.count = count;
    buffers.generation = templates.generation;
    TracyPlot("RT/Instances", static_cast<double>(count));
}

vk::AccelerationStructureBuildGeometryInfoKHR
RayTracingScene::tlasBuildInfo(const vk::AccelerationStructureGeometryKHR& geometry) const
{
    // full build every frame: entities move, and a refit degrades with motion (spec §10.3)
    return vk::AccelerationStructureBuildGeometryInfoKHR{
        .type = vk::AccelerationStructureTypeKHR::eTopLevel,
        .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
        .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
        .geometryCount = 1,
        .pGeometries = &geometry,
    };
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
    buffers.blasAddresses = createHostBuffer(sizeof(vk::DeviceAddress) * vk::DeviceSize{capacity}, vma,
                                             device.vkdevice, device.queueFamilyIndices,
                                             std::format("RayBlasAddresses_{}", frameSlot), kBlasAddressesTracyName);
    buffers.tlasInstances =
        createAlignedBuffer(sizeof(vk::AccelerationStructureInstanceKHR) * vk::DeviceSize{capacity},
                            kInstanceDataAlignment,
                            vk::BufferUsageFlagBits2::eStorageBuffer |
                                vk::BufferUsageFlagBits2::eAccelerationStructureBuildInputReadOnlyKHR,
                            std::format("TlasInstances_{}", frameSlot));

    const vk::AccelerationStructureGeometryKHR geometry = instanceGeometry(buffers.tlasInstances.address);
    const vk::AccelerationStructureBuildGeometryInfoKHR buildInfo = tlasBuildInfo(geometry);
    const vk::AccelerationStructureBuildSizesInfoKHR sizes = device.vkdevice.getAccelerationStructureBuildSizesKHR(
        vk::AccelerationStructureBuildTypeKHR::eDevice, buildInfo, capacity);
    buffers.tlasStorage = createAlignedBuffer(sizes.accelerationStructureSize, kTlasStorageAlignment,
                                              vk::BufferUsageFlagBits2::eAccelerationStructureStorageKHR,
                                              std::format("TlasStorage_{}", frameSlot));
    buffers.scratch =
        createAlignedBuffer(sizes.buildScratchSize,
                            device.capabilities.accelerationStructure.minAccelerationStructureScratchOffsetAlignment,
                            vk::BufferUsageFlagBits2::eStorageBuffer, std::format("TlasScratch_{}", frameSlot));

    vk::raii::Buffer& storage = buffers.tlasStorage.buffer;
    const vk::DeviceAddress storageBase = device.vkdevice.getBufferAddress({.buffer = *storage});
    buffers.tlas = vk::raii::AccelerationStructureKHR(
        device.vkdevice, vk::AccelerationStructureCreateInfoKHR{
                             .buffer = *storage,
                             .offset = buffers.tlasStorage.address - storageBase,
                             .size = sizes.accelerationStructureSize,
                             .type = vk::AccelerationStructureTypeKHR::eTopLevel,
                         });
    setDebugName(device.vkdevice, buffers.tlas, std::format("Tlas_{}", frameSlot));
    buffers.tlasAddress =
        device.vkdevice.getAccelerationStructureAddressKHR({.accelerationStructure = *buffers.tlas});
    logInfo(std::format("slot {}: capacity {} instances, TLAS {} KiB, scratch {} KiB", frameSlot, capacity,
                        divideRoundUp(static_cast<uint32_t>(sizes.accelerationStructureSize), 1024),
                        divideRoundUp(static_cast<uint32_t>(sizes.buildScratchSize), 1024)),
            "RayTracingScene");
}

void RayTracingScene::destroyFrameBuffers(FrameBuffers& buffers)
{
    // the structure before the buffer that backs it
    buffers.tlas = nullptr;
    buffers.tlasAddress = 0;
    destroyHostBuffer(allocator.allocator, buffers.rayInstances, kRayInstancesTracyName);
    destroyHostBuffer(allocator.allocator, buffers.blasAddresses, kBlasAddressesTracyName);
    destroyAlignedBuffer(buffers.tlasInstances);
    destroyAlignedBuffer(buffers.tlasStorage);
    destroyAlignedBuffer(buffers.scratch);
    buffers.capacity = 0;
    buffers.count = 0;
    buffers.generation = ~0u;
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
    if (buffers.isTimed) {
        cmd.resetQueryPool(*timestamps, firstQuery, kQueriesPerFrame);
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *timestamps, firstQuery);
    }

    // this slot's previous build on this queue read the instances and wrote the scratch and the TLAS
    const vk::MemoryBarrier2 reuseBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
        .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
        .dstStageMask =
            vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
        .dstAccessMask =
            vk::AccessFlagBits2::eAccelerationStructureReadKHR | vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &reuseBarrier});

    if (buffers.count > 0) {
        const RtInstancesPushData push{
            .frameRoot = frameData.frameRootAddress(frameSlot),
            .blasAddresses = buffers.blasAddresses.address,
            .tlasInstances = buffers.tlasInstances.address,
            .count = buffers.count,
            .pad0 = 0,
        };
        ShaderObjects::bindComputeShader(cmd, shaderObjects.rtShaders.writeInstances);
        cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
        cmd.dispatch(divideRoundUp(buffers.count, kRtInstancesGroupSize), 1, 1);
        // instance build input is read as shader data by the build
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

    // an empty scene still builds: the frame root always points at a valid TLAS
    const vk::AccelerationStructureGeometryKHR geometry = instanceGeometry(buffers.tlasInstances.address);
    vk::AccelerationStructureBuildGeometryInfoKHR buildInfo = tlasBuildInfo(geometry);
    buildInfo.dstAccelerationStructure = *buffers.tlas;
    buildInfo.scratchData.deviceAddress = buffers.scratch.address;
    const vk::AccelerationStructureBuildRangeInfoKHR range{.primitiveCount = buffers.count};
    const vk::AccelerationStructureBuildRangeInfoKHR* const ranges = &range;
    cmd.buildAccelerationStructuresKHR(buildInfo, ranges);
    if (buffers.isTimed) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *timestamps, firstQuery + 2);
    }
}

void RayTracingScene::collectTimestamps(uint32_t frameSlot)
{
    FrameBuffers& buffers = frames[frameSlot];
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
    TracyPlot("TLAS build (async)", static_cast<double>(histories[1][historyHead]));
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
        .capacity = std::ranges::max_element(frames, {}, &FrameBuffers::capacity)->capacity,
        .instanceWriteMs = average(histories[0]),
        .tlasBuildMs = average(histories[1]),
    };
}
