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
// view written as a PNG under logs/guides; then the normal checks compare captures pixel by pixel.
// ENGINE_GUIDE_CAPTURE=1 enables the run; the engine quits once the last shot is written and the checks are logged
class EngineGuideCapture
{
public:
    [[nodiscard]] static bool isRequested();

    // what only the engine can do for the capture before this frame
    struct Request
    {
        // replaces every entity with this model at the origin
        std::optional<std::filesystem::path> model;
        bool shouldCapture = false;
    };

    // modelBounds: entity 0's model-space bounding sphere, once the scene has an entity
    [[nodiscard]] Request beginFrame(Camera& camera, DebugViewSettings& debugView, std::optional<glm::vec4> modelBounds);
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

    struct Shot
    {
        // model folder, relative to the repository root
        const char* folder;
        // PNG file prefix
        const char* name;
        Variant variant;
        // from the bounds center towards the camera; normalized on use
        glm::vec3 viewDirection;
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
    static constexpr std::array<Shot, 8> kShots = {{
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
    }};
    static constexpr std::array<NormalCheck, 2> kNormalChecks = {{
        {"A4 generated tangents match the shipped ones (NormalTangentMirrorTest shading normal)", kMirrorGeneratedShot,
         DebugView::ShadingNormal, kMirrorShippedShot, DebugView::ShadingNormal},
        {"A4 flat normals without NORMAL (MetalRoughSpheres shading normal against the geometric normal)",
         kFlatNormalsShot, DebugView::ShadingNormal, kFlatNormalsShot, DebugView::GeometricNormal},
    }};
    static constexpr std::array<DebugView, 6> kViews = {
        DebugView::DiffuseAlbedo, DebugView::SpecularF0,      DebugView::Roughness,
        DebugView::ShadingNormal, DebugView::GeometricNormal, DebugView::Emissive,
    };
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
};
