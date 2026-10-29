#pragma once

#include "../Constants.h"
#include "core/types.hpp"
#include "core/vk_device.hpp"
#include "core/vk_resource_manager.hpp"
#include "scene/vk_camera.hpp"
#include "vk_dgc.hpp"
#include "vk_shader_objects.hpp"

#include <array>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

// read once per recorded frame; the UI changes them between frames
struct MeshletCullSettings
{
    bool isInstanceFrustumEnabled = true;
    bool isFrustumEnabled = true;
    bool isConeEnabled = true;
    bool isStatsEnabled = true;
    // keeps culling against the camera captured when it was frozen; fly out to inspect the result
    bool isCameraFrozen = false;
    // 0 = device maxMeshWorkGroupCount; small values force many DGC sequences per execute
    uint32_t maxMeshletsPerSequence = 0;
};

// one retired frame: CPU-side totals plus the GPU counters read back after its fence
struct MeshletCullStats
{
    uint32_t instances = 0;
    uint32_t drawRecords = 0;
    uint32_t totalMeshlets = 0;
    uint32_t meshletsPerSequence = 0;
    std::array<uint32_t, kMeshStateGroupCount> groupMeshlets{};
    // draw records of the instances that passed the instance tier
    uint32_t visibleDrawRecords = 0;
    std::array<uint32_t, kMeshStateGroupCount> visibleMeshlets{};
    std::array<uint32_t, kMeshStateGroupCount> sequences{};
    // zero unless MeshletCullSettings::isStatsEnabled
    uint32_t instancesCulled = 0;
    uint32_t testedMeshlets = 0;
    uint32_t frustumCulled = 0;
    uint32_t coneCulled = 0;
    uint32_t unculledMeshlets = 0;
    float cpuPrepareMs = 0.0f;
};

// GPU culling in compute: node instances (frustum) -> draw records -> meshlets (frustum + normal cone) ->
// per state group visible lists -> DGC sequences
class MeshletCulling
{
public:
    MeshletCulling(Device& device, ResourceManager& resourceManager, ShaderObjects& shaderObjects,
                   DeviceGeneratedCommands& dgc);
    ~MeshletCulling();

    MeshletCulling(const MeshletCulling&) = delete;
    MeshletCulling& operator=(const MeshletCulling&) = delete;
    MeshletCulling(MeshletCulling&&) = delete;
    MeshletCulling& operator=(MeshletCulling&&) = delete;

    // after the slot's fence: reads back its counters, refreshes its templates when the scene layout changed,
    // writes the cull view and sizes the DGC buffers
    void prepareFrame(uint32_t frameSlot, const Camera& camera);
    // outside the render pass, before DGC preprocess; heaps must be bound
    void recordCull(const vk::raii::CommandBuffer& cmd) const;

    [[nodiscard]] const MeshletCullStats& getStats() const noexcept { return stats; }

    MeshletCullSettings settings;

private:
    static constexpr uint32_t kNoGeneration = ~0u;

    // built once per node-instance layout, uploaded to every frame slot
    struct SceneTemplates
    {
        std::vector<GpuInstanceCullItem> instances;
        std::vector<GpuDrawRecord> drawRecords;
        std::array<uint32_t, kMeshStateGroupCount> groupMeshlets{};
        // ResourceManager::nodeInstanceLayoutGeneration it was built from
        uint32_t generation = kNoGeneration;
    };
    // per frame slot; replaced only while the slot is not in flight
    struct FrameBuffers
    {
        HostBuffer cullView;
        // GpuCullCounters in host-visible VRAM: atomics stay on the device, the CPU reads 48 B after the fence
        HostBuffer counters;
        HostBuffer instanceItems;
        HostBuffer drawRecordTemplates;
        uint32_t instanceCapacity = 0;
        // also the capacity of drawRecords
        uint32_t drawRecordCapacity = 0;
        // written by cullInstances
        DeviceBuffer drawRecords;
        DeviceBuffer visibleMeshlets;
        uint32_t visibleCapacity = 0;
        // SceneTemplates::generation of the uploaded templates
        uint32_t generation = kNoGeneration;
    };
    // what prepareFrame recorded into a slot, paired with its counters on readback
    struct RecordedFrame
    {
        MeshletCullStats totals;
        bool hasResults = false;
    };

    void readBackCounters(uint32_t frameSlot);
    void buildTemplates();
    void uploadTemplates(uint32_t frameSlot);
    void writeCullView(uint32_t frameSlot, const Camera& camera, uint32_t perSequence);
    [[nodiscard]] uint32_t meshletsPerSequence(uint32_t totalMeshlets) const;
    [[nodiscard]] MeshStateGroup stateGroupOf(const PrimitiveDraw& primitive) const;
    void createFrameBuffers();
    void destroyFrameBuffers();

    Device& device;
    ResourceManager& resourceManager;
    ShaderObjects& shaderObjects;
    DeviceGeneratedCommands& dgc;

    SceneTemplates templates;
    std::array<FrameBuffers, MAX_FRAMES_IN_FLIGHT> frames;
    std::array<RecordedFrame, MAX_FRAMES_IN_FLIGHT> recorded{};
    MeshletCullStats stats;

    // set by prepareFrame for recordCull
    uint32_t frame = 0;

    // captured on the frame the camera froze
    glm::mat4 frozenViewProj{1.0f};
    glm::vec3 frozenPosition{0.0f};
    bool wasCameraFrozen = false;
};
