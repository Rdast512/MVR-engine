#include "vk_engine.hpp"
#include "../Constants.h"
#include "../static_headers/logger.hpp"
#include "../util/debug.hpp"
#include "../util/vk_tracy.hpp"
#if ENGINE_ENABLE_IMGUI
    #include "imgui.h"
    #include "imgui_impl_sdl3.h"
    #include "imgui_impl_vulkan.h"
#endif

#include <algorithm>
#include <format>
#include <utility>

#if ENGINE_ENABLE_IMGUI
namespace
{
    [[nodiscard]] double toMiB(vk::DeviceSize bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }
} // namespace
#endif


Engine::~Engine() { cleanup(); }

void Engine::initialize()
{
    ZoneScopedN("Engine::initialize");
    // Hard-sync runtime flag to the compile-time switch so a half-enabled path is impossible.
    enableImGui = (ENGINE_ENABLE_IMGUI != 0);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error("Failed to initialize SDL: " + std::string(SDL_GetError()));
    }

    window = SDL_CreateWindow("Vulkan", static_cast<int>(WIDTH), static_cast<int>(HEIGHT),
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);

    if (window == nullptr) {
        throw std::runtime_error("Failed to create window");
    }

    SDL_SetWindowRelativeMouseMode(window, true);

#if ENGINE_ENABLE_IMGUI
    if (enableImGui) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        log_info("ImGui enabled (ENGINE_ENABLE_IMGUI=1)", "Engine");
    }
#else
    log_info("ImGui fully disabled (ENGINE_ENABLE_IMGUI=0) — no context, backends, or GPU resources", "Engine");
#endif

    device = std::make_unique<Device>(window, false);
    device->init();

    allocator = std::make_unique<VkAllocator>(*device);

    descriptorManager = std::make_unique<DescriptorManager>(device->vkdevice, allocator->allocator,
                                                            device->queueFamilyIndices, device->capabilities);
    descriptorManager->init();

    swapChain = std::make_unique<SwapChain>(window, *device);
    swapChain->init();

    camera = std::make_unique<Camera>(*swapChain);
    textureManager = std::make_unique<TextureManager>(*device, *allocator, *descriptorManager);
    textureManager->init();

    scene = std::make_unique<Scene>();
    assetsLoader = std::make_unique<AssetsLoader>(scene->objectStorage, *textureManager, scene->geometryStore,
                                                  scene->materialStore, scene->lightStore);

    const glm::vec3 initialAssetPos{0.0f, 0.0f, 0.0f};
    if (!assetsLoader->loadModel(MODEL_PATH.string(), initialAssetPos)) {
        log_error(std::format("Startup model failed to load: {}", MODEL_PATH.string()), "Engine");
    }
    // Aim free-fly camera at the only startup model so the scene is visible immediately.
    camera->focusOn(initialAssetPos);
    resourceManager = std::make_unique<ResourceManager>(*device, *allocator, scene->geometryStore, scene->materialStore,
                                                        scene->lightStore, scene->objectStorage);
    resourceManager->init();
    resourceManager->createCameraBuffers(*camera);

    tracyContext = std::make_unique<VkTracyContext>();
    {
        const vk::CommandBufferAllocateInfo tracySetupCommandBufferAllocateInfo{
            .commandPool = *resourceManager->commandPool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        };
        vk::raii::CommandBuffers tracySetupCommandBuffers(device->vkdevice, tracySetupCommandBufferAllocateInfo);
        tracyContext->init(device->instance, device->physicalDevice, device->vkdevice, device->graphicsQueue,
                           tracySetupCommandBuffers.front(), "Graphics Queue");
    }


#if ENGINE_ENABLE_IMGUI
    if (enableImGui) {
        createImGuiDescriptorPool();
    }
#endif

    pipeline = std::make_unique<Pipeline>(*resourceManager, *descriptorManager, device->vkdevice,
                                          swapChain->swapChainExtent, swapChain->swapChainImageFormat);
    pipeline->init();

    renderer = std::make_unique<Renderer>(*device, *swapChain, *resourceManager, *descriptorManager, *pipeline, *camera,
                                          tracyContext.get(), enableImGui);
    renderer->rebuildSwapchainResources();

#if ENGINE_ENABLE_IMGUI
    if (enableImGui) {
        ImGui_ImplSDL3_InitForVulkan(window);
        ImGui_ImplVulkan_InitInfo init_info = {};
        init_info.ApiVersion = VK_API_VERSION_1_4;
        init_info.Instance = *device->instance;
        init_info.PhysicalDevice = *device->physicalDevice;
        init_info.Device = *device->vkdevice;
        init_info.QueueFamily = device->graphicsIndex;
        init_info.Queue = *device->graphicsQueue;
        init_info.DescriptorPool = *imguiDescriptorPool;
        const auto imageCount = static_cast<uint32_t>(swapChain->swapChainImages.size());
        init_info.MinImageCount = std::max<uint32_t>(imageCount, 2);
        init_info.ImageCount = std::max<uint32_t>(imageCount, init_info.MinImageCount);
        init_info.UseDynamicRendering = true;
        imguiColorFormat = static_cast<VkFormat>(swapChain->swapChainImageFormat);
        imguiDepthFormat = static_cast<VkFormat>(resourceManager->findDepthFormat());
        imguiPipelineRenderingInfo = VkPipelineRenderingCreateInfoKHR{};
        imguiPipelineRenderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        imguiPipelineRenderingInfo.colorAttachmentCount = 1;
        imguiPipelineRenderingInfo.pColorAttachmentFormats = &imguiColorFormat;
        imguiPipelineRenderingInfo.depthAttachmentFormat = imguiDepthFormat;
        imguiPipelineRenderingInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
        init_info.PipelineInfoMain.RenderPass = VK_NULL_HANDLE;
        init_info.PipelineInfoMain.Subpass = 0;
        init_info.PipelineInfoMain.MSAASamples = static_cast<VkSampleCountFlagBits>(device->msaaSamples);
        init_info.PipelineInfoMain.PipelineRenderingCreateInfo = imguiPipelineRenderingInfo;
        if (!ImGui_ImplVulkan_Init(&init_info)) {
            throw std::runtime_error("ImGui_ImplVulkan_Init failed");
        }
    }
#endif

    initialized = true;
}

void Engine::createImGuiDescriptorPool()
{
#if ENGINE_ENABLE_IMGUI
    ZoneScopedN("Engine::createImGuiDescriptorPool");
    // Backends require separate sampled image + sampler descriptors (not combined).
    auto& vkDevice = device->vkdevice;
    const uint32_t imguiSampledImageMin = std::max<uint32_t>(IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE, 1000);
    const uint32_t imguiSamplerMin = std::max<uint32_t>(IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE, 1000);
    std::array poolSizes{
        vk::DescriptorPoolSize{.type = vk::DescriptorType::eSampler, .descriptorCount = imguiSamplerMin},
        vk::DescriptorPoolSize{.type = vk::DescriptorType::eSampledImage, .descriptorCount = imguiSampledImageMin},
        vk::DescriptorPoolSize{.type = vk::DescriptorType::eStorageImage, .descriptorCount = 1000},
        vk::DescriptorPoolSize{.type = vk::DescriptorType::eUniformTexelBuffer, .descriptorCount = 1000},
        vk::DescriptorPoolSize{.type = vk::DescriptorType::eStorageTexelBuffer, .descriptorCount = 1000},
        vk::DescriptorPoolSize{.type = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1000},
        vk::DescriptorPoolSize{.type = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1000},
        vk::DescriptorPoolSize{.type = vk::DescriptorType::eUniformBufferDynamic, .descriptorCount = 1000},
        vk::DescriptorPoolSize{.type = vk::DescriptorType::eStorageBufferDynamic, .descriptorCount = 1000},
        vk::DescriptorPoolSize{.type = vk::DescriptorType::eInputAttachment, .descriptorCount = 1000}};

    const uint32_t maxSets = 1000 * static_cast<uint32_t>(poolSizes.size());
    vk::DescriptorPoolCreateInfo poolInfo{.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
                                          .maxSets = maxSets,
                                          .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
                                          .pPoolSizes = poolSizes.data()};
    imguiDescriptorPool = vk::raii::DescriptorPool(vkDevice, poolInfo);
    setDebugName(vkDevice, imguiDescriptorPool, "ImGuiDescriptorPool");
#endif
}

void Engine::run()
{
    if (!initialized) {
        initialize();
    }


    bool quit = false;
    bool minimized = false;
    // OS keyboard focus (not ImGui "game focus"). Used to soft-cap FPS when another window is on top.
    bool windowFocused = true;
    // Game mode: relative mouse + hidden ImGui. UI mode (I): free cursor, only ImGui focused.
    lastTime = std::chrono::high_resolution_clock::now();
    fpsTime = lastTime;
    // Background throttle only — focused path relies on swapchain vsync (FIFO).
    constexpr double kUnfocusedTargetMs = 1000.0 / 30.0;
    auto& deviceRef = device->vkdevice;

    const auto setGameFocus = [this](bool gameFocused)
    {
        // gameFocused = true  → capture mouse, hide cursor (look/move)
        // gameFocused = false → free mouse, show cursor (ImGui only)
        SDL_SetWindowRelativeMouseMode(window, gameFocused);
        if (gameFocused) {
            SDL_HideCursor();
        } else {
            SDL_ShowCursor();
        }
    };
    imguiUiOpen = false;
    setImGuiInputEnabled(false);
    setGameFocus(true);

    while (!quit) {
        ZoneScopedN("Frame");

        auto currentTime = std::chrono::high_resolution_clock::now();
        frameCount++;

        // averaged over one second, shown by the ImGui stats overlay
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
                // Only feed ImGui while the UI is open so it cannot steal game input.
                if (enableImGui && imguiUiOpen) {
                    ImGui_ImplSDL3_ProcessEvent(&e);
                }
#endif

                if (e.type == SDL_EVENT_QUIT) {
                    quit = true;
                }
#if ENGINE_ENABLE_IMGUI
                else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_I && !e.key.repeat) {
                    // I toggles ImGui. While typing in an ImGui field, let 'i' go to the widget.
                    const bool typingInImGui = enableImGui && imguiUiOpen && ImGui::GetIO().WantTextInput;
                    if (!typingInImGui && enableImGui) {
                        imguiUiOpen = !imguiUiOpen;
                        setImGuiInputEnabled(imguiUiOpen);
                        // Open UI → ImGui focus. Close UI → game focus.
                        setGameFocus(!imguiUiOpen);
                    }
                }
#endif
                else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !imguiUiOpen && e.button.button == SDL_BUTTON_LEFT) {
                    // Re-assert relative mode on click while in game focus — SDL may
                    // drop it on focus loss until a mouse button is pressed.
                    setGameFocus(true);
                } else if (e.type == SDL_EVENT_MOUSE_MOTION && !imguiUiOpen) {
                    camera->rotate(-e.motion.xrel, e.motion.yrel);
                } else if (e.type == SDL_EVENT_MOUSE_WHEEL && !imguiUiOpen) {
                    camera->addFov(-e.wheel.y * 2.0f); // scroll up = zoom in (narrower FOV)
                } else if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
                    windowFocused = true;
                    setGameFocus(!imguiUiOpen);
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
        if (enableImGui) {
            drawImGui();
            applyPendingModelActions();
        }
#endif

        if (!minimized && !quit) {
            // ── Camera keyboard movement (game focus only) ───────
            // WASD        → forward / back / left / right
            // LShift      → up (+Y)
            // LCtrl       → down (-Y)
            if (!imguiUiOpen) {
                ZoneScopedN("CameraInput");
                const float dt = std::chrono::duration<float>(currentTime - lastTime).count();
                const float speed = 5.0f;
                const float step = speed * dt;

                int keyCount = 0;
                const bool* keys = SDL_GetKeyboardState(&keyCount);

                if (keys[SDL_SCANCODE_W])
                    camera->moveForward(step);
                if (keys[SDL_SCANCODE_S])
                    camera->moveForward(-step);
                if (keys[SDL_SCANCODE_A])
                    camera->moveRight(-step);
                if (keys[SDL_SCANCODE_D])
                    camera->moveRight(step);
                if (keys[SDL_SCANCODE_LSHIFT])
                    camera->moveUp(step);
                if (keys[SDL_SCANCODE_LCTRL])
                    camera->moveUp(-step);
            }

            // Upload camera for this frame's in-flight slot before recording/submit.
            {
                ZoneScopedN("DrawFrame");
                camera->updateCameraData(renderer->currentFrame);
                renderer->drawFrame();
            }
        } else {
            ZoneScopedN("MinimizedWait");
            SDL_Delay(100);
        }

        // When unfocused, Windows often stops vsync throttling and the loop burns CPU/GPU.
        // Soft-cap only in that case; focused frames stay paced by present mode.
        if (!windowFocused && !minimized && !quit) {
            ZoneScopedN("UnfocusedFramePacing");
            const auto frameEndTime = std::chrono::high_resolution_clock::now();
            const double frameMs =
                std::chrono::duration<double, std::milli>(frameEndTime - currentTime).count();
            if (frameMs < kUnfocusedTargetMs) {
                SDL_Delay(static_cast<Uint32>(kUnfocusedTargetMs - frameMs));
            }
        }

        lastTime = currentTime;
        FrameMark;
    }
    deviceRef.waitIdle();
}


void Engine::render()
{
    if (renderer) {
        renderer->drawFrame();
    }
}

void Engine::setImGuiInputEnabled([[maybe_unused]] bool enabled)
{
#if ENGINE_ENABLE_IMGUI
    if (!enableImGui) {
        return;
    }
    // the SDL3 backend shows the OS cursor every frame unless cursor changes are off
    constexpr ImGuiConfigFlags kGameFocusFlags = ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
    ImGuiIO& io = ImGui::GetIO();
    if (enabled) {
        io.ConfigFlags &= ~kGameFocusFlags;
    } else {
        io.ConfigFlags |= kGameFocusFlags;
    }
#endif
}

void Engine::drawImGui()
{
#if ENGINE_ENABLE_IMGUI
    ZoneScopedN("ImGuiCPU");
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    drawStatsOverlay();
    if (imguiUiOpen) {
        if (!hasScannedAssets) {
            scanFolder();
        }
        constexpr ImVec2 kWindowPos{16.0f, 16.0f};
        constexpr ImVec2 kWindowSize{480.0f, 0.0f};
        ImGui::SetNextWindowPos(kWindowPos, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(kWindowSize, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Engine")) {
            drawLoadPanel();
            drawLoadedModelsPanel();
        }
        ImGui::End();
    }
    ImGui::Render();
#endif
}

void Engine::drawStatsOverlay() const
{
#if ENGINE_ENABLE_IMGUI
    constexpr float kMargin = 10.0f;
    constexpr float kBackgroundAlpha = 0.35f;
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // pivot (1, 0): anchored by its top-right corner
    ImGui::SetNextWindowPos({viewport->WorkPos.x + viewport->WorkSize.x - kMargin, viewport->WorkPos.y + kMargin},
                            ImGuiCond_Always, {1.0f, 0.0f});
    ImGui::SetNextWindowBgAlpha(kBackgroundAlpha);
    if (ImGui::Begin("##stats", nullptr, kFlags)) {
        ImGui::Text("%.0f FPS  %.2f ms", fps, frameMs);
    }
    ImGui::End();
#endif
}

void Engine::drawLoadPanel()
{
#if ENGINE_ENABLE_IMGUI
    if (!ImGui::CollapsingHeader("Load model", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float rescanWidth = ImGui::CalcTextSize("Rescan").x + style.FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(-(rescanWidth + style.ItemSpacing.x));
    if (ImGui::InputText("##folder", &assetsPathInput[0], IM_ARRAYSIZE(assetsPathInput),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        scanFolder();
    }
    ImGui::SetItemTooltip("Folder with one subfolder per model; Enter rescans");
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) {
        scanFolder();
    }

    constexpr float kVisibleRows = 8.0f;
    if (discoveredAssets.empty()) {
        ImGui::TextDisabled("No model folders with a .gltf/.glb/.obj found");
    } else if (ImGui::BeginListBox("##models", {-FLT_MIN, kVisibleRows * ImGui::GetTextLineHeightWithSpacing()})) {
        for (std::size_t i = 0; i < discoveredAssets.size(); ++i) {
            const bool isSelected = selectedAssetIndex == static_cast<int>(i);
            const std::string label = discoveredAssets[i].filename().string();
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(label.c_str(), isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
                selectedAssetIndex = static_cast<int>(i);
                hasPendingLoad = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            }
            ImGui::PopID();
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndListBox();
    }

    ImGui::InputFloat3("Position", &loadedModelPosition[0], "%.2f");

    const bool hasSelection =
        selectedAssetIndex >= 0 && static_cast<std::size_t>(selectedAssetIndex) < discoveredAssets.size();
    ImGui::BeginDisabled(!hasSelection);
    if (ImGui::Button("Load")) {
        hasPendingLoad = true;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Double-click a model to load it directly");
    if (!loadStatus.empty()) {
        constexpr ImVec4 kErrorColor{1.0f, 0.4f, 0.4f, 1.0f};
        ImGui::SameLine();
        if (isLoadStatusError) {
            ImGui::TextColored(kErrorColor, "%s", loadStatus.c_str());
        } else {
            ImGui::TextUnformatted(loadStatus.c_str());
        }
    }
#endif
}

void Engine::drawLoadedModelsPanel()
{
#if ENGINE_ENABLE_IMGUI
    const ObjectStorage& storage = scene->objectStorage;
    // ### keeps the header id stable while the count changes
    const std::string header = std::format("Loaded models ({})###loaded", storage.size());
    if (!ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    const auto [gpuUsedBytes, gpuCapacityBytes] = resourceManager->assetTotals();
    ImGui::Text("GPU geometry %.1f MiB, %u textures", toMiB(gpuUsedBytes), textureManager->size());
    if (storage.empty()) {
        ImGui::TextDisabled("Nothing loaded");
        return;
    }

    constexpr int kColumnCount = 6;
    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##loadedModels", kColumnCount, kTableFlags)) {
        return;
    }
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Prims");
    ImGui::TableSetupColumn("Meshlets");
    ImGui::TableSetupColumn("Textures");
    ImGui::TableSetupColumn("Geometry");
    ImGui::TableSetupColumn("##unload");
    ImGui::TableHeadersRow();

    for (EntityId id = 0; id < storage.size(); ++id) {
        ImGui::PushID(static_cast<int>(id));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(std::filesystem::path(storage.names[id]).filename().string().c_str());
        ImGui::SetItemTooltip("%s", storage.names[id].c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%u", storage.primitiveCounts[id]);
        ImGui::TableNextColumn();
        ImGui::Text("%u", storage.meshletDraws[id].meshletCount);
        ImGui::TableNextColumn();
        ImGui::Text("%zu", storage.textureRefs[id].size());
        ImGui::TableNextColumn();
        ImGui::Text("%.1f MiB", toMiB(resourceManager->modelGpuBytes(id)));
        ImGui::TableNextColumn();
        if (ImGui::SmallButton("Unload")) {
            pendingUnload = id;
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
#endif
}

void Engine::applyPendingModelActions()
{
    if (const std::optional<EntityId> id = std::exchange(pendingUnload, std::nullopt)) {
        unloadModel(*id);
    }
    if (std::exchange(hasPendingLoad, false)) {
        loadSelectedModel();
    }
}


void Engine::scanFolder()
{
    ZoneScopedN("Engine::scanFolder");
    discoveredAssets.clear();
    selectedAssetIndex = -1;
    hasScannedAssets = true;

    std::filesystem::path rootPath = std::filesystem::path(assetsPathInput).make_preferred();
    if (rootPath.empty()) {
        rootPath = std::filesystem::path(ENGINE_MODELS_DIR).make_preferred();
    }

    std::error_code errorCode;
    if (!std::filesystem::exists(rootPath, errorCode) || !std::filesystem::is_directory(rootPath, errorCode)) {
        log_info("ImGui scanFolder failed: invalid asset directory", "Engine");
        return;
    }

    auto folderContainsModel = [&errorCode](const std::filesystem::path& folder) -> bool {
        for (const auto& file : std::filesystem::directory_iterator(
                 folder, std::filesystem::directory_options::skip_permission_denied, errorCode)) {
            if (errorCode || !file.is_regular_file(errorCode)) {
                continue;
            }
            std::string extension = file.path().extension().string();
            std::ranges::transform(extension, extension.begin(), [](unsigned char character) -> char
                                   { return static_cast<char>(std::tolower(character)); });
            // same formats AssetsLoader::loadModel picks from a folder
            if (extension == ".gltf" || extension == ".glb" || extension == ".obj") {
                return true;
            }
        }
        return false;
    };

    for (const auto& entry : std::filesystem::directory_iterator(
             rootPath, std::filesystem::directory_options::skip_permission_denied, errorCode)) {
        if (errorCode) {
            break;
        }
        if (!entry.is_directory(errorCode)) {
            continue;
        }
        auto folderPath = entry.path();
        folderPath.make_preferred();
        if (folderContainsModel(folderPath)) {
            discoveredAssets.emplace_back(std::move(folderPath));
        }
    }

    std::ranges::sort(discoveredAssets);
    if (!discoveredAssets.empty()) {
        selectedAssetIndex = 0;
    }

    log_info(std::format("ImGui scanFolder found {} model folders", discoveredAssets.size()), "Engine");
}

void Engine::recreateSwapchain()
{
    ZoneScopedN("Engine::recreateSwapchain");
    if (!swapChain || !renderer || !resourceManager) {
        return;
    }

    swapChain->recreateSwapChain();
    renderer->rebuildSwapchainResources();

#if ENGINE_ENABLE_IMGUI
    if (enableImGui) {
        const auto imageCount = static_cast<uint32_t>(swapChain->swapChainImages.size());
        ImGui_ImplVulkan_SetMinImageCount(std::max<uint32_t>(imageCount, 2));
        imguiColorFormat = static_cast<VkFormat>(swapChain->swapChainImageFormat);
    }
#endif

#ifdef TRACY_ENABLE
    TracyMessageL("Swapchain recreated");
    TracyPlot("Vulkan/SwapchainWidth", static_cast<double>(swapChain->swapChainExtent.width));
    TracyPlot("Vulkan/SwapchainHeight", static_cast<double>(swapChain->swapChainExtent.height));
    TracyPlot("Vulkan/SwapchainImagesInUse", static_cast<double>(swapChain->swapChainImages.size()));
#endif
}

void Engine::loadSelectedModel()
{
    ZoneScopedN("Engine::loadSelectedModel");
    if (selectedAssetIndex < 0 || static_cast<std::size_t>(selectedAssetIndex) >= discoveredAssets.size()) {
        log_info("Load model: no selected asset", "Engine");
        return;
    }

    const std::filesystem::path& assetPath = discoveredAssets[static_cast<std::size_t>(selectedAssetIndex)];
    const std::string name = assetPath.filename().string();
    log_info(std::format("Load model started: {}", assetPath.string()), "Engine");
#ifdef TRACY_ENABLE
    {
        const std::string msg = std::format("LoadModel {}", assetPath.string());
        TracyMessage(msg.c_str(), msg.size());
    }
#endif
    device->vkdevice.waitIdle();
    const auto start = std::chrono::steady_clock::now();
    std::optional<EntityId> id;
    try {
        id = assetsLoader->loadModel(assetPath.string(), glm::make_vec3(&loadedModelPosition[0]));
    } catch (const std::exception& error) {
        // UI boundary: a broken model must not take the engine down
        log_error(std::format("Load model {} failed: {}", name, error.what()), "Engine");
    }
    // uploads the new model, or discards what a failed load left in the scratch
    resourceManager->flushGpuAssets();
    if (!id) {
        loadStatus = std::format("Failed to load {}", name);
        isLoadStatusError = true;
        return;
    }
    resourceManager->ensureInstanceCapacity(scene->objectStorage.size());
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    loadStatus = std::format("Loaded {} in {:.2f} s", name, seconds);
    isLoadStatusError = false;
}

void Engine::unloadModel(EntityId id)
{
    ZoneScopedN("Engine::unloadModel");
    ObjectStorage& storage = scene->objectStorage;
    if (id >= storage.size()) {
        return;
    }
    const std::string name = std::filesystem::path(storage.names[id]).filename().string();
    // frames in flight still read the buffers, textures and heap slots being freed
    device->vkdevice.waitIdle();
    textureManager->release(storage.textureRefs[id]);
    resourceManager->eraseModel(id);
#if ENGINE_USE_MIMALLOC
    mi_collect(true);
#endif
    log_info(std::format("Unloaded model {}: {} textures left", name, textureManager->size()), "Engine");
    loadStatus = std::format("Unloaded {}", name);
    isLoadStatusError = false;
}

void Engine::shutdown() { cleanup(); }

void Engine::cleanup()
{
    ZoneScopedN("Engine::cleanup");
    if (!initialized)
        return;
    auto& deviceRef = device->vkdevice;
    {
        ZoneScopedN("Engine::cleanup::waitIdle");
        deviceRef.waitIdle();
    }

    if (tracyContext) {
        tracyContext->shutdown();
        tracyContext.reset();
    }

#if ENGINE_ENABLE_IMGUI
    if (enableImGui) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        // clear() destroys the VkDescriptorPool. reset() is vkResetDescriptorPool and
        // leaves the raii handle (and its DeviceDispatcher*) alive past device.reset().
        imguiDescriptorPool.clear();
    }
#endif

    if (scene) {
        scene->objectStorage.clear();
    }
    log_info("Object storage cleared", "Engine");
    // Explicitly clear command buffers before destroying other resources
    if (resourceManager) {
        resourceManager->commandBuffers.clear();
        resourceManager->transferCommandBuffer.clear();
    }

    renderer.reset();
    pipeline.reset();
    descriptorManager.reset();
    textureManager.reset();
    resourceManager.reset(); // before assetsLoader: holds refs to its vertex/index vectors
    assetsLoader.reset();
    scene.reset();
    camera.reset();
    log_info("Resources cleaned up", "Engine");
    swapChain.reset();
    log_info("Swap chain cleaned up", "Engine");
    allocator.reset();
    log_info("Allocator cleaned up", "Engine");
    SDL_DestroyWindow(window);
    log_info("Window destroyed", "Engine");
    window = nullptr;
    SDL_Quit();
    device.reset();
    log_info("Device cleaned up", "Engine");
    initialized = false;
    log_info("Engine cleaned up", "Engine");
}
