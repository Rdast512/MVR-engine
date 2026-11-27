#include "vk_engine.hpp"
#include "../Constants.h"
#include "../static_headers/logger.hpp"
#include "../util/debug.hpp"
#include "../util/vk_tracy.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    // ENGINE_INDIRECT_MODE=cycle: the scripted mode switch of spec/m4/g7_indirect_modes.md
    constexpr uint32_t kEngineIndirectCycleFrames = 64;

    // ENGINE_WINDOW_SIZE=WxH (e.g. 2560x1369 for C-G3-6 stress runs)
    [[nodiscard]] std::pair<int, int> requestedWindowSize()
    {
        const char* text = std::getenv("ENGINE_WINDOW_SIZE");
        if (text == nullptr || *text == '\0') {
            return {static_cast<int>(WIDTH), static_cast<int>(HEIGHT)};
        }
        const std::string_view view(text);
        const size_t sep = view.find_first_of("xX");
        uint32_t w = 0;
        uint32_t h = 0;
        if (sep != std::string_view::npos) {
            const auto rw = std::from_chars(view.data(), view.data() + sep, w);
            const auto rh = std::from_chars(view.data() + sep + 1, view.data() + view.size(), h);
            if (rw.ec == std::errc{} && rw.ptr == view.data() + sep && rh.ec == std::errc{} &&
                rh.ptr == view.data() + view.size() && w >= 64 && h >= 64) {
                return {static_cast<int>(w), static_cast<int>(h)};
            }
        }
        logError(std::format("ENGINE_WINDOW_SIZE='{}' invalid (expected WxH); using {}x{}", view, WIDTH, HEIGHT),
                 "Engine");
        return {static_cast<int>(WIDTH), static_cast<int>(HEIGHT)};
    }

    // ENGINE_FULLSCREEN=1: fullscreen at desktop resolution, the C-G3-6 full-screen stress case; overrides
    // ENGINE_WINDOW_SIZE
    [[nodiscard]] bool isFullscreenRequested()
    {
        const char* text = std::getenv("ENGINE_FULLSCREEN");
        return text != nullptr && *text != '\0' && std::string_view(text) != "0";
    }
} // namespace

Engine::~Engine() { cleanup(); }

void Engine::initialize()
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error("Failed to initialize SDL: " + std::string(SDL_GetError()));
    }

    const auto [windowWidth, windowHeight] = requestedWindowSize();
    const SDL_WindowFlags fullscreenFlag = isFullscreenRequested() ? SDL_WINDOW_FULLSCREEN : 0;
    window = SDL_CreateWindow("Vulkan", windowWidth, windowHeight,
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | fullscreenFlag);

    if (window == nullptr) {
        throw std::runtime_error("Failed to create window");
    }

    SDL_SetWindowRelativeMouseMode(window, true);

#if !ENGINE_ENABLE_IMGUI
    logInfo("ImGui fully disabled (ENGINE_ENABLE_IMGUI=0) — no context, backends, or GPU resources", "Engine");
#endif

    // NRC is required (engine spec §11.4): no device without its DLL and extensions
    nrcLibrary = std::make_unique<NrcLibrary>();
    const char* basePath = SDL_GetBasePath();
    if (!nrcLibrary->load(basePath != nullptr ? std::filesystem::path(basePath) : std::filesystem::current_path())) {
        throw std::runtime_error("NRC_Vulkan.dll failed to load");
    }
    device = std::make_unique<Device>(window, false,
                                      mergeSdkExtensions(Dlss::requiredExtensions(), nrcLibrary->requiredExtensions()));
    device->init();

    allocator = std::make_unique<VkAllocator>(*device);

    descriptorManager = std::make_unique<DescriptorManager>(device->vkdevice, allocator->allocator,
                                                            device->queueFamilyIndices, device->capabilities);
    descriptorManager->init();

#if ENGINE_ENABLE_CHECKS
    checks = CheckHarness::fromEnvironment();
    const bool isAutomated = checks && checks->isAutomated();
#else
    logInfo("Checks not built (ENGINE_BUILD_CHECKS=OFF): runner variables and ENGINE_CONFIG are ignored", "Engine");
    constexpr bool isAutomated = false;
#endif

    swapChain = std::make_unique<SwapChain>(window, *device);
    swapChain->isUncapped = isAutomated;
    swapChain->init();

    camera = std::make_unique<Camera>(*swapChain);
#if ENGINE_ENABLE_CHECKS
    camera->isJitterEnabled = Camera::jitterRequest().value_or(!checks || checks->isJitterEnabledByDefault());
#else
    camera->isJitterEnabled = Camera::jitterRequest().value_or(true);
#endif
    camera->isJitterForced = Camera::jitterRequest().value_or(false);
    textureManager = std::make_unique<TextureManager>(*device, *allocator, *descriptorManager);
    textureManager->init();

    scene = std::make_unique<Scene>();
    assetsLoader = std::make_unique<AssetsLoader>(scene->objectStorage, *textureManager, scene->geometryStore,
                                                  scene->materialStore, scene->lightStore);

    const glm::vec3 initialAssetPos{0.0f, 0.0f, 0.0f};
    // ENGINE_MODEL: any run's model (m8 I13), relative to the repository root like the lab cases
    std::optional<std::filesystem::path> engineModel;
    if (const char* modelOverride = std::getenv("ENGINE_MODEL"); modelOverride != nullptr && *modelOverride != '\0') {
        const std::filesystem::path model(modelOverride);
        engineModel = model.is_absolute() ? model : std::filesystem::path(ENGINE_MODELS_DIR).parent_path() / model;
    }
#if ENGINE_ENABLE_CHECKS
    // a check scene (ENGINE_CHECK_SCENE) or the bench may choose it
    const std::string startupModel = checks ? checks->chooseStartupModel(engineModel, MODEL_PATH).string()
                                            : engineModel.value_or(MODEL_PATH).string();
#else
    const std::string startupModel = engineModel.value_or(MODEL_PATH).string();
#endif
    if (!assetsLoader->loadModel(startupModel, initialAssetPos)) {
        logError(std::format("Startup model failed to load: {}", startupModel), "Engine");
    }
    // scene is visible immediately
    camera->focusOn(initialAssetPos);
    frameSync = std::make_unique<FrameSync>(*device);
    // flushes the startup model
    assetBuffers = std::make_unique<AssetBuffers>(*device, *allocator, *frameSync, scene->geometryStore,
                                                  scene->materialStore, scene->lightStore, scene->objectStorage);
    frameData = std::make_unique<FrameData>(*device, *allocator, *assetBuffers, scene->geometryStore,
                                            scene->objectStorage);
    camera->createBuffers(*device, *allocator);

    tracyContext = std::make_unique<VkTracyContext>();
    {
        const vk::CommandBufferAllocateInfo tracySetupCommandBufferAllocateInfo{
            .commandPool = *frameSync->commandPool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        };
        vk::raii::CommandBuffers tracySetupCommandBuffers(device->vkdevice, tracySetupCommandBufferAllocateInfo);
        tracyContext->init(device->instance, device->physicalDevice, device->vkdevice, device->graphicsQueue,
                           tracySetupCommandBuffers.front(), "Graphics Queue");
    }

    shaderObjects = std::make_unique<ShaderObjects>(device->vkdevice);
    shaderObjects->init();

    renderer = std::make_unique<Renderer>(*device, *swapChain, *allocator, *frameSync, *scene, *assetBuffers,
                                          *frameData, *descriptorManager, *shaderObjects,
                                          *camera, *nrcLibrary, tracyContext.get());
    if (RtxdiContext::isReferenceRequested()) {
        renderer->getRtxdi().directMode = DirectMode::RestirDi;
        renderer->getRtxdi().directResampling = DirectResamplingMode::Reference;
    }
    // ENGINE_INDIRECT_MODE: a kLegacyIndirectModes id pins (bounce, cache), cycle switches it every 64 frames
    if (const char* text = std::getenv("ENGINE_INDIRECT_MODE")) {
        const std::string_view request(text);
        isIndirectModeCycling = request == kIndirectModeCycle;
        const auto mode = std::ranges::find_if(
            kLegacyIndirectModes, [request](const LegacyIndirectModeInfo& info) { return request == info.id; });
        if (mode != kLegacyIndirectModes.end()) {
            renderer->indirectBounce = mode->stages.bounce;
            renderer->radianceCache = mode->stages.cache;
        } else if (!isIndirectModeCycling) {
            logError(std::format("ENGINE_INDIRECT_MODE='{}' is not a mode; ignored", request), "Engine");
        }
    }
#if ENGINE_ENABLE_CHECKS
    // before the swapchain resources: ENGINE_CONFIG may change what they size
    std::optional<CheckRequest> startupRequest;
    if (checks) {
        startupRequest = checks->attach(CheckHostContext{
            .device = *device,
            .allocator = *allocator,
            .shaderObjects = *shaderObjects,
            .scene = *scene,
            .assets = *assetBuffers,
            .renderer = *renderer,
            .camera = *camera,
        });
    }
#endif
    renderer->rebuildSwapchainResources();

#if ENGINE_ENABLE_IMGUI
    ui = std::make_unique<EngineUi>(UiContext{
        .window = window,
        .device = *device,
        .swapChain = *swapChain,
        .scene = *scene,
        .assets = *assetBuffers,
        .textures = *textureManager,
        .renderer = *renderer,
        .camera = *camera,
    });
#endif

#if ENGINE_ENABLE_CHECKS
    if (startupRequest) {
        applyCheckRequest(*startupRequest);
    }
#endif

    initialized = true;
}

bool Engine::run()
{
    if (!initialized) {
        initialize();
    }

    bool quit = false;
    bool minimized = false;
    // OS keyboard focus, not ImGui game focus; soft-caps FPS when another window is on top
    bool windowFocused = true;
    lastTime = std::chrono::high_resolution_clock::now();
    fpsTime = lastTime;
    // focused frames rely on swapchain vsync (FIFO)
    constexpr double kUnfocusedTargetMs = 1000.0 / 30.0;
    constexpr float kCameraSpeed = 5.0f;
    auto& deviceRef = device->vkdevice;
#if ENGINE_ENABLE_CHECKS
    // automated runs own the camera, the scene and the frame pace
    const bool isAutomated = checks && checks->isAutomated();
#else
    constexpr bool isAutomated = false;
#endif
    const bool isCameraInputEnabled = !isAutomated;

    // game focus captures the mouse and hides the cursor; UI focus (I key) frees both
    const auto setGameFocus = [this](bool gameFocused)
    {
        SDL_SetWindowRelativeMouseMode(window, gameFocused);
        if (gameFocused) {
            SDL_HideCursor();
        } else {
            SDL_ShowCursor();
        }
    };
    isUiOpen = false;
#if ENGINE_ENABLE_IMGUI
    ui->setInputEnabled(false);
#endif
    setGameFocus(true);

    while (!quit) {
        ZoneScopedN("Frame");

        const auto currentTime = std::chrono::high_resolution_clock::now();
        frameCount++;

        // averaged over one second, shown by the UI stats overlay
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - fpsTime);
        if (duration.count() >= 1000) {
            fps = static_cast<float>(frameCount) * 1000.0f / static_cast<float>(duration.count());
            frameMs = static_cast<float>(duration.count()) / static_cast<float>(frameCount);
            frameCount = 0;
            fpsTime = currentTime;
        }

        {
            ZoneScopedN("EventPoll");
            SDL_Event e{};
            while (SDL_PollEvent(&e) != 0) {
#if ENGINE_ENABLE_IMGUI
                // feed the UI only while open so it cannot steal game input
                if (isUiOpen) {
                    ui->processEvent(e);
                }
#endif

                if (e.type == SDL_EVENT_QUIT) {
                    quit = true;
                }
#if ENGINE_ENABLE_IMGUI
                else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_I && !e.key.repeat) {
                    // typing 'i' in a UI field goes to the widget
                    if (!(isUiOpen && ui->isTextInputActive())) {
                        isUiOpen = !isUiOpen;
                        ui->setInputEnabled(isUiOpen);
                        setGameFocus(!isUiOpen);
                    }
                }
#endif
                else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !isUiOpen && e.button.button == SDL_BUTTON_LEFT) {
                    // SDL may drop relative mode on focus loss until a click
                    setGameFocus(true);
                } else if (e.type == SDL_EVENT_MOUSE_MOTION && !isUiOpen && isCameraInputEnabled) {
                    camera->rotate(-e.motion.xrel, e.motion.yrel);
                } else if (e.type == SDL_EVENT_MOUSE_WHEEL && !isUiOpen && isCameraInputEnabled) {
                    camera->addFov(-e.wheel.y * 2.0f); // scroll up = zoom in (narrower FOV)
                } else if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
                    windowFocused = true;
                    setGameFocus(!isUiOpen);
                } else if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                    windowFocused = false;
                } else if (e.type == SDL_EVENT_WINDOW_RESIZED) {
                    // a drag sends many; the swapchain is recreated once, below
                    if (renderer) {
                        renderer->requestSwapchainRecreate();
                    }
                } else if (e.type == SDL_EVENT_WINDOW_MINIMIZED) {
                    minimized = true;
                } else if (e.type == SDL_EVENT_WINDOW_RESTORED) {
                    minimized = false;
                }
            }
        }

        if (!minimized && renderer && renderer->isSwapchainRecreateRequested()) {
            recreateSwapchain();
        }

#if ENGINE_ENABLE_IMGUI
        ui->buildFrame(FrameStats{.fps = fps, .frameMs = frameMs});
        applyUiRequests(ui->takeRequests());
#endif

        if (!minimized && !quit) {
            // WASD moves, LShift up, LCtrl down
            if (!isUiOpen && isCameraInputEnabled) {
                ZoneScopedN("CameraInput");
                const float dt = std::chrono::duration<float>(currentTime - lastTime).count();
                const float step = kCameraSpeed * dt;

                int keyCount = 0;
                const bool* keys = SDL_GetKeyboardState(&keyCount);

                if (keys[SDL_SCANCODE_W]) {
                    camera->moveForward(step);
                }
                if (keys[SDL_SCANCODE_S]) {
                    camera->moveForward(-step);
                }
                if (keys[SDL_SCANCODE_A]) {
                    camera->moveRight(-step);
                }
                if (keys[SDL_SCANCODE_D]) {
                    camera->moveRight(step);
                }
                if (keys[SDL_SCANCODE_LSHIFT]) {
                    camera->moveUp(step);
                }
                if (keys[SDL_SCANCODE_LCTRL]) {
                    camera->moveUp(-step);
                }
            }

#if ENGINE_ENABLE_CHECKS
            if (checks) {
                applyCheckRequest(checks->beginFrame());
            }
#endif
            if (isIndirectModeCycling) {
                const auto& mode =
                    kLegacyIndirectModes[renderer->frameNumber / kEngineIndirectCycleFrames % kLegacyIndirectModes.size()];
                renderer->indirectBounce = mode.stages.bounce;
                renderer->radianceCache = mode.stages.cache;
            }
            if (renderer->denoiser == DenoiserMode::Off && renderer->aaMode == AaMode::Accumulation) {
                // any lab switch edit starts a new mean, as the RTXDI sample's resetAccumulation on every UI edit
                LabConfig config = captureLabConfig(*renderer, *camera);
                if (accumulationConfig && !isSameConfig(*accumulationConfig, config)) {
                    renderer->resetAccumulation();
                }
                accumulationConfig = std::move(config);
            } else {
                accumulationConfig.reset();
            }
            {
                ZoneScopedN("DrawFrame");
                renderer->drawFrame();
            }
#if ENGINE_ENABLE_CHECKS
            if (checks) {
                quit = checks->endFrame();
            }
#endif
        } else {
            ZoneScopedN("MinimizedWait");
            SDL_Delay(100);
        }

        // unfocused windows often lose vsync pacing and burn CPU/GPU; automated runners stay uncapped
        if (!windowFocused && !minimized && !quit && !isAutomated) {
            ZoneScopedN("UnfocusedFramePacing");
            const auto frameEndTime = std::chrono::high_resolution_clock::now();
            const double elapsedMs = std::chrono::duration<double, std::milli>(frameEndTime - currentTime).count();
            if (elapsedMs < kUnfocusedTargetMs) {
                SDL_Delay(static_cast<Uint32>(kUnfocusedTargetMs - elapsedMs));
            }
        }

        lastTime = currentTime;
        FrameMark;
    }
    deviceRef.waitIdle();
#if ENGINE_ENABLE_CHECKS
    return checks ? checks->finish() : true;
#else
    return true;
#endif
}

#if ENGINE_ENABLE_IMGUI
void Engine::applyUiRequests(const UiRequests& requests)
{
    // UI boundary: a failed scene action is reported, never thrown out of the main loop
    const auto run = [this](std::string_view action, const auto& sceneAction) {
        try {
            const std::optional<SceneActionStatus> status = sceneAction();
            if (status) {
                ui->setLoadStatus(status->text, status->isError);
            }
        } catch (const std::exception& error) {
            logError(std::format("{} failed: {}", action, error.what()), "Engine");
            ui->setLoadStatus(std::format("{} failed: {}", action, error.what()), true);
            // the action may be half applied: rebuild the frame data from the scene as it now is
            frameData->syncScene();
        }
    };
    if (requests.unload) {
        run("Unload", [&] { return unloadModel(*requests.unload); });
    }
    if (const std::optional<UiRequests::Load>& load = requests.load) {
        run("Load", [&] { return std::optional(loadModel(load->asset, load->position)); });
    }
    if (requests.shouldClearInstanceField) {
        run("Clear instance field", [&] { return clearInstanceField(); });
    }
    if (const std::optional<UiRequests::InstanceField>& field = requests.spawnInstanceField) {
        run("Spawn instance field",
            [&] { return std::optional(spawnInstanceField(field->center, field->side, field->spacing)); });
    }
    if (const std::optional<OmmBakeSettings>& settings = requests.rebakeOmms) {
        run("OMM re-bake", [&] {
            const SceneActionStatus status = rebakeOmms(*settings);
            if (!status.isError) {
                ui->setAppliedOmmSettings(*settings);
            }
            return std::optional(status);
        });
    }
}
#endif

void Engine::replaceModels(const std::filesystem::path& model, std::string_view subsystem)
{
    // load before unloading the previous entities: every swap drops the older models while the new one stays
    // resident (PBR plan C1)
    const EntityId previousCount = scene->objectStorage.size();
    const SceneActionStatus status = loadModel(model, glm::vec3(0.0f));
    if (status.isError) {
        logError(status.text, subsystem);
    }
    for (EntityId erased = 0; erased < previousCount; ++erased) {
        unloadModel(0);
    }
}

#if ENGINE_ENABLE_CHECKS
void Engine::applyCheckRequest(const CheckRequest& request)
{
    if (request.load) {
        const SceneActionStatus status = loadModel(request.load->model, request.load->position);
        if (status.isError) {
            logError(status.text, request.logSubsystem);
        }
    }
    if (request.replaceModels) {
        replaceModels(*request.replaceModels, request.logSubsystem);
    }
    if (request.windowSize) {
        // arrives as SDL_EVENT_WINDOW_RESIZED in a later poll
        SDL_SetWindowSize(window, request.windowSize->x, request.windowSize->y);
    }
    if (const std::optional<CheckRequest::InstanceField>& field = request.instanceField) {
        const EntityId firstEntity = scene->objectStorage.size();
        const SceneActionStatus status = spawnInstanceField(field->center, field->side, field->spacing);
        if (status.isError) {
            throw std::runtime_error(status.text);
        }
        checks->onInstanceFieldSpawned(firstEntity, scene->objectStorage.size() - firstEntity, status.text);
    }
}
#endif

void Engine::recreateSwapchain()
{
    if (!swapChain || !renderer) {
        return;
    }

    renderer->recreateSwapchain();

#if ENGINE_ENABLE_IMGUI
    ui->onSwapchainRecreated();
#endif
}

Engine::SceneActionStatus Engine::loadModel(const std::filesystem::path& assetPath, glm::vec3 position)
{
    ZoneScopedN("Engine::loadModel");
    const std::string name = assetPath.filename().string();
    logInfo(std::format("Load model started: {}", assetPath.string()), "Engine");
#ifdef TRACY_ENABLE
    {
        const std::string msg = std::format("LoadModel {}", assetPath.string());
        TracyMessage(msg.c_str(), msg.size());
    }
#endif
    const auto start = std::chrono::steady_clock::now();
    // same file already on the GPU: a new entity shares its geometry, materials and textures
    if (const std::optional<std::filesystem::path> modelFile = AssetsLoader::resolveModelPath(assetPath)) {
        if (const std::optional<ModelId> model = assetBuffers->findModel(modelFile->string())) {
            device->vkdevice.waitIdle();
            assetBuffers->instantiateModel(*model, Transform{.position = position});
            frameData->syncScene();
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            return {.text = std::format("Instanced {} in {:.3f} s (shared)", name, seconds)};
        }
    }
    device->vkdevice.waitIdle();
    std::optional<EntityId> id;
    try {
        id = assetsLoader->loadModel(assetPath.string(), position);
    } catch (const std::exception& error) {
        // UI boundary: a broken model must not take the engine down
        logError(std::format("Load model {} failed: {}", name, error.what()), "Engine");
    }
    // uploads the new model, or discards what a failed load left in the scratch
    assetBuffers->flush();
    if (!id) {
        return {.text = std::format("Failed to load {}", name), .isError = true};
    }
    frameData->syncScene();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return {.text = std::format("Loaded {} in {:.2f} s", name, seconds)};
}

Engine::SceneActionStatus Engine::spawnInstanceField(glm::vec3 center, int side, float spacing)
{
    ZoneScopedN("Engine::spawnInstanceField");
    const auto start = std::chrono::steady_clock::now();
    const std::optional<std::filesystem::path> modelFile = AssetsLoader::resolveModelPath(INSTANCE_FIELD_MODEL_PATH);
    if (!modelFile) {
        return {.text = std::format("No instance field model at {}", INSTANCE_FIELD_MODEL_PATH.string()),
                .isError = true};
    }
    // frames in flight still read the node-instance buffers the layout rebuild replaces
    device->vkdevice.waitIdle();
    std::optional<ModelId> model = assetBuffers->findModel(modelFile->string());
    std::optional<EntityId> loaded;
    if (!model) {
        try {
            loaded = assetsLoader->loadModel(modelFile->string(), center);
        } catch (const std::exception& error) {
            logError(std::format("Instance field load failed: {}", error.what()), "Engine");
        }
        assetBuffers->flush();
        if (!loaded) {
            return {.text = std::format("Failed to load {}", modelFile->filename().string()), .isError = true};
        }
        model = scene->objectStorage.modelIds[*loaded];
    }

    const auto cells = static_cast<uint32_t>(side);
    const float cellSize = spacing * 2.0f * assetBuffers->models()[*model].localBounds.w;
    const float half = 0.5f * static_cast<float>(cells - 1);
    std::vector<Transform> transforms;
    transforms.reserve(cells * cells);
    for (uint32_t z = 0; z < cells; ++z) {
        for (uint32_t x = 0; x < cells; ++x) {
            // golden-ratio yaw sequence: evenly spread facings for the cone test
            const float turns = std::fmod(static_cast<float>(transforms.size()) * std::numbers::phi_v<float>, 1.0f);
            const glm::vec3 offset{(static_cast<float>(x) - half) * cellSize, 0.0f,
                                   (static_cast<float>(z) - half) * cellSize};
            transforms.push_back(Transform{.position = center + offset,
                                           .rotation = {0.0f, turns * 2.0f * std::numbers::pi_v<float>, 0.0f}});
        }
    }
    // a fresh load already created one entity: it takes the first cell
    std::span<const Transform> remaining = transforms;
    if (loaded) {
        scene->objectStorage.transforms[*loaded] = transforms.front();
        // placement, not motion
        scene->objectStorage.prevModelMatrices[*loaded] = computeModelMatrix(transforms.front());
        remaining = remaining.subspan(1);
    }
    if (!remaining.empty()) {
        assetBuffers->instantiateModel(*model, remaining);
    }
    frameData->syncScene();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return {.text = std::format("Spawned {} x {} in {:.3f} s", transforms.size(), modelFile->filename().string(),
                                seconds)};
}

std::optional<Engine::SceneActionStatus> Engine::clearInstanceField()
{
    ZoneScopedN("Engine::clearInstanceField");
    const std::optional<std::filesystem::path> modelFile = AssetsLoader::resolveModelPath(INSTANCE_FIELD_MODEL_PATH);
    const std::optional<ModelId> model = modelFile ? assetBuffers->findModel(modelFile->string()) : std::nullopt;
    if (!model) {
        return std::nullopt;
    }
    device->vkdevice.waitIdle();
    ObjectStorage& storage = scene->objectStorage;
    // the last erase drops the model and shifts later model ids onto it, so stop by count
    const uint32_t count = assetBuffers->models()[*model].refCount;
    uint32_t erased = 0;
    // from the back: an erase shifts only the ids after it
    for (EntityId id = storage.size(); id-- > 0 && erased < count;) {
        if (storage.modelIds[id] == *model) {
            textureManager->release(assetBuffers->eraseEntity(id));
            ++erased;
        }
    }
    frameData->syncScene();
    return SceneActionStatus{.text = std::format("Cleared {} instances", erased)};
}

Engine::SceneActionStatus Engine::rebakeOmms(const OmmBakeSettings& settings)
{
    ZoneScopedN("Engine::rebakeOmms");
    const auto start = std::chrono::steady_clock::now();
    assetsLoader->ommSettings = settings;
    ObjectStorage& storage = scene->objectStorage;
    // model files in first-entity order, each with its entities' transforms
    std::vector<std::pair<std::string, std::vector<Transform>>> placements;
    for (EntityId id = 0; id < storage.size(); ++id) {
        if (storage.modelIds[id] == kInvalidModelId) {
            continue;
        }
        const std::string& key = assetBuffers->models()[storage.modelIds[id]].key;
        auto it = std::ranges::find(placements, key, &std::pair<std::string, std::vector<Transform>>::first);
        if (it == placements.end()) {
            it = placements.insert(placements.end(), {key, {}});
        }
        it->second.push_back(storage.transforms[id]);
    }
    if (placements.empty()) {
        return {.text = "OMM settings apply to the next load"};
    }

    // a shared model keeps its old bake (findModel), so every entity goes first
    device->vkdevice.waitIdle();
    for (EntityId id = storage.size(); id-- > 0;) {
        textureManager->release(assetBuffers->eraseEntity(id));
    }
    uint32_t failedCount = 0;
    for (const auto& [key, transforms] : placements) {
        std::optional<EntityId> loaded;
        try {
            loaded = assetsLoader->loadModel(key, transforms.front().position);
        } catch (const std::exception& error) {
            logError(std::format("OMM re-bake load {} failed: {}", key, error.what()), "Engine");
        }
        assetBuffers->flush();
        if (!loaded) {
            ++failedCount;
            continue;
        }
        storage.transforms[*loaded] = transforms.front();
        // placement, not motion
        storage.prevModelMatrices[*loaded] = computeModelMatrix(transforms.front());
        if (transforms.size() > 1) {
            assetBuffers->instantiateModel(storage.modelIds[*loaded], std::span(transforms).subspan(1));
        }
    }
    frameData->syncScene();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (failedCount > 0) {
        return {.text = std::format("OMM re-bake: {} of {} models failed to reload", failedCount, placements.size()),
                .isError = true};
    }
    return {.text = std::format("Re-baked OMMs of {} models in {:.2f} s", placements.size(), seconds)};
}

std::optional<Engine::SceneActionStatus> Engine::unloadModel(EntityId id)
{
    ZoneScopedN("Engine::unloadModel");
    ObjectStorage& storage = scene->objectStorage;
    if (id >= storage.size()) {
        return std::nullopt;
    }
    const std::string name = std::filesystem::path(storage.names[id]).filename().string();
#ifdef TRACY_ENABLE
    {
        const std::string msg = std::format("UnloadModel {}", name);
        TracyMessage(msg.c_str(), msg.size());
    }
#endif
    // frames in flight still read the buffers, textures and heap slots being freed
    device->vkdevice.waitIdle();
    // textures come back only when this was the model's last entity
    textureManager->release(assetBuffers->eraseEntity(id));
    frameData->syncScene();
#if ENGINE_USE_MIMALLOC
    mi_collect(true);
#endif
    logInfo(std::format("Unloaded model {}: {} textures left", name, textureManager->size()), "Engine");
    return SceneActionStatus{.text = std::format("Unloaded {}", name)};
}

void Engine::cleanup()
{
    // also runs after a failed initialize(): every step tears down only what exists
    if (!device && window == nullptr && SDL_WasInit(0) == 0) {
        return;
    }
    if (device && *device->vkdevice) {
        device->vkdevice.waitIdle();
    }

    if (tracyContext) {
        tracyContext->shutdown();
        tracyContext.reset();
    }

#if ENGINE_ENABLE_IMGUI
    // after waitIdle, before the renderer whose present pass draws it
    ui.reset();
#endif
#if ENGINE_ENABLE_CHECKS
    // unregisters its frame hook and frees the check buffers, before the renderer and the allocator
    checks.reset();
#endif

    if (scene) {
        scene->objectStorage.clear();
    }
    logInfo("Object storage cleared", "Engine");

    renderer.reset();
    shaderObjects.reset();
    descriptorManager.reset();
    textureManager.reset();
    frameData.reset();
    assetBuffers.reset(); // before assetsLoader: holds refs to its vertex/index vectors
    // after everything that recorded into its command buffers
    frameSync.reset();
    assetsLoader.reset();
    scene.reset();
    camera.reset();
    logInfo("Resources cleaned up", "Engine");
    swapChain.reset();
    logInfo("Swap chain cleaned up", "Engine");
    allocator.reset();
    logInfo("Allocator cleaned up", "Engine");
    // owns the surface, which must go before its window
    device.reset();
    logInfo("Device cleaned up", "Engine");
    nrcLibrary.reset();
    if (window != nullptr) {
        SDL_DestroyWindow(window);
        logInfo("Window destroyed", "Engine");
        window = nullptr;
    }
    SDL_Quit();
    initialized = false;
    logInfo("Engine cleaned up", "Engine");
}
