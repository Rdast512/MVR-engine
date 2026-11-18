#pragma once

#include "render/debug_view.hpp"
#include "render/vk_renderer.hpp"
#include "scene/vk_camera.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

// ReSTIR GI convergence check on the startup Sponza (spec/m4/g5_restir_gi.md §3, engine spec §14). Per pose, on a
// static frame: the ground truth (RtxdiContext::kGiGroundTruthBounces traced after X2, NRC paused, no reuse) averaged
// over many frames; then the same estimator again (the check's own noise floor) and ReSTIR GI on the same paths with
// convergedGiSettings, each averaged and compared with it on the Indirect radiance view. The shipped pipeline (NRC,
// then ReSTIR GI) is measured and logged, not gated, with convergedGiSettings and with the NVIDIA defaults: NRC's bias
// and RTXDI's heuristics (considerations C-G5-1 to C-G5-3). ENGINE_GI_CHECK=1 enables the run; the engine quits after
// the summary
class EngineGiCheck
{
public:
    [[nodiscard]] static bool isRequested();

    struct Request
    {
        bool shouldCapture = false;
    };

    // rtxdi's GI fields and the camera are the check's for the whole run
    [[nodiscard]] Request beginFrame(Camera& camera, RtxdiContext& rtxdi, DebugViewSettings& debugView);
    // after the frame's fence; true once every pose is compared and the summary is logged
    [[nodiscard]] bool endFrame(const std::optional<Renderer::DisplayCapture>& capture);

private:
    enum class Phase : uint8_t
    {
        GroundTruth,
        // the ground truth's estimator once more: what the metrics read for an unbiased estimate of this length
        NoiseFloor,
        // ReSTIR GI against the ground truth's own samples: its bias alone
        Reuse,
        // NRC, then ReSTIR GI: NRC's bias on top
        Pipeline,
        // the same with the NVIDIA defaults: the bias of RTXDI's heuristics on top
        Defaults,
        Count,
    };

    struct PhaseInfo
    {
        const char* name;
        // frames before the first capture: reservoir history (ReSTIR GI) and training after the pause (NRC)
        uint32_t warmupFrames;
        uint32_t frames;
        // shares of the ground truth's summed luminance; 0: logged, not gated
        double maxBias;
        double maxTileP90;
    };

    struct Pose
    {
        const char* name;
        // the startup camera, else lookAt(position, target)
        bool isStartup;
        glm::vec3 position;
        glm::vec3 target;
    };

    static constexpr uint32_t kStartupFrames = 8;
    // 1-sample multi-bounce paths are far noisier than ReSTIR DI's lamp: 16 px tiles leave 8% p90 for an unbiased
    // estimate (2026-11-18)
    static constexpr uint32_t kTileSize = 32;
    // limits: spec/m4/g5_restir_gi.md §3
    static constexpr std::array<PhaseInfo, static_cast<size_t>(Phase::Count)> kPhases = {{
        {"ground truth", 4, 2048, 0.0, 0.0},
        {"ground truth again (noise floor)", 4, 512, 0.01, 0.06},
        {"ReSTIR GI (converged settings, traced paths)", 64, 512, 0.015, 0.08},
        // NRC retrains after the pause
        {"NRC + ReSTIR GI (converged settings)", 512, 512, 0.0, 0.0},
        {"NRC + ReSTIR GI (NVIDIA defaults)", 64, 512, 0.0, 0.0},
    }};
    static constexpr std::array<Pose, 2> kPoses = {{
        {"gallery (startup)", true, {}, {}},
        {"courtyard", false, {6.0f, 2.5f, 1.5f}, {-10.0f, 1.0f, -1.0f}},
    }};

    void applyPhase(RtxdiContext& rtxdi) const;
    [[nodiscard]] bool isGated() const noexcept { return kPhases[static_cast<size_t>(phase)].maxBias > 0.0; }
    // logs the phase's metrics; true when within its limits
    [[nodiscard]] bool evaluatePhase() const;
    // next phase or pose; true after the last one
    bool finishPhase();

    uint32_t frame = 0;
    size_t poseIndex = 0;
    Phase phase = Phase::GroundTruth;
    uint32_t phaseFrame = 0;
    uint32_t capturedFrames = 0;
    bool isCaptureRequested = false;
    bool isCaptureMissing = false;
    bool isFinished = false;
    vk::Extent2D extent{};
    // per pixel over the phase's captures: luminance, and its squared error against the ground truth (or, for the
    // ground truth, squared luminance)
    std::vector<double> sum;
    std::vector<double> squaredSum;
    std::vector<double> groundTruth;
    double groundTruthNoise = 0.0;
    uint32_t checkCount = 0;
    uint32_t passedCount = 0;
};
