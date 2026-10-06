#pragma once
#include "../Constants.h"
#include "assets_loader.hpp"
#include "texture_manager.hpp"
#include "../render/vk_pipeline.hpp"
#include "../render/vk_renderer.hpp"
#include "../util/vk_tracy.hpp"
#include "vk_allocator.hpp"
#include "vk_descriptors.hpp"
#include "vk_device.hpp"
#include "vk_resource_manager.hpp"
#include "vk_swapchain.hpp"
#include "scene/vk_camera.hpp"
#include "scene/vk_scene.hpp"

#include <optional>
#include <string>


class Engine{
    // Driven by Constants.h ENGINE_ENABLE_IMGUI. When false, no ImGui Vulkan/SDL backends.
    bool enableImGui = (ENGINE_ENABLE_IMGUI != 0);
    // Runtime UI visibility (I key). Hidden + game-focused until toggled open. No-op if !enableImGui.
    bool imguiUiOpen = false;

    SDL_Window *window = nullptr;
    std::unique_ptr<Device> device;
    std::unique_ptr<VkAllocator> allocator;
    std::unique_ptr<SwapChain> swapChain;
    std::unique_ptr<Scene> scene;
    std::unique_ptr<AssetsLoader> assetsLoader;
    std::unique_ptr<ResourceManager> resourceManager;
    std::unique_ptr<VkTracyContext> tracyContext;
    std::unique_ptr<TextureManager> textureManager;
    std::unique_ptr<DescriptorManager> descriptorManager;
    std::unique_ptr<Camera> camera;
    std::unique_ptr<Pipeline> pipeline;
    std::unique_ptr<Renderer> renderer;
    bool initialized = false;
    std::chrono::high_resolution_clock::time_point lastTime;
    std::chrono::high_resolution_clock::time_point fpsTime;
    int frameCount = 0;
    float fps = 0.0f;
    float frameMs = 0.0f;

    vk::raii::DescriptorPool imguiDescriptorPool = nullptr;
    VkFormat imguiColorFormat = VK_FORMAT_UNDEFINED;
    VkFormat imguiDepthFormat = VK_FORMAT_UNDEFINED;
    VkPipelineRenderingCreateInfoKHR imguiPipelineRenderingInfo{};
    std::vector<std::filesystem::path> discoveredAssets;
    int selectedAssetIndex = -1;
    bool hasScannedAssets = false;
    float loadedModelPosition[3] = {0.0f, 0.0f, 0.0f};
    char assetsPathInput[260] = ENGINE_MODELS_DIR;
    std::string loadStatus;
    bool isLoadStatusError = false;
    // UI actions run after the ImGui frame so no panel iterates storage that changes under it
    bool hasPendingLoad = false;
    std::optional<EntityId> pendingUnload;
    void createImGuiDescriptorPool();
    // stops ImGui from reading the mouse / changing the cursor while the game has focus
    void setImGuiInputEnabled(bool enabled);
    void drawImGui();
    void drawStatsOverlay() const;
    void drawLoadPanel();
    void drawLoadedModelsPanel();
    void applyPendingModelActions();
    void loadSelectedModel();
    void unloadModel(EntityId id);
    // Full host-side swapchain recreate (waitIdle inside swapchain, color/depth/sync, ImGui).
    void recreateSwapchain();

public:
    ~Engine();
    void initialize();
    void run();
    void render();
    void cleanup();
    void shutdown();
    // fills discoveredAssets with the subfolders of assetsPathInput holding a .gltf/.glb/.obj
    void scanFolder();
};