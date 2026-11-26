#pragma once

#include "../Constants.h"
#include "check_types.hpp"
#include "render/frame_hook.hpp"
#include "util/vk_utils.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

class Device;
class VkAllocator;
class ShaderObjects;

// the presented image of one frame, read back for the runners
struct DisplayCapture
{
    vk::Extent2D extent;
    // RenderTargets::kDisplayFormat texels, linear, row-major without padding
    std::span<const uint16_t> rgba16f;
};

// the renderer's frame hook in a checks build: motion check, ray check and display capture after the debug view
class CheckPasses final : public FrameHook
{
public:
    CheckPasses(const Device& device, const VkAllocator& allocator, const ShaderObjects& shaderObjects);
    ~CheckPasses() override;

    // ENGINE_MOTION_TEST: checks/motion_check.slang runs after the debug view of every later frame
    void enableMotionCheck();
    // the frame recorded last; complete only after its fence, e.g. a waitIdle
    [[nodiscard]] const GpuMotionCheckStats& getLastMotionCheck() const;

    // ENGINE_RT_CHECK: checks/rt_check.slang runs after the debug view of every later frame
    void enableRayCheck();
    // the frame recorded last; complete only after its fence, e.g. a waitIdle
    [[nodiscard]] const GpuRayCheckStats& getLastRayCheck() const;

    // the next recorded frame copies the presented image to host memory after the debug view
    void requestDisplayCapture() noexcept { isDisplayCapturePending = true; }
    // the frame recorded last, if it captured; complete only after its fence, e.g. a waitIdle
    [[nodiscard]] std::optional<DisplayCapture> getLastDisplayCapture() const;

    void recordAfterDebugView(const FrameHookContext& context) override;
    [[nodiscard]] bool tracesRays() const noexcept override { return rayCheckStats.front().mapped != nullptr; }

private:
    // reads what the debug view reads, after the same barrier; only the stats buffer is written
    static void recordStatsPass(const FrameHookContext& context, const vk::raii::ShaderEXT& shader,
                                vk::DeviceAddress stats);
    // grows this slot's readback buffer; the slot's fence has been waited
    void recordDisplayCapture(const FrameHookContext& context);

    const Device& device;
    const VkAllocator& allocator;
    const ShaderObjects& shaderObjects;
    vk::raii::ShaderEXT motionCheckShader = nullptr;
    vk::raii::ShaderEXT rayCheckShader = nullptr;
    // per frame slot; empty unless enabled. Zeroed by the host before the slot records
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> motionCheckStats;
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> rayCheckStats;
    // per frame slot; grown on the first capture at a larger extent
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> displayCaptures;
    std::array<vk::DeviceSize, MAX_FRAMES_IN_FLIGHT> displayCaptureCapacities{};
    // extent of the capture the slot recorded last; nullopt when it recorded none
    std::array<std::optional<vk::Extent2D>, MAX_FRAMES_IN_FLIGHT> displayCaptureExtents;
    uint32_t lastRecordedSlot = 0;
    bool isDisplayCapturePending = false;
};
