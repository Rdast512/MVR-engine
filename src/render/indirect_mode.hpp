#pragma once

#include <array>
#include <cstdint>
#include <utility>

// which passes produce indirectRadiance (engine spec §11.7, spec/m4/g7_indirect_modes.md). NRC stays configured in
// every mode; ReSTIR GI mode only pauses it
enum class IndirectMode : uint8_t
{
    // cache-terminated samples, then ReSTIR GI: the tuning target (m8 I15)
    NrcRestirGi,
    // cache-terminated samples shaded without reuse (RTXGI IndirectDiffuse Nrc mode): the default
    Nrc,
    // one-bounce samples, then ReSTIR GI (RTXDI sample)
    RestirGi,
    Count,
};

// shipped since 2026-11-25: NVIDIA's NRC sample, as the other defaults are NVIDIA's; NRC + ReSTIR GI stays the
// tuning target
inline constexpr IndirectMode kDefaultIndirectMode = IndirectMode::Nrc;

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
