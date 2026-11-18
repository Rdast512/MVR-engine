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
#include "vk_dlss.hpp"
#include "vk_post.hpp"
#include "vk_gpu_timer.hpp"
#include "vk_hiz.hpp"
#include "vk_indirect.hpp"
#include "vk_nrc.hpp"
#include "debug_view.hpp"
#include "vk_meshlet_culling.hpp"
#include "vk_render_targets.hpp"
#include "vk_rt_scene.hpp"
#include "vk_rtxdi.hpp"
#include "vk_world_lights.hpp"
#include "vk_shader_objects.hpp"
#include "util/vk_utils.hpp"

#include <array>
#include <chrono>
#include <optional>
#include <span>

class VkTracyContext;

class Renderer
{
public:
    Renderer(Device& device, SwapChain& swapChain, const VkAllocator& allocator, FrameSync& frameSync,
             const Scene& scene, const AssetBuffers& assets, FrameData& frameData, DescriptorManager& descriptorManager,
             ShaderObjects& shaderObjects, Camera& camera, const NrcLibrary& nrcLibrary,
             VkTracyContext* tracyContext = nullptr);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    void setTracyContext(VkTracyContext* tracyContextIn);
    void rebuildSwapchainResources();
    // a window resize, or an out-of-date or suboptimal swapchain found by drawFrame
    void requestSwapchainRecreate() noexcept { isSwapchainRecreatePending = true; }
    [[nodiscard]] bool isSwapchainRecreateRequested() const noexcept { return isSwapchainRecreatePending; }
    // waits idle, replaces the swapchain and rebuilds the resources sized by it; between frames only
    void recreateSwapchain();
    void drawFrame();
    void waitIdle() const;

    uint32_t currentFrame = 0;
    // monotonic; VMA refreshes its memory budget per frame index
    uint32_t frameNumber = 0;
    // read once per recorded frame; the UI changes it between frames
    DebugViewSettings debugView;

    [[nodiscard]] MeshletCulling& getCulling() noexcept { return culling; }
    [[nodiscard]] RtxdiContext& getRtxdi() noexcept { return rtxdi; }
    [[nodiscard]] NrcCache& getNrc() noexcept { return nrc; }
    [[nodiscard]] WorldLights& getWorldLights() noexcept { return worldLights; }
    [[nodiscard]] RayTracingScene& getRayTracingScene() noexcept { return rtScene; }
    [[nodiscard]] Dlss& getDlss() noexcept { return dlss; }
    [[nodiscard]] PostProcess& getPost() noexcept { return post; }
    // the next Lit frame starts DLSS-RR without history
    void resetDlssHistory() noexcept { isDlssHistoryValid = false; }
    // Lit with at least one light: present shows DLSS-RR. Lit without lights would be black, so it shows the unlit
    // albedo of the debug view pass, as before H1
    [[nodiscard]] bool isDlssPresented() const
    {
        return debugView.view == DebugView::Lit && worldLights.hasLights();
    }
    [[nodiscard]] const GpuTimer& getGpuTimer() const noexcept { return gpuTimer; }
    [[nodiscard]] uint32_t getHiZMipCount() const noexcept { return hiz.mipCount(); }
    [[nodiscard]] RayTracingScene::Stats getRayTracingStats() const noexcept { return rtScene.getStats(); }

    // ENGINE_MOTION_TEST: passes/motion_check.slang runs after the debug view of every later frame
    void enableMotionCheck();
    // the frame recorded last; complete only after its fence, e.g. a waitIdle
    [[nodiscard]] const GpuMotionCheckStats& getLastMotionCheck() const;

    // ENGINE_RT_CHECK: passes/rt_check.slang runs after the debug view of every later frame
    void enableRayCheck();
    // the frame recorded last; complete only after its fence, e.g. a waitIdle
    [[nodiscard]] const GpuRayCheckStats& getLastRayCheck() const;

    // ENGINE_GUIDE_CAPTURE: the next recorded frame copies the presented image (the DLSS-RR output for Lit, else the
    // display image) to host memory after the debug view
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
    // submitted before the image is acquired: cull, raster, Hi-Z, resolve
    void recordFrameCommands(vk::raii::CommandBuffer& cmd);
    // submitted after the acquire with the present commands, waiting for this frame's TLAS build: the passes that may
    // trace rays (ReSTIR DI, indirect + NRC), reconstruction, debug view, self-tests, display capture. Records into
    // raySegments
    void recordRayCommands();
    // resets, begins and binds the heaps of the next ray segment
    [[nodiscard]] const vk::raii::CommandBuffer& beginRaySegment();
    // ends cmd and records one NRC / NGX call alone in the next segment, between heap binds; returns the segment opened
    // after it
    template <typename RecordSdk>
    [[nodiscard]] const vk::raii::CommandBuffer& recordIsolated(const vk::raii::CommandBuffer& cmd,
                                                                RecordSdk&& recordSdk);
    // submitted after the acquire, waiting on its semaphore: present pass and UI
    void recordPresentCommands(vk::raii::CommandBuffer& cmd, uint32_t imageIndex);
    // before the next queue operation or NRC / NGX call: one issued while NRC's kernels or the DLSS-RR after them run
    // loses the device, and GPU-side waits do not prevent it (considerations C-G3-6). Waits on the host for the last
    // NRC ray submit, then presents the pending frame
    void finishPreviousFrame();
    // async compute: RT instances + PTLAS update; waits for the previous frame's rays on FrameSync::rayTimeline,
    // signals frameNumber on FrameSync::computeTimeline
    void submitComputeCommands();
    void recordPhase(vk::raii::CommandBuffer& cmd, CullPhase phase);
    void recordResolve(const vk::raii::CommandBuffer& cmd);
    void recordDirectLight(const vk::raii::CommandBuffer& cmd);
    // NRC begin frame -> update paths -> indirect (query) pass -> QueryAndTrain -> NRC resolve into the GI reservoirs
    // -> ReSTIR GI; returns the ray segment open after it
    [[nodiscard]] const vk::raii::CommandBuffer& recordIndirect(const vk::raii::CommandBuffer& cmd);
    // ReSTIR GI temporal (skipped by the reference) -> spatial + final shading into indirectRadiance
    void recordIndirectResampling(const vk::raii::CommandBuffer& cmd);
    // after a vendor SDK call (NGX, NRC) that bound its own descriptor state
    void bindHeaps(const vk::raii::CommandBuffer& cmd) const;
    // combine -> noisyColor; DLSS-RR -> exposure -> tonemap into the post target when the Lit view is shown; returns
    // the ray segment open after it
    [[nodiscard]] const vk::raii::CommandBuffer& recordReconstruction(const vk::raii::CommandBuffer& cmd);
    void recordDebugView(const vk::raii::CommandBuffer& cmd);
    void recordMotionCheck(const vk::raii::CommandBuffer& cmd);
    void recordRayCheck(const vk::raii::CommandBuffer& cmd);
    // SER pipeline against traceClosest, reorder off then on
    // grows this slot's readback buffer; the slot's fence has been waited
    void recordDisplayCapture(const vk::raii::CommandBuffer& cmd);
    void recordPresent(vk::raii::CommandBuffer& cmd, uint32_t imageIndex);
    // DebugView::HiZPyramid: covers the present pass with the pyramid of the final depth
    void recordHiZView(const vk::raii::CommandBuffer& cmd) const;
    // one thread per render pixel, FramePushData
    void dispatchFullscreen(const vk::raii::CommandBuffer& cmd, const vk::raii::ShaderEXT& shader) const;
    [[nodiscard]] uint32_t previousFrameSlot() const noexcept
    {
        return (currentFrame + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
    }
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
    RayTracingScene rtScene;
    WorldLights worldLights;
    RtxdiContext rtxdi;
    // the radiance cache behind the indirect pass's samples
    NrcCache nrc;
    // NRC commands went into this frame's ray segments: EndFrame follows their submit
    bool isNrcRecorded = false;
    // this frame's ray command buffers; the first raySegmentCount are recorded, in submission order
    std::span<vk::raii::CommandBuffer> raySegments;
    uint32_t raySegmentCount = 0;
    // FrameSync::rayTimeline value of the last ray submit with NRC commands; 0 before the first
    uint64_t lastNrcRaySubmit = 0;
    // a frame's present, issued once the next frame's frame commands are recorded (finishPreviousFrame)
    struct PendingPresent
    {
        uint32_t imageIndex = 0;
        vk::Semaphore renderSemaphore;
    };
    std::optional<PendingPresent> pendingPresent;
    Dlss dlss;
    PostProcess post;
    // false after a resize or a frame without the Lit view: the next evaluation resets DLSS history
    bool isDlssHistoryValid = false;
    // world light rows of the last frame; a change (load, unload, sun toggle) resets history, since reservoirs hold
    // light indices
    uint32_t lastLightCount = 0;
    bool isLightSetChanged = false;
    std::chrono::steady_clock::time_point lastFrameTime{};
    // per frame slot: the timeline value its last async compute submit signals
    std::array<uint64_t, MAX_FRAMES_IN_FLIGHT> computeTimelineValues{};
    GpuTimer gpuTimer;
    VkTracyContext* tracyContext = nullptr;
    // per frame slot; empty unless enableMotionCheck
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> motionCheckStats;
    // per frame slot; empty unless enableRayCheck
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> rayCheckStats;
    uint32_t lastRecordedFrame = 0;
    // per frame slot; grown on the first capture at a larger extent
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> displayCaptures;
    std::array<vk::DeviceSize, MAX_FRAMES_IN_FLIGHT> displayCaptureCapacities{};
    // extent of the capture the slot recorded last; nullopt when it recorded none
    std::array<std::optional<vk::Extent2D>, MAX_FRAMES_IN_FLIGHT> displayCaptureExtents;
    bool isDisplayCapturePending = false;
    bool isSwapchainRecreatePending = false;
};
