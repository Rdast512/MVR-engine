#pragma once

#include "render/vk_renderer.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// shared by the convergence checks of the self-test runners: ReSTIR DI (EngineGuideCapture) and ReSTIR GI
// (EngineGiCheck) compare a mean over frames against a reference image

// shaders/core/math.slang luminance of every texel
[[nodiscard]] std::vector<float> decodeLuminance(const Renderer::DisplayCapture& capture);

struct TileErrors
{
    double median = 0.0;
    double p90 = 0.0;
    double max = 0.0;
    size_t count = 0;
};

// |sum(mean - reference)| / sum(reference) per tile: signed within a tile, so the noise cancels and a local bias does
// not. Tiles with a black reference are skipped; count 0 when every tile is
[[nodiscard]] TileErrors compareTiles(std::span<const double> mean, std::span<const double> reference,
                                      vk::Extent2D extent, uint32_t tileSize);
