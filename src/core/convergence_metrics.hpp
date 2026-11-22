#pragma once

#include "render/vk_renderer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// shared by the convergence checks of the self-test runners: ReSTIR DI (EngineGuideCapture) and ReSTIR GI
// (EngineGiCheck) compare a mean over frames against a reference image

struct DecodedFrame
{
    // shaders/core/math.slang luminance of every texel (0 where non-finite so sums stay valid)
    std::vector<float> luminance;
    // sum of finite linear RGB across all texels
    std::array<double, 3> rgbTotal{0.0, 0.0, 0.0};
    uint64_t nonFinitePixels = 0;
    uint64_t negativePixels = 0;
};

[[nodiscard]] DecodedFrame decodeFrame(const Renderer::DisplayCapture& capture);

// per-channel relative bias against the reference; falls back to referenceLuminance for near-zero channels
[[nodiscard]] std::array<double, 3> computeChannelBias(const std::array<double, 3>& meanRgb,
                                                       const std::array<double, 3>& referenceRgb,
                                                       double referenceLuminance);

struct TileErrors
{
    double median = 0.0;
    double p90 = 0.0;
    double max = 0.0;
    size_t count = 0;
    // sum of positive mean in tiles whose reference is zero, as a share of total reference luminance
    double zeroRefLeak = 0.0;
    size_t zeroRefLeakTiles = 0;
    // signed sum(mean - reference) / sum(reference) over the lit tiles below kDimTileShare of the average lit tile:
    // light leaking into places that are dim but not black reads positive
    double dimBias = 0.0;
    size_t dimTiles = 0;
};

inline constexpr double kDimTileShare = 0.1;

// |sum(mean - reference)| / sum(reference) per tile: signed within a tile, so the noise cancels and a local bias does
// not. Tiles with a black reference are tracked separately in zeroRefLeak; count 0 when every tile is black
[[nodiscard]] TileErrors compareTiles(std::span<const double> mean, std::span<const double> reference,
                                      vk::Extent2D extent, uint32_t tileSize);
