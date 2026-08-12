#include "vk_renderer.hpp"
#include "../Constants.h"
#include "../util/vk_tracy.hpp"
#include "../util/vk_utils.hpp"
#if ENGINE_ENABLE_IMGUI
    #include "imgui.h"
    #include "imgui_impl_vulkan.h"
#endif

#include <format>

Renderer::Renderer(Device& device, SwapChain& swapChain, ResourceManager& resourceManager,
                   DescriptorManager& descriptorManager, Pipeline& pipeline, Camera& camera,
                   VkTracyContext* tracyContext, bool imguiEnabled) :
    device(device), swapChain(swapChain), resourceManager(resourceManager), descriptorManager(descriptorManager),
    pipeline(pipeline), camera(camera), dgc(device, resourceManager, descriptorManager, pipeline),
    tracyContext(tracyContext), imguiEnabled(imguiEnabled)
{
    dgc.init();
}

void Renderer::setTracyContext(VkTracyContext* tracyContextIn) { tracyContext = tracyContextIn; }

void Renderer::rebuildSwapchainResources() const
{
    ZoneScopedN("SwapchainRecreate");
    resourceManager.updateSwapChainExtent(swapChain.swapChainExtent);
    resourceManager.updateSwapChainImageFormat(swapChain.swapChainImageFormat);
    resourceManager.setSwapChainImageCount(static_cast<uint32_t>(swapChain.swapChainImages.size()));
    resourceManager.createColorResources();
    resourceManager.createDepthResources();
    // Aspect / render-target size changed (resize and out-of-date paths).
    camera.setFov(camera.getFovDegrees());
    TracyPlot("Vulkan/SwapchainWidth", static_cast<double>(swapChain.swapChainExtent.width));
    TracyPlot("Vulkan/SwapchainHeight", static_cast<double>(swapChain.swapChainExtent.height));
    TracyPlot("Vulkan/SwapchainImagesInUse", static_cast<double>(swapChain.swapChainImages.size()));
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
    TracyPlot("Vulkan/SwapchainImagesInUse", static_cast<double>(swapChain.swapChainImages.size()));

    {
        ZoneScopedN("FenceWait");
        while (vk::Result::eTimeout == deviceRef.waitForFences(fence, vk::True, UINT64_MAX))
            ;
    }

    vk::Result result;
    uint32_t imageIndex;
    {
        ZoneScopedN("AcquireImage");
        auto acquired = swapChainKHR.acquireNextImage(UINT64_MAX, presentSemaphore, nullptr);
        result = acquired.result;
        imageIndex = acquired.value;
    }

    if (result == vk::Result::eErrorOutOfDateKHR) {
        ZoneScopedN("SwapchainRecreate_Acquire");
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

    vk::SemaphoreSubmitInfo waitSemaphoreInfo = {.semaphore = presentSemaphore,
                                                 .stageMask = vk::PipelineStageFlagBits2::eTopOfPipe};

    vk::CommandBufferSubmitInfo commandBufferInfo = {.commandBuffer = *commandBuffer};

    auto& renderSemaphore = *resourceManager.renderFinishedSemaphore[imageIndex];
    vk::SemaphoreSubmitInfo signalSemaphoreInfo = {.semaphore = renderSemaphore,
                                                   .stageMask = vk::PipelineStageFlagBits2::eBottomOfPipe};

    {
        ZoneScopedN("UpdateUBO");
        resourceManager.updateUniformBuffers(currentFrame);
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
        ZoneScopedN("SwapchainRecreate_Present");
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
    const bool gpuTrace = tracyContext != nullptr && tracyContext->active();
    TracyVkCtx const gpuCtx = gpuTrace ? tracyContext->handle() : nullptr;
#endif

    cmd.begin({});

    const vk::ImageSubresourceRange colorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    {
        ZoneScopedN("TransitionToRender");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneTransitionToRender, *cmd, "GPU_TransitionToRender", gpuTrace);
#endif
        transitionImageLayout(&cmd, swapChain.swapChainImages[imageIndex], 1, vk::ImageLayout::eUndefined,
                              vk::ImageLayout::eColorAttachmentOptimal, colorRange, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, vk::PipelineStageFlagBits2::eTopOfPipe,
                              vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eNone,
                              vk::AccessFlagBits2::eColorAttachmentWrite);
    }

    // NVIDIA compressed clear requires all-0 or all-1 components for sRGB targets
    // (BestPractices-NVIDIA-ClearColor-NotCompressed). Alpha 1.0 blocked compression.
    const vk::ClearValue clearColor = vk::ClearColorValue(0.0f, 0.0f, 0.0f, 0.0f);
    const vk::ClearValue clearDepth = vk::ClearDepthStencilValue(1.0f, 0);
    vk::RenderingAttachmentInfo colorAttachmentInfo = {.imageView = resourceManager.colorImageView,
                                                       .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                                                       .resolveMode = vk::ResolveModeFlagBits::eAverage,
                                                       .resolveImageView = swapChain.swapChainImageViews[imageIndex],
                                                       .resolveImageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                                                       .loadOp = vk::AttachmentLoadOp::eClear,
                                                       .storeOp = vk::AttachmentStoreOp::eStore,
                                                       .clearValue = clearColor};
    vk::RenderingAttachmentInfo depthAttachmentInfo = {.imageView = resourceManager.depthImageView,
                                                       .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
                                                       .loadOp = vk::AttachmentLoadOp::eClear,
                                                       .storeOp = vk::AttachmentStoreOp::eDontCare,
                                                       .clearValue = clearDepth};

    vk::RenderingInfo renderingInfo = {.renderArea = {.offset = {0, 0}, .extent = swapChain.swapChainExtent},
                                       .layerCount = 1,
                                       .colorAttachmentCount = 1,
                                       .pColorAttachments = &colorAttachmentInfo,
                                       .pDepthAttachment = &depthAttachmentInfo};

    dgc.updateSequences(currentFrame, camera);

    {
        ZoneScopedN("DgcPreprocess");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneDgcPreprocess, *cmd, "GPU_DgcPreprocess", gpuTrace);
#endif
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline.pipeline);
        cmd.setViewport(0,
                        vk::Viewport(0.0f, 0.0f, static_cast<float>(swapChain.swapChainExtent.width),
                                     static_cast<float>(swapChain.swapChainExtent.height), 0.0f, 1.0f));
        cmd.setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), swapChain.swapChainExtent));
        cmd.bindResourceHeapEXT(descriptorManager.resourceHeapInfo);
        cmd.bindSamplerHeapEXT(descriptorManager.samplerHeapInfo);

        // bind once: preprocess snapshots this state; it persists into the render pass
        dgc.recordPreprocess(cmd);
    }

    cmd.beginRendering(renderingInfo);
    {
        ZoneScopedN("DrawCalls");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneDrawCalls, *cmd, "GPU_DrawCalls", gpuTrace);
#endif
        dgc.recordExecute(cmd);
    }

#if ENGINE_ENABLE_IMGUI
    if (imguiEnabled && imguiVisible) {
        ZoneScopedN("RenderImGui");
    #ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneImGui, *cmd, "GPU_ImGui", gpuTrace);
    #endif
        if (ImGui::GetDrawData() != nullptr) {
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), *cmd);
        }
    }
#endif

    cmd.endRendering();
    {
        ZoneScopedN("TransitionToPresent");
#ifdef TRACY_ENABLE
        TracyVkNamedZone(gpuCtx, gpuZoneTransitionToPresent, *cmd, "GPU_TransitionToPresent", gpuTrace);
#endif
        transitionImageLayout(&cmd, swapChain.swapChainImages[imageIndex], 1, vk::ImageLayout::eColorAttachmentOptimal,
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
