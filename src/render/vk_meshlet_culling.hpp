#pragma once

#include "../Constants.h"
#include "core/types.hpp"
#include "core/vk_device.hpp"
#include "core/vk_resource_manager.hpp"
#include "scene/vk_camera.hpp"
#include "vk_dgc.hpp"
#include "vk_hiz.hpp"
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
    // hi-z test of node instances: early against the previous pyramid, late re-tests the candidates
    bool isOcclusionEnabled = true;
    bool isStatsEnabled = true;
    // keeps culling against the camera captured when it was frozen; fly out to inspect the result
    bool isCameraFrozen = false;
    // per-triangle tests in the mesh shaders; off by default until measured (meshlet_culling_options.md 2D)
    bool isTriangleBackfaceEnabled = false;
    bool isSmallTriangleEnabled = false;
    // 0 = device maxMeshWorkGroupCount; small values force many DGC sequences per execute
    uint32_t maxMeshletsPerSequence = 0;
};

// whether the early phase may test against the previous pyramid; otherwise it skips occlusion and the late phase
// gets no candidates
enum class OcclusionHistory : uint32_t
{
    Valid = 0,
    Disabled,       // MeshletCullSettings::isOcclusionEnabled is off
    NoPyramid,      // first frame or resize: the pyramid was recreated
    CameraCut,      // teleport or projection change
    SceneChanged,   // node-instance layout changed
    CameraFrozen,   // the pyramid follows the render camera, not the frozen cull camera
};

[[nodiscard]] constexpr const char* occlusionHistoryName(OcclusionHistory history)
{
    switch (history) {
    case OcclusionHistory::Valid:
        return "valid";
    case OcclusionHistory::Disabled:
        return "occlusion off";
    case OcclusionHistory::NoPyramid:
        return "no pyramid";
    case OcclusionHistory::CameraCut:
        return "camera cut";
    case OcclusionHistory::SceneChanged:
        return "scene changed";
    case OcclusionHistory::CameraFrozen:
        return "camera frozen";
    }
    return "";
}

// one retired frame: CPU-side totals plus the GPU counters read back after its fence
struct MeshletCullStats
{
    uint32_t instances = 0;
    uint32_t drawRecords = 0;
    uint32_t totalMeshlets = 0;
    uint32_t meshletsPerSequence = 0;
    std::array<uint32_t, kMeshStateGroupCount> groupMeshlets{};
    OcclusionHistory history = OcclusionHistory::Valid;
    // frames recorded with an invalid history since startup (Disabled excluded)
    uint32_t fallbackFrames = 0;
    // indexed by CullPhase; the per-test fields are zero unless MeshletCullSettings::isStatsEnabled
    std::array<GpuCullCounters, kCullPhaseCount> phases{};
    float cpuPrepareMs = 0.0f;

    [[nodiscard]] const GpuCullCounters& phase(CullPhase cullPhase) const
    {
        return phases[std::to_underlying(cullPhase)];
    }
};

[[nodiscard]] constexpr uint32_t visibleMeshletCount(const GpuCullCounters& counters)
{
    uint32_t count = 0;
    for (const uint32_t groupCount : counters.visibleMeshlets) {
        count += groupCount;
    }
    return count;
}

// GPU culling in compute: node instances (frustum + hi-z) -> draw records -> meshlets (frustum + normal cone + hi-z)
// -> per state group visible lists -> DGC sequences. runs once per CullPhase: instances and meshlets occluded early
// become candidates the late phase re-tests against the fresh pyramid
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
    void prepareFrame(uint32_t frameSlot, const Camera& camera, const HiZPyramid& hiz);
    // outside the render pass, before the phase's DGC preprocess; heaps must be bound. early first: it resets the
    // counters of both phases
    void recordCull(const vk::raii::CommandBuffer& cmd, CullPhase phase) const;
    // after the late raster: makes the counters of both phases visible to readBackCounters
    static void recordCounterReadback(const vk::raii::CommandBuffer& cmd);

    [[nodiscard]] const MeshletCullStats& getStats() const noexcept { return stats; }

    MeshletCullSettings settings;

private:
    static constexpr uint32_t kNoGeneration = ~0u;
    using PhaseCullViews = std::array<GpuCullView, kCullPhaseCount>;

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
        // PhaseCullViews
        HostBuffer cullView;
        // GpuCullCounters per phase in host-visible VRAM: atomics stay on the device, the CPU reads them after the
        // fence
        HostBuffer counters;
        HostBuffer instanceItems;
        // uint per instance item: early-occluded rows for the late phase, sized with instanceCapacity
        DeviceBuffer instanceCandidates;
        HostBuffer drawRecordTemplates;
        uint32_t instanceCapacity = 0;
        // also the capacity of drawRecords
        uint32_t drawRecordCapacity = 0;
        // written by cullInstances, one region of drawRecordCapacity per phase
        DeviceBuffer drawRecords;
        // one region per phase
        DeviceBuffer visibleMeshlets;
        uint32_t visibleCapacity = 0;
        // visible-list entries occluded early; capacity is the scene's meshlet total
        DeviceBuffer meshletCandidates;
        uint32_t meshletCandidateCapacity = 0;
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
    void writeCullView(uint32_t frameSlot, const Camera& camera, const HiZPyramid& hiz, OcclusionHistory history,
                       uint32_t perSequence);
    [[nodiscard]] OcclusionHistory occlusionHistory(const Camera& camera, const HiZPyramid& hiz,
                                                    bool isLayoutChanged) const;
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
    uint32_t fallbackFrames = 0;
};
