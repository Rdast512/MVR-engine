#pragma once

#include <array>
#include <cstdint>
#include <utility>

// Composable lighting-stage pipeline toggles (spec/m8/m8_3_pipeline_toggles.md §2).
// Every stage switches at a frame boundary without a scene reload; disabled stages skip GPU work and retire optional
// buffers/targets after MAX_FRAMES_IN_FLIGHT frames.

// Stage 10: primary direct lighting
enum class DirectMode : uint8_t
{
    // 0 GPU work; skips all rtxdi_* passes and retires DI reservoirs
    Off = 0,
    // single-pass fused RIS + final visibility ray, user candidate counts, zero DI reservoir traffic (PERF-C1)
    StreamingRis,
    // multi-pass ReSTIR DI controlled by DirectResamplingMode
    RestirDi,
    Count,
};
inline constexpr size_t kDirectModeCount = std::to_underlying(DirectMode::Count);

// Active when DirectMode == DirectMode::RestirDi
enum class DirectResamplingMode : uint8_t
{
    TemporalAndSpatial = 0,
    // Temporal ON, Spatial OFF (Engine Target ReSTIR DI candidate)
    Temporal,
    // Spatial ON, Temporal OFF
    Spatial,
    // Ground-truth override: Power_RIS, 32 local, 8 inf, 0 BRDF, no reuse (stays on 2-pass rtxdi_initial + shade)
    Reference,
    Count,
};
inline constexpr size_t kDirectResamplingModeCount = std::to_underlying(DirectResamplingMode::Count);

// Stage 11-12: bounce-1 indirect lighting
enum class IndirectBounceMode : uint8_t
{
    // skips indirect, cache, and ReSTIR GI; pauses NRC; retires GI buffers
    Off = 0,
    // raw uncorrelated 1-bounce PT writing indirectRadiance directly without GI reservoir traffic (PERF-D1)
    RawPt,
    // 1-bounce PT -> GI reservoirs -> ReSTIR GI temporal + spatial + shade
    RestirGi,
    Count,
};
inline constexpr size_t kIndirectBounceModeCount = std::to_underlying(IndirectBounceMode::Count);

// Multi-bounce tail (active when IndirectBounceMode != Off and giReferenceBounces == 0)
enum class RadianceCacheMode : uint8_t
{
    // 1-bounce indirect only; pauses NRC (C-G3-3); zero NRC SDK isolation segments
    Off = 0,
    // NVIDIA NRC 0.16
    Nrc,
    // NVIDIA SHaRC v1.8.3 pure Vulkan BDA spatial hash radiance cache (M8.4)
    Sharc,
    Count,
};
inline constexpr size_t kRadianceCacheModeCount = std::to_underlying(RadianceCacheMode::Count);

// One-click demo-analog presets (fixed toggle + settings bundles; no separate renderer code paths)
enum class PipelinePreset : uint8_t
{
    Custom = 0,
    // Streaming RIS + Raw 1-Bounce PT + NRC + DLSS-RR
    EngineTargetRis,
    // ReSTIR DI Temporal (Basic, History 6, Spatial OFF) + Raw 1-Bounce PT + NRC + DLSS-RR
    EngineTargetTemporal,
    // reference/RTXPT-main IntroSample analog (Uniform 8 local / 1 inf / 0 BRDF, RawPt, SHaRC)
    RtxptAnalog,
    // reference/RTXDI/Samples/FullSample analog (ReSTIR DI Medium, ReSTIR GI, Cache Off)
    RtxdiAnalog,
    // reference/RTXGI/Samples/IndirectDiffuse analog (Uniform 8 local / 1 inf / 0 BRDF, RawPt, NRC)
    RtxgiAnalog,
    Count,
};
inline constexpr size_t kPipelinePresetCount = std::to_underlying(PipelinePreset::Count);

// Shipped startup toggles until M8.5 selects the 1.0 default between EngineTargetRis and EngineTargetTemporal
inline constexpr DirectMode kDefaultDirectMode = DirectMode::RestirDi;
inline constexpr DirectResamplingMode kDefaultDirectResamplingMode = DirectResamplingMode::TemporalAndSpatial;
inline constexpr IndirectBounceMode kDefaultIndirectBounceMode = IndirectBounceMode::RawPt;
inline constexpr RadianceCacheMode kDefaultRadianceCacheMode = RadianceCacheMode::Nrc;

[[nodiscard]] constexpr bool isRadianceCacheBuilt(RadianceCacheMode /*mode*/) noexcept
{
    return true;
}

[[nodiscard]] constexpr const char* directModeLabel(DirectMode mode) noexcept
{
    switch (mode) {
    case DirectMode::Off:
        return "Off";
    case DirectMode::StreamingRis:
        return "Streaming RIS";
    case DirectMode::RestirDi:
        return "ReSTIR DI";
    case DirectMode::Count:
        return "";
    }
    return "";
}

[[nodiscard]] constexpr const char* directResamplingModeLabel(DirectResamplingMode mode) noexcept
{
    switch (mode) {
    case DirectResamplingMode::TemporalAndSpatial:
        return "Temporal + Spatial";
    case DirectResamplingMode::Temporal:
        return "Temporal";
    case DirectResamplingMode::Spatial:
        return "Spatial";
    case DirectResamplingMode::Reference:
        return "Reference (None)";
    case DirectResamplingMode::Count:
        return "";
    }
    return "";
}

[[nodiscard]] constexpr const char* indirectBounceModeLabel(IndirectBounceMode mode) noexcept
{
    switch (mode) {
    case IndirectBounceMode::Off:
        return "Off";
    case IndirectBounceMode::RawPt:
        return "Raw 1-Bounce PT";
    case IndirectBounceMode::RestirGi:
        return "ReSTIR GI";
    case IndirectBounceMode::Count:
        return "";
    }
    return "";
}

[[nodiscard]] constexpr const char* radianceCacheModeLabel(RadianceCacheMode mode) noexcept
{
    switch (mode) {
    case RadianceCacheMode::Off:
        return "Off";
    case RadianceCacheMode::Nrc:
        return "NRC";
    case RadianceCacheMode::Sharc:
        return "SHaRC";
    case RadianceCacheMode::Count:
        return "";
    }
    return "";
}

[[nodiscard]] constexpr const char* pipelinePresetLabel(PipelinePreset preset) noexcept
{
    switch (preset) {
    case PipelinePreset::Custom:
        return "(Custom)";
    case PipelinePreset::EngineTargetRis:
        return "Engine Target (RIS)";
    case PipelinePreset::EngineTargetTemporal:
        return "Engine Target (Temporal)";
    case PipelinePreset::RtxptAnalog:
        return "RTXPT Analog";
    case PipelinePreset::RtxdiAnalog:
        return "RTXDI Analog";
    case PipelinePreset::RtxgiAnalog:
        return "RTXGI Analog";
    case PipelinePreset::Count:
        return "";
    }
    return "";
}

// Legacy 3-way IndirectMode naming kept for ENGINE_INDIRECT_MODE and guide-capture filename suffixes
struct IndirectStagePair
{
    IndirectBounceMode bounce = kDefaultIndirectBounceMode;
    RadianceCacheMode cache = kDefaultRadianceCacheMode;

    constexpr bool operator==(const IndirectStagePair&) const = default;
};

struct LegacyIndirectModeInfo
{
    const char* id;
    const char* label;
    IndirectStagePair stages;
};

inline constexpr std::array<LegacyIndirectModeInfo, 3> kLegacyIndirectModes = {{
    {"nrc-restir-gi", "NRC + ReSTIR GI", {IndirectBounceMode::RestirGi, RadianceCacheMode::Nrc}},
    {"nrc", "NRC", {IndirectBounceMode::RawPt, RadianceCacheMode::Nrc}},
    {"restir-gi", "ReSTIR GI", {IndirectBounceMode::RestirGi, RadianceCacheMode::Off}},
}};

inline constexpr const char* kIndirectModeCycle = "cycle";
