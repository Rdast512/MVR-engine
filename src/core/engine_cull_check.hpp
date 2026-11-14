#pragma once

#include "render/debug_view.hpp"
#include "render/vk_meshlet_culling.hpp"
#include "render/vk_renderer.hpp"
#include "scene/vk_camera.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

// culling image check on the startup Sponza (engine spec §16.2 M0/M1): every pose is captured with every cull test
// on, the first capture right after a camera turn so phase 1 tests against a stale pyramid, then with every test off.
// Lossless culling leaves both images bit-identical. ENGINE_CULL_CHECK=1 enables the run; the engine quits after the
// summary
class EngineCullCheck
{
public:
    [[nodiscard]] static bool isRequested();

    struct Request
    {
        bool shouldCapture = false;
    };

    [[nodiscard]] Request beginFrame(Camera& camera, MeshletCullSettings& culling, DebugViewSettings& debugView);
    // after the frame's fence; true once every pose is compared and the summary is logged
    [[nodiscard]] bool endFrame(const std::optional<Renderer::DisplayCapture>& capture);

private:
    struct Pose
    {
        const char* name;
        // the startup position, or kCourtyardCenter
        bool isCourtyard;
        // Camera::rotate units from the startup orientation
        float yaw;
        float pitch;
    };

    // frame of a pose; the camera turns during the approach, so the first culled capture has stale occlusion history
    enum class Step : uint32_t
    {
        // teleport cut, then kApproachFrames - 1 turning frames ending on the pose
        Approach,
        CulledAlbedo = Approach + 1,
        CulledDepth,
        // until the unculled captures are a whole number of jitter cycles after the culled ones
        UnculledSettle,
        UnculledAlbedo,
        UnculledDepth,
    };

    static constexpr uint32_t kApproachFrames = 8;
    // culled capture to unculled capture, before rounding up to the jitter cycle: one settle frame
    static constexpr uint32_t kMinCaptureDistance = 3;
    static constexpr uint32_t kWarmupFrames = 8;
    // 45 degrees and the approach turn, in Camera::rotate units
    static constexpr float kEighthTurn = 392.699f;
    static constexpr float kApproachStep = 25.0f;
    // open to the sky in the startup Sponza (as EngineMotionTest)
    static constexpr glm::vec3 kCourtyardCenter{0.0f, 2.0f, 0.0f};
    static constexpr std::array<Pose, 16> kPoses = {{
        {"arcade 0", false, 0.0f * kEighthTurn, 0.0f},
        {"arcade 45", false, 1.0f * kEighthTurn, 0.0f},
        {"arcade 90", false, 2.0f * kEighthTurn, 0.0f},
        {"arcade 135", false, 3.0f * kEighthTurn, 0.0f},
        {"arcade 180", false, 4.0f * kEighthTurn, 0.0f},
        {"arcade 225", false, 5.0f * kEighthTurn, 0.0f},
        {"arcade 270", false, 6.0f * kEighthTurn, 0.0f},
        {"arcade 315", false, 7.0f * kEighthTurn, 0.0f},
        {"courtyard 0 up", true, 0.0f * kEighthTurn, -300.0f},
        {"courtyard 90 up", true, 2.0f * kEighthTurn, -300.0f},
        {"courtyard 180 up", true, 4.0f * kEighthTurn, -300.0f},
        {"courtyard 270 up", true, 6.0f * kEighthTurn, -300.0f},
        {"courtyard 45 down", true, 1.0f * kEighthTurn, 300.0f},
        {"courtyard 135 down", true, 3.0f * kEighthTurn, 300.0f},
        {"courtyard 225 down", true, 5.0f * kEighthTurn, 300.0f},
        {"courtyard 315 down", true, 7.0f * kEighthTurn, 300.0f},
    }};

    [[nodiscard]] Step getStep() const;
    // pixels whose rgb differs between the two captures
    [[nodiscard]] static uint64_t countDifferences(const std::vector<uint16_t>& culled,
                                                   const Renderer::DisplayCapture& unculled);

    uint32_t frame = 0;
    size_t poseIndex = 0;
    uint32_t poseFrame = 0;
    // frames from a culled capture to its unculled one; a multiple of the jitter phase count
    uint32_t captureDistance = kMinCaptureDistance;
    uint32_t passedCount = 0;
    // sums of the deltas given to Camera::rotate
    float yaw = 0.0f;
    float pitch = 0.0f;
    std::optional<glm::vec3> startupPosition;
    std::vector<uint16_t> culledAlbedo;
    std::vector<uint16_t> culledDepth;
    uint64_t albedoDifferences = 0;
    bool isCaptureMissing = false;
};
