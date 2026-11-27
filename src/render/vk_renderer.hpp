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
#include "vk_sharc.hpp"
#include "debug_view.hpp"
#include "frame_hook.hpp"
#include "denoiser_mode.hpp"
#include "glass_settings.hpp"
#include "pipeline_modes.hpp"
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
    // thin-walled glass (m8 I13); read once per recorded frame
    GlassSettings glass;
    // read once per recorded frame; changes reset indirect & DLSS-RR history for one frame
    IndirectBounceMode indirectBounce = kDefaultIndirectBounceMode;
    RadianceCacheMode radianceCache = kDefaultRadianceCacheMode;
    // read at a frame boundary (spec/features/planned/nvidia_settings_layout.md §4.1): a change rebuilds the targets
    // like a DLSS mode change, since Off renders at the display extent
    DenoiserMode denoiser = DenoiserMode::DlssRr;
    // read while the denoiser is off
    AaMode aaMode = AaMode::None;
    // Accumulation: frames averaged at most, then an exponential mean of that length; 0 = no limit
    uint32_t accumulationFrameLimit = 0;
    // set by the check runners: exposure adaptation and DLSS-RR advance by this instead of the wall clock, so a run
    // repeats frame for frame (spec/fixesM1-6/i1_check_runners.md)
    std::optional<float> fixedFrameDeltaMs;
    // no NRC work this frame: Indirect off, Cache != NRC, or ground-truth paths tracing the bounces the cache would answer
    [[nodiscard]] bool isNrcPaused() const noexcept
    {
        return indirectBounce == IndirectBounceMode::Off || radianceCache != RadianceCacheMode::Nrc ||
            rtxdi.giReferenceBounces != 0;
    }
    // SHaRC runs when Indirect is on, Cache == Sharc, and ground-truth reference paths are off
    [[nodiscard]] bool isSharcActive() const noexcept
    {
        return indirectBounce != IndirectBounceMode::Off && radianceCache == RadianceCacheMode::Sharc &&
            rtxdi.giReferenceBounces == 0;
    }
    // async-compute light preparation is skipped when no pass reads world lights or RIS buffers
    [[nodiscard]] bool isLightPreparationNeeded() const noexcept
    {
        const bool isGlassActive = rtScene.glassRowCount() > 0 && glass.isPassEnabled;
        const bool isDebugLights =
            debugView.view == DebugView::UnshadowedLights || debugView.view == DebugView::ShadowedLights;
        return rtxdi.directMode != DirectMode::Off || indirectBounce != IndirectBounceMode::Off || isGlassActive ||
            isDebugLights;
    }

    [[nodiscard]] MeshletCulling& getCulling() noexcept { return culling; }
    [[nodiscard]] RtxdiContext& getRtxdi() noexcept { return rtxdi; }
    [[nodiscard]] NrcCache& getNrc() noexcept { return nrc; }
    [[nodiscard]] SharcCache& getSharc() noexcept { return sharc; }
    [[nodiscard]] WorldLights& getWorldLights() noexcept { return worldLights; }
    [[nodiscard]] RayTracingScene& getRayTracingScene() noexcept { return rtScene; }
    [[nodiscard]] Dlss& getDlss() noexcept { return dlss; }
    [[nodiscard]] PostProcess& getPost() noexcept { return post; }
    // the next Lit frame starts DLSS-RR without history
    void resetDlssHistory() noexcept { isDlssHistoryValid = false; }
    // extra GpuFrameRoot::frameFlags bits for self-test runners (ENGINE_DLSS_CHECK, m8 I14 F25)
    uint32_t extraFrameFlags = 0;
    // GpuFrameRoot::exposure: the settings lab's DLSS-RR input scale (m8 I16)
    float dlssInputScale = 1.0f;
    // GpuFrameRoot::preDenoiserClamp; 0 is off (full_testing T11)
    float preDenoiserClamp = 0.0f;
    // GpuFrameRoot::inputHistoryWeight; 0 is off (settings lab, m8 I16)
    float inputHistoryWeight = 0.0f;
    // Lit with at least one light: present shows the post target. Lit without lights would be black, so it shows the
    // unlit albedo of the debug view pass, as before H1
    [[nodiscard]] bool isLitPresented() const { return debugView.view == DebugView::Lit && worldLights.hasLights(); }
    // DLSS-RR reconstructs the post input: the Lit view with the denoiser on
    [[nodiscard]] bool isDlssEvaluated() const
    {
        return isLitPresented() && appliedDenoiser == DenoiserMode::DlssRr;
    }
    // the Accumulation AA mode runs: post reads the running mean of noisyColor
    [[nodiscard]] bool isAccumulationActive() const
    {
        return isLitPresented() && appliedDenoiser == DenoiserMode::Off && aaMode == AaMode::Accumulation;
    }
    // frames in the running mean as of the last recorded frame; 0 outside the Accumulation mode
    [[nodiscard]] uint32_t getAccumulatedFrames() const noexcept { return accumulatedFrames; }
    // the next frame starts a new mean, as the RTXDI sample's resetAccumulation (any lighting setting edit)
    void resetAccumulation() noexcept { isAccumulationResetRequested = true; }
    // rolling 120-frame averages of CPU frame work (F8, against 2 ms budget)
    struct CpuStats
    {
        float drawFrameMs = 0.0f;
        float frameDataWriteMs = 0.0f;
        float recordCmdsMs = 0.0f;
        float submitMs = 0.0f;
        float waitsMs = 0.0f;
    };

    [[nodiscard]] const GpuTimer& getGpuTimer() const noexcept { return gpuTimer; }
    [[nodiscard]] uint32_t getHiZMipCount() const noexcept { return hiz.mipCount(); }
    [[nodiscard]] RayTracingScene::Stats getRayTracingStats() const noexcept { return rtScene.getStats(); }
    [[nodiscard]] RtxdiContext::LightPrepStats getLightPrepStats() const noexcept { return rtxdi.getLightPrepStats(); }
    [[nodiscard]] CpuStats getCpuStats() const noexcept;
    // the glass pass's counters of the last completed frame that ran it, zero otherwise (m8 I13 §6)
    [[nodiscard]] const GpuGlassCounters& getGlassCounters() const noexcept { return lastGlassCounters; }

    // non-owning; set once before the first frame
    void setFrameHook(FrameHook* hook) noexcept { frameHook = hook; }
    struct PresentedImage
    {
        // in eGeneral, RenderTargets::kDisplayFormat
        vk::Image image;
        vk::Extent2D extent;
    };
    // the image present reads: the post target at display extent when the Lit view is shown, else the display image
    [[nodiscard]] PresentedImage getPresentedImage() const;

private:
    // submitted before the image is acquired: cull, raster, Hi-Z, resolve
    void recordFrameCommands(vk::raii::CommandBuffer& cmd);
    // submitted after the acquire with the present commands, waiting for this frame's TLAS build: the passes that may
    // trace rays (ReSTIR DI, indirect + NRC), reconstruction, debug view, frame hook. Records into raySegments
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
    // DGC execute (ENGINE_DGC=1), or the indirect count draw
    void recordRasterBin(vk::raii::CommandBuffer& cmd, CullPhase phase, RasterBin bin) const;
    void recordClassify(const vk::raii::CommandBuffer& cmd);
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
    // thin-walled glass layers over noisyColor, between combine and DLSS-RR (m8 I13)
    void recordGlass(const vk::raii::CommandBuffer& cmd);
    // after the fence: the running mean's frame count (§4.4), and the accumulated target's creation and retirement
    void prepareAccumulation(bool isLightingStateChanged);
    // noisyColor into the running mean; returns the post input
    [[nodiscard]] uint32_t recordAccumulation(const vk::raii::CommandBuffer& cmd);
    void recordDebugView(const vk::raii::CommandBuffer& cmd);
    void recordPresent(vk::raii::CommandBuffer& cmd, uint32_t imageIndex);
    // DebugView::HiZPyramid: covers the present pass with the pyramid of the final depth
    void recordHiZView(const vk::raii::CommandBuffer& cmd) const;
    // one thread per render pixel, FramePushData
    // tile: the shader's square workgroup size, kFullscreenTile by default
    void dispatchFullscreen(const vk::raii::CommandBuffer& cmd, const vk::raii::ShaderEXT& shader,
                            uint32_t tile = kFullscreenTile) const;
    [[nodiscard]] uint32_t previousFrameSlot() const noexcept
    {
        return (currentFrame + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
    }
    static void setViewport(const vk::raii::CommandBuffer& cmd, vk::Extent2D extent);

    Device& device;
    SwapChain& swapChain;
    const VkAllocator& allocator;
    FrameSync& frameSync;
    const Scene& scene;
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
    // pure Vulkan BDA spatial hash radiance cache (M8.4)
    SharcCache sharc;
    bool isSharcRecorded = false;
    uint32_t lastSharcLayoutGeneration = kNoneIndex;
    // what decides the meaning of the indirect samples; a change between frames resets their history
    struct IndirectState
    {
        IndirectBounceMode bounce = kDefaultIndirectBounceMode;
        RadianceCacheMode cache = kDefaultRadianceCacheMode;
        bool isGiReference = false;
        // M8.2 BUG-UI-7: track exact bounce count so dragging "Bounces after X2" resets indirect/DLSS/Accumulation
        uint32_t giReferenceBounces = 0;
        bool arePathInfiniteLightsExact = RtxdiContext::defaultGiSettings().areInfiniteLightsShadedExactly;
        bool isSecondaryResamplingEffective = false;
        RtxdiContext::GiSettings giSettings = RtxdiContext::defaultGiSettings();

        bool operator==(const IndirectState&) const = default;
    };
    IndirectState appliedIndirect;
    bool isIndirectHistoryReset = false;
    // M8.3 PERF-D2: last frame each lazy lighting target was recorded; retired after MAX_FRAMES_IN_FLIGHT
    uint32_t lastGiStagnancyFrame = 0;
    uint32_t lastGiDuplicationFrame = 0;
    uint32_t lastRawIndirectWeightFrame = 0;
    bool appliedSpecularHitDistanceWritten = false;
    // this frame's ray command buffers; the first raySegmentCount are recorded, in submission order
    std::span<vk::raii::CommandBuffer> raySegments;
    uint32_t raySegmentCount = 0;
    // MG-P7 (I4 §3): pure-Vulkan segments before the frame's first SDK segment (NRC or DLSS-RR), submitted before
    // AcquireImage; the SDK work and all after it stay with presentCmd in one post-acquire submit (C-G3-6)
    uint32_t preSdkRaySegmentCount = 0;
    // FrameSync::rayTimeline value of the last ray submit with NRC commands; 0 before the first
    uint64_t lastNrcRaySubmit = 0;
    // C-G3-6 (NP1): isolated SDK command buffers (recordIsolated) avoid the device loss without a host wait;
    // ENGINE_NRC_HOST_WAIT=1 re-enables the wait as a fallback if a driver regresses
    bool isNrcHostWaitEnabled = false;
    // NP2 change detection: unjittered camera and sun state of the last frame NRC ran
    glm::mat4 lastNrcViewProj{0.0f};
    glm::vec3 lastNrcCameraPos{0.0f};
    WorldLights::Sun lastNrcSun{};
    bool hasNrcChangeBaseline = false;
    // a frame's present, issued once the next frame's frame commands are recorded (finishPreviousFrame)
    struct PendingPresent
    {
        uint32_t imageIndex = 0;
        vk::Semaphore renderSemaphore;
    };
    std::optional<PendingPresent> pendingPresent;
    Dlss dlss;
    // the denoiser the targets were built for; see denoiser
    DenoiserMode appliedDenoiser = DenoiserMode::DlssRr;
    // frames in the running mean, 0 while the Accumulation mode is not active
    uint32_t accumulatedFrames = 0;
    uint32_t lastAccumulationFrameLimit = 0;
    bool isAccumulationResetRequested = false;
    // the last frame that recorded the accumulate pass; its target is released once that frame retired
    uint32_t lastAccumulationFrame = 0;
    // still camera test: the unjittered view of the last frame (the RTXDI sample compares GetViewMatrix())
    glm::mat4 lastAccumulationView{0.0f};
    WorldLights::Sun lastAccumulationSun{};
    PostProcess post;
    // false after a resize or a frame without the Lit view: the next evaluation resets DLSS history
    bool isDlssHistoryValid = false;
    Dlss::Guides appliedDlssGuides{};
    NrcResolveMode appliedNrcResolveMode = NrcResolveMode::AddQueryResultToOutput;
    bool appliedJitterEnabled = true;
    enum class DlssInputAccumulationMode : uint8_t
    {
        None,
        Ema,
        Accumulate,
        Frozen,
    };
    struct DlssInputState
    {
        float scale = 1.0f;
        float clamp = 0.0f;
        float historyWeight = 0.0f;
        uint32_t flags = 0;

        bool operator==(const DlssInputState&) const = default;
    };
    DlssInputAccumulationMode appliedAccumulationMode = DlssInputAccumulationMode::None;
    DlssInputState appliedDlssInput{};
    // world light rows of the last frame; a change (load, unload, sun toggle) resets history, since reservoirs hold
    // light indices
    uint32_t lastLightCount = 0;
    bool wereTriangleLightsSampled = true;
    bool wereDiInfiniteLightsExact = RtxdiContext::defaultSettings().areInfiniteLightsShadedExactly;
    bool isLightSetChanged = false;
    bool isDiHistoryReset = false;
    std::chrono::steady_clock::time_point lastFrameTime{};
    // per frame slot: the timeline value its last async compute submit signals
    std::array<uint64_t, MAX_FRAMES_IN_FLIGHT> computeTimelineValues{};
    GpuTimer gpuTimer;
    static constexpr size_t kCpuHistorySize = GpuTimer::kHistorySize;
    std::array<std::array<float, kCpuHistorySize>, 5> cpuHistories{};
    size_t cpuHistoryHead = 0;
    size_t cpuHistoryCount = 0;
    VkTracyContext* tracyContext = nullptr;
    FrameHook* frameHook = nullptr;
    // per frame slot, GpuRayDebugCounters; empty unless kDebugRays
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> rayDebugCounters;
    // per frame slot, GpuGlassCounters; reset by the host after the slot's fence
    std::array<CounterReadback, MAX_FRAMES_IN_FLIGHT> glassCounters;
    std::array<bool, MAX_FRAMES_IN_FLIGHT> isGlassRecorded{};
    GpuGlassCounters lastGlassCounters{};
    // GlassSettings of the frame being recorded: the ray commands are recorded before the frame root is written
    uint32_t frameGlassFlags = 0;
    uint32_t frameGlassMaxLayers = 1;
    GlassSettings appliedGlass{};
    // the slot's counters hold a recorded frame's results
    std::array<bool, MAX_FRAMES_IN_FLIGHT> isRayDebugRecorded{};
    uint64_t rayDebugFrames = 0;
    uint64_t rayCandidates = 0;
    bool isSwapchainRecreatePending = false;
    bool hasDumpedLightPdfStats = false;
};
