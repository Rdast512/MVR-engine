#include "engine_ui.hpp"
#if ENGINE_ENABLE_IMGUI
    #include "../static_headers/logger.hpp"
    #include "../util/debug.hpp"
    #include "../util/vk_tracy.hpp"
    #include "core/texture_manager.hpp"
    #include "core/vk_device.hpp"
    #include "core/vk_asset_buffers.hpp"
    #include "core/vk_swapchain.hpp"
    #include "render/vk_renderer.hpp"
    #include "scene/vk_camera.hpp"
    #include "scene/vk_scene.hpp"

    #include "imgui.h"
    #include "imgui_impl_sdl3.h"
    #include "imgui_impl_vulkan.h"

    #include <algorithm>
    #include <cmath>
    #include <format>
    #include <utility>

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

    [[nodiscard]] const char* lightTypeName(GpuLightType type)
    {
        switch (type) {
        case GpuLightType::Directional: return "Directional";
        case GpuLightType::Spot:        return "Spot";
        default:                        return "Point";
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

    // enum values need not be contiguous (RTXDI's temporal bias modes skip Pairwise)
    template <typename Enum, size_t Count>
    void comboEnum(const char* label, Enum& value, const std::array<const char*, Count>& names,
                   const std::array<Enum, Count>& values)
    {
        int index = static_cast<int>(std::ranges::find(values, value) - values.begin());
        if (ImGui::Combo(label, &index, names.data(), static_cast<int>(Count))) {
            value = values[static_cast<size_t>(index)];
        }
    }
} // namespace

EngineUi::EngineUi(const UiContext& context) : context(context)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    try {
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        for (ImVec4& color : ImGui::GetStyle().Colors) {
            color = srgbToLinear(color);
        }

        createDescriptorPool();

        const Device& device = context.device;
        ImGui_ImplSDL3_InitForVulkan(context.window);
        ImGui_ImplVulkan_InitInfo initInfo = {};
        initInfo.ApiVersion = VK_API_VERSION_1_4;
        initInfo.Instance = *device.instance;
        initInfo.PhysicalDevice = *device.physicalDevice;
        initInfo.Device = *device.vkdevice;
        initInfo.QueueFamily = device.graphicsIndex;
        initInfo.Queue = *device.graphicsQueue;
        initInfo.DescriptorPool = *descriptorPool;
        const auto imageCount = static_cast<uint32_t>(context.swapChain.swapChainImages.size());
        initInfo.MinImageCount = std::max<uint32_t>(imageCount, 2);
        initInfo.ImageCount = std::max<uint32_t>(imageCount, initInfo.MinImageCount);
        initInfo.UseDynamicRendering = true;
        colorFormat = static_cast<VkFormat>(context.swapChain.swapChainImageFormat);
        // drawn in the present pass, which has no depth attachment
        depthFormat = VK_FORMAT_UNDEFINED;
        pipelineRenderingInfo = VkPipelineRenderingCreateInfoKHR{};
        pipelineRenderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        pipelineRenderingInfo.colorAttachmentCount = 1;
        pipelineRenderingInfo.pColorAttachmentFormats = &colorFormat;
        pipelineRenderingInfo.depthAttachmentFormat = depthFormat;
        pipelineRenderingInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
        initInfo.PipelineInfoMain.RenderPass = VK_NULL_HANDLE;
        initInfo.PipelineInfoMain.Subpass = 0;
        initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = pipelineRenderingInfo;
        if (!ImGui_ImplVulkan_Init(&initInfo)) {
            throw std::runtime_error("ImGui_ImplVulkan_Init failed");
        }
    } catch (...) {
        shutdown();
        throw;
    }
    logInfo("ImGui enabled (ENGINE_ENABLE_IMGUI=1)", "Engine");
}

EngineUi::~EngineUi() { shutdown(); }

void EngineUi::shutdown()
{
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    // backends register their user data only after a successful init
    if (ImGui::GetIO().BackendRendererUserData != nullptr) {
        ImGui_ImplVulkan_Shutdown();
    }
    if (ImGui::GetIO().BackendPlatformUserData != nullptr) {
        ImGui_ImplSDL3_Shutdown();
    }
    ImGui::DestroyContext();
    // clear() destroys the VkDescriptorPool. reset() is vkResetDescriptorPool and
    // leaves the raii handle (and its DeviceDispatcher*) alive past device.reset().
    descriptorPool.clear();
}

void EngineUi::createDescriptorPool()
{
    // backends need separate sampled image + sampler descriptors, not combined
    constexpr uint32_t kPoolCount = 1000;
    const auto& vkDevice = context.device.vkdevice;
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
    descriptorPool = vk::raii::DescriptorPool(vkDevice, poolInfo);
    setDebugName(vkDevice, descriptorPool, "ImGuiDescriptorPool");
}

void EngineUi::processEvent(const SDL_Event& event) { ImGui_ImplSDL3_ProcessEvent(&event); }

void EngineUi::setInputEnabled(bool enabled)
{
    isOpen = enabled;
    // the SDL3 backend shows the OS cursor every frame unless cursor changes are off
    constexpr ImGuiConfigFlags kGameFocusFlags = ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
    ImGuiIO& io = ImGui::GetIO();
    if (enabled) {
        io.ConfigFlags &= ~kGameFocusFlags;
    } else {
        io.ConfigFlags |= kGameFocusFlags;
    }
}

bool EngineUi::isTextInputActive() const { return ImGui::GetIO().WantTextInput; }

UiRequests EngineUi::takeRequests() { return std::exchange(requests, UiRequests{}); }

void EngineUi::setLoadStatus(std::string_view text, bool isError)
{
    loadStatus = text;
    isLoadStatusError = isError;
}

void EngineUi::onSwapchainRecreated()
{
    const auto imageCount = static_cast<uint32_t>(context.swapChain.swapChainImages.size());
    ImGui_ImplVulkan_SetMinImageCount(std::max<uint32_t>(imageCount, 2));
    colorFormat = static_cast<VkFormat>(context.swapChain.swapChainImageFormat);
}

void EngineUi::buildFrame(const FrameStats& stats)
{
    ZoneScopedN("ImGuiCPU");
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    drawStatsOverlay(stats);
    if (isOpen) {
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
            drawCameraPanel();
            drawDebugViewPanel();
            drawCullingPanel();
            drawSunPanel();
            drawRayTracingPanel();
            drawPostPanel();
            drawInstanceFieldPanel();
        }
        ImGui::End();
    }
    ImGui::Render();
}

void EngineUi::drawStatsOverlay(const FrameStats& stats) const
{
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
        ImGui::Text("%.0f FPS  %.2f ms", stats.fps, stats.frameMs);
        // labels screenshots and captures taken in a debug view
        if (const DebugView view = context.renderer.debugView.view; view != DebugView::Lit) {
            ImGui::Text("View: %s", debugViewInfo(view).name);
        } else if (!context.renderer.isDlssPresented()) {
            ImGui::Text("No lights: unlit albedo");
        }
    }
    ImGui::End();
}

void EngineUi::drawLoadPanel()
{
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

    bool shouldLoad = false;
    constexpr float kVisibleRows = 8.0f;
    if (discoveredAssets.empty()) {
        ImGui::TextDisabled("No model folders with a .gltf/.glb found");
    } else if (ImGui::BeginListBox("##models", {-FLT_MIN, kVisibleRows * ImGui::GetTextLineHeightWithSpacing()})) {
        for (std::size_t i = 0; i < discoveredAssets.size(); ++i) {
            const bool isSelected = selectedAssetIndex == static_cast<int>(i);
            const std::string label = discoveredAssets[i].filename().string();
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(label.c_str(), isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
                selectedAssetIndex = static_cast<int>(i);
                shouldLoad = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
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
        shouldLoad = true;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Double-click a model to load it directly");
    if (shouldLoad && hasSelection) {
        requests.load = UiRequests::Load{.asset = discoveredAssets[static_cast<std::size_t>(selectedAssetIndex)],
                                         .position = glm::make_vec3(loadedModelPosition.data())};
    }
    if (!loadStatus.empty()) {
        constexpr ImVec4 kErrorColor{1.0f, 0.4f, 0.4f, 1.0f};
        ImGui::SameLine();
        if (isLoadStatusError) {
            ImGui::TextColored(kErrorColor, "%s", loadStatus.c_str());
        } else {
            ImGui::TextUnformatted(loadStatus.c_str());
        }
    }
}

void EngineUi::drawLoadedModelsPanel()
{
    const ObjectStorage& storage = context.scene.objectStorage;
    // ### keeps the header id stable while the count changes
    const std::string header = std::format("Loaded models ({})###loaded", storage.size());
    if (!ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    const auto [gpuUsedBytes, gpuCapacityBytes] = context.assets.assetTotals();
    ImGui::Text("GPU geometry %.1f MiB, %u textures", toMiB(gpuUsedBytes), context.textures.size());
    if (storage.empty()) {
        ImGui::TextDisabled("Nothing loaded");
        return;
    }

    constexpr int kColumnCount = 8;
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
    ImGui::TableSetupColumn("OMM");
    ImGui::TableSetupColumn("##unload");
    ImGui::TableHeadersRow();

    // instance fields reach thousands of rows; only the visible ones are built
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(storage.size()));
    while (clipper.Step()) {
        for (auto id = static_cast<EntityId>(clipper.DisplayStart); id < static_cast<EntityId>(clipper.DisplayEnd);
             ++id) {
            ImGui::PushID(static_cast<int>(id));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(std::filesystem::path(storage.names[id]).filename().string().c_str());
            ImGui::SetItemTooltip("%s", storage.names[id].c_str());
            const ModelId modelId = storage.modelIds[id];
            const std::span<const ModelAllocation> models = context.assets.models();
            const bool hasModel = modelId < models.size();
            const ModelAllocation* model = hasModel ? &models[modelId] : nullptr;
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
                ImGui::Text("%.1f MiB x%u", toMiB(context.assets.modelGpuBytes(modelId)), model->refCount);
                ImGui::SetItemTooltip("geometry shared by %u entities loaded from the same file", model->refCount);
            } else {
                ImGui::Text("%.1f MiB", toMiB(hasModel ? context.assets.modelGpuBytes(modelId) : 0));
            }
            ImGui::TableNextColumn();
            if (model != nullptr && model->omm.primitiveCount > 0) {
                const OmmBakeSummary& omm = model->omm;
                ImGui::Text("%u", omm.primitiveCount);
                ImGui::SetItemTooltip("alpha-tested prims baked in %.1f ms\nmicro-triangles opaque %llu, transparent "
                                      "%llu, unknown %llu\nmin known area %.3f\narray %.1f KiB, index %.1f KiB",
                                      omm.bakeMilliseconds, static_cast<unsigned long long>(omm.opaque),
                                      static_cast<unsigned long long>(omm.transparent),
                                      static_cast<unsigned long long>(omm.unknown), omm.minKnownAreaMetric,
                                      static_cast<double>(omm.arrayBytes) / 1024.0,
                                      static_cast<double>(omm.indexBytes) / 1024.0);
            } else {
                ImGui::TextDisabled("-");
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Unload")) {
                requests.unload = id;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void EngineUi::drawDebugViewPanel()
{
    if (!ImGui::CollapsingHeader("Debug view")) {
        return;
    }
    DebugViewSettings& settings = context.renderer.debugView;

    if (ImGui::BeginCombo("View", debugViewInfo(settings.view).name)) {
        for (size_t i = 0; i < kDebugViews.size(); ++i) {
            const auto view = static_cast<DebugView>(i);
            const DebugViewInfo& info = kDebugViews[i];
            if (i > 0 && info.category != kDebugViews[i - 1].category) {
                ImGui::SeparatorText(debugViewCategoryName(info.category));
            }
            const bool isSelected = view == settings.view;
            // a paused cache answers no queries
            const bool isPausedCache = view == DebugView::NrcCache && context.renderer.isNrcPaused();
            ImGui::BeginDisabled(!info.isImplemented || isPausedCache);
            if (ImGui::Selectable(info.name, isSelected)) {
                settings.view = view;
            }
            ImGui::EndDisabled();
            if (!info.isImplemented) {
                ImGui::SetItemTooltip("reserved, not implemented yet");
            } else if (isPausedCache) {
                ImGui::SetItemTooltip("NRC is paused: ReSTIR GI mode or ground-truth paths");
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    if (settings.view != DebugView::HiZPyramid) {
        ImGui::Checkbox("Triangle edges", &settings.showEdges);
        ImGui::SetItemTooltip("wireframe from the resolve's analytic barycentrics");
    }

    // per-category options; lighting and upscaling views add theirs here
    switch (debugViewInfo(settings.view).category) {
    case DebugViewCategory::Lit:
        ImGui::TextDisabled("no lighting before M3: the diffuse albedo guide");
        break;
    case DebugViewCategory::Geometry:
        ImGui::TextDisabled("meshlet colors are per model: instances match");
        break;
    case DebugViewCategory::Culling:
        if (settings.view == DebugView::HiZPyramid) {
            const int lastMip = static_cast<int>(context.renderer.getHiZMipCount()) - 1;
            int mip = std::min(static_cast<int>(settings.hizMip), lastMip);
            if (ImGui::SliderInt("Mip", &mip, 0, lastMip)) {
                settings.hizMip = static_cast<uint32_t>(mip);
            }
            ImGui::TextDisabled("pyramid of the final depth: what the next frame's early phase tests against");
        } else {
            ImGui::TextDisabled("blue: drawn early; orange: recovered by the late phase");
        }
        break;
    case DebugViewCategory::Guides:
        if (settings.view == DebugView::MotionVectors) {
            ImGui::TextDisabled("|x| red, |y| green, 16 px saturates");
            drawEntityTransformControls();
        } else if (settings.view == DebugView::Reprojection) {
            ImGui::TextDisabled("albedo error x4: black except disocclusions");
            drawEntityTransformControls();
        }
        break;
    case DebugViewCategory::RayTracing:
        ImGui::TextDisabled("primary rays through the TLAS: match the raster view away from edges");
        break;
    case DebugViewCategory::Lighting:
        if (settings.view == DebugView::DirectLighting) {
            ImGui::Checkbox("ReSTIR DI reference", &context.renderer.getRtxdi().isReference);
            ImGui::SetItemTooltip("no reuse, more initial candidates: the unbiased ground truth of the reuse passes");
            ImGui::TextDisabled("converges to Shadowed lights");
        } else if (settings.view == DebugView::NrcCache && context.renderer.isNrcPaused()) {
            ImGui::TextDisabled("NRC is paused: black, not the cache");
        }
        break;
    case DebugViewCategory::Upscaling:
        break;
    }
}

// moves an entity by hand: the motion test cases of spec/features/motion_vectors.md §5
void EngineUi::drawEntityTransformControls()
{
    ObjectStorage& storage = context.scene.objectStorage;
    if (storage.empty()) {
        return;
    }
    const int lastEntity = static_cast<int>(storage.size()) - 1;
    transformTestEntity = std::min(transformTestEntity, lastEntity);
    ImGui::SliderInt("Entity", &transformTestEntity, 0, lastEntity);
    Transform& transform = storage.transforms[static_cast<EntityId>(transformTestEntity)];
    ImGui::DragFloat3("Position", &transform.position.x, 0.01f);
    ImGui::DragFloat3("Rotation", &transform.rotation.x, 0.01f);
    ImGui::SetItemTooltip("radians");
    ImGui::DragFloat3("Scale", &transform.scale.x, 0.01f);
    ImGui::SetItemTooltip("a negative axis mirrors the instance");
}

void EngineUi::drawCameraPanel()
{
    if (!ImGui::CollapsingHeader("Camera")) {
        return;
    }
    const glm::vec3& position = context.camera.cameraData.cameraPos;
    ImGui::Text("Position: %.2f, %.2f, %.2f", position.x, position.y, position.z);
    ImGui::InputFloat3("Coordinates", teleportPosition.data());
    if (ImGui::Button("Teleport")) {
        context.camera.teleportTo(glm::make_vec3(teleportPosition.data()));
    }
    ImGui::Checkbox("Jitter", &context.camera.isJitterEnabled);
    ImGui::SetItemTooltip("Halton(2,3) subpixel jitter, %u phases. Only on frames DLSS-RR resolves",
                          context.camera.getJitterPhaseCount());
}

void EngineUi::drawCullingPanel()
{
    if (!ImGui::CollapsingHeader("Culling", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    MeshletCulling& culling = context.renderer.getCulling();
    MeshletCullSettings& settings = culling.settings;

    ImGui::Checkbox("Instance frustum", &settings.isInstanceFrustumEnabled);
    ImGui::SetItemTooltip("node-instance sphere test before any meshlet is tested");
    ImGui::SameLine();
    ImGui::Checkbox("Frustum", &settings.isFrustumEnabled);
    ImGui::SetItemTooltip("meshlet sphere test");
    ImGui::SameLine();
    ImGui::Checkbox("Normal cone", &settings.isConeEnabled);
    ImGui::SetItemTooltip("meshlet backface test; double-sided materials skip it");
    ImGui::SameLine();
    ImGui::Checkbox("Sub-pixel", &settings.isSubPixelEnabled);
    ImGui::SetItemTooltip("instances and meshlets whose conservative screen rect holds no pixel center; lossless");
    ImGui::Checkbox("Counters", &settings.isStatsEnabled);
    ImGui::SetItemTooltip("per-test counters cost a few wave atomics per meshlet group");
    ImGui::SameLine();
    ImGui::Checkbox("Occlusion", &settings.isOcclusionEnabled);
    ImGui::SetItemTooltip("hi-z test of instances and meshlets against the previous (early) or fresh (late) pyramid");
    ImGui::SameLine();
    ImGui::Checkbox("Freeze culling camera", &settings.isCameraFrozen);
    ImGui::SetItemTooltip("cull against the view at the moment of freezing; fly out to inspect the result. "
                          "occlusion keeps testing against that frame's pyramid, which is not rebuilt while frozen");
    ImGui::Checkbox("Triangle backface", &settings.isTriangleBackfaceEnabled);
    ImGui::SetItemTooltip("per-triangle orientation test in the mesh shader, also for triangles crossing the near plane; "
                          "double-sided materials skip it");
    ImGui::SameLine();
    ImGui::Checkbox("Small triangles", &settings.isSmallTriangleEnabled);
    ImGui::SetItemTooltip("per-triangle test in the mesh shader: culls triangles whose bounds miss every pixel center");

    int perSequence = static_cast<int>(settings.maxMeshletsPerSequence);
    constexpr int kPerSequenceStep = 16;
    constexpr int kPerSequenceFastStep = 256;
    if (ImGui::InputInt("Meshlets / sequence", &perSequence, kPerSequenceStep, kPerSequenceFastStep)) {
        settings.maxMeshletsPerSequence = static_cast<uint32_t>(std::max(perSequence, 0));
    }
    ImGui::SetItemTooltip("0 = device limit. Small values force many DGC sequences per execute "
                          "(driver 616.92 dropped sequences past 96 in one execute)");

    const MeshletCullStats& stats = culling.getStats();
    const GpuCullCounters& early = stats.phase(CullPhase::Early);
    const GpuCullCounters& late = stats.phase(CullPhase::Late);
    const uint32_t earlyVisible = visibleMeshletCount(early);
    const uint32_t lateVisible = visibleMeshletCount(late);

    ImGui::Text("Occlusion history: %s%s, fallback frames %u", occlusionHistoryName(stats.history),
                stats.isPyramidFrozen ? " (pyramid frozen)" : "", stats.fallbackFrames);
    ImGui::SetItemTooltip("without history the early phase skips occlusion and the late phase gets no candidates");
    if (const uint32_t overflowed = overflowedCount(early) + overflowedCount(late); overflowed > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "GPU lists overflowed: %u appends dropped", overflowed);
        ImGui::SetItemTooltip("a culling list is sized too small; see the dropped rows below");
    }

    // rows: early, late, frame total (empty cells where a test does not run in that phase)
    constexpr int64_t kNone = -1;
    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    const auto cell = [](int64_t value) {
        ImGui::TableNextColumn();
        if (value != kNone) {
            ImGui::Text("%lld", static_cast<long long>(value));
        }
    };
    const auto row = [&](const char* label, int64_t earlyValue, int64_t lateValue, int64_t total,
                         const char* tooltip = nullptr) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(label);
        if (tooltip != nullptr) {
            ImGui::SetItemTooltip("%s", tooltip);
        }
        cell(earlyValue);
        cell(lateValue);
        cell(total);
    };
    const auto sum = [](uint32_t lhs, uint32_t rhs) { return static_cast<int64_t>(lhs) + rhs; };

    if (ImGui::BeginTable("##cullInstances", 4, kTableFlags)) {
        ImGui::TableSetupColumn("Instances");
        ImGui::TableSetupColumn("Early");
        ImGui::TableSetupColumn("Late");
        ImGui::TableSetupColumn("Frame");
        ImGui::TableHeadersRow();
        row("Tested", stats.instances, late.instanceDispatch[0], stats.instances,
            "late: candidates occluded early, re-tested against the fresh pyramid");
        if (settings.isStatsEnabled) {
            row("Frustum culled", early.instancesCulled, kNone, early.instancesCulled);
            row("Sub-pixel culled", early.instancesSubPixel, kNone, early.instancesSubPixel);
            row("Occluded", early.instancesOccluded, late.instancesOccluded, late.instancesOccluded,
                "early: became candidates; frame: still occluded after the late re-test");
            row("Drawn", early.instancesDrawn, late.instancesDrawn, sum(early.instancesDrawn, late.instancesDrawn));
        }
        row("Draw records", early.meshletDispatch[0], late.meshletDispatch[0],
            sum(early.meshletDispatch[0], late.meshletDispatch[0]));
        row("Candidates dropped", early.candidatesOverflowed, kNone, early.candidatesOverflowed,
            "appends past the candidate list's capacity; nonzero is a sizing bug");
        row("Draw records dropped", early.drawRecordsOverflowed, late.drawRecordsOverflowed,
            sum(early.drawRecordsOverflowed, late.drawRecordsOverflowed),
            "appends past the phase's draw-record region; nonzero is a sizing bug");
        ImGui::EndTable();
    }

    if (ImGui::BeginTable("##cullMeshlets", 4, kTableFlags)) {
        ImGui::TableSetupColumn("Meshlets");
        ImGui::TableSetupColumn("Early");
        ImGui::TableSetupColumn("Late");
        ImGui::TableSetupColumn("Frame");
        ImGui::TableHeadersRow();
        row("Total", kNone, kNone, stats.totalMeshlets);
        if (settings.isStatsEnabled) {
            row("Tested", early.testedMeshlets, late.testedMeshlets, sum(early.testedMeshlets, late.testedMeshlets),
                "meshlets of drawn instances; late: of recovered instances");
            row("Groups tested", early.groupsTested, late.groupsTested, sum(early.groupsTested, late.groupsTested),
                "meshlet groups (draw records) whose box was tested before their meshlets");
            row("Groups culled whole", early.groupsCulled, late.groupsCulled,
                sum(early.groupsCulled, late.groupsCulled),
                "the group box alone was outside, sub-pixel or occluded; its meshlets count in the rows below");
            row("Rejected re-tested", kNone, late.meshletsRetested, kNone,
                "reject-mask bits of early records: meshlets that failed only occlusion; only it is redone");
            row("Frustum culled", early.frustumCulled, late.frustumCulled, sum(early.frustumCulled, late.frustumCulled));
            row("Cone culled", early.coneCulled, late.coneCulled, sum(early.coneCulled, late.coneCulled));
            row("Sub-pixel culled", early.meshletsSubPixel, late.meshletsSubPixel,
                sum(early.meshletsSubPixel, late.meshletsSubPixel));
            row("Occluded", early.occlusionCulled, late.occlusionCulled, late.occlusionCulled,
                "early: set in the reject masks; frame: still occluded after the late re-test");
        }
        row("Visible", earlyVisible, lateVisible, sum(earlyVisible, lateVisible));
        row("Visible dropped", early.visibleOverflowed, late.visibleOverflowed,
            sum(early.visibleOverflowed, late.visibleOverflowed),
            "appends past a raster bin's visible list; nonzero is a sizing bug");
        for (const RasterBin group : {RasterBin::Opaque, RasterBin::AlphaTested}) {
            const auto index = std::to_underlying(group);
            row(group == RasterBin::Opaque ? "  opaque" : "  alpha-tested",
                early.visibleMeshlets[index], late.visibleMeshlets[index],
                sum(early.visibleMeshlets[index], late.visibleMeshlets[index]));
        }
        for (const RasterBin group : {RasterBin::Opaque, RasterBin::AlphaTested}) {
            const auto index = std::to_underlying(group);
            row(group == RasterBin::Opaque ? "DGC sequences opaque" : "DGC sequences alpha-tested",
                early.sequenceCount[index], late.sequenceCount[index],
                sum(early.sequenceCount[index], late.sequenceCount[index]));
        }
        ImGui::EndTable();
    }
    const bool isTriangleCulling = settings.isTriangleBackfaceEnabled || settings.isSmallTriangleEnabled;
    if (settings.isStatsEnabled && isTriangleCulling && ImGui::BeginTable("##cullTriangles", 4, kTableFlags)) {
        ImGui::TableSetupColumn("Triangles");
        ImGui::TableSetupColumn("Early");
        ImGui::TableSetupColumn("Late");
        ImGui::TableSetupColumn("Frame");
        ImGui::TableHeadersRow();
        row("Tested", early.trianglesTested, late.trianglesTested, sum(early.trianglesTested, late.trianglesTested),
            "triangles of visible meshlets");
        row("Backface culled", early.trianglesBackface, late.trianglesBackface,
            sum(early.trianglesBackface, late.trianglesBackface));
        row("Small culled", early.trianglesSmall, late.trianglesSmall, sum(early.trianglesSmall, late.trianglesSmall),
            "front-facing triangles that cover no pixel center");
        ImGui::EndTable();
    }
    if (stats.totalMeshlets > 0) {
        ImGui::Text("Drawn %.1f%% of meshlets, %u per sequence",
                    100.0f * static_cast<float>(earlyVisible + lateVisible) / static_cast<float>(stats.totalMeshlets),
                    stats.meshletsPerSequence);
    }

    ImGui::SeparatorText("Timings");
    ImGui::Text("CPU prepare %.3f ms", stats.cpuPrepareMs);
    ImGui::SetItemTooltip("cull view every frame; draw record templates only when the scene layout changes");
    const GpuTimer& timer = context.renderer.getGpuTimer();
    if (!timer.isSupported()) {
        ImGui::TextDisabled("GPU timestamps not supported on the graphics queue");
        return;
    }
    float gpuTotalMs = 0.0f;
    for (size_t i = 0; i < kGpuTimerScopeCount; ++i) {
        const auto scope = static_cast<GpuTimerScope>(i);
        if (scope != GpuTimerScope::AcquireWait) {
            gpuTotalMs += timer.averageMs(scope);
        }
    }
    ImGui::Text("GPU total %.3f ms (rolling %zu frames)", gpuTotalMs, GpuTimer::kHistorySize);
    ImGui::SetItemTooltip("sum of the scopes below except Acquire wait, which is idle time until the swapchain "
                          "image is available");
    const RayTracingScene::Stats rt = context.renderer.getRayTracingStats();
    ImGui::Text("Async compute: RT instances %.3f ms, PTLAS build %.3f ms", rt.instanceWriteMs, rt.tlasBuildMs);
    ImGui::SetItemTooltip("compute queue, overlapping the frame above; not in the total");
    ImGui::Text("PTLAS instances %u / capacity %u, %u partitions, %u written", rt.instances, rt.capacity,
                rt.partitions, rt.writtenInstances);
    ImGui::SetItemTooltip("one per node instance x primitive with meshlets; the count is exact, so it cannot "
                          "overflow, and the capacity grows with the scene layout. Written: instance updates of the "
                          "last completed frame, 0 while nothing moves");
    constexpr float kPlotHeight = 32.0f;
    for (size_t i = 0; i < kGpuTimerScopeCount; ++i) {
        const auto scope = static_cast<GpuTimerScope>(i);
        const std::string overlay = std::format("{} {:.3f} ms", gpuTimerScopeName(scope), timer.averageMs(scope));
        const auto& samples = timer.history(scope);
        ImGui::PlotLines(std::format("##gpuTimer{}", i).c_str(), samples.data(), static_cast<int>(samples.size()),
                         timer.historyOffset(), overlay.c_str(), 0.0f, FLT_MAX, {-FLT_MIN, kPlotHeight});
    }
}

// runtime controls of what is built: RTX Mega Geometry (PTLAS), RTXDI and DLSS-RR. RTXGI (NRC, ReSTIR GI) has no pass
// yet
void EngineUi::drawRayTracingPanel()
{
    if (!ImGui::CollapsingHeader("Ray tracing")) {
        return;
    }
    const auto sliderU32 = [](const char* label, uint32_t& value, uint32_t minimum, uint32_t maximum) {
        ImGui::SliderScalar(label, ImGuiDataType_U32, &value, &minimum, &maximum, "%u", ImGuiSliderFlags_AlwaysClamp);
    };

    ImGui::SeparatorText("RTX Mega Geometry");
    RayTracingScene::Settings& ptlas = context.renderer.getRayTracingScene().settings;
    const RayTracingScene::Stats rt = context.renderer.getRayTracingStats();
    ImGui::Text("PTLAS: %u instances in %u partitions, %u written, update %.3f ms", rt.instances, rt.partitions,
                rt.writtenInstances, rt.tlasBuildMs);
    ImGui::SetItemTooltip("one CLAS per meshlet and one cluster BLAS per primitive, built at load; the PTLAS is "
                          "updated in place on async compute");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    sliderU32("Instances per partition", ptlas.instancesPerPartition, 1, 1024);
    ImGui::SetItemTooltip("grid density of the PTLAS partitions; a change rebuilds the PTLAS");
    ImGui::Checkbox("Force full PTLAS build", &ptlas.isFullBuildForced);
    ImGui::SetItemTooltip("rewrite every instance into an empty PTLAS each frame, to compare against the "
                          "changed-only update");

    ImGui::SeparatorText("RTXDI (ReSTIR DI)");
    const WorldLights& lights = context.renderer.getWorldLights();
    ImGui::Text("Lights: %u punctual, %u triangle, %u infinite", lights.punctualLocalLightCount(),
                lights.triangleLightCount(), lights.infiniteLightCount());
    ImGui::SetItemTooltip("world light rows: point and spot, one per triangle of every emissive primitive instance "
                          "(RTXDI's local region), then directional and the sun");
    ImGui::Checkbox("Sample triangle lights", &context.renderer.getWorldLights().areTriangleLightsSampled);
    ImGui::SetItemTooltip("off: RTXDI samples point, spot and directional lights only, and rays that hit an emissive "
                          "triangle add its emission instead; resets lighting history");
    RtxdiContext& rtxdi = context.renderer.getRtxdi();
    RtxdiContext::Settings& di = rtxdi.settings;
    ImGui::Checkbox("Reference (no reuse)", &rtxdi.isReference);
    ImGui::SetItemTooltip("no reuse, 32 local + 8 infinite candidates: the unbiased ground truth of the reuse passes");
    ImGui::BeginDisabled(rtxdi.isReference);
    comboEnum("Local light sampling", di.localLightSamplingMode, std::array{"Uniform", "Power RIS", "ReGIR RIS"},
              std::array{ReSTIRDI_LocalLightSamplingMode::Uniform, ReSTIRDI_LocalLightSamplingMode::Power_RIS,
                         ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS});
    ImGui::SetItemTooltip("how point, spot and triangle light candidates are picked, at the primary surface and at "
                          "every path vertex: uniformly, from tiles presampled by power, or from the tiles of the "
                          "world-space cell around the surface (ReGIR)");
    sliderU32("Local light candidates", di.activeLocalLightSamples(), 0, 64);
    ImGui::SetItemTooltip("initial RIS candidates per pixel from point, spot and triangle lights; each sampling mode "
                          "keeps its own count");
    sliderU32("Infinite light candidates", di.infiniteLightSamples, 0, 16);
    ImGui::SetItemTooltip("initial RIS candidates per pixel from directional lights");
    sliderU32("BRDF candidates", di.brdfSamples, 0, 4);
    ImGui::SetItemTooltip("one ray each along a BRDF sample; a hit on an emissive triangle names its light");
    ImGui::EndDisabled();
    ImGui::Checkbox("Initial visibility", &di.isInitialVisibilityEnabled);
    ImGui::SetItemTooltip("one shadow ray per pixel in the initial pass; occluded samples are dropped before reuse");

    ImGui::BeginDisabled(rtxdi.isReference);
    sliderU32("Max history length", di.maxHistoryLength, 1, 64);
    ImGui::SetItemTooltip("frames of temporal reuse: longer is smoother, slower to react to changes");
    comboEnum("Temporal bias correction", di.temporalBiasCorrection, std::array{"Off", "Basic", "Ray traced"},
              std::array{ReSTIRDI_TemporalBiasCorrectionMode::Off, ReSTIRDI_TemporalBiasCorrectionMode::Basic,
                         ReSTIRDI_TemporalBiasCorrectionMode::Raytraced});
    ImGui::SetItemTooltip("Ray traced adds a shadow ray per pixel reusing last frame's sample");
    ImGui::Checkbox("Visibility shortcut", &di.isVisibilityShortcutEnabled);
    ImGui::SetItemTooltip("skip the temporal bias-correction ray for samples tested at their pixel last frame");
    ImGui::SameLine();
    ImGui::Checkbox("Fallback sampling", &di.isFallbackSamplingEnabled);
    ImGui::SetItemTooltip("when no history pixel matches, reuse last frame's reservoir at the same pixel (RTXDI 3.2, "
                          "aimed at screen-edge disocclusions; considerations C-M4-1)");
    ImGui::Checkbox("Permutation sampling", &di.isPermutationSamplingEnabled);
    ImGui::SetItemTooltip("take history from a permuted neighbor pixel: decorrelates frames for the denoiser, raises "
                          "frame-to-frame change (considerations C-F3-3)");

    // the stratified neighbor walk covers quadrants in pairs
    sliderU32("Spatial samples", di.spatialSamples, 0, 16);
    di.spatialSamples &= ~1u;
    ImGui::SetItemTooltip("neighbor reservoirs merged per pixel by the spatial pass (even)");
    sliderU32("Disocclusion boost samples", di.disocclusionBoostSamples, 0, 32);
    di.disocclusionBoostSamples &= ~1u;
    ImGui::SetItemTooltip("spatial samples while a pixel's history is shorter than the boost history length (even)");
    sliderU32("Boost history length", di.disocclusionHistoryLength, 0, 64);
    ImGui::SetItemTooltip("history length below which the disocclusion boost applies; 0 never boosts (RTXDI default)");
    ImGui::SliderFloat("Sampling radius", &di.samplingRadius, 1.0f, 64.0f, "%.0f px", ImGuiSliderFlags_AlwaysClamp);
    comboEnum("Spatial bias correction", di.spatialBiasCorrection, std::array{"Off", "Basic", "Pairwise", "Ray traced"},
              std::array{ReSTIRDI_SpatialBiasCorrectionMode::Off, ReSTIRDI_SpatialBiasCorrectionMode::Basic,
                         ReSTIRDI_SpatialBiasCorrectionMode::Pairwise, ReSTIRDI_SpatialBiasCorrectionMode::Raytraced});
    ImGui::SetItemTooltip("Ray traced adds a shadow ray per merged neighbor");
    ImGui::Checkbox("Material similarity", &di.isMaterialSimilarityTestEnabled);
    ImGui::SetItemTooltip("reject neighbors whose roughness or reflectance differs too much");

    ImGui::SliderFloat("Depth threshold", &di.depthThreshold, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("relative depth difference above which a neighbor or history pixel is rejected");
    ImGui::SliderFloat("Normal threshold", &di.normalThreshold, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("minimum dot product of normals for a neighbor or history pixel to be reused");
    ImGui::Checkbox("Boiling filter", &di.isBoilingFilterEnabled);
    ImGui::SetItemTooltip("suppress reservoirs far brighter than their 8x8 tile, which spread as bright blotches");
    ImGui::BeginDisabled(!di.isBoilingFilterEnabled);
    ImGui::SliderFloat("Boiling strength", &di.boilingFilterStrength, 0.0f, 1.0f, "%.2f",
                       ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
    ImGui::Checkbox("Reuse final visibility", &di.isFinalVisibilityReused);
    ImGui::SetItemTooltip("skip the shading ray for a sample tested at most 4 frames ago within 16 px; stale on "
                          "moving geometry");
    if (ImGui::Button("NVIDIA defaults")) {
        di = RtxdiContext::defaultSettings();
    }
    ImGui::SetItemTooltip("the RTXDI sample's Medium preset");
    ImGui::SameLine();
    if (ImGui::Button("Converged")) {
        di = RtxdiContext::convergedSettings();
    }
    ImGui::SetItemTooltip("the convergence check's configuration: ray-traced bias correction, temporal only, no "
                          "fallback, permutation or boiling filter");
    ImGui::EndDisabled();

    ImGui::SeparatorText("DLSS Ray Reconstruction");
    Dlss& dlss = context.renderer.getDlss();
    const vk::Extent2D render = dlss.getRenderExtent();
    const vk::Extent2D display = dlss.getDisplayExtent();
    ImGui::Text("Render %ux%u -> display %ux%u (%.2fx), %u jitter phases", render.width, render.height, display.width,
                display.height, static_cast<float>(display.width) / static_cast<float>(render.width),
                context.camera.getJitterPhaseCount());
    constexpr std::array kQualityModes{"DLAA", "Quality", "Balanced", "Performance", "Ultra Performance"};
    int quality = static_cast<int>(dlss.settings.quality);
    if (ImGui::Combo("Mode", &quality, kQualityModes.data(), static_cast<int>(kQualityModes.size()))) {
        dlss.settings.quality = static_cast<Dlss::Quality>(quality);
    }
    ImGui::SetItemTooltip("render resolution: DLAA renders at display size. A change recreates the feature and the "
                          "render targets, and resets history");
    constexpr std::array kPresets{"Default", "D (transformer)", "E (latest transformer)", "F (RR2)"};
    int preset = static_cast<int>(dlss.settings.preset);
    if (ImGui::Combo("Preset", &preset, kPresets.data(), static_cast<int>(kPresets.size()))) {
        dlss.settings.preset = static_cast<Dlss::Preset>(preset);
    }
    ImGui::SetItemTooltip("model hint; an NVIDIA app override takes precedence (NGX logs it as a DRS override)");
    if (ImGui::Button("Reset history")) {
        context.renderer.resetDlssHistory();
    }
    ImGui::SetItemTooltip("the next Lit frame starts without accumulated history");
    if (!context.renderer.isDlssPresented()) {
        ImGui::SameLine();
        ImGui::TextDisabled(context.renderer.debugView.view == DebugView::Lit
                                ? "not evaluated: no lights, Lit shows unlit albedo"
                                : "evaluated only in the Lit view");
    }

    if (ImGui::Button("Defaults")) {
        ptlas = {};
        rtxdi.isReference = false;
        di = RtxdiContext::defaultSettings();
        rtxdi.isGiReference = false;
        rtxdi.giSettings = RtxdiContext::defaultGiSettings();
        rtxdi.giReferenceBounces = 0;
        dlss.settings = {};
        context.renderer.getNrc().settings = {};
        context.renderer.indirectMode = IndirectMode::RestirGi;
    }

    ImGui::SeparatorText("Indirect (M4)");
    // spec/m4/g7_indirect_modes.md
    IndirectMode& indirectMode = context.renderer.indirectMode;
    if (ImGui::BeginCombo("Indirect mode", indirectModeInfo(indirectMode).label)) {
        for (size_t i = 0; i < kIndirectModes.size(); ++i) {
            const auto mode = static_cast<IndirectMode>(i);
            if (ImGui::Selectable(kIndirectModes[i].label, mode == indirectMode)) {
                indirectMode = mode;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("ReSTIR GI (one bounce, NRC paused) ships on both tiers, the others are for A/B "
                          "measurement. NRC: the cache samples without reuse. A switch resets ReSTIR GI and DLSS-RR "
                          "history");
    // spec/m4/g3_nrc.md
    NrcCache& nrcCache = context.renderer.getNrc();
    NrcCache::Settings& nrc = nrcCache.settings;
    ImGui::Checkbox("Train NRC", &nrc.isTrainingEnabled);
    ImGui::SetItemTooltip("off freezes the cache and skips the training paths");
    int iterations = static_cast<int>(nrc.trainingIterations);
    if (ImGui::SliderInt("NRC training iterations", &iterations, 1, 4)) {
        nrc.trainingIterations = static_cast<uint32_t>(iterations);
    }
    ImGui::SetItemTooltip("16K training records each; the training paths grow with it");
    const vk::Extent2D training = nrcCache.getTrainingExtent();
    const char* nrcState = !nrcCache.isActive()          ? "no scene"
                           : context.renderer.isNrcPaused() ? "paused"
                                                            : "active";
    ImGui::TextDisabled("%s, training paths %ux%u", nrcState, training.width, training.height);

    // spec/m4/g5_restir_gi.md
    ImGui::SeparatorText("ReSTIR GI");
    RtxdiContext::GiSettings& gi = rtxdi.giSettings;
    bool isGroundTruth = rtxdi.giReferenceBounces > 0;
    if (ImGui::Checkbox("Ground-truth paths", &isGroundTruth)) {
        rtxdi.giReferenceBounces = isGroundTruth ? RtxdiContext::kGiGroundTruthBounces : 0u;
    }
    ImGui::SetItemTooltip("trace the bounces after X2 instead of querying NRC, which pauses: with Reference, the "
                          "ground truth of engine spec section 14 when accumulated");
    ImGui::BeginDisabled(!isGroundTruth);
    sliderU32("Bounces after X2", rtxdi.giReferenceBounces, 1, 16);
    ImGui::EndDisabled();
    ImGui::Checkbox("Reference (no reuse)##gi", &rtxdi.isGiReference);
    ImGui::SetItemTooltip("shade each pixel's initial sample alone: the unbiased estimate the reuse converges to");
    ImGui::BeginDisabled(rtxdi.isGiReference);
    sliderU32("Max history length##gi", gi.maxHistoryLength, 1, 64);
    sliderU32("Max sample age", gi.maxReservoirAge, 1, RtxdiContext::kGiUnlimitedSampleAge);
    ImGui::SetItemTooltip("frames a sample is reused at most; each pixel draws its own limit in [0.5, 1) of this. "
                          "Reacts to lighting changes, biases a static frame bright (considerations C-G5-1); %u "
                          "never discards",
                          RtxdiContext::kGiUnlimitedSampleAge);
    constexpr std::array kGiBiasNames{"Off", "Basic", "Ray traced"};
    constexpr std::array kGiBiasModes{RTXDI_GIBiasCorrectionMode::Off, RTXDI_GIBiasCorrectionMode::Basic,
                                      RTXDI_GIBiasCorrectionMode::Raytraced};
    comboEnum("Temporal bias correction##gi", gi.temporalBiasCorrection, kGiBiasNames, kGiBiasModes);
    ImGui::SetItemTooltip("Ray traced adds a reconnection ray from last frame's surface");
    ImGui::Checkbox("Fallback sampling##gi", &gi.isFallbackSamplingEnabled);
    ImGui::SetItemTooltip("when no history pixel matches, reuse last frame's sample at the same pixel");
    ImGui::SameLine();
    ImGui::Checkbox("Permutation sampling##gi", &gi.isPermutationSamplingEnabled);
    sliderU32("Spatial samples##gi", gi.spatialSamples, 0, 8);
    ImGui::SliderFloat("Sampling radius##gi", &gi.samplingRadius, 1.0f, 64.0f, "%.0f px", ImGuiSliderFlags_AlwaysClamp);
    comboEnum("Spatial bias correction##gi", gi.spatialBiasCorrection, kGiBiasNames, kGiBiasModes);
    ImGui::SetItemTooltip("Ray traced adds a reconnection ray per merged neighbor");
    ImGui::SliderFloat("Depth threshold##gi", &gi.depthThreshold, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SliderFloat("Normal threshold##gi", &gi.normalThreshold, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::Checkbox("Boiling filter##gi", &gi.isBoilingFilterEnabled);
    ImGui::BeginDisabled(!gi.isBoilingFilterEnabled);
    ImGui::SliderFloat("Boiling strength##gi", &gi.boilingFilterStrength, 0.0f, 1.0f, "%.2f",
                       ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
    ImGui::Checkbox("Final visibility", &gi.isFinalVisibilityEnabled);
    ImGui::SetItemTooltip("a reconnection ray from the pixel to its final sample, which may be a neighbor's");
    ImGui::SameLine();
    ImGui::Checkbox("Final MIS", &gi.isFinalMisEnabled);
    ImGui::SetItemTooltip("blend in the pixel's own initial sample where its BRDF lobe is glossy");
    ImGui::Checkbox("Secondary DI resampling", &gi.isSecondaryResamplingEnabled);
    ImGui::SetItemTooltip("X2's light reuses this frame's ReSTIR DI reservoirs where X2 is on screen");
    ImGui::SameLine();
    ImGui::Checkbox("Firefly clamp", &gi.isFireflyClampEnabled);
    ImGui::SetItemTooltip("clamps X2's radiance in display units; biased");
    if (ImGui::Button("Defaults##gi")) {
        gi = RtxdiContext::defaultGiSettings();
    }
    ImGui::SetItemTooltip("shipped: NVIDIA's with ray-traced spatial bias correction");
    ImGui::SameLine();
    if (ImGui::Button("NVIDIA defaults##gi")) {
        gi = RtxdiContext::nvidiaGiSettings();
    }
    ImGui::SetItemTooltip("the RTXDI sample's ReSTIR GI settings (the library defaults)");
    ImGui::SameLine();
    if (ImGui::Button("Converged##gi")) {
        gi = RtxdiContext::convergedGiSettings();
    }
    ImGui::SetItemTooltip("the GI check's configuration: ray-traced bias correction, no fallback, permutation or "
                          "boiling filter");
    ImGui::EndDisabled();
}

// the engine's directional light after the asset lights (PBR plan E3)
void EngineUi::drawSunPanel()
{
    if (!ImGui::CollapsingHeader("Sun")) {
        return;
    }
    WorldLights::Sun& sun = context.renderer.getWorldLights().sun;
    ImGui::Checkbox("Enabled##sun", &sun.isEnabled);
    ImGui::SetItemTooltip("one directional light after the scene's own; toggling it resets lighting history");
    ImGui::SliderFloat("Azimuth", &sun.azimuthDegrees, -180.0f, 180.0f, "%.0f deg", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("from +X toward +Z; Sponza's courtyard runs along X");
    ImGui::SliderFloat("Elevation", &sun.elevationDegrees, 0.0f, 90.0f, "%.0f deg", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SliderFloat("Illuminance", &sun.illuminance, 0.01f, 10000.0f, "%.3g lux",
                       ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("noisyColor is RGBA16F and not pre-exposed: very high values can overflow it");
    ImGui::ColorEdit3("Color##sun", &sun.color.x, ImGuiColorEditFlags_Float);
    if (ImGui::Button("Defaults##sun")) {
        sun = WorldLights::defaultSun();
    }
}

// exposure and tonemap after DLSS-RR (PBR plan H2)
void EngineUi::drawPostPanel()
{
    if (!ImGui::CollapsingHeader("Post")) {
        return;
    }
    PostProcess& post = context.renderer.getPost();
    PostProcess::Settings& settings = post.settings;
    if (!context.renderer.isDlssPresented()) {
        ImGui::TextDisabled(context.renderer.debugView.view == DebugView::Lit
                                ? "inactive: no lights, Lit shows unlit albedo"
                                : "inactive: runs only in the Lit view");
    }
    const GpuExposureReadback& exposure = post.getLastExposure();
    ImGui::Text("Exposure %.3g (%+.2f EV), target %.3g", exposure.exposure,
                exposure.exposure > 0.0f ? std::log2(exposure.exposure) : 0.0f, exposure.targetExposure);
    ImGui::Text("Average luminance %.3g from %u metered pixels", exposure.averageLuminance, exposure.pixels);
    ImGui::Checkbox("Auto exposure", &settings.isAutoExposure);
    ImGui::SetItemTooltip("targets middle grey using the 40th to 95th luminance percentiles, including black pixels");
    if (settings.isAutoExposure) {
        ImGui::SliderFloat("Compensation", &settings.compensationEv, -8.0f, 8.0f, "%+.1f EV",
                           ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat("Light adaptation", &settings.adaptationSpeedUp, 0.1f, 10.0f, "%.1f /s",
                           ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("halves the EV gap in 1 / speed seconds as the scene gets brighter");
        ImGui::SliderFloat("Dark adaptation", &settings.adaptationSpeedDown, 0.1f, 10.0f, "%.1f /s",
                           ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("halves the EV gap in 1 / speed seconds as the scene gets darker");
    } else {
        ImGui::SliderFloat("Exposure", &settings.manualEv, settings.minEv, settings.maxEv, "%+.1f EV",
                           ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("log2 of the scale applied before the tonemap");
    }
    ImGui::DragFloatRange2("Exposure range", &settings.minEv, &settings.maxEv, 0.1f, -32.0f, 32.0f, "%+.1f EV",
                           nullptr, ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::Button("Snap to target")) {
        post.requestSnap();
    }
    ImGui::SameLine();
    if (ImGui::Button("Defaults##post")) {
        settings = {};
    }
    ImGui::TextDisabled("Tonemap: ACES (Hill's fit), applied once");
}

void EngineUi::drawInstanceFieldPanel()
{
    if (!ImGui::CollapsingHeader("Instance field")) {
        return;
    }
    constexpr int kMaxSide = 64;
    ImGui::SliderInt("Grid side", &instanceFieldSide, 1, kMaxSide);
    ImGui::SliderFloat("Spacing", &instanceFieldSpacing, 0.5f, 4.0f, "%.2f x diameter");
    if (ImGui::Button("Spawn")) {
        requests.spawnInstanceField = UiRequests::InstanceField{
            .center = glm::make_vec3(loadedModelPosition.data()),
            .side = instanceFieldSide,
            .spacing = instanceFieldSpacing,
        };
    }
    ImGui::SetItemTooltip("%s grid centered on the load Position; every instance shares one GPU model",
                          INSTANCE_FIELD_MODEL_PATH.filename().string().c_str());
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        requests.shouldClearInstanceField = true;
    }
    ImGui::SameLine();
    ImGui::Text("%d instances", instanceFieldSide * instanceFieldSide);
}

void EngineUi::drawAssetsPanel() const
{
    if (!ImGui::CollapsingHeader("Assets")) {
        return;
    }
    const MaterialStore& materials = context.scene.materialStore;
    const LightStore& lights = context.scene.lightStore;
    const ObjectStorage& storage = context.scene.objectStorage;

    // primitives are stored once per model; every entity of the model draws them
    std::vector<uint32_t> primitiveUses(materials.infos.size(), 0);
    std::vector<uint32_t> entityUses(materials.infos.size(), 0);
    for (const ModelAllocation& model : context.assets.models()) {
        for (uint32_t p = model.primitives.first; p < model.primitives.end(); ++p) {
            const uint32_t materialId = context.scene.geometryStore.primitiveDraws[p].materialId;
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
                if (const std::optional<TextureManager::TextureInfo> texture = context.textures.textureInfo(heapIndex)) {
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
                const bool isSpecularGlossiness = (m.flags & GpuMaterialFlag::SpecularGlossiness) != 0;
                if (isSpecularGlossiness) {
                    propertyRow("Specular / glossiness", "({:.2f}, {:.2f}, {:.2f}) / {:.2f}", m.specularFactor.r,
                                m.specularFactor.g, m.specularFactor.b, m.glossinessFactor);
                } else {
                    propertyRow("Metallic / roughness", "{:.2f} / {:.2f}", m.metallicFactor, m.roughnessFactor);
                }
                propertyRow("Emissive", "({:.2f}, {:.2f}, {:.2f})", m.emissiveFactor.r, m.emissiveFactor.g,
                            m.emissiveFactor.b);
                propertyRow("Normal scale", "{:.2f}", m.normalScale);
                propertyRow("Occlusion strength", "{:.2f}", m.occlusionStrength);
                propertyRow("Alpha", "{}, cutoff {:.2f}{}", alphaModeName(m.flags), m.alphaCutoff,
                            (m.flags & GpuMaterialFlag::DoubleSided) != 0 ? ", double sided" : "");
                textureRow(context.textures, "Albedo", m.baseColorTex);
                textureRow(context.textures, isSpecularGlossiness ? "Specular-glossiness" : "Metallic-roughness",
                           m.metalRoughTex);
                textureRow(context.textures, "Normal", m.normalTex);
                textureRow(context.textures, "Occlusion", m.occlusionTex);
                textureRow(context.textures, "Emissive map", m.emissiveTex);
                drawMaterialExtensions(context.textures, info.extension);
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
                if (light.type != GpuLightType::Directional) {
                    propertyRow("Range", "{}", light.range > 0.0f ? std::format("{:.2f}", light.range) : "infinite");
                }
                if (light.type == GpuLightType::Spot) {
                    propertyRow("Cone inner / outer", "{:.1f} / {:.1f} deg", glm::degrees(light.innerCone),
                                glm::degrees(light.outerCone));
                }
                // GPU rows are model space; the shader applies the owner's model matrix
                const glm::mat4 model =
                    hasOwner ? computeModelMatrix(storage.transforms[light.instanceIndex]) : glm::mat4{1.0f};
                if (light.type != GpuLightType::Directional) {
                    const glm::vec3 position{model * glm::vec4(light.position, 1.0f)};
                    propertyRow("Position", "({:.2f}, {:.2f}, {:.2f})", position.x, position.y, position.z);
                }
                if (light.type != GpuLightType::Point) {
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
}

void EngineUi::scanFolder()
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
            if (extension == ".gltf" || extension == ".glb") {
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
#endif
