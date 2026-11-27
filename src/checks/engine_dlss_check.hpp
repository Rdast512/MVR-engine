#pragma once

#include "checks/check_passes.hpp"
#include "check_poses.hpp"
#include "check_clock.hpp"
#include "render/vk_renderer.hpp"
#include "scene/vk_camera.hpp"

#include <cstdint>
#include <optional>
#include <vector>

// M8 I14 Phase 2 (F25): automated DLSS-RR input & history-trust check (ENGINE_DLSS_CHECK=1).
// Feeds noise-free analytical radiance (FrameFlag::CleanDlssInput) into DLSS-RR on static and slowly panning poses
// (Sponza or ENGINE_CHECK_SCENE=bistro_cafe):
//   1) clean-input static baseline gated on temporal std < 0.5% and flicker < 1.0%;
//   2) perturbation controls toggling jitter sign, normal sign, roughness packing, depth polarity and proj[1][1] Y-flip.
class EngineDlssCheck
{
public:
    [[nodiscard]] static bool isRequested();

    EngineDlssCheck();

    struct Request
    {
        bool shouldCapture = false;
    };

    [[nodiscard]] Request beginFrame(Camera& camera, Renderer& renderer);
    [[nodiscard]] bool endFrame(const std::optional<DisplayCapture>& capture);
    [[nodiscard]] bool isPassed() const noexcept { return isFinished && failedCount == 0; }

private:
    enum class Variant : uint8_t
    {
        Shipped,
        JitterInverted,
        NormalInverted,
        RoughnessSquared,
        DepthNonInverted,
        ProjYFlipped,
    };

    struct Step
    {
        Variant variant;
        bool isPanning;
        uint32_t warmupFrames;
        uint32_t frames;
    };

    static constexpr uint32_t kStartupFrames = 8;
    // ENGINE_CHECK_FPS, read once
    const float frameDeltaMs = checkFrameDeltaMs();
    static constexpr float kPanYawDeltaPerFrame = 0.25f;
    static constexpr double kFlickerCv = 0.1;
    static constexpr double kLitDisplayCutoff = 1e-3;
    static constexpr double kMaxStaticCleanStd = 0.005;
    static constexpr double kMaxStaticCleanFlicker = 0.01;

    [[nodiscard]] static const char* variantName(Variant variant);
    [[nodiscard]] static std::vector<Step> buildSteps();

    void startStep(Camera& camera, Renderer& renderer);
    void accumulate(const DisplayCapture& capture);
    void finishStep();

    CheckScene scene = readCheckScene();
    std::vector<Step> steps;
    size_t stepIndex = 0;
    uint32_t stepFrame = 0;
    uint32_t frame = 0;
    bool isCaptureRequested = false;
    bool isFinished = false;
    uint32_t failedCount = 0;
    bool hasStartupPose = false;
    glm::vec3 startupPos{0.0f};
    glm::vec3 startupTarget{0.0f, 0.0f, -1.0f};

    vk::Extent2D extent{};
    uint32_t capturedFrames = 0;
    uint64_t nonFinitePixels = 0;
    bool isCaptureMissing = false;
    std::vector<double> sum;
    std::vector<double> squaredSum;
    std::vector<float> previous;
    double frameChange = 0.0;

    double shippedStaticStd = 0.0;
    double shippedPanF2f = 0.0;
};
