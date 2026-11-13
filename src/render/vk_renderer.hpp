#pragma once
#include "../Constants.h"
#include "core/vk_allocator.hpp"
#include "core/vk_asset_buffers.hpp"
#include "core/vk_descriptors.hpp"
#include "core/vk_frame_data.hpp"
#include "core/vk_frame_sync.hpp"
#include "core/vk_swapchain.hpp"
#include "scene/vk_camera.hpp"
#include "scene/vk_scene.hpp"
#include "vk_dgc.hpp"
#include "vk_gpu_timer.hpp"
#include "vk_hiz.hpp"
#include "debug_view.hpp"
#include "vk_meshlet_culling.hpp"
#include "vk_render_targets.hpp"
#include "vk_shader_objects.hpp"
#include "util/vk_utils.hpp"

#include <array>
#include <optional>
#include <span>

class VkTracyContext;

class Renderer
{
public:
    Renderer(Device& device, SwapChain& swapChain, const VkAllocator& allocator, FrameSync& frameSync,
             const Scene& scene, const AssetBuffers& assets, FrameData& frameData, DescriptorManager& descriptorManager,
             ShaderObjects& shaderObjects, Camera& camera, VkTracyContext* tracyContext = nullptr);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    void setTracyContext(VkTracyContext* tracyContextIn);
    void rebuildSwapchainResources();
    void drawFrame();
    void waitIdle() const;

    uint32_t currentFrame = 0;
    // monotonic; VMA refreshes its memory budget per frame index
    uint32_t frameNumber = 0;
    // read once per recorded frame; the UI changes it between frames
    DebugViewSettings debugView;

    [[nodiscard]] MeshletCulling& getCulling() noexcept { return culling; }
    [[nodiscard]] const GpuTimer& getGpuTimer() const noexcept { return gpuTimer; }
    [[nodiscard]] uint32_t getHiZMipCount() const noexcept { return hiz.mipCount(); }

    // ENGINE_MOTION_TEST: passes/motion_check.slang runs after the debug view of every later frame
    void enableMotionCheck();
    // the frame recorded last; complete only after its fence, e.g. a waitIdle
    [[nodiscard]] const GpuMotionCheckStats& getLastMotionCheck() const;

    // ENGINE_GUIDE_CAPTURE: the next recorded frame copies the display image to host memory after the debug view
    void requestDisplayCapture() noexcept { isDisplayCapturePending = true; }
    struct DisplayCapture
    {
        vk::Extent2D extent;
        // RenderTargets::kDisplayFormat texels, linear, row-major without padding
        std::span<const uint16_t> rgba16f;
    };
    // the frame recorded last, if it captured; complete only after its fence, e.g. a waitIdle
    [[nodiscard]] std::optional<DisplayCapture> getLastDisplayCapture() const;

private:
    // submitted before the image is acquired: cull, raster, Hi-Z, resolve, debug view
    void recordFrameCommands(vk::raii::CommandBuffer& cmd);
    // submitted after the acquire, waiting on its semaphore: present pass and UI
    void recordPresentCommands(vk::raii::CommandBuffer& cmd, uint32_t imageIndex);
    void recordPhase(vk::raii::CommandBuffer& cmd, CullPhase phase);
    void recordResolve(const vk::raii::CommandBuffer& cmd);
    void recordDebugView(const vk::raii::CommandBuffer& cmd);
    void recordMotionCheck(const vk::raii::CommandBuffer& cmd);
    // grows this slot's readback buffer; the slot's fence has been waited
    void recordDisplayCapture(const vk::raii::CommandBuffer& cmd);
    void recordPresent(vk::raii::CommandBuffer& cmd, uint32_t imageIndex);
    // DebugView::HiZPyramid: covers the present pass with the pyramid of the final depth
    void recordHiZView(const vk::raii::CommandBuffer& cmd) const;
    // one thread per render pixel, FramePushData
    void dispatchFullscreen(const vk::raii::CommandBuffer& cmd, const vk::raii::ShaderEXT& shader) const;
    static void setViewport(const vk::raii::CommandBuffer& cmd, vk::Extent2D extent);

    Device& device;
    SwapChain& swapChain;
    const VkAllocator& allocator;
    FrameSync& frameSync;
    FrameData& frameData;
    DescriptorManager& descriptorManager;
    Camera& camera;
    const ShaderObjects& shaderObjects;
    DeviceGeneratedCommands dgc;
    // after dgc: writes into its sequence buffers
    MeshletCulling culling;
    HiZPyramid hiz;
    // owns the depth attachment hiz is built from; recreated before it
    RenderTargets targets;
    GpuTimer gpuTimer;
    VkTracyContext* tracyContext = nullptr;
    // per frame slot; empty unless enableMotionCheck
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> motionCheckStats;
    uint32_t lastRecordedFrame = 0;
    // per frame slot; grown on the first capture at a larger extent
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> displayCaptures;
    std::array<vk::DeviceSize, MAX_FRAMES_IN_FLIGHT> displayCaptureCapacities{};
    // extent of the capture the slot recorded last; nullopt when it recorded none
    std::array<std::optional<vk::Extent2D>, MAX_FRAMES_IN_FLIGHT> displayCaptureExtents;
    bool isDisplayCapturePending = false;
};
