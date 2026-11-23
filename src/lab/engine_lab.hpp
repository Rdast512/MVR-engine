#pragma once

#include "lab_cases.hpp"
#include "lab_metrics.hpp"
#include "lab_plan.hpp"
#include "render/vk_renderer.hpp"
#include "scene/vk_camera.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// the settings lab (spec/features/planned/full_testing.md): ENGINE_LAB=<plan> runs a plan's configs on its cases and quits.
// It tries settings; the engine checks measure the shipped state and never read the lab (§0). Every run starts from a
// camera cut at a fixed frame clock, seeded from a hash of its case, kind, view, config and seed, so a run's numbers do
// not depend on what ran before it. Results: [Lab] MEASURED lines in logs/engine.log, logs/lab/<plan>.csv, a ranking per
// case and metric in noise-floor units, and logs/lab/<plan>_cost.ps1 for the bench-mode cost runs.
// ENGINE_LAB_CASES=a,b narrows the plan's cases; ENGINE_LAB_PNG=1 writes each run's frame, mean and error to logs/lab
class EngineLab
{
public:
    // ENGINE_LAB's plan name; nullopt when unset
    [[nodiscard]] static std::optional<std::string> requestedPlan();

    // throws std::runtime_error when the plan does not parse: a typo must not silently test the base
    explicit EngineLab(std::string_view planName);

    struct Request
    {
        // replaces every entity with this model at the origin
        std::optional<std::filesystem::path> model;
        bool shouldCapture = false;
    };

    // modelBounds: entity 0's model-space bounding sphere. The camera, the debug view, the sun, exposure and every
    // lighting setting are the lab's for the whole run
    [[nodiscard]] Request beginFrame(Camera& camera, Renderer& renderer, std::optional<glm::vec4> modelBounds);
    // after the frame's fence; true once the plan's results are written
    [[nodiscard]] bool endFrame(const Camera& camera, const std::optional<Renderer::DisplayCapture>& capture);
    // a run fails only on missing captures or non-finite pixels; every result is MEASURED
    [[nodiscard]] bool isPassed() const noexcept { return isFinished && failedRuns == 0; }

private:
    enum class Variant : uint8_t
    {
        Config,
        // the reference's own estimator against the reference: the bias kinds' noise floor
        ReferenceAgain,
    };

    struct Metric
    {
        const LabMetricInfo* info;
        double value;
    };

    struct Run
    {
        size_t caseIndex;
        LabKind kind;
        DebugView view;
        LabConfig config;
        std::string configText;
        uint32_t seedIndex;
        Variant variant;
        std::vector<Metric> metrics{};
        bool isScheduled = false;
        bool isDone = false;
        bool isFailed = false;
        [[nodiscard]] std::optional<double> metric(const LabMetricInfo* info) const;
    };

    // case, kind, view
    using Slot = std::tuple<size_t, LabKind, DebugView>;

    struct Comparison
    {
        std::string label;
        size_t baseline;
        size_t run;
    };

    // two directions of one switch's flip, for the interaction flag (spec §6.3)
    struct OfatRecord
    {
        std::string switchName;
        size_t forward;
        size_t base;
        size_t backward;
        size_t target;
    };

    struct PairRecord
    {
        std::string label;
        size_t base;
        size_t first;
        size_t second;
        size_t both;
    };

    struct LeverRecord
    {
        std::string group;
        std::string label;
        // the fix (or the sweep's start) and the lever over it
        size_t reference;
        size_t run;
        // the lever on the shipped base, and the base
        std::optional<size_t> base;
        std::optional<size_t> onBase;
    };

    struct Bisection
    {
        LabMetricKey key;
        size_t caseIndex;
        size_t baseRun;
        size_t targetRun;
        std::vector<LabSwitchValue> differing{};
        std::vector<size_t> current{};
        size_t granularity = 2;
        std::vector<std::vector<size_t>> candidates{};
        size_t chunkCount = 0;
        size_t candidateIndex = 0;
        double targetEffect = 0.0;
        double setEffect = 0.0;
        size_t testedRuns = 0;
        bool isStarted = false;
        bool isDone = false;
        std::string outcome{};
    };

    struct StackRequest
    {
        LabMetricKey key;
        LabScope scope;
        bool isScheduled = false;
    };

    enum class PhaseType : uint8_t
    {
        Stability,
        DiReference,
        DiCompare,
        GiTruth,
        GiCompare,
        LightReference,
        LightChange,
        Depth,
        EdgeReference,
        EdgeMove,
    };

    enum class SunState : uint8_t
    {
        Case,
        // azimuth + kSunMoveDegrees
        Moved,
        Off,
    };

    enum class PoseState : uint8_t
    {
        Case,
        PanStart,
        DollyStart,
    };

    struct Phase
    {
        PhaseType type;
        DebugView view;
        LabConfig config{};
        bool isGiReference = false;
        uint32_t giReferenceBounces = 0;
        uint32_t warmup = 0;
        uint32_t frames = 0;
        uint32_t changeAt = kNoChange;
        SunState sun = SunState::Case;
        SunState sunAfter = SunState::Case;
        PoseState pose = PoseState::Case;
        uint32_t seed = 0;
        // DiReference / GiTruth: where the mean goes; DiCompare / GiCompare: what it is compared with
        std::string referenceKey{};
        // GiCompare: the 8-bounce truth
        std::string secondReferenceKey{};
        // metric prefix: GiCompare "1b." / "8b." ("" compares with both truths), light change, screen edge motion
        std::string prefix{};
    };

    struct Reference
    {
        vk::Extent2D extent{};
        std::vector<double> mean;
        std::array<double, 3> rgb{0.0, 0.0, 0.0};
        double luminanceTotal = 0.0;
    };

    struct PoseView
    {
        glm::vec3 position{0.0f};
        glm::vec3 target{0.0f, 0.0f, -1.0f};
    };

    static constexpr uint32_t kNoChange = UINT32_MAX;
    static constexpr uint32_t kStartupFrames = 8;
    // after a model load: the load frame's uploads, then light and acceleration-structure rebuilds
    static constexpr uint32_t kModelSettleFrames = 16;
    static constexpr float kFrameDeltaMs = 1000.0f / 60.0f;
    // I1's step lengths (engine_temporal_check.hpp), the lab's own copy (spec §0)
    static constexpr uint32_t kStabilityWarmup = 300;
    static constexpr uint32_t kStabilityFrames = 480;
    static constexpr uint32_t kDiReferenceFrames = 1024;
    static constexpr uint32_t kDiTileSize = 16;
    // the GI check's (engine_gi_check.hpp)
    static constexpr uint32_t kGiTruthWarmup = 4;
    static constexpr uint32_t kGiTruthFrames = 2048;
    static constexpr uint32_t kGiWarmup = 64;
    // NRC retrains after its pause
    static constexpr uint32_t kGiNrcWarmup = 512;
    static constexpr uint32_t kGiFrames = 512;
    static constexpr uint32_t kGiTileSize = 32;
    static constexpr uint32_t kLightReferenceWarmup = 256;
    static constexpr uint32_t kLightReferenceFrames = 256;
    static constexpr uint32_t kLightChangeWarmup = 448;
    static constexpr uint32_t kLightChangeAt = 64;
    static constexpr uint32_t kLightChangeFrames = 64 + 512;
    static constexpr uint32_t kEdgeWarmup = 256;
    static constexpr uint32_t kEdgeFrames = 256;
    static constexpr uint32_t kEdgeReferenceAt = 128;
    static constexpr float kSunMoveDegrees = 30.0f;
    static constexpr float kPanDegrees = 10.0f;
    static constexpr float kDollyMeters = 1.0f;
    static constexpr float kFramingMargin = 1.05f;
    // effects below this many noise floors are "no effect" (spec §2)
    static constexpr double kEffectFloors = 2.0;
    // bisection: a subset reproduces the target when it gives this share of the target's effect (spec §6.4)
    static constexpr double kReproduceShare = 0.8;
    static constexpr uint32_t kBenchFrames = 2000;

    [[nodiscard]] static uint32_t seedOf(std::string_view key);
    [[nodiscard]] static std::string viewName(DebugView view);

    // plan expansion
    void expandPlan();
    void expandEntry(const LabEntry& entry);
    [[nodiscard]] size_t ensureRun(size_t caseIndex, LabKind kind, DebugView view, const LabConfig& config,
                                   uint32_t seedIndex = 0, Variant variant = Variant::Config);
    void ensureFloorRuns(const Slot& slot);
    void schedule(size_t run);
    void addCostRequest(size_t caseIndex, const LabConfig& config);
    [[nodiscard]] bool isCaseSelected(size_t caseIndex) const;
    [[nodiscard]] std::vector<Slot> slotsOf(const LabScope& scope) const;

    // adaptive work: bisections, then stacks
    void scheduleAdaptive();
    void advanceBisection(Bisection& bisection);
    void scheduleStack(StackRequest& stack);

    // execution
    [[nodiscard]] bool startNextRun();
    void buildPhases(const Run& run);
    void startPhase(Camera& camera, Renderer& renderer, std::optional<glm::vec4> modelBounds);
    [[nodiscard]] WorldLights::Sun sunFor(SunState state) const;
    [[nodiscard]] std::optional<PoseView> casePose(const Camera& camera, std::optional<glm::vec4> modelBounds) const;
    [[nodiscard]] static PoseView motionStart(const PoseView& pose, PoseState state);
    void accumulate(const Camera& camera, const Renderer::DisplayCapture& capture);
    void finishPhase();
    void finishStability();
    void finishCompare(const std::vector<double>& mean);
    void finishLightChange();
    void finishEdgeReference();
    void finishEdgeMove();
    void addMetric(std::string_view name, double value);
    void failRun(const std::string& text);
    void finishRun();
    void resetAccumulators();
    [[nodiscard]] std::string runLabel(const Run& run) const;

    // results
    [[nodiscard]] double floorOf(const Run& run, const LabMetricInfo* info) const;
    void finishPlan();
    void logRanking() const;
    void logInteractions() const;
    void logBisections() const;
    void logDecisions() const;
    void logTarget() const;
    void writeCsv() const;
    void writeCostScript() const;
    [[nodiscard]] std::string effectText(const Run& baseline, const Run& run, const LabMetricInfo* info) const;

    LabPlan plan;
    std::vector<size_t> selectedCases;
    bool isPngRequested = false;

    std::vector<Run> runs;
    std::map<std::string, size_t> runIndex;
    std::map<Slot, std::vector<size_t>> floorRuns;
    std::map<Slot, size_t> referenceAgainRuns;
    std::vector<Comparison> comparisons;
    std::vector<OfatRecord> ofatRecords;
    std::vector<PairRecord> pairRecords;
    std::vector<LeverRecord> leverRecords;
    std::vector<Bisection> bisections;
    std::vector<StackRequest> stacks;
    // case and config of every bench run the cost script lists
    std::vector<std::pair<size_t, LabConfig>> costRequests;
    std::vector<size_t> queue;
    size_t queuePosition = 0;

    uint32_t frame = 0;
    bool isFinished = false;
    bool isPlanDone = false;
    uint32_t failedRuns = 0;
    std::optional<PoseView> startupPose;
    std::string loadedModel = "models/Sponza";
    uint32_t settleFrames = 0;
    std::map<std::string, Reference> references;
    size_t referencesCase = SIZE_MAX;

    std::optional<size_t> currentRun;
    std::vector<Phase> phases;
    size_t phaseIndex = 0;
    uint32_t phaseFrame = 0;
    bool isCaptureRequested = false;
    std::optional<float> pinnedExposure;

    // the current phase's captures
    vk::Extent2D extent{};
    uint32_t capturedFrames = 0;
    uint64_t nonFinitePixels = 0;
    bool isCaptureMissing = false;
    labMetrics::StabilityAccumulator stability;
    std::vector<double> sum;
    std::array<double, 3> rgbSum{0.0, 0.0, 0.0};
    std::vector<double> frameMeans;
    std::vector<labMetrics::FrameRegions> edgeFrames;
    // light change: the last reference phase
    double lightReference = 0.0;
    double lightTolerance = 0.0;
    // screen edge: per motion
    labMetrics::DepthMap initialDepth;
    labMetrics::DepthMap finalDepth;
    std::vector<labMetrics::Region> masks;
    vk::Extent2D edgeReferenceExtent{};
    std::vector<double> edgeReference;
    std::array<double, static_cast<size_t>(labMetrics::Region::Count)> edgeTolerance{};
    std::array<double, static_cast<size_t>(labMetrics::Region::Count)> edgeSteadyError{};
};
