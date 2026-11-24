#include "lab_metrics.hpp"
#include "core/convergence_metrics.hpp"

#include <algorithm>
#include <cmath>

namespace labMetrics
{
    namespace
    {
        struct BlockEvents
        {
            uint32_t rises = 0;
            uint32_t dips = 0;
            uint32_t riseFrames = 0;
            double riseSize = 0.0;
        };

        [[nodiscard]] double medianOf(std::vector<float>& values)
        {
            const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
            std::nth_element(values.begin(), middle, values.end());
            return *middle;
        }

        // runs of frames beyond the threshold on either side of the median; a run still open at the end counts
        [[nodiscard]] BlockEvents findBlockEvents(std::span<const float> series, double median, double threshold)
        {
            BlockEvents events;
            double peak = 0.0;
            bool isRising = false;
            bool isDipping = false;
            for (const float value : series) {
                const double change = value - median;
                if (change > threshold) {
                    events.rises += isRising ? 0 : 1;
                    peak = isRising ? std::max(peak, change) : change;
                    ++events.riseFrames;
                } else if (isRising) {
                    events.riseSize += peak / median;
                }
                isRising = change > threshold;
                events.dips += !isDipping && -change > threshold ? 1 : 0;
                isDipping = -change > threshold;
            }
            events.riseSize += isRising ? peak / median : 0.0;
            return events;
        }
    } // namespace

    void StabilityAccumulator::add(std::span<const float> luminance, vk::Extent2D frameExtent)
    {
        const size_t texels = luminance.size();
        if (frames == 0) {
            sum.assign(texels, 0.0);
            squaredSum.assign(texels, 0.0);
            productSum.assign(texels, 0.0);
            first.assign(luminance.begin(), luminance.end());
            extent = frameExtent;
            blockExtent = vk::Extent2D{(extent.width + kPopBlockPixels - 1) / kPopBlockPixels,
                                       (extent.height + kPopBlockPixels - 1) / kPopBlockPixels};
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
        if (texels == size_t{extent.width} * extent.height) {
            const size_t blocks = size_t{blockExtent.width} * blockExtent.height;
            std::vector<double> blockSums(blocks, 0.0);
            std::vector<uint32_t> blockTexels(blocks, 0);
            for (uint32_t y = 0; y < extent.height; ++y) {
                const size_t row = size_t{y / kPopBlockPixels} * blockExtent.width;
                for (uint32_t x = 0; x < extent.width; ++x) {
                    blockSums[row + x / kPopBlockPixels] += luminance[size_t{y} * extent.width + x];
                    ++blockTexels[row + x / kPopBlockPixels];
                }
            }
            for (size_t block = 0; block < blocks; ++block) {
                blockMeans.push_back(static_cast<float>(blockSums[block] / blockTexels[block]));
            }
        }
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

        const size_t blocks = size_t{blockExtent.width} * blockExtent.height;
        if (blocks == 0 || blockMeans.size() != blocks * frames) {
            return result;
        }
        std::vector<float> series(frames);
        std::vector<float> scratch(frames);
        std::vector<uint32_t> blockRises(blocks, 0);
        BlockEvents total;
        size_t litBlocks = 0;
        size_t risingBlocks = 0;
        for (size_t block = 0; block < blocks; ++block) {
            for (size_t frame = 0; frame < frames; ++frame) {
                series[frame] = blockMeans[frame * blocks + block];
            }
            scratch = series;
            const double median = medianOf(scratch);
            if (median <= litCutoff) {
                continue;
            }
            ++litBlocks;
            for (size_t frame = 0; frame < frames; ++frame) {
                scratch[frame] = static_cast<float>(std::abs(series[frame] - median));
            }
            // 1.4826 MAD: a normal distribution's std, robust to the events being counted
            const double threshold = std::max(kPopSigmas * 1.4826 * medianOf(scratch), kPopMinChange * median);
            const BlockEvents events = findBlockEvents(series, median, threshold);
            blockRises[block] = events.rises;
            risingBlocks += events.rises > 0 ? 1 : 0;
            total.rises += events.rises;
            total.dips += events.dips;
            total.riseFrames += events.riseFrames;
            total.riseSize += events.riseSize;
        }
        result.rises = total.rises / count;
        result.dips = total.dips / count;
        result.riseSize = total.rises > 0 ? 100.0 * total.riseSize / total.rises : 0.0;
        result.riseFrames = total.rises > 0 ? static_cast<double>(total.riseFrames) / total.rises : 0.0;
        result.riseSpread =
            litBlocks > 0 ? 100.0 * static_cast<double>(risingBlocks) / static_cast<double>(litBlocks) : 0.0;
        const double largestRises = std::max(*std::ranges::max_element(blockRises), 1u);
        result.riseMap.resize(sum.size());
        for (uint32_t y = 0; y < extent.height; ++y) {
            for (uint32_t x = 0; x < extent.width; ++x) {
                const size_t block = size_t{y / kPopBlockPixels} * blockExtent.width + x / kPopBlockPixels;
                result.riseMap[size_t{y} * extent.width + x] = blockRises[block] / largestRises;
            }
        }
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
