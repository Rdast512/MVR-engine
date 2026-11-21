#include "engine_bench.hpp"
#include "../static_headers/logger.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <format>
#include <string_view>
#include <utility>

namespace
{
template <typename T>
[[nodiscard]] std::optional<T> readEnvNumber(const char* name)
{
    const char* text = std::getenv(name);
    if (text == nullptr) {
        return std::nullopt;
    }
    const std::string_view view{text};
    T value{};
    const auto [end, error] = std::from_chars(view.data(), view.data() + view.size(), value);
    if (error != std::errc{} || end != view.data() + view.size()) {
        logError(std::format("{}='{}' is not a number; ignored", name, view), "Bench");
        return std::nullopt;
    }
    return value;
}
} // namespace

std::optional<EngineBench::Config> EngineBench::readConfig(uint32_t defaultMeasuredFrames)
{
    const uint32_t frames = readEnvNumber<uint32_t>("ENGINE_BENCH_FRAMES").value_or(defaultMeasuredFrames);
    if (frames == 0) {
        return std::nullopt;
    }
    const auto readEnvString = [](const char* name) {
        const char* text = std::getenv(name);
        return text != nullptr ? std::string(text) : std::string{};
    };
    return Config{
        .measuredFrames = frames,
        .fieldSide = readEnvNumber<int>("ENGINE_BENCH_FIELD").value_or(0),
        .isFieldMoving = readEnvNumber<int>("ENGINE_BENCH_FIELD_MOVE").value_or(0) != 0,
        .modelPath = readEnvString("ENGINE_BENCH_MODEL"),
        .pose = readEnvString("ENGINE_BENCH_POSE"),
    };
}

void EngineBench::setFieldMotion(EntityId firstEntity, uint32_t entityCount, uint32_t side, float centerX,
                                 float cellSize) noexcept
{
    fieldFirstEntity = firstEntity;
    fieldEntityCount = entityCount;
    fieldSide = side;
    fieldCenterX = centerX;
    fieldCellSize = cellSize;
}

void EngineBench::stepField(ObjectStorage& storage)
{
    if (!config.isFieldMoving || fieldEntityCount == 0 || fieldSide == 0 || fieldCellSize <= 0.0f) {
        return;
    }
    const float span = static_cast<float>(fieldSide) * fieldCellSize;
    const float minX = fieldCenterX - 0.5f * span;
    const float maxX = fieldCenterX + 0.5f * span;
    const float step = 0.25f * fieldCellSize;
    for (uint32_t i = 0; i < fieldEntityCount; ++i) {
        const EntityId id = fieldFirstEntity + i;
        if (id >= storage.size()) {
            break;
        }
        const uint32_t row = i / fieldSide;
        const float dir = (row & 1u) == 0 ? 1.0f : -1.0f;
        float& x = storage.transforms[id].position.x;
        x += dir * step;
        bool wrapped = false;
        if (x > maxX) {
            x -= span;
            wrapped = true;
        } else if (x < minX) {
            x += span;
            wrapped = true;
        }
        if (wrapped) {
            storage.prevModelMatrices[id] = computeModelMatrix(storage.transforms[id]);
        }
    }
}

bool EngineBench::onFrame(const GpuTimer& timer, const MeshletCullStats& cullStats,
                          const RayTracingScene::Stats& rtStats, const RtxdiContext::LightPrepStats& lightPrep,
                          const Renderer::CpuStats& cpuStats)
{
    ++frame;
    if (frame <= kWarmupFrames || (frame - kWarmupFrames) % GpuTimer::kHistorySize != 0) {
        return false;
    }
    // one sample per full history window, so windows never overlap
    for (size_t i = 0; i < kGpuTimerScopeCount; ++i) {
        const float ms = timer.averageMs(static_cast<GpuTimerScope>(i));
        sums[i] += ms;
        minimums[i] = std::min(minimums[i], ms);
    }
    ++windowCount;
    if (windowCount * GpuTimer::kHistorySize < config.measuredFrames) {
        return false;
    }
    logResults(timer, cullStats, rtStats, lightPrep, cpuStats);
    return true;
}

void EngineBench::logResults(const GpuTimer& timer, const MeshletCullStats& cullStats,
                             const RayTracingScene::Stats& rtStats, const RtxdiContext::LightPrepStats& lightPrep,
                             const Renderer::CpuStats& cpuStats) const
{
    logInfo(std::format("Bench: field side {}{}, {} windows of {} frames after {} warm-up frames", config.fieldSide,
                        config.isFieldMoving ? " (moving)" : "", windowCount, GpuTimer::kHistorySize, kWarmupFrames),
            "Bench");
    double total = 0.0;
    for (size_t i = 0; i < kGpuTimerScopeCount; ++i) {
        const auto scope = static_cast<GpuTimerScope>(i);
        const double mean = sums[i] / windowCount;
        // GPU idle, not GPU work
        if (scope != GpuTimerScope::AcquireWait) {
            total += mean;
        }
        logInfo(std::format("  {:<22} mean {:8.4f} ms   best window {:8.4f} ms", gpuTimerScopeName(scope), mean,
                            minimums[i]),
                "Bench");
    }
    logInfo(std::format("  {:<22} mean {:8.4f} ms", "GPU total", total), "Bench");
    // async compute overlaps the frame: last rolling window, not in the total
    logInfo(std::format("  async compute: light prep {:.4f} ms (world {:.4f}, mips {:.4f}, presample {:.4f}), "
                        "RT instances {:.4f} ms, PTLAS build {:.4f} ms, {} instances, {} partitions, {} written",
                        lightPrep.totalMs, lightPrep.worldLightsMs, lightPrep.pdfMipsMs, lightPrep.presampleMs,
                        rtStats.instanceWriteMs, rtStats.tlasBuildMs, rtStats.instances, rtStats.partitions,
                        rtStats.writtenInstances),
            "Bench");
    logInfo(std::format("  CPU frame: drawFrame {:.4f} ms (waits {:.4f}, record {:.4f}, FrameData::write {:.4f}, "
                        "submit {:.4f})",
                        cpuStats.drawFrameMs, cpuStats.waitsMs, cpuStats.recordCmdsMs, cpuStats.frameDataWriteMs,
                        cpuStats.submitMs),
            "Bench");
    if (timer.isSupported() && lightPrep.lastStartTick != 0 && rtStats.lastEndTick != 0) {
        const double tickMs = static_cast<double>(timer.timestampPeriod()) * 1e-6;
        const uint64_t gfxStart = timer.lastBeginTick();
        const uint64_t gfxResolveEnd = timer.lastEndTick(GpuTimerScope::Resolve);
        const uint64_t cmpStart = lightPrep.lastStartTick;
        const uint64_t cmpEnd = rtStats.lastEndTick;
        const double cmpDurationMs = static_cast<double>(cmpEnd - cmpStart) * tickMs;
        const double preRayMs = static_cast<double>(gfxResolveEnd - gfxStart) * tickMs;
        const double startOffsetMs = static_cast<double>(static_cast<int64_t>(cmpStart - gfxStart)) * tickMs;
        const uint64_t overlapStart = std::max(gfxStart, cmpStart);
        const uint64_t overlapEnd = std::min(gfxResolveEnd, cmpEnd);
        const double overlapMs = overlapEnd > overlapStart ? static_cast<double>(overlapEnd - overlapStart) * tickMs : 0.0;
        const double spillMs = cmpEnd > gfxResolveEnd ? static_cast<double>(cmpEnd - gfxResolveEnd) * tickMs : 0.0;
        logInfo(std::format("  async overlap (last frame): compute {:.4f} ms (offset {:+.4f} ms vs gfxStart), "
                            "pre-ray gfx {:.4f} ms, overlap {:.4f} ms, spill past resolve {:.4f} ms",
                            cmpDurationMs, startOffsetMs, preRayMs, overlapMs, spillMs),
                "Bench");
    }
    // a static view gives the same counts every frame
    const GpuCullCounters& early = cullStats.phase(CullPhase::Early);
    const GpuCullCounters& late = cullStats.phase(CullPhase::Late);
    logInfo(std::format("  meshlets visible {} + {} late, occluded {} early / {} late, history {}",
                        visibleMeshletCount(early), visibleMeshletCount(late), early.occlusionCulled,
                        late.occlusionCulled, occlusionHistoryName(cullStats.history)),
            "Bench");
}
