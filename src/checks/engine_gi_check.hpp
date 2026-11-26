#pragma once

#include "checks/check_passes.hpp"
#include "check_poses.hpp"
#include "gi_settings_sets.hpp"
#include "render/debug_view.hpp"
#include "render/vk_renderer.hpp"
#include "scene/vk_camera.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <tuple>
#include <vector>

// ReSTIR GI convergence check on the startup Sponza (spec/m4/g5_restir_gi.md §3, engine spec §14). Per pose, on a
// static frame: the ground truth (RtxdiContext::kGiGroundTruthBounces traced after X2, NRC paused, no reuse) averaged
// over many frames; then the same estimator again (the check's own noise floor) and ReSTIR GI on the same paths with
// convergedGiSettings, each averaged and compared with it on the Indirect radiance view. Both NRC phases (NRC + ReSTIR
// GI with convergedGiSettings, and NRC mode without reuse) are gated against the same multi-bounce ground truth (M8 I7,
// after fixing C-G5-3); the NVIDIA-defaults phase stays measured for RTXDI's heuristic bias (C-G5-1). Per indirect mode
// (spec/m4/g7_indirect_modes.md): ReSTIR GI mode is gated against a one-bounce ground truth (its own paths without
// reuse) with both converged and shipped settings, and measured with NVIDIA's settings. ENGINE_GI_CHECK=1 enables the
// run; the engine quits after the summary. ENGINE_GI_CHECK_SEED picks another realization of every phase
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
    [[nodiscard]] bool endFrame(const std::optional<DisplayCapture>& capture);
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
        // the shipped settings without ReSTIR PT's DLSS-RR profile, the set shipped before it (O11)
        RestirGiModeNoRrProfile,
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
        GiSettingsSet settings;
        // frames before the first capture: reservoir history (ReSTIR GI) and training after the pause (NRC)
        uint32_t warmupFrames;
        uint32_t frames;
        // shares of the ground truth's summed luminance, per pose (kSponzaCheckPoses order) and set from the five-seed spread
        // (spec/fixesM1-6/i1_check_runners.md §5); maxBias 0: logged, not gated. A compared phase's tile limit is at
        // least kNoiseFloorTileFactor times the noise floor's tile p90; 0 on a noise floor: bias gated alone
        std::array<double, 2> maxBias;
        double maxTileP90;
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
        {"ground truth", Role::GroundTruth, IndirectMode::NrcRestirGi, kTraced, true, GiSettingsSet::Converged, 4,
         2048, {0.0, 0.0}, 0.0},
        // gallery: a firefly in either 2048- or 512-frame multi-bounce estimate moves the pair by up to 2% (I1 seeds)
        {"ground truth again (noise floor)", Role::NoiseFloor, IndirectMode::NrcRestirGi, kTraced, true,
         GiSettingsSet::Converged, 4, 512, {0.03, 0.01}, 0.06},
        {"ReSTIR GI (converged settings, traced paths)", Role::Compared, IndirectMode::NrcRestirGi, kTraced, false,
         GiSettingsSet::Converged, 64, 512, {0.02, 0.015}, 0.08},
        // NRC retrains after the pause; gated from five-seed spread after M8 I7's sentinel zero-continuation fix
        // (gallery -3.93..-1.74% / p90 11.6..17.9%, courtyard +0.74..+1.07% / p90 7.6..12.5%; NRC non-deterministic C-T-5)
        {"NRC + ReSTIR GI (converged settings)", Role::Compared, IndirectMode::NrcRestirGi, 0, false,
         GiSettingsSet::Converged, 512, 512, {0.05, 0.02}, 0.22},
        {"NRC + ReSTIR GI (NVIDIA defaults)", Role::Compared, IndirectMode::NrcRestirGi, 0, false,
         GiSettingsSet::Nvidia, 64, 512, {0.0, 0.0}, 0.0},
        // five-seed spread: gallery -3.63..-0.98% / p90 6.1..13.1%, courtyard -0.44..-0.06% / p90 3.3..9.0%
        {"NRC mode", Role::Compared, IndirectMode::Nrc, 0, false, GiSettingsSet::Converged, 64, 512, {0.05, 0.015},
         0.18},
        {"one-bounce ground truth (ReSTIR GI mode, no reuse)", Role::GroundTruth, IndirectMode::RestirGi, 0, true,
         GiSettingsSet::Converged, 4, 2048, {0.0, 0.0}, 0.0},
        {"one-bounce ground truth again (noise floor)", Role::NoiseFloor, IndirectMode::RestirGi, 0, true,
         GiSettingsSet::Converged, 4, 512, {0.01, 0.01}, 0.0},
        {"ReSTIR GI mode (converged settings) against one bounce", Role::Compared, IndirectMode::RestirGi, 0, false,
         GiSettingsSet::Converged, 64, 512, {0.015, 0.015}, 0.08},
        // what ships, and what it replaced (spec/fixesM1-6/m8_optimization_and_fixes.md O1, I5): gated from five-seed
        // spread (+0.67..+1.09% gallery / -1.36..-0.90% courtyard, tile p90 22.0..28.8% vs NVIDIA's 52..70%)
        {"ReSTIR GI mode (shipped defaults) against one bounce", Role::Compared, IndirectMode::RestirGi, 0, false,
         GiSettingsSet::Shipped, 64, 512, {0.02, 0.02}, 0.35},
        {"ReSTIR GI mode (NVIDIA defaults) against one bounce", Role::Compared, IndirectMode::RestirGi, 0, false,
         GiSettingsSet::Nvidia, 64, 512, {0.0, 0.0}, 0.0},
        {"ReSTIR GI mode (shipped + secondary DI resampling) against one bounce", Role::Compared,
         IndirectMode::RestirGi, 0, false, GiSettingsSet::ShippedSecondary, 64, 512, {0.0, 0.0}, 0.0},
        {"ReSTIR GI mode (shipped + firefly clamp) against one bounce", Role::Compared, IndirectMode::RestirGi, 0,
         false, GiSettingsSet::ShippedClamp, 64, 512, {0.0, 0.0}, 0.0},
        {"ReSTIR GI mode (shipped + both) against one bounce", Role::Compared, IndirectMode::RestirGi, 0, false,
         GiSettingsSet::ShippedSecondaryClamp, 64, 512, {0.0, 0.0}, 0.0},
        {"ReSTIR GI mode (shipped without secondary roughness floor) against one bounce", Role::Compared,
         IndirectMode::RestirGi, 0, false, GiSettingsSet::ShippedNoFloor, 64, 512, {0.0, 0.0}, 0.0},
        {"ReSTIR GI mode (shipped without DLSS-RR profile) against one bounce", Role::Compared,
         IndirectMode::RestirGi, 0, false, GiSettingsSet::ShippedNoRrProfile, 64, 512, {0.0, 0.0}, 0.0},
    }};
    static_assert(kSponzaCheckPoses.size() == std::tuple_size_v<decltype(PhaseInfo::maxBias)>);
    static_assert(kBistroCafeCheckPoses.size() == kSponzaCheckPoses.size());
    // ENGINE_GI_CHECK_SEED=n: a whole run's realization, for the gates' spread across seeds (M8 I1)
    static constexpr uint32_t kRunSeedStride = kPoseSeedStride * static_cast<uint32_t>(kSponzaCheckPoses.size());

    [[nodiscard]] static uint32_t readRunSeed();
    void applyPhase(RtxdiContext& rtxdi, IndirectMode& indirectMode) const;
    // the limits are Sponza's; another check scene is measured, not gated
    [[nodiscard]] double maxBias() const noexcept
    {
        return scene == CheckScene::Sponza ? kPhases[static_cast<size_t>(phase)].maxBias[poseIndex] : 0.0;
    }
    [[nodiscard]] const std::array<CheckPose, 2>& poses() const noexcept { return checkPoses(scene); }
    [[nodiscard]] bool isGated() const noexcept { return maxBias() > 0.0; }
    // logs the phase's metrics; true when within its limits. A noise floor keeps its tile p90
    [[nodiscard]] bool evaluatePhase();
    // next phase or pose; true after the last one
    bool finishPhase();

    CheckScene scene = readCheckScene();
    uint32_t runSeedBase = readRunSeed() * kRunSeedStride;
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
    // ENGINE_CHECK_PNG: the ground truth's scale, shared by the pose's phase images
    double pngScale = 1.0;
    std::array<double, 3> groundTruthRgb{0.0, 0.0, 0.0};
    uint64_t groundTruthNonFinite = 0;
    uint64_t groundTruthNegative = 0;
    double groundTruthNoise = 0.0;
    // tile p90 of the current ground truth's noise floor phase
    double noiseFloorTileP90 = 0.0;
    uint32_t checkCount = 0;
    uint32_t passedCount = 0;
};
