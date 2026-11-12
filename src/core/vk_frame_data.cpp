#include "vk_frame_data.hpp"
#include <algorithm>
#include <cassert>
#include <format>
#include "static_headers/logger.hpp"
#include "util/vk_tracy.hpp"

namespace
{
    constexpr const char* kInstanceUboTracyName = "GPU/InstanceUBO";
    constexpr const char* kNodeInstanceTracyName = "GPU/NodeInstances";
} // namespace

FrameData::FrameData(const Device& deviceWrapper, const VkAllocator& allocator, const AssetBuffers& assets,
                     const GeometryStore& geometryStore, ObjectStorage& objectStorage)
    : allocator(allocator),
      device(deviceWrapper.vkdevice),
      queueFamilyIndices(deviceWrapper.queueFamilyIndices),
      assets(assets),
      geometryStore(geometryStore),
      objectStorage(objectStorage)
{
    logInfo("init() started", "FrameData");
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        // fixed size and count: not a Tracy memory pool
        frameRoots[i] = createHostBuffer(sizeof(GpuFrameRoot), allocator.allocator, device, queueFamilyIndices,
                                         std::format("FrameRoot_{}", i), nullptr);
    }
    syncScene();
}

FrameData::~FrameData()
{
    logInfo("Destructor called", "FrameData");
    destroyInstanceUboBuffers();
    for (HostBuffer& root : frameRoots) {
        destroyHostBuffer(allocator.allocator, root, nullptr);
    }
    for (HostBuffer& rows : nodeInstanceBuffers) {
        destroyHostBuffer(allocator.allocator, rows, kNodeInstanceTracyName);
    }
}

void FrameData::syncScene()
{
    if (syncedSceneGeneration == assets.sceneGeneration()) {
        return;
    }
    ZoneScopedN("FrameData::syncScene");
    syncedSceneGeneration = assets.sceneGeneration();
    // at least one row: the frame root always holds a live instance address
    ensureInstanceCapacity(std::max(objectStorage.size(), 1u));
    rebuildNodeInstanceLayout();
    tracyPlotCapacities();
}

void FrameData::destroyInstanceUboBuffers()
{
    ZoneScopedN("FrameData::destroyInstanceUboBuffers");
    for (HostBuffer& ubo : instanceUbos) {
        destroyHostBuffer(allocator.allocator, ubo, kInstanceUboTracyName);
    }
    instanceCapacity = 0;
}

void FrameData::tracyPlotCapacities() const
{
    TracyPlot("Vulkan/InstanceCapacity", static_cast<double>(instanceCapacity));
    TracyPlot("Vulkan/NodeInstanceCount", static_cast<double>(nodeInstanceCount));
    TracyPlot("Vulkan/InstanceUboBytes", static_cast<double>(sizeof(GpuObjectUB) * instanceCapacity));
}

void FrameData::writeFrameRoot(uint32_t frameSlot, const FrameRootInputs& inputs)
{
    *static_cast<GpuFrameRoot*>(frameRoots[frameSlot].mapped) = GpuFrameRoot{
        .camera = inputs.camera,
        .instances = instanceUbos[frameSlot].address,
        .models = assets.modelTableAddress(),
        .primitives = assets.asset(AssetBuffer::Primitives).address,
        .materials = assets.asset(AssetBuffer::Materials).address,
        .pbrExtensions = assets.asset(AssetBuffer::PbrExt).address,
        .lights = assets.asset(AssetBuffer::Lights).address,
        .tlas = 0,
        .nodeInstances = nodeInstanceBuffers[frameSlot].address,
        .drawRecords = inputs.drawRecords,
        .visibleMeshlets = inputs.visibleMeshlets,
        // the light buffer's used bytes track LightStore::uploaded exactly (AssetBuffers::uploadLights)
        .lightCount = assets.uploadedCount(AssetBuffer::Lights),
        .instanceCount = objectStorage.size(),
        .primitiveCount = assets.uploadedCount(AssetBuffer::Primitives),
        .nodeInstanceCount = nodeInstanceCount,
        .lateVisibleBase = inputs.lateVisibleBase,
        .debugView = inputs.debugView,
        .cullCounters = inputs.cullCounters,
        .triangleCull = inputs.triangleCullFlags,
        .subpixelSnap = inputs.subpixelSnap,
        .targets = inputs.targets,
        .frameFlags = inputs.frameFlags,
        .pad0 = 0,
        .rayInstances = 0,
        .rtxdiConstants = 0,
        .cacheConstants = 0,
        .frameIndex = inputs.frameIndex,
        .exposure = 1.0f,
    };
}

void FrameData::write(uint32_t currentImage, const FrameRootInputs& inputs)
{
    ZoneScopedN("FrameData::write");
    assert(syncedSceneGeneration == assets.sceneGeneration() && "syncScene missing after a scene change");
    writeFrameRoot(currentImage, inputs);
    // reads prevModelMatrices before the entity loop below advances them
    writeNodeInstances(currentImage);

    // write-combined memory: whole rows, never read back
    auto* instances = static_cast<GpuObjectUB*>(instanceUbos[currentImage].mapped);
    for (EntityId id = 0; id < objectStorage.size(); ++id) {
        const glm::mat4 model = computeModelMatrix(objectStorage.transforms[id]);
        if ((objectStorage.flags[id] & EntityFlag::Active) == 0) {
            // reactivation must not jump by the whole pause's motion
            objectStorage.prevModelMatrices[id] = model;
            continue;
        }
        instances[id] = GpuObjectUB{
            .modelMatrix = model,
            .prevModelMatrix = objectStorage.prevModelMatrices[id],
            .boundingSphere = transformBoundingSphere(model, objectStorage.localBounds[id]),
            // entities loaded from one file share its model
            .modelIndex = objectStorage.modelIds[id],
            .instanceFlags = objectStorage.flags[id],
            .pad0 = 0,
            .pad1 = 0,
        };
        objectStorage.prevModelMatrices[id] = model;
    }
}

void FrameData::rebuildNodeInstanceLayout()
{
    entityNodeInstanceFirsts.resize(objectStorage.size());
    const std::span<const ModelAllocation> models = assets.models();
    uint32_t rows = 0;
    for (EntityId id = 0; id < objectStorage.size(); ++id) {
        entityNodeInstanceFirsts[id] = rows;
        const ModelId modelId = objectStorage.modelIds[id];
        if (modelId != kInvalidModelId) {
            rows += models[modelId].nodes.count;
        }
    }
    nodeInstanceCount = rows;
    ++nodeInstanceLayoutGeneration;
    if (rows <= nodeInstanceCapacity && nodeInstanceBuffers[0].memory != nullptr) {
        return;
    }
    // callers hold the GPU idle, so every frame slot can be replaced
    nodeInstanceCapacity = std::max({rows, nodeInstanceCapacity * 2, 1u});
    logInfo(std::format("node instances: {} rows, capacity {}", rows, nodeInstanceCapacity), "FrameData");
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        destroyHostBuffer(allocator.allocator, nodeInstanceBuffers[i], kNodeInstanceTracyName);
        nodeInstanceBuffers[i] =
            createHostBuffer(sizeof(GpuNodeInstance) * static_cast<vk::DeviceSize>(nodeInstanceCapacity),
                             allocator.allocator, device, queueFamilyIndices, std::format("NodeInstances_{}", i),
                             kNodeInstanceTracyName);
    }
}

void FrameData::writeNodeInstances(uint32_t frameSlot)
{
    ZoneScopedN("FrameData::writeNodeInstances");
    // write-combined memory: whole rows, never read back
    auto* rows = static_cast<GpuNodeInstance*>(nodeInstanceBuffers[frameSlot].mapped);
    const auto entityCount = static_cast<EntityId>(entityNodeInstanceFirsts.size());
    const std::span<const ModelAllocation> models = assets.models();
    for (EntityId id = 0; id < entityCount; ++id) {
        const ModelId modelId = objectStorage.modelIds[id];
        if (modelId == kInvalidModelId || (objectStorage.flags[id] & EntityFlag::Active) == 0) {
            continue;
        }
        const glm::mat4 entityMatrix = computeModelMatrix(objectStorage.transforms[id]);
        const glm::mat4& prevEntityMatrix = objectStorage.prevModelMatrices[id];
        const ElementRange nodes = models[modelId].nodes;
        for (uint32_t k = 0; k < nodes.count; ++k) {
            const NodeDraw& node = geometryStore.nodeDraws[nodes.first + k];
            const glm::mat4 world = entityMatrix * node.localToModel;
            // the entity transform can mirror too, so winding follows the combined matrix
            const bool isMirrored = glm::determinant(glm::mat3(world)) < 0.0f;
            rows[entityNodeInstanceFirsts[id] + k] = GpuNodeInstance{
                .worldMatrix = world,
                .prevWorldMatrix = prevEntityMatrix * node.localToModel,
                .entityIndex = id,
                .modelIndex = modelId,
                .nodeIndex = k,
                .flags = (node.flags & ~NodeFlag::FlipWinding) | (isMirrored ? NodeFlag::FlipWinding : 0u),
            };
        }
    }
}

void FrameData::ensureInstanceCapacity(uint32_t entityCount)
{
    if (entityCount <= instanceCapacity)
    {
        return;
    }

    ZoneScopedN("FrameData::ensureInstanceCapacity");
    // headroom so interactive loads do not reallocate every time
    const uint32_t newCapacity = std::max(entityCount, instanceCapacity * 2);
    logInfo(std::format("Growing instance GpuObjectUB capacity {} -> {}", instanceCapacity, newCapacity),
             "FrameData");

    destroyInstanceUboBuffers();
    instanceCapacity = newCapacity;

    const vk::DeviceSize bufferSize = sizeof(GpuObjectUB) * static_cast<vk::DeviceSize>(instanceCapacity);
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
    {
        instanceUbos[i] = createHostBuffer(bufferSize, allocator.allocator, device, queueFamilyIndices,
                                           std::format("InstanceObjectUB_{}", i), kInstanceUboTracyName);
    }
}
