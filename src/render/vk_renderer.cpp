#include "vk_renderer.hpp"
#include "../Constants.h"
#include "../util/vk_tracy.hpp"
#include "../util/vk_utils.hpp"
#include "push_data.hpp"
#include "static_headers/logger.hpp"
#if ENGINE_ENABLE_IMGUI
    #include "imgui.h"
    #include "imgui_impl_vulkan.h"
#endif

#include <algorithm>
#include <format>
#include <numeric>
#include <stdexcept>

namespace
{
    const vk::ImageSubresourceRange kColorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    static_assert(kFullscreenTile == kMaterialTileSize, "classify launches one group per resolve tile");
    // matches shaders/passes/rtxdi_gi_duplication.slang kDuplicationGroup
    constexpr uint32_t kGiDuplicationTile = 16;

    constexpr uint32_t divideRoundUp(uint32_t value, uint32_t divisor) { return (value + divisor - 1) / divisor; }
} // namespace

Renderer::Renderer(Device& device, SwapChain& swapChain, const VkAllocator& allocator, FrameSync& frameSync,
                   const Scene& scene, const AssetBuffers& assets, FrameData& frameData,
                   DescriptorManager& descriptorManager, ShaderObjects& shaderObjects, Camera& camera,
                   const NrcLibrary& nrcLibrary, VkTracyContext* tracyContext) :
    device(device), swapChain(swapChain), allocator(allocator), frameSync(frameSync), scene(scene), frameData(frameData),
    descriptorManager(descriptorManager), camera(camera), shaderObjects(shaderObjects),
    dgc(device, allocator, descriptorManager, shaderObjects),
    culling(device, allocator, scene, assets, frameData, shaderObjects, dgc),
    hiz(device, allocator, frameSync, descriptorManager, shaderObjects),
    targets(device, allocator, frameSync, descriptorManager),
    rtScene(device, allocator, frameSync, scene, assets, frameData, shaderObjects),
    worldLights(device, allocator, frameSync, assets, frameData, rtScene, shaderObjects), rtxdi(device, allocator, frameSync, descriptorManager, shaderObjects, worldLights),
    nrc(device, allocator, nrcLibrary), sharc(device, allocator), dlss(device, frameSync),
    post(device, allocator, shaderObjects), gpuTimer(device), tracyContext(tracyContext)
{
    const vk::DeviceSize maxPushData = device.capabilities.descriptorHeap.maxPushDataSize;
    if (kMaxPushDataBytes > maxPushData) {
        throw std::runtime_error(
            std::format("push data of {} B exceeds the device's maxPushDataSize of {} B", kMaxPushDataBytes, maxPushData));
    }
    dgc.init();
    wereDiInfiniteLightsExact = rtxdi.areDiInfiniteLightsExact();
    appliedIndirect.arePathInfiniteLightsExact = rtxdi.arePathInfiniteLightsExact();
    appliedJitterEnabled = camera.isJitterEnabled;
    if (const char* waitEnv = std::getenv("ENGINE_NRC_HOST_WAIT"); waitEnv != nullptr && *waitEnv != '\0') {
        isNrcHostWaitEnabled = std::string_view(waitEnv) != "0";
        logInfo(std::format("NRC host wait (C-G3-6): {}", isNrcHostWaitEnabled ? "on" : "off"), "Renderer");
    }
    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        glassCounters[slot] = createCounterReadback(sizeof(GpuGlassCounters), allocator.allocator, device.vkdevice,
                                                    device.queueFamilyIndices, std::format("GlassCounters_{}", slot));
    }
    if constexpr (kDebugRays) {
        for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
            rayDebugCounters[slot] =
                createHostBuffer(sizeof(GpuRayDebugCounters), allocator.allocator, device.vkdevice,
                                 device.queueFamilyIndices, std::format("RayDebugCounters_{}", slot), nullptr);
            *static_cast<GpuRayDebugCounters*>(rayDebugCounters[slot].mapped) = {};
        }
    }
}

Renderer::~Renderer()
{
    if constexpr (kDebugRays) {
        const std::string summary =
            std::format("Ray contract: {} ray-query candidates over {} frames", rayCandidates, rayDebugFrames);
        if (rayCandidates == 0) {
            logInfo(summary, "RayContract");
        } else {
            logError(summary, "RayContract");
        }
    }
    for (HostBuffer& counters : rayDebugCounters) {
        destroyHostBuffer(allocator.allocator, counters, nullptr);
    }
    for (CounterReadback& counters : glassCounters) {
        destroyCounterReadback(allocator.allocator, counters);
    }
}

Renderer::PresentedImage Renderer::getPresentedImage() const
{
    // both are RGBA16F
    if (isLitPresented()) {
        return {.image = targets.post().image, .extent = targets.getDisplayExtent()};
    }
    return {.image = targets.getDisplayImage(), .extent = targets.getExtent()};
}

void Renderer::setTracyContext(VkTracyContext* tracyContextIn) { tracyContext = tracyContextIn; }

void Renderer::rebuildSwapchainResources()
{
    frameSync.recreate(static_cast<uint32_t>(swapChain.swapChainImages.size()));
    // DLSS-RR's quality mode picks the render resolution for the swapchain's (engine spec §2.1). The feature is kept
    // while the denoiser is off, which renders at the display extent as the NVIDIA samples do
    const vk::Extent2D displayExtent = swapChain.swapChainExtent;
    const vk::Extent2D dlssRenderExtent = dlss.recreate(displayExtent);
    appliedDenoiser = denoiser;
    const vk::Extent2D renderExtent = appliedDenoiser == DenoiserMode::Off ? displayExtent : dlssRenderExtent;
    targets.recreate(renderExtent, displayExtent);
    hiz.recreate(targets);
    rtxdi.recreate(renderExtent);
    nrc.recreate(renderExtent);
    camera.setRenderExtent(renderExtent);
    isDlssHistoryValid = false;
    // the exposure image starts at 1 again
    post.requestSnap();
}

void Renderer::recreateSwapchain()
{
    // the pending present uses the old swapchain and a present semaphore the rebuild destroys; an out-of-date result
    // requests the recreate already running
    finishPreviousFrame();
    isSwapchainRecreatePending = false;
    swapChain.recreateSwapChain();
    rebuildSwapchainResources();
}

void Renderer::drawFrame()
{
    ZoneScopedN("Renderer::drawFrame");
    const auto cpuFrameStart = std::chrono::steady_clock::now();
    auto toMs = [](auto begin, auto end) {
        return std::chrono::duration<float, std::milli>(end - begin).count();
    };
    float cpuWaitsMs = 0.0f;
    float cpuRecordMs = 0.0f;
    float cpuFrameDataWriteMs = 0.0f;
    float cpuSubmitMs = 0.0f;
    auto& deviceRef = device.vkdevice;
    auto& graphicsQueue = device.graphicsQueue;
    auto& swapChainKHR = swapChain.swapChain;
    auto& fence = *frameSync.inFlightFences[currentFrame];
    auto& acquireSemaphore = *frameSync.presentCompleteSemaphore[currentFrame];
    auto& frameCmd = frameSync.commandBuffers[currentFrame];
    auto& presentCmd = frameSync.presentCommandBuffers[currentFrame];

    if (dlss.isRecreateNeeded() || denoiser != appliedDenoiser) {
        // a new DLSS mode, preset or denoiser may change the render resolution: every frame in flight reads the old
        // targets
        finishPreviousFrame();
        deviceRef.waitIdle();
        rebuildSwapchainResources();
    }

    {
        ZoneScopedN("FenceWait");
        const auto waitStart = std::chrono::steady_clock::now();
        while (vk::Result::eTimeout == deviceRef.waitForFences(fence, vk::True, UINT64_MAX))
            ;
        cpuWaitsMs += toMs(waitStart, std::chrono::steady_clock::now());
    }
    {
        ZoneScopedN("ComputeWait");
        // the slot's async compute work reads its node instances and owns its TLAS buffers. The ray submit waits for
        // it before the fence signals, so this returns at once; it keeps slot reuse independent of what that submit
        // records
        const auto waitStart = std::chrono::steady_clock::now();
        const uint64_t value = computeTimelineValues[currentFrame];
        const vk::SemaphoreWaitInfo waitInfo{
            .semaphoreCount = 1, .pSemaphores = &*frameSync.computeTimeline, .pValues = &value};
        while (vk::Result::eTimeout == deviceRef.waitSemaphores(waitInfo, UINT64_MAX))
            ;
        cpuWaitsMs += toMs(waitStart, std::chrono::steady_clock::now());
    }
    vmaSetCurrentFrameIndex(allocator.allocator, ++frameNumber);
    allocator.tracyPlotMemoryBudget();

    if (WorldLights::isPdfStatsRequested() && !hasDumpedLightPdfStats && frameNumber > MAX_FRAMES_IN_FLIGHT &&
        worldLights.localLightCount() > 0) {
        // the slot fence and compute timeline just completed, so this slot's worldLights hold valid GPU output
        worldLights.dumpLightPdfStats(currentFrame, frameSync, scene, camera.cameraData.viewProj);
        hasDumpedLightPdfStats = true;
    }
    // after the fence: prepare RT scene before reading isDlssEvaluated() so frame 0 of emissive-only scenes sees its
    // triangle lights and aligns camera jitter with DLSS-RR (m8 I14 F25)
    rtScene.prepareFrame(currentFrame, frameNumber - 1);
    const bool isLitDlss = isDlssEvaluated();
    const bool isSpecularHitDistanceWritten =
        ((isLitDlss && dlss.guides.isSpecularHitDistanceUsed) || debugView.view == DebugView::SpecularHitDistance) &&
        indirectBounce != IndirectBounceMode::Off;
    if (isLitDlss && isSpecularHitDistanceWritten != appliedSpecularHitDistanceWritten) {
        isDlssHistoryValid = false;
    }
    appliedSpecularHitDistanceWritten = isSpecularHitDistanceWritten;
    const bool isCleanDlssInput = (extraFrameFlags & FrameFlag::CleanDlssInput) != 0;
    const DlssInputAccumulationMode accumulationMode = isCleanDlssInput ? DlssInputAccumulationMode::None
        : (extraFrameFlags & FrameFlag::DlssInputAccumulate) != 0       ? DlssInputAccumulationMode::Accumulate
        : (extraFrameFlags & FrameFlag::DlssInputFrozen) != 0           ? DlssInputAccumulationMode::Frozen
        : inputHistoryWeight > 0.0f                                     ? DlssInputAccumulationMode::Ema
                                                                        : DlssInputAccumulationMode::None;
    // the settings lab warms up with DlssInputAccumulate and freezes with DlssInputFrozen; keep that accumulated sum
    // and DLSS-RR history across the freeze boundary, clear on every other entry into accumulation
    const bool isFreezeTransition = appliedAccumulationMode == DlssInputAccumulationMode::Accumulate &&
        accumulationMode == DlssInputAccumulationMode::Frozen;
    const bool isAccumulationModeReset = accumulationMode != appliedAccumulationMode && !isFreezeTransition;
    if (accumulationMode != DlssInputAccumulationMode::None) {
        targets.ensureDlssInputAccumulation();
        if (isAccumulationModeReset) {
            targets.requestDlssInputAccumulationClear();
        }
    }
    appliedAccumulationMode = accumulationMode;
    constexpr uint32_t kDlssInputFlagsMask = FrameFlag::CleanDlssInput | FrameFlag::InvertDlssNormal |
        FrameFlag::SquareDlssRoughness | FrameFlag::DlssInputDirectOnly | FrameFlag::DlssInputIndirectOnly |
        FrameFlag::PreDenoiserClampNeighbors;
    const DlssInputState dlssInput{
        .scale = std::max(dlssInputScale, 1e-6f),
        .clamp = std::max(preDenoiserClamp, 0.0f),
        .historyWeight = accumulationMode == DlssInputAccumulationMode::Ema ? inputHistoryWeight : 0.0f,
        .flags = extraFrameFlags & kDlssInputFlagsMask,
    };
    if (dlssInput != appliedDlssInput || isAccumulationModeReset) {
        isDlssHistoryValid = false;
        // M8.2 BUG-E5: clear DLSS input accumulation/EMA whenever input scale/clamp/flags change
        if (accumulationMode != DlssInputAccumulationMode::None && !isFreezeTransition) {
            targets.requestDlssInputAccumulationClear();
        }
        appliedDlssInput = dlssInput;
    }
    const bool isJitterChanged = camera.isJitterEnabled != appliedJitterEnabled;
    if (isJitterChanged) {
        isDlssHistoryValid = false;
        appliedJitterEnabled = camera.isJitterEnabled;
    }
    // the accumulated mean resolves the jitter as DLSS-RR does; No AA without a denoiser has nothing to resolve it
    camera.updateCameraData(currentFrame, frameNumber, isLitDlss || isAccumulationActive());
    {
        const IndirectState indirect{.bounce = indirectBounce,
                                     .cache = radianceCache,
                                     .isGiReference = rtxdi.isGiReference,
                                     .giReferenceBounces = rtxdi.giReferenceBounces,
                                     .arePathInfiniteLightsExact = rtxdi.arePathInfiniteLightsExact(),
                                     .isSecondaryResamplingEffective = rtxdi.isSecondaryResamplingEffective(),
                                     .giSettings = rtxdi.giSettings};
        // the samples change meaning (spec/m8/m8_3_pipeline_toggles.md): ReSTIR GI and DLSS-RR start over, ReSTIR DI keeps
        isIndirectHistoryReset = indirect != appliedIndirect;
        if (isIndirectHistoryReset) {
            TracyMessageL("Indirect history reset");
            logInfo(std::format("indirect: {} + {}{}{}", indirectBounceModeLabel(indirect.bounce),
                                radianceCacheModeLabel(indirect.cache),
                                indirect.isGiReference ? ", no reuse" : "",
                                indirect.giReferenceBounces != 0 ? ", ground-truth paths" : ""),
                    "Renderer");
            isDlssHistoryValid = false;
        }
        appliedIndirect = indirect;
    }
    // after the RT scene followed a new layout: world lights and NRC both read its layout and dynamic-motion state
    worldLights.prepareFrame(currentFrame, frameNumber - 1);
    isLightSetChanged =
        worldLights.lightCount() != lastLightCount || worldLights.areTriangleLightsSampled != wereTriangleLightsSampled;
    lastLightCount = worldLights.lightCount();
    wereTriangleLightsSampled = worldLights.areTriangleLightsSampled;
    // NP2: camera, sun, light set, scene layout or dynamic transforms invalidate the cache; switching between
    // RawPt+NRC and RestirGi+NRC (or changing GI reuse settings) does not alter NRC's training target (unpausing NRC
    // already reloads the countdown via NrcCache::wasPaused)
    const bool isNrcSceneChanged = !hasNrcChangeBaseline || camera.isCut() ||
        camera.cameraData.viewProj != lastNrcViewProj || camera.cameraData.cameraPos != lastNrcCameraPos ||
        worldLights.sun != lastNrcSun || isLightSetChanged || rtScene.hasSceneChanged();
    if (!isNrcPaused()) {
        lastNrcViewProj = camera.cameraData.viewProj;
        lastNrcCameraPos = camera.cameraData.cameraPos;
        lastNrcSun = worldLights.sun;
        hasNrcChangeBaseline = true;
    }
    if (!isNrcPaused() && !nrc.hasNewBounds(rtScene.getSceneBounds()) && nrc.isNetworkDiverged(currentFrame)) {
        ZoneScopedN("NrcDivergedReset");
        TracyMessageL("NRC network diverged");
        const auto waitStart = std::chrono::steady_clock::now();
        finishPreviousFrame();
        deviceRef.waitIdle();
        cpuWaitsMs += toMs(waitStart, std::chrono::steady_clock::now());
        nrc.recreateNetwork();
        isIndirectHistoryReset = true;
        isDlssHistoryValid = false;
    }
    nrc.prepareFrame(currentFrame, rtScene.getSceneBounds(), isNrcPaused(), isNrcSceneChanged);
    const bool isSharcLayoutChanged =
        lastSharcLayoutGeneration != kNoneIndex && frameData.layoutGeneration() != lastSharcLayoutGeneration;
    lastSharcLayoutGeneration = frameData.layoutGeneration();
    const bool isSharcDirty =
        sharc.prepareFrame(currentFrame, frameNumber, isSharcActive(), targets.getExtent(),
                           camera.cameraData.cameraPos, isSharcLayoutChanged || isLightSetChanged);
    if (isSharcDirty) {
        isIndirectHistoryReset = true;
        isDlssHistoryValid = false;
    }
    const bool areDiInfiniteExact = rtxdi.areDiInfiniteLightsExact();
    const bool willDiReuseTemporally = rtxdi.directMode == DirectMode::RestirDi &&
        (rtxdi.directResampling == DirectResamplingMode::Temporal ||
         rtxdi.directResampling == DirectResamplingMode::TemporalAndSpatial);
    const bool isDiModeChanged = rtxdi.directMode != rtxdi.getAppliedDirectMode() ||
        rtxdi.directResampling != rtxdi.getAppliedDirectResampling();
    const bool isDiSettingsChanged = rtxdi.isSettingsDirty();
    // re-entering temporal DI reuse after Off, StreamingRis, Reference or Spatial must not read stale/uninitialized
    // reservoirs (FrameFlag::HistoryReset is global and also resets motion/GI/DLSS)
    isDiHistoryReset =
        areDiInfiniteExact != wereDiInfiniteLightsExact || (!rtxdi.isTemporalReuseEnabled() && willDiReuseTemporally);
    wereDiInfiniteLightsExact = areDiInfiniteExact;
    if (isDiHistoryReset || isDiModeChanged || isDiSettingsChanged) {
        // direct-light noise profile, preset or reservoir light set changed
        isDlssHistoryValid = false;
    }
    rtxdi.prepareFrame(currentFrame, frameNumber, indirectBounce == IndirectBounceMode::RestirGi,
                       camera.cameraData.cameraPos, frameNumber - 1);
    // M8.3 PERF-D2: lazily allocate/retire optional lighting targets before targets.prepareFrame writes GpuTargets
    const bool needsGiStagnancy = indirectBounce == IndirectBounceMode::RestirGi && rtxdi.isGiStagnancyEnabled();
    if (needsGiStagnancy) {
        targets.ensureGiStagnancy();
        lastGiStagnancyFrame = frameNumber;
    } else if (targets.hasGiStagnancy() && frameNumber >= lastGiStagnancyFrame + MAX_FRAMES_IN_FLIGHT) {
        targets.releaseGiStagnancy();
    }
    const bool needsGiDuplication =
        indirectBounce == IndirectBounceMode::RestirGi && rtxdi.isGiDuplicationMapEnabled();
    if (needsGiDuplication) {
        targets.ensureGiDuplication();
        lastGiDuplicationFrame = frameNumber;
    } else if (targets.hasGiDuplication() && frameNumber >= lastGiDuplicationFrame + MAX_FRAMES_IN_FLIGHT) {
        targets.releaseGiDuplication();
    }
    const bool needsRawIndirectWeight =
        indirectBounce == IndirectBounceMode::RawPt && !isNrcPaused() && nrc.isActive();
    if (needsRawIndirectWeight) {
        targets.ensureRawIndirectWeight();
        lastRawIndirectWeightFrame = frameNumber;
    } else if (targets.hasRawIndirectWeight() && frameNumber >= lastRawIndirectWeightFrame + MAX_FRAMES_IN_FLIGHT) {
        targets.releaseRawIndirectWeight();
    }
    targets.prepareFrame(currentFrame, frameNumber, isLitPresented(), isSpecularHitDistanceWritten,
                         accumulationMode != DlssInputAccumulationMode::None, needsRawIndirectWeight);
    const bool isGlassChanged = glass != appliedGlass && rtScene.glassRowCount() > 0;
    const bool isNrcResolveChanged = nrc.settings.resolveMode != appliedNrcResolveMode;
    prepareAccumulation(isDiModeChanged || isDiSettingsChanged || isJitterChanged || isGlassChanged ||
                        isNrcResolveChanged || isSharcDirty);
    gpuTimer.collect(currentFrame);
    post.readBack(currentFrame);
    if (isGlassRecorded[currentFrame]) {
        lastGlassCounters = *static_cast<const GpuGlassCounters*>(glassCounters[currentFrame].readback.mapped);
    } else {
        lastGlassCounters = {};
    }
    if (glass != appliedGlass) {
        if (rtScene.glassRowCount() > 0) {
            isDlssHistoryValid = false;
        }
        appliedGlass = glass;
    }
    if (dlss.guides != appliedDlssGuides) {
        isDlssHistoryValid = false;
        appliedDlssGuides = dlss.guides;
    }
    if (nrc.settings.resolveMode != appliedNrcResolveMode) {
        isDlssHistoryValid = false;
        appliedNrcResolveMode = nrc.settings.resolveMode;
    }
    if (void* mapped = rayDebugCounters[currentFrame].mapped) {
        auto& counters = *static_cast<GpuRayDebugCounters*>(mapped);
        if (isRayDebugRecorded[currentFrame]) {
            ++rayDebugFrames;
            rayCandidates += counters.candidates;
            if (counters.candidates != 0) {
                logError(std::format("frame {}: {} ray queries met a candidate (spec/m6/d4_ray_contract.md)",
                                     frameNumber - MAX_FRAMES_IN_FLIGHT, counters.candidates),
                         "RayContract");
            }
        }
        counters = {};
    }

    // recordReconstruction updates isDlssHistoryValid for the next frame before UpdateUBO writes this frame's root
    const bool hadDlssOutput = isDlssHistoryValid;
    // the fence covers both submits of this slot; every path below ends in a submit that signals it
    deviceRef.resetFences(fence);
    frameCmd.reset();
    {
        ZoneScopedN("RecordFrameCommands");
        const auto recStart = std::chrono::steady_clock::now();
        recordFrameCommands(frameCmd);
        const auto finishStart = std::chrono::steady_clock::now();
        cpuRecordMs += toMs(recStart, finishStart);
        // frame commands overlap the previous frame's NRC and DLSS-RR; the ray commands call both SDKs
        finishPreviousFrame();
        const auto rayRecStart = std::chrono::steady_clock::now();
        cpuWaitsMs += toMs(finishStart, rayRecStart);
        recordRayCommands();
        cpuRecordMs += toMs(rayRecStart, std::chrono::steady_clock::now());
    }
    {
        ZoneScopedN("UpdateUBO");
        const auto uboStart = std::chrono::steady_clock::now();
        // resize and swapchain recreation arrive as camera cuts
        const bool isHistoryReset =
            camera.isCut() || culling.isSceneLayoutChanged() || isLightSetChanged || isDiHistoryReset;
        if (isHistoryReset) {
            TracyMessageL("History reset");
        }
        frameData.write(currentFrame,
                        FrameRootInputs{
                            .camera = camera.cameraBufferAddresses[currentFrame],
                            .targets = targets.targetsAddress(currentFrame),
                            .drawRecords = culling.drawRecordsAddress(currentFrame),
                            .visibleMeshlets = culling.visibleMeshletsAddress(currentFrame),
                            .cullCounters = culling.countersAddress(currentFrame),
                            .tlas = rtScene.tlasAddress(),
                            .rayInstances = rtScene.rayInstancesAddress(currentFrame),
                            .worldLights = worldLights.address(currentFrame),
                            // a light set change resets history, so both frames share their rows
                            .prevWorldLights = isHistoryReset ? 0 : worldLights.address(previousFrameSlot()),
                            .rtxdiConstants = rtxdi.constantsAddress(currentFrame),
                            .cacheConstants = nrc.constantsAddress(currentFrame),
                            .sharcConstants = sharc.constantsAddress(currentFrame),
                            .lateVisibleBase = culling.lateVisibleBase(currentFrame),
                            .triangleCullFlags = culling.triangleCullFlags(currentFrame),
                            .subpixelSnap = culling.getSubpixelSnap(),
                            .debugView = debugView.packed(),
                            .frameFlags = (isHistoryReset ? FrameFlag::HistoryReset : 0u) |
                                (isIndirectHistoryReset ? FrameFlag::IndirectHistoryReset : 0u) |
                                (worldLights.areTriangleLightsSampled ? 0u : FrameFlag::TriangleLightsUnsampled) |
                                ((hadDlssOutput && !isHistoryReset) ? FrameFlag::DlssHistoryValid : 0u) |
                                (rtxdi.getAppliedDirectMode() == DirectMode::Off ? FrameFlag::DirectDisabled : 0u) |
                                (indirectBounce == IndirectBounceMode::Off ? FrameFlag::IndirectDisabled : 0u) |
                                (indirectBounce == IndirectBounceMode::RawPt ? FrameFlag::RawIndirect : 0u) |
                                extraFrameFlags,
                            .frameIndex = frameNumber,
                            .lightCount = worldLights.lightCount(),
                            .localLightCount = worldLights.localLightCount(),
                            .rayInstanceLights = rtScene.rayInstanceLightsAddress(currentFrame),
                            .firstTriangleLight = worldLights.punctualLocalLightCount(),
                            .triangleLightCount = worldLights.triangleLightCount(),
                            .rayDebug = rayDebugCounters[currentFrame].address,
                            .materialBins = targets.materialBinsAddress(),
                            .glassFlags = frameGlassFlags,
                            .glassMaxLayers = frameGlassMaxLayers,
                            .glassCounters = glassCounters[currentFrame].address,
                            .dlssInputScale = dlssInputScale,
                            .preDenoiserClamp = preDenoiserClamp,
                            .inputHistoryWeight = inputHistoryWeight,
                        });
        cpuFrameDataWriteMs = toMs(uboStart, std::chrono::steady_clock::now());
    }
    // after the frame root and node instances of the slot are written
    std::array<vk::CommandBufferSubmitInfo, FrameSync::kRaySegmentsPerFrame> rayCmdInfos{};
    for (uint32_t i = 0; i < raySegmentCount; ++i) {
        rayCmdInfos[i].commandBuffer = *raySegments[i];
    }
    // the next frame's in-place PTLAS update and light prep wait for this frame's rays
    const vk::SemaphoreSubmitInfo raySignal{.semaphore = *frameSync.rayTimeline,
                                            .value = frameNumber,
                                            .stageMask = vk::PipelineStageFlagBits2::eAllCommands};
    // MG-P7 (I4 §3): submit the pure-Vulkan ray work before the first SDK segment (NRC, else DLSS-RR) ahead of
    // AcquireImage so acquire/present CPU latency never enters RayStartWait; keep the SDK work and all after it with
    // presentCmd in one post-acquire submit so no second submit arrives while NRC/NGX CUDA runs (C-G3-6)
    const bool isPreAcquireRaySubmit = preSdkRaySegmentCount > 0;
    // NRC frames trace after the split (NRC update, indirect), so their ray signal stays post-acquire
    const bool isPreSdkRaySignal =
        isPreAcquireRaySubmit && !isNrcRecorded && !(frameHook != nullptr && frameHook->tracesRays());
    vk::SemaphoreSubmitInfo tlasWait{};
    {
        const auto submitStart = std::chrono::steady_clock::now();
        submitComputeCommands();
        // after submitComputeCommands() updates computeTimelineValues[currentFrame] = frameNumber; the first stage
        // that traces waits for this frame's TLAS (spec/features/planned/ray_tracing_mega_geometry.md §3.5)
        tlasWait = vk::SemaphoreSubmitInfo{.semaphore = *frameSync.computeTimeline,
                                           .value = computeTimelineValues[currentFrame],
                                           .stageMask = vk::PipelineStageFlagBits2::eComputeShader |
                                               vk::PipelineStageFlagBits2::eRayTracingShaderKHR};
        {
            ZoneScopedN("QueueSubmitFrame");
            const vk::CommandBufferSubmitInfo frameCmdInfo{.commandBuffer = *frameCmd};
            if (isPreAcquireRaySubmit) {
                const std::array earlySubmits{
                    // no wait: cull, raster and resolve run while async compute builds PTLAS and lights
                    vk::SubmitInfo2{.commandBufferInfoCount = 1, .pCommandBufferInfos = &frameCmdInfo},
                    vk::SubmitInfo2{
                        .waitSemaphoreInfoCount = 1,
                        .pWaitSemaphoreInfos = &tlasWait,
                        .commandBufferInfoCount = preSdkRaySegmentCount,
                        .pCommandBufferInfos = rayCmdInfos.data(),
                        .signalSemaphoreInfoCount = isPreSdkRaySignal ? 1u : 0u,
                        .pSignalSemaphoreInfos = isPreSdkRaySignal ? &raySignal : nullptr,
                    },
                };
                graphicsQueue.submit2(earlySubmits);
            } else {
                // no wait: cull, raster and compute run while the CPU acquires the image, and overlap the TLAS build
                graphicsQueue.submit2(
                    vk::SubmitInfo2{.commandBufferInfoCount = 1, .pCommandBufferInfos = &frameCmdInfo});
            }
        }
        cpuSubmitMs += toMs(submitStart, std::chrono::steady_clock::now());
    }

    vk::Result result{};
    uint32_t imageIndex{0};
    {
        ZoneScopedN("AcquireImage");
        const auto acqStart = std::chrono::steady_clock::now();
        auto acquired = swapChainKHR.acquireNextImage(UINT64_MAX, acquireSemaphore, nullptr);
        result = acquired.result;
        imageIndex = acquired.value;
        cpuWaitsMs += toMs(acqStart, std::chrono::steady_clock::now());
    }
    // out of date: no present this frame; the post-acquire ray/DLSS submit alone signals the fence, so the slot stays
    // usable
    const bool isImageAcquired = result != vk::Result::eErrorOutOfDateKHR;
    if (isImageAcquired && result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
        throw std::runtime_error("failed to acquire swap chain image!");
    }
    if (isImageAcquired) {
        ZoneScopedN("RecordPresentCommands");
        const auto presentRecStart = std::chrono::steady_clock::now();
        presentCmd.reset();
        recordPresentCommands(presentCmd, imageIndex);
        cpuRecordMs += toMs(presentRecStart, std::chrono::steady_clock::now());
    }

    const vk::Semaphore renderSemaphore =
        isImageAcquired ? *frameSync.renderFinishedSemaphore[imageIndex] : vk::Semaphore{};
    {
        ZoneScopedN("QueueSubmitRayPresent");
        const auto submitStart = std::chrono::steady_clock::now();
        // only the present pass waits for the image; the swapchain barrier chains with this stage
        const vk::SemaphoreSubmitInfo acquireWait{.semaphore = acquireSemaphore,
                                                  .stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput};
        const vk::CommandBufferSubmitInfo presentCmdInfo{.commandBuffer = *presentCmd};
        const vk::SemaphoreSubmitInfo renderSignal{.semaphore = renderSemaphore,
                                                   .stageMask = vk::PipelineStageFlagBits2::eBottomOfPipe};
        // M8.1 BUG-B4: frameHook ray checks run in postRaySubmit after DLSS-RR even when NRC is paused
        const bool needsPostTlasWait = isNrcRecorded || (frameHook != nullptr && frameHook->tracesRays());
        const vk::SubmitInfo2 postRaySubmit = isPreAcquireRaySubmit
            ? vk::SubmitInfo2{
                  // NRC and RT-check frames trace here too; the early batch's wait does not cover this batch
                  .waitSemaphoreInfoCount = needsPostTlasWait ? 1u : 0u,
                  .pWaitSemaphoreInfos = needsPostTlasWait ? &tlasWait : nullptr,
                  .commandBufferInfoCount = raySegmentCount - preSdkRaySegmentCount,
                  .pCommandBufferInfos = rayCmdInfos.data() + preSdkRaySegmentCount,
                  .signalSemaphoreInfoCount = isPreSdkRaySignal ? 0u : 1u,
                  .pSignalSemaphoreInfos = isPreSdkRaySignal ? nullptr : &raySignal,
              }
            : vk::SubmitInfo2{
                  .waitSemaphoreInfoCount = 1,
                  .pWaitSemaphoreInfos = &tlasWait,
                  .commandBufferInfoCount = raySegmentCount,
                  .pCommandBufferInfos = rayCmdInfos.data(),
                  .signalSemaphoreInfoCount = 1,
                  .pSignalSemaphoreInfos = &raySignal,
              };
        const std::array submits{
            postRaySubmit,
            vk::SubmitInfo2{.waitSemaphoreInfoCount = 1,
                            .pWaitSemaphoreInfos = &acquireWait,
                            .commandBufferInfoCount = 1,
                            .pCommandBufferInfos = &presentCmdInfo,
                            .signalSemaphoreInfoCount = 1,
                            .pSignalSemaphoreInfos = &renderSignal},
        };
        // the SDK work (NRC / DLSS-RR) shares one submit call with the present pass: no separate submit while it runs
        const uint32_t submitCount = isImageAcquired ? 2 : 1;
        graphicsQueue.submit2(vk::ArrayProxy<const vk::SubmitInfo2>(submitCount, submits.data()), fence);
        cpuSubmitMs += toMs(submitStart, std::chrono::steady_clock::now());
    }
    if (isNrcRecorded) {
        nrc.endFrame(graphicsQueue);
        lastNrcRaySubmit = frameNumber;
    }
    cpuHistories[0][cpuHistoryHead] = toMs(cpuFrameStart, std::chrono::steady_clock::now());
    cpuHistories[1][cpuHistoryHead] = cpuFrameDataWriteMs;
    cpuHistories[2][cpuHistoryHead] = cpuRecordMs;
    cpuHistories[3][cpuHistoryHead] = cpuSubmitMs;
    cpuHistories[4][cpuHistoryHead] = cpuWaitsMs;
    cpuHistoryHead = (cpuHistoryHead + 1) % kCpuHistorySize;
    cpuHistoryCount = std::min(cpuHistoryCount + 1, kCpuHistorySize);
    if (!isImageAcquired) {
        requestSwapchainRecreate();
        return;
    }
    pendingPresent = PendingPresent{.imageIndex = imageIndex, .renderSemaphore = renderSemaphore};
    currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

Renderer::CpuStats Renderer::getCpuStats() const noexcept
{
    const auto average = [this](const std::array<float, kCpuHistorySize>& samples) {
        return cpuHistoryCount == 0
            ? 0.0f
            : std::reduce(samples.begin(), samples.end(), 0.0f) / static_cast<float>(cpuHistoryCount);
    };
    return CpuStats{
        .drawFrameMs = average(cpuHistories[0]),
        .frameDataWriteMs = average(cpuHistories[1]),
        .recordCmdsMs = average(cpuHistories[2]),
        .submitMs = average(cpuHistories[3]),
        .waitsMs = average(cpuHistories[4]),
    };
}

void Renderer::finishPreviousFrame()
{
    if (isNrcHostWaitEnabled && lastNrcRaySubmit != 0) {
        ZoneScopedN("NrcWait");
        const vk::SemaphoreWaitInfo nrcWait{
            .semaphoreCount = 1, .pSemaphores = &*frameSync.rayTimeline, .pValues = &lastNrcRaySubmit};
        (void)device.vkdevice.waitSemaphores(nrcWait, UINT64_MAX);
        lastNrcRaySubmit = 0;
    }
    if (!pendingPresent) {
        return;
    }
    const PendingPresent present = *pendingPresent;
    pendingPresent.reset();
    const vk::PresentInfoKHR presentInfo{.waitSemaphoreCount = 1,
                                         .pWaitSemaphores = &present.renderSemaphore,
                                         .swapchainCount = 1,
                                         .pSwapchains = &*swapChain.swapChain,
                                         .pImageIndices = &present.imageIndex};
    vk::Result result{};
    {
        ZoneScopedN("Present");
        result = device.presentQueue.presentKHR(presentInfo);
    }
    if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR) {
        requestSwapchainRecreate();
    } else if (result != vk::Result::eSuccess) {
        throw std::runtime_error("failed to present swap chain image!");
    }
}

// everything up to the display image; independent of the swapchain image
void Renderer::recordFrameCommands(vk::raii::CommandBuffer& cmd)
{
    ZoneScoped;
#ifdef TRACY_ENABLE
    const bool gpuTrace = tracyContext != nullptr && tracyContext->isActive();
    TracyVkCtx const gpuCtx = gpuTrace ? tracyContext->getHandle() : nullptr;
#endif

    cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    {
        ZoneScopedN("TransitionToRender");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneTransitionToRender, *cmd, "GPU_TransitionToRender", gpuTrace);
#endif
        // cleared on load; waits for the previous frame's hi-z, resolve and debug-view reads
        transitionImageLayout(&cmd, targets.getDepthImage(), vk::ImageLayout::eUndefined,
                              vk::ImageLayout::eDepthStencilAttachmentOptimal,
                              {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, vk::PipelineStageFlagBits2::eComputeShader,
                              vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                  vk::PipelineStageFlagBits2::eLateFragmentTests,
                              vk::AccessFlagBits2::eNone,
                              vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                  vk::AccessFlagBits2::eDepthStencilAttachmentWrite);
        // content discarded: empty pixels are decided by depth; waits for the previous frame's compute reads
        transitionImageLayout(&cmd, targets.getVisBufferImage(), vk::ImageLayout::eUndefined,
                              vk::ImageLayout::eColorAttachmentOptimal, kColorRange, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, vk::PipelineStageFlagBits2::eComputeShader,
                              vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eNone,
                              vk::AccessFlagBits2::eColorAttachmentWrite);
        targets.recordPendingInit(cmd);
    }

    // also sizes the DGC buffers for this slot
    culling.prepareFrame(currentFrame, camera, hiz);
    gpuTimer.begin(cmd, currentFrame);

    cmd.bindResourceHeapEXT(descriptorManager.resourceHeapInfo);
    cmd.bindSamplerHeapEXT(descriptorManager.samplerHeapInfo);
    setViewport(cmd, targets.getExtent());

    {
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneEarlyPhase, *cmd, "GPU_EarlyPhase", gpuTrace);
#endif
        recordPhase(cmd, CullPhase::Early);
    }
    {
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneLatePhase, *cmd, "GPU_LatePhase", gpuTrace);
#endif
        recordPhase(cmd, CullPhase::Late);
    }
    culling.recordCounterReadback(cmd);

    {
        ZoneScopedN("Classify");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneClassify, *cmd, "GPU_Classify", gpuTrace);
#endif
        recordClassify(cmd);
    }
    {
        ZoneScopedN("Resolve");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneResolve, *cmd, "GPU_Resolve", gpuTrace);
#endif
        recordResolve(cmd);
    }
    cmd.end();
}

void Renderer::recordRayCommands()
{
    ZoneScoped;
#ifdef TRACY_ENABLE
    const bool gpuTrace = tracyContext != nullptr && tracyContext->isActive();
    TracyVkCtx const gpuCtx = gpuTrace ? tracyContext->getHandle() : nullptr;
#endif

    raySegments = frameSync.raySegments(currentFrame);
    raySegmentCount = 0;
    preSdkRaySegmentCount = 0;
    // the frame root written after this recording carries the same glass state
    frameGlassFlags = glass.flags(rtScene.glassRowCount() > 0);
    frameGlassMaxLayers = glass.clampedMaxLayers();
    // moves on when an SDK call opens a new segment
    const vk::raii::CommandBuffer* cmd = &beginRaySegment();
    // this frame's guide writes and last frame's reservoir writes -> ray/combine passes; emitted here so Direct == Off
    // still orders resolve -> indirect/combine/debug and keeps RayStartWait valid (spec/m8/m8_3_pipeline_toggles.md)
    const vk::MemoryBarrier2 rayStartBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageRead |
            vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd->pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &rayStartBarrier});
    // first ray pass: the span since resolve covers the ray submit gap and the computeTimeline wait (F4)
    gpuTimer.end(*cmd, GpuTimerScope::RayStartWait);
    {
        ZoneScopedN("DirectLight");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneDirectLight, **cmd, "GPU_DirectLight", gpuTrace);
#endif
        recordDirectLight(*cmd);
    }
    {
        // no GPU zone: a Tracy zone cannot span segments
        ZoneScopedN("Indirect");
        cmd = &recordIndirect(*cmd);
    }
    {
        ZoneScopedN("Reconstruction");
        cmd = &recordReconstruction(*cmd);
    }
    {
        ZoneScopedN("DebugView");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneDebugView, **cmd, "GPU_DebugView", gpuTrace);
#endif
        recordDebugView(*cmd);
    }
    if (frameHook != nullptr) {
        const PresentedImage presented = getPresentedImage();
        frameHook->recordAfterDebugView(FrameHookContext{.cmd = *cmd,
                                                         .frameSlot = currentFrame,
                                                         .frameRoot = frameData.frameRootAddress(currentFrame),
                                                         .renderExtent = targets.getExtent(),
                                                         .presentedImage = presented.image,
                                                         .presentedExtent = presented.extent});
    }
    gpuTimer.end(*cmd, GpuTimerScope::FrameHook);
    if (rayDebugCounters[currentFrame].mapped != nullptr) {
        // every ray pass of the frame is on this queue before here; a fence wait alone does not make device writes
        // visible to the host
        const vk::MemoryBarrier2 readbackBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        };
        cmd->pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &readbackBarrier});
        isRayDebugRecorded[currentFrame] = true;
    }

    cmd->end();
}

const vk::raii::CommandBuffer& Renderer::beginRaySegment()
{
    if (raySegmentCount == raySegments.size()) {
        throw std::logic_error("ray segments exhausted");
    }
    vk::raii::CommandBuffer& cmd = raySegments[raySegmentCount++];
    cmd.reset();
    cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    // heap bindings do not carry over between command buffers
    bindHeaps(cmd);
    return cmd;
}

// NRC's and NGX's CUDA launches get a command buffer of their own: no engine state recorded before them reaches them,
// and none they leave reaches the engine passes after them
template <typename RecordSdk>
const vk::raii::CommandBuffer& Renderer::recordIsolated(const vk::raii::CommandBuffer& cmd, RecordSdk&& recordSdk)
{
    // MG-P7: the first SDK segment ends the pre-acquire part
    if (preSdkRaySegmentCount == 0) {
        preSdkRaySegmentCount = raySegmentCount;
    }
    cmd.end();
    const vk::raii::CommandBuffer& sdkCmd = beginRaySegment();
    std::forward<RecordSdk>(recordSdk)(sdkCmd);
    bindHeaps(sdkCmd);
    sdkCmd.end();
    return beginRaySegment();
}

// present pass + UI into the acquired image; earlier submits on this queue are ordered by the barriers inside
void Renderer::recordPresentCommands(vk::raii::CommandBuffer& cmd, uint32_t imageIndex)
{
    ZoneScoped;
#ifdef TRACY_ENABLE
    const bool gpuTrace = tracyContext != nullptr && tracyContext->isActive();
    TracyVkCtx const gpuCtx = gpuTrace ? tracyContext->getHandle() : nullptr;
#endif

    cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    // heap bindings do not carry over from the frame command buffer
    cmd.bindResourceHeapEXT(descriptorManager.resourceHeapInfo);
    cmd.bindSamplerHeapEXT(descriptorManager.samplerHeapInfo);
    {
        ZoneScopedN("Present");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZonePresent, *cmd, "GPU_Present", gpuTrace);
#endif
        recordPresent(cmd, imageIndex);
    }

    if (tracyContext) {
        ZoneScopedN("TracyVkCollect");
        tracyContext->collect(cmd);
    }

    cmd.end();
}

void Renderer::submitComputeCommands()
{
    ZoneScopedN("QueueSubmitCompute");
    vk::raii::CommandBuffer& cmd = frameSync.computeCommandBuffers[currentFrame];
    cmd.reset();
    cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    // the shader objects are created for descriptor heaps
    cmd.bindResourceHeapEXT(descriptorManager.resourceHeapInfo);
    cmd.bindSamplerHeapEXT(descriptorManager.samplerHeapInfo);
    // MG-P7: record RT instances + PTLAS before light preparation so reuseBarrier does not stall behind presampling
    // and light preparation's compute dispatches can overlap buildPartitionedAccelerationStructuresNV. Defensive heap
    // rebind: whether the PTLAS build keeps the heap bindings is not verified
    rtScene.recordBuild(cmd, currentFrame);
    if (isLightPreparationNeeded()) {
        bindHeaps(cmd);
        rtxdi.recordLightPreparation(cmd, currentFrame, frameData.frameRootAddress(currentFrame));
    } else {
        rtxdi.skipLightPreparation(currentFrame);
    }
    cmd.end();

    // frame numbers only grow, so every signal is larger than the last
    computeTimelineValues[currentFrame] = frameNumber;
    const vk::CommandBufferSubmitInfo cmdInfo{.commandBuffer = *cmd};
    const vk::SemaphoreSubmitInfo signalInfo{.semaphore = *frameSync.computeTimeline,
                                             .value = frameNumber,
                                             .stageMask = vk::PipelineStageFlagBits2::eAllCommands};
    // the PTLAS is updated in place, and the light rows written here are the previous frame's previous-frame rows:
    // its ray passes must have finished with both. Its ray submit is already queued, so the wait never precedes its
    // signal
    const vk::SemaphoreSubmitInfo rayWait{.semaphore = *frameSync.rayTimeline,
                                          .value = frameNumber - 1,
                                          .stageMask = vk::PipelineStageFlagBits2::eComputeShader |
                                                       vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR};
    device.computeQueue.submit2(vk::SubmitInfo2{.waitSemaphoreInfoCount = 1,
                                                .pWaitSemaphoreInfos = &rayWait,
                                                .commandBufferInfoCount = 1,
                                                .pCommandBufferInfos = &cmdInfo,
                                                .signalSemaphoreInfoCount = 1,
                                                .pSignalSemaphoreInfos = &signalInfo});
}

void Renderer::waitIdle() const { device.vkdevice.waitIdle(); }

void Renderer::setViewport(const vk::raii::CommandBuffer& cmd, vk::Extent2D extent)
{
    // shader objects require the *WithCount variants
    cmd.setViewportWithCount(
        vk::Viewport(0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f));
    cmd.setScissorWithCount(vk::Rect2D(vk::Offset2D(0, 0), extent));
}

void Renderer::dispatchFullscreen(const vk::raii::CommandBuffer& cmd, const vk::raii::ShaderEXT& shader,
                                  uint32_t tile) const
{
    const FramePushData push{.frameRoot = frameData.frameRootAddress(currentFrame)};
    ShaderObjects::bindComputeShader(cmd, shader);
    cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
    const vk::Extent2D extent = targets.getExtent();
    cmd.dispatch(divideRoundUp(extent.width, tile), divideRoundUp(extent.height, tile), 1);
}

void Renderer::recordRasterBin(vk::raii::CommandBuffer& cmd, CullPhase phase, RasterBin bin) const
{
    if (shaderObjects.isDgcEnabled) {
        dgc.recordExecute(cmd, phase, bin);
        return;
    }
    const vk::DeviceAddress frameRoot = frameData.frameRootAddress(currentFrame);
    dgc.recordIndirectDraw(cmd, phase, bin, culling.indirectDrawPush(currentFrame, phase, bin, frameRoot));
}

// cull -> preprocess -> vis-buffer raster -> hi-z; the late phase draws over the early one
void Renderer::recordPhase(vk::raii::CommandBuffer& cmd, CullPhase phase)
{
#ifdef TRACY_ENABLE
    const bool gpuTrace = tracyContext != nullptr && tracyContext->isActive();
    TracyVkCtx const gpuCtx = gpuTrace ? tracyContext->getHandle() : nullptr;
#endif
    const bool isEarly = phase == CullPhase::Early;
    const auto scope = [isEarly](GpuTimerScope early, GpuTimerScope late) { return isEarly ? early : late; };

    {
        ZoneScopedN("MeshletCull");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneMeshletCull, *cmd, "GPU_MeshletCull", gpuTrace);
#endif
        culling.recordCull(cmd, phase);
        gpuTimer.end(cmd, scope(GpuTimerScope::EarlyCull, GpuTimerScope::LateCull));
    }

    {
        ZoneScopedN("DgcPreprocess");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneDgcPreprocess, *cmd, "GPU_DgcPreprocess", gpuTrace);
#endif
        // preprocess takes its state from the state command buffer; the scope stays without DGC so bench rows match
        if (shaderObjects.isDgcEnabled) {
            dgc.recordPreprocess(cmd, phase);
        }
        gpuTimer.end(cmd, scope(GpuTimerScope::EarlyPreprocess, GpuTimerScope::LatePreprocess));
    }

    if (!isEarly) {
        // the late raster loads the early vis buffer; the early hi-z build sampled depth
        const vk::MemoryBarrier2 visBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,
        };
        const vk::ImageMemoryBarrier2 depthBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask =
                vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
            .dstAccessMask =
                vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
            .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .newLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = targets.getDepthImage(),
            .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                                .pMemoryBarriers = &visBarrier,
                                                .imageMemoryBarrierCount = 1,
                                                .pImageMemoryBarriers = &depthBarrier});
    }

    // the vis buffer needs no clear: empty pixels are decided by depth (reverse Z: 0 is the far plane)
    const vk::RenderingAttachmentInfo visAttachmentInfo = {
        .imageView = targets.getVisBufferView(),
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = isEarly ? vk::AttachmentLoadOp::eDontCare : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingAttachmentInfo depthAttachmentInfo = {
        .imageView = targets.getDepthView(),
        .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
        .loadOp = isEarly ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearDepthStencilValue(0.0f, 0),
    };
    const vk::RenderingInfo renderingInfo = {.renderArea = {.offset = {0, 0}, .extent = targets.getExtent()},
                                             .layerCount = 1,
                                             .colorAttachmentCount = 1,
                                             .pColorAttachments = &visAttachmentInfo,
                                             .pDepthAttachment = &depthAttachmentInfo};

    cmd.beginRendering(renderingInfo);
    {
        ZoneScopedN("DrawCalls");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneDrawCalls, *cmd, "GPU_DrawCalls", gpuTrace);
#endif
        // opaque first: its depth makes occluded alpha-tested fragments fail the test before they sample
        recordRasterBin(cmd, phase, RasterBin::Opaque);
        gpuTimer.end(cmd, scope(GpuTimerScope::EarlyDraw, GpuTimerScope::LateDraw));
        recordRasterBin(cmd, phase, RasterBin::AlphaTested);
        gpuTimer.end(cmd, scope(GpuTimerScope::EarlyAlphaDraw, GpuTimerScope::LateAlphaDraw));
    }
    cmd.endRendering();

    {
        ZoneScopedN("HiZBuild");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneHiZBuild, *cmd, "GPU_HiZBuild", gpuTrace);
#endif
        // a frozen culling camera keeps the pyramid of its freeze frame
        hiz.recordBuild(cmd, culling.isPyramidFrozen());
        gpuTimer.end(cmd, scope(GpuTimerScope::EarlyHiZ, GpuTimerScope::LateHiZ));
    }
}

// vis buffer + depth -> a material bin per pixel, the bins' tile lists and the resolve's DGC sequences
// (spec/m7/e1_material_binning.md); depth is already eShaderReadOnlyOptimal after the late hi-z build
void Renderer::recordClassify(const vk::raii::CommandBuffer& cmd)
{
    const vk::ImageMemoryBarrier2 visBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = targets.getVisBufferImage(),
        .subresourceRange = kColorRange,
    };
    // earlier frames' reads of the guide, display and bin images (compute, present fragment), and of the one copy of
    // the bin buffer and the resolve preprocess region (preprocess, execute) -> this frame's writes
    const vk::MemoryBarrier2 writeAfterReadBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader |
                        vk::PipelineStageFlagBits2::eCommandPreprocessEXT | vk::PipelineStageFlagBits2::eDrawIndirect,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eClear |
                        vk::PipelineStageFlagBits2::eCommandPreprocessEXT,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eTransferWrite |
                         vk::AccessFlagBits2::eCommandPreprocessWriteEXT,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                            .pMemoryBarriers = &writeAfterReadBarrier,
                                            .imageMemoryBarrierCount = 1,
                                            .pImageMemoryBarriers = &visBarrier});

    cmd.fillMemoryKHR({.address = targets.materialBinsAddress(), .size = kMaterialBinCountBytes}, kBoundAddress, 0);
    // cleared counts -> classify's appends
    const vk::MemoryBarrier2 fillBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eClear,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &fillBarrier});
    dispatchFullscreen(cmd, shaderObjects.frameShaders.classify);

    // tile counts -> binArgs reads them and writes the sequences
    const vk::MemoryBarrier2 countBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &countBarrier});
    const FramePushData push{.frameRoot = frameData.frameRootAddress(currentFrame)};
    ShaderObjects::bindComputeShader(cmd, shaderObjects.frameShaders.binArgs);
    cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
    cmd.dispatch(1, 1, 1);
    gpuTimer.end(cmd, GpuTimerScope::Classify);
}

// one DGC execute (per-bin dispatches without DGC): a sequence per non-empty bin, each bin shader over its tile list
// with the overlap guard
void Renderer::recordResolve(const vk::raii::CommandBuffer& cmd)
{
    // sequences and count -> preprocess and execute; binIds and tile lists -> the bin shaders
    const vk::MemoryBarrier2 sequenceBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eCommandPreprocessEXT | vk::PipelineStageFlagBits2::eDrawIndirect |
                        vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eCommandPreprocessReadEXT | vk::AccessFlagBits2::eIndirectCommandRead |
                         vk::AccessFlagBits2::eShaderStorageRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &sequenceBarrier});
    if (shaderObjects.isResolvePerBin) {
        // no DGC or GPU-AV (C-E1-3): binArgsPerBinMain's slot per bin, same shaders and push data as the DGC
        // sequence; an empty bin dispatches 0 groups
        const vk::DeviceAddress materialBins = targets.materialBinsAddress();
        for (uint32_t bin = 0; bin < kMaterialBinCount; ++bin) {
            const ResolvePushData push{
                .frameRoot = frameData.frameRootAddress(currentFrame),
                .tileList = targets.materialTileListAddress(static_cast<MaterialBin>(bin)),
            };
            ShaderObjects::bindComputeShader(cmd, shaderObjects.resolveShaders[bin]);
            cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
            dispatchIndirectAt(cmd,
                               materialBins + offsetof(GpuMaterialBins, sequences) + bin * sizeof(ResolveDgcSequence) +
                                   offsetof(ResolveDgcSequence, dispatch),
                               kBoundAddress);
        }
    } else {
        const vk::DeviceAddress materialBins = targets.materialBinsAddress();
        dgc.recordResolvePreprocess(cmd, materialBins);
        dgc.recordResolveExecute(cmd, materialBins);
    }
    gpuTimer.end(cmd, GpuTimerScope::Resolve);
}

// Direct lighting (Off, single-pass Streaming RIS, or multi-pass ReSTIR DI into directRadiance)
void Renderer::recordDirectLight(const vk::raii::CommandBuffer& cmd)
{
    const DirectMode mode = rtxdi.getAppliedDirectMode();
    if (mode == DirectMode::Off) {
        gpuTimer.end(cmd, GpuTimerScope::RtxdiInitial);
        gpuTimer.end(cmd, GpuTimerScope::RtxdiTemporal);
        gpuTimer.end(cmd, GpuTimerScope::RtxdiShade);
        return;
    }
    if (mode == DirectMode::StreamingRis) {
        gpuTimer.end(cmd, GpuTimerScope::RtxdiInitial);
        gpuTimer.end(cmd, GpuTimerScope::RtxdiTemporal);
        dispatchFullscreen(cmd, shaderObjects.frameShaders.rtxdiStreamingRis);
        gpuTimer.end(cmd, GpuTimerScope::RtxdiShade);
        return;
    }
    const vk::MemoryBarrier2 resamplingBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageRead |
            vk::AccessFlagBits2::eShaderStorageWrite,
    };
    const vk::DependencyInfo dependency{.memoryBarrierCount = 1, .pMemoryBarriers = &resamplingBarrier};
    // RIS has candidates unless every light is directional and shaded exactly (hasRisLightCandidates in
    // rtxdi_constants.slang; no environment light). Without them rtxdi_shade only writes black or the exact sun and
    // stores empty reservoirs, so the reservoirs skipped meanwhile are never reused
    const bool hasRisCandidates =
        worldLights.localLightCount() > 0 || (worldLights.hasLights() && !rtxdi.areDiInfiniteLightsExact());
    if (hasRisCandidates) {
        dispatchFullscreen(cmd, shaderObjects.frameShaders.rtxdiInitial);
        // initial reservoirs -> the temporal pass's loads and in-place stores
        cmd.pipelineBarrier2(dependency);
    }
    gpuTimer.end(cmd, GpuTimerScope::RtxdiInitial);
    if (hasRisCandidates && rtxdi.isTemporalReuseEnabled()) {
        dispatchFullscreen(cmd, shaderObjects.frameShaders.rtxdiTemporal);
        // temporal reservoirs -> the spatial pass's neighbor loads
        cmd.pipelineBarrier2(dependency);
    }
    gpuTimer.end(cmd, GpuTimerScope::RtxdiTemporal);
    dispatchFullscreen(cmd, shaderObjects.frameShaders.rtxdiShade);
    gpuTimer.end(cmd, GpuTimerScope::RtxdiShade);
}

const vk::raii::CommandBuffer& Renderer::recordIndirect(const vk::raii::CommandBuffer& firstCmd)
{
    const vk::raii::CommandBuffer* cmd = &firstCmd;
    if (indirectBounce == IndirectBounceMode::Off) {
        isNrcRecorded = false;
        isSharcRecorded = false;
        gpuTimer.end(*cmd, GpuTimerScope::NrcUpdate);
        gpuTimer.end(*cmd, GpuTimerScope::SharcUpdate);
        gpuTimer.end(*cmd, GpuTimerScope::SharcResolve);
        gpuTimer.end(*cmd, GpuTimerScope::Indirect);
        gpuTimer.end(*cmd, GpuTimerScope::NrcTrain);
        gpuTimer.end(*cmd, GpuTimerScope::NrcResolve);
        gpuTimer.end(*cmd, GpuTimerScope::GiTemporal);
        gpuTimer.end(*cmd, GpuTimerScope::GiShade);
        return *cmd;
    }
#ifdef TRACY_ENABLE
    const bool gpuTrace = tracyContext != nullptr && tracyContext->isActive();
    TracyVkCtx const gpuCtx = gpuTrace ? tracyContext->getHandle() : nullptr;
#endif
    const vk::DeviceAddress frameRoot = frameData.frameRootAddress(currentFrame);
    // a paused cache keeps its weights; prepareFrame zeroed the slot's constants, so the query pass makes no queries
    isNrcRecorded = nrc.isActive() && !isNrcPaused();
    isSharcRecorded = sharc.isActiveThisFrame();
    // M8.1 BUG-E3: DirectCacheView queries primary vertex 0, matching DebugView::NrcCache
    const bool isCacheView =
        debugView.view == DebugView::NrcCache || nrc.settings.resolveMode == NrcResolveMode::DirectCacheView;
    if (isNrcRecorded) {
        ZoneScopedN("NrcUpdate");
        cmd = &recordIsolated(*cmd, [&](const vk::raii::CommandBuffer& sdkCmd) {
#ifdef TRACY_ENABLE
            TracyVkNamedZone(gpuCtx, gpuZoneNrcBeginFrame, *sdkCmd, "GPU_NrcBeginFrame", gpuTrace);
#endif
            nrc.recordBeginFrame(sdkCmd, currentFrame, isCacheView);
        });
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneNrcUpdate, **cmd, "GPU_NrcUpdate", gpuTrace);
#endif
        nrc.recordUpdate(*cmd, frameRoot, shaderObjects.frameShaders.nrcUpdate);
    }
    gpuTimer.end(*cmd, GpuTimerScope::NrcUpdate);
    if (isSharcRecorded) {
        ZoneScopedN("SharcUpdate");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneSharcUpdate, **cmd, "GPU_SharcUpdate", gpuTrace);
#endif
        sharc.recordUpdate(*cmd, currentFrame, frameRoot, shaderObjects.frameShaders.sharcUpdate);
    }
    gpuTimer.end(*cmd, GpuTimerScope::SharcUpdate);
    if (isSharcRecorded) {
        ZoneScopedN("SharcResolve");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneSharcResolve, **cmd, "GPU_SharcResolve", gpuTrace);
#endif
        sharc.recordResolve(*cmd, currentFrame, frameRoot, shaderObjects.frameShaders.sharcResolve);
    }
    gpuTimer.end(*cmd, GpuTimerScope::SharcResolve);
    {
        ZoneScopedN("IndirectQuery");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneIndirectQuery, **cmd, "GPU_IndirectQuery", gpuTrace);
#endif
        recordIndirectPass(*cmd, shaderObjects.frameShaders.indirect, frameRoot, targets.getExtent());
    }
    gpuTimer.end(*cmd, GpuTimerScope::Indirect);
    if (isNrcRecorded) {
        ZoneScopedN("NrcTrain");
        cmd = &recordIsolated(*cmd, [&](const vk::raii::CommandBuffer& sdkCmd) {
#ifdef TRACY_ENABLE
            TracyVkNamedZone(gpuCtx, gpuZoneNrcTrain, *sdkCmd, "GPU_NrcTrain", gpuTrace);
#endif
            nrc.recordTrain(sdkCmd);
        });
    }
    gpuTimer.end(*cmd, GpuTimerScope::NrcTrain);
    const bool isNrcDebugResolve = isNrcRecorded && nrc.isDebugResolveActive();
    // the cache view shows the query at the primary instead; debug resolve overwrites indirectRadiance directly.
    // nrcBoundary at the end of recordTrain already orders indirect -> nrcResolve
    if (isNrcRecorded && !isCacheView && !isNrcDebugResolve) {
        ZoneScopedN("NrcResolve");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneNrcResolve, **cmd, "GPU_NrcResolve", gpuTrace);
#endif
        nrc.recordHealthReset(*cmd, currentFrame);
        dispatchFullscreen(*cmd, shaderObjects.frameShaders.nrcResolve);
        nrc.recordHealthReadback(*cmd, currentFrame);
    }
    gpuTimer.end(*cmd, GpuTimerScope::NrcResolve);
    if (isNrcDebugResolve) {
        gpuTimer.end(*cmd, GpuTimerScope::GiTemporal);
        gpuTimer.end(*cmd, GpuTimerScope::GiShade);
        cmd = &recordIsolated(*cmd, [&](const vk::raii::CommandBuffer& sdkCmd) {
            nrc.recordDebugResolve(sdkCmd, targets.getIndirectRadianceImage(), targets.getExtent());
        });
    } else if (indirectBounce == IndirectBounceMode::RawPt) {
        // M8.3 PERF-D1: indirect.slang (+ nrc_resolve.slang) wrote indirectRadiance directly; combineBarrier orders it
        gpuTimer.end(*cmd, GpuTimerScope::GiTemporal);
        gpuTimer.end(*cmd, GpuTimerScope::GiShade);
    } else {
        recordIndirectResampling(*cmd);
    }
    return *cmd;
}

void Renderer::recordIndirectResampling(const vk::raii::CommandBuffer& cmd)
{
    ZoneScopedN("RestirGi");
#ifdef TRACY_ENABLE
    const bool gpuTrace = tracyContext != nullptr && tracyContext->isActive();
    TracyVkCtx const gpuCtx = gpuTrace ? tracyContext->getHandle() : nullptr;
    TracyVkNamedZone(gpuCtx, gpuZoneRestirGi, *cmd, "GPU_RestirGi", gpuTrace);
#endif
    // GI reservoir stores (indirect pass, NRC resolve, temporal pass) -> the next pass's loads and stores and the
    // debug view
    const vk::MemoryBarrier2 reservoirBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    const vk::DependencyInfo dependency{.memoryBarrierCount = 1, .pMemoryBarriers = &reservoirBarrier};
    cmd.pipelineBarrier2(dependency);
    // the reference shades the initial samples without reuse
    if (rtxdi.isGiReuseEnabled()) {
        dispatchFullscreen(cmd, shaderObjects.frameShaders.giTemporal);
        cmd.pipelineBarrier2(dependency);
    }
    gpuTimer.end(cmd, GpuTimerScope::GiTemporal);
    dispatchFullscreen(cmd, shaderObjects.frameShaders.giShade);
    // timed with the shade pass; its counts are next frame's temporal input
    if (rtxdi.isGiDuplicationMapEnabled()) {
        // the shade pass's sample ids -> the count; this frame's temporal reads of the counts -> their overwrite
        cmd.pipelineBarrier2(dependency);
        dispatchFullscreen(cmd, shaderObjects.frameShaders.giDuplication, kGiDuplicationTile);
    }
    gpuTimer.end(cmd, GpuTimerScope::GiShade);
}

void Renderer::bindHeaps(const vk::raii::CommandBuffer& cmd) const
{
    cmd.bindResourceHeapEXT(descriptorManager.resourceHeapInfo);
    cmd.bindSamplerHeapEXT(descriptorManager.samplerHeapInfo);
}

const vk::raii::CommandBuffer& Renderer::recordReconstruction(const vk::raii::CommandBuffer& cmd)
{
    const vk::AccessFlags2 accumulationAccess = appliedAccumulationMode != DlssInputAccumulationMode::None
        ? (vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite)
        : vk::AccessFlags2{};
    // directRadiance, indirectRadiance and guide writes -> the combine's reads; previous combine's accumulation
    // writes -> combine's accumulation read-modify-write
    const vk::MemoryBarrier2 combineBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead | accumulationAccess,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &combineBarrier});
    dispatchFullscreen(cmd, shaderObjects.frameShaders.combine);
    gpuTimer.end(cmd, GpuTimerScope::Combine);
    recordGlass(cmd);
    gpuTimer.end(cmd, GpuTimerScope::Glass);

    const auto now = std::chrono::steady_clock::now();
    const float wallDeltaMs = lastFrameTime == std::chrono::steady_clock::time_point{}
        ? 0.0f
        : std::chrono::duration<float, std::milli>(now - lastFrameTime).count();
    const float frameDeltaMs = fixedFrameDeltaMs.value_or(wallDeltaMs);
    lastFrameTime = now;
    if (!isDlssEvaluated()) {
        // the output is not shown; its history would be stale when it is
        isDlssHistoryValid = false;
        gpuTimer.end(cmd, GpuTimerScope::Dlss);
        if (!isLitPresented()) {
            gpuTimer.end(cmd, GpuTimerScope::Accumulation);
            gpuTimer.end(cmd, GpuTimerScope::Post);
            return cmd;
        }
        // denoiser off: post reads noisyColor or its running mean, at the display extent
        const uint32_t postInput = accumulatedFrames > 0 ? recordAccumulation(cmd) : targets.noisyColorSampledIndex();
        gpuTimer.end(cmd, GpuTimerScope::Accumulation);
        post.record(cmd, currentFrame, frameData.frameRootAddress(currentFrame), postInput, targets.getDisplayExtent(),
                    frameDeltaMs * 1e-3f);
        gpuTimer.end(cmd, GpuTimerScope::Post);
        return cmd;
    }
    // noisyColor and guide writes -> NGX's reads. NGX opens each evaluation with clears and fills of its own resources
    // without a barrier against the previous evaluation's writes (SYNC-HAZARD-WRITE-AFTER-WRITE), so this orders those
    // too
    const vk::MemoryBarrier2 inputBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eAllTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eAllTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageRead |
            vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eTransferWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &inputBarrier});
    const auto toDlss =
        [](const RenderTargets::ImageRef& image, vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor)
    { return Dlss::Image{.image = image.image, .view = image.view, .format = image.format, .aspect = aspect}; };
    const GpuCameraData& cameraData = camera.cameraData;
    const vk::Extent2D renderExtent = targets.getExtent();
    const bool isGlassGuideBlended =
        (frameGlassFlags & (GlassFlag::Pass | GlassFlag::BlendGuides)) == (GlassFlag::Pass | GlassFlag::BlendGuides);
    // history resets with the frame's (camera cut, resize, layout change) and after frames without the Lit view
    const bool isHistoryReset = !isDlssHistoryValid || camera.isCut() || culling.isSceneLayoutChanged() ||
        isLightSetChanged || isDiHistoryReset;
    const Dlss::Frame frame{
        .color = toDlss(targets.noisyColor()),
        .depth = toDlss(targets.depth(), vk::ImageAspectFlagBits::eDepth),
        .motion = toDlss(targets.guide(frameNumber, GuideImage::Motion)),
        // m8 I13 §4 variant b: the albedo guides blended with the glass layers
        .diffuseAlbedo = toDlss(isGlassGuideBlended ? targets.glassAlbedo(GuideImage::DiffuseAlbedo)
                                                    : targets.guide(frameNumber, GuideImage::DiffuseAlbedo)),
        .specularAlbedo = toDlss(isGlassGuideBlended ? targets.glassAlbedo(GuideImage::SpecAlbedo)
                                                     : targets.guide(frameNumber, GuideImage::SpecAlbedo)),
        .normalRoughness = toDlss(targets.guide(frameNumber, GuideImage::NormalRoughness)),
        .specularHitDistance = appliedSpecularHitDistanceWritten ? toDlss(targets.specularHitDistance()) : Dlss::Image{},
        .output = toDlss(targets.upscaled()),
        // NDC units, y down like pixels
        .jitterPixels = cameraData.jitterOffset * 0.5f * glm::vec2(renderExtent.width, renderExtent.height),
        .view = cameraData.view,
        .proj = cameraData.proj,
        .frameDeltaMs = frameDeltaMs,
        .isReset = isHistoryReset,
    };
#ifdef TRACY_ENABLE
    const bool gpuTrace = tracyContext != nullptr && tracyContext->isActive();
    TracyVkCtx const gpuCtx = gpuTrace ? tracyContext->getHandle() : nullptr;
#endif
    // NGX binds its own pipelines and descriptor sets, which invalidate the heap bindings
    const vk::raii::CommandBuffer& postCmd = recordIsolated(cmd, [&](const vk::raii::CommandBuffer& sdkCmd) {
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneDlss, *sdkCmd, "GPU_Dlss", gpuTrace);
#endif
        dlss.evaluate(sdkCmd, frame);
    });
    isDlssHistoryValid = true;
    gpuTimer.end(postCmd, GpuTimerScope::Dlss);
    gpuTimer.end(postCmd, GpuTimerScope::Accumulation);
    post.record(postCmd, currentFrame, frameData.frameRootAddress(currentFrame), targets.upscaledSampledIndex(),
                targets.getDisplayExtent(), frameDeltaMs * 1e-3f);
    gpuTimer.end(postCmd, GpuTimerScope::Post);
    return postCmd;
}

void Renderer::prepareAccumulation(bool isLightingStateChanged)
{
    // the unjittered view: Accumulation keeps advancing the jitter, so the mean is antialiased
    const bool isStill = camera.cameraData.view == lastAccumulationView;
    lastAccumulationView = camera.cameraData.view;
    const bool isSunChanged = worldLights.sun != lastAccumulationSun;
    lastAccumulationSun = worldLights.sun;
    if (!isAccumulationActive()) {
        accumulatedFrames = 0;
        lastAccumulationFrameLimit = accumulationFrameLimit;
        isAccumulationResetRequested = false;
        // retired: every frame up to frameNumber - MAX_FRAMES_IN_FLIGHT completed before this slot's fence
        if (targets.hasAccumulated() && frameNumber - lastAccumulationFrame >= MAX_FRAMES_IN_FLIGHT) {
            targets.releaseAccumulated();
        }
        return;
    }
    targets.ensureAccumulated();
    // M8.2 BUG-UI-10: reset on any lighting/state change (captured before rtxdi.prepareFrame) or lowered frame limit
    const bool isLimitLowered = accumulationFrameLimit > 0 && accumulationFrameLimit < accumulatedFrames &&
        accumulationFrameLimit != lastAccumulationFrameLimit;
    lastAccumulationFrameLimit = accumulationFrameLimit;
    const bool isReset = accumulatedFrames == 0 || !isStill || isAccumulationResetRequested || camera.isCut() ||
        culling.isSceneLayoutChanged() || isLightSetChanged || isIndirectHistoryReset || isDiHistoryReset ||
        isLightingStateChanged || isLimitLowered || rtScene.hasSceneChanged() || isSunChanged;
    isAccumulationResetRequested = false;
    accumulatedFrames = isReset ? 1u : accumulatedFrames + 1u;
    if (accumulationFrameLimit > 0) {
        accumulatedFrames = std::min(accumulatedFrames, accumulationFrameLimit);
    }
}

uint32_t Renderer::recordAccumulation(const vk::raii::CommandBuffer& cmd)
{
    // combine's and glass's noisyColor writes, and last frame's accumulated writes -> the read-modify-write
    const vk::MemoryBarrier2 inputBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageRead |
            vk::AccessFlagBits2::eShaderStorageWrite,
    };
    // a new target's content is undefined; its first frame has weight 1 and overwrites every texel
    const bool isLayoutPending = targets.takeAccumulatedLayoutPending();
    const vk::ImageMemoryBarrier2 layoutBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eNone,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = targets.getAccumulatedImage(),
        .subresourceRange = kColorRange,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                            .pMemoryBarriers = &inputBarrier,
                                            .imageMemoryBarrierCount = isLayoutPending ? 1u : 0u,
                                            .pImageMemoryBarriers = isLayoutPending ? &layoutBarrier : nullptr});
    const vk::Extent2D extent = targets.getDisplayExtent();
    const AccumulatePushData push{
        .input = targets.noisyColorSampledIndex(),
        .accumulated = targets.accumulatedStorageIndex(),
        .size = extent,
        .weight = 1.0f / static_cast<float>(accumulatedFrames),
        .pad0 = 0,
    };
    ShaderObjects::bindComputeShader(cmd, shaderObjects.frameShaders.accumulate);
    cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
    cmd.dispatch(divideRoundUp(extent.width, kFullscreenTile), divideRoundUp(extent.height, kFullscreenTile), 1);
    lastAccumulationFrame = frameNumber;
    return targets.accumulatedSampledIndex();
}

void Renderer::recordGlass(const vk::raii::CommandBuffer& cmd)
{
    isGlassRecorded[currentFrame] = (frameGlassFlags & GlassFlag::Pass) != 0;
    if (!isGlassRecorded[currentFrame]) {
        return;
    }
    // combine's noisyColor writes -> the glass pass's read-modify-write
    const vk::MemoryBarrier2 colorBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &colorBarrier});
    recordCounterReset(cmd, glassCounters[currentFrame]);
    dispatchFullscreen(cmd, shaderObjects.frameShaders.glass);
    // the counters are read on the host after the slot's fence; noisyColor's consumers order themselves
    recordCounterReadback(cmd, glassCounters[currentFrame]);
}

void Renderer::recordDebugView(const vk::raii::CommandBuffer& cmd)
{
    // guide, directRadiance and reservoir writes -> reads; submission order also covers the previous frame's set
    // (Reprojection)
    const vk::MemoryBarrier2 guideBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &guideBarrier});
    // the post target replaces it; the frame hook still finds the barrier above
    if (!isLitPresented()) {
        dispatchFullscreen(cmd, shaderObjects.frameShaders.debugView);
    }
    gpuTimer.end(cmd, GpuTimerScope::DebugView);
}

// display image -> swapchain, then the Hi-Z view and ImGui on top
void Renderer::recordPresent(vk::raii::CommandBuffer& cmd, uint32_t imageIndex)
{
    // display image and hi-z pyramid writes -> fragment samples
    const vk::MemoryBarrier2 displayBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
    };
    // chains with the acquire semaphore wait stage
    const vk::ImageMemoryBarrier2 swapchainBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = swapChain.swapChainImages[imageIndex],
        .subresourceRange = kColorRange,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                            .pMemoryBarriers = &displayBarrier,
                                            .imageMemoryBarrierCount = 1,
                                            .pImageMemoryBarriers = &swapchainBarrier});
    // the barrier waits for the acquire semaphore, so this span is the GPU idle time until the image arrives
    gpuTimer.end(cmd, GpuTimerScope::AcquireWait);

    // the fullscreen triangle covers every pixel
    const vk::RenderingAttachmentInfo swapchainAttachmentInfo = {
        .imageView = swapChain.swapChainImageViews[imageIndex],
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eDontCare,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::Extent2D displayExtent = swapChain.swapChainExtent;
    cmd.beginRendering(vk::RenderingInfo{.renderArea = {.offset = {0, 0}, .extent = displayExtent},
                                         .layerCount = 1,
                                         .colorAttachmentCount = 1,
                                         .pColorAttachments = &swapchainAttachmentInfo});
    shaderObjects.bindPresentState(cmd);
    setViewport(cmd, displayExtent);
    const PresentPushData push{.frameRoot = frameData.frameRootAddress(currentFrame),
                               .displaySize = displayExtent};
    cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
    cmd.drawMeshTasksEXT(1, 1, 1);
    if (debugView.view == DebugView::HiZPyramid) {
        recordHiZView(cmd);
    }
    gpuTimer.end(cmd, GpuTimerScope::Present);

#if ENGINE_ENABLE_IMGUI
    if (ImGui::GetDrawData() != nullptr) {
        ZoneScopedN("RenderImGui");
        // its descriptor sets invalidate the heap bindings; nothing after it in this command buffer uses them
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), *cmd);
    }
#endif
    cmd.endRendering();
    gpuTimer.end(cmd, GpuTimerScope::Ui);

    transitionImageLayout(&cmd, swapChain.swapChainImages[imageIndex], vk::ImageLayout::eColorAttachmentOptimal,
                          vk::ImageLayout::ePresentSrcKHR, kColorRange, VK_QUEUE_FAMILY_IGNORED,
                          VK_QUEUE_FAMILY_IGNORED, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                          vk::PipelineStageFlagBits2::eBottomOfPipe, vk::AccessFlagBits2::eColorAttachmentWrite, {});
}

void Renderer::recordHiZView(const vk::raii::CommandBuffer& cmd) const
{
    ZoneScopedN("HiZView");
    const HiZViewPushData push{
        .pyramid = hiz.pyramidHeapIndex(),
        .mip = debugView.hizMip,
        .nearZ = camera.getNearPlane(),
        .farZ = camera.getFarPlane(),
        .screenSize = swapChain.swapChainExtent,
    };
    shaderObjects.bindHiZViewState(cmd);
    cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
    cmd.drawMeshTasksEXT(1, 1, 1);
}
