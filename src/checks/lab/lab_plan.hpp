#pragma once

#include "lab_cases.hpp"
#include "render/debug_view.hpp"
#include "render/settings_registry.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// a metric of a lab kind (spec/features/planned/full_testing.md §4.2); names are unique across kinds, so `view:metric` names
// both the kind and the view
struct LabMetricInfo
{
    const char* name;
    LabKind kind;
    // ranked at the end of a plan; the others are logged and written to the CSV only
    bool isRanked;
    // a signed bias against a reference: its noise floor also covers the reference read against itself
    bool isSignedBias;
    // the smallest floor: the metric's resolution (8-frame blocks for settle counts, 0.01 pts for percentages)
    double minFloor;
};

inline constexpr double kPercentFloor = 0.01;
inline constexpr double kSettleFloor = 8.0;

inline constexpr std::array<LabMetricInfo, 44> kLabMetrics = {{
    {"mean", LabKind::Stability, false, false, 1e-9},
    {"std", LabKind::Stability, true, false, kPercentFloor},
    {"flicker", LabKind::Stability, true, false, kPercentFloor},
    {"lit", LabKind::Stability, false, false, kPercentFloor},
    {"f2f", LabKind::Stability, true, false, kPercentFloor},
    {"swing", LabKind::Stability, true, false, kPercentFloor},
    {"lag1", LabKind::Stability, true, false, 0.005},
    // m8 I16: block pops (lab_metrics.hpp kPopBlockPixels)
    {"rises", LabKind::Stability, true, false, 0.01},
    {"dips", LabKind::Stability, false, false, 0.01},
    {"riseSize", LabKind::Stability, false, false, kPercentFloor},
    {"riseFrames", LabKind::Stability, false, false, 0.1},
    {"riseSpread", LabKind::Stability, false, false, kPercentFloor},
    {"blockStd", LabKind::Stability, true, false, kPercentFloor},
    {"blockCorr", LabKind::Stability, false, false, 0.01},
    {"bias", LabKind::DiBias, true, true, kPercentFloor},
    {"p50", LabKind::DiBias, false, false, kPercentFloor},
    {"p90", LabKind::DiBias, true, false, kPercentFloor},
    {"darkLeak", LabKind::DiBias, true, false, kPercentFloor},
    {"dimBias", LabKind::DiBias, true, true, kPercentFloor},
    {"hotBias", LabKind::DiBias, true, true, kPercentFloor},
    {"1b.bias", LabKind::GiBias, true, true, kPercentFloor},
    {"1b.p50", LabKind::GiBias, false, false, kPercentFloor},
    {"1b.p90", LabKind::GiBias, true, false, kPercentFloor},
    {"1b.darkLeak", LabKind::GiBias, true, false, kPercentFloor},
    {"1b.dimBias", LabKind::GiBias, true, true, kPercentFloor},
    {"1b.hotBias", LabKind::GiBias, true, true, kPercentFloor},
    {"1b.rgbBias", LabKind::GiBias, false, false, kPercentFloor},
    {"8b.bias", LabKind::GiBias, true, true, kPercentFloor},
    {"8b.p50", LabKind::GiBias, false, false, kPercentFloor},
    {"8b.p90", LabKind::GiBias, true, false, kPercentFloor},
    {"8b.darkLeak", LabKind::GiBias, true, false, kPercentFloor},
    {"8b.dimBias", LabKind::GiBias, true, true, kPercentFloor},
    {"8b.hotBias", LabKind::GiBias, true, true, kPercentFloor},
    {"8b.rgbBias", LabKind::GiBias, false, false, kPercentFloor},
    {"move.settle", LabKind::LightChange, true, false, kSettleFloor},
    {"move.err16", LabKind::LightChange, false, false, kPercentFloor},
    {"toggle.settle", LabKind::LightChange, true, false, kSettleFloor},
    {"toggle.err16", LabKind::LightChange, false, false, kPercentFloor},
    {"pan.band", LabKind::ScreenEdge, true, false, kSettleFloor},
    {"pan.disoccluded", LabKind::ScreenEdge, true, false, kSettleFloor},
    {"dolly.band", LabKind::ScreenEdge, true, false, kSettleFloor},
    {"dolly.disoccluded", LabKind::ScreenEdge, true, false, kSettleFloor},
    // GPU time is never measured in the lab's process (spec §2): the cost script's bench runs report it
    {"gpuMs", LabKind::Cost, false, false, 0.01},
    {"cpuMs", LabKind::Cost, false, false, 0.01},
}};

[[nodiscard]] const LabMetricInfo* findLabMetric(std::string_view name);
[[nodiscard]] std::optional<DebugView> findDebugView(std::string_view name);
// diBias reads Direct lighting, giBias Indirect radiance; the other kinds read the plan's views
[[nodiscard]] std::optional<DebugView> fixedViewOf(LabKind kind);

struct LabMetricKey
{
    DebugView view;
    const LabMetricInfo* metric;
};

// the cases, views and kinds a directive runs on: the last `cases`, `views` and `kinds` lines above it
struct LabScope
{
    std::vector<size_t> cases;
    std::vector<DebugView> views{DebugView::Lit};
    uint32_t kinds = labKindBit(LabKind::Stability);
};

enum class LabEntryType : uint8_t
{
    // a whole config, compared with the base
    Set,
    // every switch where base and target differ, flipped alone, both directions
    Ofat,
    // one switch across values, on the base or `over` a config
    Sweep,
    // the 2x2 of two switches
    Pairs,
    // a lever (spec §6.6): over the fix and on the base
    Lever,
    // ddmin over the switches base and target differ in (spec §6.4)
    Bisect,
    // the fix plus every lever group's best config on a metric
    Stack,
};

struct LabEntry
{
    LabEntryType type;
    LabScope scope{};
    size_t line = 0;
    // Set: the config text; Lever: "L1 spatial"; Sweep: "over" text
    std::string label{};
    // Lever: the first word of the label; Sweep: the group named by `lever`
    std::string group{};
    // Set: the config; Lever: over the fix; Sweep: the config the sweep starts from
    LabConfig config{};
    // Lever: the same overrides on the base
    LabConfig onBase{};
    const LabSwitch* sweepSwitch = nullptr;
    std::vector<double> sweepValues{};
    std::array<std::optional<LabSwitchValue>, 2> pair{};
    std::optional<LabMetricKey> metric{};
};

struct LabPlan
{
    std::string name;
    LabConfig base;
    std::optional<LabConfig> target;
    std::optional<LabConfig> fix;
    uint32_t seeds = 3;
    std::vector<LabEntry> entries;
    // planned switches and cases the plan names: skipped, logged
    std::vector<std::string> skipped;
};

struct LabPlanParse
{
    std::optional<LabPlan> plan;
    std::string error;
};

// `name`: a built-in plan or a file in tests/lab/ (spec §6.1)
[[nodiscard]] LabPlanParse loadLabPlan(std::string_view name);
[[nodiscard]] LabPlanParse parseLabPlan(std::string_view name, std::string_view text);
[[nodiscard]] std::optional<std::string_view> builtInLabPlan(std::string_view name);
