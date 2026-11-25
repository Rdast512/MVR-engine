#pragma once

#include "imgui.h"

#include <cmath>

// the swapchain is *_SRGB, so ImGui's sRGB-authored colors must be stored linear
[[nodiscard]] inline ImVec4 srgbToLinear(const ImVec4& color)
{
    const auto channel = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    return {channel(color.x), channel(color.y), channel(color.z), color.w};
}
