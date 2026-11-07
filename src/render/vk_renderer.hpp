#pragma once
#include "core/vk_descriptors.hpp"
#include "core/vk_resource_manager.hpp"
#include "core/vk_swapchain.hpp"
#include "scene/vk_camera.hpp"
#include "vk_dgc.hpp"
#include "vk_gpu_timer.hpp"
#include "vk_hiz.hpp"
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
    // DebugView::HiZPyramid: covers the late pass with the pyramid the late phase tested against
    void recordHiZView(const vk::raii::CommandBuffer& cmd) const;
    void recordPhase(vk::raii::CommandBuffer& cmd, CullPhase phase, uint32_t imageIndex, RasterVariant rasterVariant);

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
    GpuTimer gpuTimer;
    VkTracyContext* tracyContext = nullptr;
    // unused when imgui is compiled out
    [[maybe_unused]] bool imguiEnabled = false;
};
