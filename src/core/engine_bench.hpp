#pragma once

#include "../render/vk_gpu_timer.hpp"
#include "../render/vk_meshlet_culling.hpp"
#include "../render/vk_rt_scene.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>

// fixed-scene GPU timing run: averages every timer scope over whole history windows, logs them, then the engine quits.
// ENGINE_BENCH_FRAMES = measured frames (enables the run), ENGINE_BENCH_FIELD = instance-field side (0 = none)
class EngineBench
{
public:
    struct Config
    {
        uint32_t measuredFrames = 0;
        int fieldSide = 0;
    };

    [[nodiscard]] static std::optional<Config> readConfig(uint32_t defaultMeasuredFrames = 0);

    explicit EngineBench(Config config) : config(config) {}

    [[nodiscard]] const Config& getConfig() const noexcept { return config; }
    // once per frame; true once the results are logged. The cull counters logged are the last frame's
    [[nodiscard]] bool onFrame(const GpuTimer& timer, const MeshletCullStats& cullStats,
                               const RayTracingScene::Stats& rtStats);

private:
    // shader object warm-up and GPU clock ramp
    static constexpr uint32_t kWarmupFrames = 2 * GpuTimer::kHistorySize;

    void logResults(const MeshletCullStats& cullStats, const RayTracingScene::Stats& rtStats) const;

    Config config;
    uint32_t frame = 0;
    uint32_t windowCount = 0;
    std::array<double, kGpuTimerScopeCount> sums{};
    std::array<float, kGpuTimerScopeCount> minimums = [] {
        std::array<float, kGpuTimerScopeCount> values{};
        values.fill(std::numeric_limits<float>::max());
        return values;
    }();
};
