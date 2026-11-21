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
// without reuse), and measured there with the shipped and the NVIDIA settings. ENGINE_GI_CHECK=1 enables the run; the engine quits after the summary
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
    [[nodiscard]] bool isPassed() const noexcept { return isFinished && checkCount > 0 && passedCount == checkCount; }

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
        // the same with the shipped and the NVIDIA settings
        RestirGiModeDefaults,
        RestirGiModeNvidia,
        // the shipped settings with the RTXDI sample's X2 options (spec/features/m8_optimization_and_fixes.md O2, O6)
        RestirGiModeSecondary,
        RestirGiModeClamp,
        RestirGiModeSecondaryClamp,
        // the shipped settings without the secondary roughness floor (O9)
        RestirGiModeNoFloor,
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

    enum class GiSettingsSource : uint8_t
    {
        // RtxdiContext::convergedGiSettings
        Converged,
        // RtxdiContext::defaultGiSettings, shipped
        Shipped,
        // RtxdiContext::nvidiaGiSettings
        Nvidia,
        // Shipped with secondary DI resampling, the firefly clamp, or both
        ShippedSecondary,
        ShippedClamp,
        ShippedSecondaryClamp,
        ShippedNoFloor,
    };

    struct PhaseInfo
    {
        const char* name;
        Role role;
        IndirectMode mode;
        // RtxdiContext::giReferenceBounces: > 0 traces the bounces after X2 and pauses NRC
        uint32_t referenceBounces;
        bool isGiReference;
        GiSettingsSource settings;
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
    // deterministic RNG stride per phase (even, larger than any phase length) so adding/reordering phases never shifts seeds
    static constexpr uint32_t kPhaseSeedStride = 16384;
    static constexpr uint32_t kPoseSeedStride = 262144;
    // 1-sample multi-bounce paths are far noisier than ReSTIR DI's lamp: 16 px tiles leave 8% p90 for an unbiased
    // estimate (2026-11-18)
    static constexpr uint32_t kTileSize = 32;
    // an unbiased estimate's tile p90 times this: covers 1-bounce courtyard p90 (~2.1x floor, Jacobian clamp) while staying well below biased defaults (~10x)
    static constexpr double kNoiseFloorTileFactor = 2.5;
    // dimmer RGB channels have slightly higher relative Monte Carlo variance than luminance
    static constexpr double kRgbBiasFactor = 1.5;
    // limits: spec/m4/g5_restir_gi.md §3; ReSTIR GI mode: spec/m4/g7_indirect_modes.md §3. The one-bounce ground truth
    // is noisier per tile (sun fireflies in the gallery's dim tiles): its own floor sets the tile limit
    static constexpr uint32_t kTraced = RtxdiContext::kGiGroundTruthBounces;
    static constexpr std::array<PhaseInfo, static_cast<size_t>(Phase::Count)> kPhases = {{
        {"ground truth", Role::GroundTruth, IndirectMode::NrcRestirGi, kTraced, true, GiSettingsSource::Converged, 4,
         2048, 0.0, 0.0},
        {"ground truth again (noise floor)", Role::NoiseFloor, IndirectMode::NrcRestirGi, kTraced, true,
         GiSettingsSource::Converged, 4, 512, 0.01, 0.06},
        {"ReSTIR GI (converged settings, traced paths)", Role::Compared, IndirectMode::NrcRestirGi, kTraced, false,
         GiSettingsSource::Converged, 64, 512, 0.015, 0.08},
        // NRC retrains after the pause
        {"NRC + ReSTIR GI (converged settings)", Role::Compared, IndirectMode::NrcRestirGi, 0, false,
         GiSettingsSource::Converged, 512, 512, 0.0, 0.0},
        {"NRC + ReSTIR GI (NVIDIA defaults)", Role::Compared, IndirectMode::NrcRestirGi, 0, false,
         GiSettingsSource::Nvidia, 64, 512, 0.0, 0.0},
        {"NRC mode", Role::Compared, IndirectMode::Nrc, 0, false, GiSettingsSource::Converged, 64, 512, 0.0, 0.0},
        {"one-bounce ground truth (ReSTIR GI mode, no reuse)", Role::GroundTruth, IndirectMode::RestirGi, 0, true,
         GiSettingsSource::Converged, 4, 2048, 0.0, 0.0},
        {"one-bounce ground truth again (noise floor)", Role::NoiseFloor, IndirectMode::RestirGi, 0, true,
         GiSettingsSource::Converged, 4, 512, 0.01, 0.0},
        {"ReSTIR GI mode (converged settings) against one bounce", Role::Compared, IndirectMode::RestirGi, 0, false,
         GiSettingsSource::Converged, 64, 512, 0.015, 0.08},
        // what ships, and what it replaced (spec/features/m8_optimization_and_fixes.md O1)
        {"ReSTIR GI mode (shipped defaults) against one bounce", Role::Compared, IndirectMode::RestirGi, 0, false,
         GiSettingsSource::Shipped, 64, 512, 0.0, 0.0},
        {"ReSTIR GI mode (NVIDIA defaults) against one bounce", Role::Compared, IndirectMode::RestirGi, 0, false,
         GiSettingsSource::Nvidia, 64, 512, 0.0, 0.0},
        {"ReSTIR GI mode (shipped + secondary DI resampling) against one bounce", Role::Compared,
         IndirectMode::RestirGi, 0, false, GiSettingsSource::ShippedSecondary, 64, 512, 0.0, 0.0},
        {"ReSTIR GI mode (shipped + firefly clamp) against one bounce", Role::Compared, IndirectMode::RestirGi, 0,
         false, GiSettingsSource::ShippedClamp, 64, 512, 0.0, 0.0},
        {"ReSTIR GI mode (shipped + both) against one bounce", Role::Compared, IndirectMode::RestirGi, 0, false,
         GiSettingsSource::ShippedSecondaryClamp, 64, 512, 0.0, 0.0},
        {"ReSTIR GI mode (shipped without secondary roughness floor) against one bounce", Role::Compared,
         IndirectMode::RestirGi, 0, false, GiSettingsSource::ShippedNoFloor, 64, 512, 0.0, 0.0},
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
    std::array<double, 3> rgbSum{0.0, 0.0, 0.0};
    uint64_t nonFinitePixels = 0;
    uint64_t negativePixels = 0;
    std::vector<double> groundTruth;
    std::array<double, 3> groundTruthRgb{0.0, 0.0, 0.0};
    uint64_t groundTruthNonFinite = 0;
    uint64_t groundTruthNegative = 0;
    double groundTruthNoise = 0.0;
    // tile p90 of the current ground truth's noise floor phase
    double noiseFloorTileP90 = 0.0;
    uint32_t checkCount = 0;
    uint32_t passedCount = 0;
};
