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
#include <vector>

// M8 I1 temporal runners on the startup Sponza (spec/fixesM1-6/i1_check_runners.md), in ReSTIR GI mode at a fixed
// frame clock, every step from a camera cut with its own seed, so a run repeats frame for frame. Every result is
// MEASURED; a run fails only on missing captures or non-finite pixels. One kind per run, the engine quits after it:
//   ENGINE_STABILITY_CHECK=1: Lit and Indirect radiance temporal noise of a static camera per GI settings set;
//   ENGINE_LIGHT_CHANGE_CHECK=1: frames until Indirect radiance settles after a sun move and a sun toggle;
//   ENGINE_SCREEN_EDGE_CHECK=1: frames until the band a pan or a backward dolly reveals converges.
// ENGINE_CHECK_SCENE=bistro_cafe runs them in Bistro's café, where stability also covers the DI sets, the NRC modes
// and Direct lighting against RTXDI's reference (bias and dark leak); ENGINE_CHECK_SCENE=bistro runs stability and
// screen edge on Bistro's glass poses per glass state, under a sun through the café windows (m8 I13).
// ENGINE_CHECK_STEPS=a|b keeps the stability
// steps whose label contains a or b; ENGINE_CHECK_PNG=1 writes each stability step's first frame, mean and noise map
class EngineTemporalCheck
{
public:
    enum class Kind : uint8_t
    {
        Stability,
        LightChange,
        ScreenEdge,
        // m8 I14 F27: Power_RIS DI against independent BRDF-only exact textured emission (areTriangleLightsSampled=false)
        EmissiveRef,
    };

    // the first requested kind in Kind's order; a second one is logged and ignored
    [[nodiscard]] static std::optional<Kind> requestedKind();

    explicit EngineTemporalCheck(Kind kind);

    struct Request
    {
        bool shouldCapture = false;
    };

    // the camera, the debug view, the sun, exposure and the GI fields of rtxdi are the runner's for the whole run
    [[nodiscard]] Request beginFrame(Camera& camera, Renderer& renderer);
    // after the frame's fence; true once the last step is logged
    [[nodiscard]] bool endFrame(const Camera& camera, const std::optional<DisplayCapture>& capture);
    [[nodiscard]] bool isPassed() const noexcept { return isFinished && failedCount == 0; }

private:
    enum class StepType : uint8_t
    {
        // static camera, temporal statistics per pixel
        Stability,
        // the new light state from a cut: its converged mean
        LightReference,
        // the old light state, then the new one at changeAt
        LightChange,
        // one Depth view frame: view depth and matrices for the screen-edge masks
        Depth,
        // the pose from a cut: the first changeAt captures average into its image, the rest read the static error
        // against it
        EdgeReference,
        // the motion's start pose, then the pose at changeAt without a cut
        EdgeMove,
        // m8 I14 F27: independent exact textured emission reference (areTriangleLightsSampled=false, 1-bounce BRDF)
        EmissiveReference,
        // m8 I14 F27: Power_RIS DI reference compared against EmissiveReference
        EmissiveCompare,
    };

    enum class SunState : uint8_t
    {
        Default,
        // azimuth + kSunMoveDegrees
        Moved,
        Off,
    };

    enum class Motion : uint8_t
    {
        Pan,
        Dolly,
    };

    // ReSTIR DI settings of a step
    enum class DiSet : uint8_t
    {
        Shipped,
        Nvidia,
        Converged,
        // RTXDI without reuse (RtxdiContext::isReference): Direct lighting's reference
        Reference,
        // the I6 variants and the GI and NRC steps over them are the lab's cafe plan since full_testing T12
    };

    // glass settings of a step (m8 I13, ENGINE_CHECK_SCENE=bistro); not in the step's seed, so the variants of one step
    // see the same noise and differ only by the glass
    enum class GlassState : uint8_t
    {
        Shipped,
        // no glass pass: glass is neither rasterized nor composited
        PassOff,
        // the shipped pass with the shadow transmittance (I13 §3), off by default
        ShadowTransmittance,
        // DLSS-RR albedo guides blended with the glass layers (I13 §4 variant b)
        BlendGuides,
    };

    struct Step
    {
        StepType type;
        uint8_t pose;
        GiSettingsSet set;
        DebugView view;
        // the state before changeAt; LightReference: its only state
        SunState sun;
        SunState sunAfter;
        Motion motion;
        // Depth: the pose itself, else the motion's start pose
        bool isFinalPose;
        // frames before the first capture
        uint32_t warmupFrames;
        uint32_t frames;
        // the captured frame that first shows the change; kNoChange: none
        uint32_t changeAt;
        DiSet di = DiSet::Shipped;
        IndirectMode mode = IndirectMode::RestirGi;
        GlassState glass = GlassState::Shipped;
    };

    struct PoseView
    {
        glm::vec3 position{0.0f};
        glm::vec3 target{0.0f, 0.0f, -1.0f};
    };

    struct DepthMap
    {
        vk::Extent2D extent{};
        // view depth per texel, render resolution
        std::vector<float> viewDepth;
        glm::mat4 view{1.0f};
        glm::mat4 invView{1.0f};
        glm::mat4 invProj{1.0f};
        glm::mat4 viewProj{1.0f};
    };

    enum class Region : uint8_t
    {
        Interior,
        // off screen in the initial view
        Band,
        // behind a closer surface in the initial view
        Disoccluded,
        Count,
    };

    struct RegionSums
    {
        double value = 0.0;
        double reference = 0.0;
        double absoluteError = 0.0;
    };
    using FrameRegions = std::array<RegionSums, static_cast<size_t>(Region::Count)>;

    static constexpr uint32_t kNoChange = UINT32_MAX;
    static constexpr uint32_t kStartupFrames = 8;
    static constexpr float kFrameDeltaMs = 1000.0f / 60.0f;
    // longer than any step
    static constexpr uint32_t kStepSeedStride = 16384;
    // settling is judged on non-overlapping blocks of this many frames, within max(kSettledError, kNoiseSigmas times
    // the block-mean noise of the static reference step)
    static constexpr uint32_t kBlockFrames = 8;
    static constexpr double kNoiseSigmas = 4.0;
    // stability: CV above this is a flickering pixel. Lit pixels: on the Lit view (tonemapped, linear display
    // values) a mean above kLitDisplayCutoff, about sRGB code 3, the darkest visible; on the linear radiance views a
    // mean above kLitShare of the frame's mean
    static constexpr double kFlickerCv = 0.1;
    static constexpr double kLitDisplayCutoff = 1e-3;
    static constexpr double kLitShare = 0.05;
    static constexpr double kSettledError = 0.02;
    // screen edge: a revealed region's pixel error at most this times its own in the static reference step
    static constexpr double kSettledPixelRatio = 1.25;
    static constexpr float kSunMoveDegrees = 30.0f;
    static constexpr float kPanDegrees = 10.0f;
    static constexpr float kDollyMeters = 1.0f;
    // a revealed region below this share of the frame is not reported
    static constexpr double kMinRegionShare = 0.01;
    // a point this much farther than the initial view's surface there was hidden behind it
    static constexpr float kDisocclusionScale = 1.02f;
    static constexpr float kDisocclusionOffsetMeters = 0.02f;
    // Direct lighting's reference: longer than a step, RTXDI's reference is one sample per light candidate
    static constexpr uint32_t kDirectReferenceFrames = 1024;
    // tiles of the Direct lighting comparison, as the guide capture's convergence check
    static constexpr uint32_t kDirectTileSize = 16;

    [[nodiscard]] static std::vector<Step> buildSteps(Kind kind, CheckScene scene);
    [[nodiscard]] static const char* diSetName(DiSet set);
    [[nodiscard]] static const char* glassStateName(GlassState state);
    // shipped: the startup settings
    [[nodiscard]] static GlassSettings glassSettingsFor(GlassState state, const GlassSettings& shipped);
    // shipped: the startup settings
    [[nodiscard]] static RtxdiContext::Settings diSettingsFor(DiSet set, const RtxdiContext::Settings& shipped);
    // frame index base of the step's RTXDI seeds
    [[nodiscard]] static uint32_t stepSeed(const Step& step);
    // the scene's base sun: the default, or through the café windows in the Bistro glass scene
    [[nodiscard]] WorldLights::Sun sunFor(SunState state) const;
    // where a motion starts; it ends at the pose, the view the other runners measure
    [[nodiscard]] static PoseView startPose(const PoseView& pose, Motion motion);
    [[nodiscard]] static const char* kindName(Kind kind);
    [[nodiscard]] std::string stepLabel(const Step& step) const;

    void startStep(Camera& camera, Renderer& renderer);
    void accumulate(const Camera& camera, const DisplayCapture& capture);
    void accumulateEdge(const std::vector<float>& luminance, vk::Extent2D extent);
    void buildMasks();
    void finishStep();
    void logStability(const Step& step);
    // Direct lighting against the pose's DI reference step
    void logDirectBias(const Step& step, const std::vector<double>& mean);
    void logLightChange(const Step& step);
    void logEdgeMove(const Step& step);
    void fail(const std::string& text);

    Kind kind;
    CheckScene scene = readCheckScene();
    std::vector<Step> steps;
    size_t stepIndex = 0;
    uint32_t stepFrame = 0;
    uint32_t frame = 0;
    bool isCaptureRequested = false;
    bool isFinished = false;
    uint32_t failedCount = 0;
    std::array<PoseView, 2> poses{};
    bool hasStartupPose = false;
    // the DI settings at the first step: DiSet::Shipped restores them (ENGINE_DI_SAMPLING included)
    std::optional<RtxdiContext::Settings> startupDiSettings;
    std::optional<GlassSettings> startupGlassSettings;
    // the last DI reference step's mean Direct lighting and its pose
    std::vector<double> directReference;
    uint8_t directReferencePose = UINT8_MAX;

    // the current step's captures
    vk::Extent2D extent{};
    uint32_t capturedFrames = 0;
    uint64_t nonFinitePixels = 0;
    bool isCaptureMissing = false;
    std::vector<double> sum;
    std::vector<double> squaredSum;
    std::vector<float> previous;
    double frameChange = 0.0;
    std::vector<double> frameMeans;
    std::vector<FrameRegions> edgeFrames;

    // light change: the last reference step's mean
    double lightReference = 0.0;
    double lightTolerance = 0.0;
    // screen edge: per motion, the initial and the final pose's depth, the masks, and the last reference image
    DepthMap initialDepth;
    DepthMap finalDepth;
    std::vector<Region> masks;
    vk::Extent2D referenceExtent{};
    std::vector<double> reference;
    // per region, from the reference step's frames after its image: the mean tolerance and the static pixel error
    std::array<double, static_cast<size_t>(Region::Count)> edgeTolerance{};
    std::array<double, static_cast<size_t>(Region::Count)> edgeSteadyError{};
    std::optional<float> pinnedExposure;
};
