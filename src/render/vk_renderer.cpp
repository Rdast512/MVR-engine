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
    // matches passes/resolve.slang and passes/debug_view.slang
    constexpr uint32_t kFullscreenTile = 8;

    constexpr uint32_t divideRoundUp(uint32_t value, uint32_t divisor) { return (value + divisor - 1) / divisor; }
} // namespace

Renderer::Renderer(Device& device, SwapChain& swapChain, ResourceManager& resourceManager,
                   DescriptorManager& descriptorManager, ShaderObjects& shaderObjects, Camera& camera,
                   VkTracyContext* tracyContext, bool imguiEnabled) :
    device(device), swapChain(swapChain), resourceManager(resourceManager), descriptorManager(descriptorManager),
    camera(camera), shaderObjects(shaderObjects), dgc(device, resourceManager, descriptorManager, shaderObjects),
    culling(device, resourceManager, shaderObjects, dgc),
    hiz(device, resourceManager, descriptorManager, shaderObjects), targets(device, resourceManager, descriptorManager),
    gpuTimer(device), tracyContext(tracyContext),
    imguiEnabled(imguiEnabled)
{
    const vk::DeviceSize maxPushData = device.capabilities.descriptorHeap.maxPushDataSize;
    if (kMaxPushDataBytes > maxPushData) {
        throw std::runtime_error(
            std::format("push data of {} B exceeds the device's maxPushDataSize of {} B", kMaxPushDataBytes, maxPushData));
    }
    dgc.init();
}

void Renderer::setTracyContext(VkTracyContext* tracyContextIn) { tracyContext = tracyContextIn; }

void Renderer::rebuildSwapchainResources()
{
    resourceManager.updateSwapChainExtent(swapChain.swapChainExtent);
    resourceManager.updateSwapChainImageFormat(swapChain.swapChainImageFormat);
    resourceManager.setSwapChainImageCount(static_cast<uint32_t>(swapChain.swapChainImages.size()));
    // render resolution equals display until upscaling (M3)
    resourceManager.renderExtent = swapChain.swapChainExtent;
    resourceManager.createDepthResources();
    targets.recreate(resourceManager.renderExtent);
    hiz.recreate();
    camera.setRenderExtent(resourceManager.renderExtent);
}

void Renderer::drawFrame()
{
    ZoneScopedN("Renderer::drawFrame");
    auto& deviceRef = device.vkdevice;
    auto& graphicsQueue = device.graphicsQueue;
    auto& presentQueue = device.presentQueue;
    auto& swapChainKHR = swapChain.swapChain;
    auto& fence = *resourceManager.inFlightFences[currentFrame];
    auto& acquireSemaphore = *resourceManager.presentCompleteSemaphore[currentFrame];
    auto& frameCmd = resourceManager.commandBuffers[currentFrame];
    auto& presentCmd = resourceManager.presentCommandBuffers[currentFrame];

    {
        ZoneScopedN("FenceWait");
        while (vk::Result::eTimeout == deviceRef.waitForFences(fence, vk::True, UINT64_MAX))
            ;
    }
    vmaSetCurrentFrameIndex(resourceManager.allocator.allocator, ++frameNumber);
    resourceManager.tracyPlotMemoryBudget();

    // after the fence: this slot's camera buffer and query results are no longer touched by the GPU
    camera.updateCameraData(currentFrame);
    targets.prepareFrame(currentFrame, frameNumber);
    gpuTimer.collect(currentFrame);

    // the fence covers both submits of this slot; every path below ends in a submit that signals it
    deviceRef.resetFences(fence);
    frameCmd.reset();
    {
        ZoneScopedN("RecordFrameCommands");
        recordFrameCommands(frameCmd);
    }
    {
        ZoneScopedN("UpdateUBO");
        // resize and swapchain recreation arrive as camera cuts
        const bool isHistoryReset = camera.isCut() || culling.isSceneLayoutChanged();
        if (isHistoryReset) {
            TracyMessageL("History reset");
        }
        resourceManager.updateUniformBuffers(currentFrame, debugView.packed(),
                                             isHistoryReset ? FrameFlag::HistoryReset : 0u, frameNumber);
    }
    {
        ZoneScopedN("QueueSubmitFrame");
        // no wait: cull, raster and compute run while the CPU acquires the image
        const vk::CommandBufferSubmitInfo frameCmdInfo{.commandBuffer = *frameCmd};
        graphicsQueue.submit2(vk::SubmitInfo2{.commandBufferInfoCount = 1, .pCommandBufferInfos = &frameCmdInfo});
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
    auto& renderSemaphore = *resourceManager.renderFinishedSemaphore[imageIndex];
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
        transitionImageLayout(&cmd, resourceManager.depthAttachment.image, vk::ImageLayout::eUndefined,
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
    {
        ZoneScopedN("DebugView");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneDebugView, *cmd, "GPU_DebugView", gpuTrace);
#endif
        recordDebugView(cmd);
    }

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
    const FramePushData push{.frameRoot = resourceManager.frameRootAddress(currentFrame)};
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
            .image = *resourceManager.depthAttachment.image,
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
        .imageView = resourceManager.depthAttachment.view,
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

void Renderer::recordDebugView(const vk::raii::CommandBuffer& cmd)
{
    // guide writes -> guide reads; submission order also covers the previous frame's set (Reprojection)
    const vk::MemoryBarrier2 guideBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &guideBarrier});
    dispatchFullscreen(cmd, shaderObjects.frameShaders.debugView);
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
    const PresentPushData push{.frameRoot = resourceManager.frameRootAddress(currentFrame),
                               .displaySize = displayExtent};
    cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
    cmd.drawMeshTasksEXT(1, 1, 1);
    if (debugView.view == DebugView::HiZPyramid) {
        recordHiZView(cmd);
    }
    gpuTimer.end(cmd, GpuTimerScope::Present);

#if ENGINE_ENABLE_IMGUI
    if (imguiEnabled && ImGui::GetDrawData() != nullptr) {
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
