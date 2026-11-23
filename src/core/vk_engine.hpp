#pragma once
#include "../Constants.h"
#include "assets_loader.hpp"
#include "engine_bench.hpp"
#include "engine_cull_check.hpp"
#include "engine_dlss_check.hpp"
#include "engine_gi_check.hpp"
#include "engine_ray_check.hpp"
#include "engine_guide_capture.hpp"
#include "engine_motion_test.hpp"
#include "engine_temporal_check.hpp"
#include "texture_manager.hpp"
#include "lab/engine_lab.hpp"
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
    // set from the environment: fixed camera, no input, uncapped present, quits after logging GPU timings
    std::optional<EngineBench> bench;
    // set from the environment: scripted motion-vector checks, no camera input, quits after logging the results
    std::optional<EngineMotionTest> motionTest;
    // set from the environment: guide PNGs of glTF samples, no camera input, quits after the last one
    std::optional<EngineGuideCapture> guideCapture;
    // set from the environment: culling image check on the startup scene, quits after logging the results
    std::optional<EngineCullCheck> cullCheck;
    std::optional<EngineGiCheck> giCheck;
    // set from the environment: DLSS-RR clean-input and perturbation check (ENGINE_DLSS_CHECK=1, m8 I14 F25)
    std::optional<EngineDlssCheck> dlssCheck;
    // set from the environment: one M8 I1 temporal runner (stability, light change, screen edge), quits after it
    std::optional<EngineTemporalCheck> temporalCheck;
    // set from the environment: primary rays against the vis buffer every frame, alongside another runner or alone
    std::optional<EngineRayCheck> rayCheck;
    // ENGINE_LAB=<plan>: the settings lab, alone in its process (spec/features/planned/full_testing.md §8), quits after the plan
    std::optional<EngineLab> lab;
    // ENGINE_INDIRECT_MODE=cycle: the indirect mode follows the frame number
    bool isIndirectModeCycling = false;
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
    // loads and resizes the motion test asks for before its frame
    void applyMotionTestRequest(const EngineMotionTest::Request& request);
    // loads model at the origin, then unloads every entity that was there before it
    void replaceModels(const std::filesystem::path& model, std::string_view subsystem);
    // the model swap and capture the guide capture asks for before its frame
    void applyGuideCaptureRequest(const EngineGuideCapture::Request& request);
    // the lab's model swap before its frame
    void applyLabRequest(const EngineLab::Request& request);
    // ENGINE_CONFIG for the interactive engine and bench runs; the checks and the lab log that they ignore it
    void applyEngineConfig();
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