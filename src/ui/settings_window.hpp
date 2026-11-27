#pragma once
#include "../Constants.h"

#if ENGINE_ENABLE_IMGUI
    #include "render/settings_registry.hpp"
    #include "settings_rows.hpp"

    #include "imgui.h"

    #include <cstdint>
    #include <initializer_list>
    #include <optional>
    #include <vector>

class Camera;
class Renderer;

// the RTXDI FullSample's Settings window (spec/features/planned/nvidia_settings_layout.md §2): NVIDIA's sections,
// order, labels, ranges and help text, so an engine config and a sample config compare row by row. Rows the engine
// lacks stay disabled; the engine's extra rows sit in one Engine node per section. Compare with highlights the rows
// whose lab switch differs from a comparison config (§5)
class SettingsWindow
{
public:
    SettingsWindow(Renderer& renderer, Camera& camera) : renderer(renderer), camera(camera) {}

    void draw();

private:
    // §5 entries
    enum class CompareEntry : uint8_t
    {
        None,
        Shipped,
        EngineTargetRis,
        EngineTargetTemporal,
        RtxptAnalog,
        RtxdiAnalog,
        RtxgiAnalog,
        NvidiaFast,
        NvidiaMedium,
        NvidiaUnbiased,
        NvidiaUltra,
        NvidiaReference,
        Tuned,
        Converged,
        Count,
    };
    // the DI Preset combo (§3): NVIDIA's list (Medium ships), then the engine's sets
    enum class DiPreset : uint8_t
    {
        Custom,
        Fast,
        Medium,
        Unbiased,
        Ultra,
        Reference,
        Tuned,
        Converged,
        Count,
    };

    void drawCompareWith();
    void drawPipeline();
    void drawScene();
    void drawGeneralRendering();
    void drawStaticContext();
    void drawDirectLighting();
    void drawDirectInitialSampling();
    void drawDirectTemporal();
    void drawDirectSpatial();
    void drawDirectFinalShading();
    void drawDirectEngine();
    void drawIndirectLighting();
    void drawIndirectNrc();
    void drawIndirectSharc();
    void drawIndirectResampling();
    void drawIndirectEngine();
    void drawDenoising();
    void drawPostProcessing();
    void drawConfig();

    // the pipeline preset matching the current toggles/settings, else Custom
    [[nodiscard]] PipelinePreset currentPipelinePreset() const;
    // the preset whose DI state equals the current one, else Custom
    [[nodiscard]] DiPreset currentDiPreset() const;
    void applyDiPreset(DiPreset preset);
    void setDenoiser(DenoiserMode mode);

    // a tree node whose label counts the differing switches of its rows and of nested nodes; open: its rows become the
    // rows a beginRow may name until endNode
    [[nodiscard]] bool beginNode(const char* label, SettingsRows rows, std::initializer_list<SettingsRows> nested = {},
                                 std::optional<ImU32> color = std::nullopt, ImGuiTreeNodeFlags flags = 0);
    void endNode();
    // a row editing these switches: its label takes the comparison color while one differs
    void beginRow(std::initializer_list<const char*> switches);
    // NVIDIA's (?) marker, first with the comparison's values of a highlighted row
    void endRow(const char* help = nullptr);
    [[nodiscard]] const LabSwitch* differingSwitch(const char* name) const;
    [[nodiscard]] uint32_t countDiffering(SettingsRows rows, std::initializer_list<SettingsRows> nested) const;

    Renderer& renderer;
    Camera& camera;
    CompareEntry compareEntry = CompareEntry::Shipped;
    LabConfig comparison;
    // switches whose current value differs from comparison, this UI frame
    std::vector<const LabSwitch*> differing;
    std::vector<SettingsRows> openNodes;
    std::vector<const LabSwitch*> rowDiffering;
    bool isRowHighlighted = false;
    bool isAdvancedShown = false;
    bool isCompatPresetAutoApplied = true;
};
#endif
