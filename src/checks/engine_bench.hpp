#pragma once

#include "core/object_storage.hpp"
#include "../render/vk_gpu_timer.hpp"
#include "../render/vk_meshlet_culling.hpp"
#include "../render/vk_renderer.hpp"
#include "../render/vk_rt_scene.hpp"
#include "../render/vk_rtxdi.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

// fixed-scene GPU timing run: averages every timer scope over whole history windows, logs them, then the engine quits.
// ENGINE_BENCH_FRAMES = measured frames (enables the run), ENGINE_BENCH_FIELD = instance-field side (0 = none),
// ENGINE_BENCH_FIELD_MOVE = 1 translates field instances across XZ partitions each frame (C-C4-5 / I10),
// ENGINE_BENCH_MODEL = startup model path override, ENGINE_BENCH_POSE = "courtyard" for sunlit Sponza pose, or a
// Bistro glass pose name ("street windows", "bar shelf", check_poses.hpp) with its sun
class EngineBench
{
public:
    struct Config
    {
        uint32_t measuredFrames = 0;
        int fieldSide = 0;
        bool isFieldMoving = false;
        std::string modelPath;
        std::string pose;
    };

    [[nodiscard]] static std::optional<Config> readConfig(uint32_t defaultMeasuredFrames = 0);

    explicit EngineBench(Config config) : config(std::move(config)) {}

    [[nodiscard]] const Config& getConfig() const noexcept { return config; }
    void setFieldMotion(EntityId firstEntity, uint32_t entityCount, uint32_t side, float centerX,
                        float cellSize) noexcept;
    // advances moving field instances before drawFrame when isFieldMoving is set
    void stepField(ObjectStorage& storage);
    // once per frame; true once the results are logged. The cull and glass counters logged are the last frame's
    [[nodiscard]] bool onFrame(const GpuTimer& timer, const MeshletCullStats& cullStats,
                               const RayTracingScene::Stats& rtStats, const RtxdiContext::LightPrepStats& lightPrep,
                               const Renderer::CpuStats& cpuStats, const GpuGlassCounters& glassCounters);

private:
    // shader object warm-up and GPU clock ramp
    static constexpr uint32_t kWarmupFrames = 2 * GpuTimer::kHistorySize;

    void logResults(const GpuTimer& timer, const MeshletCullStats& cullStats, const RayTracingScene::Stats& rtStats,
                    const RtxdiContext::LightPrepStats& lightPrep, const Renderer::CpuStats& cpuStats,
                    const GpuGlassCounters& glassCounters) const;

    Config config;
    EntityId fieldFirstEntity = 0;
    uint32_t fieldEntityCount = 0;
    uint32_t fieldSide = 0;
    float fieldCenterX = 0.0f;
    float fieldCellSize = 0.0f;
    uint32_t frame = 0;
    uint32_t windowCount = 0;
    std::array<double, kGpuTimerScopeCount> sums{};
    std::array<float, kGpuTimerScopeCount> minimums = [] {
        std::array<float, kGpuTimerScopeCount> values{};
        values.fill(std::numeric_limits<float>::max());
        return values;
    }();
};
