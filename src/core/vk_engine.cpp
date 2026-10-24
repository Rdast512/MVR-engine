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
#include <cmath>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if ENGINE_ENABLE_IMGUI
namespace
{
    [[nodiscard]] double toMiB(vk::DeviceSize bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

    // the swapchain is *_SRGB, so ImGui's sRGB-authored colors must be stored linear
    [[nodiscard]] ImVec4 srgbToLinear(const ImVec4& color)
    {
        const auto channel = [](float c) {
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        };
        return {channel(color.x), channel(color.y), channel(color.z), color.w};
    }

    [[nodiscard]] std::string formatSize(vk::DeviceSize bytes)
    {
        constexpr vk::DeviceSize kMiB = 1024 * 1024;
        return bytes >= kMiB ? std::format("{:.1f} MiB", toMiB(bytes))
                             : std::format("{:.1f} KiB", static_cast<double>(bytes) / 1024.0);
    }

    // one row of the asset tree: label | value | size, size column left current for the caller
    template <typename... Args>
    void propertyRow(const char* label, std::format_string<Args...> fmt, Args&&... args)
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                     ImGuiTreeNodeFlags_SpanFullWidth);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(std::format(fmt, std::forward<Args>(args)...).c_str());
        ImGui::TableNextColumn();
    }

    void textureRow(const TextureManager& textures, const char* slot, uint32_t heapIndex)
    {
        if (heapIndex == kNoneIndex) {
            return;
        }
        const std::optional<TextureManager::TextureInfo> info = textures.textureInfo(heapIndex);
        if (!info) {
            propertyRow(slot, "missing (heap {})", heapIndex);
            return;
        }
        propertyRow(slot, "{}", info->name);
        ImGui::TextUnformatted(formatSize(info->bytes).c_str());
    }

    [[nodiscard]] const char* alphaModeName(uint32_t flags)
    {
        switch (flags & GpuMaterialFlag::AlphaModeMask) {
        case GpuMaterialFlag::AlphaMask:  return "mask";
        case GpuMaterialFlag::AlphaBlend: return "blend";
        default:                          return "opaque";
        }
    }

    [[nodiscard]] const char* lightTypeName(uint32_t type)
    {
        switch (type) {
        case 0:  return "Directional";
        case 2:  return "Spot";
        default: return "Point";
        }
    }

    // distinct textures a material samples, base and extension slots
    [[nodiscard]] std::vector<uint32_t> materialTextures(const MaterialStore::MaterialInfo& info)
    {
        const GpuMaterial& m = info.material;
        const MaterialPbrExtension& e = info.extension;
        std::vector<uint32_t> heapIndices{
            m.baseColorTex,         m.metalRoughTex,          m.normalTex,
            m.occlusionTex,         m.emissiveTex,            e.specular.tex,
            e.specularColor.tex,    e.clearcoat.tex,          e.clearcoatRoughness.tex,
            e.clearcoatNormal.tex,  e.sheenColor.tex,         e.sheenRoughness.tex,
            e.transmission.tex,     e.thickness.tex,          e.iridescence.tex,
            e.iridescenceThickness.tex, e.anisotropy.tex,     e.diffuseTransmission.tex,
            e.diffuseTransmissionColor.tex,
        };
        std::erase(heapIndices, kNoneIndex);
        std::ranges::sort(heapIndices);
        const auto [first, last] = std::ranges::unique(heapIndices);
        heapIndices.erase(first, last);
        return heapIndices;
    }

    void drawMaterialExtensions(const TextureManager& textures, const MaterialPbrExtension& e)
    {
        const auto has = [&e](uint32_t flag) { return (e.flags & flag) != 0; };
        if (has(MaterialExtFlag::Unlit)) {
            propertyRow("Unlit", "yes");
        }
        if (has(MaterialExtFlag::EmissiveStrength)) {
            propertyRow("Emissive strength", "{:.2f}", e.emissiveStrength);
        }
        if (has(MaterialExtFlag::Ior)) {
            propertyRow("IOR", "{:.3f}", e.ior);
        }
        if (has(MaterialExtFlag::Dispersion)) {
            propertyRow("Dispersion", "{:.3f}", e.dispersion);
        }
        if (has(MaterialExtFlag::Specular)) {
            propertyRow("Specular", "{:.2f}, color ({:.2f}, {:.2f}, {:.2f})", e.specularFactor,
                        e.specularColorFactor.r, e.specularColorFactor.g, e.specularColorFactor.b);
            textureRow(textures, "Specular map", e.specular.tex);
            textureRow(textures, "Specular color map", e.specularColor.tex);
        }
        if (has(MaterialExtFlag::Clearcoat)) {
            propertyRow("Clearcoat", "{:.2f}, roughness {:.2f}", e.clearcoatFactor, e.clearcoatRoughnessFactor);
            textureRow(textures, "Clearcoat map", e.clearcoat.tex);
            textureRow(textures, "Clearcoat roughness map", e.clearcoatRoughness.tex);
            textureRow(textures, "Clearcoat normal map", e.clearcoatNormal.tex);
        }
        if (has(MaterialExtFlag::Sheen)) {
            propertyRow("Sheen", "color ({:.2f}, {:.2f}, {:.2f}), roughness {:.2f}", e.sheenColorFactor.r,
                        e.sheenColorFactor.g, e.sheenColorFactor.b, e.sheenRoughnessFactor);
            textureRow(textures, "Sheen color map", e.sheenColor.tex);
            textureRow(textures, "Sheen roughness map", e.sheenRoughness.tex);
        }
        if (has(MaterialExtFlag::Transmission)) {
            propertyRow("Transmission", "{:.2f}", e.transmissionFactor);
            textureRow(textures, "Transmission map", e.transmission.tex);
        }
        if (has(MaterialExtFlag::Volume)) {
            propertyRow("Volume", "thickness {:.3f}, attenuation distance {:.2f}", e.thicknessFactor,
                        e.attenuationDistance);
            textureRow(textures, "Thickness map", e.thickness.tex);
        }
        if (has(MaterialExtFlag::Iridescence)) {
            propertyRow("Iridescence", "{:.2f}, ior {:.2f}, thickness [{:.0f}, {:.0f}] nm", e.iridescenceFactor,
                        e.iridescenceIor, e.iridescenceThicknessMin, e.iridescenceThicknessMax);
            textureRow(textures, "Iridescence map", e.iridescence.tex);
            textureRow(textures, "Iridescence thickness map", e.iridescenceThickness.tex);
        }
        if (has(MaterialExtFlag::Anisotropy)) {
            propertyRow("Anisotropy", "{:.2f}, rotation {:.2f}", e.anisotropyStrength, e.anisotropyRotation);
            textureRow(textures, "Anisotropy map", e.anisotropy.tex);
        }
        if (has(MaterialExtFlag::DiffuseTransmission)) {
            propertyRow("Diffuse transmission", "{:.2f}", e.diffuseTransmissionFactor);
            textureRow(textures, "Diffuse transmission map", e.diffuseTransmission.tex);
            textureRow(textures, "Diffuse transmission color map", e.diffuseTransmissionColor.tex);
        }
    }
} // namespace
#endif

Engine::~Engine() { cleanup(); }

void Engine::initialize()
{
    // runtime flag mirrors the compile-time switch
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
        for (ImVec4& color : ImGui::GetStyle().Colors) {
            color = srgbToLinear(color);
        }
        logInfo("ImGui enabled (ENGINE_ENABLE_IMGUI=1)", "Engine");
    }
#else
    logInfo("ImGui fully disabled (ENGINE_ENABLE_IMGUI=0) — no context, backends, or GPU resources", "Engine");
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
        logError(std::format("Startup model failed to load: {}", MODEL_PATH.string()), "Engine");
    }
    // scene is visible immediately
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

    shaderObjects = std::make_unique<ShaderObjects>(*resourceManager, *descriptorManager, device->vkdevice);
    shaderObjects->init();

    renderer = std::make_unique<Renderer>(*device, *swapChain, *resourceManager, *descriptorManager, *shaderObjects,
                                          *camera, tracyContext.get(), enableImGui);
    renderer->rebuildSwapchainResources();

#if ENGINE_ENABLE_IMGUI
    if (enableImGui) {
        ImGui_ImplSDL3_InitForVulkan(window);
        ImGui_ImplVulkan_InitInfo initInfo = {};
        initInfo.ApiVersion = VK_API_VERSION_1_4;
        initInfo.Instance = *device->instance;
        initInfo.PhysicalDevice = *device->physicalDevice;
        initInfo.Device = *device->vkdevice;
        initInfo.QueueFamily = device->graphicsIndex;
        initInfo.Queue = *device->graphicsQueue;
        initInfo.DescriptorPool = *imguiDescriptorPool;
        const auto imageCount = static_cast<uint32_t>(swapChain->swapChainImages.size());
        initInfo.MinImageCount = std::max<uint32_t>(imageCount, 2);
        initInfo.ImageCount = std::max<uint32_t>(imageCount, initInfo.MinImageCount);
        initInfo.UseDynamicRendering = true;
        imguiColorFormat = static_cast<VkFormat>(swapChain->swapChainImageFormat);
        imguiDepthFormat = static_cast<VkFormat>(resourceManager->findDepthFormat());
        imguiPipelineRenderingInfo = VkPipelineRenderingCreateInfoKHR{};
        imguiPipelineRenderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        imguiPipelineRenderingInfo.colorAttachmentCount = 1;
        imguiPipelineRenderingInfo.pColorAttachmentFormats = &imguiColorFormat;
        imguiPipelineRenderingInfo.depthAttachmentFormat = imguiDepthFormat;
        imguiPipelineRenderingInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
        initInfo.PipelineInfoMain.RenderPass = VK_NULL_HANDLE;
        initInfo.PipelineInfoMain.Subpass = 0;
        initInfo.PipelineInfoMain.MSAASamples = static_cast<VkSampleCountFlagBits>(device->msaaSamples);
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = imguiPipelineRenderingInfo;
        if (!ImGui_ImplVulkan_Init(&initInfo)) {
            throw std::runtime_error("ImGui_ImplVulkan_Init failed");
        }
    }
#endif

    initialized = true;
}

void Engine::createImGuiDescriptorPool()
{
#if ENGINE_ENABLE_IMGUI
    // backends need separate sampled image + sampler descriptors, not combined
    constexpr uint32_t kPoolCount = 1000;
    auto& vkDevice = device->vkdevice;
    const uint32_t imguiSampledImageMin =
        std::max<uint32_t>(IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE, kPoolCount);
    const uint32_t imguiSamplerMin =
        std::max<uint32_t>(IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE, kPoolCount);
    const std::array poolSizes{
        vk::DescriptorPoolSize{.type = vk::DescriptorType::eSampler, .descriptorCount = imguiSamplerMin},
        vk::DescriptorPoolSize{.type = vk::DescriptorType::eSampledImage, .descriptorCount = imguiSampledImageMin},
        vk::DescriptorPoolSize{.type = vk::DescriptorType::eStorageImage, .descriptorCount = kPoolCount},
        vk::DescriptorPoolSize{.type = vk::DescriptorType::eUniformTexelBuffer, .descriptorCount = kPoolCount},
        vk::DescriptorPoolSize{.type = vk::DescriptorType::eStorageTexelBuffer, .descriptorCount = kPoolCount},
        vk::DescriptorPoolSize{.type = vk::DescriptorType::eUniformBuffer, .descriptorCount = kPoolCount},
        vk::DescriptorPoolSize{.type = vk::DescriptorType::eStorageBuffer, .descriptorCount = kPoolCount},
        vk::DescriptorPoolSize{.type = vk::DescriptorType::eUniformBufferDynamic, .descriptorCount = kPoolCount},
        vk::DescriptorPoolSize{.type = vk::DescriptorType::eStorageBufferDynamic, .descriptorCount = kPoolCount},
        vk::DescriptorPoolSize{.type = vk::DescriptorType::eInputAttachment, .descriptorCount = kPoolCount}};

    const uint32_t maxSets = kPoolCount * static_cast<uint32_t>(poolSizes.size());
    const vk::DescriptorPoolCreateInfo poolInfo{.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
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
    // OS keyboard focus, not ImGui game focus; soft-caps FPS when another window is on top
    bool windowFocused = true;
    lastTime = std::chrono::high_resolution_clock::now();
    fpsTime = lastTime;
    // focused frames rely on swapchain vsync (FIFO)
    constexpr double kUnfocusedTargetMs = 1000.0 / 30.0;
    constexpr float kCameraSpeed = 5.0f;
    auto& deviceRef = device->vkdevice;

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
    imguiUiOpen = false;
    setImGuiInputEnabled(false);
    setGameFocus(true);

    while (!quit) {
        ZoneScopedN("Frame");

        const auto currentTime = std::chrono::high_resolution_clock::now();
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
                // feed ImGui only while open so it cannot steal game input
                if (enableImGui && imguiUiOpen) {
                    ImGui_ImplSDL3_ProcessEvent(&e);
                }
#endif

                if (e.type == SDL_EVENT_QUIT) {
                    quit = true;
                }
#if ENGINE_ENABLE_IMGUI
                else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_I && !e.key.repeat) {
                    // typing 'i' in an ImGui field goes to the widget
                    const bool typingInImGui = enableImGui && imguiUiOpen && ImGui::GetIO().WantTextInput;
                    if (!typingInImGui && enableImGui) {
                        imguiUiOpen = !imguiUiOpen;
                        setImGuiInputEnabled(imguiUiOpen);
                        setGameFocus(!imguiUiOpen);
                    }
                }
#endif
                else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !imguiUiOpen && e.button.button == SDL_BUTTON_LEFT) {
                    // SDL may drop relative mode on focus loss until a click
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
            // WASD moves, LShift up, LCtrl down
            if (!imguiUiOpen) {
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
        if (!windowFocused && !minimized && !quit) {
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
            drawAssetsPanel();
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

    constexpr int kColumnCount = 7;
    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##loadedModels", kColumnCount, kTableFlags)) {
        return;
    }
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Nodes");
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
        const ModelId modelId = storage.modelIds[id];
        const bool hasModel = modelId < resourceManager->modelAllocations.size();
        const ModelAllocation* model = hasModel ? &resourceManager->modelAllocations[modelId] : nullptr;
        ImGui::TableNextColumn();
        ImGui::Text("%u", model != nullptr ? model->nodes.count : 0);
        ImGui::SetItemTooltip("mesh placements; prims and meshlets are stored once per mesh");
        ImGui::TableNextColumn();
        ImGui::Text("%u", storage.primitiveCounts[id]);
        ImGui::TableNextColumn();
        ImGui::Text("%u", storage.meshletDraws[id].meshletCount);
        ImGui::TableNextColumn();
        ImGui::Text("%zu", model != nullptr ? model->textureRefs.size() : 0);
        ImGui::TableNextColumn();
        if (model != nullptr && model->refCount > 1) {
            ImGui::Text("%.1f MiB x%u", toMiB(resourceManager->modelGpuBytes(modelId)), model->refCount);
            ImGui::SetItemTooltip("geometry shared by %u entities loaded from the same file", model->refCount);
        } else {
            ImGui::Text("%.1f MiB", toMiB(hasModel ? resourceManager->modelGpuBytes(modelId) : 0));
        }
        ImGui::TableNextColumn();
        if (ImGui::SmallButton("Unload")) {
            pendingUnload = id;
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
#endif
}

void Engine::drawAssetsPanel() const
{
#if ENGINE_ENABLE_IMGUI
    if (!ImGui::CollapsingHeader("Assets")) {
        return;
    }
    const MaterialStore& materials = scene->materialStore;
    const LightStore& lights = scene->lightStore;
    const ObjectStorage& storage = scene->objectStorage;

    // primitives are stored once per model; every entity of the model draws them
    std::vector<uint32_t> primitiveUses(materials.infos.size(), 0);
    std::vector<uint32_t> entityUses(materials.infos.size(), 0);
    for (const ModelAllocation& model : resourceManager->modelAllocations) {
        for (uint32_t p = model.primitives.first; p < model.primitives.end(); ++p) {
            const uint32_t materialId = scene->geometryStore.primitiveDraws[p].materialId;
            const uint32_t id = materialId == kNoneIndex ? materials.defaultMaterialId() : materialId;
            if (id < primitiveUses.size()) {
                ++primitiveUses[id];
            }
        }
        for (const uint32_t id : model.materialRefs) {
            entityUses[id] += model.refCount;
        }
    }

    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("##assets", 3, kTableFlags)) {
        return;
    }
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    const ImVec2 swatchSize{ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()};
    constexpr ImGuiColorEditFlags kSwatchFlags = ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop;

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    const std::string materialsLabel = std::format("Materials ({})###materials", materials.liveCount());
    const bool isMaterialsOpen = ImGui::TreeNodeEx(materialsLabel.c_str(), ImGuiTreeNodeFlags_SpanFullWidth);
    ImGui::TableNextColumn();
    ImGui::TextDisabled("cache %u hits / %u misses", materials.cacheHits, materials.cacheMisses);
    ImGui::SetItemTooltip("hit: a loaded material identical to a live row reuses it, across models");
    ImGui::TableNextColumn();
    if (isMaterialsOpen) {
        constexpr vk::DeviceSize kRowBytes = sizeof(GpuMaterial) + sizeof(GpuMaterialExtension);
        for (uint32_t id = 0; id < materials.infos.size(); ++id) {
            const MaterialStore::MaterialInfo& info = materials.infos[id];
            if (info.isFree) {
                continue;
            }
            const GpuMaterial& m = info.material;
            vk::DeviceSize textureBytes = 0;
            for (const uint32_t heapIndex : materialTextures(info)) {
                if (const std::optional<TextureManager::TextureInfo> texture = textureManager->textureInfo(heapIndex)) {
                    textureBytes += texture->bytes;
                }
            }

            ImGui::PushID(static_cast<int>(id));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string label = info.name.empty() ? std::format("material {}", id) : info.name;
            const bool isOpen = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanFullWidth);
            ImGui::TableNextColumn();
            ImGui::ColorButton("##albedo", {m.baseColorFactor.r, m.baseColorFactor.g, m.baseColorFactor.b, 1.0f},
                               kSwatchFlags, swatchSize);
            ImGui::SameLine();
            ImGui::Text("%u objects, %u prims, %u hits", entityUses[id], primitiveUses[id], info.dedupHits);
            ImGui::SetItemTooltip("objects: entities drawing it; prims: unique primitives using it; hits: loads that "
                                  "reused this row instead of adding one (%u models hold it)",
                                  info.refCount);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(formatSize(kRowBytes + textureBytes).c_str());
            ImGui::SetItemTooltip("%s row + %s textures (shared textures count for every material using them)",
                                  formatSize(kRowBytes).c_str(), formatSize(textureBytes).c_str());

            if (isOpen) {
                propertyRow("Base color", "({:.2f}, {:.2f}, {:.2f}, {:.2f})", m.baseColorFactor.r,
                            m.baseColorFactor.g, m.baseColorFactor.b, m.baseColorFactor.a);
                propertyRow("Metallic / roughness", "{:.2f} / {:.2f}", m.metallicFactor, m.roughnessFactor);
                propertyRow("Emissive", "({:.2f}, {:.2f}, {:.2f})", m.emissiveFactor.r, m.emissiveFactor.g,
                            m.emissiveFactor.b);
                propertyRow("Normal scale", "{:.2f}", m.normalScale);
                propertyRow("Occlusion strength", "{:.2f}", m.occlusionStrength);
                propertyRow("Alpha", "{}, cutoff {:.2f}{}", alphaModeName(m.flags), m.alphaCutoff,
                            (m.flags & GpuMaterialFlag::DoubleSided) != 0 ? ", double sided" : "");
                textureRow(*textureManager, "Albedo", m.baseColorTex);
                textureRow(*textureManager, "Metallic-roughness", m.metalRoughTex);
                textureRow(*textureManager, "Normal", m.normalTex);
                textureRow(*textureManager, "Occlusion", m.occlusionTex);
                textureRow(*textureManager, "Emissive map", m.emissiveTex);
                drawMaterialExtensions(*textureManager, info.extension);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }

    // warm accent keeps lights apart from material rows
    const ImVec4 lightAccent = srgbToLinear({1.0f, 0.78f, 0.3f, 1.0f});
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushStyleColor(ImGuiCol_Text, lightAccent);
    const std::string lightsLabel = std::format("Lights ({})###lights", lights.uploadedCount());
    const bool isLightsOpen = ImGui::TreeNodeEx(lightsLabel.c_str(), ImGuiTreeNodeFlags_SpanFullWidth);
    ImGui::PopStyleColor();
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    if (isLightsOpen) {
        for (uint32_t i = 0; i < lights.uploadedCount(); ++i) {
            const GpuLight& light = lights.uploaded[i];
            const bool hasOwner = light.instanceIndex < storage.size();
            const std::string owner =
                hasOwner ? std::filesystem::path(storage.names[light.instanceIndex]).stem().string() : "?";

            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, lightAccent);
            const std::string label = std::format("{} light {}", lightTypeName(light.type), i);
            const bool isOpen = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanFullWidth);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::ColorButton("##color", {light.color.r, light.color.g, light.color.b, 1.0f}, kSwatchFlags,
                               swatchSize);
            ImGui::SameLine();
            ImGui::Text("%.1f, %s", light.intensity, owner.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(formatSize(sizeof(GpuLight)).c_str());

            if (isOpen) {
                propertyRow("Color", "({:.2f}, {:.2f}, {:.2f})", light.color.r, light.color.g, light.color.b);
                propertyRow("Intensity", "{:.2f}", light.intensity);
                if (light.type != 0) {
                    propertyRow("Range", "{}", light.range > 0.0f ? std::format("{:.2f}", light.range) : "infinite");
                }
                if (light.type == 2) {
                    propertyRow("Cone inner / outer", "{:.1f} / {:.1f} deg", glm::degrees(light.innerCone),
                                glm::degrees(light.outerCone));
                }
                // GPU rows are model space; the shader applies the owner's model matrix
                const glm::mat4 model =
                    hasOwner ? computeModelMatrix(storage.transforms[light.instanceIndex]) : glm::mat4{1.0f};
                if (light.type != 0) {
                    const glm::vec3 position{model * glm::vec4(light.position, 1.0f)};
                    propertyRow("Position", "({:.2f}, {:.2f}, {:.2f})", position.x, position.y, position.z);
                }
                if (light.type != 1) {
                    const glm::vec3 direction = glm::normalize(glm::mat3(model) * light.direction);
                    propertyRow("Direction", "({:.2f}, {:.2f}, {:.2f})", direction.x, direction.y, direction.z);
                }
                propertyRow("Owner", "{} (entity {})", owner, light.instanceIndex);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
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
    discoveredAssets.clear();
    selectedAssetIndex = -1;
    hasScannedAssets = true;

    std::filesystem::path rootPath = std::filesystem::path(assetsPathInput).make_preferred();
    if (rootPath.empty()) {
        rootPath = std::filesystem::path(ENGINE_MODELS_DIR).make_preferred();
    }

    std::error_code errorCode;
    if (!std::filesystem::exists(rootPath, errorCode) || !std::filesystem::is_directory(rootPath, errorCode)) {
        logInfo("ImGui scanFolder failed: invalid asset directory", "Engine");
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

    logInfo(std::format("ImGui scanFolder found {} model folders", discoveredAssets.size()), "Engine");
}

void Engine::recreateSwapchain()
{
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
}

void Engine::loadSelectedModel()
{
    ZoneScopedN("Engine::loadSelectedModel");
    if (selectedAssetIndex < 0 || static_cast<std::size_t>(selectedAssetIndex) >= discoveredAssets.size()) {
        logInfo("Load model: no selected asset", "Engine");
        return;
    }

    const std::filesystem::path& assetPath = discoveredAssets[static_cast<std::size_t>(selectedAssetIndex)];
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
        if (const std::optional<ModelId> model = resourceManager->findModel(modelFile->string())) {
            device->vkdevice.waitIdle();
            resourceManager->instantiateModel(*model,
                                              Transform{.position = glm::make_vec3(&loadedModelPosition[0])});
            resourceManager->ensureInstanceCapacity(scene->objectStorage.size());
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            loadStatus = std::format("Instanced {} in {:.3f} s (shared)", name, seconds);
            isLoadStatusError = false;
            return;
        }
    }
    device->vkdevice.waitIdle();
    std::optional<EntityId> id;
    try {
        id = assetsLoader->loadModel(assetPath.string(), glm::make_vec3(&loadedModelPosition[0]));
    } catch (const std::exception& error) {
        // UI boundary: a broken model must not take the engine down
        logError(std::format("Load model {} failed: {}", name, error.what()), "Engine");
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
    // textures come back only when this was the model's last entity
    textureManager->release(resourceManager->eraseEntity(id));
#if ENGINE_USE_MIMALLOC
    mi_collect(true);
#endif
    logInfo(std::format("Unloaded model {}: {} textures left", name, textureManager->size()), "Engine");
    loadStatus = std::format("Unloaded {}", name);
    isLoadStatusError = false;
}

void Engine::cleanup()
{
    if (!initialized) {
        return;
    }
    auto& deviceRef = device->vkdevice;
    deviceRef.waitIdle();

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
    logInfo("Object storage cleared", "Engine");
    // command buffers go before the resources they reference
    if (resourceManager) {
        resourceManager->commandBuffers.clear();
        resourceManager->transferCommandBuffer.clear();
    }

    renderer.reset();
    shaderObjects.reset();
    descriptorManager.reset();
    textureManager.reset();
    resourceManager.reset(); // before assetsLoader: holds refs to its vertex/index vectors
    assetsLoader.reset();
    scene.reset();
    camera.reset();
    logInfo("Resources cleaned up", "Engine");
    swapChain.reset();
    logInfo("Swap chain cleaned up", "Engine");
    allocator.reset();
    logInfo("Allocator cleaned up", "Engine");
    SDL_DestroyWindow(window);
    logInfo("Window destroyed", "Engine");
    window = nullptr;
    SDL_Quit();
    device.reset();
    logInfo("Device cleaned up", "Engine");
    initialized = false;
    logInfo("Engine cleaned up", "Engine");
}
