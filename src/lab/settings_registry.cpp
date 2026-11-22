#include "settings_registry.hpp"
#include "render/vk_renderer.hpp"
#include "scene/vk_camera.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <ranges>
#include <type_traits>
#include <utility>

// a new lighting field must become a lab switch (or be excluded in labSwitches with a reason); these break the build
// until it does
static_assert(sizeof(RtxdiContext::Settings) == 84, "a new RtxdiContext::Settings field needs a lab switch");
static_assert(sizeof(RtxdiContext::GiSettings) == 92, "a new RtxdiContext::GiSettings field needs a lab switch");
static_assert(sizeof(NrcCache::Settings) == 16, "a new NrcCache::Settings field needs a lab switch");
static_assert(sizeof(Dlss::Settings) == 2, "a new Dlss::Settings field needs a lab switch");

namespace
{
    using DiSettings = RtxdiContext::Settings;
    using GiSettings = RtxdiContext::GiSettings;

    template <typename T>
    double toDouble(T value)
    {
        if constexpr (std::is_enum_v<T>) {
            return static_cast<double>(std::to_underlying(value));
        } else {
            return static_cast<double>(value);
        }
    }

    template <typename T>
    T fromDouble(double value)
    {
        if constexpr (std::is_same_v<T, bool>) {
            return value != 0.0;
        } else if constexpr (std::is_enum_v<T>) {
            return static_cast<T>(static_cast<std::underlying_type_t<T>>(value));
        } else if constexpr (std::is_integral_v<T>) {
            return static_cast<T>(std::llround(value));
        } else {
            return static_cast<T>(value);
        }
    }

    template <auto Field>
    double getDi(const LabConfig& config)
    {
        return toDouble(config.di.*Field);
    }
    template <auto Field>
    void setDi(LabConfig& config, double value)
    {
        using T = std::remove_cvref_t<decltype(config.di.*Field)>;
        config.di.*Field = fromDouble<T>(value);
    }
    template <auto Field>
    double getGi(const LabConfig& config)
    {
        return toDouble(config.gi.*Field);
    }
    template <auto Field>
    void setGi(LabConfig& config, double value)
    {
        using T = std::remove_cvref_t<decltype(config.gi.*Field)>;
        config.gi.*Field = fromDouble<T>(value);
    }

    constexpr std::array<const char* const, 3> kDiSamplingNames{"uniform", "power_ris", "regir"};
    // RTXDI_BIAS_CORRECTION_* values: off 0, basic 1, pairwise 2, raytraced 3; empty slots are never valid input
    constexpr std::array<const char* const, 4> kDiTemporalCorrectionNames{"off", "basic", "", "raytraced"};
    constexpr std::array<const char* const, 4> kDiSpatialCorrectionNames{"off", "basic", "pairwise", "raytraced"};
    constexpr std::array<const char* const, 4> kGiCorrectionNames{"off", "basic", "", "raytraced"};
    constexpr std::array<const char* const, 3> kDecorrelationNames{"none", "uniform", "stagnancy"};
    constexpr std::array<const char* const, std::to_underlying(IndirectMode::Count)> kIndirectModeNames{
        "nrcRestirGi", "nrc", "restirGi"};
    constexpr std::array<const char* const, 5> kDlssQualityNames{"dlaa", "quality", "balanced", "performance",
                                                                 "ultraPerformance"};
    constexpr std::array<const char* const, 4> kDlssPresetNames{"default", "d", "e", "f"};
    // NrcResolveMode in NrcStructures.h order
    constexpr std::array<const char* const, 11> kNrcResolveNames{
        "addQueryResultToOutput",          "replaceOutputWithQueryResult",   "trainingBounceHeatMap",
        "trainingBounceHeatMapSmoothed",   "primaryVertexTrainingRadiance",  "primaryVertexTrainingRadianceSmoothed",
        "secondaryVertexTrainingRadiance", "secondaryVertexTrainingRadianceSmoothed", "queryIndex",
        "trainingQueryIndex",              "directCacheView"};

    constexpr const char* kDiDefaults = "RTXDI ReSTIRDI.cpp defaults + FullSample Medium (UserInterface.cpp)";
    constexpr const char* kGiDefaults = "RTXDI ReSTIRGI.cpp defaults";
    constexpr const char* kPtProfile = "RTXDI ReSTIRPT.cpp GetDefaultReSTIRPT*Params(dlssRR = true)";
    constexpr const char* kEngineOnly = "engine extension, no NVIDIA counterpart";

    double planned(const LabConfig&) { return 0.0; }
    void plannedSet(LabConfig&, double) {}

    const std::vector<LabSwitch>& registry()
    {
        static const std::vector<LabSwitch> switches = {
            // ReSTIR DI (spec §3.1)
            {"di.sampling", LabSwitchType::Enum, kDiSamplingNames, getDi<&DiSettings::localLightSamplingMode>,
             setDi<&DiSettings::localLightSamplingMode>, kDiDefaults},
            // one count for every mode, as the RTXDI sample's presets set them
            {"di.localSamples", LabSwitchType::Uint, {},
             [](const LabConfig& config) { return static_cast<double>(config.di.activeLocalLightSamples()); },
             [](LabConfig& config, double value) {
                 config.di.localLightSamples.fill(static_cast<uint32_t>(std::llround(value)));
             },
             kDiDefaults},
            {"di.infiniteSamples", LabSwitchType::Uint, {}, getDi<&DiSettings::infiniteLightSamples>,
             setDi<&DiSettings::infiniteLightSamples>, kDiDefaults},
            {"di.brdfSamples", LabSwitchType::Uint, {}, getDi<&DiSettings::brdfSamples>,
             setDi<&DiSettings::brdfSamples>, kDiDefaults},
            {"di.initialVisibility", LabSwitchType::Bool, {}, getDi<&DiSettings::isInitialVisibilityEnabled>,
             setDi<&DiSettings::isInitialVisibilityEnabled>, kDiDefaults},
            {"di.history", LabSwitchType::Uint, {}, getDi<&DiSettings::maxHistoryLength>,
             setDi<&DiSettings::maxHistoryLength>, kDiDefaults},
            {"di.temporalCorrection", LabSwitchType::Enum, kDiTemporalCorrectionNames,
             getDi<&DiSettings::temporalBiasCorrection>, setDi<&DiSettings::temporalBiasCorrection>, kDiDefaults},
            {"di.visibilityShortcut", LabSwitchType::Bool, {}, getDi<&DiSettings::isVisibilityShortcutEnabled>,
             setDi<&DiSettings::isVisibilityShortcutEnabled>, kDiDefaults},
            {"di.fallback", LabSwitchType::Bool, {}, getDi<&DiSettings::isFallbackSamplingEnabled>,
             setDi<&DiSettings::isFallbackSamplingEnabled>, kDiDefaults},
            {"di.permutation", LabSwitchType::Bool, {}, getDi<&DiSettings::isPermutationSamplingEnabled>,
             setDi<&DiSettings::isPermutationSamplingEnabled>, kDiDefaults},
            {"di.spatialSamples", LabSwitchType::Uint, {}, getDi<&DiSettings::spatialSamples>,
             setDi<&DiSettings::spatialSamples>, kDiDefaults},
            {"di.boostSamples", LabSwitchType::Uint, {}, getDi<&DiSettings::disocclusionBoostSamples>,
             setDi<&DiSettings::disocclusionBoostSamples>, kDiDefaults},
            {"di.boostHistory", LabSwitchType::Uint, {}, getDi<&DiSettings::disocclusionHistoryLength>,
             setDi<&DiSettings::disocclusionHistoryLength>, kDiDefaults},
            {"di.radius", LabSwitchType::Float, {}, getDi<&DiSettings::samplingRadius>,
             setDi<&DiSettings::samplingRadius>, kDiDefaults},
            {"di.spatialCorrection", LabSwitchType::Enum, kDiSpatialCorrectionNames,
             getDi<&DiSettings::spatialBiasCorrection>, setDi<&DiSettings::spatialBiasCorrection>, kDiDefaults},
            {"di.materialTest", LabSwitchType::Bool, {}, getDi<&DiSettings::isMaterialSimilarityTestEnabled>,
             setDi<&DiSettings::isMaterialSimilarityTestEnabled>, kDiDefaults},
            {"di.depthThreshold", LabSwitchType::Float, {}, getDi<&DiSettings::depthThreshold>,
             setDi<&DiSettings::depthThreshold>, kDiDefaults},
            {"di.normalThreshold", LabSwitchType::Float, {}, getDi<&DiSettings::normalThreshold>,
             setDi<&DiSettings::normalThreshold>, kDiDefaults},
            {"di.boilingFilter", LabSwitchType::Bool, {}, getDi<&DiSettings::isBoilingFilterEnabled>,
             setDi<&DiSettings::isBoilingFilterEnabled>, kDiDefaults},
            {"di.boilingStrength", LabSwitchType::Float, {}, getDi<&DiSettings::boilingFilterStrength>,
             setDi<&DiSettings::boilingFilterStrength>, kDiDefaults},
            {"di.finalVisibilityReuse", LabSwitchType::Bool, {}, getDi<&DiSettings::isFinalVisibilityReused>,
             setDi<&DiSettings::isFinalVisibilityReused>, kDiDefaults},
            {"di.reference", LabSwitchType::Bool, {}, [](const LabConfig& config) { return toDouble(config.isDiReference); },
             [](LabConfig& config, double value) { config.isDiReference = value != 0.0; }, "RTXDI ResamplingMode::None"},

            // ReSTIR GI and the DLSS-RR profile (spec §3.2)
            {"gi.history", LabSwitchType::Uint, {}, getGi<&GiSettings::maxHistoryLength>,
             setGi<&GiSettings::maxHistoryLength>, kGiDefaults},
            {"gi.age", LabSwitchType::Uint, {}, getGi<&GiSettings::maxReservoirAge>,
             setGi<&GiSettings::maxReservoirAge>, kGiDefaults},
            {"gi.temporalCorrection", LabSwitchType::Enum, kGiCorrectionNames, getGi<&GiSettings::temporalBiasCorrection>,
             setGi<&GiSettings::temporalBiasCorrection>, kGiDefaults},
            {"gi.fallback", LabSwitchType::Bool, {}, getGi<&GiSettings::isFallbackSamplingEnabled>,
             setGi<&GiSettings::isFallbackSamplingEnabled>, kGiDefaults},
            {"gi.permutation", LabSwitchType::Bool, {}, getGi<&GiSettings::isPermutationSamplingEnabled>,
             setGi<&GiSettings::isPermutationSamplingEnabled>, kGiDefaults},
            {"gi.spatialSamples", LabSwitchType::Uint, {}, getGi<&GiSettings::spatialSamples>,
             setGi<&GiSettings::spatialSamples>, kGiDefaults},
            {"gi.radius", LabSwitchType::Float, {}, getGi<&GiSettings::samplingRadius>,
             setGi<&GiSettings::samplingRadius>, kGiDefaults},
            {"gi.spatialCorrection", LabSwitchType::Enum, kGiCorrectionNames, getGi<&GiSettings::spatialBiasCorrection>,
             setGi<&GiSettings::spatialBiasCorrection>, kGiDefaults},
            {"gi.depthThreshold", LabSwitchType::Float, {}, getGi<&GiSettings::depthThreshold>,
             setGi<&GiSettings::depthThreshold>, kGiDefaults},
            {"gi.normalThreshold", LabSwitchType::Float, {}, getGi<&GiSettings::normalThreshold>,
             setGi<&GiSettings::normalThreshold>, kGiDefaults},
            {"gi.boilingFilter", LabSwitchType::Bool, {}, getGi<&GiSettings::isBoilingFilterEnabled>,
             setGi<&GiSettings::isBoilingFilterEnabled>, kGiDefaults},
            {"gi.boilingStrength", LabSwitchType::Float, {}, getGi<&GiSettings::boilingFilterStrength>,
             setGi<&GiSettings::boilingFilterStrength>, kGiDefaults},
            {"gi.fullTileBoiling", LabSwitchType::Bool, {}, getGi<&GiSettings::isFullTileBoilingFilterEnabled>,
             setGi<&GiSettings::isFullTileBoilingFilterEnabled>, kEngineOnly},
            {"gi.finalVisibility", LabSwitchType::Bool, {}, getGi<&GiSettings::isFinalVisibilityEnabled>,
             setGi<&GiSettings::isFinalVisibilityEnabled>, kGiDefaults},
            {"gi.finalMis", LabSwitchType::Bool, {}, getGi<&GiSettings::isFinalMisEnabled>,
             setGi<&GiSettings::isFinalMisEnabled>, kGiDefaults},
            {"gi.secondaryResampling", LabSwitchType::Bool, {}, getGi<&GiSettings::isSecondaryResamplingEnabled>,
             setGi<&GiSettings::isSecondaryResamplingEnabled>, "FullSample Medium (brdfptParams.enableSecondaryResampling)"},
            {"gi.fireflyClamp", LabSwitchType::Bool, {}, getGi<&GiSettings::isFireflyClampEnabled>,
             setGi<&GiSettings::isFireflyClampEnabled>, kEngineOnly},
            {"gi.roughnessFloor", LabSwitchType::Bool, {}, getGi<&GiSettings::isSecondaryRoughnessFloorEnabled>,
             setGi<&GiSettings::isSecondaryRoughnessFloorEnabled>, "FullSample LightingPasses.cpp BRDF ray tracing"},
            {"gi.decorrelation", LabSwitchType::Enum, kDecorrelationNames, getGi<&GiSettings::decorrelationMode>,
             setGi<&GiSettings::decorrelationMode>, kPtProfile},
            {"gi.decorrelationFactor", LabSwitchType::Float, {}, getGi<&GiSettings::decorrelationFactor>,
             setGi<&GiSettings::decorrelationFactor>, kPtProfile},
            {"gi.decorrelationExponent", LabSwitchType::Float, {}, getGi<&GiSettings::decorrelationStagnancyExponent>,
             setGi<&GiSettings::decorrelationStagnancyExponent>, kPtProfile},
            {"gi.decorrelationEma", LabSwitchType::Float, {}, getGi<&GiSettings::decorrelationEmaFactor>,
             setGi<&GiSettings::decorrelationEmaFactor>, kPtProfile},
            {"gi.fireflyReplacement", LabSwitchType::Bool, {}, getGi<&GiSettings::isFireflyReplacementEnabled>,
             setGi<&GiSettings::isFireflyReplacementEnabled>, kPtProfile},
            {"gi.fireflyStrength", LabSwitchType::Float, {}, getGi<&GiSettings::fireflyReplacementStrength>,
             setGi<&GiSettings::fireflyReplacementStrength>, kPtProfile},
            {"gi.fireflyBound", LabSwitchType::Bool, {}, getGi<&GiSettings::isFireflyReplacementBounded>,
             setGi<&GiSettings::isFireflyReplacementBounded>, kPtProfile},
            {"gi.fireflyMultiplyBound", LabSwitchType::Float, {}, getGi<&GiSettings::fireflyReplacementMultiplyBound>,
             setGi<&GiSettings::fireflyReplacementMultiplyBound>, kPtProfile},
            {"gi.duplicationReduction", LabSwitchType::Bool, {}, getGi<&GiSettings::isDuplicationHistoryReductionEnabled>,
             setGi<&GiSettings::isDuplicationHistoryReductionEnabled>, kPtProfile},
            {"gi.historyReductionStrength", LabSwitchType::Float, {}, getGi<&GiSettings::historyReductionStrength>,
             setGi<&GiSettings::historyReductionStrength>, kPtProfile},

            // indirect mode, NRC, frame (spec §3.3)
            {"frame.indirectMode", LabSwitchType::Enum, kIndirectModeNames,
             [](const LabConfig& config) { return toDouble(config.indirectMode); },
             [](LabConfig& config, double value) { config.indirectMode = fromDouble<IndirectMode>(value); },
             kEngineOnly},
            {"frame.jitter", LabSwitchType::Bool, {}, [](const LabConfig& config) { return toDouble(config.isJitterEnabled); },
             [](LabConfig& config, double value) { config.isJitterEnabled = value != 0.0; }, "DLSS-RR requires it"},
            {"frame.dlssQuality", LabSwitchType::Enum, kDlssQualityNames,
             [](const LabConfig& config) { return toDouble(config.dlss.quality); },
             [](LabConfig& config, double value) { config.dlss.quality = fromDouble<Dlss::Quality>(value); },
             "DLSS-RR integration guide"},
            {"frame.dlssPreset", LabSwitchType::Enum, kDlssPresetNames,
             [](const LabConfig& config) { return toDouble(config.dlss.preset); },
             [](LabConfig& config, double value) { config.dlss.preset = fromDouble<Dlss::Preset>(value); },
             "DLSS-RR integration guide"},
            {"nrc.training", LabSwitchType::Bool, {}, [](const LabConfig& config) { return toDouble(config.nrc.isTrainingEnabled); },
             [](LabConfig& config, double value) { config.nrc.isTrainingEnabled = value != 0.0; }, "NrcGuide.md"},
            {"nrc.idleTrainingFrames", LabSwitchType::Uint, {},
             [](const LabConfig& config) { return toDouble(config.nrc.idleTrainingFrames); },
             [](LabConfig& config, double value) {
                 config.nrc.idleTrainingFrames = static_cast<uint32_t>(std::llround(value));
             },
             kEngineOnly},
            {"nrc.iterations", LabSwitchType::Uint, {},
             [](const LabConfig& config) { return toDouble(config.nrc.trainingIterations); },
             [](LabConfig& config, double value) {
                 config.nrc.trainingIterations = static_cast<uint32_t>(std::llround(value));
             },
             "NrcGuide.md"},
            {"nrc.resolveMode", LabSwitchType::Enum, kNrcResolveNames,
             [](const LabConfig& config) { return toDouble(config.nrc.resolveMode); },
             [](LabConfig& config, double value) { config.nrc.resolveMode = fromDouble<NrcResolveMode>(value); },
             "NrcGuide.md §Debugging"},

            // planned: their engine features are not built (spec §7)
            {"frame.historyConfidence", LabSwitchType::Bool, {}, planned, plannedSet, "RTXDI Confidence.md", "T8"},
            {"frame.specularGuides", LabSwitchType::Bool, {}, planned, plannedSet, "DLSS-RR integration guide", "T10"},
            {"gi.targetCompression", LabSwitchType::Float, {}, planned, plannedSet, "RTXDI NoiseAndBias.md §Target PDF",
             "T11"},
            {"frame.preDenoiserClamp", LabSwitchType::Float, {}, planned, plannedSet, kEngineOnly, "T11"},
            {"scene.emissiveScale", LabSwitchType::Float, {}, planned, plannedSet, kEngineOnly, "T8 (cafe.toggle)"},
        };
        return switches;
    }

    std::string_view trim(std::string_view text)
    {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
            text.remove_prefix(1);
        }
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
            text.remove_suffix(1);
        }
        return text;
    }

    bool equalsIgnoreCase(std::string_view left, std::string_view right)
    {
        return std::ranges::equal(left, right, [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        });
    }

    struct NamedSet
    {
        const char* name;
        std::optional<DiSettings> di;
        std::optional<GiSettings> gi;
    };

    std::vector<NamedSet> diSets()
    {
        return {{"shipped", RtxdiContext::defaultSettings(), std::nullopt},
                {"nvidia", RtxdiContext::nvidiaSettings(), std::nullopt},
                {"converged", RtxdiContext::convergedSettings(), std::nullopt}};
    }

    std::vector<NamedSet> giSets()
    {
        return {{"shipped", std::nullopt, RtxdiContext::defaultGiSettings()},
                {"nvidia", std::nullopt, RtxdiContext::nvidiaGiSettings()},
                {"converged", std::nullopt, RtxdiContext::convergedGiSettings()}};
    }

    bool isSameValue(double left, double right)
    {
        return std::abs(left - right) <= 1e-6 * std::max({1.0, std::abs(left), std::abs(right)});
    }

    // false: not a set name
    bool applySet(LabConfig& config, std::string_view domain, std::string_view name)
    {
        if (equalsIgnoreCase(domain, "di")) {
            if (equalsIgnoreCase(name, "reference")) {
                config.di = RtxdiContext::defaultSettings();
                config.isDiReference = true;
                return true;
            }
            for (const NamedSet& set : diSets()) {
                if (equalsIgnoreCase(name, set.name)) {
                    config.di = *set.di;
                    config.isDiReference = false;
                    return true;
                }
            }
            return false;
        }
        if (equalsIgnoreCase(domain, "gi")) {
            for (const NamedSet& set : giSets()) {
                if (equalsIgnoreCase(name, set.name)) {
                    config.gi = *set.gi;
                    return true;
                }
            }
            return false;
        }
        if (equalsIgnoreCase(domain, "frame") && equalsIgnoreCase(name, "shipped")) {
            const LabConfig shipped;
            config.indirectMode = shipped.indirectMode;
            config.dlss = shipped.dlss;
            config.nrc = shipped.nrc;
            config.isJitterEnabled = shipped.isJitterEnabled;
            return true;
        }
        return false;
    }

    bool isDomain(const LabSwitch& entry, std::string_view domain)
    {
        const std::string_view name(entry.name);
        return name.starts_with(domain) && name.size() > domain.size() && name[domain.size()] == '.';
    }
} // namespace

std::span<const LabSwitch> labSwitches() { return registry(); }

const LabSwitch* findLabSwitch(std::string_view name)
{
    for (const LabSwitch& entry : registry()) {
        if (equalsIgnoreCase(entry.name, name)) {
            return &entry;
        }
    }
    return nullptr;
}

std::string formatSwitchValue(const LabSwitch& entry, double value)
{
    switch (entry.type) {
    case LabSwitchType::Bool:
        return value != 0.0 ? "on" : "off";
    case LabSwitchType::Uint:
        return std::format("{}", std::llround(value));
    case LabSwitchType::Float:
        return std::format("{:g}", value);
    case LabSwitchType::Enum: {
        const auto index = static_cast<size_t>(std::llround(value));
        return index < entry.valueNames.size() ? std::string(entry.valueNames[index]) : std::format("{}", index);
    }
    }
    return {};
}

std::optional<double> parseSwitchValue(const LabSwitch& entry, std::string_view text)
{
    text = trim(text);
    switch (entry.type) {
    case LabSwitchType::Bool:
        if (equalsIgnoreCase(text, "on") || equalsIgnoreCase(text, "true") || text == "1") {
            return 1.0;
        }
        if (equalsIgnoreCase(text, "off") || equalsIgnoreCase(text, "false") || text == "0") {
            return 0.0;
        }
        return std::nullopt;
    case LabSwitchType::Uint: {
        uint64_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc() || end != text.data() + text.size()) {
            return std::nullopt;
        }
        return static_cast<double>(value);
    }
    case LabSwitchType::Float: {
        double value = 0.0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc() || end != text.data() + text.size() || !std::isfinite(value)) {
            return std::nullopt;
        }
        return value;
    }
    case LabSwitchType::Enum:
        for (size_t index = 0; index < entry.valueNames.size(); ++index) {
            if (*entry.valueNames[index] != '\0' && equalsIgnoreCase(text, entry.valueNames[index])) {
                return static_cast<double>(index);
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

LabConfigParse parseLabConfig(std::string_view text, const LabConfig& base)
{
    LabConfig config = base;
    // sets first, then overrides left to right: "di.spatialSamples=2 di=nvidia" is NVIDIA's set with 2 samples
    for (const bool isSetPass : {true, false}) {
        for (const auto part : std::views::split(text, ' ')) {
            const std::string_view item = trim(std::string_view(part.begin(), part.end()));
            if (item.empty()) {
                continue;
            }
            const size_t equals = item.find('=');
            if (equals == std::string_view::npos) {
                return {std::nullopt, std::format("'{}' is not domain=set or domain.switch=value", item)};
            }
            const std::string_view key = item.substr(0, equals);
            const std::string_view value = item.substr(equals + 1);
            const bool isSet = key.find('.') == std::string_view::npos;
            if (isSet != isSetPass) {
                continue;
            }
            if (isSet) {
                if (!applySet(config, key, value)) {
                    return {std::nullopt, std::format("'{}' is not a set (di=shipped|nvidia|converged|reference, "
                                                      "gi=shipped|nvidia|converged, frame=shipped)",
                                                      item)};
                }
                continue;
            }
            const LabSwitch* entry = findLabSwitch(key);
            if (entry == nullptr) {
                return {std::nullopt, std::format("'{}' is not a lab switch", key)};
            }
            if (entry->notBuilt != nullptr) {
                return {std::nullopt,
                        std::format("'{}' is planned, its engine feature is not built ({})", key, entry->notBuilt),
                        true};
            }
            const std::optional<double> parsed = parseSwitchValue(*entry, value);
            if (!parsed) {
                return {std::nullopt, std::format("'{}' is not a value of {}", value, entry->name)};
            }
            entry->set(config, *parsed);
        }
    }
    return {config, {}};
}

std::vector<LabSwitchValue> differingSwitches(const LabConfig& from, const LabConfig& to)
{
    std::vector<LabSwitchValue> result;
    for (const LabSwitch& entry : registry()) {
        if (entry.notBuilt == nullptr && !isSameValue(entry.get(from), entry.get(to))) {
            result.push_back({&entry, entry.get(to)});
        }
    }
    return result;
}

bool isSameConfig(const LabConfig& left, const LabConfig& right) { return differingSwitches(left, right).empty(); }

std::string formatLabConfig(const LabConfig& config)
{
    // the DI and GI sets that leave the fewest overrides
    const auto closest = [&](const std::vector<NamedSet>& sets, std::string_view domain) {
        const NamedSet* best = nullptr;
        size_t bestCount = SIZE_MAX;
        for (const NamedSet& set : sets) {
            LabConfig candidate = config;
            if (set.di) {
                candidate.di = *set.di;
            } else {
                candidate.gi = *set.gi;
            }
            size_t count = 0;
            for (const LabSwitchValue& difference : differingSwitches(candidate, config)) {
                count += isDomain(*difference.entry, domain) ? 1 : 0;
            }
            if (count < bestCount) {
                best = &set;
                bestCount = count;
            }
        }
        return best;
    };
    const std::vector<NamedSet> diCandidates = diSets();
    const std::vector<NamedSet> giCandidates = giSets();
    const NamedSet* di = closest(diCandidates, "di");
    const NamedSet* gi = closest(giCandidates, "gi");
    LabConfig base;
    base.di = *di->di;
    base.gi = *gi->gi;
    std::string text = std::format("di={} gi={} frame=shipped", di->name, gi->name);
    for (const LabSwitchValue& difference : differingSwitches(base, config)) {
        text += std::format(" {}={}", difference.entry->name, formatSwitchValue(*difference.entry, difference.value));
    }
    return text;
}

void applyLabConfig(const LabConfig& config, Renderer& renderer, Camera& camera)
{
    RtxdiContext& rtxdi = renderer.getRtxdi();
    rtxdi.settings = config.di;
    rtxdi.giSettings = config.gi;
    rtxdi.isReference = config.isDiReference;
    // the GI references belong to the engine checks, never to a lab config
    rtxdi.isGiReference = false;
    rtxdi.giReferenceBounces = 0;
    renderer.indirectMode = config.indirectMode;
    renderer.getDlss().settings = config.dlss;
    renderer.getNrc().settings = config.nrc;
    camera.isJitterEnabled = config.isJitterEnabled;
}

LabConfig captureLabConfig(Renderer& renderer, const Camera& camera)
{
    RtxdiContext& rtxdi = renderer.getRtxdi();
    LabConfig config;
    config.di = rtxdi.settings;
    config.gi = rtxdi.giSettings;
    config.isDiReference = rtxdi.isReference;
    config.indirectMode = renderer.indirectMode;
    config.dlss = renderer.getDlss().settings;
    config.nrc = renderer.getNrc().settings;
    config.isJitterEnabled = camera.isJitterEnabled;
    return config;
}

std::optional<LabConfigParse> readEngineConfig(const LabConfig& base)
{
    const char* text = std::getenv("ENGINE_CONFIG");
    if (text == nullptr || *text == '\0') {
        return std::nullopt;
    }
    return parseLabConfig(text, base);
}
