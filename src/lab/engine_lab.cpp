#include "engine_lab.hpp"
#include "../static_headers/logger.hpp"
#include "core/check_images.hpp"
#include "core/convergence_metrics.hpp"

#include <SDL3/SDL_filesystem.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <limits>
#include <numeric>
#include <ranges>
#include <set>
#include <stdexcept>
#include <utility>

namespace
{
    constexpr std::string_view kCategory = "Lab";
    constexpr const char* kLabFolder = "lab";

    std::filesystem::path repositoryRoot() { return std::filesystem::path(ENGINE_MODELS_DIR).parent_path(); }

    std::filesystem::path labDirectory()
    {
        const char* base = SDL_GetBasePath();
        const std::filesystem::path directory = std::filesystem::path(base != nullptr ? base : "") / "logs" / kLabFolder;
        std::error_code errorCode;
        std::filesystem::create_directories(directory, errorCode);
        return directory;
    }

    bool isVariableSet(const char* name)
    {
        const char* text = std::getenv(name);
        return text != nullptr && std::string_view(text) == "1";
    }

    const char* unitOf(const LabMetricInfo* info)
    {
        if (info->minFloor == kSettleFloor) {
            return " frames";
        }
        return info->minFloor == kPercentFloor ? " pts" : "";
    }

    std::string csvField(std::string_view text)
    {
        std::string field = "\"";
        for (const char c : text) {
            field += c == '"' ? std::string("\"\"") : std::string(1, c);
        }
        return field + "\"";
    }
} // namespace

std::optional<double> EngineLab::Run::metric(const LabMetricInfo* info) const
{
    for (const Metric& entry : metrics) {
        if (entry.info == info) {
            return entry.value;
        }
    }
    return std::nullopt;
}

std::optional<std::string> EngineLab::requestedPlan()
{
    const char* text = std::getenv("ENGINE_LAB");
    if (text == nullptr || *text == '\0') {
        return std::nullopt;
    }
    return std::string(text);
}

EngineLab::EngineLab(std::string_view planName) : isPngRequested(isVariableSet("ENGINE_LAB_PNG"))
{
    LabPlanParse parse = loadLabPlan(planName);
    if (!parse.plan) {
        throw std::runtime_error(std::format("ENGINE_LAB: {}", parse.error));
    }
    plan = std::move(*parse.plan);
    for (const std::string& skipped : plan.skipped) {
        logInfo(std::format("plan {}: skipped {}", plan.name, skipped), kCategory);
    }
    if (const char* filter = std::getenv("ENGINE_LAB_CASES"); filter != nullptr && *filter != '\0') {
        for (const auto part : std::views::split(std::string_view(filter), ',')) {
            const std::string_view name(part.begin(), part.end());
            const LabCase* labCase = findLabCase(name);
            if (labCase == nullptr) {
                throw std::runtime_error(std::format("ENGINE_LAB_CASES: '{}' is not a lab case", name));
            }
            selectedCases.push_back(labCaseIndex(*labCase));
        }
    }
    expandPlan();
    logInfo(std::format("plan {}: {} runs before bisection and stacks, {} cost runs for the bench script, results "
                        "follow",
                        plan.name, queue.size(), costRequests.size()),
            kCategory);
}

uint32_t EngineLab::seedOf(std::string_view key)
{
    // FNV-1a, then the murmur3 finalizer
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const char c : key) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
    }
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdull;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ull;
    hash ^= hash >> 33;
    // even, below 2^32 minus the longest phase: RTXDI's buffer parity follows the frame index
    constexpr uint32_t kLongestPhase = 1u << 14;
    return static_cast<uint32_t>(hash % (UINT32_MAX - kLongestPhase)) & ~1u;
}

std::string EngineLab::viewName(DebugView view) { return kDebugViews[std::to_underlying(view)].name; }

bool EngineLab::isCaseSelected(size_t caseIndex) const
{
    return selectedCases.empty() || std::ranges::contains(selectedCases, caseIndex);
}

std::vector<EngineLab::Slot> EngineLab::slotsOf(const LabScope& scope) const
{
    std::vector<Slot> slots;
    for (const size_t caseIndex : scope.cases) {
        if (!isCaseSelected(caseIndex)) {
            continue;
        }
        const uint32_t kinds = scope.kinds & kLabCases[caseIndex].kinds;
        for (uint32_t kind = 0; kind < std::to_underlying(LabKind::Cost); ++kind) {
            if ((kinds & (1u << kind)) == 0) {
                continue;
            }
            const auto labKind = static_cast<LabKind>(kind);
            if (const std::optional<DebugView> fixed = fixedViewOf(labKind)) {
                slots.emplace_back(caseIndex, labKind, *fixed);
                continue;
            }
            for (const DebugView view : scope.views) {
                slots.emplace_back(caseIndex, labKind, view);
            }
        }
    }
    return slots;
}

size_t EngineLab::ensureRun(size_t caseIndex, LabKind kind, DebugView view, const LabConfig& config,
                            uint32_t seedIndex, Variant variant)
{
    LabConfig runConfig = config;
    // the GI check's configs: its truths have no jitter (spec §4.2)
    if (kind == LabKind::GiBias) {
        runConfig.isJitterEnabled = false;
    }
    std::string text = formatLabConfig(runConfig);
    const std::string key = std::format("{}|{}|{}|{}|{}|{}", kLabCases[caseIndex].name,
                                        kLabKindNames[std::to_underlying(kind)], viewName(view), text, seedIndex,
                                        std::to_underlying(variant));
    if (const auto found = runIndex.find(key); found != runIndex.end()) {
        return found->second;
    }
    runs.push_back({.caseIndex = caseIndex,
                    .kind = kind,
                    .view = view,
                    .config = runConfig,
                    .configText = std::move(text),
                    .seedIndex = seedIndex,
                    .variant = variant});
    runIndex.emplace(key, runs.size() - 1);
    return runs.size() - 1;
}

void EngineLab::schedule(size_t run)
{
    if (!runs[run].isScheduled) {
        runs[run].isScheduled = true;
        queue.push_back(run);
    }
}

void EngineLab::ensureFloorRuns(const Slot& slot)
{
    if (floorRuns.contains(slot)) {
        return;
    }
    const auto [caseIndex, kind, view] = slot;
    std::vector<size_t>& seeds = floorRuns[slot];
    for (uint32_t seed = 0; seed < plan.seeds; ++seed) {
        seeds.push_back(ensureRun(caseIndex, kind, view, plan.base, seed));
        schedule(seeds.back());
    }
    if (kind == LabKind::DiBias || kind == LabKind::GiBias) {
        const size_t again = ensureRun(caseIndex, kind, view, plan.base, 0, Variant::ReferenceAgain);
        referenceAgainRuns[slot] = again;
        schedule(again);
    }
}

void EngineLab::addCostRequest(size_t caseIndex, const LabConfig& config)
{
    const bool isKnown = std::ranges::any_of(costRequests, [&](const auto& request) {
        return request.first == caseIndex && isSameConfig(request.second, config);
    });
    if (!isKnown) {
        costRequests.emplace_back(caseIndex, config);
    }
}

void EngineLab::expandPlan()
{
    for (const LabEntry& entry : plan.entries) {
        expandEntry(entry);
    }
    // grouped by case: one model load per case
    std::ranges::stable_sort(queue, {}, [&](size_t run) { return runs[run].caseIndex; });
}

void EngineLab::expandEntry(const LabEntry& entry)
{
    const std::vector<Slot> slots = slotsOf(entry.scope);
    const bool isCostRequested = (entry.scope.kinds & labKindBit(LabKind::Cost)) != 0;
    const auto cost = [&](const LabConfig& config) {
        if (!isCostRequested) {
            return;
        }
        for (const size_t caseIndex : entry.scope.cases) {
            if (isCaseSelected(caseIndex) && (kLabCases[caseIndex].kinds & labKindBit(LabKind::Cost)) != 0) {
                addCostRequest(caseIndex, plan.base);
                addCostRequest(caseIndex, config);
            }
        }
    };
    const auto compare = [&](size_t baseline, size_t run, std::string label) {
        schedule(baseline);
        schedule(run);
        comparisons.push_back({std::move(label), baseline, run});
    };

    switch (entry.type) {
    case LabEntryType::Set:
        for (const Slot& slot : slots) {
            ensureFloorRuns(slot);
            const auto [caseIndex, kind, view] = slot;
            compare(floorRuns[slot].front(), ensureRun(caseIndex, kind, view, entry.config),
                    std::format("set {}", entry.label));
        }
        cost(entry.config);
        break;
    case LabEntryType::Ofat: {
        const std::vector<LabSwitchValue> differing = differingSwitches(plan.base, *plan.target);
        for (const Slot& slot : slots) {
            ensureFloorRuns(slot);
            const auto [caseIndex, kind, view] = slot;
            const size_t base = floorRuns[slot].front();
            const size_t target = ensureRun(caseIndex, kind, view, *plan.target);
            compare(base, target, "target");
            for (const LabSwitchValue& difference : differing) {
                const LabSwitch& entrySwitch = *difference.entry;
                LabConfig forwardConfig = plan.base;
                entrySwitch.set(forwardConfig, difference.value);
                LabConfig backwardConfig = *plan.target;
                entrySwitch.set(backwardConfig, entrySwitch.get(plan.base));
                const std::string flip =
                    std::format("{} {}→{}", entrySwitch.name, formatSwitchValue(entrySwitch, entrySwitch.get(plan.base)),
                                formatSwitchValue(entrySwitch, difference.value));
                const size_t forward = ensureRun(caseIndex, kind, view, forwardConfig);
                const size_t backward = ensureRun(caseIndex, kind, view, backwardConfig);
                compare(base, forward, std::format("{} from base", flip));
                compare(backward, target, std::format("{} in target", flip));
                ofatRecords.push_back({entrySwitch.name, forward, base, backward, target});
            }
        }
        cost(*plan.target);
        break;
    }
    case LabEntryType::Sweep: {
        const LabSwitch& sweepSwitch = *entry.sweepSwitch;
        const std::string over = entry.label.empty() ? std::string("base") : entry.label;
        for (const Slot& slot : slots) {
            ensureFloorRuns(slot);
            const auto [caseIndex, kind, view] = slot;
            const size_t start = ensureRun(caseIndex, kind, view, entry.config);
            if (start != floorRuns[slot].front()) {
                compare(floorRuns[slot].front(), start, std::format("over {}", over));
            }
            for (const double value : entry.sweepValues) {
                LabConfig config = entry.config;
                sweepSwitch.set(config, value);
                const size_t run = ensureRun(caseIndex, kind, view, config);
                if (run == start) {
                    continue;
                }
                const std::string label = std::format("{} {}→{} over {}", sweepSwitch.name,
                                                      formatSwitchValue(sweepSwitch, sweepSwitch.get(entry.config)),
                                                      formatSwitchValue(sweepSwitch, value), over);
                compare(start, run, label);
                if (!entry.group.empty()) {
                    leverRecords.push_back({entry.group,
                                            std::format("{} {}={}", entry.group, sweepSwitch.name,
                                                        formatSwitchValue(sweepSwitch, value)),
                                            start, run, std::nullopt, std::nullopt});
                }
            }
        }
        for (const double value : entry.sweepValues) {
            LabConfig config = entry.config;
            sweepSwitch.set(config, value);
            cost(config);
        }
        break;
    }
    case LabEntryType::Pairs: {
        LabConfig first = plan.base;
        entry.pair[0]->entry->set(first, entry.pair[0]->value);
        LabConfig second = plan.base;
        entry.pair[1]->entry->set(second, entry.pair[1]->value);
        LabConfig both = first;
        entry.pair[1]->entry->set(both, entry.pair[1]->value);
        const std::string firstLabel = std::format("{}={}", entry.pair[0]->entry->name,
                                                   formatSwitchValue(*entry.pair[0]->entry, entry.pair[0]->value));
        const std::string secondLabel = std::format("{}={}", entry.pair[1]->entry->name,
                                                    formatSwitchValue(*entry.pair[1]->entry, entry.pair[1]->value));
        for (const Slot& slot : slots) {
            ensureFloorRuns(slot);
            const auto [caseIndex, kind, view] = slot;
            const size_t base = floorRuns[slot].front();
            const size_t firstRun = ensureRun(caseIndex, kind, view, first);
            const size_t secondRun = ensureRun(caseIndex, kind, view, second);
            const size_t bothRun = ensureRun(caseIndex, kind, view, both);
            compare(base, firstRun, std::format("pair {} alone", firstLabel));
            compare(base, secondRun, std::format("pair {} alone", secondLabel));
            compare(base, bothRun, std::format("pair {} + {}", firstLabel, secondLabel));
            pairRecords.push_back({std::format("{} + {}", firstLabel, secondLabel), base, firstRun, secondRun, bothRun});
        }
        cost(both);
        break;
    }
    case LabEntryType::Lever:
        for (const Slot& slot : slots) {
            ensureFloorRuns(slot);
            const auto [caseIndex, kind, view] = slot;
            const size_t base = floorRuns[slot].front();
            const size_t fix = ensureRun(caseIndex, kind, view, *plan.fix);
            const size_t overFix = ensureRun(caseIndex, kind, view, entry.config);
            const size_t onBase = ensureRun(caseIndex, kind, view, entry.onBase);
            compare(base, fix, "fix");
            compare(fix, overFix, std::format("{} over fix", entry.label));
            compare(base, onBase, std::format("{} on base", entry.label));
            leverRecords.push_back({entry.group, entry.label, fix, overFix, base, onBase});
        }
        cost(*plan.fix);
        cost(entry.config);
        break;
    case LabEntryType::Bisect: {
        const LabMetricKey key = *entry.metric;
        for (const size_t caseIndex : entry.scope.cases) {
            if (!isCaseSelected(caseIndex) || (kLabCases[caseIndex].kinds & labKindBit(key.metric->kind)) == 0) {
                continue;
            }
            const Slot slot{caseIndex, key.metric->kind, key.view};
            ensureFloorRuns(slot);
            const size_t target = ensureRun(caseIndex, key.metric->kind, key.view, *plan.target);
            schedule(target);
            bisections.push_back({.key = key, .caseIndex = caseIndex, .baseRun = floorRuns[slot].front(),
                                  .targetRun = target});
        }
        break;
    }
    case LabEntryType::Stack:
        for (const Slot& slot : slots) {
            ensureFloorRuns(slot);
            const auto [caseIndex, kind, view] = slot;
            schedule(ensureRun(caseIndex, kind, view, *plan.fix));
        }
        stacks.push_back({*entry.metric, entry.scope});
        break;
    }
}

void EngineLab::scheduleAdaptive()
{
    const size_t queued = queue.size();
    for (Bisection& bisection : bisections) {
        if (!bisection.isDone) {
            advanceBisection(bisection);
        }
    }
    if (queue.size() > queued || std::ranges::any_of(bisections, [](const Bisection& b) { return !b.isDone; })) {
        return;
    }
    for (StackRequest& stack : stacks) {
        if (!stack.isScheduled) {
            scheduleStack(stack);
            return;
        }
    }
}

void EngineLab::advanceBisection(Bisection& bisection)
{
    const LabMetricInfo* info = bisection.key.metric;
    if (!runs[bisection.baseRun].isDone || !runs[bisection.targetRun].isDone) {
        return;
    }
    // copies: ensureRun below may grow runs
    const LabConfig baseConfig = runs[bisection.baseRun].config;
    const LabConfig targetConfig = runs[bisection.targetRun].config;
    const auto finish = [&](std::string outcome) {
        bisection.outcome = std::move(outcome);
        bisection.isDone = true;
    };
    const std::optional<double> baseValue = runs[bisection.baseRun].metric(info);
    if (!bisection.isStarted) {
        const std::optional<double> targetValue = runs[bisection.targetRun].metric(info);
        if (!baseValue || !targetValue) {
            finish("the base or the target run failed");
            return;
        }
        bisection.targetEffect = *targetValue - *baseValue;
        const double floor = floorOf(runs[bisection.baseRun], info);
        if (std::abs(bisection.targetEffect) < kEffectFloors * floor) {
            finish(std::format("the target's effect {:+.3g}{} is within {} noise floors ({:.3g})",
                               bisection.targetEffect, unitOf(info), kEffectFloors, floor));
            return;
        }
        bisection.differing = differingSwitches(baseConfig, targetConfig);
        bisection.current.resize(bisection.differing.size());
        std::iota(bisection.current.begin(), bisection.current.end(), size_t{0});
        bisection.isStarted = true;
    }
    const auto configOf = [&](const std::vector<size_t>& subset) {
        LabConfig config = baseConfig;
        for (const size_t index : subset) {
            bisection.differing[index].entry->set(config, bisection.differing[index].value);
        }
        return config;
    };
    while (bisection.current.size() >= 2) {
        if (bisection.candidates.empty()) {
            const size_t count = std::min(bisection.granularity, bisection.current.size());
            bisection.candidates.assign(count, {});
            for (size_t index = 0; index < bisection.current.size(); ++index) {
                bisection.candidates[index * count / bisection.current.size()].push_back(bisection.current[index]);
            }
            bisection.chunkCount = count;
            // with two chunks each complement is the other chunk
            if (count > 2) {
                for (size_t chunk = 0; chunk < count; ++chunk) {
                    std::vector<size_t> complement;
                    std::ranges::set_difference(bisection.current, bisection.candidates[chunk],
                                                std::back_inserter(complement));
                    bisection.candidates.push_back(std::move(complement));
                }
            }
            bisection.candidateIndex = 0;
        }
        bool isReduced = false;
        while (bisection.candidateIndex < bisection.candidates.size()) {
            const std::vector<size_t>& candidate = bisection.candidates[bisection.candidateIndex];
            const size_t run = ensureRun(bisection.caseIndex, info->kind, bisection.key.view, configOf(candidate));
            if (!runs[run].isDone) {
                if (!runs[run].isScheduled) {
                    ++bisection.testedRuns;
                }
                schedule(run);
                return;
            }
            const std::optional<double> value = runs[run].metric(info);
            if (value && (*value - *baseValue) / bisection.targetEffect >= kReproduceShare) {
                const bool isChunk = bisection.candidateIndex < bisection.chunkCount;
                bisection.current = candidate;
                bisection.setEffect = *value - *baseValue;
                bisection.granularity = isChunk ? 2 : std::max<size_t>(bisection.granularity - 1, 2);
                bisection.candidates.clear();
                isReduced = true;
                break;
            }
            ++bisection.candidateIndex;
        }
        if (isReduced) {
            continue;
        }
        if (bisection.granularity >= bisection.current.size()) {
            break;
        }
        bisection.granularity = std::min(bisection.granularity * 2, bisection.current.size());
        bisection.candidates.clear();
    }
    if (bisection.current.size() == bisection.differing.size()) {
        bisection.setEffect = bisection.targetEffect;
    }
    std::string set;
    for (const size_t index : bisection.current) {
        const LabSwitchValue& difference = bisection.differing[index];
        set += std::format("{}{}={}", set.empty() ? "" : " ", difference.entry->name,
                           formatSwitchValue(*difference.entry, difference.value));
    }
    finish(std::format("{} (effect {:+.3g}{} of the target's {:+.3g}, {} runs)", set, bisection.setEffect,
                       unitOf(info), bisection.targetEffect, bisection.testedRuns));
}

void EngineLab::scheduleStack(StackRequest& stack)
{
    stack.isScheduled = true;
    const LabMetricInfo* info = stack.key.metric;
    // a lever group's best config: the lowest mean of the metric over the cases it ran on
    std::vector<std::string> groups;
    for (const LeverRecord& record : leverRecords) {
        if (record.group != "Stack" && !std::ranges::contains(groups, record.group)) {
            groups.push_back(record.group);
        }
    }
    LabConfig config = *plan.fix;
    std::string chosen;
    for (const std::string& group : groups) {
        std::map<std::string, std::pair<double, double>> totals;
        for (const LeverRecord& record : leverRecords) {
            const Run& run = runs[record.run];
            const Run& reference = runs[record.reference];
            if (record.group != group || run.view != stack.key.view || run.kind != info->kind ||
                !isSameConfig(reference.config, *plan.fix)) {
                continue;
            }
            const std::optional<double> value = run.metric(info);
            const std::optional<double> fixValue = reference.metric(info);
            if (value && fixValue) {
                totals[record.label].first += *value - *fixValue;
                totals[record.label].second += 1.0;
            }
        }
        const auto best = std::ranges::min_element(totals, {}, [](const auto& entry) {
            return entry.second.first / entry.second.second;
        });
        if (best == totals.end() || best->second.first >= 0.0) {
            continue;
        }
        const auto record = std::ranges::find_if(leverRecords, [&](const LeverRecord& r) { return r.label == best->first; });
        for (const LabSwitchValue& difference : differingSwitches(*plan.fix, runs[record->run].config)) {
            difference.entry->set(config, difference.value);
        }
        chosen += std::format("{}{}", chosen.empty() ? "" : ", ", best->first);
    }
    logInfo(std::format("plan {}: stack on {}:{} = fix + {} -> {}", plan.name, viewName(stack.key.view), info->name,
                        chosen.empty() ? std::string("nothing better than the fix") : chosen, formatLabConfig(config)),
            kCategory);
    const LabEntry entry{.type = LabEntryType::Lever,
                         .scope = stack.scope,
                         .label = "Stack",
                         .group = "Stack",
                         .config = config,
                         .onBase = config};
    const size_t queued = queue.size();
    expandEntry(entry);
    std::stable_sort(queue.begin() + static_cast<std::ptrdiff_t>(queued), queue.end(),
                     [&](size_t left, size_t right) { return runs[left].caseIndex < runs[right].caseIndex; });
}

bool EngineLab::startNextRun()
{
    for (;;) {
        while (queuePosition < queue.size()) {
            const size_t run = queue[queuePosition++];
            if (runs[run].isDone) {
                continue;
            }
            currentRun = run;
            buildPhases(runs[run]);
            phaseIndex = 0;
            phaseFrame = 0;
            return true;
        }
        const size_t queued = queue.size();
        scheduleAdaptive();
        if (queue.size() == queued) {
            return false;
        }
    }
}

void EngineLab::buildPhases(const Run& run)
{
    phases.clear();
    const LabCase& labCase = kLabCases[run.caseIndex];
    const std::string runKey = std::format("{}|{}|{}|{}|{}|{}", labCase.name, kLabKindNames[std::to_underlying(run.kind)],
                                           viewName(run.view), run.configText, run.seedIndex,
                                           std::to_underlying(run.variant));
    const auto phaseSeed = [&](std::string_view tag) { return seedOf(std::format("{}|{}", runKey, tag)); };
    switch (run.kind) {
    case LabKind::Stability:
        phases.push_back({.type = PhaseType::Stability, .view = run.view, .config = run.config,
                          .warmup = kStabilityWarmup, .frames = kStabilityFrames, .seed = phaseSeed("stability")});
        break;
    case LabKind::DiBias: {
        LabConfig referenceConfig;
        referenceConfig.isDiReference = true;
        referenceConfig.dlss = run.config.dlss;
        referenceConfig.isJitterEnabled = run.config.isJitterEnabled;
        const std::string key = std::format("di|{}|{}|{}|{}", labCase.name, std::to_underlying(run.config.dlss.quality),
                                            run.config.isJitterEnabled, run.seedIndex);
        if (!references.contains(key)) {
            phases.push_back({.type = PhaseType::DiReference, .view = DebugView::DirectLighting,
                              .config = referenceConfig, .warmup = kStabilityWarmup, .frames = kDiReferenceFrames,
                              .seed = seedOf(key), .referenceKey = key});
        }
        phases.push_back({.type = PhaseType::DiCompare, .view = DebugView::DirectLighting,
                          .config = run.variant == Variant::ReferenceAgain ? referenceConfig : run.config,
                          .warmup = kStabilityWarmup, .frames = kStabilityFrames, .seed = phaseSeed("di"),
                          .referenceKey = key});
        break;
    }
    case LabKind::GiBias: {
        // the GI check's truths: one bounce (ReSTIR GI mode's paths without reuse) and kGiGroundTruthBounces
        const auto truthConfig = [&](IndirectMode mode) {
            LabConfig config;
            config.gi = RtxdiContext::convergedGiSettings();
            config.indirectMode = mode;
            config.isJitterEnabled = false;
            config.dlss = run.config.dlss;
            return config;
        };
        const std::string oneKey =
            std::format("gi1|{}|{}|{}", labCase.name, std::to_underlying(run.config.dlss.quality), run.seedIndex);
        const std::string eightKey =
            std::format("gi8|{}|{}|{}", labCase.name, std::to_underlying(run.config.dlss.quality), run.seedIndex);
        const Phase oneBounce{.type = PhaseType::GiTruth, .view = DebugView::IndirectRadiance,
                              .config = truthConfig(IndirectMode::RestirGi), .isGiReference = true,
                              .giReferenceBounces = 0, .warmup = kGiTruthWarmup, .frames = kGiTruthFrames,
                              .seed = seedOf(oneKey), .referenceKey = oneKey};
        const Phase eightBounces{.type = PhaseType::GiTruth, .view = DebugView::IndirectRadiance,
                                 .config = truthConfig(IndirectMode::NrcRestirGi), .isGiReference = true,
                                 .giReferenceBounces = RtxdiContext::kGiGroundTruthBounces, .warmup = kGiTruthWarmup,
                                 .frames = kGiTruthFrames, .seed = seedOf(eightKey), .referenceKey = eightKey};
        if (!references.contains(oneKey)) {
            phases.push_back(oneBounce);
        }
        if (!references.contains(eightKey)) {
            phases.push_back(eightBounces);
        }
        if (run.variant == Variant::ReferenceAgain) {
            for (Phase again : {oneBounce, eightBounces}) {
                again.type = PhaseType::GiCompare;
                again.frames = kGiFrames;
                again.prefix = again.giReferenceBounces == 0 ? "1b." : "8b.";
                again.seed = phaseSeed(again.prefix);
                phases.push_back(std::move(again));
            }
            break;
        }
        const bool isNrc = run.config.indirectMode != IndirectMode::RestirGi;
        phases.push_back({.type = PhaseType::GiCompare, .view = DebugView::IndirectRadiance, .config = run.config,
                          .warmup = isNrc ? kGiNrcWarmup : kGiWarmup, .frames = kGiFrames, .seed = phaseSeed("gi"),
                          .referenceKey = oneKey, .secondReferenceKey = eightKey});
        break;
    }
    case LabKind::LightChange:
        for (const auto& [prefix, before, after] : {std::tuple{"move.", SunState::Case, SunState::Moved},
                                                    std::tuple{"toggle.", SunState::Off, SunState::Case}}) {
            phases.push_back({.type = PhaseType::LightReference, .view = run.view, .config = run.config,
                              .warmup = kLightReferenceWarmup, .frames = kLightReferenceFrames, .sun = after,
                              .sunAfter = after, .seed = phaseSeed(std::format("{}reference", prefix))});
            phases.push_back({.type = PhaseType::LightChange, .view = run.view, .config = run.config,
                              .warmup = kLightChangeWarmup, .frames = kLightChangeFrames, .changeAt = kLightChangeAt,
                              .sun = before, .sunAfter = after, .seed = phaseSeed(prefix), .prefix = prefix});
        }
        break;
    case LabKind::ScreenEdge:
        for (const auto& [prefix, start] :
             {std::pair{"pan.", PoseState::PanStart}, std::pair{"dolly.", PoseState::DollyStart}}) {
            phases.push_back({.type = PhaseType::Depth, .view = DebugView::Depth, .config = run.config,
                              .frames = 1, .pose = start, .seed = phaseSeed(std::format("{}depth0", prefix))});
            phases.push_back({.type = PhaseType::Depth, .view = DebugView::Depth, .config = run.config,
                              .frames = 1, .pose = PoseState::Case, .seed = phaseSeed(std::format("{}depth1", prefix)),
                              .prefix = prefix});
            phases.push_back({.type = PhaseType::EdgeReference, .view = run.view, .config = run.config,
                              .warmup = kEdgeWarmup, .frames = kEdgeFrames, .changeAt = kEdgeReferenceAt,
                              .seed = phaseSeed(std::format("{}reference", prefix))});
            phases.push_back({.type = PhaseType::EdgeMove, .view = run.view, .config = run.config,
                              .warmup = kEdgeWarmup, .frames = kEdgeFrames, .changeAt = 0, .pose = start,
                              .seed = phaseSeed(prefix), .prefix = prefix});
        }
        break;
    case LabKind::Cost:
    case LabKind::Count:
        break;
    }
}

WorldLights::Sun EngineLab::sunFor(SunState state) const
{
    const LabCase& labCase = kLabCases[runs[*currentRun].caseIndex];
    WorldLights::Sun sun = WorldLights::defaultSun();
    sun.isEnabled = labCase.sun.isEnabled;
    sun.azimuthDegrees = labCase.sun.azimuthDegrees.value_or(sun.azimuthDegrees);
    sun.elevationDegrees = labCase.sun.elevationDegrees.value_or(sun.elevationDegrees);
    if (state == SunState::Moved) {
        sun.azimuthDegrees += kSunMoveDegrees;
    } else if (state == SunState::Off) {
        sun.isEnabled = false;
    }
    return sun;
}

std::optional<EngineLab::PoseView> EngineLab::casePose(const Camera& camera, std::optional<glm::vec4> modelBounds) const
{
    const LabCase& labCase = kLabCases[runs[*currentRun].caseIndex];
    switch (labCase.framing) {
    case LabFraming::Startup:
        return startupPose;
    case LabFraming::Pose:
        return PoseView{labCase.position, labCase.target};
    case LabFraming::Bounds: {
        if (!modelBounds) {
            return std::nullopt;
        }
        // the guide capture's framing: the bounding sphere fills the vertical field of view
        const glm::vec3 center(*modelBounds);
        const float halfFov = glm::radians(camera.getFovDegrees()) * 0.5f;
        const float distance = kFramingMargin * modelBounds->w / std::sin(halfFov);
        return PoseView{center + distance * glm::normalize(labCase.position), center};
    }
    }
    return std::nullopt;
}

EngineLab::PoseView EngineLab::motionStart(const PoseView& pose, PoseState state)
{
    const glm::vec3 forward = glm::normalize(pose.target - pose.position);
    if (state == PoseState::PanStart) {
        const float angle = -glm::radians(kPanDegrees);
        const glm::vec3 turned{forward.x * std::cos(angle) + forward.z * std::sin(angle), forward.y,
                               -forward.x * std::sin(angle) + forward.z * std::cos(angle)};
        return {pose.position, pose.position + turned};
    }
    if (state == PoseState::DollyStart) {
        // ahead of the pose, inside its frustum: clear of geometry, which the space behind a pose need not be
        const glm::vec3 position = pose.position + forward * kDollyMeters;
        return {position, position + forward};
    }
    return pose;
}

void EngineLab::startPhase(Camera& camera, Renderer& renderer, std::optional<glm::vec4> modelBounds)
{
    const Phase& phase = phases[phaseIndex];
    applyLabConfig(phase.config, renderer, camera);
    RtxdiContext& rtxdi = renderer.getRtxdi();
    rtxdi.isGiReference = phase.isGiReference;
    rtxdi.giReferenceBounces = phase.giReferenceBounces;
    renderer.debugView.view = phase.view;
    renderer.getWorldLights().sun = sunFor(phase.sun);
    renderer.getWorldLights().areTriangleLightsSampled = true;
    const std::optional<PoseView> pose = casePose(camera, modelBounds);
    if (!pose) {
        isCaptureMissing = true;
        return;
    }
    const PoseView view = motionStart(*pose, phase.pose);
    camera.lookAt(view.position, view.target);

    PostProcess& post = renderer.getPost();
    post.requestSnap();
    // the move compares against its reference's image at one exposure
    if (phase.type == PhaseType::EdgeMove && phase.view == DebugView::Lit) {
        const float exposure = post.getLastExposure().exposure;
        if (exposure > 0.0f) {
            pinnedExposure = exposure;
        }
    } else {
        pinnedExposure.reset();
    }
    post.settings.isAutoExposure = !pinnedExposure.has_value();
    if (pinnedExposure) {
        post.settings.manualEv = std::log2(*pinnedExposure);
    }
}

EngineLab::Request EngineLab::beginFrame(Camera& camera, Renderer& renderer, std::optional<glm::vec4> modelBounds)
{
    Request request;
    RtxdiContext& rtxdi = renderer.getRtxdi();
    if (isFinished || isPlanDone) {
        rtxdi.frameIndexOverride.reset();
        camera.jitterPhaseOverride.reset();
        return request;
    }
    renderer.fixedFrameDeltaMs = kFrameDeltaMs;
    if (frame < kStartupFrames || settleFrames > 0) {
        return request;
    }
    if (!startupPose) {
        // the startup camera after its first frames: forward is current
        startupPose = PoseView{camera.cameraData.cameraPos, camera.cameraData.cameraPos + camera.getForward()};
    }
    if (!currentRun && !startNextRun()) {
        isPlanDone = true;
        return request;
    }
    const LabCase& labCase = kLabCases[runs[*currentRun].caseIndex];
    if (loadedModel != labCase.model) {
        request.model = repositoryRoot() / labCase.model;
        loadedModel = labCase.model;
        settleFrames = kModelSettleFrames;
        return request;
    }
    if (referencesCase != runs[*currentRun].caseIndex) {
        references.clear();
        referencesCase = runs[*currentRun].caseIndex;
        // the phases were built against the old references
        buildPhases(runs[*currentRun]);
    }
    const Phase& phase = phases[phaseIndex];
    if (phaseFrame == 0) {
        startPhase(camera, renderer, modelBounds);
    }
    if (phase.changeAt != kNoChange && phaseFrame == phase.warmup + phase.changeAt) {
        if (phase.type == PhaseType::LightChange) {
            renderer.getWorldLights().sun = sunFor(phase.sunAfter);
        } else if (phase.type == PhaseType::EdgeMove) {
            if (const std::optional<PoseView> pose = casePose(camera, modelBounds)) {
                camera.lookAt(pose->position, pose->target, false);
            }
        }
    }
    rtxdi.frameIndexOverride = phase.seed + phaseFrame;
    camera.jitterPhaseOverride = phaseFrame;
    isCaptureRequested = phaseFrame >= phase.warmup;
    request.shouldCapture = isCaptureRequested;
    return request;
}

bool EngineLab::endFrame(const Camera& camera, const std::optional<Renderer::DisplayCapture>& capture)
{
    if (isFinished) {
        return true;
    }
    ++frame;
    if (frame <= kStartupFrames) {
        return false;
    }
    if (settleFrames > 0) {
        --settleFrames;
        return false;
    }
    if (isPlanDone) {
        finishPlan();
        isFinished = true;
        return true;
    }
    if (!currentRun) {
        return false;
    }
    if (isCaptureRequested) {
        if (!capture || (capturedFrames > 0 && capture->extent != extent)) {
            isCaptureMissing = true;
        } else {
            accumulate(camera, *capture);
        }
        isCaptureRequested = false;
    }
    const Phase& phase = phases[phaseIndex];
    if (++phaseFrame < phase.warmup + phase.frames) {
        return false;
    }
    finishPhase();
    phaseFrame = 0;
    if (runs[*currentRun].isFailed) {
        phaseIndex = phases.size();
    }
    if (++phaseIndex < phases.size()) {
        return false;
    }
    finishRun();
    currentRun.reset();
    return false;
}

void EngineLab::accumulate(const Camera& camera, const Renderer::DisplayCapture& capture)
{
    const Phase& phase = phases[phaseIndex];
    const DecodedFrame decoded = decodeFrame(capture);
    const size_t texels = decoded.luminance.size();
    if (capturedFrames == 0) {
        extent = capture.extent;
        if (isPngRequested && (phase.type == PhaseType::Stability || phase.type == PhaseType::DiCompare ||
                               phase.type == PhaseType::GiCompare)) {
            writeCheckCapturePng(std::format("{} frame", runLabel(runs[*currentRun])), capture, kLabFolder);
        }
    }
    nonFinitePixels += decoded.nonFinitePixels;
    switch (phase.type) {
    case PhaseType::Stability:
        stability.add(decoded.luminance, capture.extent);
        break;
    case PhaseType::DiReference:
    case PhaseType::DiCompare:
    case PhaseType::GiTruth:
    case PhaseType::GiCompare:
        if (sum.empty()) {
            sum.assign(texels, 0.0);
        }
        for (size_t texel = 0; texel < texels; ++texel) {
            sum[texel] += decoded.luminance[texel];
        }
        for (size_t channel = 0; channel < 3; ++channel) {
            rgbSum[channel] += decoded.rgbTotal[channel];
        }
        break;
    case PhaseType::LightReference:
    case PhaseType::LightChange: {
        double total = 0.0;
        for (const float value : decoded.luminance) {
            total += value;
        }
        frameMeans.push_back(texels > 0 ? total / static_cast<double>(texels) : 0.0);
        break;
    }
    case PhaseType::Depth: {
        labMetrics::DepthMap& depth = phase.pose == PoseState::Case ? finalDepth : initialDepth;
        const GpuCameraData& data = camera.cameraData;
        depth.extent = capture.extent;
        depth.view = data.view;
        depth.invView = data.invView;
        depth.invProj = data.invProj;
        depth.viewProj = data.viewProj;
        depth.viewDepth.resize(texels);
        // inverts debug_view.slang depthShade: 1 - log(z / near) / log(far / near)
        const float depthRange = data.farZ / data.nearZ;
        for (size_t texel = 0; texel < texels; ++texel) {
            depth.viewDepth[texel] = data.nearZ * std::pow(depthRange, 1.0f - decoded.luminance[texel]);
        }
        break;
    }
    case PhaseType::EdgeReference:
        if (capturedFrames < phase.changeAt) {
            if (sum.empty()) {
                sum.assign(texels, 0.0);
            }
            for (size_t texel = 0; texel < texels; ++texel) {
                sum[texel] += decoded.luminance[texel];
            }
            break;
        }
        if (capturedFrames == phase.changeAt) {
            edgeReferenceExtent = extent;
            edgeReference.resize(sum.size());
            for (size_t texel = 0; texel < sum.size(); ++texel) {
                edgeReference[texel] = sum[texel] / phase.changeAt;
            }
        }
        [[fallthrough]];
    case PhaseType::EdgeMove:
        if (masks.empty() || capture.extent != edgeReferenceExtent) {
            isCaptureMissing = true;
        } else {
            edgeFrames.push_back(labMetrics::sumRegions(decoded.luminance, capture.extent, edgeReference, masks,
                                                        finalDepth.extent));
        }
        break;
    }
    ++capturedFrames;
}

void EngineLab::addMetric(std::string_view name, double value)
{
    const LabMetricInfo* info = findLabMetric(name);
    if (info != nullptr && std::isfinite(value)) {
        runs[*currentRun].metrics.push_back({info, value});
    }
}

void EngineLab::failRun(const std::string& text)
{
    Run& run = runs[*currentRun];
    if (!run.isFailed) {
        logError(std::format("FAIL {}: {}", runLabel(run), text), kCategory);
        run.isFailed = true;
        ++failedRuns;
    }
}

void EngineLab::resetAccumulators()
{
    extent = vk::Extent2D{};
    capturedFrames = 0;
    nonFinitePixels = 0;
    isCaptureMissing = false;
    stability = {};
    sum.clear();
    rgbSum = {0.0, 0.0, 0.0};
    frameMeans.clear();
    edgeFrames.clear();
}

void EngineLab::finishPhase()
{
    const Phase& phase = phases[phaseIndex];
    if (isCaptureMissing || capturedFrames == 0) {
        failRun("capture missing, resized, or the case has no camera");
    } else if (nonFinitePixels > 0) {
        failRun(std::format("{} non-finite pixels", nonFinitePixels));
    } else {
        std::vector<double> mean(sum.size());
        for (size_t texel = 0; texel < sum.size(); ++texel) {
            mean[texel] = sum[texel] / capturedFrames;
        }
        switch (phase.type) {
        case PhaseType::Stability:
            finishStability();
            break;
        case PhaseType::DiReference:
        case PhaseType::GiTruth: {
            Reference& reference = references[phase.referenceKey];
            reference.extent = extent;
            reference.luminanceTotal = std::accumulate(mean.begin(), mean.end(), 0.0);
            reference.mean = std::move(mean);
            for (size_t channel = 0; channel < 3; ++channel) {
                reference.rgb[channel] = rgbSum[channel] / capturedFrames;
            }
            break;
        }
        case PhaseType::DiCompare:
        case PhaseType::GiCompare:
            finishCompare(mean);
            break;
        case PhaseType::LightReference: {
            const double total = std::accumulate(frameMeans.begin(), frameMeans.end(), 0.0);
            lightReference = total / static_cast<double>(frameMeans.size());
            lightTolerance = std::max(labMetrics::kSettledError,
                                      labMetrics::kNoiseSigmas *
                                          labMetrics::blockNoise(frameMeans, labMetrics::kBlockFrames, lightReference));
            break;
        }
        case PhaseType::LightChange:
            finishLightChange();
            break;
        case PhaseType::Depth:
            if (phase.pose == PoseState::Case) {
                masks = labMetrics::buildRegionMasks(initialDepth, finalDepth);
                if (masks.empty()) {
                    failRun("screen edge depth captures missing or resized");
                }
            }
            break;
        case PhaseType::EdgeReference:
            finishEdgeReference();
            break;
        case PhaseType::EdgeMove:
            finishEdgeMove();
            break;
        }
    }
    resetAccumulators();
}

void EngineLab::finishStability()
{
    const Phase& phase = phases[phaseIndex];
    const labMetrics::StabilityAccumulator::Result result = stability.finish(phase.view == DebugView::Lit);
    addMetric("mean", result.mean);
    addMetric("std", result.temporalStd);
    addMetric("flicker", result.flicker);
    addMetric("lit", result.lit);
    addMetric("f2f", result.frameChange);
    addMetric("swing", result.largestSwing);
    addMetric("lag1", result.lagCorrelation);
    addMetric("rises", result.rises);
    addMetric("dips", result.dips);
    addMetric("riseSize", result.riseSize);
    addMetric("riseFrames", result.riseFrames);
    addMetric("riseSpread", result.riseSpread);
    if (isPngRequested) {
        const std::string label = runLabel(runs[*currentRun]);
        writeCheckLuminancePng(std::format("{} mean", label), extent, result.means, checkPngScale(result.means),
                               kLabFolder);
        writeCheckUnitPng(std::format("{} cv", label), extent, result.variation, kLabFolder);
        writeCheckUnitPng(std::format("{} rises", label), extent, result.riseMap, kLabFolder);
    }
}

void EngineLab::finishCompare(const std::vector<double>& mean)
{
    const Phase& phase = phases[phaseIndex];
    const auto compareWith = [&](const std::string& key, std::string_view prefix, uint32_t tileSize) {
        const auto found = references.find(key);
        if (found == references.end() || found->second.extent != extent) {
            failRun(std::format("reference {} missing or resized", key));
            return;
        }
        const Reference& reference = found->second;
        const labMetrics::BiasComparison comparison =
            labMetrics::compareToReference(mean, reference.mean, extent, tileSize);
        if (!comparison.isValid) {
            failRun(std::format("reference {} is black", key));
            return;
        }
        const labMetrics::BiasResult& result = comparison.result;
        addMetric(std::format("{}bias", prefix), result.bias);
        addMetric(std::format("{}p50", prefix), result.tileMedian);
        addMetric(std::format("{}p90", prefix), result.tileP90);
        addMetric(std::format("{}darkLeak", prefix), result.darkLeak);
        addMetric(std::format("{}dimBias", prefix), result.dimBias);
        addMetric(std::format("{}hotBias", prefix), result.hotBias);
        if (!prefix.empty()) {
            const std::array<double, 3> meanRgb{rgbSum[0] / capturedFrames, rgbSum[1] / capturedFrames,
                                                rgbSum[2] / capturedFrames};
            const std::array<double, 3> rgbBias = computeChannelBias(meanRgb, reference.rgb, reference.luminanceTotal);
            addMetric(std::format("{}rgbBias", prefix),
                      100.0 * std::max({std::abs(rgbBias[0]), std::abs(rgbBias[1]), std::abs(rgbBias[2])}));
        }
        if (isPngRequested) {
            writeCheckErrorPng(std::format("{} {}error", runLabel(runs[*currentRun]), prefix), extent, mean,
                               reference.mean, checkPngScale(reference.mean), kLabFolder);
        }
    };
    if (phase.type == PhaseType::DiCompare) {
        compareWith(phase.referenceKey, "", kDiTileSize);
    } else if (phase.prefix.empty()) {
        compareWith(phase.referenceKey, "1b.", kGiTileSize);
        compareWith(phase.secondReferenceKey, "8b.", kGiTileSize);
    } else {
        compareWith(phase.referenceKey, phase.prefix, kGiTileSize);
    }
}

void EngineLab::finishLightChange()
{
    const Phase& phase = phases[phaseIndex];
    if (frameMeans.size() <= phase.changeAt) {
        failRun("light change: too few captures");
        return;
    }
    const std::vector<double> after(frameMeans.begin() + phase.changeAt, frameMeans.end());
    const auto frames = static_cast<uint32_t>(after.size());
    const auto trailingError = [&](uint32_t index) {
        const uint32_t first = index + 1 >= labMetrics::kBlockFrames ? index + 1 - labMetrics::kBlockFrames : 0;
        double total = 0.0;
        for (uint32_t i = first; i <= index; ++i) {
            total += after[i];
        }
        const double mean = total / static_cast<double>(index - first + 1);
        return lightReference > 0.0 ? (mean - lightReference) / lightReference : 0.0;
    };
    const uint32_t settled = labMetrics::settledFrames(frames, labMetrics::kBlockFrames, [&](uint32_t start) {
        return std::abs(trailingError(start + labMetrics::kBlockFrames - 1)) <= lightTolerance;
    });
    addMetric(std::format("{}settle", phase.prefix), settled);
    addMetric(std::format("{}err16", phase.prefix), 100.0 * trailingError(std::min(16u, frames) - 1));
}

void EngineLab::finishEdgeReference()
{
    for (size_t region = 0; region < edgeTolerance.size(); ++region) {
        std::vector<double> values(edgeFrames.size());
        labMetrics::RegionSums total;
        for (size_t index = 0; index < edgeFrames.size(); ++index) {
            const labMetrics::RegionSums& sums = edgeFrames[index][region];
            values[index] = sums.value;
            total.value += sums.value;
            total.reference += sums.reference;
            total.absoluteError += sums.absoluteError;
        }
        const double mean = values.empty() ? 0.0 : total.value / static_cast<double>(values.size());
        edgeTolerance[region] = std::max(labMetrics::kSettledError,
                                         labMetrics::kNoiseSigmas *
                                             labMetrics::blockNoise(values, labMetrics::kBlockFrames, mean));
        edgeSteadyError[region] = total.reference > 0.0 ? total.absoluteError / total.reference : 0.0;
    }
}

void EngineLab::finishEdgeMove()
{
    using labMetrics::Region;
    const Phase& phase = phases[phaseIndex];
    const auto frames = static_cast<uint32_t>(edgeFrames.size());
    std::array<size_t, std::to_underlying(Region::Count)> regionPixels{};
    for (const Region region : masks) {
        ++regionPixels[std::to_underlying(region)];
    }
    for (const auto& [region, name] : {std::pair{Region::Band, "band"}, std::pair{Region::Disoccluded, "disoccluded"}}) {
        const size_t index = std::to_underlying(region);
        // a few hundred pixels near black read noise as error
        if (static_cast<double>(regionPixels[index]) < labMetrics::kMinRegionShare * static_cast<double>(masks.size()) ||
            edgeFrames.empty() || edgeFrames.front()[index].reference <= 0.0) {
            continue;
        }
        const uint32_t settled = labMetrics::settledFrames(frames, labMetrics::kBlockFrames, [&](uint32_t start) {
            labMetrics::RegionSums sums;
            for (uint32_t frameIndex = start; frameIndex < std::min(start + labMetrics::kBlockFrames, frames);
                 ++frameIndex) {
                sums.value += edgeFrames[frameIndex][index].value;
                sums.reference += edgeFrames[frameIndex][index].reference;
                sums.absoluteError += edgeFrames[frameIndex][index].absoluteError;
            }
            const double meanError = sums.reference > 0.0 ? std::abs(sums.value - sums.reference) / sums.reference : 0.0;
            const double pixelError = sums.reference > 0.0 ? sums.absoluteError / sums.reference : 0.0;
            return meanError <= edgeTolerance[index] &&
                pixelError <= labMetrics::kSettledPixelRatio * edgeSteadyError[index];
        });
        addMetric(std::format("{}{}", phase.prefix, name), settled);
    }
}

std::string EngineLab::runLabel(const Run& run) const
{
    return std::format("{}, {}, {}, {}, {}, seed {}{}", plan.name, kLabCases[run.caseIndex].name, viewName(run.view),
                       run.configText, kLabKindNames[std::to_underlying(run.kind)], run.seedIndex,
                       run.variant == Variant::ReferenceAgain ? ", reference again" : "");
}

void EngineLab::finishRun()
{
    Run& run = runs[*currentRun];
    run.isDone = true;
    if (run.isFailed) {
        return;
    }
    std::string fields;
    for (const Metric& metric : run.metrics) {
        fields += std::format("{}{} {:.4g}", fields.empty() ? "" : ", ", metric.info->name, metric.value);
    }
    logInfo(std::format("MEASURED {}: {}", runLabel(run), fields), kCategory);
}

double EngineLab::floorOf(const Run& run, const LabMetricInfo* info) const
{
    const Slot slot{run.caseIndex, run.kind, run.view};
    double floor = info->minFloor;
    if (const auto seeds = floorRuns.find(slot); seeds != floorRuns.end()) {
        double low = std::numeric_limits<double>::max();
        double high = std::numeric_limits<double>::lowest();
        for (const size_t index : seeds->second) {
            if (const std::optional<double> value = runs[index].metric(info)) {
                low = std::min(low, *value);
                high = std::max(high, *value);
            }
        }
        if (high >= low) {
            floor = std::max(floor, high - low);
        }
    }
    if (info->isSignedBias) {
        if (const auto again = referenceAgainRuns.find(slot); again != referenceAgainRuns.end()) {
            if (const std::optional<double> value = runs[again->second].metric(info)) {
                floor = std::max(floor, std::abs(*value));
            }
        }
    }
    return floor;
}

std::string EngineLab::effectText(const Run& baseline, const Run& run, const LabMetricInfo* info) const
{
    const std::optional<double> before = baseline.metric(info);
    const std::optional<double> after = run.metric(info);
    if (!before || !after) {
        return {};
    }
    const double delta = *after - *before;
    return std::format("{:+.3g}{} ({:.1f} floors)", delta, unitOf(info), std::abs(delta) / floorOf(baseline, info));
}

void EngineLab::finishPlan()
{
    logRanking();
    logInteractions();
    logBisections();
    logDecisions();
    logTarget();
    writeCsv();
    writeCostScript();
    const auto done = std::ranges::count_if(runs, [](const Run& run) { return run.isDone; });
    const std::string summary = std::format("plan {}: {} runs, {} failed, {} skipped directives; CSV and cost script in "
                                            "logs/lab",
                                            plan.name, done, failedRuns, plan.skipped.size());
    if (failedRuns == 0) {
        logInfo(summary, kCategory);
    } else {
        logError(summary, kCategory);
    }
}

void EngineLab::logRanking() const
{
    // per case, view and ranked metric: every comparison by effect in noise floors
    struct Effect
    {
        std::string label;
        double delta;
        double floors;
    };
    std::map<std::tuple<size_t, DebugView, const LabMetricInfo*>, std::vector<Effect>> effects;
    for (const Comparison& comparison : comparisons) {
        const Run& baseline = runs[comparison.baseline];
        const Run& run = runs[comparison.run];
        for (const Metric& metric : run.metrics) {
            const std::optional<double> before = baseline.metric(metric.info);
            if (!metric.info->isRanked || !before) {
                continue;
            }
            const double delta = metric.value - *before;
            effects[{run.caseIndex, run.view, metric.info}].push_back(
                {comparison.label, delta, std::abs(delta) / floorOf(baseline, metric.info)});
        }
    }
    for (auto& [key, list] : effects) {
        const auto& [caseIndex, view, info] = key;
        std::ranges::sort(list, std::greater{}, &Effect::floors);
        std::string ranked;
        std::vector<std::string> noEffect;
        for (const Effect& effect : list) {
            if (effect.floors >= kEffectFloors) {
                ranked += std::format("{}{} {:+.3g}{} ({:.1f} floors)", ranked.empty() ? "" : ", ", effect.label,
                                      effect.delta, unitOf(info), effect.floors);
            } else if (!std::ranges::contains(noEffect, effect.label)) {
                noEffect.push_back(effect.label);
            }
        }
        std::string none;
        for (const std::string& label : noEffect) {
            none += none.empty() ? label : "; " + label;
        }
        logInfo(std::format("largest effects on {} {} {}: {}{}", kLabCases[caseIndex].name, viewName(view), info->name,
                            ranked.empty() ? std::string("none") : ranked,
                            none.empty() ? std::string() : std::format(" | no effect: {}", none)),
                kCategory);
    }
}

void EngineLab::logInteractions() const
{
    // a switch with an effect in one direction only needs another switch to matter (spec §6.3)
    for (const OfatRecord& record : ofatRecords) {
        const Run& forward = runs[record.forward];
        const Run& base = runs[record.base];
        const Run& backward = runs[record.backward];
        const Run& target = runs[record.target];
        for (const Metric& metric : forward.metrics) {
            const std::optional<double> baseValue = base.metric(metric.info);
            const std::optional<double> backwardValue = backward.metric(metric.info);
            const std::optional<double> targetValue = target.metric(metric.info);
            if (!metric.info->isRanked || !baseValue || !backwardValue || !targetValue) {
                continue;
            }
            const double floor = floorOf(base, metric.info);
            const bool isForward = std::abs(metric.value - *baseValue) >= kEffectFloors * floor;
            const bool isBackward = std::abs(*targetValue - *backwardValue) >= kEffectFloors * floor;
            if (isForward != isBackward) {
                logInfo(std::format("interaction candidate on {} {} {}: {} has an effect {} only (from base {:+.3g}, in "
                                    "target {:+.3g}, floor {:.3g})",
                                    kLabCases[base.caseIndex].name, viewName(base.view), metric.info->name,
                                    record.switchName, isForward ? "from the base" : "in the target",
                                    metric.value - *baseValue, *targetValue - *backwardValue, floor),
                        kCategory);
            }
        }
    }
    // |Δ(A+B) − Δ(A) − Δ(B)| above the floor: the pair interacts
    for (const PairRecord& record : pairRecords) {
        const Run& base = runs[record.base];
        for (const Metric& metric : runs[record.both].metrics) {
            const std::optional<double> baseValue = base.metric(metric.info);
            const std::optional<double> first = runs[record.first].metric(metric.info);
            const std::optional<double> second = runs[record.second].metric(metric.info);
            if (!metric.info->isRanked || !baseValue || !first || !second) {
                continue;
            }
            const double nonAdditive = std::abs((metric.value - *baseValue) - (*first - *baseValue) -
                                                (*second - *baseValue));
            const double floor = floorOf(base, metric.info);
            logInfo(std::format("pair {} on {} {} {}: non-additivity {:.3g}{} ({:.1f} floors){}", record.label,
                                kLabCases[base.caseIndex].name, viewName(base.view), metric.info->name, nonAdditive,
                                unitOf(metric.info), nonAdditive / floor,
                                nonAdditive >= kEffectFloors * floor ? ": interacting" : ""),
                    kCategory);
        }
    }
}

void EngineLab::logBisections() const
{
    for (const Bisection& bisection : bisections) {
        logInfo(std::format("bisect {} {}:{}: {}", kLabCases[bisection.caseIndex].name, viewName(bisection.key.view),
                            bisection.key.metric->name, bisection.isDone ? bisection.outcome : "not finished"),
                kCategory);
    }
}

void EngineLab::logDecisions() const
{
    // spec §6.6: per lever and case, every ranked metric's change in noise floors; worth shipping when Lit std or
    // flicker drop by more than kEffectFloors on a café case and no bias or lag rises by more than kEffectFloors
    std::vector<std::string> labels;
    for (const LeverRecord& record : leverRecords) {
        if (!std::ranges::contains(labels, record.label)) {
            labels.push_back(record.label);
        }
    }
    if (leverRecords.empty()) {
        return;
    }
    std::ofstream csv(labDirectory() / std::format("{}_decisions.csv", plan.name));
    csv << "lever,case,view,metric,over,delta,floors\n";
    for (const std::string& label : labels) {
        bool isCafeGain = false;
        std::vector<std::string> regressions;
        std::map<size_t, std::string> lines;
        for (const LeverRecord& record : leverRecords) {
            if (record.label != label) {
                continue;
            }
            const Run& reference = runs[record.reference];
            const Run& run = runs[record.run];
            const bool isCafe = std::string_view(kLabCases[run.caseIndex].name).starts_with("cafe.");
            for (const Metric& metric : run.metrics) {
                const std::optional<double> before = reference.metric(metric.info);
                if (!metric.info->isRanked || !before) {
                    continue;
                }
                const double delta = metric.value - *before;
                const double floors = delta / floorOf(reference, metric.info);
                std::string& line = lines[run.caseIndex];
                line += std::format("{}{} {} {:+.3g}{} ({:+.1f})", line.empty() ? "" : ", ", viewName(run.view),
                                    metric.info->name, delta, unitOf(metric.info), floors);
                csv << std::format("{},{},{},{},fix,{},{}\n", csvField(label), kLabCases[run.caseIndex].name,
                                   csvField(viewName(run.view)), metric.info->name, delta, floors);
                const std::string_view name(metric.info->name);
                if (isCafe && run.view == DebugView::Lit && (name == "std" || name == "flicker") &&
                    floors <= -kEffectFloors) {
                    isCafeGain = true;
                }
                const bool isBias = metric.info->isSignedBias && std::abs(floors) > kEffectFloors;
                const bool isLag = name.ends_with("settle") && floors > kEffectFloors;
                if (isBias || isLag) {
                    regressions.push_back(std::format("{} {} {}", kLabCases[run.caseIndex].name, viewName(run.view),
                                                      metric.info->name));
                }
            }
            if (record.base && record.onBase) {
                for (const Metric& metric : runs[*record.onBase].metrics) {
                    const std::optional<double> before = runs[*record.base].metric(metric.info);
                    if (metric.info->isRanked && before) {
                        const double delta = metric.value - *before;
                        csv << std::format("{},{},{},{},base,{},{}\n", csvField(label), kLabCases[run.caseIndex].name,
                                           csvField(viewName(run.view)), metric.info->name, delta,
                                           delta / floorOf(runs[*record.base], metric.info));
                    }
                }
            }
        }
        for (const auto& [caseIndex, line] : lines) {
            logInfo(std::format("decision {} on {} (over its reference, floors in parentheses): {}", label,
                                kLabCases[caseIndex].name, line),
                    kCategory);
        }
        const std::string verdict = !regressions.empty()
            ? std::format("regresses {}", [&] {
                  std::string text;
                  for (const std::string& regression : regressions) {
                      text += text.empty() ? regression : "; " + regression;
                  }
                  return text;
              }())
            : isCafeGain ? std::string("worth shipping (cost: run the bench script)")
                         : std::string("no café gain above the floor");
        logInfo(std::format("decision {}: {}", label, verdict), kCategory);
    }
}

void EngineLab::logTarget() const
{
    // spec §6.6: the café against the stable scenes' Lit std; I12 sets the bar from these
    const LabMetricInfo* info = findLabMetric("std");
    std::string text;
    for (const char* name : {"cafe.tables", "cafe.dining", "sponza.courtyard", "sponza.gallery"}) {
        const size_t caseIndex = labCaseIndex(*findLabCase(name));
        const auto seeds = floorRuns.find(Slot{caseIndex, LabKind::Stability, DebugView::Lit});
        if (seeds == floorRuns.end()) {
            continue;
        }
        const std::optional<double> base = runs[seeds->second.front()].metric(info);
        std::string fixText;
        if (plan.fix) {
            for (const Run& run : runs) {
                if (run.caseIndex == caseIndex && run.kind == LabKind::Stability && run.view == DebugView::Lit &&
                    run.seedIndex == 0 && run.variant == Variant::Config && isSameConfig(run.config, *plan.fix)) {
                    if (const std::optional<double> value = run.metric(info)) {
                        fixText = std::format(", fix {:.2f}%", *value);
                    }
                }
            }
        }
        if (base) {
            text += std::format("{}{} base {:.2f}%{}", text.empty() ? "" : "; ", name, *base, fixText);
        }
    }
    if (!text.empty()) {
        logInfo(std::format("target: Lit temporal std {}", text), kCategory);
    }
}

void EngineLab::writeCsv() const
{
    std::ofstream csv(labDirectory() / std::format("{}.csv", plan.name));
    csv << "plan,case,kind,view,seed,variant,failed,config";
    for (const LabMetricInfo& metric : kLabMetrics) {
        csv << std::format(",{},{} floor", metric.name, metric.name);
    }
    csv << '\n';
    for (const Run& run : runs) {
        if (!run.isDone) {
            continue;
        }
        csv << std::format("{},{},{},{},{},{},{},{}", csvField(plan.name), kLabCases[run.caseIndex].name,
                           kLabKindNames[std::to_underlying(run.kind)], csvField(viewName(run.view)), run.seedIndex,
                           run.variant == Variant::ReferenceAgain ? "reference again" : "config", run.isFailed ? 1 : 0,
                           csvField(run.configText));
        for (const LabMetricInfo& metric : kLabMetrics) {
            if (const std::optional<double> value = run.metric(&metric)) {
                csv << std::format(",{},{}", *value, floorOf(run, &metric));
            } else {
                csv << ",,";
            }
        }
        csv << '\n';
    }
}

void EngineLab::writeCostScript() const
{
    if (costRequests.empty()) {
        return;
    }
    // spec §2: cost runs are bench-mode runs on their own, without layers, never mixed with image runs; ABBA per
    // config against the base (I4)
    std::ofstream script(labDirectory() / std::format("{}_cost.ps1", plan.name));
    script << std::format("# ENGINE_LAB={} cost runs: bench mode, validation layers off, ABBA against the base.\n"
                          "# Run from anywhere; each run's engine.log is kept next to this script\n",
                          plan.name);
    script << "$ErrorActionPreference = 'Stop'\n"
              "$root = Resolve-Path \"$PSScriptRoot\\..\\..\"\n"
              "$env:VK_LOADER_LAYERS_DISABLE = '*validation*'\n"
              "function Invoke-Bench([string]$name, [string]$config, [string]$model, [string]$pose) {\n"
              "    $env:ENGINE_CONFIG = $config\n";
    script << std::format("    $env:ENGINE_BENCH_FRAMES = '{}'\n", kBenchFrames);
    script << "    $env:ENGINE_BENCH_MODEL = $model\n"
              "    $env:ENGINE_BENCH_POSE = $pose\n"
              "    & \"$root\\engine.exe\" | Out-Null\n";
    script << std::format("    Copy-Item \"$root\\logs\\engine.log\" \"$PSScriptRoot\\{}_cost_$name.log\"\n", plan.name);
    script << "}\n";
    uint32_t index = 0;
    for (const auto& [caseIndex, config] : costRequests) {
        const LabCase& labCase = kLabCases[caseIndex];
        // bench mode knows the startup camera and the courtyard pose only
        const bool isCourtyard = std::string_view(labCase.name) == "sponza.courtyard";
        if (labCase.framing != LabFraming::Startup && !isCourtyard) {
            script << std::format("# {}: no bench pose for this case\n", labCase.name);
            continue;
        }
        if (isSameConfig(config, plan.base)) {
            continue;
        }
        const std::string model = (repositoryRoot() / labCase.model).string();
        const std::string base = formatLabConfig(plan.base);
        const std::string text = formatLabConfig(config);
        for (const auto& [suffix, runConfig] : {std::pair{"A1", base}, std::pair{"B1", text}, std::pair{"B2", text},
                                                std::pair{"A2", base}}) {
            script << std::format("Invoke-Bench '{:03}_{}_{}' '{}' '{}' '{}'\n", index, labCase.name, suffix,
                                  runConfig, model, isCourtyard ? "courtyard" : "");
        }
        ++index;
    }
}
