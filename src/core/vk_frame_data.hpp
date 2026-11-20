#pragma once
#include "Constants.h"
#include "geometry_store.hpp"
#include "object_storage.hpp"
#include "util/vk_utils.hpp"
#include "vk_allocator.hpp"
#include "vk_asset_buffers.hpp"
#include "vk_device.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

// everything the frame root needs from the passes of this frame; built and consumed in one Renderer::drawFrame
struct FrameRootInputs {
    vk::DeviceAddress camera;
    vk::DeviceAddress targets;
    vk::DeviceAddress drawRecords;
    vk::DeviceAddress visibleMeshlets;
    vk::DeviceAddress cullCounters;
    // RayTracingScene of this slot, built on the async compute queue
    vk::DeviceAddress tlas;
    vk::DeviceAddress rayInstances;
    // WorldLights of this slot, written on the async compute queue
    vk::DeviceAddress worldLights;
    // WorldLights of the previous frame's slot; 0 on a history reset
    vk::DeviceAddress prevWorldLights;
    // RtxdiContext constants of this slot, written after its fence
    vk::DeviceAddress rtxdiConstants;
    // NrcCache constants of this slot, written while its ray commands are recorded
    vk::DeviceAddress cacheConstants;
    uint32_t lateVisibleBase;
    uint32_t triangleCullFlags;
    float subpixelSnap;
    // DebugViewSettings::packed() of the frame being recorded
    uint32_t debugView;
    uint32_t frameFlags;
    uint32_t frameIndex;
    // WorldLights rows: the asset's lights, then the engine's sun
    uint32_t lightCount;
    uint32_t localLightCount;
    // GpuRayDebugCounters of this slot; 0 unless kDebugRays
    vk::DeviceAddress rayDebug;
};

// Per-frame-slot host-visible data rewritten by the CPU every frame: the GpuFrameRoot, the GpuObjectUB rows and the
// entity x node GpuNodeInstance rows. Shaders reach everything else through the frame root.
class FrameData {
public:
    // creates the frame roots and runs the first syncScene
    FrameData(const Device& deviceWrapper, const VkAllocator& allocator, const AssetBuffers& assets,
              const GeometryStore& geometryStore, ObjectStorage& objectStorage);
    ~FrameData();

    FrameData(const FrameData&) = delete;
    FrameData& operator=(const FrameData&) = delete;
    FrameData(FrameData&&) = delete;
    FrameData& operator=(FrameData&&) = delete;

    // after every load, instance and unload, GPU idle: grows the object rows to the entity count and rebuilds the
    // node-instance layout when AssetBuffers::sceneGeneration moved
    void syncScene();
    // after the slot's fence and command recording; the layout must be current
    void write(uint32_t frameSlot, const FrameRootInputs& inputs);

    [[nodiscard]] vk::DeviceAddress frameRootAddress(uint32_t frameSlot) const noexcept
    {
        return frameRoots[frameSlot].address;
    }
    // bumped by every node-instance layout rebuild; consumers cache per-layout data against it
    [[nodiscard]] uint32_t layoutGeneration() const noexcept { return nodeInstanceLayoutGeneration; }
    // first node-instance row per entity (entity-major)
    [[nodiscard]] std::span<const uint32_t> entityNodeInstanceFirst() const noexcept { return entityNodeInstanceFirsts; }

private:
    static constexpr uint32_t kNoGeneration = ~0u;

    // grows/recreates the per-frame GpuObjectUB arrays to fit entityCount entries
    void ensureInstanceCapacity(uint32_t entityCount);
    void destroyInstanceUboBuffers();
    // entity-major node-instance rows; grows the per-frame buffers (GPU idle)
    void rebuildNodeInstanceLayout();
    void writeFrameRoot(uint32_t frameSlot, const FrameRootInputs& inputs);
    void writeNodeInstances(uint32_t frameSlot);
    void tracyPlotCapacities() const;

    const VkAllocator& allocator;
    const vk::raii::Device& device;
    const std::vector<uint32_t>& queueFamilyIndices;
    const AssetBuffers& assets;
    const GeometryStore& geometryStore;
    ObjectStorage& objectStorage;

    // one GpuObjectUB[capacity] buffer per frame-in-flight, rewritten by the CPU every frame
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> instanceUbos;
    // GpuObjectUB slots per frame buffer, may exceed entity count
    uint32_t instanceCapacity = 0;
    // GpuNodeInstance[nodeInstanceCapacity] per frame slot: one row per entity x node, rewritten every frame
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> nodeInstanceBuffers;
    uint32_t nodeInstanceCapacity = 0;
    std::vector<uint32_t> entityNodeInstanceFirsts;
    uint32_t nodeInstanceCount = 0;
    uint32_t nodeInstanceLayoutGeneration = 0;
    // AssetBuffers::sceneGeneration the rows were last synced to
    uint32_t syncedSceneGeneration = kNoGeneration;
    // one GpuFrameRoot per frame-in-flight, rewritten every frame
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> frameRoots;
};
