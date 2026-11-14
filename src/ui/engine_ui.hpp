#pragma once
#include "../Constants.h"

#if ENGINE_ENABLE_IMGUI
    #include "core/object_storage.hpp"

    #include <SDL3/SDL.h>
    #include <glm/glm.hpp>
    #include <vulkan/vulkan_raii.hpp>

    #include <array>
    #include <filesystem>
    #include <optional>
    #include <string>
    #include <string_view>
    #include <vector>

class AssetBuffers;
class Camera;
class Device;
class Renderer;
class Scene;
class SwapChain;
class TextureManager;

// what the panels read; owned by Engine, outlives EngineUi
struct UiContext
{
    SDL_Window* window;
    const Device& device;
    const SwapChain& swapChain;
    Scene& scene;
    const AssetBuffers& assets;
    const TextureManager& textures;
    // debug view, culling settings and stats, GPU timer
    Renderer& renderer;
    // teleport
    Camera& camera;
};

// scene changes asked for by panels; Engine runs them after the ImGui frame
struct UiRequests
{
    struct Load
    {
        std::filesystem::path asset;
        glm::vec3 position;
    };
    struct InstanceField
    {
        glm::vec3 center;
        int side;
        float spacing;
    };

    std::optional<Load> load;
    std::optional<EntityId> unload;
    std::optional<InstanceField> spawnInstanceField;
    bool shouldClearInstanceField = false;
};

struct FrameStats
{
    float fps = 0.0f;
    float frameMs = 0.0f;
};

// ImGui context, backends and every debug panel
class EngineUi
{
public:
    // after Renderer: backends need the device, swapchain format and image count
    explicit EngineUi(const UiContext& context);
    // after waitIdle: the backends destroy GPU objects
    ~EngineUi();

    EngineUi(const EngineUi&) = delete;
    EngineUi& operator=(const EngineUi&) = delete;
    EngineUi(EngineUi&&) = delete;
    EngineUi& operator=(EngineUi&&) = delete;

    void processEvent(const SDL_Event& event);
    // panels are open exactly while they take input; closed, ImGui stops reading the mouse and changing the cursor
    void setInputEnabled(bool enabled);
    // typing in a text field; keeps hotkeys out of the field
    [[nodiscard]] bool isTextInputActive() const;
    // records ImGui draw data for the present pass; never changes scene storage
    void buildFrame(const FrameStats& stats);
    [[nodiscard]] UiRequests takeRequests();
    void setLoadStatus(std::string_view text, bool isError);
    void onSwapchainRecreated();

private:
    void createDescriptorPool();
    // also unwinds a constructor that threw midway
    void shutdown();
    // fills discoveredAssets with the subfolders of assetsPathInput holding a .gltf/.glb
    void scanFolder();
    void drawStatsOverlay(const FrameStats& stats) const;
    void drawLoadPanel();
    void drawLoadedModelsPanel();
    void drawAssetsPanel() const;
    void drawDebugViewPanel();
    void drawEntityTransformControls();
    void drawCameraPanel();
    void drawCullingPanel();
    void drawSunPanel();
    void drawRayTracingPanel();
    void drawPostPanel();
    void drawInstanceFieldPanel();

    UiContext context;
    UiRequests requests;
    bool isOpen = false;

    vk::raii::DescriptorPool descriptorPool = nullptr;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkPipelineRenderingCreateInfoKHR pipelineRenderingInfo{};

    std::vector<std::filesystem::path> discoveredAssets;
    int selectedAssetIndex = -1;
    bool hasScannedAssets = false;
    std::array<float, 3> loadedModelPosition{};
    std::array<float, 3> teleportPosition{};
    char assetsPathInput[260] = ENGINE_MODELS_DIR;
    std::string loadStatus;
    bool isLoadStatusError = false;
    // entity moved by the motion-vector debug controls
    int transformTestEntity = 0;
    // instance field: side x side grid of INSTANCE_FIELD_MODEL_PATH centered on loadedModelPosition
    int instanceFieldSide = 16;
    // cell size in model bounding diameters
    float instanceFieldSpacing = 1.5f;
};
#endif
