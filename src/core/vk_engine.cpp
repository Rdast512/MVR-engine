#include "vk_engine.hpp"
#include "../Constants.h"
#include "../static_headers/logger.hpp"
#include "../util/debug.hpp"
#include "../util/vk_tracy.hpp"

#include <cmath>
#include <filesystem>
#include <format>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

Engine::~Engine() { cleanup(); }

void Engine::initialize()
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error("Failed to initialize SDL: " + std::string(SDL_GetError()));
    }

    window = SDL_CreateWindow("Vulkan", static_cast<int>(WIDTH), static_cast<int>(HEIGHT),
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);

    if (window == nullptr) {
        throw std::runtime_error("Failed to create window");
    }

    SDL_SetWindowRelativeMouseMode(window, true);

#if !ENGINE_ENABLE_IMGUI
    logInfo("ImGui fully disabled (ENGINE_ENABLE_IMGUI=0) — no context, backends, or GPU resources", "Engine");
#endif

    device = std::make_unique<Device>(window, false);
    device->init();

    allocator = std::make_unique<VkAllocator>(*device);

    descriptorManager = std::make_unique<DescriptorManager>(device->vkdevice, allocator->allocator,
                                                            device->queueFamilyIndices, device->capabilities);
    descriptorManager->init();

    if (const std::optional<EngineBench::Config> benchConfig = EngineBench::readConfig()) {
        bench.emplace(*benchConfig);
    }

    swapChain = std::make_unique<SwapChain>(window, *device);
    swapChain->isUncapped = bench.has_value();
    swapChain->init();

    camera = std::make_unique<Camera>(*swapChain);
    textureManager = std::make_unique<TextureManager>(*device, *allocator, *descriptorManager);
    textureManager->init();

    scene = std::make_unique<Scene>();
    assetsLoader = std::make_unique<AssetsLoader>(scene->objectStorage, *textureManager, scene->geometryStore,
                                                  scene->materialStore, scene->lightStore);

    const glm::vec3 initialAssetPos{0.0f, 0.0f, 0.0f};
    if (!assetsLoader->loadModel(MODEL_PATH.string(), initialAssetPos)) {
        logError(std::format("Startup model failed to load: {}", MODEL_PATH.string()), "Engine");
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
                                          *camera, tracyContext.get());
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

    if (bench && bench->getConfig().fieldSide > 0) {
        constexpr float kBenchFieldSpacing = 1.5f;
        const SceneActionStatus status = spawnInstanceField(initialAssetPos, bench->getConfig().fieldSide,
                                                            kBenchFieldSpacing);
        if (status.isError) {
            throw std::runtime_error(status.text);
        }
        logInfo(status.text, "Bench");
    }

    initialized = true;
}

void Engine::run()
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
    // the bench view stays where initialize put it
    const bool isCameraInputEnabled = !bench;

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
                    if (swapChain && renderer) {
                        recreateSwapchain();
                    }
                } else if (e.type == SDL_EVENT_WINDOW_MINIMIZED) {
                    minimized = true;
                } else if (e.type == SDL_EVENT_WINDOW_RESTORED) {
                    minimized = false;
                }
            }
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

            {
                ZoneScopedN("DrawFrame");
                renderer->drawFrame();
            }
        } else {
            ZoneScopedN("MinimizedWait");
            SDL_Delay(100);
        }

        // unfocused windows often lose vsync pacing and burn CPU/GPU
        if (!windowFocused && !minimized && !quit && !bench) {
            ZoneScopedN("UnfocusedFramePacing");
            const auto frameEndTime = std::chrono::high_resolution_clock::now();
            const double elapsedMs = std::chrono::duration<double, std::milli>(frameEndTime - currentTime).count();
            if (elapsedMs < kUnfocusedTargetMs) {
                SDL_Delay(static_cast<Uint32>(kUnfocusedTargetMs - elapsedMs));
            }
        }

        lastTime = currentTime;
        FrameMark;
        if (bench && !minimized && bench->onFrame(renderer->getGpuTimer(), renderer->getCulling().getStats())) {
            quit = true;
        }
    }
    deviceRef.waitIdle();
}

#if ENGINE_ENABLE_IMGUI
void Engine::applyUiRequests(const UiRequests& requests)
{
    const auto report = [this](const std::optional<SceneActionStatus>& status) {
        if (status) {
            ui->setLoadStatus(status->text, status->isError);
        }
    };
    if (requests.unload) {
        report(unloadModel(*requests.unload));
    }
    if (const std::optional<UiRequests::Load>& load = requests.load) {
        report(loadModel(load->asset, load->position));
    }
    if (requests.shouldClearInstanceField) {
        report(clearInstanceField());
    }
    if (const std::optional<UiRequests::InstanceField>& field = requests.spawnInstanceField) {
        report(spawnInstanceField(field->center, field->side, field->spacing));
    }
}
#endif

void Engine::recreateSwapchain()
{
    if (!swapChain || !renderer) {
        return;
    }

    swapChain->recreateSwapChain();
    renderer->rebuildSwapchainResources();

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
    if (window != nullptr) {
        SDL_DestroyWindow(window);
        logInfo("Window destroyed", "Engine");
        window = nullptr;
    }
    SDL_Quit();
    device.reset();
    logInfo("Device cleaned up", "Engine");
    initialized = false;
    logInfo("Engine cleaned up", "Engine");
}
