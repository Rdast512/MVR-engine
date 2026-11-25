#include "lab_plan.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <sstream>
#include <utility>

namespace
{
    // spec/features/planned/full_testing.md §6.1; shipped and NVIDIA differ in 6 DI and 9 GI switches (§6.4)
    constexpr std::string_view kSetsPlan = R"(# shipped, NVIDIA and converged on every case
base di=shipped gi=shipped
seeds 3
cases sponza.gallery sponza.courtyard bistro.street cafe.tables cafe.dining
views Lit "Direct lighting" "Indirect radiance"
kinds stability diBias giBias
sets di=nvidia gi=nvidia
sets di=converged gi=converged
cases guide.lamp guide.terminator guide.emissive
kinds diBias
sets di=nvidia gi=nvidia
sets di=converged gi=converged
)";

    constexpr std::string_view kDiOfatPlan = R"(# every DI switch where shipped and NVIDIA differ, alone, both directions
base di=shipped gi=shipped
target di=nvidia gi=shipped
seeds 3
cases cafe.tables cafe.dining
views Lit "Direct lighting"
kinds stability diBias
ofat
)";

    constexpr std::string_view kGiOfatPlan = R"(# every GI switch where shipped and NVIDIA differ, alone, both directions; then
# the smallest switch set behind the dining room's dim-tile GI bias against eight bounces (F23; the shipped mode
# is NRC + ReSTIR GI since I15)
base di=shipped gi=shipped
target di=shipped gi=nvidia
seeds 3
cases cafe.tables cafe.dining sponza.gallery
views Lit "Indirect radiance"
kinds stability giBias
ofat
cases cafe.dining
bisect "Indirect radiance:8b.dimBias"
)";

    // the café steps of the I12 investigation (EngineTemporalCheck's café list), so T12 can move them out of the check
    constexpr std::string_view kCafePlan = R"(# what the café work ran by hand (F22, F23), mirrored around the shipped base: it
# carries F22's DI fix (spatial I15, permutation I16), I15's GI age and NRC + ReSTIR GI
base di=shipped gi=shipped
seeds 3
cases cafe.tables cafe.dining
views "Direct lighting" Lit
kinds stability diBias
sets di=nvidia
sets di=converged
sets di.spatialSamples=0
sets di.permutation=off
sets di.sampling=regir
sets di.spatialSamples=0 di.permutation=off
views Lit
kinds stability
sets di=nvidia gi=nvidia
sets di.spatialSamples=0 di.permutation=off gi.decorrelation=none
sets di.spatialSamples=0 di.permutation=off gi=nvidia
sets di.spatialSamples=0 di.permutation=off frame.indirectMode=restirGi
sets di.spatialSamples=0 di.permutation=off frame.indirectMode=nrc
views Lit "Indirect radiance"
sets gi.decorrelation=none
sets gi.fireflyReplacement=off
sets gi.age=1024
sets gi=nvidia
sets gi=converged
sets frame.indirectMode=restirGi
sets frame.indirectMode=nrc
kinds giBias
sets gi=nvidia
sets gi=converged
sets frame.indirectMode=restirGi
sets frame.indirectMode=nrc
)";

    constexpr std::string_view kLeakPlan = R"(# dim and dark leak where a bright exterior meets a dark interior
base di=shipped gi=shipped
target di=nvidia gi=nvidia
seeds 3
cases cafe.door guide.terminator
kinds diBias giBias
sets di=reference
ofat
)";

    // spec §6.6: every lever over the F22 fix and on the shipped base; sweeps over the fix
    constexpr std::string_view kStabilityLeversPlan = R"(# F22 candidates: what makes the café stable enough not to notice
base di=shipped gi=shipped
seeds 3
# the F22 fix: shipped since I15 (spatial) and I16 (permutation), so over fix and on base read the same
fix di.spatialSamples=2 di.permutation=on
cases cafe.tables cafe.dining sponza.gallery sponza.courtyard bistro.street guide.lamp guide.terminator guide.emissive glossy.floor glossy.metal
views Lit "Direct lighting" "Indirect radiance"
kinds stability diBias giBias cost
# L1 DI spatial reuse and permutation
lever L1 no spatial: di.spatialSamples=0
lever L1 no permutation: di.permutation=off
lever L1 neither: di.spatialSamples=0 di.permutation=off
sweep di.spatialSamples 0 2 4 over fix lever L1
sweep di.radius 4 8 16 32 over fix lever L1
sweep gi.radius 4 8 16 32 over fix lever L1
# L2 ReGIR with spatial reuse
lever L2 regir: di.sampling=regir di.spatialSamples=2
lever L2 regir no permutation: di.sampling=regir di.spatialSamples=2 di.permutation=off
# L3 boiling filters and firefly clamps
sweep di.boilingStrength 0.1 0.2 0.35 0.5 over fix di.boilingFilter=on lever L3
lever L3 gi boiling: gi.boilingFilter=on
sweep gi.boilingStrength 0.1 0.2 0.5 over fix gi.boilingFilter=on lever L3
sweep gi.fireflyStrength 0.7 0.9 over fix lever L3
lever L3 firefly bound: gi.fireflyBound=on
lever L3 firefly clamp: gi.fireflyClamp=on
# L4 history length, both ways
sweep di.history 8 12 20 40 60 over fix lever L4
sweep gi.history 4 8 12 20 40 60 over fix lever L4
# L5 world cache for the GI tail (shipped since I15), against one bounce
lever L5 restir gi: frame.indirectMode=restirGi
lever L5 nrc: frame.indirectMode=nrc
# L6 DLSS-RR model, resolution and guides
lever L6 preset d: frame.dlssPreset=d
lever L6 preset e: frame.dlssPreset=e
lever L6 dlaa: frame.dlssQuality=dlaa
lever L6 specular guides: frame.specularGuides=on
# L7 shaping before the denoiser
lever L7 target compression: gi.targetCompression=4
lever L7 pre-denoiser clamp: frame.preDenoiserClamp=8
lever L7 firefly clamp: gi.fireflyClamp=on
# L4 and L4c lag: sun move and toggle on Sponza, the emissive toggle in the café
cases sponza.gallery sponza.courtyard cafe.toggle
views "Indirect radiance" "Direct lighting"
kinds lightChange
sweep di.history 8 12 20 40 60 over fix lever L4
sweep gi.history 4 8 12 20 40 60 over fix lever L4
sweep di.history 8 12 20 40 60 over fix frame.historyConfidence=on lever L4c
sweep gi.history 4 8 12 20 40 60 over fix frame.historyConfidence=on lever L4c
# the fix plus each lever group's best, on every case above
cases cafe.tables cafe.dining sponza.gallery sponza.courtyard bistro.street guide.lamp guide.terminator guide.emissive
views Lit "Direct lighting" "Indirect radiance"
kinds stability diBias giBias cost
stack Lit:std
)";

    constexpr std::array<std::pair<std::string_view, std::string_view>, 6> kBuiltInPlans = {{
        {"sets", kSetsPlan},
        {"di-ofat", kDiOfatPlan},
        {"gi-ofat", kGiOfatPlan},
        {"cafe", kCafePlan},
        {"leak", kLeakPlan},
        {"stability-levers", kStabilityLeversPlan},
    }};

    bool equalsIgnoreCase(std::string_view left, std::string_view right)
    {
        return std::ranges::equal(left, right, [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        });
    }

    // whitespace-separated, double quotes keep spaces, '#' outside quotes ends the line
    std::vector<std::string> tokenize(std::string_view line)
    {
        std::vector<std::string> tokens;
        std::string current;
        bool isQuoted = false;
        bool hasToken = false;
        for (const char c : line) {
            if (c == '"') {
                isQuoted = !isQuoted;
                hasToken = true;
            } else if (!isQuoted && c == '#') {
                break;
            } else if (!isQuoted && std::isspace(static_cast<unsigned char>(c))) {
                if (hasToken) {
                    tokens.push_back(std::move(current));
                    current.clear();
                    hasToken = false;
                }
            } else {
                current += c;
                hasToken = true;
            }
        }
        if (hasToken) {
            tokens.push_back(std::move(current));
        }
        return tokens;
    }

    std::string join(std::span<const std::string> tokens)
    {
        std::string text;
        for (const std::string& token : tokens) {
            text += text.empty() ? token : " " + token;
        }
        return text;
    }

    class PlanParser
    {
    public:
        explicit PlanParser(std::string_view name) { plan.name = name; }

        bool parseLine(size_t lineNumber, std::string_view line)
        {
            line_ = lineNumber;
            const std::vector<std::string> tokens = tokenize(line);
            if (tokens.empty()) {
                return true;
            }
            const std::string_view directive = tokens.front();
            const std::span<const std::string> args(tokens.begin() + 1, tokens.end());
            if (directive == "cases") {
                return parseCases(args);
            }
            if (directive == "views") {
                return parseViews(args);
            }
            if (directive == "kinds") {
                return parseKinds(args);
            }
            if (directive == "base") {
                return parseConfigInto(args, LabConfig{}, plan.base, "base");
            }
            if (directive == "target") {
                LabConfig target;
                if (!parseConfigInto(args, LabConfig{}, target, "target")) {
                    return false;
                }
                plan.target = target;
                return true;
            }
            if (directive == "fix") {
                LabConfig fix;
                if (!parseConfigInto(args, plan.base, fix, "fix")) {
                    return false;
                }
                plan.fix = fix;
                return true;
            }
            if (directive == "seeds") {
                uint32_t seeds = 0;
                if (args.size() != 1 ||
                    std::from_chars(args[0].data(), args[0].data() + args[0].size(), seeds).ec != std::errc() ||
                    seeds == 0 || seeds > kMaxSeeds) {
                    return fail(std::format("seeds takes one number in 1..{}", kMaxSeeds));
                }
                plan.seeds = seeds;
                return true;
            }
            if (directive == "sets") {
                return parseSets(args);
            }
            if (directive == "ofat") {
                if (!plan.target) {
                    return fail("ofat needs a target line above it");
                }
                return addEntry({.type = LabEntryType::Ofat});
            }
            if (directive == "sweep") {
                return parseSweep(args);
            }
            if (directive == "pairs") {
                return parsePairs(args);
            }
            if (directive == "lever") {
                return parseLever(args);
            }
            if (directive == "bisect" || directive == "stack") {
                return parseMetricDirective(directive == "bisect" ? LabEntryType::Bisect : LabEntryType::Stack, args);
            }
            return fail(std::format("'{}' is not a directive", directive));
        }

        LabPlanParse finish() &&
        {
            if (!error.empty()) {
                return {std::nullopt, std::move(error)};
            }
            return {std::move(plan), {}};
        }

        [[nodiscard]] bool hasFailed() const { return !error.empty(); }

    private:
        static constexpr uint32_t kMaxSeeds = 16;

        bool fail(std::string text)
        {
            error = std::format("plan {} line {}: {}", plan.name, line_, text);
            return false;
        }

        void skip(std::string text) { plan.skipped.push_back(std::format("line {}: {}", line_, text)); }

        bool addEntry(LabEntry entry)
        {
            entry.scope = scope;
            entry.line = line_;
            if (scope.cases.empty()) {
                return fail("no runnable case above this line (cases ...)");
            }
            plan.entries.push_back(std::move(entry));
            return true;
        }

        bool parseCases(std::span<const std::string> args)
        {
            scope.cases.clear();
            for (const std::string& name : args) {
                const LabCase* labCase = findLabCase(name);
                if (labCase == nullptr) {
                    return fail(std::format("'{}' is not a lab case", name));
                }
                if (labCase->notBuilt != nullptr) {
                    skip(std::format("case {} is planned: {}", name, labCase->notBuilt));
                    continue;
                }
                scope.cases.push_back(labCaseIndex(*labCase));
            }
            return true;
        }

        bool parseViews(std::span<const std::string> args)
        {
            scope.views.clear();
            for (const std::string& name : args) {
                const std::optional<DebugView> view = findDebugView(name);
                if (!view) {
                    return fail(std::format("'{}' is not a debug view", name));
                }
                scope.views.push_back(*view);
            }
            return !scope.views.empty() || fail("views needs at least one view");
        }

        bool parseKinds(std::span<const std::string> args)
        {
            scope.kinds = 0;
            for (const std::string& name : args) {
                const auto found = std::ranges::find_if(kLabKindNames, [&](const char* kind) { return name == kind; });
                if (found == kLabKindNames.end()) {
                    return fail(std::format("'{}' is not a kind ({})", name, join(std::vector<std::string>(
                                                                                     kLabKindNames.begin(),
                                                                                     kLabKindNames.end()))));
                }
                scope.kinds |= 1u << static_cast<uint32_t>(found - kLabKindNames.begin());
            }
            return scope.kinds != 0 || fail("kinds needs at least one kind");
        }

        // false with a planned switch: the caller skips the directive
        std::optional<LabConfig> parseConfig(std::string_view text, const LabConfig& over, bool& isPlanned)
        {
            const LabConfigParse parse = parseLabConfig(text, over);
            isPlanned = parse.isPlanned;
            if (parse.isPlanned) {
                skip(parse.error);
                return std::nullopt;
            }
            if (!parse.config) {
                fail(parse.error);
                return std::nullopt;
            }
            return parse.config;
        }

        bool parseConfigInto(std::span<const std::string> args, const LabConfig& over, LabConfig& out, const char* what)
        {
            bool isPlanned = false;
            const std::optional<LabConfig> config = parseConfig(join(args), over, isPlanned);
            if (isPlanned) {
                return fail(std::format("{} names a planned switch", what));
            }
            if (!config) {
                return false;
            }
            out = *config;
            return true;
        }

        bool parseSets(std::span<const std::string> args)
        {
            const std::string text = join(args);
            bool isPlanned = false;
            const std::optional<LabConfig> config = parseConfig(text, LabConfig{}, isPlanned);
            if (!config) {
                return isPlanned;
            }
            return addEntry({.type = LabEntryType::Set, .label = text, .config = *config});
        }

        // `over [fix] overrides...`: the config a sweep starts from; nullopt on error or a planned switch
        std::optional<LabConfig> parseOver(std::span<const std::string> args, std::string& label, bool& isPlanned)
        {
            LabConfig start = plan.base;
            std::span<const std::string> overrides = args;
            if (!overrides.empty() && overrides.front() == "fix") {
                if (!plan.fix) {
                    fail("'over fix' needs a fix line above it");
                    return std::nullopt;
                }
                start = *plan.fix;
                overrides = overrides.subspan(1);
            }
            label = join(args);
            return parseConfig(join(overrides), start, isPlanned);
        }

        bool parseSweep(std::span<const std::string> args)
        {
            if (args.size() < 2) {
                return fail("sweep takes a switch and its values");
            }
            const LabSwitch* entry = findLabSwitch(args[0]);
            if (entry == nullptr) {
                return fail(std::format("'{}' is not a lab switch", args[0]));
            }
            if (entry->notBuilt != nullptr) {
                skip(std::format("sweep of {}: planned ({})", entry->name, entry->notBuilt));
                return true;
            }
            const auto overAt = std::ranges::find(args, "over");
            const auto leverAt = std::ranges::find(args, "lever");
            const auto valuesEnd = std::min(overAt, leverAt);
            LabEntry sweep{.type = LabEntryType::Sweep, .config = plan.base, .sweepSwitch = entry};
            for (auto value = args.begin() + 1; value != valuesEnd; ++value) {
                const std::optional<double> parsed = parseSwitchValue(*entry, *value);
                if (!parsed) {
                    return fail(std::format("'{}' is not a value of {}", *value, entry->name));
                }
                sweep.sweepValues.push_back(*parsed);
            }
            if (sweep.sweepValues.empty()) {
                return fail("sweep needs at least one value");
            }
            if (overAt != args.end()) {
                const auto overEnd = leverAt > overAt ? leverAt : args.end();
                bool isPlanned = false;
                const std::optional<LabConfig> start = parseOver(
                    std::span<const std::string>(overAt + 1, overEnd), sweep.label, isPlanned);
                if (!start) {
                    return isPlanned;
                }
                sweep.config = *start;
            }
            if (leverAt != args.end()) {
                if (leverAt + 1 == args.end()) {
                    return fail("lever needs a group name");
                }
                sweep.group = *(leverAt + 1);
            }
            return addEntry(std::move(sweep));
        }

        std::optional<LabSwitchValue> parsePairSwitch(std::string_view text)
        {
            const size_t equals = text.find('=');
            const LabSwitch* entry = findLabSwitch(text.substr(0, equals));
            if (entry == nullptr) {
                fail(std::format("'{}' is not a lab switch", text.substr(0, equals)));
                return std::nullopt;
            }
            if (entry->notBuilt != nullptr) {
                fail(std::format("{} is planned ({})", entry->name, entry->notBuilt));
                return std::nullopt;
            }
            if (equals != std::string_view::npos) {
                const std::optional<double> value = parseSwitchValue(*entry, text.substr(equals + 1));
                if (!value) {
                    fail(std::format("'{}' is not a value of {}", text.substr(equals + 1), entry->name));
                    return std::nullopt;
                }
                return LabSwitchValue{entry, *value};
            }
            if (!plan.target) {
                fail("pairs without values take them from a target line above it");
                return std::nullopt;
            }
            return LabSwitchValue{entry, entry->get(*plan.target)};
        }

        bool parsePairs(std::span<const std::string> args)
        {
            if (args.size() != 2) {
                return fail("pairs takes two switches (name or name=value)");
            }
            LabEntry pairs{.type = LabEntryType::Pairs};
            for (size_t index = 0; index < 2; ++index) {
                pairs.pair[index] = parsePairSwitch(args[index]);
                if (!pairs.pair[index]) {
                    return false;
                }
            }
            return addEntry(std::move(pairs));
        }

        bool parseLever(std::span<const std::string> args)
        {
            const auto labelEnd = std::ranges::find_if(args, [](const std::string& token) { return token.ends_with(':'); });
            if (labelEnd == args.end()) {
                return fail("lever takes 'lever <group> <name>: <overrides>'");
            }
            std::vector<std::string> labelTokens(args.begin(), labelEnd + 1);
            labelTokens.back().pop_back();
            std::erase_if(labelTokens, [](const std::string& token) { return token.empty(); });
            if (labelTokens.empty()) {
                return fail("lever needs a group name");
            }
            if (!plan.fix) {
                return fail("lever needs a fix line above it");
            }
            const std::string overrides = join(std::span<const std::string>(labelEnd + 1, args.end()));
            bool isPlanned = false;
            const std::optional<LabConfig> overFix = parseConfig(overrides, *plan.fix, isPlanned);
            if (!overFix) {
                return isPlanned;
            }
            const std::optional<LabConfig> onBase = parseConfig(overrides, plan.base, isPlanned);
            if (!onBase) {
                return isPlanned;
            }
            return addEntry({.type = LabEntryType::Lever,
                             .label = join(labelTokens),
                             .group = labelTokens.front(),
                             .config = *overFix,
                             .onBase = *onBase});
        }

        bool parseMetricDirective(LabEntryType type, std::span<const std::string> args)
        {
            if (args.size() != 1) {
                return fail("takes one metric, view:metric (\"Indirect radiance:1b.dimBias\")");
            }
            const size_t colon = args[0].rfind(':');
            if (colon == std::string::npos) {
                return fail(std::format("'{}' is not view:metric", args[0]));
            }
            const std::optional<DebugView> view = findDebugView(std::string_view(args[0]).substr(0, colon));
            const LabMetricInfo* metric = findLabMetric(std::string_view(args[0]).substr(colon + 1));
            if (!view || metric == nullptr) {
                return fail(std::format("'{}' names no view or no metric", args[0]));
            }
            if (const std::optional<DebugView> fixed = fixedViewOf(metric->kind); fixed && *fixed != *view) {
                return fail(std::format("{} reads {}", metric->name,
                                        kDebugViews[std::to_underlying(*fixed)].name));
            }
            if (type == LabEntryType::Bisect && !plan.target) {
                return fail("bisect needs a target line above it");
            }
            if (type == LabEntryType::Stack && !plan.fix) {
                return fail("stack needs a fix line above it");
            }
            LabScope metricScope = scope;
            metricScope.kinds |= labKindBit(metric->kind);
            LabEntry entry{.type = type, .metric = LabMetricKey{*view, metric}};
            const LabScope saved = scope;
            scope = metricScope;
            const bool isAdded = addEntry(std::move(entry));
            scope = saved;
            return isAdded;
        }

        LabPlan plan;
        LabScope scope;
        size_t line_ = 0;
        std::string error;
    };
} // namespace

const LabMetricInfo* findLabMetric(std::string_view name)
{
    for (const LabMetricInfo& metric : kLabMetrics) {
        if (name == metric.name) {
            return &metric;
        }
    }
    return nullptr;
}

std::optional<DebugView> findDebugView(std::string_view name)
{
    for (size_t index = 0; index < kDebugViews.size(); ++index) {
        if (equalsIgnoreCase(name, kDebugViews[index].name)) {
            return static_cast<DebugView>(index);
        }
    }
    return std::nullopt;
}

std::optional<DebugView> fixedViewOf(LabKind kind)
{
    switch (kind) {
    case LabKind::DiBias:
        return DebugView::DirectLighting;
    case LabKind::GiBias:
        return DebugView::IndirectRadiance;
    default:
        return std::nullopt;
    }
}

std::optional<std::string_view> builtInLabPlan(std::string_view name)
{
    for (const auto& [planName, text] : kBuiltInPlans) {
        if (name == planName) {
            return text;
        }
    }
    return std::nullopt;
}

LabPlanParse parseLabPlan(std::string_view name, std::string_view text)
{
    PlanParser parser(name);
    size_t lineNumber = 0;
    for (const auto line : std::views::split(text, '\n')) {
        if (!parser.parseLine(++lineNumber, std::string_view(line.begin(), line.end())) || parser.hasFailed()) {
            break;
        }
    }
    return std::move(parser).finish();
}

LabPlanParse loadLabPlan(std::string_view name)
{
    if (const std::optional<std::string_view> text = builtInLabPlan(name)) {
        return parseLabPlan(name, *text);
    }
    const std::filesystem::path directory = std::filesystem::path(ENGINE_MODELS_DIR).parent_path() / "tests" / "lab";
    for (const std::filesystem::path& file : {directory / name, directory / std::format("{}.lab", name)}) {
        std::ifstream stream(file);
        if (!stream) {
            continue;
        }
        std::ostringstream text;
        text << stream.rdbuf();
        return parseLabPlan(name, text.str());
    }
    std::string names;
    for (const auto& [planName, text] : kBuiltInPlans) {
        names += names.empty() ? planName : std::format(", {}", planName);
    }
    return {std::nullopt, std::format("plan '{}' is neither built in ({}) nor a file in {}", name, names,
                                      directory.string())};
}
