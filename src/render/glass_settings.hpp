#pragma once

#include "core/types.hpp"

#include <algorithm>
#include <cstdint>

// thin-walled glass (m8 I13, spec/fixesM1-6/i13_glass_transmission.md): the glass pass and the shadow transmittance.
// Read once per recorded frame; the UI changes it between frames
struct GlassSettings
{
    // off: glass is neither rasterized nor composited, so it is invisible (a debug switch)
    bool isPassEnabled = true;
    // RTXDI's GlassPass walks up to 8; 4 covers a bottle behind a window (I13 §2, decided with §6's counters)
    uint32_t maxLayers = 4;
    bool isReflectionEnabled = true;
    bool isDiffuseShareEnabled = true;
    bool isEmissionEnabled = true;
    // final DI, X2's direct light and final ReSTIR GI shading through glass (I13 §3). Off: +0.68 ms GPU on Bistro's
    // street pose for -0.9% Direct lighting, and RTXDI's FullSample shadows ignore glass too (I13 §8)
    bool isShadowTransmittanceEnabled = false;
    // DLSS-RR albedo guides blended with the layers (I13 §4 variant b); off: the surface behind (variant a)
    bool isGuideBlendEnabled = false;

    // GpuFrameRoot::glassFlags; 0 without glass instances, so a scene without glass pays nothing
    [[nodiscard]] uint32_t flags(bool hasGlass) const noexcept
    {
        if (!hasGlass) {
            return 0;
        }
        return (isPassEnabled ? GlassFlag::Pass : 0u) |
            (isShadowTransmittanceEnabled ? GlassFlag::ShadowTransmittance : 0u) |
            (isReflectionEnabled ? GlassFlag::Reflection : 0u) | (isDiffuseShareEnabled ? GlassFlag::DiffuseShare : 0u) |
            (isEmissionEnabled ? GlassFlag::Emission : 0u) | (isGuideBlendEnabled ? GlassFlag::BlendGuides : 0u);
    }

    [[nodiscard]] uint32_t clampedMaxLayers() const noexcept { return std::clamp(maxLayers, 1u, kGlassMaxLayersLimit); }

    bool operator==(const GlassSettings&) const = default;
};
