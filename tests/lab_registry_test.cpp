// settings lab registry and plans (spec/features/planned/full_testing.md §7 T1, T4): config strings round-trip, sets apply
// before overrides, every built-in plan parses. CPU only: no device, no engine run
#include "test_support.hpp"

#include "checks/lab/engine_lab.hpp"
#include "checks/lab/lab_cases.hpp"
#include "checks/lab/lab_plan.hpp"
#include "render/settings_registry.hpp"
#include "ui/settings_rows.hpp"

#include <array>
#include <cstdio>
#include <exception>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    // built-in plans, then the tests/lab plan files
    constexpr std::array kPlanNames{"sets",
                                    "di-ofat",
                                    "gi-ofat",
                                    "cafe",
                                    "leak",
                                    "stability-levers",
                                    "i15_di",
                                    "i15_di_a",
                                    "i15_di_b",
                                    "i15_guide_a",
                                    "i15_guide_b",
                                    "i15_gi",
                                    "i15_gi_age",
                                    "i15_nrc_di0",
                                    "i15_nrc_dia",
                                    "i15_nrc_dib",
                                    "i15_nrc_gi",
                                    "boiling",
                                    "i16_permutation",
                                    "i16_perm_noboil",
                                    "i16_noboil",
                                    "i16_sun",
                                    "i16_variance",
                                    "i16_gi_variance",
                                    "i16_gi_o2",
                                    "i16_gi_o2_32",
                                    "i16_gi_exact",
                                    "i16_nrc_training",
                                    "i16_cv_maps",
                                    "i16_cv_nojitter",
                                    "i16_guide_base",
                                    "i16_guide_spec",
                                    "i16_raw_edges",
                                    "i16_pops",
                                    "i16_pop_levers",
                                    "i16_pop_history",
                                    "i16_pop_base",
                                    "i16_pop_dih4",
                                    "i16_pop_dih8",
                                    "i16_attrib",
                                    "i16_di_sampling",
                                    "i16_gi_levers",
                                    "i16_floor",
                                    "i16_clamp",
                                    "i16_input_history",
                                    "i16_input_history_check",
                                    "i16_input_structure",
                                    "i16_light_tiles",
                                    "i16_regir_tiles",
                                    "i16_di_spatial_only",
                                    "i16_preset_samples",
                                    "i16_dlaa",
                                    "i16_val_base",
                                    "i16_val_regir",
                                    "i16_val_regir32",
                                    "i16_bias_tiles",
                                    "i16_bias_regir",
                                    "i16_val_cost",
                                    "i16_val_tiles",
                                    "i16_val_tiles32",
                                    "i16_val_tiles16",
                                    "i16_tiles_cost"};

    bool roundTrips(const LabConfig& config)
    {
        const std::string text = formatLabConfig(config);
        const LabConfigParse parse = parseLabConfig(text);
        return parse.config && isSameConfig(*parse.config, config) && formatLabConfig(*parse.config) == text;
    }

    // a value other than the current one, within the switch's range
    double otherValue(const LabSwitch& entry, double value)
    {
        switch (entry.type) {
        case LabSwitchType::Bool:
            return value != 0.0 ? 0.0 : 1.0;
        case LabSwitchType::Uint:
            return value + 3.0;
        case LabSwitchType::Float:
            return value * 0.5 + 0.25;
        case LabSwitchType::Enum:
            for (size_t index = 0; index < entry.valueNames.size(); ++index) {
                if (static_cast<double>(index) != value && *entry.valueNames[index] != '\0') {
                    return static_cast<double>(index);
                }
            }
            return value;
        }
        return value;
    }
} // namespace

int main()
{
    for (const char* text :
         {"di=shipped gi=shipped frame=shipped", "di=nvidia gi=nvidia", "di=converged gi=converged", "di=reference",
          "gi=nvidia frame.indirectMode=nrc", "di=fast", "di=medium", "di=unbiased", "di=ultra",
          "frame.denoiser=off frame.aa=accumulation frame.accumulationFrames=64", "di.temporalOnly=1", "di=tuned gi=tuned"}) {
        const LabConfigParse parse = parseLabConfig(text);
        test::check(parse.config.has_value() && roundTrips(*parse.config),
                    std::format("'{}' round-trips", text).c_str());
    }

    std::set<std::string_view> names;
    for (const LabSwitch& entry : labSwitches()) {
        test::check(names.insert(entry.name).second, std::format("{} is registered once", entry.name).c_str());
        if (entry.notBuilt != nullptr) {
            const LabConfigParse parse = parseLabConfig(std::format("{}=1", entry.name));
            test::check(!parse.config && parse.isPlanned, std::format("{} is refused as planned", entry.name).c_str());
            continue;
        }
        LabConfig config;
        const double value = otherValue(entry, entry.get(config));
        entry.set(config, value);
        const std::string text = formatLabConfig(config);
        const LabConfigParse parse = parseLabConfig(text);
        test::check(parse.config && entry.get(*parse.config) == entry.get(config) && roundTrips(config),
                    std::format("{}: '{}' round-trips", entry.name, text).c_str());
    }

    {
        const LabConfigParse parse = parseLabConfig("di.spatialSamples=4 di=nvidia");
        test::check(parse.config && parse.config->di.spatialSamples == 4 &&
                        parse.config->di.isPermutationSamplingEnabled ==
                            RtxdiContext::nvidiaSettings().isPermutationSamplingEnabled,
                    "sets apply before overrides");
    }
    for (const char* text : {"di.spatialSample=2", "di.spatialSamples=two", "gi.temporalCorrection=pairwise",
                             "di=nvidiaa", "spatialSamples"}) {
        const LabConfigParse parse = parseLabConfig(text);
        test::check(!parse.config && !parse.isPlanned && !parse.error.empty(),
                    std::format("'{}' is refused: {}", text, parse.error).c_str());
    }

    // NVIDIA's presets (UserInterface.cpp UIData::ApplyPreset); spatial counts normalized to the stratified walk
    using Preset = RtxdiContext::QualityPreset;
    test::check(RtxdiContext::nvidiaSettings() == RtxdiContext::nvidiaPreset(Preset::Medium),
                "the NVIDIA defaults are the Medium preset");
    test::check(RtxdiContext::defaultSettings() == RtxdiContext::nvidiaSettings() &&
                    RtxdiContext::defaultGiSettings() == RtxdiContext::nvidiaGiSettings() &&
                    LabConfig{}.indirectMode == IndirectMode::Nrc,
                "the shipped settings are NVIDIA's, in NRC mode");
    {
        const RtxdiContext::Settings fast = RtxdiContext::nvidiaPreset(Preset::Fast);
        test::check(fast.localLightSamplingMode == ReSTIRDI_LocalLightSamplingMode::Power_RIS &&
                        fast.localLightSamples[std::to_underlying(ReSTIRDI_LocalLightSamplingMode::Power_RIS)] == 4 &&
                        fast.brdfSamples == 0 &&
                        fast.temporalBiasCorrection == ReSTIRDI_TemporalBiasCorrectionMode::Off &&
                        fast.spatialBiasCorrection == ReSTIRDI_SpatialBiasCorrectionMode::Off &&
                        fast.spatialSamples == 2 && fast.disocclusionBoostSamples == 2 && fast.isBoilingFilterEnabled,
                    "Fast preset");
        const RtxdiContext::Settings unbiased = RtxdiContext::nvidiaPreset(Preset::Unbiased);
        test::check(unbiased.localLightSamplingMode == ReSTIRDI_LocalLightSamplingMode::Uniform &&
                        unbiased.spatialBiasCorrection == ReSTIRDI_SpatialBiasCorrectionMode::Raytraced &&
                        !unbiased.isBoilingFilterEnabled && !unbiased.isVisibilityShortcutEnabled &&
                        !unbiased.isFinalVisibilityReused && unbiased.disocclusionBoostSamples == 8,
                    "Unbiased preset");
        const LabConfigParse parse = parseLabConfig("di=ultra");
        test::check(parse.config && parse.config->di == RtxdiContext::nvidiaPreset(Preset::Ultra),
                    "di=ultra is the Ultra preset");
    }

    // the settings window: every row edits a registered switch, and every switch an NVIDIA preset or the converged
    // set changes has a row to highlight (nvidia_settings_layout.md §5)
    for (const SettingsRows rows : settingsRows::kAll) {
        for (const SettingsRow& row : rows) {
            test::check(findLabSwitch(row.switchName) != nullptr,
                        std::format("settings row {} is a lab switch", row.switchName).c_str());
        }
    }
    std::vector<LabConfig> comparisons;
    for (const Preset preset : {Preset::Fast, Preset::Medium, Preset::Unbiased, Preset::Ultra, Preset::Reference}) {
        LabConfig config;
        config.di = RtxdiContext::nvidiaPreset(preset);
        config.isDiReference = preset == Preset::Reference;
        config.gi = RtxdiContext::nvidiaGiSettings();
        config.indirectMode = IndirectMode::RestirGi;
        config.dlss.quality = Dlss::Quality::Dlaa;
        comparisons.push_back(config);
    }
    comparisons.emplace_back();
    comparisons.back().di = RtxdiContext::tunedSettings();
    comparisons.back().gi = RtxdiContext::tunedGiSettings();
    comparisons.back().indirectMode = IndirectMode::NrcRestirGi;
    comparisons.emplace_back();
    comparisons.back().di = RtxdiContext::convergedSettings();
    comparisons.back().gi = RtxdiContext::convergedGiSettings();
    for (const LabConfig& comparison : comparisons) {
        for (const LabSwitchValue& difference : differingSwitches(comparison, LabConfig{})) {
            test::check(settingsRows::hasRow(difference.entry->name),
                        std::format("{} has a settings row", difference.entry->name).c_str());
        }
    }
    // M8.2 S5 (C-NS-1): Compare with Shipped highlights nothing; Compare with NVIDIA Medium highlights exactly
    // {frame.indirectMode, frame.dlssQuality} and closed-section counts match deduplicated settingsRows
    {
        const LabConfig shipped{};
        test::check(differingSwitches(shipped, shipped).empty(), "Compare with Shipped highlights 0 rows");
        const LabConfig& nvidiaMedium = comparisons[1];
        const std::vector<LabSwitchValue> mediumDiff = differingSwitches(nvidiaMedium, shipped);
        std::set<std::string_view> mediumDiffNames;
        for (const LabSwitchValue& diff : mediumDiff) {
            mediumDiffNames.insert(diff.entry->name);
        }
        const std::set<std::string_view> expectedMediumDiff{"frame.indirectMode", "frame.dlssQuality"};
        test::check(mediumDiffNames == expectedMediumDiff,
                    "Compare with NVIDIA Medium highlights exact {frame.indirectMode, frame.dlssQuality}");
        // closed section distinct counts (with Show Advanced Settings on/off): shared DI/GI thresholds count once
        const auto countSection = [](const std::vector<LabSwitchValue>& diffs,
                                     std::initializer_list<SettingsRows> lists, bool showAdvanced) -> uint32_t {
            std::set<std::string_view> seen;
            for (const SettingsRows rows : lists) {
                for (const SettingsRow& row : rows) {
                    if (row.isAdvanced && !showAdvanced) {
                        continue;
                    }
                    for (const LabSwitchValue& diff : diffs) {
                        if (std::string_view(diff.entry->name) == row.switchName) {
                            seen.insert(row.switchName);
                        }
                    }
                }
            }
            return static_cast<uint32_t>(seen.size());
        };
        using namespace settingsRows;
        test::check(countSection(mediumDiff, {kIndirect, kIndirectReuse, kIndirectNrc, kIndirectTemporal,
                                              kIndirectSpatial, kIndirectEngine, kIndirectPtProfile},
                                 false) == 1 &&
                        countSection(mediumDiff, {kDenoising, kDenoisingDlss}, false) == 1 &&
                        countSection(mediumDiff, {kDirect, kDirectLocalLights, kDirectInitial, kDirectTemporal,
                                                  kDirectSpatial, kDirectFinal, kDirectEngine},
                                     true) == 0,
                    "Compare with NVIDIA Medium closed-section counts (Indirect=1, Denoising=1, Direct=0)");
        // shared threshold deduplication: editing di.depthThreshold appears in both kDirectTemporal and kDirectSpatial
        LabConfig thresholdEdit{};
        thresholdEdit.di.depthThreshold = shipped.di.depthThreshold + 0.1f;
        const std::vector<LabSwitchValue> thresholdDiff = differingSwitches(shipped, thresholdEdit);
        test::check(countSection(thresholdDiff, {kDirectTemporal, kDirectSpatial}, false) == 0 &&
                        countSection(thresholdDiff, {kDirectTemporal, kDirectSpatial}, true) == 1,
                    "shared advanced DI threshold hidden when !showAdvanced and counted once when showAdvanced");
    }

    std::set<std::string_view> caseNames;
    for (const LabCase& labCase : kLabCases) {
        test::check(caseNames.insert(labCase.name).second && labCase.kinds != 0,
                    std::format("case {} is unique and has kinds", labCase.name).c_str());
    }
    for (const char* name : kPlanNames) {
        const LabPlanParse parse = loadLabPlan(name);
        test::check(parse.plan.has_value() && !parse.plan->entries.empty(),
                    std::format("plan {} parses{}{}", name, parse.error.empty() ? "" : ": ", parse.error).c_str());
        if (parse.plan) {
            for (const std::string& skipped : parse.plan->skipped) {
                std::printf("       %s skips %s\n", name, skipped.c_str());
            }
        }
    }
    // the runner's expansion without a device: every run, floor, comparison and cost request of each plan
    for (const char* name : kPlanNames) {
        bool isExpanded = true;
        try {
            const EngineLab lab(name);
        } catch (const std::exception& error) {
            std::printf("       %s\n", error.what());
            isExpanded = false;
        }
        test::check(isExpanded, std::format("plan {} expands", name).c_str());
    }
    {
        const LabPlanParse parse = parseLabPlan("typo", "cases cafe.tables\nsets di.spatialSample=2\n");
        test::check(!parse.plan && parse.error.contains("line 2"), "a plan typo is refused with its line");
    }
    return test::finish();
}
