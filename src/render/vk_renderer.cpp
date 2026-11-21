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

#include <format>
#include <numeric>
#include <stdexcept>

namespace
{
    const vk::ImageSubresourceRange kColorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    // matches passes/classify.slang, passes/debug_view.slang and the passes/rtxdi_*.slang
    constexpr uint32_t kFullscreenTile = 8;
    static_assert(kFullscreenTile == kMaterialTileSize, "classify launches one group per resolve tile");
    // matches shaders/passes/rtxdi_gi_duplication.slang kDuplicationGroup
    constexpr uint32_t kGiDuplicationTile = 16;

    constexpr uint32_t divideRoundUp(uint32_t value, uint32_t divisor) { return (value + divisor - 1) / divisor; }
} // namespace

Renderer::Renderer(Device& device, SwapChain& swapChain, const VkAllocator& allocator, FrameSync& frameSync,
                   const Scene& scene, const AssetBuffers& assets, FrameData& frameData,
                   DescriptorManager& descriptorManager, ShaderObjects& shaderObjects, Camera& camera,
                   const NrcLibrary& nrcLibrary, VkTracyContext* tracyContext) :
    device(device), swapChain(swapChain), allocator(allocator), frameSync(frameSync), frameData(frameData),
    descriptorManager(descriptorManager), camera(camera), shaderObjects(shaderObjects),
    dgc(device, allocator, descriptorManager, shaderObjects),
    culling(device, allocator, scene, assets, frameData, shaderObjects, dgc),
    hiz(device, allocator, frameSync, descriptorManager, shaderObjects),
    targets(device, allocator, frameSync, descriptorManager),
    rtScene(device, allocator, frameSync, scene, assets, frameData, shaderObjects),
    worldLights(device, allocator, frameSync, assets, frameData, rtScene, shaderObjects), rtxdi(device, allocator, frameSync, descriptorManager, shaderObjects, worldLights),
    nrc(device, allocator, nrcLibrary), dlss(device, frameSync),
    post(device, allocator, shaderObjects), gpuTimer(device), tracyContext(tracyContext)
{
    const vk::DeviceSize maxPushData = device.capabilities.descriptorHeap.maxPushDataSize;
    if (kMaxPushDataBytes > maxPushData) {
        throw std::runtime_error(
            std::format("push data of {} B exceeds the device's maxPushDataSize of {} B", kMaxPushDataBytes, maxPushData));
    }
    dgc.init();
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
    for (HostBuffer& stats : motionCheckStats) {
        destroyHostBuffer(allocator.allocator, stats, nullptr);
    }
    for (HostBuffer& stats : rayCheckStats) {
        destroyHostBuffer(allocator.allocator, stats, nullptr);
    }
    for (HostBuffer& capture : displayCaptures) {
        destroyHostBuffer(allocator.allocator, capture, nullptr);
    }
}

void Renderer::enableMotionCheck()
{
    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        if (motionCheckStats[slot].mapped == nullptr) {
            motionCheckStats[slot] = createHostBuffer(sizeof(GpuMotionCheckStats), allocator.allocator, device.vkdevice,
                                                      device.queueFamilyIndices,
                                                      std::format("MotionCheckStats_{}", slot), nullptr);
        }
    }
}

void Renderer::enableRayCheck()
{
    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        if (rayCheckStats[slot].mapped == nullptr) {
            rayCheckStats[slot] = createHostBuffer(sizeof(GpuRayCheckStats), allocator.allocator, device.vkdevice,
                                                   device.queueFamilyIndices, std::format("RayCheckStats_{}", slot),
                                                   nullptr);
        }
    }
}

const GpuRayCheckStats& Renderer::getLastRayCheck() const
{
    if (rayCheckStats[lastRecordedFrame].mapped == nullptr) {
        throw std::logic_error("ray check read without enableRayCheck");
    }
    return *static_cast<const GpuRayCheckStats*>(rayCheckStats[lastRecordedFrame].mapped);
}

const GpuMotionCheckStats& Renderer::getLastMotionCheck() const
{
    if (motionCheckStats[lastRecordedFrame].mapped == nullptr) {
        throw std::logic_error("motion check read without enableMotionCheck");
    }
    return *static_cast<const GpuMotionCheckStats*>(motionCheckStats[lastRecordedFrame].mapped);
}

std::optional<Renderer::DisplayCapture> Renderer::getLastDisplayCapture() const
{
    const std::optional<vk::Extent2D>& extent = displayCaptureExtents[lastRecordedFrame];
    if (!extent) {
        return std::nullopt;
    }
    const size_t halfCount = static_cast<size_t>(extent->width) * extent->height * 4;
    return DisplayCapture{.extent = *extent,
                          .rgba16f = {static_cast<const uint16_t*>(displayCaptures[lastRecordedFrame].mapped), halfCount}};
}

void Renderer::setTracyContext(VkTracyContext* tracyContextIn) { tracyContext = tracyContextIn; }

void Renderer::rebuildSwapchainResources()
{
    frameSync.recreate(static_cast<uint32_t>(swapChain.swapChainImages.size()));
    // DLSS-RR's quality mode picks the render resolution for the swapchain's (engine spec §2.1)
    const vk::Extent2D displayExtent = swapChain.swapChainExtent;
    const vk::Extent2D renderExtent = dlss.recreate(displayExtent);
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

    if (dlss.isRecreateNeeded()) {
        // a new DLSS mode or preset may change the render resolution: every frame in flight reads the old targets
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

    // after the fence: this slot's camera buffer and query results are no longer touched by the GPU
    camera.updateCameraData(currentFrame, frameNumber, isDlssPresented());
    targets.prepareFrame(currentFrame, frameNumber, isDlssPresented());
    rtScene.prepareFrame(currentFrame, frameNumber - 1);
    {
        const IndirectState indirect{.mode = indirectMode,
                                     .isGiReference = rtxdi.isGiReference,
                                     .isGroundTruthPaths = rtxdi.giReferenceBounces != 0,
                                     .giSettings = rtxdi.giSettings};
        // the samples change meaning (spec/m4/g7_indirect_modes.md): ReSTIR GI and DLSS-RR start over, ReSTIR DI keeps
        isIndirectHistoryReset = indirect != appliedIndirect;
        if (isIndirectHistoryReset) {
            TracyMessageL("Indirect history reset");
            logInfo(std::format("indirect: {}{}{}", indirectModeInfo(indirect.mode).label,
                                indirect.isGiReference ? ", no reuse" : "",
                                indirect.isGroundTruthPaths ? ", ground-truth paths" : ""),
                    "Renderer");
            isDlssHistoryValid = false;
        }
        appliedIndirect = indirect;
    }
    // after the RT scene followed a new layout: its bounds are NRC's
    nrc.prepareFrame(currentFrame, rtScene.getSceneBounds(), isNrcPaused());
    worldLights.prepareFrame(currentFrame, frameNumber - 1);
    isLightSetChanged = worldLights.lightCount() != lastLightCount ||
        worldLights.areTriangleLightsSampled != wereTriangleLightsSampled;
    lastLightCount = worldLights.lightCount();
    wereTriangleLightsSampled = worldLights.areTriangleLightsSampled;
    if (rtxdi.isReference != rtxdi.isReferenceEnabled()) {
        // direct-light noise profile changes abruptly across reference/reuse switches
        isDlssHistoryValid = false;
    }
    rtxdi.prepareFrame(currentFrame, frameNumber, indirectMode != IndirectMode::Nrc, camera.cameraData.cameraPos,
                       frameNumber - 1);
    gpuTimer.collect(currentFrame);
    post.readBack(currentFrame);
    if (void* stats = motionCheckStats[currentFrame].mapped) {
        // host writes before the submit are visible to it
        *static_cast<GpuMotionCheckStats*>(stats) = {};
    }
    if (void* stats = rayCheckStats[currentFrame].mapped) {
        *static_cast<GpuRayCheckStats*>(stats) = {};
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
        const bool isHistoryReset = camera.isCut() || culling.isSceneLayoutChanged() || isLightSetChanged;
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
                            .lateVisibleBase = culling.lateVisibleBase(currentFrame),
                            .triangleCullFlags = culling.triangleCullFlags(currentFrame),
                            .subpixelSnap = culling.getSubpixelSnap(),
                            .debugView = debugView.packed(),
                            .frameFlags = (isHistoryReset ? FrameFlag::HistoryReset : 0u) |
                                (isIndirectHistoryReset ? FrameFlag::IndirectHistoryReset : 0u) |
                                (worldLights.areTriangleLightsSampled ? 0u : FrameFlag::TriangleLightsUnsampled),
                            .frameIndex = frameNumber,
                            .lightCount = worldLights.lightCount(),
                            .localLightCount = worldLights.localLightCount(),
                            .rayInstanceLights = rtScene.rayInstanceLightsAddress(currentFrame),
                            .firstTriangleLight = worldLights.punctualLocalLightCount(),
                            .triangleLightCount = worldLights.triangleLightCount(),
                            .rayDebug = rayDebugCounters[currentFrame].address,
                            .materialBins = targets.materialBinsAddress(),
                        });
        cpuFrameDataWriteMs = toMs(uboStart, std::chrono::steady_clock::now());
    }
    // after the frame root and node instances of the slot are written
    {
        const auto submitStart = std::chrono::steady_clock::now();
        submitComputeCommands();
        {
            ZoneScopedN("QueueSubmitFrame");
            // no wait: cull, raster and compute run while the CPU acquires the image, and overlap the TLAS build
            const vk::CommandBufferSubmitInfo frameCmdInfo{.commandBuffer = *frameCmd};
            graphicsQueue.submit2(vk::SubmitInfo2{.commandBufferInfoCount = 1, .pCommandBufferInfos = &frameCmdInfo});
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
    // out of date: no present this frame; the ray submits alone signal the fence, so the slot stays usable
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
        // the first stage that traces waits for this frame's TLAS (spec/features/ray_tracing_mega_geometry.md §3.5);
        // the compute submit is already queued, so the wait never precedes its signal
        std::array<vk::CommandBufferSubmitInfo, FrameSync::kRaySegmentsPerFrame> rayCmdInfos{};
        for (uint32_t i = 0; i < raySegmentCount; ++i) {
            rayCmdInfos[i].commandBuffer = *raySegments[i];
        }
        const vk::SemaphoreSubmitInfo tlasWait{.semaphore = *frameSync.computeTimeline,
                                               .value = computeTimelineValues[currentFrame],
                                               .stageMask = vk::PipelineStageFlagBits2::eComputeShader |
                                                   vk::PipelineStageFlagBits2::eRayTracingShaderKHR};
        // the next frame's in-place PTLAS update waits for this frame's rays
        const vk::SemaphoreSubmitInfo raySignal{.semaphore = *frameSync.rayTimeline,
                                                .value = frameNumber,
                                                .stageMask = vk::PipelineStageFlagBits2::eAllCommands};
        // only the present pass waits for the image; the swapchain barrier chains with this stage
        const vk::SemaphoreSubmitInfo acquireWait{.semaphore = acquireSemaphore,
                                                  .stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput};
        const vk::CommandBufferSubmitInfo presentCmdInfo{.commandBuffer = *presentCmd};
        const vk::SemaphoreSubmitInfo renderSignal{.semaphore = renderSemaphore,
                                                   .stageMask = vk::PipelineStageFlagBits2::eBottomOfPipe};
        const std::array submits{
            vk::SubmitInfo2{.waitSemaphoreInfoCount = 1,
                            .pWaitSemaphoreInfos = &tlasWait,
                            .commandBufferInfoCount = raySegmentCount,
                            .pCommandBufferInfos = rayCmdInfos.data(),
                            .signalSemaphoreInfoCount = 1,
                            .pSignalSemaphoreInfos = &raySignal},
            vk::SubmitInfo2{.waitSemaphoreInfoCount = 1,
                            .pWaitSemaphoreInfos = &acquireWait,
                            .commandBufferInfoCount = 1,
                            .pCommandBufferInfos = &presentCmdInfo,
                            .signalSemaphoreInfoCount = 1,
                            .pSignalSemaphoreInfos = &renderSignal},
        };
        // the ray work (NRC) shares one submit call with the present pass: no separate submit while NRC runs
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
    if (lastNrcRaySubmit != 0) {
        ZoneScopedN("NrcWait");
        const vk::SemaphoreWaitInfo nrcWait{
            .semaphoreCount = 1, .pSemaphores = &*frameSync.rayTimeline, .pValues = &lastNrcRaySubmit};
        (void)device.vkdevice.waitSemaphores(nrcWait, UINT64_MAX);
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

    cmd.begin({});

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
    MeshletCulling::recordCounterReadback(cmd);

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
    // moves on when an SDK call opens a new segment
    const vk::raii::CommandBuffer* cmd = &beginRaySegment();
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
    if (motionCheckStats[currentFrame].mapped != nullptr) {
        recordMotionCheck(*cmd);
    }
    if (rayCheckStats[currentFrame].mapped != nullptr) {
        recordRayCheck(*cmd);
    }
    gpuTimer.end(*cmd, GpuTimerScope::SelfTests);
    displayCaptureExtents[currentFrame].reset();
    if (isDisplayCapturePending) {
        recordDisplayCapture(*cmd);
        isDisplayCapturePending = false;
    }
    lastRecordedFrame = currentFrame;
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
    cmd.begin({});
    // heap bindings do not carry over between command buffers
    bindHeaps(cmd);
    return cmd;
}

// NRC's and NGX's CUDA launches get a command buffer of their own: no engine state recorded before them reaches them,
// and none they leave reaches the engine passes after them
template <typename RecordSdk>
const vk::raii::CommandBuffer& Renderer::recordIsolated(const vk::raii::CommandBuffer& cmd, RecordSdk&& recordSdk)
{
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

    cmd.begin({});
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
    rtxdi.recordLightPreparation(cmd, currentFrame, frameData.frameRootAddress(currentFrame));
    rtScene.recordBuild(cmd, currentFrame);
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
        // preprocess takes its state from the state command buffer
        dgc.recordPreprocess(cmd, phase);
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
        dgc.recordExecute(cmd, phase, RasterBin::Opaque);
        gpuTimer.end(cmd, scope(GpuTimerScope::EarlyDraw, GpuTimerScope::LateDraw));
        dgc.recordExecute(cmd, phase, RasterBin::AlphaTested);
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

    const vk::Buffer materialBins = targets.getMaterialBinsBuffer();
    cmd.fillBuffer(materialBins, 0, kMaterialBinCountBytes, 0);
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

// one DGC execute: a sequence per non-empty bin, each bin shader over its tile list with the overlap guard
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
    const vk::DeviceAddress materialBins = targets.materialBinsAddress();
    dgc.recordResolvePreprocess(cmd, materialBins);
    dgc.recordResolveExecute(cmd, materialBins);
    gpuTimer.end(cmd, GpuTimerScope::Resolve);
}

// ReSTIR DI: initial reservoirs, temporal resampling, then spatial resampling + shading into directRadiance
void Renderer::recordDirectLight(const vk::raii::CommandBuffer& cmd)
{
    // this frame's guide writes and last frame's reservoir writes -> reads and writes of every pass. Reads of
    // directRadiance by earlier frames are ordered before this frame's resolve (recordResolve)
    const vk::MemoryBarrier2 resamplingBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageRead |
                         vk::AccessFlagBits2::eShaderStorageWrite,
    };
    const vk::DependencyInfo dependency{.memoryBarrierCount = 1, .pMemoryBarriers = &resamplingBarrier};
    cmd.pipelineBarrier2(dependency);
    // first ray pass: the span since resolve covers the ray submit gap and the computeTimeline wait (F4)
    gpuTimer.end(cmd, GpuTimerScope::RayStartWait);
    // with no lights rtxdi_shade only writes black. Lights arrive only with a load, which resets history, so the
    // reservoirs skipped meanwhile are never reused
    const bool hasLights = worldLights.hasLights();
    if (hasLights) {
        dispatchFullscreen(cmd, shaderObjects.frameShaders.rtxdiInitial);
        // initial reservoirs -> the temporal pass's loads and in-place stores
        cmd.pipelineBarrier2(dependency);
    }
    gpuTimer.end(cmd, GpuTimerScope::RtxdiInitial);
    if (hasLights) {
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
#ifdef TRACY_ENABLE
    const bool gpuTrace = tracyContext != nullptr && tracyContext->isActive();
    TracyVkCtx const gpuCtx = gpuTrace ? tracyContext->getHandle() : nullptr;
#endif
    const vk::DeviceAddress frameRoot = frameData.frameRootAddress(currentFrame);
    // a paused cache keeps its weights; prepareFrame zeroed the slot's constants, so the query pass makes no queries
    isNrcRecorded = nrc.isActive() && !isNrcPaused();
    const bool isCacheView = debugView.view == DebugView::NrcCache;
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
    // the cache view shows the query at the primary instead; the reservoirs keep G2's radiance
    if (isNrcRecorded && !isCacheView) {
        ZoneScopedN("NrcResolve");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneNrcResolve, **cmd, "GPU_NrcResolve", gpuTrace);
#endif
        dispatchFullscreen(*cmd, shaderObjects.frameShaders.nrcResolve);
    }
    gpuTimer.end(*cmd, GpuTimerScope::NrcResolve);
    recordIndirectResampling(*cmd);
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
    // debug view. Last frame's GI writes are ordered by recordDirectLight's first barrier
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
    // directRadiance, indirectRadiance and guide writes -> the combine's reads
    const vk::MemoryBarrier2 combineBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &combineBarrier});
    dispatchFullscreen(cmd, shaderObjects.frameShaders.combine);
    gpuTimer.end(cmd, GpuTimerScope::Combine);

    const auto now = std::chrono::steady_clock::now();
    const float wallDeltaMs = lastFrameTime == std::chrono::steady_clock::time_point{}
        ? 0.0f
        : std::chrono::duration<float, std::milli>(now - lastFrameTime).count();
    const float frameDeltaMs = fixedFrameDeltaMs.value_or(wallDeltaMs);
    lastFrameTime = now;
    if (!isDlssPresented()) {
        // the output is not shown; its history would be stale when it is
        isDlssHistoryValid = false;
        gpuTimer.end(cmd, GpuTimerScope::Dlss);
        gpuTimer.end(cmd, GpuTimerScope::Post);
        return cmd;
    }
    // noisyColor and guide writes -> NGX's reads. NGX opens each evaluation with clears and fills of its own resources
    // without a barrier against the previous evaluation's writes (SYNC-HAZARD-WRITE-AFTER-WRITE), so this orders those too
    const vk::MemoryBarrier2 inputBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eAllTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eAllTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageRead |
            vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eTransferWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &inputBarrier});
    const auto toDlss = [](const RenderTargets::ImageRef& image,
                           vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor)
    {
        return Dlss::Image{.image = image.image, .view = image.view, .format = image.format, .aspect = aspect};
    };
    const GpuCameraData& cameraData = camera.cameraData;
    const vk::Extent2D renderExtent = targets.getExtent();
    // history resets with the frame's (camera cut, resize, layout change) and after frames without the Lit view
    const bool isHistoryReset =
        !isDlssHistoryValid || camera.isCut() || culling.isSceneLayoutChanged() || isLightSetChanged;
    const Dlss::Frame frame{
        .color = toDlss(targets.noisyColor()),
        .depth = toDlss(targets.depth(), vk::ImageAspectFlagBits::eDepth),
        .motion = toDlss(targets.guide(frameNumber, GuideImage::Motion)),
        .diffuseAlbedo = toDlss(targets.guide(frameNumber, GuideImage::DiffuseAlbedo)),
        .specularAlbedo = toDlss(targets.guide(frameNumber, GuideImage::SpecAlbedo)),
        .normalRoughness = toDlss(targets.guide(frameNumber, GuideImage::NormalRoughness)),
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
    post.record(postCmd, currentFrame, frameData.frameRootAddress(currentFrame), targets.getDisplayExtent(),
                frameDeltaMs * 1e-3f);
    gpuTimer.end(postCmd, GpuTimerScope::Post);
    return postCmd;
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
    // DLSS-RR's output replaces it; the motion and ray checks still find the barrier above
    if (!isDlssPresented()) {
        dispatchFullscreen(cmd, shaderObjects.frameShaders.debugView);
    }
    gpuTimer.end(cmd, GpuTimerScope::DebugView);
}

// reads what the debug view reads, after the same barrier; only the stats buffer is written
void Renderer::recordMotionCheck(const vk::raii::CommandBuffer& cmd)
{
    const MotionCheckPushData push{.frameRoot = frameData.frameRootAddress(currentFrame),
                                   .stats = motionCheckStats[currentFrame].address};
    ShaderObjects::bindComputeShader(cmd, shaderObjects.frameShaders.motionCheck);
    cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
    const vk::Extent2D extent = targets.getExtent();
    cmd.dispatch(divideRoundUp(extent.width, kFullscreenTile), divideRoundUp(extent.height, kFullscreenTile), 1);
    // a fence wait alone does not make device writes visible to the host
    const vk::MemoryBarrier2 readbackBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &readbackBarrier});
}

// reads what the resolve reads, after the debug view's barrier; only the stats buffer is written
void Renderer::recordRayCheck(const vk::raii::CommandBuffer& cmd)
{
    const RayCheckPushData push{.frameRoot = frameData.frameRootAddress(currentFrame),
                                .stats = rayCheckStats[currentFrame].address};
    ShaderObjects::bindComputeShader(cmd, shaderObjects.frameShaders.rayCheck);
    cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
    const vk::Extent2D extent = targets.getExtent();
    cmd.dispatch(divideRoundUp(extent.width, kFullscreenTile), divideRoundUp(extent.height, kFullscreenTile), 1);
    // a fence wait alone does not make device writes visible to the host
    const vk::MemoryBarrier2 readbackBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &readbackBarrier});
}

void Renderer::recordDisplayCapture(const vk::raii::CommandBuffer& cmd)
{
    constexpr vk::DeviceSize kTexelBytes = 8;
    // the image present reads: both are RGBA16F
    const bool isUpscaled = isDlssPresented();
    const vk::Extent2D extent = isUpscaled ? targets.getDisplayExtent() : targets.getExtent();
    const vk::Image source = isUpscaled ? targets.post().image : targets.getDisplayImage();
    const vk::DeviceSize size = vk::DeviceSize{extent.width} * extent.height * kTexelBytes;
    HostBuffer& capture = displayCaptures[currentFrame];
    if (displayCaptureCapacities[currentFrame] < size) {
        destroyHostBuffer(allocator.allocator, capture, nullptr);
        // read back by the CPU: cached memory, not ReBAR
        createBuffer(size, vk::BufferUsageFlagBits2::eTransferDst,
                     vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent |
                         vk::MemoryPropertyFlagBits::eHostCached,
                     capture.buffer, capture.memory, allocator.allocator, device.vkdevice, device.queueFamilyIndices,
                     std::format("DisplayCapture_{}", currentFrame), VMA_ALLOCATION_CREATE_MAPPED_BIT);
        VmaAllocationInfo allocationInfo{};
        vmaGetAllocationInfo(allocator.allocator, capture.memory, &allocationInfo);
        capture.mapped = allocationInfo.pMappedData;
        displayCaptureCapacities[currentFrame] = size;
    }

    // debug view writes -> copy; the display image stays in eGeneral
    const vk::MemoryBarrier2 copyBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &copyBarrier});
    const vk::BufferImageCopy2 region{
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {extent.width, extent.height, 1},
    };
    cmd.copyImageToBuffer2(vk::CopyImageToBufferInfo2{.srcImage = source,
                                                      .srcImageLayout = vk::ImageLayout::eGeneral,
                                                      .dstBuffer = *capture.buffer,
                                                      .regionCount = 1,
                                                      .pRegions = &region});
    const std::array<vk::MemoryBarrier2, 2> afterCopy{{
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        },
        // the next frame's debug view overwrites the copied texels: execution dependency only
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        },
    }};
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = static_cast<uint32_t>(afterCopy.size()),
                                            .pMemoryBarriers = afterCopy.data()});
    displayCaptureExtents[currentFrame] = extent;
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
