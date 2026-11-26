#include "check_images.hpp"
#include "../static_headers/logger.hpp"

#include <glm/gtc/packing.hpp>
#include <SDL3/SDL_filesystem.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

namespace
{
    uint8_t encodeSrgb(double linear)
    {
        const double clamped = std::clamp(linear, 0.0, 1.0);
        const double encoded =
            clamped <= 0.0031308 ? clamped * 12.92 : 1.055 * std::pow(clamped, 1.0 / 2.4) - 0.055;
        return static_cast<uint8_t>(std::lround(encoded * 255.0));
    }

    double reinhard(double value) { return value > 0.0 ? value / (1.0 + value) : 0.0; }

    std::filesystem::path checkFile(std::string_view name, std::string_view folder)
    {
        std::string stem(name);
        std::ranges::replace_if(stem, [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); }, '_');
        const char* base = SDL_GetBasePath();
        const std::filesystem::path directory = std::filesystem::path(base != nullptr ? base : "") / "logs" / folder;
        std::error_code errorCode;
        std::filesystem::create_directories(directory, errorCode);
        return directory / (stem + ".png");
    }

    void writeRgba(std::string_view name, vk::Extent2D extent, const std::vector<uint8_t>& pixels,
                   std::string_view folder)
    {
        const std::filesystem::path file = checkFile(name, folder);
        stbi_write_png_compression_level = 1;
        if (stbi_write_png(file.string().c_str(), static_cast<int>(extent.width), static_cast<int>(extent.height), 4,
                           pixels.data(), static_cast<int>(extent.width) * 4) == 0) {
            logError(std::format("could not write {}", file.string()), "CheckImages");
        }
    }
} // namespace

bool isCheckPngRequested()
{
    const char* text = std::getenv("ENGINE_CHECK_PNG");
    return text != nullptr && std::string_view(text) == "1";
}

void writeCheckCapturePng(std::string_view name, const DisplayCapture& capture, std::string_view folder)
{
    const size_t texels = size_t{capture.extent.width} * capture.extent.height;
    std::vector<uint8_t> pixels(texels * 4);
    for (size_t texel = 0; texel < texels; ++texel) {
        for (size_t channel = 0; channel < 3; ++channel) {
            pixels[texel * 4 + channel] = encodeSrgb(glm::unpackHalf1x16(capture.rgba16f[texel * 4 + channel]));
        }
        pixels[texel * 4 + 3] = 255;
    }
    writeRgba(name, capture.extent, pixels, folder);
}

double checkPngScale(std::span<const double> values)
{
    double total = 0.0;
    for (const double value : values) {
        total += std::max(value, 0.0);
    }
    const double mean = values.empty() ? 0.0 : total / static_cast<double>(values.size());
    // the mean lands at Reinhard(0.5) = 1/3, about mid gray once sRGB encoded
    return mean > 0.0 ? 0.5 / mean : 1.0;
}

void writeCheckLuminancePng(std::string_view name, vk::Extent2D extent, std::span<const double> values, double scale,
                            std::string_view folder)
{
    std::vector<uint8_t> pixels(values.size() * 4);
    for (size_t texel = 0; texel < values.size(); ++texel) {
        const uint8_t gray = encodeSrgb(reinhard(values[texel] * scale));
        pixels[texel * 4] = gray;
        pixels[texel * 4 + 1] = gray;
        pixels[texel * 4 + 2] = gray;
        pixels[texel * 4 + 3] = 255;
    }
    writeRgba(name, extent, pixels, folder);
}

void writeCheckErrorPng(std::string_view name, vk::Extent2D extent, std::span<const double> values,
                        std::span<const double> reference, double scale, std::string_view folder)
{
    constexpr double kContext = 0.25;
    std::vector<uint8_t> pixels(values.size() * 4);
    for (size_t texel = 0; texel < values.size(); ++texel) {
        const double error = (values[texel] - reference[texel]) * scale;
        const double context = kContext * reinhard(reference[texel] * scale);
        pixels[texel * 4] = encodeSrgb(std::max(reinhard(error), context));
        pixels[texel * 4 + 1] = encodeSrgb(context);
        pixels[texel * 4 + 2] = encodeSrgb(std::max(reinhard(-error), context));
        pixels[texel * 4 + 3] = 255;
    }
    writeRgba(name, extent, pixels, folder);
}

void writeCheckUnitPng(std::string_view name, vk::Extent2D extent, std::span<const double> values,
                       std::string_view folder)
{
    std::vector<uint8_t> pixels(values.size() * 4);
    for (size_t texel = 0; texel < values.size(); ++texel) {
        const auto gray = static_cast<uint8_t>(std::lround(std::clamp(values[texel], 0.0, 1.0) * 255.0));
        pixels[texel * 4] = gray;
        pixels[texel * 4 + 1] = gray;
        pixels[texel * 4 + 2] = gray;
        pixels[texel * 4 + 3] = 255;
    }
    writeRgba(name, extent, pixels, folder);
}
