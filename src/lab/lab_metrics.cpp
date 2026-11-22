#include "lab_metrics.hpp"
#include "core/convergence_metrics.hpp"

#include <algorithm>
#include <cmath>

namespace labMetrics
{
    void StabilityAccumulator::add(std::span<const float> luminance)
    {
        const size_t texels = luminance.size();
        if (frames == 0) {
            sum.assign(texels, 0.0);
            squaredSum.assign(texels, 0.0);
            productSum.assign(texels, 0.0);
            first.assign(luminance.begin(), luminance.end());
        }
        double frameSum = 0.0;
        for (size_t texel = 0; texel < texels; ++texel) {
            const double value = luminance[texel];
            sum[texel] += value;
            squaredSum[texel] += value * value;
            if (frames > 0) {
                productSum[texel] += value * previous[texel];
                frameChangeSum += std::abs(value - previous[texel]);
            }
            frameSum += value;
        }
        frameMeans.push_back(texels > 0 ? frameSum / static_cast<double>(texels) : 0.0);
        previous.assign(luminance.begin(), luminance.end());
        ++frames;
    }

    StabilityAccumulator::Result StabilityAccumulator::finish(bool isDisplayView) const
    {
        Result result;
        if (frames < 2 || sum.empty()) {
            return result;
        }
        const double count = frames;
        double signal = 0.0;
        for (const double value : sum) {
            signal += value / count;
        }
        result.mean = signal / static_cast<double>(sum.size());
        const double litCutoff = isDisplayView ? kLitDisplayCutoff : kLitShare * result.mean;
        double deviation = 0.0;
        size_t litPixels = 0;
        size_t flickeringPixels = 0;
        double lagTotal = 0.0;
        size_t lagPixels = 0;
        result.means.resize(sum.size());
        result.variation.assign(sum.size(), 0.0);
        for (size_t texel = 0; texel < sum.size(); ++texel) {
            const double mean = sum[texel] / count;
            const double variance = std::max(squaredSum[texel] / count - mean * mean, 0.0);
            const double pixelDeviation = std::sqrt(variance);
            result.means[texel] = mean;
            deviation += pixelDeviation;
            if (mean <= litCutoff) {
                continue;
            }
            ++litPixels;
            flickeringPixels += pixelDeviation > kFlickerCv * mean ? 1 : 0;
            result.variation[texel] = pixelDeviation / mean;
            // Σ_{t<n-1} (x_t − μ)(x_{t+1} − μ) = P − μ (2S − x_0 − x_{n−1}) + (n − 1) μ²
            const double squaredDeviations = variance * count;
            if (squaredDeviations > 1e-12 * mean * mean * count) {
                const double lagged = productSum[texel] -
                    mean * (2.0 * sum[texel] - first[texel] - previous[texel]) + (count - 1.0) * mean * mean;
                lagTotal += lagged / squaredDeviations;
                ++lagPixels;
            }
        }
        result.temporalStd = signal > 0.0 ? 100.0 * deviation / signal : 0.0;
        result.flicker =
            litPixels > 0 ? 100.0 * static_cast<double>(flickeringPixels) / static_cast<double>(litPixels) : 0.0;
        result.lit = 100.0 * static_cast<double>(litPixels) / static_cast<double>(sum.size());
        result.frameChange = signal > 0.0 ? 100.0 * frameChangeSum / (count - 1.0) / signal : 0.0;
        result.lagCorrelation = lagPixels > 0 ? lagTotal / static_cast<double>(lagPixels) : 0.0;
        double largestSwing = 0.0;
        for (size_t start = 0; start + kBlockFrames <= frameMeans.size(); start += kBlockFrames) {
            double total = 0.0;
            for (size_t index = start; index < start + kBlockFrames; ++index) {
                total += frameMeans[index];
            }
            largestSwing = std::max(largestSwing, std::abs(total / kBlockFrames - result.mean));
        }
        result.largestSwing = result.mean > 0.0 ? 100.0 * largestSwing / result.mean : 0.0;
        return result;
    }

    BiasComparison compareToReference(std::span<const double> mean, std::span<const double> reference,
                                      vk::Extent2D extent, uint32_t tileSize)
    {
        if (mean.size() != reference.size() || mean.size() != size_t{extent.width} * extent.height) {
            return {};
        }
        double meanTotal = 0.0;
        double referenceTotal = 0.0;
        for (size_t texel = 0; texel < mean.size(); ++texel) {
            meanTotal += mean[texel];
            referenceTotal += reference[texel];
        }
        if (referenceTotal <= 0.0) {
            return {};
        }
        const TileErrors tiles = compareTiles(mean, reference, extent, tileSize);
        return {true,
                {.bias = 100.0 * (meanTotal - referenceTotal) / referenceTotal,
                 .tileMedian = 100.0 * tiles.median,
                 .tileP90 = 100.0 * tiles.p90,
                 .darkLeak = 100.0 * tiles.zeroRefLeak,
                 .dimBias = 100.0 * tiles.dimBias,
                 .hotBias = 100.0 * tiles.hotBias,
                 .tiles = tiles.count,
                 .dimTiles = tiles.dimTiles,
                 .hotTiles = tiles.hotTiles}};
    }

    double blockNoise(std::span<const double> values, uint32_t block, double mean)
    {
        const size_t blocks = values.size() / block;
        if (blocks < 2 || mean <= 0.0) {
            return 0.0;
        }
        double squared = 0.0;
        for (size_t index = 0; index < blocks; ++index) {
            double total = 0.0;
            for (size_t frame = index * block; frame < (index + 1) * block; ++frame) {
                total += values[frame];
            }
            const double error = total / block - mean;
            squared += error * error;
        }
        return std::sqrt(squared / static_cast<double>(blocks - 1)) / mean;
    }

    std::vector<Region> buildRegionMasks(const DepthMap& initial, const DepthMap& final)
    {
        const vk::Extent2D size = final.extent;
        if (initial.extent != size || final.viewDepth.empty() || initial.viewDepth.empty()) {
            return {};
        }
        std::vector<Region> masks(size_t{size.width} * size.height, Region::Interior);
        for (uint32_t y = 0; y < size.height; ++y) {
            for (uint32_t x = 0; x < size.width; ++x) {
                const size_t texel = size_t{y} * size.width + x;
                const glm::vec2 ndc{(static_cast<float>(x) + 0.5f) / static_cast<float>(size.width) * 2.0f - 1.0f,
                                    (static_cast<float>(y) + 0.5f) / static_cast<float>(size.height) * 2.0f - 1.0f};
                const glm::vec4 nearPoint = final.invProj * glm::vec4(ndc, 1.0f, 1.0f);
                const glm::vec3 viewPoint = glm::vec3(nearPoint) / nearPoint.w;
                const glm::vec3 scaled = viewPoint * (final.viewDepth[texel] / std::abs(viewPoint.z));
                const glm::vec4 world = final.invView * glm::vec4(scaled, 1.0f);
                const glm::vec4 clip = initial.viewProj * world;
                const glm::vec2 initialNdc = glm::vec2(clip) / clip.w;
                if (clip.w <= 0.0f || std::abs(initialNdc.x) > 1.0f || std::abs(initialNdc.y) > 1.0f) {
                    masks[texel] = Region::Band;
                    continue;
                }
                const uint32_t initialX = std::min(
                    static_cast<uint32_t>((initialNdc.x * 0.5f + 0.5f) * static_cast<float>(size.width)),
                    size.width - 1);
                const uint32_t initialY = std::min(
                    static_cast<uint32_t>((initialNdc.y * 0.5f + 0.5f) * static_cast<float>(size.height)),
                    size.height - 1);
                const float surfaceDepth = initial.viewDepth[size_t{initialY} * size.width + initialX];
                const float pointDepth = std::abs((initial.view * world).z);
                if (pointDepth > surfaceDepth * kDisocclusionScale + kDisocclusionOffsetMeters) {
                    masks[texel] = Region::Disoccluded;
                }
            }
        }
        return masks;
    }

    FrameRegions sumRegions(std::span<const float> luminance, vk::Extent2D extent, std::span<const double> reference,
                            std::span<const Region> masks, vk::Extent2D maskExtent)
    {
        FrameRegions regions{};
        for (uint32_t y = 0; y < extent.height; ++y) {
            const uint32_t maskY = y * maskExtent.height / extent.height;
            for (uint32_t x = 0; x < extent.width; ++x) {
                const uint32_t maskX = x * maskExtent.width / extent.width;
                const size_t texel = size_t{y} * extent.width + x;
                RegionSums& sums = regions[static_cast<size_t>(masks[size_t{maskY} * maskExtent.width + maskX])];
                sums.value += luminance[texel];
                sums.reference += reference[texel];
                sums.absoluteError += std::abs(luminance[texel] - reference[texel]);
            }
        }
        return regions;
    }
} // namespace labMetrics
