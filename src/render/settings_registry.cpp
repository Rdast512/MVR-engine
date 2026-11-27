#include "settings_registry.hpp"
#include "vk_renderer.hpp"
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
static_assert(sizeof(RtxdiContext::GiSettings) == 96, "a new RtxdiContext::GiSettings field needs a lab switch");
static_assert(sizeof(NrcCache::Settings) == 16, "a new NrcCache::Settings field needs a lab switch");
// SharcCache::Settings::isContinuousClearEnabled is a debug-only continuous-clear flag not registered in labSwitches
static_assert(sizeof(SharcCache::Settings) == 52, "a new SharcCache::Settings field needs a lab switch");
static_assert(sizeof(Dlss::Settings) == 2, "a new Dlss::Settings field needs a lab switch");
static_assert(sizeof(Dlss::Guides) == 1, "a new Dlss::Guides field needs a lab switch");

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

    template <auto Field>
    double getGlass(const LabConfig& config)
    {
        return toDouble(config.glass.*Field);
    }
    template <auto Field>
    void setGlass(LabConfig& config, double value)
    {
        using T = std::remove_cvref_t<decltype(config.glass.*Field)>;
        config.glass.*Field = fromDouble<T>(value);
    }

    template <auto Field>
    double getSharc(const LabConfig& config)
    {
        return toDouble(config.sharc.*Field);
    }
    template <auto Field>
    void setSharc(LabConfig& config, double value)
    {
        using T = std::remove_cvref_t<decltype(config.sharc.*Field)>;
        config.sharc.*Field = fromDouble<T>(value);
    }

    constexpr std::array<const char* const, 3> kDiSamplingNames{"uniform", "power_ris", "regir"};
    // RTXDI_BIAS_CORRECTION_* values: off 0, basic 1, pairwise 2, raytraced 3; empty slots are never valid input
    constexpr std::array<const char* const, 4> kDiTemporalCorrectionNames{"off", "basic", "", "raytraced"};
    constexpr std::array<const char* const, 4> kDiSpatialCorrectionNames{"off", "basic", "pairwise", "raytraced"};
    constexpr std::array<const char* const, 4> kGiCorrectionNames{"off", "basic", "", "raytraced"};
    constexpr std::array<const char* const, 3> kDecorrelationNames{"none", "uniform", "stagnancy"};
    constexpr std::array<const char* const, kDirectModeCount> kDirectModeNames{"off", "streamingRis", "restirDi"};
    constexpr std::array<const char* const, kDirectResamplingModeCount> kDirectResamplingNames{
        "temporalAndSpatial", "temporal", "spatial", "reference"};
    constexpr std::array<const char* const, kIndirectBounceModeCount> kIndirectBounceNames{"off", "rawPt", "restirGi"};
    constexpr std::array<const char* const, kRadianceCacheModeCount> kRadianceCacheNames{"off", "nrc", "sharc"};
    constexpr std::array<const char* const, kSharcDebugModeCount> kSharcDebugModeNames{
        "radiance", "hash", "occupancy", "collisions", "voxelSize", "level"};
    constexpr std::array<const char* const, std::to_underlying(DlssInputTerms::Count)> kDlssInputTermNames{
        "all", "direct", "indirect"};
    constexpr std::array<const char* const, std::to_underlying(DenoiserMode::Count)> kDenoiserNames{"dlssRr", "off"};
    constexpr std::array<const char* const, std::to_underlying(AaMode::Count)> kAaNames{"none", "accumulation"};
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
            {"di.localSamples",
             LabSwitchType::Uint,
             {},
             [](const LabConfig& config) { return static_cast<double>(config.di.activeLocalLightSamples()); },
             [](LabConfig& config, double value)
             { config.di.localLightSamples.fill(static_cast<uint32_t>(std::llround(value))); },
             kDiDefaults},
            {"di.infiniteSamples",
             LabSwitchType::Uint,
             {},
             getDi<&DiSettings::infiniteLightSamples>,
             setDi<&DiSettings::infiniteLightSamples>,
             kDiDefaults},
            {"di.brdfSamples",
             LabSwitchType::Uint,
             {},
             getDi<&DiSettings::brdfSamples>,
             setDi<&DiSettings::brdfSamples>,
             kDiDefaults},
            {"di.initialVisibility",
             LabSwitchType::Bool,
             {},
             getDi<&DiSettings::isInitialVisibilityEnabled>,
             setDi<&DiSettings::isInitialVisibilityEnabled>,
             kDiDefaults},
            {"di.history",
             LabSwitchType::Uint,
             {},
             getDi<&DiSettings::maxHistoryLength>,
             setDi<&DiSettings::maxHistoryLength>,
             kDiDefaults},
            {"di.temporalCorrection", LabSwitchType::Enum, kDiTemporalCorrectionNames,
             getDi<&DiSettings::temporalBiasCorrection>, setDi<&DiSettings::temporalBiasCorrection>, kDiDefaults},
            {"di.visibilityShortcut",
             LabSwitchType::Bool,
             {},
             getDi<&DiSettings::isVisibilityShortcutEnabled>,
             setDi<&DiSettings::isVisibilityShortcutEnabled>,
             kDiDefaults},
            {"di.fallback",
             LabSwitchType::Bool,
             {},
             getDi<&DiSettings::isFallbackSamplingEnabled>,
             setDi<&DiSettings::isFallbackSamplingEnabled>,
             kDiDefaults},
            {"di.permutation",
             LabSwitchType::Bool,
             {},
             getDi<&DiSettings::isPermutationSamplingEnabled>,
             setDi<&DiSettings::isPermutationSamplingEnabled>,
             kDiDefaults},
            {"di.spatialSamples",
             LabSwitchType::Uint,
             {},
             getDi<&DiSettings::spatialSamples>,
             setDi<&DiSettings::spatialSamples>,
             kDiDefaults},
            {"di.boostSamples",
             LabSwitchType::Uint,
             {},
             getDi<&DiSettings::disocclusionBoostSamples>,
             setDi<&DiSettings::disocclusionBoostSamples>,
             kDiDefaults},
            {"di.boostHistory",
             LabSwitchType::Uint,
             {},
             getDi<&DiSettings::disocclusionHistoryLength>,
             setDi<&DiSettings::disocclusionHistoryLength>,
             kDiDefaults},
            {"di.radius",
             LabSwitchType::Float,
             {},
             getDi<&DiSettings::samplingRadius>,
             setDi<&DiSettings::samplingRadius>,
             kDiDefaults},
            {"di.spatialCorrection", LabSwitchType::Enum, kDiSpatialCorrectionNames,
             getDi<&DiSettings::spatialBiasCorrection>, setDi<&DiSettings::spatialBiasCorrection>, kDiDefaults},
            {"di.materialTest",
             LabSwitchType::Bool,
             {},
             getDi<&DiSettings::isMaterialSimilarityTestEnabled>,
             setDi<&DiSettings::isMaterialSimilarityTestEnabled>,
             kDiDefaults},
            {"di.depthThreshold",
             LabSwitchType::Float,
             {},
             getDi<&DiSettings::depthThreshold>,
             setDi<&DiSettings::depthThreshold>,
             kDiDefaults},
            {"di.normalThreshold",
             LabSwitchType::Float,
             {},
             getDi<&DiSettings::normalThreshold>,
             setDi<&DiSettings::normalThreshold>,
             kDiDefaults},
            {"di.boilingFilter",
             LabSwitchType::Bool,
             {},
             getDi<&DiSettings::isBoilingFilterEnabled>,
             setDi<&DiSettings::isBoilingFilterEnabled>,
             kDiDefaults},
            {"di.boilingStrength",
             LabSwitchType::Float,
             {},
             getDi<&DiSettings::boilingFilterStrength>,
             setDi<&DiSettings::boilingFilterStrength>,
             kDiDefaults},
            {"di.finalVisibilityReuse",
             LabSwitchType::Bool,
             {},
             getDi<&DiSettings::isFinalVisibilityReused>,
             setDi<&DiSettings::isFinalVisibilityReused>,
             kDiDefaults},
            {"di.exactInfinite",
             LabSwitchType::Bool,
             {},
             getDi<&DiSettings::areInfiniteLightsShadedExactly>,
             setDi<&DiSettings::areInfiniteLightsShadedExactly>,
             "m8 I16 phase 2a"},
            {"di.perPixelLightTiles",
             LabSwitchType::Bool,
             {},
             getDi<&DiSettings::areLightTilesPerPixel>,
             setDi<&DiSettings::areLightTilesPerPixel>,
             "RTXDI_TILE_SIZE_IN_PIXELS 16"},

            // ReSTIR GI and the DLSS-RR profile (spec §3.2)
            {"gi.history",
             LabSwitchType::Uint,
             {},
             getGi<&GiSettings::maxHistoryLength>,
             setGi<&GiSettings::maxHistoryLength>,
             kGiDefaults},
            {"gi.age",
             LabSwitchType::Uint,
             {},
             getGi<&GiSettings::maxReservoirAge>,
             setGi<&GiSettings::maxReservoirAge>,
             kGiDefaults},
            {"gi.temporalCorrection", LabSwitchType::Enum, kGiCorrectionNames,
             getGi<&GiSettings::temporalBiasCorrection>, setGi<&GiSettings::temporalBiasCorrection>, kGiDefaults},
            {"gi.fallback",
             LabSwitchType::Bool,
             {},
             getGi<&GiSettings::isFallbackSamplingEnabled>,
             setGi<&GiSettings::isFallbackSamplingEnabled>,
             kGiDefaults},
            {"gi.permutation",
             LabSwitchType::Bool,
             {},
             getGi<&GiSettings::isPermutationSamplingEnabled>,
             setGi<&GiSettings::isPermutationSamplingEnabled>,
             kGiDefaults},
            {"gi.spatialSamples",
             LabSwitchType::Uint,
             {},
             getGi<&GiSettings::spatialSamples>,
             setGi<&GiSettings::spatialSamples>,
             kGiDefaults},
            {"gi.radius",
             LabSwitchType::Float,
             {},
             getGi<&GiSettings::samplingRadius>,
             setGi<&GiSettings::samplingRadius>,
             kGiDefaults},
            {"gi.spatialCorrection", LabSwitchType::Enum, kGiCorrectionNames, getGi<&GiSettings::spatialBiasCorrection>,
             setGi<&GiSettings::spatialBiasCorrection>, kGiDefaults},
            {"gi.depthThreshold",
             LabSwitchType::Float,
             {},
             getGi<&GiSettings::depthThreshold>,
             setGi<&GiSettings::depthThreshold>,
             kGiDefaults},
            {"gi.normalThreshold",
             LabSwitchType::Float,
             {},
             getGi<&GiSettings::normalThreshold>,
             setGi<&GiSettings::normalThreshold>,
             kGiDefaults},
            {"gi.boilingFilter",
             LabSwitchType::Bool,
             {},
             getGi<&GiSettings::isBoilingFilterEnabled>,
             setGi<&GiSettings::isBoilingFilterEnabled>,
             kGiDefaults},
            {"gi.boilingStrength",
             LabSwitchType::Float,
             {},
             getGi<&GiSettings::boilingFilterStrength>,
             setGi<&GiSettings::boilingFilterStrength>,
             kGiDefaults},
            {"gi.fullTileBoiling",
             LabSwitchType::Bool,
             {},
             getGi<&GiSettings::isFullTileBoilingFilterEnabled>,
             setGi<&GiSettings::isFullTileBoilingFilterEnabled>,
             kEngineOnly},
            {"gi.finalVisibility",
             LabSwitchType::Bool,
             {},
             getGi<&GiSettings::isFinalVisibilityEnabled>,
             setGi<&GiSettings::isFinalVisibilityEnabled>,
             kGiDefaults},
            {"gi.finalMis",
             LabSwitchType::Bool,
             {},
             getGi<&GiSettings::isFinalMisEnabled>,
             setGi<&GiSettings::isFinalMisEnabled>,
             kGiDefaults},
            {"gi.secondaryResampling",
             LabSwitchType::Bool,
             {},
             getGi<&GiSettings::isSecondaryResamplingEnabled>,
             setGi<&GiSettings::isSecondaryResamplingEnabled>,
             "FullSample Medium (brdfptParams.enableSecondaryResampling)"},
            {"gi.fireflyClamp",
             LabSwitchType::Bool,
             {},
             getGi<&GiSettings::isFireflyClampEnabled>,
             setGi<&GiSettings::isFireflyClampEnabled>,
             kEngineOnly},
            {"gi.roughnessFloor",
             LabSwitchType::Bool,
             {},
             getGi<&GiSettings::isSecondaryRoughnessFloorEnabled>,
             setGi<&GiSettings::isSecondaryRoughnessFloorEnabled>,
             "FullSample LightingPasses.cpp BRDF ray tracing"},
            {"gi.pathLocalLightSamples",
             LabSwitchType::Uint,
             {},
             getGi<&GiSettings::pathLocalLightSamples>,
             setGi<&GiSettings::pathLocalLightSamples>,
             "FullSample neeLocalLightSampling: 0, the primary surface's count"},
            {"gi.exactInfinite",
             LabSwitchType::Bool,
             {},
             getGi<&GiSettings::areInfiniteLightsShadedExactly>,
             setGi<&GiSettings::areInfiniteLightsShadedExactly>,
             "m8 I16 phase 2c"},
            {"gi.decorrelation", LabSwitchType::Enum, kDecorrelationNames, getGi<&GiSettings::decorrelationMode>,
             setGi<&GiSettings::decorrelationMode>, kPtProfile},
            {"gi.decorrelationFactor",
             LabSwitchType::Float,
             {},
             getGi<&GiSettings::decorrelationFactor>,
             setGi<&GiSettings::decorrelationFactor>,
             kPtProfile},
            {"gi.decorrelationExponent",
             LabSwitchType::Float,
             {},
             getGi<&GiSettings::decorrelationStagnancyExponent>,
             setGi<&GiSettings::decorrelationStagnancyExponent>,
             kPtProfile},
            {"gi.decorrelationEma",
             LabSwitchType::Float,
             {},
             getGi<&GiSettings::decorrelationEmaFactor>,
             setGi<&GiSettings::decorrelationEmaFactor>,
             kPtProfile},
            {"gi.fireflyReplacement",
             LabSwitchType::Bool,
             {},
             getGi<&GiSettings::isFireflyReplacementEnabled>,
             setGi<&GiSettings::isFireflyReplacementEnabled>,
             kPtProfile},
            {"gi.fireflyStrength",
             LabSwitchType::Float,
             {},
             getGi<&GiSettings::fireflyReplacementStrength>,
             setGi<&GiSettings::fireflyReplacementStrength>,
             kPtProfile},
            {"gi.fireflyBound",
             LabSwitchType::Bool,
             {},
             getGi<&GiSettings::isFireflyReplacementBounded>,
             setGi<&GiSettings::isFireflyReplacementBounded>,
             kPtProfile},
            {"gi.fireflyMultiplyBound",
             LabSwitchType::Float,
             {},
             getGi<&GiSettings::fireflyReplacementMultiplyBound>,
             setGi<&GiSettings::fireflyReplacementMultiplyBound>,
             kPtProfile},
            {"gi.duplicationReduction",
             LabSwitchType::Bool,
             {},
             getGi<&GiSettings::isDuplicationHistoryReductionEnabled>,
             setGi<&GiSettings::isDuplicationHistoryReductionEnabled>,
             kPtProfile},
            {"gi.historyReductionStrength",
             LabSwitchType::Float,
             {},
             getGi<&GiSettings::historyReductionStrength>,
             setGi<&GiSettings::historyReductionStrength>,
             kPtProfile},

            // composable pipeline stages, NRC, frame (spec/m8/m8_3_pipeline_toggles.md §6.3)
            {"frame.directMode", LabSwitchType::Enum, kDirectModeNames,
             [](const LabConfig& config) { return toDouble(config.directMode); }, [](LabConfig& config, double value)
             { config.directMode = fromDouble<DirectMode>(value); }, "RTXPT Streaming RIS / RTXDI ReSTIR DI"},
            {"frame.directResampling", LabSwitchType::Enum, kDirectResamplingNames,
             [](const LabConfig& config) { return toDouble(config.directResampling); },
             [](LabConfig& config, double value)
             { config.directResampling = fromDouble<DirectResamplingMode>(value); }, "RTXDI ReSTIRDI_ResamplingMode"},
            {"frame.indirectBounce", LabSwitchType::Enum, kIndirectBounceNames,
             [](const LabConfig& config) { return toDouble(config.indirectBounce); },
             [](LabConfig& config, double value) { config.indirectBounce = fromDouble<IndirectBounceMode>(value); },
             "RTXPT Raw 1-Bounce PT / RTXDI ReSTIR GI"},
            {"frame.radianceCache", LabSwitchType::Enum, kRadianceCacheNames,
             [](const LabConfig& config) { return toDouble(config.radianceCache); },
             [](LabConfig& config, double value) { config.radianceCache = fromDouble<RadianceCacheMode>(value); },
             "RTXGI NRC / RTXPT SHaRC"},
            {"frame.jitter",
             LabSwitchType::Bool,
             {},
             [](const LabConfig& config) { return toDouble(config.isJitterEnabled); },
             [](LabConfig& config, double value) { config.isJitterEnabled = value != 0.0; },
             "DLSS-RR requires it"},
            {"frame.dlssQuality", LabSwitchType::Enum, kDlssQualityNames,
             [](const LabConfig& config) { return toDouble(config.dlss.quality); }, [](LabConfig& config, double value)
             { config.dlss.quality = fromDouble<Dlss::Quality>(value); }, "DLSS-RR integration guide"},
            {"frame.dlssPreset", LabSwitchType::Enum, kDlssPresetNames,
             [](const LabConfig& config) { return toDouble(config.dlss.preset); }, [](LabConfig& config, double value)
             { config.dlss.preset = fromDouble<Dlss::Preset>(value); }, "DLSS-RR integration guide"},
            {"frame.inputScale",
             LabSwitchType::Float,
             {},
             [](const LabConfig& config) { return static_cast<double>(config.dlssInputScale); },
             [](LabConfig& config, double value)
             { config.dlssInputScale = std::max(static_cast<float>(value), 1e-6f); },
             kEngineOnly},
            {"frame.inputTerms", LabSwitchType::Enum, kDlssInputTermNames, [](const LabConfig& config)
             { return toDouble(config.dlssInputTerms); }, [](LabConfig& config, double value)
             { config.dlssInputTerms = fromDouble<DlssInputTerms>(value); }, kEngineOnly},
            {"frame.inputFrozen",
             LabSwitchType::Bool,
             {},
             [](const LabConfig& config) { return toDouble(config.isDlssInputFrozen); },
             [](LabConfig& config, double value) { config.isDlssInputFrozen = value != 0.0; },
             kEngineOnly},
            {"frame.dlssSpecularHitDistance",
             LabSwitchType::Bool,
             {},
             [](const LabConfig& config) { return toDouble(config.dlssGuides.isSpecularHitDistanceUsed); },
             [](LabConfig& config, double value) { config.dlssGuides.isSpecularHitDistanceUsed = value != 0.0; },
             "vk_gltf_renderer / vk_denoise_dlssrr feed it; RTXDI FullSample does not"},
            {"frame.denoiser", LabSwitchType::Enum, kDenoiserNames,
             [](const LabConfig& config) { return toDouble(config.denoiser); }, [](LabConfig& config, double value)
             { config.denoiser = fromDouble<DenoiserMode>(value); }, "RTXDI FullSample DenoiserMode (NONE, DLSS_RR)"},
            {"frame.aa", LabSwitchType::Enum, kAaNames, [](const LabConfig& config) { return toDouble(config.aaMode); },
             [](LabConfig& config, double value) { config.aaMode = fromDouble<AaMode>(value); },
             "RTXDI FullSample AntiAliasingMode (None, Accumulation)"},
            {"frame.accumulationFrames",
             LabSwitchType::Uint,
             {},
             [](const LabConfig& config) { return toDouble(config.accumulationFrameLimit); },
             [](LabConfig& config, double value)
             { config.accumulationFrameLimit = static_cast<uint32_t>(std::llround(value)); },
             "RTXDI FullSample framesToAccumulate"},
            {"nrc.training",
             LabSwitchType::Bool,
             {},
             [](const LabConfig& config) { return toDouble(config.nrc.isTrainingEnabled); },
             [](LabConfig& config, double value) { config.nrc.isTrainingEnabled = value != 0.0; },
             "NrcGuide.md"},
            {"nrc.idleTrainingFrames",
             LabSwitchType::Uint,
             {},
             [](const LabConfig& config) { return toDouble(config.nrc.idleTrainingFrames); },
             [](LabConfig& config, double value)
             { config.nrc.idleTrainingFrames = static_cast<uint32_t>(std::llround(value)); },
             kEngineOnly},
            {"nrc.iterations",
             LabSwitchType::Uint,
             {},
             [](const LabConfig& config) { return toDouble(config.nrc.trainingIterations); },
             [](LabConfig& config, double value)
             { config.nrc.trainingIterations = static_cast<uint32_t>(std::llround(value)); },
             "NrcGuide.md"},
            {"nrc.resolveMode", LabSwitchType::Enum, kNrcResolveNames, [](const LabConfig& config)
             { return toDouble(config.nrc.resolveMode); }, [](LabConfig& config, double value)
             { config.nrc.resolveMode = fromDouble<NrcResolveMode>(value); }, "NrcGuide.md §Debugging"},

            // SHaRC v1.8.3 pure Vulkan BDA radiance cache (spec/m8/m8_4_sharc_cache.md §6.2)
            {"sharc.update",
             LabSwitchType::Bool,
             {},
             getSharc<&SharcCache::Settings::isUpdateEnabled>,
             setSharc<&SharcCache::Settings::isUpdateEnabled>,
             "RTXPT SharcIntegration"},
            {"sharc.resolve",
             LabSwitchType::Bool,
             {},
             getSharc<&SharcCache::Settings::isResolveEnabled>,
             setSharc<&SharcCache::Settings::isResolveEnabled>,
             "RTXPT SharcIntegration"},
            {"sharc.updateCamera",
             LabSwitchType::Bool,
             {},
             getSharc<&SharcCache::Settings::isUpdateViewCamera>,
             setSharc<&SharcCache::Settings::isUpdateViewCamera>,
             "RTXPT SharcIntegration"},
            {"sharc.downscale",
             LabSwitchType::Uint,
             {},
             getSharc<&SharcCache::Settings::downscaleFactor>,
             setSharc<&SharcCache::Settings::downscaleFactor>,
             "RTXPT SharcIntegration (downscaleFactor 4)"},
            {"sharc.capacityShift",
             LabSwitchType::Uint,
             {},
             getSharc<&SharcCache::Settings::capacityShift>,
             setSharc<&SharcCache::Settings::capacityShift>,
             "SHaRC capacity = 1 << shift (22 = 4M entries, 160 MiB)"},
            {"sharc.sceneScale",
             LabSwitchType::Float,
             {},
             getSharc<&SharcCache::Settings::sceneScale>,
             setSharc<&SharcCache::Settings::sceneScale>,
             "SHaRC HashGridParameters::sceneScale"},
            {"sharc.accumulationFrames",
             LabSwitchType::Uint,
             {},
             getSharc<&SharcCache::Settings::accumulationFrameNum>,
             setSharc<&SharcCache::Settings::accumulationFrameNum>,
             "SHaRC SharcResolveParameters::accumulationFrameNum"},
            {"sharc.staleFrames",
             LabSwitchType::Uint,
             {},
             getSharc<&SharcCache::Settings::staleFrameNumMax>,
             setSharc<&SharcCache::Settings::staleFrameNumMax>,
             "SHaRC SharcResolveParameters::staleFrameNumMax"},
            {"sharc.updateBounces",
             LabSwitchType::Uint,
             {},
             getSharc<&SharcCache::Settings::maxUpdateBounces>,
             setSharc<&SharcCache::Settings::maxUpdateBounces>,
             "RTXPT SharcIntegration"},
            {"sharc.queryBounces",
             LabSwitchType::Uint,
             {},
             getSharc<&SharcCache::Settings::maxQueryBounces>,
             setSharc<&SharcCache::Settings::maxQueryBounces>,
             "RTXPT / RTXGI SHaRC fallback continuation"},
            {"sharc.updateLightSamples",
             LabSwitchType::Uint,
             {},
             getSharc<&SharcCache::Settings::updateLocalLightSamples>,
             setSharc<&SharcCache::Settings::updateLocalLightSamples>,
             "Engine M8.5 P3 (0 = path vertex count; RTXPT SharcIntegration NEEFullSamples = 1)"},
            {"sharc.minRoughness",
             LabSwitchType::Float,
             {},
             getSharc<&SharcCache::Settings::minRoughnessOverride>,
             setSharc<&SharcCache::Settings::minRoughnessOverride>,
             "RTXPT SharcIntegration"},
            {"sharc.fireflyClamp",
             LabSwitchType::Float,
             {},
             getSharc<&SharcCache::Settings::fireflyClamp>,
             setSharc<&SharcCache::Settings::fireflyClamp>,
             "Engine scene-unit clamp (0 = off; RTXPT scales by exposure in Sample.cpp:2202)"},
            {"sharc.materialDemodulation",
             LabSwitchType::Bool,
             {},
             getSharc<&SharcCache::Settings::isMaterialDemodulationEnabled>,
             setSharc<&SharcCache::Settings::isMaterialDemodulationEnabled>,
             "SHaRC SHARC_MATERIAL_DEMODULATION"},
            {"sharc.debugMode", LabSwitchType::Enum, kSharcDebugModeNames, getSharc<&SharcCache::Settings::debugMode>,
             setSharc<&SharcCache::Settings::debugMode>, "SHaRC DebugView::SharcCache"},

            // thin-walled glass (m8 I13)
            {"glass.pass",
             LabSwitchType::Bool,
             {},
             getGlass<&GlassSettings::isPassEnabled>,
             setGlass<&GlassSettings::isPassEnabled>,
             "RTXDI FullSample GlassPass"},
            {"glass.maxLayers",
             LabSwitchType::Uint,
             {},
             getGlass<&GlassSettings::maxLayers>,
             setGlass<&GlassSettings::maxLayers>,
             "RTXDI FullSample GlassPass (8)"},
            {"glass.reflection",
             LabSwitchType::Bool,
             {},
             getGlass<&GlassSettings::isReflectionEnabled>,
             setGlass<&GlassSettings::isReflectionEnabled>,
             "RTXDI FullSample GlassPass"},
            {"glass.diffuseShare",
             LabSwitchType::Bool,
             {},
             getGlass<&GlassSettings::isDiffuseShareEnabled>,
             setGlass<&GlassSettings::isDiffuseShareEnabled>,
             kEngineOnly},
            {"glass.emission",
             LabSwitchType::Bool,
             {},
             getGlass<&GlassSettings::isEmissionEnabled>,
             setGlass<&GlassSettings::isEmissionEnabled>,
             "RTXDI FullSample GlassPass"},
            {"glass.shadowTransmittance",
             LabSwitchType::Bool,
             {},
             getGlass<&GlassSettings::isShadowTransmittanceEnabled>,
             setGlass<&GlassSettings::isShadowTransmittanceEnabled>,
             kEngineOnly},
            {"glass.blendGuides",
             LabSwitchType::Bool,
             {},
             getGlass<&GlassSettings::isGuideBlendEnabled>,
             setGlass<&GlassSettings::isGuideBlendEnabled>,
             kEngineOnly},

            // planned: their engine features are not built (spec §7)
            {"frame.historyConfidence", LabSwitchType::Bool, {}, planned, plannedSet, "RTXDI Confidence.md", "T8"},
            {"frame.specularGuides", LabSwitchType::Bool, {}, planned, plannedSet, "DLSS-RR integration guide", "T10"},
            {"gi.targetCompression",
             LabSwitchType::Float,
             {},
             planned,
             plannedSet,
             "RTXDI NoiseAndBias.md §Target PDF",
             "T11"},
            {"frame.preDenoiserClamp",
             LabSwitchType::Float,
             {},
             [](const LabConfig& config) { return static_cast<double>(config.preDenoiserClamp); },
             [](LabConfig& config, double value) { config.preDenoiserClamp = static_cast<float>(value); },
             "RTXPT firefly filter"},
            {"frame.inputHistory",
             LabSwitchType::Float,
             {},
             [](const LabConfig& config) { return static_cast<double>(config.inputHistoryWeight); },
             [](LabConfig& config, double value) { config.inputHistoryWeight = static_cast<float>(value); },
             kEngineOnly},
            {"frame.clampNeighbors",
             LabSwitchType::Bool,
             {},
             [](const LabConfig& config) { return toDouble(config.isPreDenoiserClampSpatial); },
             [](LabConfig& config, double value) { config.isPreDenoiserClampSpatial = value != 0.0; },
             "RTXPT firefly filter"},
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

    // formatLabConfig keeps the first set of a tie: shipped, nvidia and medium are one set since 2026-11-25. NVIDIA's
    // Reference is not a set: di=reference, below, is the tuned set without reuse
    std::vector<NamedSet> diSets()
    {
        using Preset = RtxdiContext::QualityPreset;
        return {{"shipped", RtxdiContext::defaultSettings(), std::nullopt},
                {"nvidia", RtxdiContext::nvidiaSettings(), std::nullopt},
                {"converged", RtxdiContext::convergedSettings(), std::nullopt},
                {"fast", RtxdiContext::nvidiaPreset(Preset::Fast), std::nullopt},
                {"medium", RtxdiContext::nvidiaPreset(Preset::Medium), std::nullopt},
                {"unbiased", RtxdiContext::nvidiaPreset(Preset::Unbiased), std::nullopt},
                {"ultra", RtxdiContext::nvidiaPreset(Preset::Ultra), std::nullopt},
                {"tuned", RtxdiContext::tunedSettings(), std::nullopt}};
    }

    std::vector<NamedSet> giSets()
    {
        return {{"shipped", std::nullopt, RtxdiContext::defaultGiSettings()},
                {"nvidia", std::nullopt, RtxdiContext::nvidiaGiSettings()},
                {"converged", std::nullopt, RtxdiContext::convergedGiSettings()},
                {"tuned", std::nullopt, RtxdiContext::tunedGiSettings()}};
    }

    bool isSameValue(double left, double right)
    {
        return std::abs(left - right) <= 1e-6 * std::max({1.0, std::abs(left), std::abs(right)});
    }

    // false: not a set name
    bool applyPipelinePresetByName(LabConfig& config, std::string_view name)
    {
        if (equalsIgnoreCase(name, "targetRis")) {
            applyPipelinePreset(config, PipelinePreset::EngineTargetRis);
            return true;
        }
        if (equalsIgnoreCase(name, "targetTemporal") || equalsIgnoreCase(name, "target")) {
            applyPipelinePreset(config, PipelinePreset::EngineTargetTemporal);
            return true;
        }
        if (equalsIgnoreCase(name, "rtxpt")) {
            applyPipelinePreset(config, PipelinePreset::RtxptAnalog);
            return true;
        }
        if (equalsIgnoreCase(name, "rtxdi")) {
            applyPipelinePreset(config, PipelinePreset::RtxdiAnalog);
            return true;
        }
        if (equalsIgnoreCase(name, "rtxgi")) {
            applyPipelinePreset(config, PipelinePreset::RtxgiAnalog);
            return true;
        }
        return false;
    }

    // false: not a set name
    bool applySet(LabConfig& config, std::string_view domain, std::string_view name)
    {
        if (equalsIgnoreCase(domain, "pipeline")) {
            return applyPipelinePresetByName(config, name);
        }
        if (equalsIgnoreCase(domain, "di")) {
            if (equalsIgnoreCase(name, "reference")) {
                // the lab's DI references since FT: unchanged when the shipped set became NVIDIA's
                config.di = RtxdiContext::tunedSettings();
                config.directMode = DirectMode::RestirDi;
                config.directResampling = DirectResamplingMode::Reference;
                return true;
            }
            for (const NamedSet& set : diSets()) {
                if (equalsIgnoreCase(name, set.name)) {
                    config.di = *set.di;
                    config.directMode = DirectMode::RestirDi;
                    config.directResampling = DirectResamplingMode::TemporalAndSpatial;
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
            config.directMode = shipped.directMode;
            config.directResampling = shipped.directResampling;
            config.indirectBounce = shipped.indirectBounce;
            config.radianceCache = shipped.radianceCache;
            config.dlss = shipped.dlss;
            config.dlssGuides = shipped.dlssGuides;
            config.nrc = shipped.nrc;
            config.sharc = shipped.sharc;
            config.isJitterEnabled = shipped.isJitterEnabled;
            config.dlssInputScale = shipped.dlssInputScale;
            config.dlssInputTerms = shipped.dlssInputTerms;
            config.isDlssInputFrozen = shipped.isDlssInputFrozen;
            config.preDenoiserClamp = shipped.preDenoiserClamp;
            config.isPreDenoiserClampSpatial = shipped.isPreDenoiserClampSpatial;
            config.inputHistoryWeight = shipped.inputHistoryWeight;
            config.denoiser = shipped.denoiser;
            config.aaMode = shipped.aaMode;
            config.accumulationFrameLimit = shipped.accumulationFrameLimit;
            return true;
        }
        return false;
    }

    // Legacy M8.3 parser aliases so all 57 tests/lab/*.lab files + 5 built-in plans parse without modification
    enum class LegacyAliasResult : uint8_t
    {
        NotAlias,
        Applied,
        BadValue,
    };

    std::optional<bool> parseBoolValue(std::string_view text)
    {
        text = trim(text);
        if (equalsIgnoreCase(text, "on") || equalsIgnoreCase(text, "true") || text == "1") {
            return true;
        }
        if (equalsIgnoreCase(text, "off") || equalsIgnoreCase(text, "false") || text == "0") {
            return false;
        }
        return std::nullopt;
    }

    LegacyAliasResult applyLegacyAlias(LabConfig& config, std::string_view key, std::string_view value)
    {
        if (equalsIgnoreCase(key, "frame.indirectMode")) {
            const std::string_view trimmed = trim(value);
            if (equalsIgnoreCase(trimmed, "nrcRestirGi")) {
                config.indirectBounce = IndirectBounceMode::RestirGi;
                config.radianceCache = RadianceCacheMode::Nrc;
                return LegacyAliasResult::Applied;
            }
            if (equalsIgnoreCase(trimmed, "nrc")) {
                config.indirectBounce = IndirectBounceMode::RawPt;
                config.radianceCache = RadianceCacheMode::Nrc;
                return LegacyAliasResult::Applied;
            }
            if (equalsIgnoreCase(trimmed, "restirGi")) {
                config.indirectBounce = IndirectBounceMode::RestirGi;
                config.radianceCache = RadianceCacheMode::Off;
                return LegacyAliasResult::Applied;
            }
            return LegacyAliasResult::BadValue;
        }
        const auto applyResamplingFlag = [&](DirectResamplingMode target) -> LegacyAliasResult {
            const std::optional<bool> enabled = parseBoolValue(value);
            if (!enabled) {
                return LegacyAliasResult::BadValue;
            }
            if (*enabled) {
                config.directMode = DirectMode::RestirDi;
                config.directResampling = target;
            } else if (config.directResampling == target) {
                config.directResampling = DirectResamplingMode::TemporalAndSpatial;
            }
            return LegacyAliasResult::Applied;
        };
        if (equalsIgnoreCase(key, "di.reference")) {
            return applyResamplingFlag(DirectResamplingMode::Reference);
        }
        if (equalsIgnoreCase(key, "di.spatialOnly")) {
            return applyResamplingFlag(DirectResamplingMode::Spatial);
        }
        if (equalsIgnoreCase(key, "di.temporalOnly")) {
            return applyResamplingFlag(DirectResamplingMode::Temporal);
        }
        return LegacyAliasResult::NotAlias;
    }

    bool isDomain(const LabSwitch& entry, std::string_view domain)
    {
        const std::string_view name(entry.name);
        return name.starts_with(domain) && name.size() > domain.size() && name[domain.size()] == '.';
    }
} // namespace

// Engine Target presets: path vertices keep NVIDIA Medium's 8 local candidates when primary DI takes more (M8.5 P3:
// Bistro cafe tables targetRis 17.98 -> 16.41 ms); NVIDIA analogs inherit the primary count
constexpr uint32_t kTargetPathLocalLightSamples = 8;

LabConfig pipelinePresetConfig(PipelinePreset preset)
{
    LabConfig config;
    config.denoiser = DenoiserMode::DlssRr;
    config.dlss.quality = Dlss::Quality::Quality;
    switch (preset) {
    case PipelinePreset::Custom:
    case PipelinePreset::Count:
        break;
    case PipelinePreset::EngineTargetRis:
        config.directMode = DirectMode::StreamingRis;
        config.directResampling = DirectResamplingMode::TemporalAndSpatial;
        config.di = RtxdiContext::targetRisSettings();
        config.gi.pathLocalLightSamples = kTargetPathLocalLightSamples;
        config.indirectBounce = IndirectBounceMode::RawPt;
        config.radianceCache = RadianceCacheMode::Nrc;
        break;
    case PipelinePreset::EngineTargetTemporal:
        config.directMode = DirectMode::RestirDi;
        config.directResampling = DirectResamplingMode::Temporal;
        config.di = RtxdiContext::targetTemporalSettings();
        config.gi.pathLocalLightSamples = kTargetPathLocalLightSamples;
        config.indirectBounce = IndirectBounceMode::RawPt;
        config.radianceCache = RadianceCacheMode::Nrc;
        break;
    case PipelinePreset::RtxptAnalog:
        config.directMode = DirectMode::StreamingRis;
        config.directResampling = DirectResamplingMode::TemporalAndSpatial;
        config.di = RtxdiContext::analogUniformRisSettings();
        config.indirectBounce = IndirectBounceMode::RawPt;
        config.radianceCache = RadianceCacheMode::Sharc;
        break;
    case PipelinePreset::RtxdiAnalog:
        config.directMode = DirectMode::RestirDi;
        config.directResampling = DirectResamplingMode::TemporalAndSpatial;
        config.di = RtxdiContext::nvidiaSettings();
        config.gi = RtxdiContext::nvidiaGiSettings();
        config.indirectBounce = IndirectBounceMode::RestirGi;
        config.radianceCache = RadianceCacheMode::Off;
        break;
    case PipelinePreset::RtxgiAnalog:
        config.directMode = DirectMode::StreamingRis;
        config.directResampling = DirectResamplingMode::TemporalAndSpatial;
        config.di = RtxdiContext::analogUniformRisSettings();
        config.indirectBounce = IndirectBounceMode::RawPt;
        config.radianceCache = RadianceCacheMode::Nrc;
        break;
    }
    return config;
}

void applyPipelinePreset(LabConfig& config, PipelinePreset preset)
{
    if (preset == PipelinePreset::Custom || preset == PipelinePreset::Count) {
        return;
    }
    const LabConfig target = pipelinePresetConfig(preset);
    config.directMode = target.directMode;
    config.directResampling = target.directResampling;
    config.di = target.di;
    config.gi = target.gi;
    config.indirectBounce = target.indirectBounce;
    config.radianceCache = target.radianceCache;
    config.denoiser = target.denoiser;
    config.dlss.quality = target.dlss.quality;
}

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
        if (const std::optional<bool> parsed = parseBoolValue(text)) {
            return *parsed ? 1.0 : 0.0;
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
    // Pass 0: pipeline=<preset>, Pass 1: domain=<set> (di=, gi=, frame=), Pass 2: domain.switch=value left to right
    for (int pass = 0; pass < 3; ++pass) {
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
            const bool isPipelineSet = isSet && equalsIgnoreCase(key, "pipeline");
            const int itemPass = isPipelineSet ? 0 : (isSet ? 1 : 2);
            if (itemPass != pass) {
                continue;
            }
            if (isSet) {
                if (!applySet(config, key, value)) {
                    return {std::nullopt,
                            std::format("'{}' is not a set (pipeline=targetRis|targetTemporal|target|rtxpt|rtxdi|rtxgi, "
                                        "di=shipped|nvidia|converged|reference|fast|medium|unbiased|ultra|tuned, "
                                        "gi=shipped|nvidia|converged|tuned, frame=shipped)",
                                        item)};
                }
                continue;
            }
            if (const LegacyAliasResult alias = applyLegacyAlias(config, key, value);
                alias != LegacyAliasResult::NotAlias) {
                if (alias == LegacyAliasResult::BadValue) {
                    return {std::nullopt, std::format("'{}' is not a value of {}", value, key)};
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
            if (entry->type == LabSwitchType::Enum && !entry->plannedValues.empty()) {
                const auto index = static_cast<size_t>(std::llround(*parsed));
                if (index < entry->plannedValues.size() && entry->plannedValues[index] != nullptr) {
                    return {std::nullopt,
                            std::format("'{}={}' is planned, its engine feature is not built ({})", entry->name, value,
                                        entry->plannedValues[index]),
                            true};
                }
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
    rtxdi.directMode = config.directMode;
    rtxdi.directResampling = config.directResampling;
    // the GI references belong to the engine checks, never to a lab config
    rtxdi.isGiReference = false;
    rtxdi.giReferenceBounces = 0;
    renderer.indirectBounce = config.indirectBounce;
    renderer.radianceCache = config.radianceCache;
    renderer.denoiser = config.denoiser;
    renderer.aaMode = config.aaMode;
    renderer.accumulationFrameLimit = config.accumulationFrameLimit;
    renderer.getDlss().settings = config.dlss;
    renderer.getDlss().guides = config.dlssGuides;
    renderer.getNrc().settings = config.nrc;
    renderer.getSharc().settings = config.sharc;
    camera.isJitterEnabled = config.isJitterEnabled;
    renderer.glass = config.glass;
    renderer.dlssInputScale = config.dlssInputScale;
    constexpr std::array<uint32_t, std::to_underlying(DlssInputTerms::Count)> kTermFlags{
        0u, FrameFlag::DlssInputDirectOnly, FrameFlag::DlssInputIndirectOnly};
    renderer.extraFrameFlags = kTermFlags[std::to_underlying(config.dlssInputTerms)] |
        (config.isPreDenoiserClampSpatial ? FrameFlag::PreDenoiserClampNeighbors : 0u);
    renderer.preDenoiserClamp = config.preDenoiserClamp;
    renderer.inputHistoryWeight = config.inputHistoryWeight;
}

LabConfig captureLabConfig(Renderer& renderer, const Camera& camera)
{
    RtxdiContext& rtxdi = renderer.getRtxdi();
    LabConfig config;
    config.di = rtxdi.settings;
    config.gi = rtxdi.giSettings;
    config.directMode = rtxdi.directMode;
    config.directResampling = rtxdi.directResampling;
    config.indirectBounce = renderer.indirectBounce;
    config.radianceCache = renderer.radianceCache;
    config.denoiser = renderer.denoiser;
    config.aaMode = renderer.aaMode;
    config.accumulationFrameLimit = renderer.accumulationFrameLimit;
    config.dlss = renderer.getDlss().settings;
    config.dlssGuides = renderer.getDlss().guides;
    config.nrc = renderer.getNrc().settings;
    config.sharc = renderer.getSharc().settings;
    config.isJitterEnabled = camera.isJitterEnabled;
    config.glass = renderer.glass;
    config.dlssInputScale = renderer.dlssInputScale;
    config.dlssInputTerms = (renderer.extraFrameFlags & FrameFlag::DlssInputDirectOnly) != 0 ? DlssInputTerms::Direct
        : (renderer.extraFrameFlags & FrameFlag::DlssInputIndirectOnly) != 0                ? DlssInputTerms::Indirect
                                                                                             : DlssInputTerms::All;
    config.isDlssInputFrozen =
        (renderer.extraFrameFlags & (FrameFlag::DlssInputAccumulate | FrameFlag::DlssInputFrozen)) != 0;
    config.preDenoiserClamp = renderer.preDenoiserClamp;
    config.isPreDenoiserClampSpatial = (renderer.extraFrameFlags & FrameFlag::PreDenoiserClampNeighbors) != 0;
    config.inputHistoryWeight = renderer.inputHistoryWeight;
    return config;
}
