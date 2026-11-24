#pragma once

#include "render/glass_settings.hpp"
#include "render/indirect_mode.hpp"
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
struct LabConfig
{
    RtxdiContext::Settings di = RtxdiContext::defaultSettings();
    RtxdiContext::GiSettings gi = RtxdiContext::defaultGiSettings();
    // RTXDI without reuse over di (RtxdiContext::isReference)
    bool isDiReference = false;
    IndirectMode indirectMode = kDefaultIndirectMode;
    Dlss::Settings dlss{};
    NrcCache::Settings nrc{};
    bool isJitterEnabled = true;
    GlassSettings glass{};
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
};

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
// `base` (the shipped config by default). Sets: di=shipped|nvidia|converged|reference, gi=shipped|nvidia|converged,
// frame=shipped
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

// ENGINE_CONFIG for the interactive engine and bench runs, over base (the state the other variables set up); nullopt
// when unset. An invalid string is returned as an error, so the run can refuse to start
[[nodiscard]] std::optional<LabConfigParse> readEngineConfig(const LabConfig& base);
