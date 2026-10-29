#pragma once
#include "core/vk_descriptors.hpp"
#include "core/vk_resource_manager.hpp"
#include "core/vk_swapchain.hpp"
#include "scene/vk_camera.hpp"
#include "vk_dgc.hpp"
#include "vk_gpu_timer.hpp"
#include "vk_meshlet_culling.hpp"
#include "vk_shader_objects.hpp"

class VkTracyContext;

class Renderer
{
public:
    Renderer(Device& device, SwapChain& swapChain, ResourceManager& resourceManager,
             DescriptorManager& descriptorManager, ShaderObjects& shaderObjects, Camera& camera,
             VkTracyContext* tracyContext = nullptr, bool imguiEnabled = false);

    void setTracyContext(VkTracyContext* tracyContextIn);
    void rebuildSwapchainResources() const;
    void drawFrame();
    void waitIdle() const;

    uint32_t currentFrame = 0;
    // read once per recorded frame; the UI changes it between frames
    DebugViewSettings debugView;

    [[nodiscard]] MeshletCulling& getCulling() noexcept { return culling; }
    [[nodiscard]] const GpuTimer& getGpuTimer() const noexcept { return gpuTimer; }

private:
    void recordCommandBuffer(uint32_t imageIndex);

    Device& device;
    SwapChain& swapChain;
    ResourceManager& resourceManager;
    DescriptorManager& descriptorManager;
    Camera& camera;
    DeviceGeneratedCommands dgc;
    // after dgc: writes into its sequence buffers
    MeshletCulling culling;
    GpuTimer gpuTimer;
    VkTracyContext* tracyContext = nullptr;
    // unused when imgui is compiled out
    [[maybe_unused]] bool imguiEnabled = false;
};
