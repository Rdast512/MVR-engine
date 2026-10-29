#pragma once

#include "../Constants.h"
#include "core/vk_device.hpp"

#include <array>
#include <cstdint>
#include <utility>
#include <vulkan/vulkan_raii.hpp>

// consecutive GPU spans of one frame; each ends where the next begins
enum class GpuTimerScope : uint32_t
{
    Cull = 0,
    Preprocess,
    Draw,
    Count,
};
inline constexpr size_t kGpuTimerScopeCount = std::to_underlying(GpuTimerScope::Count);

[[nodiscard]] constexpr const char* gpuTimerScopeName(GpuTimerScope scope)
{
    switch (scope) {
    case GpuTimerScope::Cull:
        return "Instance + meshlet cull";
    case GpuTimerScope::Preprocess:
        return "DGC preprocess";
    case GpuTimerScope::Draw:
        return "Mesh draw";
    case GpuTimerScope::Count:
        break;
    }
    return "";
}

// timestamp queries per frame slot; results are read once the slot's fence has signaled
class GpuTimer
{
public:
    static constexpr size_t kHistorySize = 120;

    explicit GpuTimer(const Device& device);

    // after the slot's fence: appends its last results to the history
    void collect(uint32_t frameSlot);
    // outside a render pass, before the first timed command
    void begin(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot);
    // ends the scope; the next scope starts here
    void end(const vk::raii::CommandBuffer& cmd, GpuTimerScope scope) const;

    [[nodiscard]] bool isSupported() const noexcept { return timestampPeriodNs > 0.0f; }
    [[nodiscard]] float averageMs(GpuTimerScope scope) const;
    // ring of kHistorySize samples ending at historyOffset()
    [[nodiscard]] const std::array<float, kHistorySize>& history(GpuTimerScope scope) const
    {
        return histories[std::to_underlying(scope)];
    }
    [[nodiscard]] int historyOffset() const noexcept { return static_cast<int>(historyHead); }

private:
    static constexpr uint32_t kQueriesPerFrame = kGpuTimerScopeCount + 1;

    vk::raii::QueryPool pool = nullptr;
    float timestampPeriodNs = 0.0f;
    uint32_t frame = 0;
    std::array<bool, MAX_FRAMES_IN_FLIGHT> isWritten{};
    std::array<std::array<float, kHistorySize>, kGpuTimerScopeCount> histories{};
    size_t historyHead = 0;
    size_t historyCount = 0;
};
