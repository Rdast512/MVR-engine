#pragma once

#include <cstdint>
#include <vulkan/vulkan_raii.hpp>

// square workgroup of the fullscreen compute passes; matches passes/classify.slang, passes/debug_view.slang and the
// passes/rtxdi_*.slang
inline constexpr uint32_t kFullscreenTile = 8;

struct FrameHookContext
{
    const vk::raii::CommandBuffer& cmd;
    uint32_t frameSlot;
    vk::DeviceAddress frameRoot;
    vk::Extent2D renderExtent;
    // the image present reads, in eGeneral: post at display extent when DLSS-RR presents, else the display image
    vk::Image presentedImage;
    vk::Extent2D presentedExtent;
};

// the renderer's one extension point after the debug view; non-owning, set once before the first frame
class FrameHook
{
public:
    FrameHook() = default;
    FrameHook(const FrameHook&) = delete;
    FrameHook& operator=(const FrameHook&) = delete;
    FrameHook(FrameHook&&) = delete;
    FrameHook& operator=(FrameHook&&) = delete;
    virtual ~FrameHook() = default;

    // after the debug view and its guide barrier, before present; reads what the debug view reads. The slot's fence
    // has been waited
    virtual void recordAfterDebugView(const FrameHookContext& context) = 0;
    // traces rays in recordAfterDebugView: the frame's ray-timeline signal moves to the post-acquire submit
    [[nodiscard]] virtual bool tracesRays() const noexcept = 0;
};
