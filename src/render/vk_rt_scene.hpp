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

// The per-frame ray-tracing scene (PBR plan C2 + C4, spec/features/ray_tracing_mega_geometry.md §3): one PTLAS
// instance per (node instance, mesh primitive with meshlets). A single PTLAS is updated in place every frame on the
// async compute queue from instance writes the GPU appends: every row after a layout change (a full build), otherwise
// only the rows whose transform changed, each in its grid partition. The ray-instance rows only change with the scene
// layout, so the CPU writes them per layout, never per frame
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
    [[nodiscard]] Stats getStats() const noexcept;

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

    // ray instances of one scene layout
    struct Templates
    {
        std::vector<GpuRayInstance> rows;
        // parallel to rows
        std::vector<GpuRayInstanceBuild> builds;
        PtlasShape shape;
        uint32_t generation = ~0u;
    };

    // device-local buffer whose address is aligned up inside a slack allocation
    struct AlignedBuffer
    {
        vk::raii::Buffer buffer = nullptr;
        VmaAllocation memory = nullptr;
        vk::DeviceAddress address = 0;
    };

    struct FrameBuffers
    {
        // host-written per layout: GpuRayInstance[capacity] and GpuRayInstanceBuild[capacity]
        HostBuffer rayInstances;
        HostBuffer builds;
        // host-reset per frame, GPU-appended: the build's one write-instance op and its op count
        HostBuffer operation;
        // GPU-written VkPartitionedAccelerationStructureWriteInstanceDataNV[capacity]
        AlignedBuffer writeInfos;
        uint32_t capacity = 0;
        uint32_t count = 0;
        uint32_t generation = ~0u;
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
        // layout generation the PTLAS was last fully built for
        uint32_t generation = ~0u;
    };

    void buildTemplates();
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
    size_t historyHead = 0;
    size_t historyCount = 0;
};
