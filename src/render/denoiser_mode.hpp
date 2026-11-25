#pragma once

#include <array>
#include <cstdint>
#include <utility>

// what turns noisyColor into the presented image (spec/features/planned/nvidia_settings_layout.md §4), as the RTXDI
// sample's DenoiserMode. Off renders at the display extent and post reads noisyColor
enum class DenoiserMode : uint8_t
{
    DlssRr,
    Off,
    Count,
};

// anti-aliasing while the denoiser is off, as the RTXDI sample's AntiAliasingMode; TAAU and DLSS SR are not built
enum class AaMode : uint8_t
{
    // no jitter: nothing resolves it
    None,
    // running mean of still frames, post reads it (§4.4)
    Accumulation,
    Count,
};

// UI labels, NVIDIA's
inline constexpr std::array<const char*, std::to_underlying(DenoiserMode::Count)> kDenoiserModeLabels{"DLSS-RR", "Off"};
inline constexpr std::array<const char*, std::to_underlying(AaMode::Count)> kAaModeLabels{"No AA", "Accumulation"};
