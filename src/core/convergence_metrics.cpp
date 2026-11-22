#include "convergence_metrics.hpp"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

DecodedFrame decodeFrame(const Renderer::DisplayCapture& capture)
{
    const size_t texelCount = static_cast<size_t>(capture.extent.width) * capture.extent.height;
    DecodedFrame frame;
    frame.luminance.resize(texelCount);
    for (size_t texel = 0; texel < texelCount; ++texel) {
        const uint16_t* rgb = &capture.rgba16f[texel * 4];
        const float r = glm::unpackHalf1x16(rgb[0]);
        const float g = glm::unpackHalf1x16(rgb[1]);
        const float b = glm::unpackHalf1x16(rgb[2]);
        if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) {
            ++frame.nonFinitePixels;
            frame.luminance[texel] = 0.0f;
            continue;
        }
        if (r < 0.0f || g < 0.0f || b < 0.0f) {
            ++frame.negativePixels;
        }
        frame.rgbTotal[0] += r;
        frame.rgbTotal[1] += g;
        frame.rgbTotal[2] += b;
        frame.luminance[texel] = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    }
    return frame;
}

std::array<double, 3> computeChannelBias(const std::array<double, 3>& meanRgb,
                                         const std::array<double, 3>& referenceRgb, double referenceLuminance)
{
    constexpr double kChannelFloorRatio = 1e-3;
    const double floorDenom = referenceLuminance * kChannelFloorRatio;
    std::array<double, 3> bias{0.0, 0.0, 0.0};
    for (size_t channel = 0; channel < 3; ++channel) {
        const double denom = referenceRgb[channel] > floorDenom ? referenceRgb[channel] : referenceLuminance;
        bias[channel] = denom > 0.0 ? (meanRgb[channel] - referenceRgb[channel]) / denom : 0.0;
    }
    return bias;
}

TileErrors compareTiles(std::span<const double> mean, std::span<const double> reference, vk::Extent2D extent,
                        uint32_t tileSize)
{
    std::vector<double> shares;
    // reference and mean sums of the lit tiles
    std::vector<std::pair<double, double>> litTiles;
    double referenceTotal = 0.0;
    double zeroRefLeakSum = 0.0;
    size_t zeroRefLeakTiles = 0;
    for (uint32_t tileY = 0; tileY < extent.height; tileY += tileSize) {
        for (uint32_t tileX = 0; tileX < extent.width; tileX += tileSize) {
            double referenceSum = 0.0;
            double meanSum = 0.0;
            for (uint32_t y = tileY; y < std::min(tileY + tileSize, extent.height); ++y) {
                for (uint32_t x = tileX; x < std::min(tileX + tileSize, extent.width); ++x) {
                    const size_t texel = static_cast<size_t>(y) * extent.width + x;
                    referenceSum += reference[texel];
                    meanSum += mean[texel];
                }
            }
            referenceTotal += referenceSum;
            if (referenceSum > 0.0) {
                shares.push_back(std::abs(meanSum - referenceSum) / referenceSum);
                litTiles.emplace_back(referenceSum, meanSum);
            } else if (meanSum > 0.0) {
                zeroRefLeakSum += meanSum;
                ++zeroRefLeakTiles;
            }
        }
    }
    const double zeroRefLeak = referenceTotal > 0.0 ? zeroRefLeakSum / referenceTotal : 0.0;
    if (shares.empty()) {
        return {.zeroRefLeak = zeroRefLeak, .zeroRefLeakTiles = zeroRefLeakTiles};
    }
    const double dimCutoff = kDimTileShare * referenceTotal / static_cast<double>(litTiles.size());
    const double hotCutoff = kHotTileFactor * referenceTotal / static_cast<double>(litTiles.size());
    double dimReference = 0.0;
    double dimMean = 0.0;
    size_t dimTiles = 0;
    double hotReference = 0.0;
    double hotMean = 0.0;
    size_t hotTiles = 0;
    for (const auto& [referenceSum, meanSum] : litTiles) {
        if (referenceSum < dimCutoff) {
            dimReference += referenceSum;
            dimMean += meanSum;
            ++dimTiles;
        } else if (referenceSum > hotCutoff) {
            hotReference += referenceSum;
            hotMean += meanSum;
            ++hotTiles;
        }
    }
    std::ranges::sort(shares);
    const auto quantile = [&](double q) {
        return shares[static_cast<size_t>(q * static_cast<double>(shares.size() - 1))];
    };
    return TileErrors{
        .median = quantile(0.5),
        .p90 = quantile(0.9),
        .max = shares.back(),
        .count = shares.size(),
        .zeroRefLeak = zeroRefLeak,
        .zeroRefLeakTiles = zeroRefLeakTiles,
        .dimBias = dimReference > 0.0 ? (dimMean - dimReference) / dimReference : 0.0,
        .dimTiles = dimTiles,
        .hotBias = hotReference > 0.0 ? (hotMean - hotReference) / hotReference : 0.0,
        .hotTiles = hotTiles,
    };
}
