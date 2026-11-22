#pragma once

#include "core/check_poses.hpp"

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

// what a lab run measures (spec/features/full_testing.md §4.2)
enum class LabKind : uint8_t
{
    Stability,
    DiBias,
    GiBias,
    LightChange,
    ScreenEdge,
    // bench-mode runs in their own process: the lab writes their commands, it never times its own frames
    Cost,
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(LabKind::Count)> kLabKindNames{
    "stability", "diBias", "giBias", "lightChange", "screenEdge", "cost"};

[[nodiscard]] constexpr uint32_t labKindBit(LabKind kind) { return 1u << static_cast<uint32_t>(kind); }

inline constexpr uint32_t kAllLabKinds = (1u << static_cast<uint32_t>(LabKind::Count)) - 1;

enum class LabFraming : uint8_t
{
    // Camera::focusOn(origin), the engine's startup camera for every model
    Startup,
    // lookAt(position, target)
    Pose,
    // the guide capture's framing: the model's bounding sphere fills the vertical field of view along position
    Bounds,
};

struct LabSun
{
    bool isEnabled = true;
    // nullopt: WorldLights::defaultSun's angles
    std::optional<float> azimuthDegrees;
    std::optional<float> elevationDegrees;
};

// a scene, a camera and the kinds that apply (spec §5); cases the engine checks share take their coordinates from
// check_poses.hpp, which stays the checks'
struct LabCase
{
    const char* name;
    // below the repository root (ENGINE_MODELS_DIR's parent)
    const char* model;
    LabFraming framing;
    // Pose: the camera position; Bounds: the direction from the bounds center toward the camera
    glm::vec3 position;
    glm::vec3 target;
    LabSun sun;
    uint32_t kinds;
    const char* stresses;
    // non-null: the case is planned, not runnable yet; names what it waits for
    const char* notBuilt = nullptr;
};

namespace labCases
{
    constexpr uint32_t kStabilityBiasKinds =
        labKindBit(LabKind::Stability) | labKindBit(LabKind::DiBias) | labKindBit(LabKind::GiBias);
    constexpr glm::vec3 kGuideFront{0.0f, 0.0f, 1.0f};
    // the guide capture's guide shots (engine_guide_capture.hpp kShots); the shots ship their own lights
    constexpr LabSun kSunOff{false, std::nullopt, std::nullopt};
} // namespace labCases

inline constexpr std::array<LabCase, 13> kLabCases = {{
    {"sponza.gallery", "models/Sponza", LabFraming::Startup, {}, {}, {}, kAllLabKinds,
     "sun bounce, indirect-dominated"},
    {"sponza.courtyard", "models/Sponza", LabFraming::Pose, kCourtyardPose.position, kCourtyardPose.target, {},
     kAllLabKinds, "direct sun, hard shadows"},
    {"bistro.street", "models/bistro/bistro.gltf", LabFraming::Startup, {}, {}, {},
     labKindBit(LabKind::Stability) | labKindBit(LabKind::DiBias) | labKindBit(LabKind::Cost),
     "76K emissive triangles plus sun, many lights"},
    {"cafe.tables", "models/bistro/bistro.gltf", LabFraming::Pose, kBistroCafeCheckPoses[0].position,
     kBistroCafeCheckPoses[0].target, {}, labCases::kStabilityBiasKinds | labKindBit(LabKind::ScreenEdge),
     "emissive-only interior, bar, mixed depth"},
    {"cafe.dining", "models/bistro/bistro.gltf", LabFraming::Pose, kBistroCafeCheckPoses[1].position,
     kBistroCafeCheckPoses[1].target, {}, labCases::kStabilityBiasKinds | labKindBit(LabKind::ScreenEdge),
     "emissive-only interior, small wall lights, frosted glass"},
    {"cafe.door", "models/bistro/bistro.gltf", LabFraming::Pose, {}, {}, {},
     labKindBit(LabKind::DiBias) | labKindBit(LabKind::GiBias),
     "a bright exterior next to a dark interior: the leak case for DI and GI",
     "T2: the pose is chosen from a frame PNG"},
    {"guide.lamp", "gltf/2.0/LightsPunctualLamp/glTF", LabFraming::Bounds, labCases::kGuideFront, {},
     labCases::kSunOff, labKindBit(LabKind::DiBias), "punctual light, BRDF candidate (I6's gate)"},
    {"guide.terminator", "models/Sponza", LabFraming::Pose, {0.0f, 4.0f, 2.5f}, {-4.0f, 7.0f, -4.0f},
     {true, 20.0f, 40.0f}, labKindBit(LabKind::DiBias), "shadow terminator, visibility reuse (I6's gate)"},
    {"guide.emissive", "gltf/2.0/EmissiveStrengthTest/glTF", LabFraming::Bounds, labCases::kGuideFront, {},
     labCases::kSunOff, labKindBit(LabKind::DiBias), "triangle lights only (I6's gate)"},
    {"glossy.floor", nullptr, LabFraming::Pose, {}, {}, {},
     labKindBit(LabKind::Stability) | labKindBit(LabKind::GiBias), "glossy reuse, final MIS, roughness floor",
     "I12: a smooth floor scene"},
    {"glossy.metal", nullptr, LabFraming::Pose, {}, {}, {},
     labKindBit(LabKind::Stability) | labKindBit(LabKind::GiBias), "glossy reuse on metal", "I12: a metal scene"},
    {"glass.window", nullptr, LabFraming::Pose, {}, {}, {},
     labKindBit(LabKind::DiBias) | labKindBit(LabKind::GiBias), "transmission, shadow transmittance",
     "I13: glass that transmits"},
    {"cafe.toggle", "models/bistro/bistro.gltf", LabFraming::Pose, kBistroCafeCheckPoses[1].position,
     kBistroCafeCheckPoses[1].target, {}, labKindBit(LabKind::LightChange),
     "lag of ReSTIR history indoors, where the sun toggle changes nothing",
     "T8: a per-material emissive scale (scene.emissiveScale)"},
}};

[[nodiscard]] constexpr const LabCase* findLabCase(std::string_view name)
{
    for (const LabCase& labCase : kLabCases) {
        if (name == labCase.name) {
            return &labCase;
        }
    }
    return nullptr;
}

[[nodiscard]] constexpr size_t labCaseIndex(const LabCase& labCase)
{
    return static_cast<size_t>(&labCase - kLabCases.data());
}
