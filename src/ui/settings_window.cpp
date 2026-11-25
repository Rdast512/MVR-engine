#include "settings_window.hpp"
#if ENGINE_ENABLE_IMGUI
    #include "render/vk_renderer.hpp"
    #include "scene/vk_camera.hpp"
    #include "ui_color.hpp"

    #include <algorithm>
    #include <array>
    #include <cassert>
    #include <cmath>
    #include <cstdio>
    #include <cstring>
    #include <format>
    #include <string>
    #include <utility>

namespace
{
    // UserInterface.cpp c_ColorRegularHeader / c_ColorAttentionHeader (ABGR), and the highlight outside both
    constexpr ImU32 kColorRegularHeader = 0xffff8080;
    constexpr ImU32 kColorAttentionHeader = 0xff80ffff;
    constexpr ImU32 kColorDiffering = 0xff40a0ff;
    // buildUI: window at (10, 10), minimum width 375, item width 100
    constexpr ImVec2 kWindowPos{10.0f, 10.0f};
    constexpr float kWindowMinWidth = 375.0f;
    constexpr float kWindowMargin = 20.0f;
    constexpr float kItemWidth = 100.0f;
    // NVIDIA's widths for the checkbox + slider pairs, the resampling mode combos and the frame limit
    constexpr float kPairedItemWidth = 69.0f;
    constexpr float kModeComboWidth = 180.0f;
    constexpr float kFrameLimitWidth = 50.0f;
    constexpr float kToneMappingIndent = 160.0f;
    constexpr float kHelpWrapWidth = 500.0f;
    constexpr const char* kNotInEngine = "(not in engine)";
    // the engine's DI and GI spatial walks and the frame limit
    constexpr uint32_t kMaxDiSpatialSamples = 32;
    constexpr uint32_t kMaxAccumulationFrames = 1024;
    // shaders/lighting/path_shading.slang secondaryResamplingParams: the RTXDI sample's Ultra secondary-surface reuse
    constexpr const char* kSecondaryBiasCorrection = "Ray Traced";
    constexpr uint32_t kSecondarySamples = 2;
    constexpr float kSecondaryRadius = 4.0f;
    constexpr float kSecondaryDepthThreshold = 0.1f;
    constexpr float kSecondaryNormalThreshold = 0.9f;
    // GiFlag::SecondaryRoughnessFloor (rtxdi_gi_shade)
    constexpr float kSecondaryRoughnessFloor = 0.5f;

    // NVIDIA's colors are sRGB-authored like ImGui's style, which the engine stores linear
    [[nodiscard]] ImVec4 headerColor(ImU32 color) { return srgbToLinear(ImGui::ColorConvertU32ToFloat4(color)); }

    // UserInterface.cpp ShowHelpMarker
    void helpMarker(const char* text)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(kHelpWrapWidth);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    // a row NVIDIA edits and the engine fixes: the engine's value, disabled
    void fixedText(const char* label, const std::string& value, const char* help = nullptr)
    {
        ImGui::BeginDisabled();
        ImGui::LabelText(label, "%s", value.c_str());
        ImGui::EndDisabled();
        if (help != nullptr) {
            helpMarker(help);
        }
    }

    void fixedCheckbox(const char* label, bool value, const char* help = nullptr)
    {
        ImGui::BeginDisabled();
        ImGui::Checkbox(label, &value);
        ImGui::EndDisabled();
        if (help != nullptr) {
            helpMarker(help);
        }
    }

    void notInEngine(const char* label, const char* help = nullptr) { fixedText(label, kNotInEngine, help); }

    bool sliderU32(const char* label, uint32_t& value, uint32_t minimum, uint32_t maximum)
    {
        return ImGui::SliderScalar(label, ImGuiDataType_U32, &value, &minimum, &maximum, "%u",
                                   ImGuiSliderFlags_AlwaysClamp);
    }

    bool sliderFloat(const char* label, float& value, float minimum, float maximum, const char* format = "%.3f")
    {
        return ImGui::SliderFloat(label, &value, minimum, maximum, format, ImGuiSliderFlags_AlwaysClamp);
    }

    // enum values need not be contiguous (RTXDI's temporal bias modes skip Pairwise)
    template <typename Enum, size_t Count>
    bool comboEnum(const char* label, Enum& value, const std::array<const char*, Count>& names,
                   const std::array<Enum, Count>& values)
    {
        int index = static_cast<int>(std::ranges::find(values, value) - values.begin());
        if (!ImGui::Combo(label, &index, names.data(), static_cast<int>(Count))) {
            return false;
        }
        value = values[static_cast<size_t>(index)];
        return true;
    }

    // UserInterface.cpp EditDISpatialSampleCount for the stratified walk: one step selects two samples, the slider
    // shows samples. 0 stays reachable (NVIDIA's minimum is 2): the converged set has no spatial neighbors
    bool editSpatialSampleCount(const char* label, uint32_t& count, uint32_t maxSamples)
    {
        int pairs = static_cast<int>(count / 2);
        const float sliderWidth = ImGui::CalcItemWidth();
        const bool isChanged =
            ImGui::SliderInt(label, &pairs, 0, static_cast<int>(maxSamples / 2), "", ImGuiSliderFlags_NoInput);
        count = static_cast<uint32_t>(pairs) * 2;
        if (ImGui::IsItemVisible()) {
            const std::string text = std::format("{}", count);
            const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
            const ImVec2 frameMin = ImGui::GetItemRectMin();
            const ImVec2 frameMax(frameMin.x + sliderWidth, ImGui::GetItemRectMax().y);
            const ImVec2 position(frameMin.x + (sliderWidth - textSize.x) * 0.5f,
                                  frameMin.y + (frameMax.y - frameMin.y - textSize.y) * 0.5f);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->PushClipRect(frameMin, frameMax, true);
            drawList->AddText(position, ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
            drawList->PopClipRect();
        }
        return isChanged;
    }

    constexpr const char* kSpatialSampleHelp =
        "Standard low discrepancy sampling allows increments of 1 sample but poorer cache coherency. Stratified low "
        "discrepancy sampling allows increments of 2 samples but greater cache coherency. The sampling pattern is "
        "chosen in the DI Spatial Resampling settings.\n\nEngine: always stratified; 0 is allowed, the Converged set "
        "takes no spatial neighbors.";


    using Preset = RtxdiContext::QualityPreset;

    // NVIDIA's frame for its presets: no NRC in the sample, native rendering (§5)
    [[nodiscard]] LabConfig nvidiaConfig(Preset preset)
    {
        LabConfig config;
        config.di = RtxdiContext::nvidiaPreset(preset);
        config.isDiReference = preset == Preset::Reference;
        config.gi = RtxdiContext::nvidiaGiSettings();
        config.indirectMode = IndirectMode::RestirGi;
        config.dlss.quality = Dlss::Quality::Dlaa;
        return config;
    }
} // namespace

void SettingsWindow::draw()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(kWindowPos, ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints({kWindowMinWidth, 0.0f},
                                        {viewport->WorkSize.x - kWindowMargin, viewport->WorkSize.y - kWindowMargin});
    if (ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushItemWidth(kItemWidth);
        drawCompareWith();
        drawScene();
        drawGeneralRendering();
        drawStaticContext();
        drawDirectLighting();
        drawIndirectLighting();
        drawDenoising();
        drawPostProcessing();
        drawConfig();
        ImGui::PopItemWidth();
    }
    ImGui::End();
    assert(openNodes.empty());
}

void SettingsWindow::drawCompareWith()
{
    constexpr std::array<const char*, std::to_underlying(CompareEntry::Count)> kLabels{
        "None",         "Shipped",          "NVIDIA Fast", "NVIDIA Medium", "NVIDIA Unbiased",
        "NVIDIA Ultra", "NVIDIA Reference", "Converged"};
    int entry = std::to_underlying(compareEntry);
    ImGui::SetNextItemWidth(kModeComboWidth);
    if (ImGui::Combo("Compare with", &entry, kLabels.data(), static_cast<int>(kLabels.size()))) {
        compareEntry = static_cast<CompareEntry>(entry);
    }
    helpMarker("Rows whose setting differs from this configuration are drawn in orange; their (?) starts with the "
               "configuration's value, and a section or node counts its differing rows after its label. NVIDIA's "
               "entries take the RTXDI sample's preset with its ReSTIR GI defaults, ReSTIR GI indirect mode (the "
               "sample has no NRC) and DLAA (it renders native).");
    ImGui::Separator();

    switch (compareEntry) {
    case CompareEntry::None:
    case CompareEntry::Shipped:
    case CompareEntry::Count:
        comparison = LabConfig{};
        break;
    case CompareEntry::NvidiaFast:
        comparison = nvidiaConfig(Preset::Fast);
        break;
    case CompareEntry::NvidiaMedium:
        comparison = nvidiaConfig(Preset::Medium);
        break;
    case CompareEntry::NvidiaUnbiased:
        comparison = nvidiaConfig(Preset::Unbiased);
        break;
    case CompareEntry::NvidiaUltra:
        comparison = nvidiaConfig(Preset::Ultra);
        break;
    case CompareEntry::NvidiaReference:
        comparison = nvidiaConfig(Preset::Reference);
        break;
    case CompareEntry::Converged:
        comparison = LabConfig{};
        comparison.di = RtxdiContext::convergedSettings();
        comparison.gi = RtxdiContext::convergedGiSettings();
        break;
    }
    differing.clear();
    if (compareEntry != CompareEntry::None) {
        for (const LabSwitchValue& difference : differingSwitches(comparison, captureLabConfig(renderer, camera))) {
            differing.push_back(difference.entry);
        }
    }
}

const LabSwitch* SettingsWindow::differingSwitch(const char* name) const
{
    for (const LabSwitch* entry : differing) {
        if (std::strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return nullptr;
}

uint32_t SettingsWindow::countDiffering(SettingsRows rows, std::initializer_list<SettingsRows> nested) const
{
    // distinct switches: the shared DI and GI threshold pairs have a row in two nodes
    std::vector<const LabSwitch*> found;
    const auto add = [&](SettingsRows list)
    {
        for (const SettingsRow& row : list) {
            const LabSwitch* entry = (row.isAdvanced && !isAdvancedShown) ? nullptr : differingSwitch(row.switchName);
            if (entry != nullptr && std::ranges::find(found, entry) == found.end()) {
                found.push_back(entry);
            }
        }
    };
    add(rows);
    for (const SettingsRows list : nested) {
        add(list);
    }
    return static_cast<uint32_t>(found.size());
}

bool SettingsWindow::beginNode(const char* label, SettingsRows rows, std::initializer_list<SettingsRows> nested,
                               std::optional<ImU32> color, ImGuiTreeNodeFlags flags)
{
    const uint32_t count = countDiffering(rows, nested);
    // ### keeps the id stable while the count changes
    const std::string text =
        count > 0 ? std::format("{} · {}###{}", label, count, label) : std::format("{}###{}", label, label);
    if (color) {
        ImGui::PushStyleColor(ImGuiCol_Text, headerColor(*color));
    }
    const bool isOpen = ImGui::TreeNodeEx(text.c_str(), flags);
    if (color) {
        ImGui::PopStyleColor();
    }
    if (isOpen) {
        openNodes.push_back(rows);
    }
    return isOpen;
}

void SettingsWindow::endNode()
{
    openNodes.pop_back();
    ImGui::TreePop();
}

void SettingsWindow::beginRow(std::initializer_list<const char*> switches)
{
    rowDiffering.clear();
    for (const char* name : switches) {
        // every edited row is listed in its node (settings_rows.hpp), so closed nodes count it
        assert(!openNodes.empty() &&
               std::ranges::any_of(openNodes.back(),
                                   [name](const SettingsRow& row) { return std::strcmp(row.switchName, name) == 0; }));
        if (const LabSwitch* entry = differingSwitch(name)) {
            rowDiffering.push_back(entry);
        }
    }
    isRowHighlighted = !rowDiffering.empty();
    if (isRowHighlighted) {
        ImGui::PushStyleColor(ImGuiCol_Text, headerColor(kColorDiffering));
    }
}

void SettingsWindow::endRow(const char* help)
{
    if (!isRowHighlighted) {
        if (help != nullptr) {
            helpMarker(help);
        }
        return;
    }
    ImGui::PopStyleColor();
    constexpr std::array<const char*, std::to_underlying(CompareEntry::Count)> kNames{
        "None",         "Shipped",          "NVIDIA Fast", "NVIDIA Medium", "NVIDIA Unbiased",
        "NVIDIA Ultra", "NVIDIA Reference", "Converged"};
    std::string text;
    for (const LabSwitch* entry : rowDiffering) {
        text += std::format("{}: {} = {}\n", kNames[std::to_underlying(compareEntry)], entry->name,
                            formatSwitchValue(*entry, entry->get(comparison)));
    }
    if (help != nullptr) {
        text += '\n';
        text += help;
    }
    helpMarker(text.c_str());
    isRowHighlighted = false;
}

SettingsWindow::DiPreset SettingsWindow::currentDiPreset() const
{
    const RtxdiContext& rtxdi = renderer.getRtxdi();
    if (rtxdi.isSpatialOnly || rtxdi.isTemporalOnly) {
        return DiPreset::Custom;
    }
    // the engine's sets first: an untouched engine shows Shipped
    if (!rtxdi.isReference && rtxdi.settings == RtxdiContext::defaultSettings()) {
        return DiPreset::Shipped;
    }
    if (!rtxdi.isReference && rtxdi.settings == RtxdiContext::convergedSettings()) {
        return DiPreset::Converged;
    }
    constexpr std::array kNvidia{std::pair{DiPreset::Fast, Preset::Fast}, std::pair{DiPreset::Medium, Preset::Medium},
                                 std::pair{DiPreset::Unbiased, Preset::Unbiased},
                                 std::pair{DiPreset::Ultra, Preset::Ultra},
                                 std::pair{DiPreset::Reference, Preset::Reference}};
    for (const auto& [diPreset, preset] : kNvidia) {
        if (rtxdi.isReference == (preset == Preset::Reference) &&
            rtxdi.settings == RtxdiContext::nvidiaPreset(preset)) {
            return diPreset;
        }
    }
    return DiPreset::Custom;
}

void SettingsWindow::applyDiPreset(DiPreset preset)
{
    RtxdiContext& rtxdi = renderer.getRtxdi();
    switch (preset) {
    case DiPreset::Custom:
    case DiPreset::Count:
        return;
    case DiPreset::Shipped:
        rtxdi.settings = RtxdiContext::defaultSettings();
        break;
    case DiPreset::Converged:
        rtxdi.settings = RtxdiContext::convergedSettings();
        break;
    case DiPreset::Fast:
        rtxdi.settings = RtxdiContext::nvidiaPreset(Preset::Fast);
        break;
    case DiPreset::Medium:
        rtxdi.settings = RtxdiContext::nvidiaPreset(Preset::Medium);
        break;
    case DiPreset::Unbiased:
        rtxdi.settings = RtxdiContext::nvidiaPreset(Preset::Unbiased);
        break;
    case DiPreset::Ultra:
        rtxdi.settings = RtxdiContext::nvidiaPreset(Preset::Ultra);
        break;
    case DiPreset::Reference:
        rtxdi.settings = RtxdiContext::nvidiaPreset(Preset::Reference);
        break;
    }
    rtxdi.isReference = preset == DiPreset::Reference;
    rtxdi.isSpatialOnly = false;
    rtxdi.isTemporalOnly = false;
    // NVIDIA re-applies the DLSS-RR preset after a quality preset so its overrides survive
    if (isCompatPresetAutoApplied && renderer.denoiser == DenoiserMode::DlssRr) {
        RtxdiContext::applyDenoiserCompatDefaults(rtxdi.giSettings, true);
    }
}

void SettingsWindow::setDenoiser(DenoiserMode mode)
{
    if (mode == renderer.denoiser) {
        return;
    }
    renderer.denoiser = mode;
    if (isCompatPresetAutoApplied) {
        RtxdiContext::applyDenoiserCompatDefaults(renderer.getRtxdi().giSettings, mode == DenoiserMode::DlssRr);
    }
}

void SettingsWindow::drawScene()
{
    using namespace settingsRows;
    if (beginNode("Scene", kScene, {kSceneEngine}, kColorRegularHeader)) {
        notInEngine("Animation Speed", "The engine has no animated scene content.");
        fixedCheckbox("Alpha-Tested Geometry", true, "Engine: the alpha-tested raster bin always runs.");
        GlassSettings& glass = renderer.glass;
        const uint32_t glassInstances = renderer.getRayTracingScene().glassRowCount();
        ImGui::BeginDisabled(glassInstances == 0);
        beginRow({"glass.pass"});
        ImGui::Checkbox("Transparent Geometry", &glass.isPassEnabled);
        endRow("Engine: composite the thin-walled glass layers in front of each pixel before the denoiser; off: "
               "glass is invisible. Disabled without glass instances.");
        ImGui::EndDisabled();
        notInEngine("Environment", "The engine has no environment map.");
        notInEngine("Environment Bias (EV)");
        notInEngine("Environment Rotation (deg)");
        notInEngine("Global Emissive Factor", "The lab's scene.emissiveScale is planned (full_testing T8).");

        if (beginNode("Engine", kSceneEngine)) {
            ImGui::SeparatorText("Sun");
            WorldLights::Sun& sun = renderer.getWorldLights().sun;
            ImGui::Checkbox("Enabled##sun", &sun.isEnabled);
            ImGui::SetItemTooltip("one directional light after the scene's own; toggling it resets lighting history");
            sliderFloat("Azimuth", sun.azimuthDegrees, -180.0f, 180.0f, "%.0f deg");
            ImGui::SetItemTooltip("from +X toward +Z; Sponza's courtyard runs along X");
            sliderFloat("Elevation", sun.elevationDegrees, 0.0f, 90.0f, "%.0f deg");
            ImGui::SliderFloat("Illuminance", &sun.illuminance, 0.01f, 10000.0f, "%.3g lux",
                               ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
            ImGui::SetItemTooltip("noisyColor is RGBA16F and not pre-exposed: very high values can overflow it");
            ImGui::ColorEdit3("Color##sun", &sun.color.x, ImGuiColorEditFlags_Float);
            if (ImGui::Button("Defaults##sun")) {
                sun = WorldLights::defaultSun();
            }

            ImGui::SeparatorText("Glass");
            const GpuGlassCounters& glassStats = renderer.getGlassCounters();
            ImGui::Text("%u glass instances; %u pixels behind glass, %u early stops, %u at the cap", glassInstances,
                        glassStats.pixels, glassStats.earlyStops, glassStats.capped);
            ImGui::SetItemTooltip("smooth thin-walled KHR_materials_transmission (roughness <= %.1f, no roughness "
                                  "texture): not rasterized, on its own ray mask; other transmissive materials render "
                                  "opaque (m8 I13)",
                                  kGlassMaxRoughness);
            ImGui::BeginDisabled(glassInstances == 0);
            beginRow({"glass.maxLayers"});
            sliderU32("Max glass layers", glass.maxLayers, 1, kGlassMaxLayersLimit);
            endRow("crossings a camera or shadow ray composites at most; back faces of single-sided glass do not "
                   "count");
            beginRow({"glass.reflection"});
            ImGui::Checkbox("Glass reflection", &glass.isReflectionEnabled);
            endRow("Fresnel-weighted mirror ray per layer, shaded with emission and one light, no indirect");
            beginRow({"glass.diffuseShare"});
            ImGui::Checkbox("Glass diffuse share", &glass.isDiffuseShareEnabled);
            endRow("the untransmitted dielectric share, Lambert with one sampled light");
            beginRow({"glass.emission"});
            ImGui::Checkbox("Glass emission", &glass.isEmissionEnabled);
            endRow();
            beginRow({"glass.shadowTransmittance"});
            ImGui::Checkbox("Shadow transmittance", &glass.isShadowTransmittanceEnabled);
            endRow("final DI, X2's direct light and final ReSTIR GI shading multiply the transmittance of the glass on "
                   "their segment; sampling and reuse stay glass-free");
            beginRow({"glass.blendGuides"});
            ImGui::Checkbox("Blend DLSS guides with glass", &glass.isGuideBlendEnabled);
            endRow("off: DLSS-RR's albedo guides are the surface behind the glass; on: blended with the glass layers' "
                   "reflectance and diffuse share");
            ImGui::EndDisabled();
            endNode();
        }
        endNode();
    }
    ImGui::Separator();
}

void SettingsWindow::drawGeneralRendering()
{
    using namespace settingsRows;
    if (beginNode("General Rendering", kGeneral, {}, kColorRegularHeader)) {
        const Dlss& dlss = renderer.getDlss();
        const bool isDenoiserOff = renderer.denoiser == DenoiserMode::Off;
        const int scale = isDenoiserOff ? 100
                                        : static_cast<int>(std::lround(100.0 * dlss.getRenderExtent().width /
                                                                       std::max(dlss.getDisplayExtent().width, 1u)));
        fixedText("Resolution Scale (%)", std::format("{}", scale),
                  "Engine: the render-to-display scale of the DLSS-RR mode (Denoising > DLSS-RR > Mode); 100 while "
                  "the denoiser is off, which renders native as the NVIDIA samples do.");

        // No AA without a denoiser leaves nothing to resolve the jitter
        const bool isJitterUnresolved = isDenoiserOff && renderer.aaMode == AaMode::None;
        ImGui::BeginDisabled(isJitterUnresolved);
        beginRow({"frame.jitter"});
        bool isJittered = camera.isJitterEnabled && !isJitterUnresolved;
        if (ImGui::Checkbox("##enablePixelJitter", &isJittered)) {
            camera.isJitterEnabled = isJittered;
        }
        endRow();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::PushItemWidth(kPairedItemWidth);
        int pattern = 1;
        ImGui::BeginDisabled();
        ImGui::Combo("Pixel Jitter", &pattern, "MSAA\0Halton\0R2\0White Noise\0");
        ImGui::EndDisabled();
        ImGui::PopItemWidth();
        helpMarker(std::format("Engine: Halton(2,3), {} phases, on frames DLSS-RR or Accumulation resolves. Off and "
                               "disabled with the denoiser off and No AA.",
                               camera.getJitterPhaseCount())
                       .c_str());

        if (beginNode("Engine", {})) {
            ImGui::SeparatorText("RTX Mega Geometry");
            RayTracingScene::Settings& ptlas = renderer.getRayTracingScene().settings;
            const RayTracingScene::Stats rt = renderer.getRayTracingStats();
            ImGui::Text("PTLAS: %u instances in %u partitions, %u written, update %.3f ms", rt.instances, rt.partitions,
                        rt.writtenInstances, rt.tlasBuildMs);
            ImGui::SetItemTooltip(
                "one CLAS per meshlet and one cluster BLAS per primitive, built at load; the PTLAS is "
                "updated in place on async compute");
            sliderU32("Instances per partition", ptlas.instancesPerPartition, 1, 1024);
            ImGui::SetItemTooltip("grid density of the PTLAS partitions; a change rebuilds the PTLAS");
            ImGui::Checkbox("Force full PTLAS build", &ptlas.isFullBuildForced);
            ImGui::SetItemTooltip("rewrite every instance into an empty PTLAS each frame, to compare against the "
                                  "changed-only update");
            endNode();
        }
        endNode();
    }
    ImGui::Separator();
}

void SettingsWindow::drawStaticContext()
{
    if (beginNode("Static ReSTIR Context Settings", {}, {}, kColorRegularHeader)) {
        helpMarker("Heavyweight settings (e.g. that dictate buffer sizes) that require recreating the context to "
                   "change.\n\nEngine: fixed at context creation (vk_rtxdi.cpp).");
        notInEngine("Importance Sample Env. Map");
        const RtxdiContext::AppliedParameters applied = renderer.getRtxdi().getAppliedParameters();
        if (ImGui::TreeNode("RTXDI Context")) {
            fixedCheckbox("Checkerboard Rendering", false);
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("ReGIR Context")) {
            const rtxdi::ReGIRStaticParameters& regir = applied.regirStatic;
            fixedText("Lights per Cell", std::format("{}", regir.lightsPerCell));
            constexpr std::array kModes{"Disabled", "Grid", "Onion"};
            const auto mode = std::to_underlying(regir.mode);
            fixedText("ReGIR Mode", mode < kModes.size() ? kModes[mode] : "?");
            if (regir.mode == rtxdi::ReGIRMode::Grid) {
                const auto& size = regir.gridParameters.gridSize;
                fixedText("Grid Resolution", std::format("{} x {} x {}", size.x, size.y, size.z));
            } else if (regir.mode == rtxdi::ReGIRMode::Onion) {
                fixedText("Onion Layers - Detail", std::format("{}", regir.onionParameters.onionDetailLayers));
                fixedText("Onion Layers - Coverage", std::format("{}", regir.onionParameters.onionCoverageLayers));
            }
            ImGui::Text("Total ReGIR Cells: %u", applied.regirLightSlotCount / std::max(regir.lightsPerCell, 1u));
            ImGui::TreePop();
        }
        endNode();
    }
    ImGui::Separator();
}

void SettingsWindow::drawDirectLighting()
{
    using namespace settingsRows;
    if (!beginNode("Direct Lighting", kDirect,
                   {kDirectLocalLights, kDirectInitial, kDirectTemporal, kDirectSpatial, kDirectFinal, kDirectEngine},
                   kColorAttentionHeader)) {
        ImGui::Separator();
        return;
    }
    RtxdiContext& rtxdi = renderer.getRtxdi();
    int directMode = 2;
    ImGui::BeginDisabled();
    ImGui::Combo("Direct Lighting Mode", &directMode, "None\0BRDF\0ReSTIR\0");
    ImGui::EndDisabled();
    helpMarker("Sample the direct lighting using ReSTIR.\n\nEngine: ReSTIR only.");

    constexpr std::array<const char*, std::to_underlying(DiPreset::Count)> kPresetLabels{
        "(Custom)", "Fast", "Medium", "Unbiased", "Ultra", "Reference", "Shipped", "Converged"};
    int preset = std::to_underlying(currentDiPreset());
    if (ImGui::Combo("Preset", &preset, kPresetLabels.data(), static_cast<int>(kPresetLabels.size()))) {
        applyDiPreset(static_cast<DiPreset>(preset));
    }
    helpMarker("NVIDIA's presets over the sample's start state (Medium), then the engine's: Shipped "
               "(RtxdiContext::defaultSettings) and Converged (the convergence check's set). The combo shows the "
               "preset the current settings equal, else (Custom). Fast's checkerboard rendering is not in the engine.");
    ImGui::Checkbox("Show Advanced Settings", &isAdvancedShown);

    ImGui::PushItemWidth(kModeComboWidth);
    enum ResamplingMode : int
    {
        None,
        Temporal,
        Spatial,
        TemporalAndSpatial,
    };
    int mode = rtxdi.isReference ? None
        : rtxdi.isSpatialOnly    ? Spatial
        : rtxdi.isTemporalOnly   ? Temporal
                                 : TemporalAndSpatial;
    beginRow({"di.reference", "di.temporalOnly", "di.spatialOnly"});
    if (ImGui::Combo("Resampling Mode", &mode, "None\0Temporal\0Spatial\0Temporal + Spatial\0")) {
        rtxdi.isReference = mode == None;
        rtxdi.isTemporalOnly = mode == Temporal;
        rtxdi.isSpatialOnly = mode == Spatial;
    }
    endRow("Engine: None is the reference, which also takes 32 Power_RIS local and 8 infinite candidates and no BRDF "
           "candidates: the unbiased ground truth of the reuse passes.");
    ImGui::PopItemWidth();
    ImGui::Separator();

    const RtxdiContext::AppliedParameters applied = rtxdi.getAppliedParameters();
    if (ImGui::TreeNode("ReGIR Presampling")) {
        helpMarker("Dynamic ReGIR Settings\n\nEngine: fixed; ReGIR is centered on the camera every frame.");
        const rtxdi::ReGIRDynamicParameters& regir = applied.regirDynamic;
        fixedText("ReGIR RIS Presampling Mode",
                  regir.presamplingMode == rtxdi::LocalLightReGIRPresamplingMode::Uniform ? "Uniform Sampling"
                                                                                          : "Power RIS",
                  "Presampling method the ReGIR algorithm uses to select lights");
        fixedText("Cell Size", std::format("{:.3f}", regir.regirCellSize));
        fixedText("Grid Build Samples", std::format("{}", regir.regirNumBuildSamples));
        fixedText("Sampling Jitter", std::format("{:.3f}", regir.regirSamplingJitter));
        notInEngine("Freeze Position");
        notInEngine("Visualize Cells");
        ImGui::TreePop();
    }
    drawDirectInitialSampling();
    ImGui::BeginDisabled(rtxdi.isReference);
    drawDirectTemporal();
    drawDirectSpatial();
    ImGui::EndDisabled();
    drawDirectFinalShading();
    drawDirectEngine();
    endNode();
    ImGui::Separator();
}

void SettingsWindow::drawDirectInitialSampling()
{
    using namespace settingsRows;
    if (!beginNode("Initial Sampling", kDirectInitial, {kDirectLocalLights})) {
        return;
    }
    RtxdiContext& rtxdi = renderer.getRtxdi();
    RtxdiContext::Settings& di = rtxdi.settings;
    // the reference overrides the candidate counts
    ImGui::BeginDisabled(rtxdi.isReference);
    if (beginNode("Local Light Sampling", kDirectLocalLights, {}, std::nullopt, ImGuiTreeNodeFlags_DefaultOpen)) {
        struct LocalMode
        {
            ReSTIRDI_LocalLightSamplingMode mode;
            const char* radio;
            const char* radioHelp;
            const char* samples;
            const char* samplesHelp;
        };
        constexpr std::array kModes{
            LocalMode{ReSTIRDI_LocalLightSamplingMode::Uniform, "Local Light Uniform Sampling",
                      "Sample local lights uniformly", "Local Light Uniform Samples",
                      "Number of samples drawn uniformly from the local light pool."},
            LocalMode{ReSTIRDI_LocalLightSamplingMode::Power_RIS, "Local Light Power RIS",
                      "Sample local lights using power-based RIS", "Local Light Power RIS Samples",
                      "Number of samples drawn from the local lights power-based RIS buffer."},
            LocalMode{ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS, "Local Light ReGIR RIS",
                      "Sample local lights using ReGIR-based RIS", "Local Light ReGIR RIS Samples",
                      "Number of samples drawn from the local lights ReGIR-based RIS buffer"},
        };
        constexpr uint32_t kMaxLocalSamples = 32;
        for (const LocalMode& entry : kModes) {
            const bool isActive = di.localLightSamplingMode == entry.mode;
            beginRow({"di.sampling"});
            if (ImGui::RadioButton(entry.radio, isActive)) {
                di.localLightSamplingMode = entry.mode;
            }
            endRow(entry.radioHelp);
            // the lab's di.localSamples is the active mode's count
            if (isActive) {
                beginRow({"di.localSamples"});
            }
            sliderU32(entry.samples, di.localLightSamples[std::to_underlying(entry.mode)], 0, kMaxLocalSamples);
            if (isActive) {
                endRow(entry.samplesHelp);
            } else {
                helpMarker(entry.samplesHelp);
            }
        }
        const rtxdi::LocalLightReGIRFallbackSamplingMode fallback =
            renderer.getRtxdi().getAppliedParameters().regirDynamic.fallbackSamplingMode;
        fixedText("ReGIR RIS Fallback Sampling Mode",
                  fallback == rtxdi::LocalLightReGIRFallbackSamplingMode::Uniform ? "Uniform Sampling" : "Power RIS",
                  "Sampling method to fall back to for surfaces outside the ReGIR volume");
        endNode();
    }

    beginRow({"di.brdfSamples"});
    sliderU32("Initial BRDF Samples", di.brdfSamples, 0, 8);
    endRow("Number of rays traced from the surface using BRDF importance sampling to find mesh lights or environment "
           "map samples. Helps glossy reflections.");
    ImGui::BeginDisabled(di.areInfiniteLightsShadedExactly);
    beginRow({"di.infiniteSamples"});
    sliderU32("Initial Infinite Light Samples", di.infiniteLightSamples, 0, 32);
    endRow("Number of samples drawn from the infinite light pool, i.e. the sun light when using the procedural "
           "environment, and the environment map when it's not importance sampled.\n\nEngine: disabled while Exact "
           "directional lights (Engine node) shades them outside RIS.");
    ImGui::EndDisabled();
    notInEngine("Initial Environment Samples",
                "Number of samples drawn from the environment map when it is "
                "importance sampled.");
    ImGui::EndDisabled();
    beginRow({"di.initialVisibility"});
    ImGui::Checkbox("Enable Initial Visibility", &di.isInitialVisibilityEnabled);
    endRow("Engine: one shadow ray per pixel in the initial pass; occluded samples are dropped before reuse.");
    const RTXDI_DIInitialSamplingParameters initial = renderer.getRtxdi().getAppliedParameters().initialSampling;
    fixedText("BRDF Sample Cutoff", std::format("{:.4f}", initial.brdfCutoff),
              "Determine how much to shorten BRDF rays. 0 to disable shortening");
    fixedText("BRDF Ray Min T", std::format("{:.3f}", initial.brdfRayMinT),
              "Minimum distance to shoot BRDF ray.\n\nEngine: 0, BRDF rays leave from an offset origin.");
    endNode();
}

void SettingsWindow::drawDirectTemporal()
{
    using namespace settingsRows;
    if (!beginNode("Temporal Resampling", kDirectTemporal)) {
        return;
    }
    RtxdiContext::Settings& di = renderer.getRtxdi().settings;
    beginRow({"di.fallback"});
    ImGui::Checkbox("Enable Fallback Sampling", &di.isFallbackSamplingEnabled);
    endRow("If the motion-reprojected search finds no matching surface, try a permuted previous-frame pixel with zero "
           "motion. Keeps the stored light UV and skips depth and normal rejection for this final attempt.");
    fixedCheckbox("Enable Previous Frame TLAS/BLAS", false,
                  "Use the previous frame TLAS for bias correction rays during temporal resampling and gradient "
                  "computation. Resutls in less biased results under motion and brighter, more complete gradients.");
    beginRow({"di.permutation"});
    ImGui::Checkbox("Enable Permutation Sampling", &di.isPermutationSamplingEnabled);
    endRow("Shuffle the pixels from the previous frame when resampling from them. This makes pixel colors less "
           "correlated temporally and therefore better suited for temporal accumulation and denoising. Also results in "
           "a higher positive bias when the Reuse Final Visibility setting is on, which somewhat counteracts the "
           "negative bias from spatial resampling.");
    beginRow({"di.temporalCorrection"});
    comboEnum("Temporal Bias Correction", di.temporalBiasCorrection, std::array{"Off", "Basic", "Raytraced"},
              std::array{ReSTIRDI_TemporalBiasCorrectionMode::Off, ReSTIRDI_TemporalBiasCorrectionMode::Basic,
                         ReSTIRDI_TemporalBiasCorrectionMode::Raytraced});
    endRow("Off = use the 1/M normalization.\nBasic = use the MIS normalization but assume that every sample is "
           "visible.\nRay Traced = use the MIS normalization and verify visibility.");
    if (isAdvancedShown) {
        beginRow({"di.depthThreshold"});
        sliderFloat("Temporal Depth Threshold", di.depthThreshold, 0.0f, 1.0f);
        endRow("Higher values result in accepting temporal samples with depths more different from the current "
               "pixel.\n\nEngine: one pair serves temporal and spatial resampling.");
        beginRow({"di.normalThreshold"});
        sliderFloat("Temporal Normal Threshold", di.normalThreshold, 0.0f, 1.0f);
        endRow("Lower values result in accepting temporal samples with normals more different from the current pixel."
               "\n\nEngine: one pair serves temporal and spatial resampling.");
        fixedText(
            "Permutation Sampling Threshold",
            std::format("{:.3f}",
                        renderer.getRtxdi().getAppliedParameters().temporalResampling.permutationSamplingThreshold),
            "Higher values result in disabling permutation sampling on less complex surfaces.");
    }
    beginRow({"di.history"});
    sliderU32("Max History Length", di.maxHistoryLength, 1, 100);
    endRow();
    beginRow({"di.boilingFilter"});
    ImGui::Checkbox("##enableBoilingFilter", &di.isBoilingFilterEnabled);
    endRow();
    ImGui::SameLine();
    ImGui::PushItemWidth(kPairedItemWidth);
    beginRow({"di.boilingStrength"});
    sliderFloat("Boiling Filter", di.boilingFilterStrength, 0.0f, 1.0f);
    endRow("The boiling filter analyzes the neighborhood of each pixel and discards the pixel's reservoir if it has a "
           "significantly higher weight than the other pixels.");
    ImGui::PopItemWidth();
    endNode();
}

void SettingsWindow::drawDirectSpatial()
{
    using namespace settingsRows;
    if (!beginNode("Spatial Resampling", kDirectSpatial)) {
        return;
    }
    RtxdiContext& rtxdi = renderer.getRtxdi();
    RtxdiContext::Settings& di = rtxdi.settings;
    fixedText("DI Neighbor Offset Sampling", "Stratified Low Discrepancy",
              "ReSTIR DI stratified low discrepancy sampling takes each group of four neighbors from four different "
              "quadrants. DI spatial sample sliders use increments of two to avoid shimmering with odd counts.\n\n"
              "Engine: fixed; it is why the spatial counts are even.");
    beginRow({"di.spatialCorrection"});
    comboEnum("Spatial Bias Correction", di.spatialBiasCorrection, std::array{"Off", "Basic", "Pairwise", "Ray Traced"},
              std::array{ReSTIRDI_SpatialBiasCorrectionMode::Off, ReSTIRDI_SpatialBiasCorrectionMode::Basic,
                         ReSTIRDI_SpatialBiasCorrectionMode::Pairwise, ReSTIRDI_SpatialBiasCorrectionMode::Raytraced});
    endRow("Off = use the 1/M normalization.\nBasic = use the MIS normalization but assume that every sample is "
           "visible.\nPairwise = pairwise MIS improves perf and specular quality (assumes every sample is visible).\n"
           "Ray Traced = use the MIS normalization and verify visibility.");
    beginRow({"di.spatialSamples"});
    editSpatialSampleCount("Spatial Samples", di.spatialSamples, kMaxDiSpatialSamples);
    endRow(kSpatialSampleHelp);
    // NVIDIA hides it outside Temporal + Spatial; disabled keeps the closed node's count matching what opens
    ImGui::BeginDisabled(rtxdi.isReference || rtxdi.isSpatialOnly || rtxdi.isTemporalOnly);
    beginRow({"di.boostSamples"});
    editSpatialSampleCount("Disocclusion Boost Samples", di.disocclusionBoostSamples, kMaxDiSpatialSamples);
    endRow("The number of spatial samples to take on surfaces which don't have sufficient accumulated history length. "
           "More samples result in faster convergence in disoccluded regions but increase processing time.\n\nEngine: "
           "Temporal + Spatial only; the history length is Boost history length (Engine node).");
    ImGui::EndDisabled();
    beginRow({"di.radius"});
    sliderFloat("Spatial Sampling Radius", di.samplingRadius, 1.0f, 32.0f);
    endRow();
    if (isAdvancedShown) {
        beginRow({"di.depthThreshold"});
        sliderFloat("Spatial Depth Threshold", di.depthThreshold, 0.0f, 1.0f);
        endRow("Higher values result in accepting samples with depths more different from the center pixel.\n\n"
               "Engine: one pair serves temporal and spatial resampling.");
        beginRow({"di.normalThreshold"});
        sliderFloat("Spatial Normal Threshold", di.normalThreshold, 0.0f, 1.0f);
        endRow("Lower values result in accepting samples with normals more different from the center pixel.\n\n"
               "Engine: one pair serves temporal and spatial resampling.");
        fixedCheckbox("Discount Naive Samples",
                      renderer.getRtxdi().getAppliedParameters().spatialResampling.discountNaiveSamples != 0,
                      "Prevents samples which are from the current frame or have no reasonable temporal history "
                      "merged being spread to neighbors.");
    }
    endNode();
}

void SettingsWindow::drawDirectFinalShading()
{
    using namespace settingsRows;
    if (!beginNode("Final Shading", kDirectFinal)) {
        return;
    }
    RtxdiContext::Settings& di = renderer.getRtxdi().settings;
    fixedCheckbox("Enable Final Visibility",
                  renderer.getRtxdi().getAppliedParameters().shading.enableFinalVisibility != 0);
    beginRow({"di.visibilityShortcut"});
    ImGui::Checkbox("Discard Invisible Samples", &di.isVisibilityShortcutEnabled);
    endRow("When a sample is determined to be occluded during final shading, its reservoir is discarded. This can "
           "significantly reduce noise, but also introduce some bias near shadow boundaries beacuse the reservoirs' M "
           "values are kept. Also, enabling this option speeds up temporal resampling with Ray Traced bias correction "
           "by skipping most of the bias correction rays.");
    beginRow({"di.finalVisibilityReuse"});
    ImGui::Checkbox("Reuse Final Visibility", &di.isFinalVisibilityReused);
    endRow("Store the fractional final visibility term in the reservoirs and reuse it later if the reservoir is not "
           "too old and has not moved too far away from its original location. Enable the Advanced Settings option to "
           "control the thresholds.");
    if (di.isFinalVisibilityReused && isAdvancedShown) {
        const RTXDI_ShadingParameters shading = renderer.getRtxdi().getAppliedParameters().shading;
        fixedText("Final Visibility - Max Distance", std::format("{:.3f}", shading.finalVisibilityMaxDistance));
        fixedText("Final Visibility - Max Age", std::format("{}", shading.finalVisibilityMaxAge));
    }
    endNode();
}

void SettingsWindow::drawDirectEngine()
{
    using namespace settingsRows;
    if (!beginNode("Engine", kDirectEngine)) {
        return;
    }
    RtxdiContext::Settings& di = renderer.getRtxdi().settings;
    WorldLights& lights = renderer.getWorldLights();
    ImGui::Text("Lights: %u punctual, %u triangle, %u infinite", lights.punctualLocalLightCount(),
                lights.triangleLightCount(), lights.infiniteLightCount());
    ImGui::SetItemTooltip("world light rows: point and spot, one per triangle of every emissive primitive instance "
                          "(RTXDI's local region), then directional and the sun");
    ImGui::Checkbox("Sample triangle lights", &lights.areTriangleLightsSampled);
    ImGui::SetItemTooltip("off: RTXDI samples point, spot and directional lights only, and rays that hit an emissive "
                          "triangle add its emission instead; resets lighting history");
    beginRow({"di.exactInfinite"});
    ImGui::Checkbox("Exact directional lights", &di.areInfiniteLightsShadedExactly);
    endRow("shade directional lights in the shade pass with one shadow ray each instead of as RIS candidates: no reuse "
           "noise on the sun");
    beginRow({"di.boostHistory"});
    sliderU32("Boost history length", di.disocclusionHistoryLength, 0, 64);
    endRow("history length below which the disocclusion boost applies; 0 never boosts (RTXDI default)");
    beginRow({"di.materialTest"});
    ImGui::Checkbox("Material similarity", &di.isMaterialSimilarityTestEnabled);
    endRow("reject neighbors whose roughness or reflectance differs too much");
    beginRow({"di.perPixelLightTiles"});
    ImGui::Checkbox("Per-pixel light tiles", &di.areLightTilesPerPixel);
    endRow("pick an independent RIS tile / ReGIR cell jitter per pixel instead of sharing one per 16x16 screen tile; "
           "also applies at path vertices and behind glass");
    endNode();
}

void SettingsWindow::drawIndirectLighting()
{
    using namespace settingsRows;
    if (!beginNode(
            "Indirect Lighting", kIndirect,
            {kIndirectReuse, kIndirectNrc, kIndirectTemporal, kIndirectSpatial, kIndirectEngine, kIndirectPtProfile},
            kColorAttentionHeader, ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Separator();
        return;
    }
    RtxdiContext& rtxdi = renderer.getRtxdi();
    RtxdiContext::GiSettings& gi = rtxdi.giSettings;
    IndirectMode& indirectMode = renderer.indirectMode;
    beginRow({"frame.indirectMode"});
    if (ImGui::BeginCombo("Indirect Lighting Mode", indirectModeInfo(indirectMode).label)) {
        for (size_t i = 0; i < kIndirectModes.size(); ++i) {
            const auto mode = static_cast<IndirectMode>(i);
            if (ImGui::Selectable(kIndirectModes[i].label, mode == indirectMode)) {
                indirectMode = mode;
            }
        }
        ImGui::EndCombo();
    }
    endRow(std::format("Trace diffuse and specular BRDF rays and resample results with ReSTIR GI. Shade the surfaces "
                       "found with BRDF rays using direct light sampling.\n\nEngine: ReSTIR GI is NVIDIA's ReSTIR GI "
                       "(one bounce, NRC paused). NRC and NRC + ReSTIR GI terminate the path in the RTXGI "
                       "IndirectDiffuse sample's radiance cache, NRC without reuse. {} ships and is the tuning "
                       "target. A switch resets ReSTIR GI and DLSS-RR history.",
                       indirectModeInfo(kDefaultIndirectMode).label)
               .c_str());

    beginRow({"gi.roughnessFloor"});
    ImGui::Checkbox("##enableMinSecondaryRoughness", &gi.isSecondaryRoughnessFloorEnabled);
    endRow();
    ImGui::SameLine();
    float roughness = gi.isSecondaryRoughnessFloorEnabled ? kSecondaryRoughnessFloor : 0.0f;
    ImGui::BeginDisabled();
    ImGui::SliderFloat("Min Secondary Roughness", &roughness, 0.0f, 1.0f);
    ImGui::EndDisabled();
    helpMarker("Engine: on or off at the sample's 0.5; shades X2 with roughness at least 0.5 while GI reuse runs: no "
               "glossy caustics in the reused samples, biased on glossy X2.");

    if (ImGui::TreeNode("Secondary Surface Light Sampling")) {
        const uint32_t localSamples = rtxdi.settings.activeLocalLightSamples();
        fixedText("Indirect Local Light Samples", std::format("{}", localSamples));
        fixedText("Indirect Inifinite Light Samples", std::format("{}", rtxdi.settings.infiniteLightSamples));
        notInEngine("Indirect Environment Samples");
        helpMarker("Engine: path vertices sample lights with the primary surface's settings (sampleLightsAt).");
        ImGui::TreePop();
    }
    if (beginNode("Reuse Primary Samples", kIndirectReuse)) {
        beginRow({"gi.secondaryResampling"});
        ImGui::Checkbox("Reuse RTXDI samples for secondary surface", &gi.isSecondaryResamplingEnabled);
        endRow("When shading a secondary surface, try to find a matching surface in screen space and reuse its light "
               "reservoir. This feature uses the Spatial Resampling function and has similar controls.");
        const std::string fixedHelp =
            "Engine: the sample's Ultra values (path_shading.slang secondaryResamplingParams)";
        fixedText("Secondary Bias Correction", kSecondaryBiasCorrection, fixedHelp.c_str());
        fixedText("Secondary Samples", std::format("{}", kSecondarySamples));
        fixedText("Secondary Sampling Radius", std::format("{:.3f}", kSecondaryRadius));
        fixedText("Secondary Depth Threshold", std::format("{:.3f}", kSecondaryDepthThreshold));
        fixedText("Secondary Normal Threshold", std::format("{:.3f}", kSecondaryNormalThreshold));
        endNode();
    }
    drawIndirectNrc();
    drawIndirectResampling();
    beginRow({"gi.finalVisibility"});
    ImGui::Checkbox("Final visibility", &gi.isFinalVisibilityEnabled);
    endRow("Engine: a reconnection ray from the pixel to its final sample, which may be a neighbor's.");
    beginRow({"gi.finalMis"});
    ImGui::Checkbox("Final MIS", &gi.isFinalMisEnabled);
    endRow("Engine: blend in the pixel's own initial sample where its BRDF lobe is glossy.");
    drawIndirectEngine();
    endNode();
    ImGui::Separator();
}

void SettingsWindow::drawIndirectNrc()
{
    using namespace settingsRows;
    if (!beginNode("NRC:", kIndirectNrc)) {
        return;
    }
    NrcCache& nrcCache = renderer.getNrc();
    NrcCache::Settings& nrc = nrcCache.settings;
    const nrc::ContextSettings& context = nrcCache.getContextSettings();
    const nrc::FrameSettings frame = nrcCache.frameSettings();
    beginRow({"nrc.training"});
    ImGui::Checkbox("Train The Cache", &nrc.isTrainingEnabled);
    endRow("Engine: off freezes the cache and skips the training paths.");
    fixedText("Training Bounces", std::format("{}", context.maxPathVertices), "Engine: the context's maxPathVertices.");
    fixedCheckbox("Learn Irradiance", context.learnIrradiance);
    fixedCheckbox("Include Direct Illumination", context.includeDirectLighting,
                  "Engine: direct light at the query vertex is sampled explicitly; the cache holds the indirect part.");
    fixedCheckbox("Skip delta vertices", frame.skipDeltaVertices);
    fixedText("Heuristic Threshold", std::format("{:.3f}", frame.terminationHeuristicThreshold));
    fixedText("Max Average Radiance Value", std::format("{:.3f}", frame.maxExpectedAverageRadianceValue));
    int resolveMode = static_cast<int>(nrc.resolveMode);
    beginRow({"nrc.resolveMode"});
    if (ImGui::Combo("Resolve Mode", &resolveMode, nrcCache.resolveModeComboString())) {
        nrc.resolveMode = static_cast<NrcResolveMode>(resolveMode);
    }
    endRow("Engine: AddQueryResultToOutput runs the custom GI reservoir resolve; any other mode runs NRC's SDK debug "
           "resolve into Indirect radiance (Debug view > Indirect radiance).");
    const vk::Extent2D training = nrcCache.getTrainingExtent();
    if (!nrcCache.isActive()) {
        ImGui::TextDisabled("no scene, training paths %ux%u", training.width, training.height);
    } else if (renderer.isNrcPaused()) {
        ImGui::TextDisabled("paused, training paths %ux%u", training.width, training.height);
    } else if (!nrcCache.isTrainingActive()) {
        ImGui::TextDisabled("active (idle, inference only)");
    } else if (nrc.idleTrainingFrames > 0) {
        ImGui::TextDisabled("active (training, %u left), paths %ux%u", nrcCache.getTrainingFramesRemaining(),
                            training.width, training.height);
    } else {
        ImGui::TextDisabled("active, training paths %ux%u", training.width, training.height);
    }
    endNode();
}

void SettingsWindow::drawIndirectResampling()
{
    using namespace settingsRows;
    RtxdiContext& rtxdi = renderer.getRtxdi();
    RtxdiContext::GiSettings& gi = rtxdi.giSettings;
    // isGiReference is the GI check's, not a lab switch: no highlight
    ImGui::PushItemWidth(kModeComboWidth);
    constexpr std::array kModes{"None", "Temporal", "Spatial", "Temporal + Spatial", "Fused Spatiotemporal"};
    constexpr size_t kNone = 0;
    constexpr size_t kTemporalAndSpatial = 3;
    const size_t current = rtxdi.isGiReference ? kNone : kTemporalAndSpatial;
    if (ImGui::BeginCombo("Resampling Mode", kModes[current])) {
        for (size_t i = 0; i < kModes.size(); ++i) {
            ImGui::BeginDisabled(i != kNone && i != kTemporalAndSpatial);
            if (ImGui::Selectable(kModes[i], i == current)) {
                rtxdi.isGiReference = i == kNone;
            }
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    helpMarker("Engine: None shades each pixel's initial sample alone, the unbiased estimate the reuse converges to; "
               "otherwise Temporal + Spatial. The NRC mode shades without reuse whatever is set here.");
    ImGui::PopItemWidth();
    ImGui::Separator();

    constexpr std::array kBiasNames{"Off", "Basic MIS", "RayTraced"};
    constexpr std::array kBiasModes{RTXDI_GIBiasCorrectionMode::Off, RTXDI_GIBiasCorrectionMode::Basic,
                                    RTXDI_GIBiasCorrectionMode::Raytraced};
    // NVIDIA hides both nodes without reuse; disabled keeps the closed section's count matching what opens
    ImGui::BeginDisabled(rtxdi.isGiReference);
    if (beginNode("Temporal Resampling", kIndirectTemporal)) {
        beginRow({"gi.depthThreshold"});
        sliderFloat("Temporal Depth Threshold", gi.depthThreshold, 0.001f, 1.0f);
        endRow("Engine: one pair serves temporal and spatial resampling.");
        beginRow({"gi.normalThreshold"});
        sliderFloat("Temporal Normal Threshold", gi.normalThreshold, 0.001f, 1.0f);
        endRow("Engine: one pair serves temporal and spatial resampling.");
        beginRow({"gi.age"});
        sliderU32("Max reservoir age", gi.maxReservoirAge, 1, RtxdiContext::kGiUnlimitedSampleAge);
        endRow(std::format("Engine: range to {} (NVIDIA 1-100), which never discards. Each pixel draws its own limit "
                           "in [0.5, 1) of this. Shipped {} (m8 I15): reacts to lighting changes and keeps stagnant "
                           "samples from brightening dim areas.",
                           RtxdiContext::kGiUnlimitedSampleAge, RtxdiContext::defaultGiSettings().maxReservoirAge)
                   .c_str());
        beginRow({"gi.history"});
        sliderU32("Max history length", gi.maxHistoryLength, 1, 100);
        endRow();
        beginRow({"gi.permutation"});
        ImGui::Checkbox("Enable permutation sampling", &gi.isPermutationSamplingEnabled);
        endRow();
        beginRow({"gi.fallback"});
        ImGui::Checkbox("Enable fallback sampling", &gi.isFallbackSamplingEnabled);
        endRow("Engine: when no history pixel matches, reuse last frame's sample at the same pixel.");
        beginRow({"gi.temporalCorrection"});
        comboEnum("Temporal bias correction", gi.temporalBiasCorrection, kBiasNames, kBiasModes);
        endRow("Engine: RayTraced adds a reconnection ray from last frame's surface.");
        beginRow({"gi.boilingFilter"});
        ImGui::Checkbox("##enableGIBoilingFilter", &gi.isBoilingFilterEnabled);
        endRow();
        ImGui::SameLine();
        ImGui::PushItemWidth(kPairedItemWidth);
        beginRow({"gi.boilingStrength"});
        sliderFloat("Boiling Filter##GIBoilingFilter", gi.boilingFilterStrength, 0.0f, 1.0f);
        endRow("Engine: after temporal reuse, drops reservoirs far brighter than their 8x8 group. The DLSS-RR profile "
               "turns it off for the firefly replacement (Engine node).");
        ImGui::PopItemWidth();
        endNode();
    }
    if (beginNode("Spatial Resampling", kIndirectSpatial)) {
        beginRow({"gi.spatialSamples"});
        sliderU32("Num spatial samples", gi.spatialSamples, 0, 8);
        endRow("Engine: range 0-8 (NVIDIA 1-7): 0 is temporal reuse alone.");
        beginRow({"gi.radius"});
        sliderFloat("Sampling Radius", gi.samplingRadius, 0.01f, 60.0f);
        endRow();
        beginRow({"gi.depthThreshold"});
        sliderFloat("Spatial Depth Threshold", gi.depthThreshold, 0.001f, 1.0f);
        endRow("Engine: one pair serves temporal and spatial resampling.");
        beginRow({"gi.normalThreshold"});
        sliderFloat("Spatial Normal Threshold", gi.normalThreshold, 0.001f, 1.0f);
        endRow("Engine: one pair serves temporal and spatial resampling.");
        beginRow({"gi.spatialCorrection"});
        comboEnum("Spatial Bias Correction Mode", gi.spatialBiasCorrection, kBiasNames, kBiasModes);
        endRow("Engine: RayTraced adds a reconnection ray per merged neighbor.");
        endNode();
    }
    ImGui::EndDisabled();
}

void SettingsWindow::drawIndirectEngine()
{
    using namespace settingsRows;
    if (!beginNode("Engine", kIndirectEngine, {kIndirectPtProfile})) {
        return;
    }
    RtxdiContext& rtxdi = renderer.getRtxdi();
    RtxdiContext::GiSettings& gi = rtxdi.giSettings;
    // spec m8 O11: ReSTIR PT's DLSS-RR profile ported to ReSTIR GI, under NVIDIA's ReSTIR PT labels
    if (beginNode("ReSTIR PT DLSS-RR profile", kIndirectPtProfile)) {
        beginRow({"gi.decorrelation"});
        comboEnum("Decorrelation mode", gi.decorrelationMode, std::array{"None", "Uniform", "Stagnancy-based"},
                  std::array{RTXDI_PTDecorrelationMode::None, RTXDI_PTDecorrelationMode::Uniform,
                             RTXDI_PTDecorrelationMode::Stagnancy});
        endRow("How to compute the per-pixel decorrelation probability for final shading.\nNone: feature disabled, "
               "regardless of the slider.\nUniform: constant probability equal to the slider across all pixels.\n"
               "Stagnancy-based: slider modulated by the temporally/spatially-smoothed reservoir Stagnancy signal, so "
               "decorrelation only fires where samples have been reused heavily.\n\nEngine: final shading shades the "
               "pixel's own initial sample instead of the reused one; the stored reservoir keeps its history.");
        const bool isDecorrelated = gi.decorrelationMode != RTXDI_PTDecorrelationMode::None;
        const bool isStagnancy = gi.decorrelationMode == RTXDI_PTDecorrelationMode::Stagnancy;
        ImGui::BeginDisabled(!isDecorrelated);
        beginRow({"gi.decorrelationFactor"});
        sliderFloat("Decorrelation factor", gi.decorrelationFactor, 0.0f, 1.0f);
        endRow("Maximum probability that final shading falls back to the unresampled initial-sampling reservoir "
               "(bypassing temporal/spatial resampling). In Uniform mode this value is used directly across all "
               "pixels; in Stagnancy mode it is further scaled by the smoothed duplication signal (0 where reservoirs "
               "are fresh, ramping up to this slider value where reservoirs have been reused heavily).");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!isStagnancy);
        beginRow({"gi.decorrelationExponent"});
        sliderFloat("Decorrelation stagnancy exponent", gi.decorrelationStagnancyExponent, 0.1f, 8.0f);
        endRow("Stagnancy mode only. Power exponent applied to the normalized smoothed Stagnancy (in [0, 1]) before it "
               "scales the decorrelation probability. Values < 1 make decorrelation ramp up earlier (more sensitive); "
               "values > 1 delay it (less sensitive). 1.0 is linear.");
        beginRow({"gi.decorrelationEma"});
        sliderFloat("Decorrelation EMA factor", gi.decorrelationEmaFactor, 0.0f, 1.0f);
        endRow("Stagnancy mode only. Exponential-moving-average blend factor for the temporally-smoothed duplication "
               "map. 1 fully uses the current frame's age/40 (no smoothing, more noisy); smaller values keep more "
               "history (smoother, slower to react). 0 means never update (not useful in practice).");
        beginRow({"gi.fireflyReplacement"});
        ImGui::Checkbox("Decorrelation firefly replacement", &gi.isFireflyReplacementEnabled);
        endRow("Stagnancy mode only. When enabled, the FinalShading pass runs an additional boiling filter and forces "
               "full decorrelation on pixels whose contribution looks like an outlier within their tile. Independent "
               "of the temporal-resampling boiling filter.");
        ImGui::BeginDisabled(!gi.isFireflyReplacementEnabled);
        beginRow({"gi.fireflyStrength"});
        sliderFloat("Decorrelation firefly filter strength", gi.fireflyReplacementStrength, 0.0f, 1.0f);
        endRow("Strength of the FinalShading boiling filter. Higher values reject more pixels as outliers.");
        beginRow({"gi.fullTileBoiling"});
        ImGui::Checkbox("Full-tile firefly test", &gi.isFullTileBoilingFilterEnabled);
        endRow("Engine: average the 8x8 tile firefly weight over all in-bounds lanes instead of nonzero lanes only.");
        ImGui::EndDisabled();
        beginRow({"gi.fireflyBound"});
        ImGui::Checkbox("Firefly replacement bias reduction", &gi.isFireflyReplacementBounded);
        endRow("Stagnancy mode only. When the decorrelation path swaps in the preserved initial-sampling reservoir, "
               "clamp its WeightSum to at most `multiplier bound * previous WeightSum` on pixels whose duplication "
               "value indicates fresh reservoirs (age == 0). Reduces bias from replacing resampled (filtered) firefly "
               "with higher value initial firefly.");
        ImGui::BeginDisabled(!gi.isFireflyReplacementBounded);
        beginRow({"gi.fireflyMultiplyBound"});
        sliderFloat("Firefly replacement multiplier bound", gi.fireflyReplacementMultiplyBound, 1.0f, 100.0f);
        endRow("Upper-bound multiplier used by the firefly-replacement bias-reduction clamp. The replacement "
               "reservoir's WeightSum is limited to `multiplier bound * previous WeightSum`. Smaller values reduces "
               "bias but can introduce more post-denoise correlation artifacts.");
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        beginRow({"gi.duplicationReduction"});
        ImGui::Checkbox("Duplication based history reduction", &gi.isDuplicationHistoryReductionEnabled);
        endRow(
            "Engine: where many of the 17x17 neighbors shared last frame's sample, temporal reuse keeps fewer frames "
            "of history; one more pass after the shade pass.");
        ImGui::BeginDisabled(!gi.isDuplicationHistoryReductionEnabled);
        beginRow({"gi.historyReductionStrength"});
        sliderFloat("History reduction strength", gi.historyReductionStrength, 0.0f, 1.0f);
        endRow();
        ImGui::EndDisabled();
        endNode();
    }

    beginRow({"gi.fireflyClamp"});
    ImGui::Checkbox("Firefly clamp", &gi.isFireflyClampEnabled);
    endRow("clamps X2's radiance in display units; biased");
    // secondary resampling takes the DI setting
    ImGui::BeginDisabled(gi.isSecondaryResamplingEnabled);
    beginRow({"gi.exactInfinite"});
    ImGui::Checkbox("Exact directional lights##gi", &gi.areInfiniteLightsShadedExactly);
    endRow("path vertices shade directional lights with one shadow ray each instead of as RIS candidates; with "
           "secondary DI resampling the DI setting applies");
    ImGui::EndDisabled();
    bool isGroundTruth = rtxdi.giReferenceBounces > 0;
    if (ImGui::Checkbox("Ground-truth paths", &isGroundTruth)) {
        rtxdi.giReferenceBounces = isGroundTruth ? RtxdiContext::kGiGroundTruthBounces : 0u;
    }
    ImGui::SetItemTooltip(
        "trace the bounces after X2 instead of querying NRC, which pauses: with Resampling Mode None, "
        "the ground truth of engine spec section 14 when accumulated");
    ImGui::BeginDisabled(!isGroundTruth);
    sliderU32("Bounces after X2", rtxdi.giReferenceBounces, 1, 16);
    ImGui::EndDisabled();
    NrcCache::Settings& nrc = renderer.getNrc().settings;
    beginRow({"nrc.idleTrainingFrames"});
    sliderU32("NRC idle train frames", nrc.idleTrainingFrames, 0, 1024);
    endRow("train for this many frames after a camera, light or scene change, then freeze until the next change (0 = "
           "train every frame)");
    beginRow({"nrc.iterations"});
    sliderU32("NRC training iterations", nrc.trainingIterations, 1, 4);
    endRow("16K training records each; the training paths grow with it");
    if (ImGui::Button("Defaults##gi")) {
        gi = RtxdiContext::defaultGiSettings();
    }
    ImGui::SetItemTooltip("shipped: NVIDIA's with ray-traced spatial bias correction and 1 spatial sample, and ReSTIR "
                          "PT's DLSS-RR profile (stagnancy decorrelation, firefly replacement with the full-tile test, "
                          "20 frames of history) with a 30-frame age limit; history reduction and the swap bound stay "
                          "off (both darken GI); exact directional lights at path vertices");
    ImGui::SameLine();
    if (ImGui::Button("NVIDIA defaults##gi")) {
        gi = RtxdiContext::nvidiaGiSettings();
    }
    ImGui::SetItemTooltip("the RTXDI sample's ReSTIR GI settings (the library defaults)");
    ImGui::SameLine();
    if (ImGui::Button("Converged##gi")) {
        gi = RtxdiContext::convergedGiSettings();
    }
    ImGui::SetItemTooltip("the GI check's configuration: ray-traced bias correction, no age limit, fallback, "
                          "permutation, boiling filter or roughness floor");
    endNode();
}

void SettingsWindow::drawDenoising()
{
    using namespace settingsRows;
    if (beginNode("Denoising", kDenoising, {kDenoisingDlss}, kColorAttentionHeader)) {
        // NVIDIA's order: Off, then DLSS-RR
        constexpr std::array kModes{DenoiserMode::Off, DenoiserMode::DlssRr};
        int index = renderer.denoiser == DenoiserMode::Off ? 0 : 1;
        beginRow({"frame.denoiser"});
        if (ImGui::Combo("Denoiser mode", &index, "Off\0DLSS-RR\0")) {
            setDenoiser(kModes[static_cast<size_t>(index)]);
        }
        endRow(
            "Engine: Off renders at the display resolution and shows the raw estimate after exposure and tonemap; "
            "No AA leaves it unjittered, Accumulation (Post-Processing) averages still frames. A switch rebuilds the "
            "render targets unless the DLSS-RR mode is DLAA, and resets history.");
        ImGui::Checkbox("Auto-apply denoiser compatibility preset", &isCompatPresetAutoApplied);
        helpMarker("On a denoiser switch, and after a DI preset with DLSS-RR, set the ReSTIR GI fields that depend on "
                   "the denoiser: history length, age limit, duplication history reduction, decorrelation and the "
                   "boiling filter. Off: ReSTIR PT's non-DLSS-RR profile. DLSS-RR: the shipped values, the engine's "
                   "DLSS-RR profile.");

        ImGui::BeginDisabled(renderer.denoiser != DenoiserMode::DlssRr);
        if (beginNode("DLSS-RR", kDenoisingDlss)) {
            Dlss& dlss = renderer.getDlss();
            beginRow({"frame.dlssQuality"});
            constexpr std::array kQualityModes{"DLAA", "Quality", "Balanced", "Performance", "Ultra Performance"};
            int quality = static_cast<int>(dlss.settings.quality);
            if (ImGui::Combo("Mode", &quality, kQualityModes.data(), static_cast<int>(kQualityModes.size()))) {
                dlss.settings.quality = static_cast<Dlss::Quality>(quality);
            }
            endRow("render resolution: DLAA renders at display size. A change recreates the feature and the render "
                   "targets, and resets history (RTXGI sample: DLSS Mode)");
            beginRow({"frame.dlssPreset"});
            constexpr std::array kPresets{"Default", "D (transformer)", "E (latest transformer)", "F (RR2)"};
            int preset = static_cast<int>(dlss.settings.preset);
            if (ImGui::Combo("Preset", &preset, kPresets.data(), static_cast<int>(kPresets.size()))) {
                dlss.settings.preset = static_cast<Dlss::Preset>(preset);
            }
            endRow("model hint; an NVIDIA app override takes precedence (NGX logs it as a DRS override)");
            beginRow({"frame.dlssSpecularHitDistance"});
            ImGui::Checkbox("Specular hit distance guide", &dlss.guides.isSpecularHitDistanceUsed);
            endRow("feeds the primary bounce's specular-lobe hit distance to ray reconstruction "
                   "(pInSpecularHitDistance)");
            if (ImGui::Button("Reset history")) {
                renderer.resetDlssHistory();
            }
            ImGui::SetItemTooltip("the next Lit frame starts without accumulated history");
            const vk::Extent2D render = dlss.getRenderExtent();
            const vk::Extent2D display = dlss.getDisplayExtent();
            ImGui::Text("Render %ux%u -> display %ux%u (%.2fx), %u jitter phases", render.width, render.height,
                        display.width, display.height,
                        static_cast<float>(display.width) / static_cast<float>(std::max(render.width, 1u)),
                        camera.getJitterPhaseCount());
            if (!renderer.isDlssEvaluated()) {
                ImGui::TextDisabled(renderer.debugView.view != DebugView::Lit ? "evaluated only in the Lit view"
                                        : !renderer.isLitPresented()          ? "not evaluated: no lights, Lit shows "
                                                                                "unlit albedo"
                                                                              : "not evaluated");
            }
            endNode();
        }
        ImGui::EndDisabled();
        endNode();
    }
    ImGui::Separator();
}

void SettingsWindow::drawPostProcessing()
{
    using namespace settingsRows;
    if (beginNode("Post-Processing", kPost, {}, kColorRegularHeader)) {
        const bool isDlssRr = renderer.denoiser == DenoiserMode::DlssRr;
        ImGui::BeginDisabled(isDlssRr);
        int aa = std::to_underlying(renderer.aaMode);
        beginRow({"frame.aa"});
        bool isChanged = ImGui::RadioButton("No AA", &aa, std::to_underlying(AaMode::None));
        ImGui::SameLine();
        isChanged |= ImGui::RadioButton("Accumulation", &aa, std::to_underlying(AaMode::Accumulation));
        ImGui::SameLine();
        ImGui::BeginDisabled();
        int unavailable = -1;
        ImGui::RadioButton("TAAU", &unavailable, 0);
        ImGui::SameLine();
        ImGui::RadioButton("DLSS SR", &unavailable, 0);
        ImGui::EndDisabled();
        endRow("Engine: TAAU and DLSS SR are not in the engine. Accumulation averages noisyColor over still frames "
               "(linear, before exposure); any setting edit, a camera, sun or scene change resets it.");
        if (isChanged) {
            renderer.aaMode = static_cast<AaMode>(aa);
        }
        ImGui::EndDisabled();
        if (isDlssRr) {
            ImGui::TextDisabled("Post processing provided by DLSS-RR");
        }

        ImGui::PushItemWidth(kFrameLimitWidth);
        beginRow({"frame.accumulationFrames"});
        const uint32_t minimum = 0;
        ImGui::DragScalar("Accum. Frame Limit", ImGuiDataType_U32, &renderer.accumulationFrameLimit, 1.0f, &minimum,
                          &kMaxAccumulationFrames, "%u", ImGuiSliderFlags_AlwaysClamp);
        endRow("Engine: 0 = no limit; at the limit the mean becomes an exponential average of that length.");
        ImGui::PopItemWidth();
        if (renderer.aaMode == AaMode::Accumulation && !isDlssRr) {
            ImGui::SameLine();
            ImGui::TextDisabled("// %u frame(s)", renderer.getAccumulatedFrames());
        }

        notInEngine("Apply Textures in Compositing");
        PostProcess& post = renderer.getPost();
        PostProcess::Settings& settings = post.settings;
        fixedCheckbox("Tone mapping", true);
        ImGui::SameLine(kToneMappingIndent);
        if (settings.isAutoExposure) {
            sliderFloat("Exposure bias", settings.compensationEv, -8.0f, 8.0f, "%+.1f EV");
        } else {
            sliderFloat("Exposure bias", settings.manualEv, settings.minEv, settings.maxEv, "%+.1f EV");
        }
        helpMarker("Engine: ACES (Hill's fit), always on. The bias is the auto exposure's compensation (-8 to +8 EV, "
                   "NVIDIA -4 to +2: the scenes range far from middle grey), or the manual exposure within the "
                   "exposure range without auto exposure.");
        notInEngine("Bloom");

        if (beginNode("Engine", {})) {
            if (!renderer.isLitPresented()) {
                ImGui::TextDisabled(renderer.debugView.view == DebugView::Lit ? "inactive: no lights, Lit shows unlit "
                                                                                "albedo"
                                                                              : "inactive: runs only in the Lit view");
            }
            const GpuExposureReadback& exposure = post.getLastExposure();
            ImGui::Text("Exposure %.3g (%+.2f EV), target %.3g", exposure.exposure,
                        exposure.exposure > 0.0f ? std::log2(exposure.exposure) : 0.0f, exposure.targetExposure);
            ImGui::Text("Average luminance %.3g from %u metered pixels", exposure.averageLuminance, exposure.pixels);
            ImGui::Checkbox("Auto exposure", &settings.isAutoExposure);
            ImGui::SetItemTooltip("targets middle grey using the 40th to 95th luminance percentiles, including black "
                                  "pixels");
            ImGui::BeginDisabled(!settings.isAutoExposure);
            ImGui::SliderFloat("Light adaptation", &settings.adaptationSpeedUp, 0.1f, 10.0f, "%.1f /s",
                               ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
            ImGui::SetItemTooltip("halves the EV gap in 1 / speed seconds as the scene gets brighter");
            ImGui::SliderFloat("Dark adaptation", &settings.adaptationSpeedDown, 0.1f, 10.0f, "%.1f /s",
                               ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
            ImGui::SetItemTooltip("halves the EV gap in 1 / speed seconds as the scene gets darker");
            ImGui::EndDisabled();
            ImGui::DragFloatRange2("Exposure range", &settings.minEv, &settings.maxEv, 0.1f, -32.0f, 32.0f, "%+.1f EV",
                                   nullptr, ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::Button("Snap to target")) {
                post.requestSnap();
            }
            ImGui::SameLine();
            if (ImGui::Button("Defaults##post")) {
                settings = {};
            }
            endNode();
        }
        endNode();
    }
    ImGui::Separator();
}

void SettingsWindow::drawConfig()
{
    if (ImGui::Button("Defaults")) {
        RtxdiContext& rtxdi = renderer.getRtxdi();
        renderer.getRayTracingScene().settings = {};
        rtxdi.isReference = false;
        rtxdi.isSpatialOnly = false;
        rtxdi.isTemporalOnly = false;
        rtxdi.settings = RtxdiContext::defaultSettings();
        rtxdi.isGiReference = false;
        rtxdi.giSettings = RtxdiContext::defaultGiSettings();
        rtxdi.giReferenceBounces = 0;
        renderer.getDlss().settings = {};
        renderer.getDlss().guides = {};
        renderer.dlssInputScale = 1.0f;
        renderer.preDenoiserClamp = 0.0f;
        renderer.inputHistoryWeight = 0.0f;
        renderer.extraFrameFlags = 0;
        renderer.getNrc().settings = {};
        renderer.indirectMode = kDefaultIndirectMode;
        renderer.denoiser = DenoiserMode::DlssRr;
        renderer.aaMode = AaMode::None;
        renderer.accumulationFrameLimit = 0;
        renderer.glass = {};
    }
    ImGui::SetItemTooltip("every setting the config string carries, back to shipped");
    // the settings lab's config string (spec/features/planned/full_testing.md §2.1): an A/B found here becomes a plan
    // line or an ENGINE_CONFIG value
    const std::string config = formatLabConfig(captureLabConfig(renderer, camera));
    ImGui::SameLine();
    if (ImGui::Button("Copy config")) {
        ImGui::SetClipboardText(config.c_str());
    }
    ImGui::SetItemTooltip("copies the lighting settings as an ENGINE_CONFIG / lab plan string");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kHelpWrapWidth);
    ImGui::TextUnformatted(config.c_str());
    ImGui::PopTextWrapPos();
}
#endif
