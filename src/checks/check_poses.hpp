#pragma once

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string_view>

// the sunlit Sponza courtyard view of the GI check, the temporal checks and ENGINE_BENCH_POSE=courtyard; the gallery
// pose is the startup camera
struct CourtyardPose
{
    glm::vec3 position;
    glm::vec3 target;
};
inline constexpr CourtyardPose kCourtyardPose{{6.0f, 2.5f, 1.5f}, {-10.0f, 1.0f, -1.0f}};

// what the GI check and the temporal checks render (ENGINE_CHECK_SCENE): the startup Sponza by default
enum class CheckScene : uint8_t
{
    Sponza,
    // Bistro's café interior: lit by emissive triangles only (m8 F21)
    BistroCafe,
    // Bistro's glass (m8 I13) under kBistroGlassSun*: the café windows from the street, the bar shelf's bottles
    Bistro,
};

struct CheckPose
{
    const char* name;
    // the startup camera, else lookAt(position, target)
    bool isStartup;
    glm::vec3 position;
    glm::vec3 target;
};

inline constexpr std::array<CheckPose, 2> kSponzaCheckPoses = {{
    {"gallery (startup)", true, {}, {}},
    {"courtyard", false, kCourtyardPose.position, kCourtyardPose.target},
}};
// the room spans x -12.3..1.5, z 0.9..16.1, floor y 0.4, ceiling ~3.7; ceiling lamps at x -3..-7, wall lights on the
// x -10.5 and z 14 walls. Tables: across the room toward the bar; dining: the back room's tables, wall lights and
// frosted windows
inline constexpr std::array<CheckPose, 2> kBistroCafeCheckPoses = {{
    {"cafe tables", false, {-0.5f, 1.7f, 9.0f}, {-10.5f, 1.2f, 8.0f}},
    {"cafe dining", false, {-5.5f, 1.7f, 8.0f}, {-7.0f, 1.0f, 15.5f}},
}};

// the café's big street windows face +z at z 13.6..16.1 (normal (-0.37, 0, 0.93)); the bar shelf's bottles stand at
// x 0..1, z 8..9. Street: through the windows into the café; bar: the shelf from across the counter
inline constexpr std::array<CheckPose, 2> kBistroGlassCheckPoses = {{
    {"street windows", false, {-7.5f, 1.7f, 20.0f}, {-6.0f, 1.5f, 14.5f}},
    {"bar shelf", false, {-3.5f, 1.7f, 9.0f}, {1.2f, 1.7f, 8.8f}},
}};
// from the street side of the windows (normal azimuth ~112 deg), lighting the terrace. No angle puts direct sun on
// the café floor behind them: the awning blocks a high sun, the buildings across the street a low one (I13 §8)
inline constexpr float kBistroGlassSunAzimuthDegrees = 110.0f;
inline constexpr float kBistroGlassSunElevationDegrees = 35.0f;

[[nodiscard]] inline CheckScene readCheckScene()
{
    const char* text = std::getenv("ENGINE_CHECK_SCENE");
    const std::string_view scene = text != nullptr ? std::string_view(text) : std::string_view();
    if (scene == "bistro_cafe") {
        return CheckScene::BistroCafe;
    }
    return scene == "bistro" ? CheckScene::Bistro : CheckScene::Sponza;
}

[[nodiscard]] inline const std::array<CheckPose, 2>& checkPoses(CheckScene scene)
{
    switch (scene) {
    case CheckScene::BistroCafe:
        return kBistroCafeCheckPoses;
    case CheckScene::Bistro:
        return kBistroGlassCheckPoses;
    case CheckScene::Sponza:
        break;
    }
    return kSponzaCheckPoses;
}

// below ENGINE_MODELS_DIR; nullptr: the engine's startup model
[[nodiscard]] inline const char* checkSceneModel(CheckScene scene)
{
    return scene == CheckScene::Sponza ? nullptr : "bistro/bistro.gltf";
}
