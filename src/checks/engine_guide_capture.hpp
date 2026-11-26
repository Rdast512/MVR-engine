#pragma once

#include "checks/check_passes.hpp"
#include "checks/check_poses.hpp"
#include "render/debug_view.hpp"
#include "render/vk_renderer.hpp"
#include "scene/vk_camera.hpp"

#include <algorithm>
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
// check compares ReSTIR DI's mean against the exact punctual sum (spec/features/done/rtxdi_3_2_update.md §4), or against
// RTXDI's no-reuse mode where triangle lights take part (spec/m7/e2_emissive_triangles.md), and the glass
// throughput check holds the glass pass's composite against its throughput (m8 I13 §5).
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
        // RtxdiContext::isReference: RTXDI without reuse, the convergence reference of emissive shots
        bool isRtxdiReference = false;
        // WorldLights::areTriangleLightsSampled
        bool areTriangleLightsSampled = true;
        // the shipped settings outside the throughput check
        GlassSettings glass;
        // RtxdiContext::frameIndexOverride and Camera::jitterPhaseOverride: the throughput check's phases repeat
        // frame for frame
        std::optional<uint32_t> frameIndex;
        std::optional<uint64_t> jitterPhase;
        // PostProcess::Settings::manualEv; auto exposure when nullopt
        std::optional<float> manualEv;
    };

    // modelBounds: entity 0's model-space bounding sphere, once the scene has an entity. rtxdi holds
    // RtxdiContext::convergedSettings through the convergence shot
    [[nodiscard]] Request beginFrame(Camera& camera, DebugViewSettings& debugView, RtxdiContext::Settings& rtxdi,
                                     std::optional<glm::vec4> modelBounds);
    // after the frame's fence; true once every shot is written and the checks are logged
    [[nodiscard]] bool endFrame(const std::optional<DisplayCapture>& capture);
    [[nodiscard]] bool isPassed() const noexcept
    {
        return isFinished && normalPassedCount == kNormalChecks.size() &&
            convergenceCheckCount == kConvergenceShots.size() && convergencePassedCount == kConvergenceShots.size() &&
            throughputCheckCount == kThroughputShots.size() && throughputPassedCount == kThroughputShots.size();
    }

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
    static constexpr std::array<Shot, 22> kShots = {{
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
        // M7 E2 (spec/m7/e2_emissive_triangles.md): five cubes of emissive strength 1 to 16 over a backdrop they light,
        // then a textured emitter
        {"gltf/2.0/EmissiveStrengthTest/glTF", "EmissiveStrengthTest_front", Variant::AsShipped, kFront},
        {"gltf/2.0/DamagedHelmet/glTF", "DamagedHelmet_front", Variant::AsShipped, kFront},
        // KHR_materials_pbrSpecularGlossiness: the right bottle and label are spec-gloss, the left ones metal-rough;
        // the sample's contract is that both look almost the same
        {"gltf/2.0/SpecGlossVsMetalRough/glTF", "SpecGlossVsMetalRough_front", Variant::AsShipped, kFront},
        // m8 I13 thin-walled glass: smooth transmissive panels (textured, masked, metal and rough variants, the last
        // two rendering opaque) in front of a backdrop; the sample ships no light, so a sun from the front right
        {"gltf/2.0/TransmissionTest/glTF", "TransmissionTest_sun", Variant::AsShipped, kFront, std::nullopt,
         SunAngles{30.0f, 40.0f}},
        // a closed thin-walled mesh, roughness 0.1: two crossings per pixel, under its own lights
        {"gltf/2.0/TransmissionSuzanne/glTF", "TransmissionSuzanne_front", Variant::AsShipped, kFront},
        // Bistro's glass check poses and sun (check_poses.hpp)
        {"models/bistro/bistro.gltf", "Bistro_street_windows", Variant::AsShipped, kFront,
         CameraPose{kBistroGlassCheckPoses[0].position, kBistroGlassCheckPoses[0].target},
         SunAngles{kBistroGlassSunAzimuthDegrees, kBistroGlassSunElevationDegrees}},
        {"models/bistro/bistro.gltf", "Bistro_bar_shelf", Variant::AsShipped, kFront,
         CameraPose{kBistroGlassCheckPoses[1].position, kBistroGlassCheckPoses[1].target},
         SunAngles{kBistroGlassSunAzimuthDegrees, kBistroGlassSunElevationDegrees}},
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
    static constexpr std::array<ShotView, 23> kViews = {{
        {DebugView::DiffuseAlbedo, IndirectMode::NrcRestirGi},
        {DebugView::SpecularF0, IndirectMode::NrcRestirGi},
        {DebugView::Roughness, IndirectMode::NrcRestirGi},
        {DebugView::ShadingNormal, IndirectMode::NrcRestirGi},
        {DebugView::GeometricNormal, IndirectMode::NrcRestirGi},
        {DebugView::Emissive, IndirectMode::NrcRestirGi},
        {DebugView::AmbientOcclusion, IndirectMode::NrcRestirGi},
        // classify's resolve bins (spec/m7/e1_material_binning.md)
        {DebugView::MaterialBin, IndirectMode::NrcRestirGi},
        {DebugView::UnshadowedLights, IndirectMode::NrcRestirGi},
        {DebugView::ShadowedLights, IndirectMode::NrcRestirGi},
        {DebugView::DirectLighting, IndirectMode::NrcRestirGi},
        {DebugView::RtxdiReservoirs, IndirectMode::NrcRestirGi},
        {DebugView::IndirectSamples, IndirectMode::NrcRestirGi},
        {DebugView::NrcCache, IndirectMode::NrcRestirGi},
        {DebugView::IndirectRadiance, IndirectMode::NrcRestirGi},
        {DebugView::DlssInput, IndirectMode::NrcRestirGi},
        // the glass pass's throughput, white without glass (m8 I13)
        {DebugView::Glass, IndirectMode::NrcRestirGi},
        {DebugView::IndirectSamples, IndirectMode::Nrc},
        {DebugView::IndirectRadiance, IndirectMode::Nrc},
        {DebugView::DlssInput, IndirectMode::Nrc},
        // no NRC cache view: the cache is paused in this mode
        {DebugView::IndirectSamples, IndirectMode::RestirGi},
        {DebugView::IndirectRadiance, IndirectMode::RestirGi},
        {DebugView::DlssInput, IndirectMode::RestirGi},
    }};
    // what ReSTIR DI's mean is held against
    enum class ConvergenceReference : uint8_t
    {
        // Shadowed lights: the exact sum over the punctual lights, one frame
        ShadowedLights,
        // RTXDI without reuse (32 Power_RIS local + 8 infinite candidates) averaged over kReferenceFrames: the
        // reference of triangle lights, which Shadowed lights leaves out
        RtxdiNone,
    };
    struct ConvergenceShot
    {
        size_t shot;
        ConvergenceReference reference;
        // off: the run tests the punctual lights alone, as Shadowed lights sums them
        bool areTriangleLightsSampled;
        // over convergedSettings' 0: a BRDF ray names the triangle light it hits (RAB_TraceRayForLocalLight)
        uint32_t brdfSamples;
    };
    // ReSTIR DI (Direct lighting) averaged over frames after each shot's views: point and spot lights among the
    // emissive bulb's triangles (lamp; presampled selection, spec/m7/f1_light_presampling.md), the directional sun
    // with shadows (Sponza_terminator), triangle lights alone with one BRDF candidate, NVIDIA's Medium
    // (EmissiveStrengthTest)
    static constexpr std::array<ConvergenceShot, 3> kConvergenceShots = {{
        {10, ConvergenceReference::RtxdiNone, true, 0},
        {14, ConvergenceReference::ShadowedLights, true, 0},
        {15, ConvergenceReference::RtxdiNone, true, 1},
    }};
    static constexpr uint32_t kConvergenceWarmupFrames = 16;
    // presampled lights correlate error across RTXDI's 16x16 tiles (one RIS tile per screen tile), the check's tile
    // size: 64 frames leave Power_RIS at p90 4.3%, 512 bring it to 1.9%. 2048 since m8 I14: with the lamp's emitters
    // culled to 561, 8 candidates read -0.44% over 512 frames and -0.08% over 2048 (one window's shortfall, not bias)
    static constexpr uint32_t kConvergenceFrames = 2048;
    static constexpr uint32_t kReferenceFrames = 1024;
    static constexpr uint32_t kConvergenceTileSize = 16;
    // shares of the reference's summed luminance; RTXDI 3.1's fused pass read +0.13% and 2.43%, its Basic bug -35%
    static constexpr double kMaxConvergenceBias = 0.005;
    static constexpr double kMaxConvergenceTileP90 = 0.03;
    [[nodiscard]] static constexpr const ConvergenceShot* findConvergenceShot(size_t index) noexcept
    {
        for (const ConvergenceShot& shot : kConvergenceShots) {
            if (shot.shot == index) {
                return &shot;
            }
        }
        return nullptr;
    }
    [[nodiscard]] static constexpr std::optional<ConvergenceReference> convergenceReference(size_t index) noexcept
    {
        const ConvergenceShot* shot = findConvergenceShot(index);
        return shot != nullptr ? std::optional(shot->reference) : std::nullopt;
    }
    [[nodiscard]] static constexpr bool isConvergenceShot(size_t index) noexcept
    {
        return findConvergenceShot(index) != nullptr;
    }
    // the glass throughput check (m8 I13 §5) after the shot's views, three phases from a camera cut at the same frame
    // indices and exposure: the pass off, then on with transmission alone (no reflection, diffuse share, emission or
    // shadow transmittance), both in DLSS input, then the Glass view's throughput and layer count. Glass changes
    // nothing before the glass pass, so on / off is the composite's throughput, held against the pass's own
    // (C-I13-5: not a CPU evaluation of the layers)
    enum class ThroughputPhase : uint8_t
    {
        PassOff,
        Transmission,
        Throughput,
    };
    // TransmissionTest_sun, Bistro_street_windows
    static constexpr std::array<size_t, 2> kThroughputShots = {18, 20};
    // the captured frame of each phase
    static constexpr uint32_t kThroughputFrames = 8;
    // even: RTXDI's buffer parity follows the frame index
    static constexpr uint32_t kThroughputFrameIndexBase = 1u << 20;
    static constexpr float kThroughputManualEv = 0.0f;
    // over pixels behind one or two layers: L1 RGB error relative to throughput x pass-off color (spec I13 §5)
    static constexpr double kMaxThroughputMedianError = 0.01;
    static constexpr double kMaxThroughputP90Error = 0.03;
    // below this share of the compared pixels' mean the half-float pass-off color is too coarse to divide by
    static constexpr double kThroughputDarkShare = 1e-3;
    static constexpr size_t kMinThroughputPixels = 1000;
    [[nodiscard]] static constexpr bool isThroughputShot(size_t index) noexcept
    {
        return std::ranges::find(kThroughputShots, index) != kThroughputShots.end();
    }
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
    [[nodiscard]] bool writePng(const DisplayCapture& capture) const;
    // keeps the capture when a normal check reads it
    void keepNormals(const DisplayCapture& capture);
    // logs every check; the number that passed
    [[nodiscard]] size_t evaluateNormalChecks() const;
    // one frame of the convergence run; true after its last frame
    [[nodiscard]] bool advanceConvergence(const std::optional<DisplayCapture>& capture);
    // one frame of the RtxdiNone reference phase
    void accumulateReference(const DisplayCapture& capture);
    // logs the metrics; true when within the limits
    [[nodiscard]] bool evaluateConvergence() const;
    // one frame of the throughput check; true after its last phase
    [[nodiscard]] bool advanceThroughput(const std::optional<DisplayCapture>& capture);
    // logs the metrics; true when within the limits. glass: the Throughput phase's capture
    [[nodiscard]] bool evaluateThroughput(const DisplayCapture& glass) const;
    // the shot's camera; a cut
    static void frameShot(Camera& camera, const Shot& shot, std::optional<glm::vec4> modelBounds);
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
        size_t shotIndex = 0;
        vk::Extent2D extent{};
        // Shadowed lights luminance and RGB sum of the convergence shot, or the RtxdiNone frames' mean
        std::vector<float> reference;
        // RtxdiNone: frames still accumulate into referenceSum
        bool isReferencePhase = false;
        std::vector<double> referenceSum;
        std::array<double, 3> referenceRgb{0.0, 0.0, 0.0};
        uint64_t referenceNonFinite = 0;
        uint64_t referenceNegative = 0;
        // tested luminance and RGB summed over the captured frames
        std::vector<double> sum;
        std::array<double, 3> rgbSum{0.0, 0.0, 0.0};
        uint64_t nonFinitePixels = 0;
        uint64_t negativePixels = 0;
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
    bool isFinished = false;
    size_t normalPassedCount = 0;
    size_t convergenceCheckCount = 0;
    size_t convergencePassedCount = 0;
    struct ThroughputRun
    {
        ThroughputPhase phase = ThroughputPhase::PassOff;
        // frames into the phase
        uint32_t frame = 0;
        vk::Extent2D extent{};
        // the PassOff and Transmission captures, RGBA16F texels
        std::vector<uint16_t> passOff;
        std::vector<uint16_t> transmission;
    };
    // engaged after a throughput shot's last view
    std::optional<ThroughputRun> throughput;
    size_t throughputCheckCount = 0;
    size_t throughputPassedCount = 0;
    // the settings replaced while a convergence shot runs
    std::optional<RtxdiContext::Settings> heldRtxdiSettings;
};
