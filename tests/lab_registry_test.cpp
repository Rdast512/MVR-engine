// settings lab registry and plans (spec/features/planned/full_testing.md §7 T1, T4): config strings round-trip, sets apply
// before overrides, every built-in plan parses. CPU only: no device, no engine run
#include "test_support.hpp"

#include "lab/engine_lab.hpp"
#include "lab/lab_cases.hpp"
#include "lab/lab_plan.hpp"
#include "lab/settings_registry.hpp"

#include <cstdio>
#include <exception>
#include <format>
#include <set>
#include <string>
#include <string_view>

namespace
{
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
    for (const char* text : {"di=shipped gi=shipped frame=shipped", "di=nvidia gi=nvidia", "di=converged gi=converged",
                             "di=reference", "gi=nvidia frame.indirectMode=nrc"}) {
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

    std::set<std::string_view> caseNames;
    for (const LabCase& labCase : kLabCases) {
        test::check(caseNames.insert(labCase.name).second && labCase.kinds != 0,
                    std::format("case {} is unique and has kinds", labCase.name).c_str());
    }
    for (const char* name : {"sets", "di-ofat", "gi-ofat", "cafe", "leak", "stability-levers", "i15_di", "i15_di_a", "i15_di_b", "i15_guide_a", "i15_guide_b", "i15_gi", "i15_gi_age", "i15_nrc_di0", "i15_nrc_dia", "i15_nrc_dib", "i15_nrc_gi", "boiling", "i16_permutation", "i16_perm_noboil", "i16_noboil", "i16_sun", "i16_variance", "i16_gi_variance", "i16_gi_o2", "i16_gi_o2_32", "i16_gi_exact", "i16_nrc_training", "i16_cv_maps", "i16_cv_nojitter", "i16_guide_base", "i16_guide_spec", "i16_raw_edges"}) {
        const LabPlanParse parse = loadLabPlan(name);
        test::check(parse.plan.has_value() && !parse.plan->entries.empty(),
                    std::format("plan {} parses{}{}", name, parse.error.empty() ? "" : ": ", parse.error)
                        .c_str());
        if (parse.plan) {
            for (const std::string& skipped : parse.plan->skipped) {
                std::printf("       %s skips %s\n", name, skipped.c_str());
            }
        }
    }
    // the runner's expansion without a device: every run, floor, comparison and cost request of each plan
    for (const char* name : {"sets", "di-ofat", "gi-ofat", "cafe", "leak", "stability-levers", "i15_di", "i15_di_a", "i15_di_b", "i15_guide_a", "i15_guide_b", "i15_gi", "i15_gi_age", "i15_nrc_di0", "i15_nrc_dia", "i15_nrc_dib", "i15_nrc_gi", "boiling", "i16_permutation", "i16_perm_noboil", "i16_noboil", "i16_sun", "i16_variance", "i16_gi_variance", "i16_gi_o2", "i16_gi_o2_32", "i16_gi_exact", "i16_nrc_training", "i16_cv_maps", "i16_cv_nojitter", "i16_guide_base", "i16_guide_spec", "i16_raw_edges"}) {
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
