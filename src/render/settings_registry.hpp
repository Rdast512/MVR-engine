#pragma once

#include "render/denoiser_mode.hpp"
#include "render/glass_settings.hpp"
#include "render/pipeline_modes.hpp"
#include "render/vk_dlss.hpp"
#include "render/vk_nrc.hpp"
#include "render/vk_rtxdi.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class Camera;
class Renderer;

// the settings lab's switch registry (spec/features/planned/full_testing.md §2, §3): every lighting field the lab can set,
// by name. The engine checks never read it: they keep their own settings sets and measure what ships
enum class DlssInputTerms : uint8_t
{
    All,
    Direct,
    Indirect,
    Count,
};

struct LabConfig
{
    RtxdiContext::Settings di = RtxdiContext::defaultSettings();
    RtxdiContext::GiSettings gi = RtxdiContext::defaultGiSettings();
    // Composable pipeline stage toggles (spec/m8/m8_3_pipeline_toggles.md §2)
    DirectMode directMode = kDefaultDirectMode;
    DirectResamplingMode directResampling = kDefaultDirectResamplingMode;
    IndirectBounceMode indirectBounce = kDefaultIndirectBounceMode;
    RadianceCacheMode radianceCache = kDefaultRadianceCacheMode;
    // spec/features/planned/nvidia_settings_layout.md §4.2: Renderer::denoiser, aaMode, accumulationFrameLimit
    DenoiserMode denoiser = DenoiserMode::DlssRr;
    AaMode aaMode = AaMode::None;
    uint32_t accumulationFrameLimit = 0;
    Dlss::Settings dlss{};
    Dlss::Guides dlssGuides{};
    NrcCache::Settings nrc{};
    bool isJitterEnabled = true;
    GlassSettings glass{};
    // attribution only (m8 I16): the DLSS-RR color input's scale and lighting terms
    float dlssInputScale = 1.0f;
    DlssInputTerms dlssInputTerms = DlssInputTerms::All;
    // the input accumulates over a phase's warmup and is held still while it is measured (the settings lab)
    bool isDlssInputFrozen = false;
    // full_testing T11: 0 is off; the reference is DLSS-RR's last output, or the 3x3 neighbors
    float preDenoiserClamp = 0.0f;
    bool isPreDenoiserClampSpatial = false;
    // still camera only: the DLSS-RR input's exponential average weight, 0 is off
    float inputHistoryWeight = 0.0f;
};

enum class LabSwitchType : uint8_t
{
    Bool,
    Uint,
    Float,
    Enum,
};

struct LabSwitch
{
    const char* name;
    LabSwitchType type;
    // Enum: the value names, in enumerator order
    std::span<const char* const> valueNames;
    double (*get)(const LabConfig&);
    void (*set)(LabConfig&, double);
    // where NVIDIA's value comes from (spec §3)
    const char* nvidiaSource;
    // non-null: the switch is planned but its engine feature is not built; names the step (spec §7)
    const char* notBuilt = nullptr;
    // Enum only: optional span parallel to valueNames; non-null entries mark unbuilt enumerators (e.g. "M8.4")
    std::span<const char* const> plannedValues{};
};

// Fixed demo-analog and Engine Target pipeline presets (spec/m8/m8_3_pipeline_toggles.md §6.1)
[[nodiscard]] LabConfig pipelinePresetConfig(PipelinePreset preset);
// Applies only the preset-owned fields (stage toggles, DI/GI sets, denoiser, DLSS quality), preserving glass/NRC/etc.
void applyPipelinePreset(LabConfig& config, PipelinePreset preset);

[[nodiscard]] std::span<const LabSwitch> labSwitches();
[[nodiscard]] const LabSwitch* findLabSwitch(std::string_view name);

// the value as config text: on / off, an enumerator name, an integer or a float
[[nodiscard]] std::string formatSwitchValue(const LabSwitch& entry, double value);
// nullopt: not a value of the switch
[[nodiscard]] std::optional<double> parseSwitchValue(const LabSwitch& entry, std::string_view text);

struct LabConfigParse
{
    std::optional<LabConfig> config;
    // the first error; empty when config is set
    std::string error;
    // the error names a planned switch (LabSwitch::notBuilt): a plan skips the run instead of failing
    bool isPlanned = false;
};

// spec §2.1: `domain=set` items first applied in order, then `domain.switch=value` overrides, left to right, over
// `base` (the shipped config by default). Sets: di=shipped|nvidia|converged|reference and NVIDIA's presets
// di=fast|medium|unbiased|ultra (medium is nvidia), gi=shipped|nvidia|converged, frame=shipped
[[nodiscard]] LabConfigParse parseLabConfig(std::string_view text, const LabConfig& base = LabConfig{});
// the shortest description: the DI and GI sets with the fewest overrides, then every switch that differs from them
[[nodiscard]] std::string formatLabConfig(const LabConfig& config);

struct LabSwitchValue
{
    const LabSwitch* entry;
    double value;
};
// the switches whose values differ, with to's values; built switches only
[[nodiscard]] std::vector<LabSwitchValue> differingSwitches(const LabConfig& from, const LabConfig& to);
[[nodiscard]] bool isSameConfig(const LabConfig& left, const LabConfig& right);

// the renderer state a config describes (rtxdi settings, reference flags, indirect mode, DLSS-RR, NRC, jitter)
void applyLabConfig(const LabConfig& config, Renderer& renderer, Camera& camera);
[[nodiscard]] LabConfig captureLabConfig(Renderer& renderer, const Camera& camera);
