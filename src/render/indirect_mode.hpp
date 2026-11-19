#pragma once

#include <array>
#include <cstdint>
#include <utility>

// which passes produce indirectRadiance (engine spec §11.7, spec/m4/g7_indirect_modes.md). NRC stays configured in
// every mode; ReSTIR GI mode only pauses it
enum class IndirectMode : uint8_t
{
    // cache-terminated samples, then ReSTIR GI: the default of both tiers
    NrcRestirGi,
    // cache-terminated samples shaded without reuse (RTXGI IndirectDiffuse Nrc mode)
    Nrc,
    // one-bounce samples, then ReSTIR GI (RTXDI sample)
    RestirGi,
    Count,
};

struct IndirectModeInfo
{
    // UI and log label
    const char* label;
    // ENGINE_INDIRECT_MODE value and file name suffix
    const char* id;
};

inline constexpr std::array<IndirectModeInfo, std::to_underlying(IndirectMode::Count)> kIndirectModes = {{
    {"NRC + ReSTIR GI", "nrc-restir-gi"},
    {"NRC", "nrc"},
    {"ReSTIR GI", "restir-gi"},
}};

[[nodiscard]] constexpr const IndirectModeInfo& indirectModeInfo(IndirectMode mode)
{
    return kIndirectModes[std::to_underlying(mode)];
}
