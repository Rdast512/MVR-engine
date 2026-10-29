#pragma once
#include "../Constants.h"
#include "assets_loader.hpp"
#include "texture_manager.hpp"
#include "../render/vk_renderer.hpp"
#include "../render/vk_shader_objects.hpp"
#include "../util/vk_tracy.hpp"
#include "vk_allocator.hpp"
#include "vk_descriptors.hpp"
#include "vk_device.hpp"
#include "vk_resource_manager.hpp"
#include "vk_swapchain.hpp"
#include "scene/vk_camera.hpp"
#include "scene/vk_scene.hpp"

#include <array>
#include <optional>
#include <string>

class Engine
{
    // from the ENGINE_ENABLE_IMGUI CMake option; false means no ImGui backends
    bool enableImGui = (ENGINE_ENABLE_IMGUI != 0);
    // toggled by the I key; hidden and game-focused until opened
    bool imguiUiOpen = false;

    SDL_Window* window = nullptr;
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
    std::unique_ptr<ShaderObjects> shaderObjects;
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
    std::array<float, 3> loadedModelPosition{};
    char assetsPathInput[260] = ENGINE_MODELS_DIR;
    std::string loadStatus;
    bool isLoadStatusError = false;
    // UI actions run after the ImGui frame so no panel iterates storage that changes under it
    bool hasPendingLoad = false;
    std::optional<EntityId> pendingUnload;
    // instance field: side x side grid of INSTANCE_FIELD_MODEL_PATH centered on loadedModelPosition
    int instanceFieldSide = 16;
    // cell size in model bounding diameters
    float instanceFieldSpacing = 1.5f;
    bool hasPendingInstanceField = false;
    bool hasPendingInstanceFieldClear = false;
    void createImGuiDescriptorPool();
    // stops ImGui from reading the mouse / changing the cursor while the game has focus
    void setImGuiInputEnabled(bool enabled);
    void drawImGui();
    void drawStatsOverlay() const;
    void drawLoadPanel();
    void drawLoadedModelsPanel();
    void drawAssetsPanel() const;
    void drawDebugViewPanel();
    void drawCullingPanel();
    void drawInstanceFieldPanel();
    void applyPendingModelActions();
    void loadSelectedModel();
    void unloadModel(EntityId id);
    // loads the field model once, then instantiates the rest of the grid in one batch
    void spawnInstanceField();
    // erases every entity of the field model
    void clearInstanceField();
    // waits idle, rebuilds swapchain-sized resources and the ImGui image count
    void recreateSwapchain();

public:
    ~Engine();
    void initialize();
    void run();
    void cleanup();
    // fills discoveredAssets with the subfolders of assetsPathInput holding a .gltf/.glb/.obj
    void scanFolder();
};