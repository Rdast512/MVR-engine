#pragma once

#include <glm/mat4x4.hpp>
#include <vulkan/vulkan.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// the settings lab's accumulators and metrics (spec/features/planned/full_testing.md §4.2). Definitions follow the I1 runners'
// (engine_temporal_check.cpp) in the lab's own copy, so the checks never change for the lab (§0); tile comparisons go
// through the shared convergence_metrics.hpp
namespace labMetrics
{
    // settling is judged on non-overlapping blocks of this many frames
    inline constexpr uint32_t kBlockFrames = 8;
    inline constexpr double kNoiseSigmas = 4.0;
    inline constexpr double kSettledError = 0.02;
    // CV above this is a flickering pixel; lit: above kLitDisplayCutoff on Lit (about sRGB code 3), above kLitShare of
    // the frame's mean on the linear views
    inline constexpr double kFlickerCv = 0.1;
    inline constexpr double kLitDisplayCutoff = 1e-3;
    inline constexpr double kLitShare = 0.05;
    // screen edge: a revealed region's pixel error at most this times its own in the static reference
    inline constexpr double kSettledPixelRatio = 1.25;
    inline constexpr double kMinRegionShare = 0.01;
    inline constexpr float kDisocclusionScale = 1.02f;
    inline constexpr float kDisocclusionOffsetMeters = 0.02f;

    // per-pixel temporal statistics of a static camera
    class StabilityAccumulator
    {
    public:
        void add(std::span<const float> luminance);
        [[nodiscard]] uint32_t getFrames() const noexcept { return frames; }

        struct Result
        {
            double mean = 0.0;
            // percentages
            double temporalStd = 0.0;
            double flicker = 0.0;
            double lit = 0.0;
            double frameChange = 0.0;
            double largestSwing = 0.0;
            // Σ (x_t − μ)(x_{t+1} − μ) / Σ (x_t − μ)² per lit pixel, averaged: 0 fresh noise, near 1 "too stable"
            double lagCorrelation = 0.0;
            std::vector<double> means;
            // lit pixels' coefficient of variation, 0 elsewhere
            std::vector<double> variation;
        };
        [[nodiscard]] Result finish(bool isDisplayView) const;

    private:
        uint32_t frames = 0;
        std::vector<double> sum;
        std::vector<double> squaredSum;
        // Σ x_t x_{t+1}
        std::vector<double> productSum;
        std::vector<float> first;
        std::vector<float> previous;
        double frameChangeSum = 0.0;
        std::vector<double> frameMeans;
    };

    struct BiasResult
    {
        // percentages of the reference's summed luminance
        double bias = 0.0;
        double tileMedian = 0.0;
        double tileP90 = 0.0;
        double darkLeak = 0.0;
        double dimBias = 0.0;
        double hotBias = 0.0;
        size_t tiles = 0;
        size_t dimTiles = 0;
        size_t hotTiles = 0;
    };
    // false in isValid: the reference is black or the sizes differ
    struct BiasComparison
    {
        bool isValid = false;
        BiasResult result;
    };
    [[nodiscard]] BiasComparison compareToReference(std::span<const double> mean, std::span<const double> reference,
                                                    vk::Extent2D extent, uint32_t tileSize);

    // relative std of the means of consecutive blocks of a static, converged sequence
    [[nodiscard]] double blockNoise(std::span<const double> values, uint32_t block, double mean);
    // frames after the change before the run of passing blocks that lasts to the end; frames + 1 when the last block
    // fails
    template <typename IsBlockPassed>
    [[nodiscard]] uint32_t settledFrames(uint32_t frames, uint32_t block, const IsBlockPassed& isBlockPassed)
    {
        const uint32_t blocks = frames / block;
        for (uint32_t index = blocks; index-- > 0;) {
            if (!isBlockPassed(index * block)) {
                return index + 1 == blocks ? frames + 1 : (index + 1) * block;
            }
        }
        return blocks == 0 ? frames + 1 : 0;
    }

    struct DepthMap
    {
        vk::Extent2D extent{};
        std::vector<float> viewDepth;
        glm::mat4 view{1.0f};
        glm::mat4 invView{1.0f};
        glm::mat4 invProj{1.0f};
        glm::mat4 viewProj{1.0f};
    };

    enum class Region : uint8_t
    {
        Interior,
        // off screen in the initial view
        Band,
        // behind a closer surface in the initial view
        Disoccluded,
        Count,
    };
    // per texel of the final view; empty when the depth maps differ in size
    [[nodiscard]] std::vector<Region> buildRegionMasks(const DepthMap& initial, const DepthMap& final);

    struct RegionSums
    {
        double value = 0.0;
        double reference = 0.0;
        double absoluteError = 0.0;
    };
    using FrameRegions = std::array<RegionSums, static_cast<size_t>(Region::Count)>;
    [[nodiscard]] FrameRegions sumRegions(std::span<const float> luminance, vk::Extent2D extent,
                                          std::span<const double> reference, std::span<const Region> masks,
                                          vk::Extent2D maskExtent);
} // namespace labMetrics
