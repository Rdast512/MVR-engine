#include "vk_renderer.hpp"
#include "../Constants.h"
#include "../util/vk_tracy.hpp"
#include "../util/vk_utils.hpp"
#include "push_data.hpp"
#if ENGINE_ENABLE_IMGUI
    #include "imgui.h"
    #include "imgui_impl_vulkan.h"
#endif

#include <stdexcept>

Renderer::Renderer(Device& device, SwapChain& swapChain, ResourceManager& resourceManager,
                   DescriptorManager& descriptorManager, ShaderObjects& shaderObjects, Camera& camera,
                   VkTracyContext* tracyContext, bool imguiEnabled) :
    device(device), swapChain(swapChain), resourceManager(resourceManager), descriptorManager(descriptorManager),
    camera(camera), shaderObjects(shaderObjects), dgc(device, resourceManager, descriptorManager, shaderObjects),
    culling(device, resourceManager, shaderObjects, dgc),
    hiz(device, resourceManager, descriptorManager, shaderObjects), gpuTimer(device), tracyContext(tracyContext),
    imguiEnabled(imguiEnabled)
{
    dgc.init();
}

void Renderer::setTracyContext(VkTracyContext* tracyContextIn) { tracyContext = tracyContextIn; }

void Renderer::rebuildSwapchainResources()
{
    resourceManager.updateSwapChainExtent(swapChain.swapChainExtent);
    resourceManager.updateSwapChainImageFormat(swapChain.swapChainImageFormat);
    resourceManager.setSwapChainImageCount(static_cast<uint32_t>(swapChain.swapChainImages.size()));
    resourceManager.createDepthResources();
    hiz.recreate();
    // aspect ratio follows the new extent
    camera.setFov(camera.getFovDegrees());
}

void Renderer::drawFrame()
{
    ZoneScopedN("Renderer::drawFrame");
    auto& deviceRef = device.vkdevice;
    auto& graphicsQueue = device.graphicsQueue;
    auto& presentQueue = device.presentQueue;
    auto& swapChainKHR = swapChain.swapChain;
    auto& fence = *resourceManager.inFlightFences[currentFrame];
    auto& presentSemaphore = *resourceManager.presentCompleteSemaphore[currentFrame];
    auto& commandBuffer = resourceManager.commandBuffers[currentFrame];

    resourceManager.tracyPlotResources();

    {
        ZoneScopedN("FenceWait");
        while (vk::Result::eTimeout == deviceRef.waitForFences(fence, vk::True, UINT64_MAX))
            ;
    }

    // after the fence: this slot's camera buffer and query results are no longer touched by the GPU
    camera.updateCameraData(currentFrame);
    gpuTimer.collect(currentFrame);

    vk::Result result{};
    uint32_t imageIndex{0};
    {
        ZoneScopedN("AcquireImage");
        auto acquired = swapChainKHR.acquireNextImage(UINT64_MAX, presentSemaphore, nullptr);
        result = acquired.result;
        imageIndex = acquired.value;
    }

    if (result == vk::Result::eErrorOutOfDateKHR) {
        swapChain.recreateSwapChain();
        rebuildSwapchainResources();
        return;
    }

    if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
        throw std::runtime_error("failed to acquire swap chain image!");
    }

    deviceRef.resetFences(fence);
    commandBuffer.reset();
    {
        ZoneScopedN("RecordCommandBuffer");
        recordCommandBuffer(imageIndex);
    }

    const vk::SemaphoreSubmitInfo waitSemaphoreInfo = {.semaphore = presentSemaphore,
                                                       .stageMask = vk::PipelineStageFlagBits2::eTopOfPipe};

    const vk::CommandBufferSubmitInfo commandBufferInfo = {.commandBuffer = *commandBuffer};

    auto& renderSemaphore = *resourceManager.renderFinishedSemaphore[imageIndex];
    const vk::SemaphoreSubmitInfo signalSemaphoreInfo = {.semaphore = renderSemaphore,
                                                         .stageMask = vk::PipelineStageFlagBits2::eBottomOfPipe};

    {
        ZoneScopedN("UpdateUBO");
        resourceManager.updateUniformBuffers(currentFrame, debugView.packed());
    }

    const vk::SubmitInfo2 submitInfo{.waitSemaphoreInfoCount = 1,
                                     .pWaitSemaphoreInfos = &waitSemaphoreInfo,
                                     .commandBufferInfoCount = 1,
                                     .pCommandBufferInfos = &commandBufferInfo,
                                     .signalSemaphoreInfoCount = 1,
                                     .pSignalSemaphoreInfos = &signalSemaphoreInfo};
    {
        ZoneScopedN("QueueSubmit");
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

void Renderer::recordCommandBuffer(uint32_t imageIndex)
{
    ZoneScoped;
    auto& commandBuffers = resourceManager.commandBuffers;
    auto& cmd = commandBuffers[currentFrame];

#ifdef TRACY_ENABLE
    const bool gpuTrace = tracyContext != nullptr && tracyContext->isActive();
    TracyVkCtx const gpuCtx = gpuTrace ? tracyContext->getHandle() : nullptr;
#endif

    cmd.begin({});

    const vk::ImageSubresourceRange colorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    {
        ZoneScopedN("TransitionToRender");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneTransitionToRender, *cmd, "GPU_TransitionToRender", gpuTrace);
#endif
        transitionImageLayout(&cmd, swapChain.swapChainImages[imageIndex], vk::ImageLayout::eUndefined,
                              vk::ImageLayout::eColorAttachmentOptimal, colorRange, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, vk::PipelineStageFlagBits2::eTopOfPipe,
                              vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eNone,
                              vk::AccessFlagBits2::eColorAttachmentWrite);
        // cleared on load; waits for the previous frame's hi-z reads
        transitionImageLayout(&cmd, resourceManager.depthAttachment.image, vk::ImageLayout::eUndefined,
                              vk::ImageLayout::eDepthStencilAttachmentOptimal,
                              {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, vk::PipelineStageFlagBits2::eComputeShader,
                              vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                  vk::PipelineStageFlagBits2::eLateFragmentTests,
                              vk::AccessFlagBits2::eNone,
                              vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                  vk::AccessFlagBits2::eDepthStencilAttachmentWrite);
    }

    // also sizes the DGC buffers for this slot
    culling.prepareFrame(currentFrame, camera, hiz);
    gpuTimer.begin(cmd, currentFrame);
    const RasterVariant rasterVariant = debugView.rasterVariant();

    cmd.bindResourceHeapEXT(descriptorManager.resourceHeapInfo);
    cmd.bindSamplerHeapEXT(descriptorManager.samplerHeapInfo);
    // shader objects require the *WithCount variants
    cmd.setViewportWithCount(vk::Viewport(0.0f, 0.0f, static_cast<float>(swapChain.swapChainExtent.width),
                                          static_cast<float>(swapChain.swapChainExtent.height), 0.0f, 1.0f));
    cmd.setScissorWithCount(vk::Rect2D(vk::Offset2D(0, 0), swapChain.swapChainExtent));

    {
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneEarlyPhase, *cmd, "GPU_EarlyPhase", gpuTrace);
#endif
        recordPhase(cmd, CullPhase::Early, imageIndex, rasterVariant);
    }
    {
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneLatePhase, *cmd, "GPU_LatePhase", gpuTrace);
#endif
        recordPhase(cmd, CullPhase::Late, imageIndex, rasterVariant);
    }
    MeshletCulling::recordCounterReadback(cmd);

    {
        ZoneScopedN("TransitionToPresent");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneTransitionToPresent, *cmd, "GPU_TransitionToPresent", gpuTrace);
#endif
        transitionImageLayout(&cmd, swapChain.swapChainImages[imageIndex], vk::ImageLayout::eColorAttachmentOptimal,
                              vk::ImageLayout::ePresentSrcKHR, colorRange, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                              vk::PipelineStageFlagBits2::eBottomOfPipe, vk::AccessFlagBits2::eColorAttachmentWrite,
                              {});
    }

    if (tracyContext) {
        ZoneScopedN("TracyVkCollect");
        tracyContext->collect(cmd);
    }

    cmd.end();
}

void Renderer::waitIdle() const { device.vkdevice.waitIdle(); }

// cull -> preprocess -> raster -> hi-z; the late phase draws over the early one and adds ImGui
void Renderer::recordPhase(vk::raii::CommandBuffer& cmd, CullPhase phase, uint32_t imageIndex,
                           RasterVariant rasterVariant)
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
        // preprocess takes its state from the per-group state command buffers
        dgc.recordPreprocess(cmd, rasterVariant, phase);
        gpuTimer.end(cmd, scope(GpuTimerScope::EarlyPreprocess, GpuTimerScope::LatePreprocess));
    }

    if (!isEarly) {
        // the early hi-z build sampled depth; the late raster loads color and depth
        const vk::MemoryBarrier2 colorBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,
        };
        // the Hi-Z view samples the early pyramid in the late pass
        const vk::MemoryBarrier2 pyramidBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
        };
        const std::array memoryBarriers = {colorBarrier, pyramidBarrier};
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
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = static_cast<uint32_t>(memoryBarriers.size()),
                                                .pMemoryBarriers = memoryBarriers.data(),
                                                .imageMemoryBarrierCount = 1,
                                                .pImageMemoryBarriers = &depthBarrier});
    }

    // nvidia compressed clear needs all-0 or all-1 components (BestPractices-NVIDIA-ClearColor-NotCompressed)
    const vk::ClearValue clearColor = vk::ClearColorValue(0.0f, 0.0f, 0.0f, 0.0f);
    const vk::ClearValue clearDepth = vk::ClearDepthStencilValue(1.0f, 0);
    const vk::AttachmentLoadOp loadOp = isEarly ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
    const vk::RenderingAttachmentInfo colorAttachmentInfo = {
        .imageView = swapChain.swapChainImageViews[imageIndex],
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = loadOp,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = clearColor};
    const vk::RenderingAttachmentInfo depthAttachmentInfo = {.imageView = resourceManager.depthAttachment.view,
                                                             .imageLayout =
                                                                 vk::ImageLayout::eDepthStencilAttachmentOptimal,
                                                             .loadOp = loadOp,
                                                             .storeOp = vk::AttachmentStoreOp::eStore,
                                                             .clearValue = clearDepth};

    const vk::RenderingInfo renderingInfo = {.renderArea = {.offset = {0, 0}, .extent = swapChain.swapChainExtent},
                                             .layerCount = 1,
                                             .colorAttachmentCount = 1,
                                             .pColorAttachments = &colorAttachmentInfo,
                                             .pDepthAttachment = &depthAttachmentInfo};

    cmd.beginRendering(renderingInfo);
    {
        ZoneScopedN("DrawCalls");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneDrawCalls, *cmd, "GPU_DrawCalls", gpuTrace);
#endif
        dgc.recordExecute(cmd, rasterVariant, phase);
        gpuTimer.end(cmd, scope(GpuTimerScope::EarlyDraw, GpuTimerScope::LateDraw));
    }

    if (!isEarly && debugView.view == DebugView::HiZPyramid) {
        recordHiZView(cmd);
    }

#if ENGINE_ENABLE_IMGUI
    if (!isEarly && imguiEnabled) {
        ZoneScopedN("RenderImGui");
    #ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneImGui, *cmd, "GPU_ImGui", gpuTrace);
    #endif
        if (ImGui::GetDrawData() != nullptr) {
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), *cmd);
            // classic descriptor sets invalidate the heap bindings the late hi-z build relies on
            cmd.bindResourceHeapEXT(descriptorManager.resourceHeapInfo);
            cmd.bindSamplerHeapEXT(descriptorManager.samplerHeapInfo);
        }
    }
#endif

    cmd.endRendering();
    if (!isEarly) {
        gpuTimer.end(cmd, GpuTimerScope::Ui);
    }

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
    // DGC execute leaves bound graphics state undefined
    cmd.setViewportWithCount(vk::Viewport(0.0f, 0.0f, static_cast<float>(swapChain.swapChainExtent.width),
                                          static_cast<float>(swapChain.swapChainExtent.height), 0.0f, 1.0f));
    cmd.setScissorWithCount(vk::Rect2D(vk::Offset2D(0, 0), swapChain.swapChainExtent));
    cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
    cmd.drawMeshTasksEXT(1, 1, 1);
}
