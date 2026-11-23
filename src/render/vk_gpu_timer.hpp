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
    EarlyCull = 0,
    EarlyPreprocess,
    // the opaque bin
    EarlyDraw,
    // the alpha-tested bin (spec/m6/d3_alpha_raster_bin.md)
    EarlyAlphaDraw,
    EarlyHiZ,
    LateCull,
    LatePreprocess,
    LateDraw,
    LateAlphaDraw,
    LateHiZ,
    // material bin per pixel, tile lists and the resolve's DGC sequences (spec/m7/e1_material_binning.md)
    Classify,
    // DGC preprocess and execute of the material-bin resolve shaders
    Resolve,
    // resolve end to the ray command buffer's first pass: the gap until the CPU submits the ray work plus its
    // computeTimeline wait for this frame's light prep and PTLAS build (F4). Not the TLAS stall alone
    RayStartWait,
    // ReSTIR DI initial sampling (after RayStartWait, F4)
    RtxdiInitial,
    RtxdiTemporal,
    // spatial resampling and shading
    RtxdiShade,
    // NRC begin frame + training paths (spec/m4/g3_nrc.md)
    NrcUpdate,
    // SER bounce ray into the initial ReSTIR GI reservoirs (spec/m4/g2_indirect_pass.md), NRC's query paths
    Indirect,
    // NRC QueryAndTrain: inference at the queries, one training step
    NrcTrain,
    // cache term into the GI reservoirs
    NrcResolve,
    // ReSTIR GI temporal resampling + boiling filter (spec/m4/g5_restir_gi.md)
    GiTemporal,
    // ReSTIR GI spatial resampling and final shading into indirectRadiance
    GiShade,
    Combine,
    // thin-walled glass layers over noisyColor (m8 I13); zero without glass instances
    Glass,
    // zero while a debug view is shown: the DLSS-RR evaluation is skipped
    Dlss,
    // exposure histogram, adaptation and tonemap; zero with DLSS-RR
    Post,
    DebugView,
    // ENGINE_MOTION_TEST and ENGINE_RT_CHECK compute passes; zero unless enabled
    SelfTests,
    // GPU idle until the swapchain image is acquired; not GPU work, so not in the total
    AcquireWait,
    Present,
    Ui,
    Count,
};
inline constexpr size_t kGpuTimerScopeCount = std::to_underlying(GpuTimerScope::Count);

// also the Tracy plot names
[[nodiscard]] constexpr const char* gpuTimerScopeName(GpuTimerScope scope)
{
    switch (scope) {
    case GpuTimerScope::EarlyCull:
        return "Early cull";
    case GpuTimerScope::EarlyPreprocess:
        return "Early DGC preprocess";
    case GpuTimerScope::EarlyDraw:
        return "Early mesh draw";
    case GpuTimerScope::EarlyAlphaDraw:
        return "Early alpha draw";
    case GpuTimerScope::EarlyHiZ:
        return "Early Hi-Z build";
    case GpuTimerScope::LateCull:
        return "Late cull";
    case GpuTimerScope::LatePreprocess:
        return "Late DGC preprocess";
    case GpuTimerScope::LateDraw:
        return "Late mesh draw";
    case GpuTimerScope::LateAlphaDraw:
        return "Late alpha draw";
    case GpuTimerScope::LateHiZ:
        return "Late Hi-Z build";
    case GpuTimerScope::Classify:
        return "Material classify";
    case GpuTimerScope::Resolve:
        return "Vis-buffer resolve";
    case GpuTimerScope::RayStartWait:
        return "Ray start wait";
    case GpuTimerScope::RtxdiInitial:
        return "ReSTIR DI initial";
    case GpuTimerScope::RtxdiTemporal:
        return "ReSTIR DI temporal";
    case GpuTimerScope::RtxdiShade:
        return "ReSTIR DI shade";
    case GpuTimerScope::NrcUpdate:
        return "NRC update";
    case GpuTimerScope::Indirect:
        return "Indirect";
    case GpuTimerScope::NrcTrain:
        return "NRC train";
    case GpuTimerScope::NrcResolve:
        return "NRC resolve";
    case GpuTimerScope::GiTemporal:
        return "ReSTIR GI temporal";
    case GpuTimerScope::GiShade:
        return "ReSTIR GI shade";
    case GpuTimerScope::Combine:
        return "Combine";
    case GpuTimerScope::Glass:
        return "Glass";
    case GpuTimerScope::Dlss:
        return "DLSS-RR";
    case GpuTimerScope::Post:
        return "Post";
    case GpuTimerScope::DebugView:
        return "Debug view";
    case GpuTimerScope::SelfTests:
        return "Self-tests";
    case GpuTimerScope::AcquireWait:
        return "Acquire wait";
    case GpuTimerScope::Present:
        return "Present";
    case GpuTimerScope::Ui:
        return "ImGui";
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
    [[nodiscard]] float timestampPeriod() const noexcept { return timestampPeriodNs; }
    // raw GPU clock ticks of the most recently collected frame (index 0 = begin, scope + 1 = end of scope)
    [[nodiscard]] uint64_t lastBeginTick() const noexcept { return lastTicks[0]; }
    [[nodiscard]] uint64_t lastEndTick(GpuTimerScope scope) const noexcept
    {
        return lastTicks[std::to_underlying(scope) + 1];
    }
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
    std::array<uint64_t, kQueriesPerFrame> lastTicks{};
    size_t historyHead = 0;
    size_t historyCount = 0;
};
