#pragma once
#include "../Constants.h"
#include "assets_loader.hpp"
#include "texture_manager.hpp"
#include "render/settings_registry.hpp"
#include "../render/nrc_library.hpp"
#include "../render/vk_renderer.hpp"
#include "../render/vk_shader_objects.hpp"
#include "../util/vk_tracy.hpp"
#include "vk_allocator.hpp"
#include "vk_descriptors.hpp"
#include "vk_device.hpp"
#include "vk_asset_buffers.hpp"
#include "vk_frame_data.hpp"
#include "vk_frame_sync.hpp"
#include "vk_swapchain.hpp"
#include "scene/vk_camera.hpp"
#include "scene/vk_scene.hpp"
#if ENGINE_ENABLE_IMGUI
    #include "ui/engine_ui.hpp"
#endif
#if ENGINE_ENABLE_CHECKS
    #include "checks/check_harness.hpp"
#endif

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

class Engine
{
    // status line of a scene action, shown by the UI
    struct SceneActionStatus
    {
        std::string text;
        bool isError = false;
    };

    // toggled by the I key; hidden and game-focused until opened. owns mouse capture and camera input
    bool isUiOpen = false;

    SDL_Window* window = nullptr;
    // before device: its extension lists feed device creation, and the NRC context outlives nothing it loads
    std::unique_ptr<NrcLibrary> nrcLibrary;
    std::unique_ptr<Device> device;
    std::unique_ptr<VkAllocator> allocator;
    std::unique_ptr<SwapChain> swapChain;
    std::unique_ptr<Scene> scene;
    std::unique_ptr<AssetsLoader> assetsLoader;
    std::unique_ptr<FrameSync> frameSync;
    std::unique_ptr<AssetBuffers> assetBuffers;
    std::unique_ptr<FrameData> frameData;
    std::unique_ptr<VkTracyContext> tracyContext;
    std::unique_ptr<TextureManager> textureManager;
    std::unique_ptr<DescriptorManager> descriptorManager;
    std::unique_ptr<Camera> camera;
    std::unique_ptr<ShaderObjects> shaderObjects;
    std::unique_ptr<Renderer> renderer;
#if ENGINE_ENABLE_IMGUI
    // after renderer: reads it through UiContext, destroyed before it
    std::unique_ptr<EngineUi> ui;
#endif
#if ENGINE_ENABLE_CHECKS
    // set from the environment: the runners, bench, lab and ENGINE_CONFIG (engine_checks.dll). After renderer: its
    // frame hook is registered with it, destroyed before it
    std::unique_ptr<CheckHarness> checks;
#endif
    // ENGINE_INDIRECT_MODE=cycle: the indirect mode follows the frame number
    bool isIndirectModeCycling = false;
    // the lighting settings of the last frame in the Accumulation AA mode: an edit starts a new mean
    std::optional<LabConfig> accumulationConfig;
    bool initialized = false;
    std::chrono::high_resolution_clock::time_point lastTime;
    std::chrono::high_resolution_clock::time_point fpsTime;
    int frameCount = 0;
    float fps = 0.0f;
    float frameMs = 0.0f;

#if ENGINE_ENABLE_IMGUI
    // after the ImGui frame, so no panel iterates storage that changes under it
    void applyUiRequests(const UiRequests& requests);
#endif
    // a model already on the GPU gets a new entity sharing it
    SceneActionStatus loadModel(const std::filesystem::path& assetPath, glm::vec3 position);
    // nullopt: no such entity
    std::optional<SceneActionStatus> unloadModel(EntityId id);
    // side x side grid, spacing in model bounding diameters; loads the field model once, then instantiates the rest
    // of the grid in one batch
    SceneActionStatus spawnInstanceField(glm::vec3 center, int side, float spacing);
    // erases every entity of the field model; nullopt: the model is not loaded
    std::optional<SceneActionStatus> clearInstanceField();
    // reloads every model with new OMM bake settings, keeping entity transforms; one load per model file
    SceneActionStatus rebakeOmms(const OmmBakeSettings& settings);
    // loads model at the origin, then unloads every entity that was there before it
    void replaceModels(const std::filesystem::path& model, std::string_view subsystem);
#if ENGINE_ENABLE_CHECKS
    // the scene and window actions the runners ask for; a failed startup instance field throws
    void applyCheckRequest(const CheckRequest& request);
#endif
    // the one recreate path, at most once per frame before it is built: swapchain, swapchain-sized resources and the
    // UI image count
    void recreateSwapchain();

public:
    ~Engine();
    void initialize();
    // false when any active self-test runner failed
    [[nodiscard]] bool run();
    void cleanup();
};