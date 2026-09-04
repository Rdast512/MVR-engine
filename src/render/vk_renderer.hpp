#pragma once
#include "core/vk_descriptors.hpp"
#include "core/vk_resource_manager.hpp"
#include "core/vk_swapchain.hpp"
#include "scene/vk_camera.hpp"
#include "vk_dgc.hpp"
#include "vk_pipeline.hpp"

class VkTracyContext;

class Renderer
{
public:
    Renderer(Device& device, SwapChain& swapChain, ResourceManager& resourceManager,
             DescriptorManager& descriptorManager, Pipeline& pipeline, Camera& camera,
             VkTracyContext* tracyContext = nullptr, bool imguiEnabled = false);

    void setTracyContext(VkTracyContext* tracyContextIn);
    void setImGuiVisible(bool visible) noexcept { imguiVisible = visible; }
    [[nodiscard]] bool isImGuiVisible() const noexcept { return imguiVisible; }
    void rebuildSwapchainResources() const;
    void drawFrame();
    void waitIdle() const;

    uint32_t currentFrame = 0;

private:
    void recordCommandBuffer(uint32_t imageIndex);

    Device& device;
    SwapChain& swapChain;
    ResourceManager& resourceManager;
    DescriptorManager& descriptorManager;
    Pipeline& pipeline;
    Camera& camera;
    DeviceGeneratedCommands dgc;
    VkTracyContext* tracyContext = nullptr;
    bool imguiEnabled = false;
    // Drawn only while the UI toggle is open (I key).
    bool imguiVisible = false;
};
