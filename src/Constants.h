#pragma once
#include <cstdint>
#include <filesystem>

constexpr int MAX_FRAMES_IN_FLIGHT = 2;
const uint32_t WIDTH = 800;
const uint32_t HEIGHT = 600;
constexpr uint64_t FenceTimeout = 100000000;
const uint32_t test = 42;

#ifndef ENGINE_SHADER_DIR
#define ENGINE_SHADER_DIR "./shaders"
#endif
#ifndef ENGINE_MODELS_DIR
#define ENGINE_MODELS_DIR "./models"
#endif
#ifndef ENGINE_TEXTURES_DIR
#define ENGINE_TEXTURES_DIR "./textures"
#endif

// ImGui master switch (0 = fully off, 1 = on).
// When 0: no ImGui context, SDL/Vulkan backends, descriptor pool, pipelines, or draws.
// Use 0 while running with validation layers so ImGui does not create GPU objects or noise.
// Override from CMake: target_compile_definitions(... ENGINE_ENABLE_IMGUI=1)
#ifndef ENGINE_ENABLE_IMGUI
#define ENGINE_ENABLE_IMGUI 0
#endif

inline const std::filesystem::path MODEL_PATH = std::filesystem::path(ENGINE_MODELS_DIR) / "room.obj";
inline const std::filesystem::path TEXTURE_PATH = std::filesystem::path(ENGINE_MODELS_DIR) / "viking_room.png";
