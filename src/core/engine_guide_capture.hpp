#pragma once

#include "render/debug_view.hpp"
#include "render/vk_renderer.hpp"
#include "scene/vk_camera.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// guide captures for the E1 comparison with the Khronos screenshots and the A4 shading-attribute checks
// (spec/pbr_implementation_plan.md E1, A4): each model alone at the origin, framed from a fixed direction, every guide
// view written as a PNG under logs/guides; then the normal checks compare captures pixel by pixel, and the convergence
// check compares ReSTIR DI's mean against the exact punctual sum (spec/features/rtxdi_3_2_update.md §4).
// ENGINE_GUIDE_CAPTURE=1 enables the run; the engine quits once the last shot is written and the checks are logged
class EngineGuideCapture
{
public:
    [[nodiscard]] static bool isRequested();

    // the engine sun's direction (WorldLights::Sun)
    struct SunAngles
    {
        float azimuthDegrees = 0.0f;
        float elevationDegrees = 0.0f;
    };

    // what only the engine can do for the capture before this frame
    struct Request
    {
        // replaces every entity with this model at the origin
        std::optional<std::filesystem::path> model;
        // the engine sun at its default illuminance; off when nullopt
        std::optional<SunAngles> sun;
        bool shouldCapture = false;
        IndirectMode indirectMode = IndirectMode::NrcRestirGi;
    };

    // modelBounds: entity 0's model-space bounding sphere, once the scene has an entity. rtxdi holds
    // RtxdiContext::convergedSettings through the convergence shot
    [[nodiscard]] Request beginFrame(Camera& camera, DebugViewSettings& debugView, RtxdiContext::Settings& rtxdi,
                                     std::optional<glm::vec4> modelBounds);
    // after the frame's fence; true once every shot is written and the checks are logged
    [[nodiscard]] bool endFrame(const std::optional<Renderer::DisplayCapture>& capture);

private:
    // a copy of the model under logs/guides/variants/<shot name> with attributes removed from its .gltf
    enum class Variant : uint8_t
    {
        AsShipped,
        NoTangents,
        // TANGENT goes too: glTF tangents require normals
        NoNormals,
    };

    // a camera in the model's space, which is world space: the model sits at the origin
    struct CameraPose
    {
        glm::vec3 position;
        glm::vec3 target;
    };

    struct Shot
    {
        // model folder, relative to the repository root
        const char* folder;
        // PNG file prefix
        const char* name;
        Variant variant;
        // from the bounds center towards the camera; normalized on use
        glm::vec3 viewDirection;
        // replaces the framing along viewDirection
        std::optional<CameraPose> pose;
        // the samples ship their own lights, as in the Khronos screenshots: the engine sun stays off unless set
        std::optional<SunAngles> sun;
    };

    // two captures whose normals must agree pixel by pixel
    struct NormalCheck
    {
        const char* name;
        size_t shot;
        DebugView view;
        size_t referenceShot;
        DebugView referenceView;
    };

    static constexpr glm::vec3 kFront{0.0f, 0.0f, 1.0f};
    static constexpr size_t kMirrorShippedShot = 4;
    static constexpr size_t kMirrorGeneratedShot = 5;
    static constexpr size_t kFlatNormalsShot = 7;
    static constexpr std::array<Shot, 15> kShots = {{
        {"gltf/2.0/MetalRoughSpheres/glTF", "MetalRoughSpheres_front", Variant::AsShipped, kFront},
        // the plane lies in xz with its normal +y
        {"gltf/2.0/TwoSidedPlane/glTF", "TwoSidedPlane_top", Variant::AsShipped, {0.0f, 0.8f, 0.6f}},
        {"gltf/2.0/TwoSidedPlane/glTF", "TwoSidedPlane_bottom", Variant::AsShipped, {0.0f, -0.8f, 0.6f}},
        // off axis, so every visible face shows the same unshaded color
        {"gltf/2.0/UnlitTest/glTF", "UnlitTest_front", Variant::AsShipped, {0.35f, 0.3f, 1.0f}},
        // A4: the shipped tangents, then the generator on the same mesh (mirrored UVs included)
        {"models/NormalTangentMirrorTest", "NormalTangentMirrorTest_front", Variant::AsShipped, kFront},
        {"models/NormalTangentMirrorTest", "NormalTangentMirrorTest_generated_tangents_front", Variant::NoTangents,
         kFront},
        // ships no TANGENT: always the generated path
        {"models/NormalTangentTest", "NormalTangentTest_front", Variant::AsShipped, kFront},
        // A4: no NORMAL and no normal texture, so the shading normal is the generated flat normal
        {"gltf/2.0/MetalRoughSpheres/glTF", "MetalRoughSpheres_flat_normals_front", Variant::NoNormals, kFront},
        // B1: KHR_texture_transform on base color, then on every slot and both uv sets
        {"gltf/2.0/TextureTransformTest/glTF", "TextureTransformTest_front", Variant::AsShipped, kFront},
        {"gltf/2.0/TextureTransformMultiTest/glTF", "TextureTransformMultiTest_front", Variant::AsShipped, kFront},
        // E2: point lights (candela, range window) and a directional one (lux) through the world light buffer
        {"gltf/2.0/LightsPunctualLamp/glTF", "LightsPunctualLamp_front", Variant::AsShipped, kFront},
        {"gltf/2.0/DirectionalLight/glTF", "DirectionalLight_front", Variant::AsShipped, kFront},
        // D4 (spec/m6/d4_ray_contract.md): cut-outs in raster and in shadows. The panels stand 0.47 in front of the
        // backdrop; a sun from the front right throws their shadows down and left onto it, past the panel edges and
        // through the cut-outs
        {"gltf/2.0/AlphaBlendModeTest/glTF", "AlphaBlendModeTest_sun", Variant::AsShipped, {0.0f, 0.35f, 1.0f},
         std::nullopt, SunAngles{60.0f, 35.0f}},
        // the courtyard vase plant and column vine under the default sun (WorldLights::defaultSun)
        {"models/Sponza", "Sponza_plants", Variant::AsShipped, kFront,
         CameraPose{{-0.48f, 5.21f, -0.31f}, {-0.18f, 4.51f, -0.91f}}, SunAngles{20.0f, 65.0f}},
        // the shadow-terminator check (D4 §1.2): the upper colonnade from the courtyard, a sun lower than the default
        // along the courtyard; the column shafts and arches turn away from it. Shadowed lights is the clean view
        {"models/Sponza", "Sponza_terminator", Variant::AsShipped, kFront,
         CameraPose{{0.0f, 4.0f, 2.5f}, {-4.0f, 7.0f, -4.0f}}, SunAngles{20.0f, 40.0f}},
    }};
    static constexpr std::array<NormalCheck, 2> kNormalChecks = {{
        {"A4 generated tangents match the shipped ones (NormalTangentMirrorTest shading normal)", kMirrorGeneratedShot,
         DebugView::ShadingNormal, kMirrorShippedShot, DebugView::ShadingNormal},
        {"A4 flat normals without NORMAL (MetalRoughSpheres shading normal against the geometric normal)",
         kFlatNormalsShot, DebugView::ShadingNormal, kFlatNormalsShot, DebugView::GeometricNormal},
    }};
    // a view in the default indirect mode, or a lighting view in another mode (spec/m4/g7_indirect_modes.md); the
    // mode switches on the captured frame, so ReSTIR GI shows its first frame without history
    struct ShotView
    {
        DebugView view;
        IndirectMode mode;
    };
    static constexpr std::array<ShotView, 21> kViews = {{
        {DebugView::DiffuseAlbedo, IndirectMode::NrcRestirGi},
        {DebugView::SpecularF0, IndirectMode::NrcRestirGi},
        {DebugView::Roughness, IndirectMode::NrcRestirGi},
        {DebugView::ShadingNormal, IndirectMode::NrcRestirGi},
        {DebugView::GeometricNormal, IndirectMode::NrcRestirGi},
        {DebugView::Emissive, IndirectMode::NrcRestirGi},
        {DebugView::AmbientOcclusion, IndirectMode::NrcRestirGi},
        {DebugView::UnshadowedLights, IndirectMode::NrcRestirGi},
        {DebugView::ShadowedLights, IndirectMode::NrcRestirGi},
        {DebugView::DirectLighting, IndirectMode::NrcRestirGi},
        {DebugView::RtxdiReservoirs, IndirectMode::NrcRestirGi},
        {DebugView::IndirectSamples, IndirectMode::NrcRestirGi},
        {DebugView::NrcCache, IndirectMode::NrcRestirGi},
        {DebugView::IndirectRadiance, IndirectMode::NrcRestirGi},
        {DebugView::DlssInput, IndirectMode::NrcRestirGi},
        {DebugView::IndirectSamples, IndirectMode::Nrc},
        {DebugView::IndirectRadiance, IndirectMode::Nrc},
        {DebugView::DlssInput, IndirectMode::Nrc},
        // no NRC cache view: the cache is paused in this mode
        {DebugView::IndirectSamples, IndirectMode::RestirGi},
        {DebugView::IndirectRadiance, IndirectMode::RestirGi},
        {DebugView::DlssInput, IndirectMode::RestirGi},
    }};
    // ReSTIR DI (Direct lighting) averaged over frames against Shadowed lights on the lamp shot, after its views
    static constexpr size_t kConvergenceShot = 10;
    static constexpr uint32_t kConvergenceWarmupFrames = 16;
    static constexpr uint32_t kConvergenceFrames = 64;
    static constexpr uint32_t kConvergenceTileSize = 16;
    // shares of the reference's summed luminance; RTXDI 3.1's fused pass read +0.13% and 2.43%, its Basic bug -35%
    static constexpr double kMaxConvergenceBias = 0.005;
    static constexpr double kMaxConvergenceTileP90 = 0.03;
    // the load frame, then frames with the shot's camera before the first capture
    static constexpr uint32_t kSettleFrames = 3;
    // bounding sphere to frame height
    static constexpr float kFramingMargin = 1.05f;
    // a check passes when this share of the compared pixels is within kMaxNormalAngleDegrees
    static constexpr double kNormalAngleQuantile = 0.999;
    // matching captures differ by at most 0.08 deg (rgba16f display, rg16 octahedral geometric normal); smooth sphere
    // normals sit 1.1 deg from their facets on average, and a tangent frame built on the file's v up to 129 deg off
    static constexpr float kMaxNormalAngleDegrees = 0.25f;

    // the model file to load for the shot; nullopt when it is missing or its variant could not be written
    [[nodiscard]] static std::optional<std::filesystem::path> prepareModel(const Shot& shot);
    // false when the file could not be written
    [[nodiscard]] bool writePng(const Renderer::DisplayCapture& capture) const;
    // keeps the capture when a normal check reads it
    void keepNormals(const Renderer::DisplayCapture& capture);
    // logs every check; the number that passed
    [[nodiscard]] size_t evaluateNormalChecks() const;
    // one frame of the convergence run; true after its last frame
    [[nodiscard]] bool advanceConvergence(const std::optional<Renderer::DisplayCapture>& capture);
    // logs the metrics; true when within the limits
    [[nodiscard]] bool evaluateConvergence() const;
    // next shot; true after the last one
    bool finishShot();

    size_t shotIndex = 0;
    size_t viewIndex = 0;
    uint32_t shotFrame = 0;
    uint32_t writtenCount = 0;
    // the loaded shot's folder and variant
    std::optional<std::pair<std::string, Variant>> loadedModel;
    // decoded normals per (shot, view) a check reads; zero where the pixel is empty
    std::map<std::pair<size_t, DebugView>, std::vector<glm::vec3>> keptNormals;

    struct ConvergenceRun
    {
        vk::Extent2D extent;
        // Shadowed lights luminance of the convergence shot
        std::vector<float> reference;
        // tested luminance summed over the captured frames
        std::vector<double> sum;
        std::vector<float> previous;
        // over frames of the pixel sum of |frame - reference| and of |frame - previous frame|
        double absoluteErrorSum = 0.0;
        double changeSum = 0.0;
        // frames since the run began, warm-up included
        uint32_t frame = 0;
        uint32_t capturedFrames = 0;
    };
    // engaged from the reference capture; the run begins after the shot's last view
    std::optional<ConvergenceRun> convergence;
    bool isConvergenceRunning = false;
    // set once the run is evaluated
    std::optional<bool> isConvergencePassed;
    // the settings replaced while the convergence shot runs
    std::optional<RtxdiContext::Settings> heldRtxdiSettings;
};
