#pragma once
#include "core/vk_descriptors.hpp"
#include "core/vk_resource_manager.hpp"
#include "core/vk_swapchain.hpp"
#include "scene/vk_camera.hpp"
#include "vk_dgc.hpp"
#include "vk_gpu_timer.hpp"
#include "vk_hiz.hpp"
#include "debug_view.hpp"
#include "vk_meshlet_culling.hpp"
#include "vk_render_targets.hpp"
#include "vk_shader_objects.hpp"

class VkTracyContext;

class Renderer
{
public:
    Renderer(Device& device, SwapChain& swapChain, ResourceManager& resourceManager,
             DescriptorManager& descriptorManager, ShaderObjects& shaderObjects, Camera& camera,
             VkTracyContext* tracyContext = nullptr, bool imguiEnabled = false);

    void setTracyContext(VkTracyContext* tracyContextIn);
    void rebuildSwapchainResources();
    void drawFrame();
    void waitIdle() const;

    uint32_t currentFrame = 0;
    // monotonic; VMA refreshes its memory budget per frame index
    uint32_t frameNumber = 0;
    // read once per recorded frame; the UI changes it between frames
    DebugViewSettings debugView;

    [[nodiscard]] MeshletCulling& getCulling() noexcept { return culling; }
    [[nodiscard]] const GpuTimer& getGpuTimer() const noexcept { return gpuTimer; }
    [[nodiscard]] uint32_t getHiZMipCount() const noexcept { return hiz.mipCount(); }

private:
    void recordCommandBuffer(uint32_t imageIndex);
    void recordPhase(vk::raii::CommandBuffer& cmd, CullPhase phase);
    void recordResolve(const vk::raii::CommandBuffer& cmd);
    void recordDebugView(const vk::raii::CommandBuffer& cmd);
    void recordPresent(vk::raii::CommandBuffer& cmd, uint32_t imageIndex);
    // DebugView::HiZPyramid: covers the present pass with the pyramid of the final depth
    void recordHiZView(const vk::raii::CommandBuffer& cmd) const;
    // one thread per render pixel, FramePushData
    void dispatchFullscreen(const vk::raii::CommandBuffer& cmd, const vk::raii::ShaderEXT& shader) const;
    static void setViewport(const vk::raii::CommandBuffer& cmd, vk::Extent2D extent);

    Device& device;
    SwapChain& swapChain;
    ResourceManager& resourceManager;
    DescriptorManager& descriptorManager;
    Camera& camera;
    const ShaderObjects& shaderObjects;
    DeviceGeneratedCommands dgc;
    // after dgc: writes into its sequence buffers
    MeshletCulling culling;
    HiZPyramid hiz;
    // after hiz: both are recreated from the depth attachment
    RenderTargets targets;
    GpuTimer gpuTimer;
    VkTracyContext* tracyContext = nullptr;
    // unused when imgui is compiled out
    [[maybe_unused]] bool imguiEnabled = false;
};
