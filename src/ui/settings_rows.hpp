#pragma once

#include <array>
#include <span>
#include <string_view>

// the settings window's tree (spec/features/planned/nvidia_settings_layout.md §2, §5): the lab switches each section
// and node edits, so a closed section can count its highlighted rows. Every row that edits a setting names its switch
// here (asserted while drawing). No ImGui: lab_registry_test checks the tables against the registry
struct SettingsRow
{
    const char* switchName;
    // drawn only with Show Advanced Settings, as NVIDIA's
    bool isAdvanced = false;
};
using SettingsRows = std::span<const SettingsRow>;

namespace settingsRows
{
    // Pipeline toggles (spec/m8/m8_3_pipeline_toggles.md §6.2)
    inline constexpr std::array<SettingsRow, 3> kPipeline{
        {{"frame.directMode"}, {"frame.indirectBounce"}, {"frame.radianceCache"}}};
    // Scene
    inline constexpr std::array<SettingsRow, 1> kScene{{{"glass.pass"}}};
    inline constexpr std::array<SettingsRow, 6> kSceneEngine{{{"glass.maxLayers"},
                                                              {"glass.reflection"},
                                                              {"glass.diffuseShare"},
                                                              {"glass.emission"},
                                                              {"glass.shadowTransmittance"},
                                                              {"glass.blendGuides"}}};
    // General Rendering
    inline constexpr std::array<SettingsRow, 1> kGeneral{{{"frame.jitter"}}};
    // Direct Lighting: Resampling Mode edits frame.directResampling
    inline constexpr std::array<SettingsRow, 1> kDirect{{{"frame.directResampling"}}};
    inline constexpr std::array<SettingsRow, 2> kDirectLocalLights{{{"di.sampling"}, {"di.localSamples"}}};
    inline constexpr std::array<SettingsRow, 3> kDirectInitial{
        {{"di.brdfSamples"}, {"di.infiniteSamples"}, {"di.initialVisibility"}}};
    inline constexpr std::array<SettingsRow, 8> kDirectTemporal{{{"di.fallback"},
                                                                 {"di.permutation"},
                                                                 {"di.temporalCorrection"},
                                                                 {"di.depthThreshold", true},
                                                                 {"di.normalThreshold", true},
                                                                 {"di.history"},
                                                                 {"di.boilingFilter"},
                                                                 {"di.boilingStrength"}}};
    inline constexpr std::array<SettingsRow, 6> kDirectSpatial{{{"di.spatialCorrection"},
                                                                {"di.spatialSamples"},
                                                                {"di.boostSamples"},
                                                                {"di.radius"},
                                                                {"di.depthThreshold", true},
                                                                {"di.normalThreshold", true}}};
    inline constexpr std::array<SettingsRow, 2> kDirectFinal{{{"di.visibilityShortcut"}, {"di.finalVisibilityReuse"}}};
    inline constexpr std::array<SettingsRow, 4> kDirectEngine{
        {{"di.exactInfinite"}, {"di.boostHistory"}, {"di.materialTest"}, {"di.perPixelLightTiles"}}};
    // Indirect Lighting
    inline constexpr std::array<SettingsRow, 3> kIndirect{
        {{"gi.roughnessFloor"}, {"gi.finalVisibility"}, {"gi.finalMis"}}};
    inline constexpr std::array<SettingsRow, 1> kIndirectReuse{{{"gi.secondaryResampling"}}};
    inline constexpr std::array<SettingsRow, 2> kIndirectNrc{{{"nrc.training"}, {"nrc.resolveMode"}}};
    inline constexpr std::array<SettingsRow, 9> kIndirectTemporal{{{"gi.depthThreshold"},
                                                                   {"gi.normalThreshold"},
                                                                   {"gi.age"},
                                                                   {"gi.history"},
                                                                   {"gi.permutation"},
                                                                   {"gi.fallback"},
                                                                   {"gi.temporalCorrection"},
                                                                   {"gi.boilingFilter"},
                                                                   {"gi.boilingStrength"}}};
    inline constexpr std::array<SettingsRow, 5> kIndirectSpatial{{{"gi.spatialSamples"},
                                                                  {"gi.radius"},
                                                                  {"gi.depthThreshold"},
                                                                  {"gi.normalThreshold"},
                                                                  {"gi.spatialCorrection"}}};
    inline constexpr std::array<SettingsRow, 4> kIndirectEngine{
        {{"gi.fireflyClamp"}, {"gi.exactInfinite"}, {"nrc.idleTrainingFrames"}, {"nrc.iterations"}}};
    inline constexpr std::array<SettingsRow, 11> kIndirectPtProfile{{{"gi.decorrelation"},
                                                                     {"gi.decorrelationFactor"},
                                                                     {"gi.decorrelationExponent"},
                                                                     {"gi.decorrelationEma"},
                                                                     {"gi.fireflyReplacement"},
                                                                     {"gi.fireflyStrength"},
                                                                     {"gi.fullTileBoiling"},
                                                                     {"gi.fireflyBound"},
                                                                     {"gi.fireflyMultiplyBound"},
                                                                     {"gi.duplicationReduction"},
                                                                     {"gi.historyReductionStrength"}}};
    // Denoising
    inline constexpr std::array<SettingsRow, 1> kDenoising{{{"frame.denoiser"}}};
    inline constexpr std::array<SettingsRow, 3> kDenoisingDlss{
        {{"frame.dlssQuality"}, {"frame.dlssPreset"}, {"frame.dlssSpecularHitDistance"}}};
    // Post-Processing
    inline constexpr std::array<SettingsRow, 2> kPost{{{"frame.aa"}, {"frame.accumulationFrames"}}};

    inline constexpr std::array<SettingsRows, 21> kAll{
        kPipeline,       kScene,             kSceneEngine,   kGeneral,          kDirect,        kDirectLocalLights,
        kDirectInitial,  kDirectTemporal,    kDirectSpatial, kDirectFinal,      kDirectEngine,  kIndirect,
        kIndirectReuse,  kIndirectNrc,       kIndirectTemporal, kIndirectSpatial, kIndirectEngine, kIndirectPtProfile,
        kDenoising,      kDenoisingDlss,     kPost};

    // the switch has a row in the window
    [[nodiscard]] inline bool hasRow(std::string_view switchName)
    {
        for (const SettingsRows rows : kAll) {
            for (const SettingsRow& row : rows) {
                if (switchName == row.switchName) {
                    return true;
                }
            }
        }
        return false;
    }
} // namespace settingsRows
