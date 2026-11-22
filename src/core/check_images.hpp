#pragma once

#include "render/vk_renderer.hpp"

#include <span>
#include <string_view>

// PNG dumps of the GI check and the temporal checks (ENGINE_CHECK_PNG=1), into logs/<folder> next to logs/engine.log:
// logs/checks for the checks, logs/lab for the settings lab. The name is sanitized into a file stem
[[nodiscard]] bool isCheckPngRequested();

// what the screen showed (the capture's display-linear RGB, sRGB encoded)
void writeCheckCapturePng(std::string_view name, const Renderer::DisplayCapture& capture,
                          std::string_view folder = "checks");
// gray luminance, value * scale through Reinhard; scale maps the image's key to mid gray
void writeCheckLuminancePng(std::string_view name, vk::Extent2D extent, std::span<const double> values, double scale,
                            std::string_view folder = "checks");
// signed error against the reference, both * scale through Reinhard: red too bright, blue too dark, over a dim gray
// copy of the reference
void writeCheckErrorPng(std::string_view name, vk::Extent2D extent, std::span<const double> values,
                        std::span<const double> reference, double scale, std::string_view folder = "checks");
// gray value clamped to [0, 1], e.g. a coefficient of variation
void writeCheckUnitPng(std::string_view name, vk::Extent2D extent, std::span<const double> values,
                       std::string_view folder = "checks");
// the scale of writeCheckLuminancePng / writeCheckErrorPng that puts the image's mean at mid gray
[[nodiscard]] double checkPngScale(std::span<const double> values);
