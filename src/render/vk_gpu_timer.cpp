#include "vk_gpu_timer.hpp"

#include "util/vk_tracy.hpp"

#include <numeric>

GpuTimer::GpuTimer(const Device& device)
{
    const vk::PhysicalDeviceLimits& limits = device.capabilities.properties2.properties.limits;
    const auto queueFamilies = device.physicalDevice.getQueueFamilyProperties2();
    if (limits.timestampPeriod <= 0.0f || queueFamilies[device.graphicsIndex].queueFamilyProperties.timestampValidBits == 0) {
        return;
    }
    timestampPeriodNs = limits.timestampPeriod;
    pool = vk::raii::QueryPool(device.vkdevice, vk::QueryPoolCreateInfo{
                                                    .queryType = vk::QueryType::eTimestamp,
                                                    .queryCount = kQueriesPerFrame * MAX_FRAMES_IN_FLIGHT,
                                                });
}

void GpuTimer::collect(uint32_t frameSlot)
{
    if (!isSupported() || !isWritten[frameSlot]) {
        return;
    }
    isWritten[frameSlot] = false;
    const auto [result, ticks] = pool.getResults<uint64_t>(frameSlot * kQueriesPerFrame, kQueriesPerFrame,
                                                           kQueriesPerFrame * sizeof(uint64_t), sizeof(uint64_t),
                                                           vk::QueryResultFlagBits::e64);
    if (result != vk::Result::eSuccess) {
        return;
    }
    for (size_t i = 0; i < kQueriesPerFrame; ++i) {
        lastTicks[i] = ticks[i];
    }
    for (size_t scope = 0; scope < kGpuTimerScopeCount; ++scope) {
        const float ms = static_cast<float>(ticks[scope + 1] - ticks[scope]) * timestampPeriodNs * 1e-6f;
        histories[scope][historyHead] = ms;
        TracyPlot(gpuTimerScopeName(static_cast<GpuTimerScope>(scope)), static_cast<double>(ms));
    }
    historyHead = (historyHead + 1) % kHistorySize;
    historyCount = std::min(historyCount + 1, kHistorySize);
}

void GpuTimer::begin(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot)
{
    if (!isSupported()) {
        return;
    }
    frame = frameSlot;
    cmd.resetQueryPool(*pool, frameSlot * kQueriesPerFrame, kQueriesPerFrame);
    cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *pool, frameSlot * kQueriesPerFrame);
    isWritten[frameSlot] = true;
}

void GpuTimer::end(const vk::raii::CommandBuffer& cmd, GpuTimerScope scope) const
{
    if (!isSupported()) {
        return;
    }
    // all commands: the span covers every earlier command, not just one stage
    cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *pool,
                        frame * kQueriesPerFrame + std::to_underlying(scope) + 1);
}

float GpuTimer::averageMs(GpuTimerScope scope) const
{
    if (historyCount == 0) {
        return 0.0f;
    }
    // unfilled slots are zero, so summing the whole ring is exact
    const auto& samples = histories[std::to_underlying(scope)];
    return std::reduce(samples.begin(), samples.end(), 0.0f) / static_cast<float>(historyCount);
}
