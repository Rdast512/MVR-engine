#pragma once
#include "../Constants.h"
#include "core/vk_allocator.hpp"
#include "core/vk_asset_buffers.hpp"
#include "core/vk_device.hpp"
#include "core/vk_frame_data.hpp"
#include "core/vk_frame_sync.hpp"
#include "scene/vk_scene.hpp"
#include "util/vk_utils.hpp"
#include "vk_shader_objects.hpp"

#include <array>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

// The per-frame ray-tracing scene (PBR plan C2 + C4, spec/features/planned/ray_tracing_mega_geometry.md §3): one PTLAS
// instance per (node instance, mesh primitive with meshlets). A single PTLAS is updated in place every frame on the
// async compute queue from instance writes the GPU appends: every row after a layout change (a full build), otherwise
// only the rows whose transform changed, each in its grid partition. The ray-instance rows only change with the scene
// layout, so the CPU writes them per layout, never per frame. With them go the emissive-triangle tables
// (spec/m7/e2_emissive_triangles.md): each ray instance's first triangle-light row, and one light_world task per
// emissive ray instance
class RayTracingScene
{
public:
    // rolling averages over kHistorySize frames
    struct Stats
    {
        uint32_t instances = 0;
        uint32_t capacity = 0;
        // instance writes of the last completed frame; 0 while nothing moves
        uint32_t writtenInstances = 0;
        uint32_t partitions = 0;
        float instanceWriteMs = 0.0f;
        float tlasBuildMs = 0.0f;
        // raw GPU clock ticks of the most recently collected compute submit (F9 overlap)
        uint64_t lastStartTick = 0;
        uint64_t lastEndTick = 0;
    };

    // runtime PTLAS controls, read by the next prepareFrame / recordBuild
    struct Settings
    {
        // grid cells are sized for about this many instances; a change rebuilds the templates and the PTLAS. ~16 and
        // ~256 showed no consistent win over 64 on the 5070 (plan C4)
        uint32_t instancesPerPartition = 64;
        // every frame writes every instance into an empty PTLAS: the full-build cost, for comparison
        bool isFullBuildForced = false;
        // reassign moved instances to their current XZ grid cell instead of keeping layout-time partitions (C-C4-5)
        bool isPartitionReassignmentEnabled = true;
    };

    RayTracingScene(const Device& device, const VkAllocator& allocator, const FrameSync& frameSync, const Scene& scene,
                    const AssetBuffers& assets, const FrameData& frameData, const ShaderObjects& shaderObjects);
    ~RayTracingScene();

    RayTracingScene(const RayTracingScene&) = delete;
    RayTracingScene& operator=(const RayTracingScene&) = delete;
    RayTracingScene(RayTracingScene&&) = delete;
    RayTracingScene& operator=(RayTracingScene&&) = delete;

    // the slot's previous compute work has completed: collects its results, follows a new scene layout.
    // lastSubmittedFrame: the frame number whose compute and ray work were submitted last; growing the PTLAS waits
    // for it, since every frame in flight traces the one PTLAS
    void prepareFrame(uint32_t frameSlot, uint64_t lastSubmittedFrame);
    // compute queue, heaps bound: instance writes -> PTLAS build. The slot's frame root must be written before submit,
    // and the submit must wait for the previous frame's ray passes at the AS build stage (the update is in place)
    void recordBuild(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot);

    [[nodiscard]] vk::DeviceAddress tlasAddress() const noexcept { return ptlas.storage.address; }
    // GpuRayInstance[], indexed by InstanceIndex()
    [[nodiscard]] vk::DeviceAddress rayInstancesAddress(uint32_t frameSlot) const noexcept
    {
        return frames[frameSlot].rayInstances.address;
    }
    // uint[], headers parallel to rayInstances (dense firstLight or kRayInstanceLightSparseBit | remapOffset)
    // followed by per-primitive sparse remap tails (m8 I14 F27)
    [[nodiscard]] vk::DeviceAddress rayInstanceLightsAddress(uint32_t frameSlot) const noexcept
    {
        return frames[frameSlot].rayInstanceLights.address;
    }
    // GpuTriangleLightRef[triangleLightCount()]
    [[nodiscard]] vk::DeviceAddress triangleLightRefsAddress(uint32_t frameSlot) const noexcept
    {
        return frames[frameSlot].triangleLightRefs.address;
    }
    // triangle lights of the layout as of the last prepareFrame
    [[nodiscard]] uint32_t triangleLightCount() const noexcept { return templates.triangleLightCount; }
    [[nodiscard]] std::span<const GpuTriangleLightRef> getTriangleLightRefs() const noexcept
    {
        return templates.lightRefs;
    }
    [[nodiscard]] std::span<const GpuRayInstance> getRayInstances() const noexcept { return templates.rows; }
    // kRayMaskGlass instances of the layout (m8 I13); 0 skips the glass pass and the shadow transmittance
    [[nodiscard]] uint32_t glassRowCount() const noexcept { return templates.glassRowCount; }
    [[nodiscard]] Stats getStats() const noexcept;
    // world box of every ray instance as of the last prepareFrame: what a ray can hit (NRC scene bounds, M4 G3);
    // empty (bboxMin > bboxMax) without instances
    [[nodiscard]] const BoundingBox& getSceneBounds() const noexcept { return templates.sceneBounds; }
    // true if the last prepareFrame rebuilt the layout or observed any dynamic entity (geometry or light) transform change
    [[nodiscard]] bool hasSceneChanged() const noexcept { return isSceneChangedThisFrame; }

    Settings settings;

private:
    static constexpr size_t kHistorySize = 120;
    // start, after the instance writes, after the PTLAS build
    static constexpr uint32_t kQueriesPerFrame = 3;

    // the PTLAS shape of one layout; the build input is never larger than the one the PTLAS was sized for
    struct PtlasShape
    {
        uint32_t instances = 0;
        uint32_t partitions = 0;
        uint32_t maxPerPartition = 0;
    };

    // per-entity dynamic tracking for sceneBounds growth (MG-B4) and partition reassignment (C-C4-5)
    struct DynamicEntity
    {
        EntityId id = 0;
        BoundingBox localBounds;
        Transform lastTransform;
        uint32_t firstRow = 0;
        uint32_t rowCount = 0;
    };

    // ray instances of one scene layout
    struct Templates
    {
        std::vector<GpuRayInstance> rows;
        // parallel to rows
        std::vector<GpuRayInstanceBuild> builds;
        // parallel to rows: primitive center in entity space (before entityMatrix), for partition reassignment
        std::vector<glm::vec3> rowLocalCenters;
        // per-row headers followed by sparse remap tails for partial textured emitters (m8 I14 F27)
        std::vector<uint32_t> rayInstanceLights;
        // one per active triangle light (m8 I14 F27)
        std::vector<GpuTriangleLightRef> lightRefs;
        std::vector<DynamicEntity> dynamicEntities;
        std::vector<uint32_t> partitionOccupancy;
        // dense cell (cy * gridAxis + cx) -> compacted Z-order partition index in [0, shape.partitions) (m8 MG-P5)
        std::vector<uint32_t> cellToPartition;
        glm::vec2 gridLower{0.0f};
        glm::vec2 gridExtent{1.0f};
        uint32_t gridAxis = 1;
        uint32_t triangleLightCount = 0;
        uint32_t glassRowCount = 0;
        PtlasShape shape;
        // FrameData::layoutGeneration and the grid density they were built for
        uint32_t layoutGeneration = ~0u;
        uint32_t instancesPerPartition = 0;
        bool isPartitionReassignmentEnabled = false;
        // bumped by every build; slots and the PTLAS compare against it
        uint32_t version = 0;
        // bumped when builds[].partition updates in place without rebuilding the layout
        uint32_t buildsVersion = 0;
        // world box of the instances; grows monotonically if an entity leaves it (MG-B4)
        BoundingBox sceneBounds;
    };

    // device-local buffer aligned at creation via VMA minAlignment (MG-B1)
    struct AlignedBuffer
    {
        vk::raii::Buffer buffer = nullptr;
        VmaAllocation memory = nullptr;
        vk::DeviceAddress address = 0;
    };

    struct FrameBuffers
    {
        // host-written per layout: GpuRayInstance[capacity], GpuRayInstanceBuild[capacity], rayInstanceLights
        // (headers + sparse remap tails), and GpuTriangleLightRef[triangleLightRefsCapacity]
        HostBuffer rayInstances;
        HostBuffer builds;
        HostBuffer rayInstanceLights;
        HostBuffer triangleLightRefs;
        // host-reset per frame, GPU-appended: the build's one write-instance op and its op count
        HostBuffer operation;
        // host-cached 4-byte readback of operation.argCount (MG-B2)
        HostBuffer argCountReadback;
        // GPU-written VkPartitionedAccelerationStructureWriteInstanceDataNV[capacity]
        AlignedBuffer writeInfos;
        uint32_t capacity = 0;
        uint32_t rayInstanceLightsCapacity = 0;
        uint32_t triangleLightRefsCapacity = 0;
        uint32_t count = 0;
        // template version uploaded
        uint32_t version = ~0u;
        uint32_t buildsVersion = ~0u;
        bool isTimed = false;
        bool isSubmitted = false;
    };

    // one per scene, traced by every frame in flight
    struct Ptlas
    {
        AlignedBuffer storage;
        AlignedBuffer scratch;
        // one row per instance: the transform the PTLAS holds, for change detection
        AlignedBuffer state;
        PtlasShape sizedFor;
        // template version the PTLAS was last fully built for
        uint32_t version = ~0u;
    };

    void buildTemplates();
    void updateDynamicEntities();
    void uploadTemplates(uint32_t frameSlot);
    // every frame in flight has finished with the PTLAS: replaces it with one for the templates' shape
    void createPtlas(uint64_t lastSubmittedFrame);
    void destroyPtlas();
    // the slot is not in flight: replaces its buffers with ones for capacity instances
    void createFrameBuffers(uint32_t frameSlot, uint32_t capacity);
    void destroyFrameBuffers(FrameBuffers& buffers);
    [[nodiscard]] AlignedBuffer createAlignedBuffer(vk::DeviceSize bytes, vk::DeviceSize alignment,
                                                    vk::BufferUsageFlags2 usage, std::string_view debugName) const;
    void destroyAlignedBuffer(AlignedBuffer& target) const;
    [[nodiscard]] vk::PartitionedAccelerationStructureInstancesInputNV buildInput(const PtlasShape& shape) const;
    void collectResults(uint32_t frameSlot);

    const Device& device;
    const VkAllocator& allocator;
    const FrameSync& frameSync;
    const Scene& scene;
    const AssetBuffers& assets;
    const FrameData& frameData;
    const ShaderObjects& shaderObjects;
    Templates templates;
    std::array<FrameBuffers, MAX_FRAMES_IN_FLIGHT> frames;
    Ptlas ptlas;
    uint32_t lastWrittenInstances = 0;

    vk::raii::QueryPool timestamps = nullptr;
    float timestampPeriodNs = 0.0f;
    std::array<std::array<float, kHistorySize>, 2> histories{};
    uint64_t lastStartTick = 0;
    uint64_t lastEndTick = 0;
    size_t historyHead = 0;
    size_t historyCount = 0;
    bool isSceneChangedThisFrame = false;
};
