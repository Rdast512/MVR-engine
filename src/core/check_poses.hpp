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
    // Bistro's café interior: lit by emissive triangles only, the windows' glass blocks the sun (m8 F21)
    BistroCafe,
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

[[nodiscard]] inline CheckScene readCheckScene()
{
    const char* text = std::getenv("ENGINE_CHECK_SCENE");
    return text != nullptr && std::string_view(text) == "bistro_cafe" ? CheckScene::BistroCafe : CheckScene::Sponza;
}

[[nodiscard]] inline const std::array<CheckPose, 2>& checkPoses(CheckScene scene)
{
    return scene == CheckScene::BistroCafe ? kBistroCafeCheckPoses : kSponzaCheckPoses;
}

// below ENGINE_MODELS_DIR; nullptr: the engine's startup model
[[nodiscard]] inline const char* checkSceneModel(CheckScene scene)
{
    return scene == CheckScene::BistroCafe ? "bistro/bistro.gltf" : nullptr;
}
