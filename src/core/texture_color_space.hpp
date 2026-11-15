#pragma once

// dependency-free: shared by the engine loader and tools/gltf_ktx2

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

enum class TextureColorSpace : uint8_t
{
    Srgb = 0,
    Linear = 1,
};

// glTF texture-info key -> sampling color space; every slot not listed holds linear data
[[nodiscard]] constexpr TextureColorSpace gltfTextureSlotColorSpace(std::string_view slot) noexcept
{
    constexpr std::array<std::string_view, 5> kSrgbSlots{
        "baseColorTexture",
        "emissiveTexture",
        "specularColorTexture",
        "sheenColorTexture",
        "diffuseTransmissionColorTexture",
    };
    return std::ranges::find(kSrgbSlots, slot) != kSrgbSlots.end() ? TextureColorSpace::Srgb
                                                                   : TextureColorSpace::Linear;
}
