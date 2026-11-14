#include "vk_renderer.hpp"
#include "../Constants.h"
#include "../util/vk_tracy.hpp"
#include "../util/vk_utils.hpp"
#include "push_data.hpp"
#if ENGINE_ENABLE_IMGUI
    #include "imgui.h"
    #include "imgui_impl_vulkan.h"
#endif

#include <format>
#include <stdexcept>

namespace
{
    const vk::ImageSubresourceRange kColorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    // matches passes/resolve.slang, passes/debug_view.slang and the passes/rtxdi_*.slang
    constexpr uint32_t kFullscreenTile = 8;

    constexpr uint32_t divideRoundUp(uint32_t value, uint32_t divisor) { return (value + divisor - 1) / divisor; }
} // namespace

Renderer::Renderer(Device& device, SwapChain& swapChain, const VkAllocator& allocator, FrameSync& frameSync,
                   const Scene& scene, const AssetBuffers& assets, FrameData& frameData,
                   DescriptorManager& descriptorManager, ShaderObjects& shaderObjects, Camera& camera,
                   VkTracyContext* tracyContext) :
    device(device), swapChain(swapChain), allocator(allocator), frameSync(frameSync), frameData(frameData),
    descriptorManager(descriptorManager), camera(camera), shaderObjects(shaderObjects),
    dgc(device, allocator, descriptorManager, shaderObjects),
    culling(device, allocator, scene, assets, frameData, shaderObjects, dgc),
    hiz(device, allocator, frameSync, descriptorManager, shaderObjects),
    targets(device, allocator, frameSync, descriptorManager),
    rtScene(device, allocator, frameSync, scene, assets, frameData, shaderObjects),
    worldLights(device, allocator, assets, frameData, shaderObjects), rtxdi(device, allocator, worldLights),
    dlss(device, frameSync), post(device, allocator, shaderObjects), gpuTimer(device), tracyContext(tracyContext)
{
    const vk::DeviceSize maxPushData = device.capabilities.descriptorHeap.maxPushDataSize;
    if (kMaxPushDataBytes > maxPushData) {
        throw std::runtime_error(
            std::format("push data of {} B exceeds the device's maxPushDataSize of {} B", kMaxPushDataBytes, maxPushData));
    }
    dgc.init();
}

Renderer::~Renderer()
{
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
    camera.setRenderExtent(renderExtent);
    isDlssHistoryValid = false;
    // the exposure image starts at 1 again
    post.requestSnap();
}

void Renderer::drawFrame()
{
    ZoneScopedN("Renderer::drawFrame");
    auto& deviceRef = device.vkdevice;
    auto& graphicsQueue = device.graphicsQueue;
    auto& presentQueue = device.presentQueue;
    auto& swapChainKHR = swapChain.swapChain;
    auto& fence = *frameSync.inFlightFences[currentFrame];
    auto& acquireSemaphore = *frameSync.presentCompleteSemaphore[currentFrame];
    auto& frameCmd = frameSync.commandBuffers[currentFrame];
    auto& rayCmd = frameSync.rayCommandBuffers[currentFrame];
    auto& presentCmd = frameSync.presentCommandBuffers[currentFrame];

    if (dlss.isRecreateNeeded()) {
        // a new DLSS mode or preset may change the render resolution: every frame in flight reads the old targets
        deviceRef.waitIdle();
        rebuildSwapchainResources();
    }

    {
        ZoneScopedN("FenceWait");
        while (vk::Result::eTimeout == deviceRef.waitForFences(fence, vk::True, UINT64_MAX))
            ;
    }
    {
        ZoneScopedN("ComputeWait");
        // the slot's async compute work reads its node instances and owns its TLAS buffers. The ray submit waits for
        // it before the fence signals, so this returns at once; it keeps slot reuse independent of what that submit
        // records
        const uint64_t value = computeTimelineValues[currentFrame];
        const vk::SemaphoreWaitInfo waitInfo{.semaphoreCount = 1,
                                             .pSemaphores = &*frameSync.computeTimeline,
                                             .pValues = &value};
        while (vk::Result::eTimeout == deviceRef.waitSemaphores(waitInfo, UINT64_MAX))
            ;
    }
    vmaSetCurrentFrameIndex(allocator.allocator, ++frameNumber);
    allocator.tracyPlotMemoryBudget();

    // after the fence: this slot's camera buffer and query results are no longer touched by the GPU
    camera.updateCameraData(currentFrame, frameNumber, isDlssPresented());
    targets.prepareFrame(currentFrame, frameNumber, isDlssPresented());
    rtScene.prepareFrame(currentFrame, frameNumber - 1);
    worldLights.prepareFrame(currentFrame);
    isLightSetChanged = worldLights.lightCount() != lastLightCount;
    lastLightCount = worldLights.lightCount();
    rtxdi.prepareFrame(currentFrame, frameNumber);
    gpuTimer.collect(currentFrame);
    post.readBack(currentFrame);
    if (void* stats = motionCheckStats[currentFrame].mapped) {
        // host writes before the submit are visible to it
        *static_cast<GpuMotionCheckStats*>(stats) = {};
    }
    if (void* stats = rayCheckStats[currentFrame].mapped) {
        *static_cast<GpuRayCheckStats*>(stats) = {};
    }

    // the fence covers both submits of this slot; every path below ends in a submit that signals it
    deviceRef.resetFences(fence);
    frameCmd.reset();
    rayCmd.reset();
    {
        ZoneScopedN("RecordFrameCommands");
        recordFrameCommands(frameCmd);
        recordRayCommands(rayCmd);
    }
    {
        ZoneScopedN("UpdateUBO");
        // resize and swapchain recreation arrive as camera cuts
        const bool isHistoryReset = camera.isCut() || culling.isSceneLayoutChanged() || isLightSetChanged;
        if (isHistoryReset) {
            TracyMessageL("History reset");
        }
        frameData.write(currentFrame, FrameRootInputs{
                                          .camera = camera.cameraBufferAddresses[currentFrame],
                                          .targets = targets.targetsAddress(currentFrame),
                                          .drawRecords = culling.drawRecordsAddress(currentFrame),
                                          .visibleMeshlets = culling.visibleMeshletsAddress(currentFrame),
                                          .cullCounters = culling.countersAddress(currentFrame),
                                          .tlas = rtScene.tlasAddress(),
                                          .rayInstances = rtScene.rayInstancesAddress(currentFrame),
                                          .worldLights = worldLights.address(currentFrame),
                                          .rtxdiConstants = rtxdi.constantsAddress(currentFrame),
                                          .lateVisibleBase = culling.lateVisibleBase(currentFrame),
                                          .triangleCullFlags = culling.triangleCullFlags(currentFrame),
                                          .subpixelSnap = culling.getSubpixelSnap(),
                                          .debugView = debugView.packed(),
                                          .frameFlags = isHistoryReset ? FrameFlag::HistoryReset : 0u,
                                          .frameIndex = frameNumber,
                                          .lightCount = worldLights.lightCount(),
                                          .localLightCount = worldLights.localLightCount(),
                                      });
    }
    // after the frame root and node instances of the slot are written
    submitComputeCommands();
    {
        ZoneScopedN("QueueSubmitFrame");
        // no wait: cull, raster and compute run while the CPU acquires the image, and overlap the TLAS build
        const vk::CommandBufferSubmitInfo frameCmdInfo{.commandBuffer = *frameCmd};
        // the first stage that traces waits for this frame's TLAS (spec/features/ray_tracing_mega_geometry.md §3.5);
        // the compute submit is already queued, so the wait never precedes its signal
        const vk::CommandBufferSubmitInfo rayCmdInfo{.commandBuffer = *rayCmd};
        const vk::SemaphoreSubmitInfo tlasWait{.semaphore = *frameSync.computeTimeline,
                                               .value = computeTimelineValues[currentFrame],
                                               .stageMask = vk::PipelineStageFlagBits2::eComputeShader};
        // the next frame's in-place PTLAS update waits for this frame's rays
        const vk::SemaphoreSubmitInfo raySignal{.semaphore = *frameSync.rayTimeline,
                                                .value = frameNumber,
                                                .stageMask = vk::PipelineStageFlagBits2::eAllCommands};
        const std::array submits{
            vk::SubmitInfo2{.commandBufferInfoCount = 1, .pCommandBufferInfos = &frameCmdInfo},
            vk::SubmitInfo2{.waitSemaphoreInfoCount = 1,
                            .pWaitSemaphoreInfos = &tlasWait,
                            .commandBufferInfoCount = 1,
                            .pCommandBufferInfos = &rayCmdInfo,
                            .signalSemaphoreInfoCount = 1,
                            .pSignalSemaphoreInfos = &raySignal},
        };
        graphicsQueue.submit2(submits);
    }

    vk::Result result{};
    uint32_t imageIndex{0};
    {
        ZoneScopedN("AcquireImage");
        auto acquired = swapChainKHR.acquireNextImage(UINT64_MAX, acquireSemaphore, nullptr);
        result = acquired.result;
        imageIndex = acquired.value;
    }

    if (result == vk::Result::eErrorOutOfDateKHR) {
        // no present this frame: an empty submit signals the fence after the frame work, so the slot stays usable
        graphicsQueue.submit2(vk::ArrayProxy<const vk::SubmitInfo2>{}, fence);
        swapChain.recreateSwapChain();
        rebuildSwapchainResources();
        return;
    }

    if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
        throw std::runtime_error("failed to acquire swap chain image!");
    }

    presentCmd.reset();
    {
        ZoneScopedN("RecordPresentCommands");
        recordPresentCommands(presentCmd, imageIndex);
    }

    // only the present pass waits for the image; the swapchain barrier chains with this stage
    const vk::SemaphoreSubmitInfo waitSemaphoreInfo = {.semaphore = acquireSemaphore,
                                                       .stageMask =
                                                           vk::PipelineStageFlagBits2::eColorAttachmentOutput};
    const vk::CommandBufferSubmitInfo presentCmdInfo = {.commandBuffer = *presentCmd};
    auto& renderSemaphore = *frameSync.renderFinishedSemaphore[imageIndex];
    const vk::SemaphoreSubmitInfo signalSemaphoreInfo = {.semaphore = renderSemaphore,
                                                         .stageMask = vk::PipelineStageFlagBits2::eBottomOfPipe};
    const vk::SubmitInfo2 submitInfo{.waitSemaphoreInfoCount = 1,
                                     .pWaitSemaphoreInfos = &waitSemaphoreInfo,
                                     .commandBufferInfoCount = 1,
                                     .pCommandBufferInfos = &presentCmdInfo,
                                     .signalSemaphoreInfoCount = 1,
                                     .pSignalSemaphoreInfos = &signalSemaphoreInfo};
    {
        ZoneScopedN("QueueSubmitPresent");
        graphicsQueue.submit2(submitInfo, fence);
    }

    const vk::PresentInfoKHR presentInfoKHR{.waitSemaphoreCount = 1,
                                            .pWaitSemaphores = &renderSemaphore,
                                            .swapchainCount = 1,
                                            .pSwapchains = &*swapChainKHR,
                                            .pImageIndices = &imageIndex};
    {
        ZoneScopedN("Present");
        result = presentQueue.presentKHR(presentInfoKHR);
    }

    if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR) {
        swapChain.recreateSwapChain();
        rebuildSwapchainResources();
    } else if (result != vk::Result::eSuccess) {
        throw std::runtime_error("failed to present swap chain image!");
    }

    currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
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
        ZoneScopedN("Resolve");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneResolve, *cmd, "GPU_Resolve", gpuTrace);
#endif
        recordResolve(cmd);
    }
    cmd.end();
}

void Renderer::recordRayCommands(vk::raii::CommandBuffer& cmd)
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
        ZoneScopedN("DirectLight");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneDirectLight, *cmd, "GPU_DirectLight", gpuTrace);
#endif
        recordDirectLight(cmd);
    }
    {
        ZoneScopedN("Reconstruction");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneReconstruction, *cmd, "GPU_Reconstruction", gpuTrace);
#endif
        recordReconstruction(cmd);
    }
    {
        ZoneScopedN("DebugView");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneDebugView, *cmd, "GPU_DebugView", gpuTrace);
#endif
        recordDebugView(cmd);
    }
    if (motionCheckStats[currentFrame].mapped != nullptr) {
        recordMotionCheck(cmd);
    }
    if (rayCheckStats[currentFrame].mapped != nullptr) {
        recordRayCheck(cmd);
    }
    displayCaptureExtents[currentFrame].reset();
    if (isDisplayCapturePending) {
        recordDisplayCapture(cmd);
        isDisplayCapturePending = false;
    }
    lastRecordedFrame = currentFrame;

    cmd.end();
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
    worldLights.recordUpdate(cmd, currentFrame);
    rtScene.recordBuild(cmd, currentFrame);
    cmd.end();

    // frame numbers only grow, so every signal is larger than the last
    computeTimelineValues[currentFrame] = frameNumber;
    const vk::CommandBufferSubmitInfo cmdInfo{.commandBuffer = *cmd};
    const vk::SemaphoreSubmitInfo signalInfo{.semaphore = *frameSync.computeTimeline,
                                             .value = frameNumber,
                                             .stageMask = vk::PipelineStageFlagBits2::eAllCommands};
    // the PTLAS is updated in place: the previous frame's ray passes must have finished tracing it. Its ray submit
    // is already queued, so the wait never precedes its signal
    const vk::SemaphoreSubmitInfo rayWait{.semaphore = *frameSync.rayTimeline,
                                          .value = frameNumber - 1,
                                          .stageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR};
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

void Renderer::dispatchFullscreen(const vk::raii::CommandBuffer& cmd, const vk::raii::ShaderEXT& shader) const
{
    const FramePushData push{.frameRoot = frameData.frameRootAddress(currentFrame)};
    ShaderObjects::bindComputeShader(cmd, shader);
    cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
    const vk::Extent2D extent = targets.getExtent();
    cmd.dispatch(divideRoundUp(extent.width, kFullscreenTile), divideRoundUp(extent.height, kFullscreenTile), 1);
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
        dgc.recordExecute(cmd, phase);
        gpuTimer.end(cmd, scope(GpuTimerScope::EarlyDraw, GpuTimerScope::LateDraw));
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

// vis buffer + depth -> guides of this frame's set; depth is already eShaderReadOnlyOptimal after the late hi-z build
void Renderer::recordResolve(const vk::raii::CommandBuffer& cmd)
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
    // earlier frames' reads of the guide and display images (compute, present fragment) -> this frame's writes
    const vk::MemoryBarrier2 writeAfterReadBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                            .pMemoryBarriers = &writeAfterReadBarrier,
                                            .imageMemoryBarrierCount = 1,
                                            .pImageMemoryBarriers = &visBarrier});
    dispatchFullscreen(cmd, shaderObjects.frameShaders.resolve);
    gpuTimer.end(cmd, GpuTimerScope::Resolve);
}

// ReSTIR DI: initial reservoirs, then fused resampling + shading into directRadiance
void Renderer::recordDirectLight(const vk::raii::CommandBuffer& cmd)
{
    // this frame's guide writes and last frame's reservoir writes -> reads and writes of either pass. Reads of
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
    // with no lights rtxdi_shade only writes black. Lights arrive only with a load, which resets history, so the
    // reservoirs skipped meanwhile are never reused
    if (worldLights.hasLights()) {
        dispatchFullscreen(cmd, shaderObjects.frameShaders.rtxdiInitial);
        // initial reservoirs -> the shade pass's loads
        cmd.pipelineBarrier2(dependency);
    }
    gpuTimer.end(cmd, GpuTimerScope::RtxdiInitial);
    dispatchFullscreen(cmd, shaderObjects.frameShaders.rtxdiShade);
    gpuTimer.end(cmd, GpuTimerScope::RtxdiShade);
}

void Renderer::recordReconstruction(const vk::raii::CommandBuffer& cmd)
{
    // directRadiance and guide writes -> the combine's reads
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
    const float frameDeltaMs = lastFrameTime == std::chrono::steady_clock::time_point{}
        ? 0.0f
        : std::chrono::duration<float, std::milli>(now - lastFrameTime).count();
    lastFrameTime = now;
    if (!isDlssPresented()) {
        // the output is not shown; its history would be stale when it is
        isDlssHistoryValid = false;
        gpuTimer.end(cmd, GpuTimerScope::Dlss);
        gpuTimer.end(cmd, GpuTimerScope::Post);
        return;
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
    dlss.evaluate(cmd, Dlss::Frame{
                           .color = toDlss(targets.noisyColor()),
                           .depth = toDlss(targets.depth(), vk::ImageAspectFlagBits::eDepth),
                           .motion = toDlss(targets.guide(frameNumber, GuideImage::Motion)),
                           .diffuseAlbedo = toDlss(targets.guide(frameNumber, GuideImage::DiffuseAlbedo)),
                           .specularAlbedo = toDlss(targets.guide(frameNumber, GuideImage::SpecAlbedo)),
                           .normalRoughness = toDlss(targets.guide(frameNumber, GuideImage::NormalRoughness)),
                           .output = toDlss(targets.upscaled()),
                           .exposure = toDlss(targets.exposure()),
                           // NDC units, y down like pixels
                           .jitterPixels = cameraData.jitterOffset * 0.5f *
                               glm::vec2(renderExtent.width, renderExtent.height),
                           .view = cameraData.view,
                           .proj = cameraData.proj,
                           .frameDeltaMs = frameDeltaMs,
                           .isReset = isHistoryReset,
                       });
    isDlssHistoryValid = true;
    // NGX binds its own pipelines and descriptor sets, which invalidate the heap bindings
    cmd.bindResourceHeapEXT(descriptorManager.resourceHeapInfo);
    cmd.bindSamplerHeapEXT(descriptorManager.samplerHeapInfo);
    gpuTimer.end(cmd, GpuTimerScope::Dlss);
    post.record(cmd, currentFrame, frameData.frameRootAddress(currentFrame), targets.getDisplayExtent(),
                frameDeltaMs * 1e-3f);
    gpuTimer.end(cmd, GpuTimerScope::Post);
}

void Renderer::recordDebugView(const vk::raii::CommandBuffer& cmd)
{
    // guide and directRadiance writes -> reads; submission order also covers the previous frame's set (Reprojection)
    const vk::MemoryBarrier2 guideBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
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
