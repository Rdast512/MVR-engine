#pragma once

#include "render/vk_rtxdi.hpp"

#include <cstdint>

// the ReSTIR GI settings sets the check runners compare (spec/fixesM1-6/i1_check_runners.md)
enum class GiSettingsSet : uint8_t
{
    // RtxdiContext::convergedGiSettings
    Converged,
    // RtxdiContext::defaultGiSettings
    Shipped,
    // RtxdiContext::nvidiaGiSettings
    Nvidia,
    // shipped with secondary DI resampling, the firefly clamp, or both (findings O2, O6)
    ShippedSecondary,
    ShippedClamp,
    ShippedSecondaryClamp,
    // shipped without the secondary roughness floor (O9)
    ShippedNoFloor,
    // shipped without ReSTIR PT's DLSS-RR profile: the set shipped before O11
    ShippedNoRrProfile,
    // shipped without one part of the profile
    ShippedNoDecorrelation,
    ShippedNoFireflyReplacement,
    // shipped with NVIDIA's sample age limit
    ShippedAgeLimit,
};

[[nodiscard]] constexpr const char* giSettingsSetName(GiSettingsSet set)
{
    switch (set) {
    case GiSettingsSet::Converged:
        return "converged";
    case GiSettingsSet::Shipped:
        return "shipped";
    case GiSettingsSet::Nvidia:
        return "NVIDIA";
    case GiSettingsSet::ShippedSecondary:
        return "shipped + secondary DI resampling";
    case GiSettingsSet::ShippedClamp:
        return "shipped + firefly clamp";
    case GiSettingsSet::ShippedSecondaryClamp:
        return "shipped + both";
    case GiSettingsSet::ShippedNoFloor:
        return "shipped without secondary roughness floor";
    case GiSettingsSet::ShippedNoRrProfile:
        return "shipped without DLSS-RR profile";
    case GiSettingsSet::ShippedNoDecorrelation:
        return "shipped without decorrelation";
    case GiSettingsSet::ShippedNoFireflyReplacement:
        return "shipped without firefly replacement";
    case GiSettingsSet::ShippedAgeLimit:
        return "shipped + NVIDIA age limit";
    }
    return "";
}

[[nodiscard]] inline RtxdiContext::GiSettings giSettingsFor(GiSettingsSet set)
{
    const RtxdiContext::GiSettings nvidia = RtxdiContext::nvidiaGiSettings();
    RtxdiContext::GiSettings settings = RtxdiContext::defaultGiSettings();
    switch (set) {
    case GiSettingsSet::Converged:
        return RtxdiContext::convergedGiSettings();
    case GiSettingsSet::Shipped:
        break;
    case GiSettingsSet::Nvidia:
        return nvidia;
    case GiSettingsSet::ShippedSecondary:
    case GiSettingsSet::ShippedClamp:
    case GiSettingsSet::ShippedSecondaryClamp:
        settings.isSecondaryResamplingEnabled = set != GiSettingsSet::ShippedClamp;
        settings.isFireflyClampEnabled = set != GiSettingsSet::ShippedSecondary;
        break;
    case GiSettingsSet::ShippedNoFloor:
        settings.isSecondaryRoughnessFloorEnabled = false;
        break;
    case GiSettingsSet::ShippedNoRrProfile:
        // history, age limit, boiling filter, decorrelation and history reduction stay NVIDIA's ReSTIR GI defaults
        settings.maxHistoryLength = nvidia.maxHistoryLength;
        settings.maxReservoirAge = nvidia.maxReservoirAge;
        settings.isBoilingFilterEnabled = nvidia.isBoilingFilterEnabled;
        settings.decorrelationMode = nvidia.decorrelationMode;
        settings.isFireflyReplacementEnabled = nvidia.isFireflyReplacementEnabled;
        settings.isDuplicationHistoryReductionEnabled = nvidia.isDuplicationHistoryReductionEnabled;
        break;
    case GiSettingsSet::ShippedNoDecorrelation:
        settings.decorrelationMode = RTXDI_PTDecorrelationMode::None;
        break;
    case GiSettingsSet::ShippedNoFireflyReplacement:
        settings.isFireflyReplacementEnabled = false;
        break;
    case GiSettingsSet::ShippedAgeLimit:
        settings.maxReservoirAge = nvidia.maxReservoirAge;
        break;
    }
    return settings;
}
