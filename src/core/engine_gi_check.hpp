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
// and RTXDI's heuristics (considerations C-G5-1 to C-G5-3). Per indirect mode (spec/m4/g7_indirect_modes.md): NRC mode
// is measured against the same ground truth; ReSTIR GI mode is gated against a one-bounce ground truth (its own paths
// without reuse). ENGINE_GI_CHECK=1 enables the run; the engine quits after the summary
class EngineGiCheck
{
public:
    [[nodiscard]] static bool isRequested();

    struct Request
    {
        bool shouldCapture = false;
    };

    // rtxdi's GI fields, the indirect mode and the camera are the check's for the whole run
    [[nodiscard]] Request beginFrame(Camera& camera, RtxdiContext& rtxdi, IndirectMode& indirectMode,
                                     DebugViewSettings& debugView);
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
        // NRC mode: the cache-terminated samples without reuse
        NrcMode,
        // ReSTIR GI mode's one-bounce paths without reuse, replacing the multi-bounce ground truth
        OneBounceGroundTruth,
        // its estimator once more
        OneBounceNoiseFloor,
        // ReSTIR GI mode against it
        RestirGiMode,
        Count,
    };

    enum class Role : uint8_t
    {
        // averaged into the ground truth the later phases compare against
        GroundTruth,
        // the ground truth's estimator once more; its tile p90 scales the next phases' tile limit
        NoiseFloor,
        Compared,
    };

    struct PhaseInfo
    {
        const char* name;
        Role role;
        IndirectMode mode;
        // RtxdiContext::giReferenceBounces: > 0 traces the bounces after X2 and pauses NRC
        uint32_t referenceBounces;
        bool isGiReference;
        // RtxdiContext::defaultGiSettings instead of convergedGiSettings
        bool isNvidiaDefaults;
        // frames before the first capture: reservoir history (ReSTIR GI) and training after the pause (NRC)
        uint32_t warmupFrames;
        uint32_t frames;
        // shares of the ground truth's summed luminance; maxBias 0: logged, not gated. A compared phase's tile limit
        // is at least kNoiseFloorTileFactor times the noise floor's tile p90; 0 on a noise floor: bias gated alone
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
    // an unbiased estimate's tile p90 times this: G5's 8% over its 3.74% multi-bounce floor (2026-11-18)
    static constexpr double kNoiseFloorTileFactor = 2.0;
    // limits: spec/m4/g5_restir_gi.md §3; ReSTIR GI mode: spec/m4/g7_indirect_modes.md §3. The one-bounce ground truth
    // is noisier per tile (sun fireflies in the gallery's dim tiles): its own floor sets the tile limit
    static constexpr uint32_t kTraced = RtxdiContext::kGiGroundTruthBounces;
    static constexpr std::array<PhaseInfo, static_cast<size_t>(Phase::Count)> kPhases = {{
        {"ground truth", Role::GroundTruth, IndirectMode::NrcRestirGi, kTraced, true, false, 4, 2048, 0.0, 0.0},
        {"ground truth again (noise floor)", Role::NoiseFloor, IndirectMode::NrcRestirGi, kTraced, true, false, 4, 512,
         0.01, 0.06},
        {"ReSTIR GI (converged settings, traced paths)", Role::Compared, IndirectMode::NrcRestirGi, kTraced, false,
         false, 64, 512, 0.015, 0.08},
        // NRC retrains after the pause
        {"NRC + ReSTIR GI (converged settings)", Role::Compared, IndirectMode::NrcRestirGi, 0, false, false, 512, 512,
         0.0, 0.0},
        {"NRC + ReSTIR GI (NVIDIA defaults)", Role::Compared, IndirectMode::NrcRestirGi, 0, false, true, 64, 512, 0.0,
         0.0},
        {"NRC mode", Role::Compared, IndirectMode::Nrc, 0, false, false, 64, 512, 0.0, 0.0},
        {"one-bounce ground truth (ReSTIR GI mode, no reuse)", Role::GroundTruth, IndirectMode::RestirGi, 0, true,
         false, 4, 2048, 0.0, 0.0},
        {"one-bounce ground truth again (noise floor)", Role::NoiseFloor, IndirectMode::RestirGi, 0, true, false, 4,
         512, 0.01, 0.0},
        {"ReSTIR GI mode (converged settings) against one bounce", Role::Compared, IndirectMode::RestirGi, 0, false,
         false, 64, 512, 0.015, 0.08},
    }};
    static constexpr std::array<Pose, 2> kPoses = {{
        {"gallery (startup)", true, {}, {}},
        {"courtyard", false, {6.0f, 2.5f, 1.5f}, {-10.0f, 1.0f, -1.0f}},
    }};

    void applyPhase(RtxdiContext& rtxdi, IndirectMode& indirectMode) const;
    [[nodiscard]] bool isGated() const noexcept { return kPhases[static_cast<size_t>(phase)].maxBias > 0.0; }
    // logs the phase's metrics; true when within its limits. A noise floor keeps its tile p90
    [[nodiscard]] bool evaluatePhase();
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
    // tile p90 of the current ground truth's noise floor phase
    double noiseFloorTileP90 = 0.0;
    uint32_t checkCount = 0;
    uint32_t passedCount = 0;
};
