#pragma once
#include "../Constants.h"
#include "core/vk_allocator.hpp"
#include "core/vk_asset_buffers.hpp"
#include "core/vk_device.hpp"
#include "core/vk_frame_data.hpp"
#include "scene/vk_scene.hpp"
#include "util/vk_utils.hpp"
#include "vk_shader_objects.hpp"

#include <array>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

// The per-frame ray-tracing scene (PBR plan C2, spec/features/ray_tracing_mega_geometry.md §3): one TLAS instance per
// (node instance, mesh primitive with meshlets), written on the GPU from the slot's node instances, then a full TLAS
// build. Recorded into the slot's async compute command buffer. The ray-instance rows only change with the scene
// layout, so the CPU writes them per layout, never per frame
class RayTracingScene
{
public:
    // rolling averages over kHistorySize frames; capacity is per frame slot
    struct Stats
    {
        uint32_t instances = 0;
        uint32_t capacity = 0;
        float instanceWriteMs = 0.0f;
        float tlasBuildMs = 0.0f;
    };

    RayTracingScene(const Device& device, const VkAllocator& allocator, const Scene& scene, const AssetBuffers& assets,
                    const FrameData& frameData, const ShaderObjects& shaderObjects);
    ~RayTracingScene();

    RayTracingScene(const RayTracingScene&) = delete;
    RayTracingScene& operator=(const RayTracingScene&) = delete;
    RayTracingScene(RayTracingScene&&) = delete;
    RayTracingScene& operator=(RayTracingScene&&) = delete;

    // the slot's previous compute work has completed: collects its timestamps, follows a new scene layout
    void prepareFrame(uint32_t frameSlot);
    // compute queue, heaps bound: instance write -> TLAS build. The slot's frame root must be written before submit
    void recordBuild(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot);

    [[nodiscard]] vk::DeviceAddress tlasAddress(uint32_t frameSlot) const noexcept { return frames[frameSlot].tlasAddress; }
    // GpuRayInstance[], indexed by InstanceIndex()
    [[nodiscard]] vk::DeviceAddress rayInstancesAddress(uint32_t frameSlot) const noexcept
    {
        return frames[frameSlot].rayInstances.address;
    }
    [[nodiscard]] Stats getStats() const noexcept;

private:
    static constexpr size_t kHistorySize = 120;
    // start, after the instance write, after the TLAS build
    static constexpr uint32_t kQueriesPerFrame = 3;

    // ray instances of one scene layout
    struct Templates
    {
        std::vector<GpuRayInstance> rows;
        // parallel to rows
        std::vector<vk::DeviceAddress> blasAddresses;
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
        // host-written per layout: GpuRayInstance[capacity] and the parallel BLAS addresses
        HostBuffer rayInstances;
        HostBuffer blasAddresses;
        // GPU-written VkAccelerationStructureInstanceKHR[capacity]
        AlignedBuffer tlasInstances;
        AlignedBuffer tlasStorage;
        AlignedBuffer scratch;
        vk::raii::AccelerationStructureKHR tlas = nullptr;
        vk::DeviceAddress tlasAddress = 0;
        uint32_t capacity = 0;
        uint32_t count = 0;
        uint32_t generation = ~0u;
        bool isTimed = false;
    };

    void buildTemplates();
    void uploadTemplates(uint32_t frameSlot);
    // the slot is not in flight: replaces its buffers and TLAS with ones for capacity instances
    void createFrameBuffers(uint32_t frameSlot, uint32_t capacity);
    void destroyFrameBuffers(FrameBuffers& buffers);
    [[nodiscard]] AlignedBuffer createAlignedBuffer(vk::DeviceSize bytes, vk::DeviceSize alignment,
                                                    vk::BufferUsageFlags2 usage, std::string_view debugName) const;
    void destroyAlignedBuffer(AlignedBuffer& target) const;
    [[nodiscard]] vk::AccelerationStructureBuildGeometryInfoKHR tlasBuildInfo(const vk::AccelerationStructureGeometryKHR& geometry) const;
    void collectTimestamps(uint32_t frameSlot);

    const Device& device;
    const VkAllocator& allocator;
    const Scene& scene;
    const AssetBuffers& assets;
    const FrameData& frameData;
    const ShaderObjects& shaderObjects;
    Templates templates;
    std::array<FrameBuffers, MAX_FRAMES_IN_FLIGHT> frames;

    vk::raii::QueryPool timestamps = nullptr;
    float timestampPeriodNs = 0.0f;
    std::array<std::array<float, kHistorySize>, 2> histories{};
    size_t historyHead = 0;
    size_t historyCount = 0;
};
