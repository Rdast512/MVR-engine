#pragma once

#include "../static_headers/logger.hpp"

#include <charconv>
#include <cstdlib>
#include <format>
#include <string_view>
#include <system_error>

// ENGINE_CHECK_FPS: the simulated frame rate of the deterministic runners' fixed clock (Renderer::fixedFrameDeltaMs:
// exposure adaptation and DLSS-RR's frame time). Per-frame algorithms (ReSTIR history, NRC, SHaRC) do not see it.
// Default 60, which every recorded baseline used
[[nodiscard]] inline float checkFrameDeltaMs()
{
    constexpr float kDefaultFps = 60.0f;
    const char* text = std::getenv("ENGINE_CHECK_FPS");
    if (text == nullptr || *text == '\0') {
        return 1000.0f / kDefaultFps;
    }
    const std::string_view view(text);
    float fps = 0.0f;
    const auto [end, error] = std::from_chars(view.data(), view.data() + view.size(), fps);
    if (error != std::errc{} || end != view.data() + view.size() || !(fps > 0.0f)) {
        logError(std::format("ENGINE_CHECK_FPS='{}' is not a positive number; using {}", view, kDefaultFps), "Checks");
        return 1000.0f / kDefaultFps;
    }
    logInfo(std::format("ENGINE_CHECK_FPS={}: fixed clock {:.3f} ms per frame", fps, 1000.0f / fps), "Checks");
    return 1000.0f / fps;
}
