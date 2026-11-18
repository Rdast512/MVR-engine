#include "convergence_metrics.hpp"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>

std::vector<float> decodeLuminance(const Renderer::DisplayCapture& capture)
{
    const size_t texelCount = static_cast<size_t>(capture.extent.width) * capture.extent.height;
    std::vector<float> luminance(texelCount);
    for (size_t texel = 0; texel < texelCount; ++texel) {
        const uint16_t* rgb = &capture.rgba16f[texel * 4];
        luminance[texel] = 0.2126f * glm::unpackHalf1x16(rgb[0]) + 0.7152f * glm::unpackHalf1x16(rgb[1]) +
            0.0722f * glm::unpackHalf1x16(rgb[2]);
    }
    return luminance;
}

TileErrors compareTiles(std::span<const double> mean, std::span<const double> reference, vk::Extent2D extent,
                        uint32_t tileSize)
{
    std::vector<double> shares;
    for (uint32_t tileY = 0; tileY < extent.height; tileY += tileSize) {
        for (uint32_t tileX = 0; tileX < extent.width; tileX += tileSize) {
            double referenceSum = 0.0;
            double difference = 0.0;
            for (uint32_t y = tileY; y < std::min(tileY + tileSize, extent.height); ++y) {
                for (uint32_t x = tileX; x < std::min(tileX + tileSize, extent.width); ++x) {
                    const size_t texel = static_cast<size_t>(y) * extent.width + x;
                    referenceSum += reference[texel];
                    difference += mean[texel] - reference[texel];
                }
            }
            if (referenceSum > 0.0) {
                shares.push_back(std::abs(difference) / referenceSum);
            }
        }
    }
    if (shares.empty()) {
        return {};
    }
    std::ranges::sort(shares);
    const auto quantile = [&](double q) { return shares[static_cast<size_t>(q * static_cast<double>(shares.size() - 1))]; };
    return TileErrors{.median = quantile(0.5), .p90 = quantile(0.9), .max = shares.back(), .count = shares.size()};
}
