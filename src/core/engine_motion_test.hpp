#pragma once

#include "core/object_storage.hpp"
#include "core/types.hpp"
#include "scene/vk_camera.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// scripted motion-vector checks (spec/features/motion_vectors.md §5) on the startup scene, read from the motion_check
// pass after every frame. ENGINE_MOTION_TEST=1 enables the run; the engine quits once the summary is logged
class EngineMotionTest
{
public:
    [[nodiscard]] static bool isRequested();

    // what only the engine can do for the test before this frame
    struct Request
    {
        std::optional<glm::vec3> loadModelAt;
        std::optional<glm::ivec2> windowSize;
    };

    // moves the camera and the test entity for this frame
    [[nodiscard]] Request beginFrame(Camera& camera, ObjectStorage& objects);
    // after the frame's fence; true once every scenario is evaluated and the summary is logged
    [[nodiscard]] bool endFrame(const GpuMotionCheckStats& stats);

private:
    enum class Scenario
    {
        StaticScene,
        CameraYaw,
        CameraYawSky,
        CameraPitchYaw,
        CameraTranslation,
        EntityTranslation,
        EntityRotation,
        MirroredCameraYaw,
        MirroredEntityRotation,
        LoadWhileMoving,
        Teleport,
        FovChange,
        Resize,
        Count,
    };

    // what a non-reset frame must show
    enum class Check
    {
        NoMotion,     // motion 0 everywhere, albedo identical
        CameraMotion, // every pixel matches the camera-only motion from depth, sky included
        EntityMotion, // reprojection removes most of the albedo change motion 0 leaves
    };

    struct ScenarioInfo
    {
        const char* name;
        Check check;
        bool hasEvent; // a cut or a load at kEventFrame: the first reset frame after it must have zero motion
        // camera pitch at the scenario start, in Camera::rotate units from the startup view; negative looks up
        float startPitch;
        // empty pixels in view, so their direction-only motion is tested: starts at kCourtyardCenter, which the
        // startup view under the Sponza arcade does not show
        bool needsSky;
    };

    struct Result
    {
        uint32_t frames = 0;
        uint32_t movingFrames = 0;
        uint32_t resetFramesAfterEvent = 0;
        float maxCameraError = 0.0f;
        uint32_t cameraOutliers = 0;
        float maxStillMotion = 0.0f;
        double emptyFractionSum = 0.0;
        double reprojectedFractionSum = 0.0;
        double stillFractionSum = 0.0;
        std::vector<std::string> failures;
    };

    static constexpr uint32_t kWarmupFrames = 8;
    static constexpr uint32_t kFramesPerScenario = 24;
    // the first frames of a scenario still carry the previous scenario's last motion
    static constexpr uint32_t kSettleFrames = 2;
    static constexpr uint32_t kEventFrame = 8;
    static constexpr uint32_t kTestEntity = 0;
    // open to the sky in the startup Sponza, which loads at the origin
    static constexpr glm::vec3 kCourtyardCenter{0.0f, 2.0f, 0.0f};
    // a still frame projects the point through two separately evaluated matrix chains: float noise of about 2e-5 px
    static constexpr float kMaxStillMotion = 1e-3f;
    // a moving frame has at least this much somewhere
    static constexpr float kMinMovingMotion = 0.1f;
    // the resolve's interpolated point lands slightly off the pixel ray on an isolated pixel now and then (up to 0.2 px
    // seen on Sponza); a real bug moves whole surfaces
    static constexpr double kMaxCameraOutlierFraction = 1e-5;
    static constexpr float kMaxCameraError = 0.5f;
    // correct motion must remove this much of the albedo change zero motion leaves
    static constexpr double kMaxReprojectedToStillRatio = 0.25;
    // below this the scene shows too little texture change to judge entity motion
    static constexpr double kMinStillFraction = 0.005;
    static constexpr double kMinEmptyFraction = 0.01;
    static constexpr std::array<ScenarioInfo, static_cast<size_t>(Scenario::Count)> kScenarios = {{
        {"static camera, static scene", Check::NoMotion, false, 0.0f, false},
        {"camera yaw", Check::CameraMotion, false, 0.0f, false},
        {"camera yaw, sky in view", Check::CameraMotion, false, -700.0f, true},
        {"camera pitch and yaw", Check::CameraMotion, false, 0.0f, false},
        {"camera translation", Check::CameraMotion, false, 0.0f, false},
        {"entity translation", Check::EntityMotion, false, 0.0f, false},
        {"entity rotation", Check::EntityMotion, false, 0.0f, false},
        {"mirrored entity, camera yaw", Check::CameraMotion, false, 0.0f, false},
        {"mirrored entity rotation", Check::EntityMotion, false, 0.0f, false},
        {"load a model while moving", Check::CameraMotion, true, 0.0f, false},
        {"teleport", Check::NoMotion, true, 0.0f, false},
        {"FOV change", Check::NoMotion, true, 0.0f, false},
        {"resize", Check::NoMotion, true, 0.0f, false},
    }};

    void evaluate(const GpuMotionCheckStats& stats);
    // adds the whole-scenario checks, logs PASS or FAIL
    void logScenario();
    // sets the entity's transform without motion: last frame's matrix becomes the new one
    static void place(ObjectStorage& objects, const Transform& transform);

    uint32_t frame = 0;
    size_t scenarioIndex = 0;
    uint32_t scenarioFrame = 0;
    uint32_t passedCount = 0;
    // sum of the pitch deltas given to Camera::rotate
    float pitch = 0.0f;
    std::optional<Transform> originalTransform;
    std::optional<glm::vec3> startupCameraPosition;
    Result result;
};
