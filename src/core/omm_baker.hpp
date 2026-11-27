#pragma once

#include <omm.h>
#include <vulkan/vulkan_core.h>
#include <glm/vec2.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// ommDebugGetStats2 of one bake; micro-triangles behind special indices are not counted
struct OmmBakeStats
{
    uint64_t opaque = 0;
    uint64_t transparent = 0;
    // unknown opaque + unknown transparent
    uint64_t unknown = 0;
    // known area in uv space / total uv space; -1 when the SDK cannot tell
    float knownAreaMetric = 0.0f;
};

// bake settings read at load; the UI re-bakes loaded models with new ones (RTXPT OpacityMicroMapUIData::BuildState)
struct OmmBakeSettings
{
    // off: no micromaps, alpha primitives are opaque to rays. a bake setting because PTLAS instance flags have no
    // DISABLE_OPACITY_MICROMAPS equivalent
    bool isEnabled = true;
    // clamped to the device's maxOpacity{2,4}StateSubdivisionLevel for the format
    uint32_t maxSubdivisionLevel = 5;
    bool isDynamicSubdivisionEnabled = true;
    // micro-triangle edge in texels when dynamic subdivision is on (SDK default)
    float dynamicSubdivisionScale = 2.0f;
    // rays force 2-state, so 4-state's unknown states resolve by promotion either way (spec/m6/README.md)
    ommFormat format = ommFormat_OC1_4_State;
    ommUnknownStatePromotion unknownStatePromotion = ommUnknownStatePromotion_ForceOpaque;
    bool isSpecialIndicesEnabled = true;
    // merges similar micromaps at the cost of known area (CPU baker only)
    bool isNearDuplicateDetectionEnabled = false;
    bool isFastBuildPreferred = false;

    bool operator==(const OmmBakeSettings&) const = default;
};

// OMM SDK 1.9.2 hard ceiling (omm.h: maxSubdivisionLevel must be in [0, 12])
inline constexpr uint32_t kOmmMaxSubdivisionLevel = 12;

// device subdivision cap for the format, bounded by the SDK ceiling
[[nodiscard]] inline uint32_t maxOmmSubdivisionLevel(const VkPhysicalDeviceOpacityMicromapPropertiesEXT& limits,
                                                     ommFormat format)
{
    const uint32_t deviceLimit = format == ommFormat_OC1_2_State ? limits.maxOpacity2StateSubdivisionLevel
                                                                 : limits.maxOpacity4StateSubdivisionLevel;
    return std::min(deviceLimit, kOmmMaxSubdivisionLevel);
}

// One primitive's OMM SDK output in Vulkan build-input layout (spec/m6/d1_omm_bake.md); D2 uploads it unchanged.
// triangleCount == 0: not baked, the primitive is opaque to rays
struct OmmBake
{
    std::vector<uint8_t> arrayData;
    // micromap descriptors (micromap triangleArray)
    std::vector<VkMicromapTriangleEXT> triangles;
    // descriptor histogram (micromap pUsageCounts)
    std::vector<VkMicromapUsageEXT> usages;
    // one entry per input triangle in final cluster order; special indices are negative
    std::vector<uint8_t> indices;
    VkIndexType indexType = VK_INDEX_TYPE_UINT32;
    uint32_t triangleCount = 0;
    // vkCmdBuildMicromapsEXT preference
    VkBuildMicromapFlagsEXT buildFlags = VK_BUILD_MICROMAP_PREFER_FAST_TRACE_BIT_EXT;
    OmmBakeStats stats;
};

// one load's bakes, for the load summary and the UI
struct OmmBakeSummary
{
    uint32_t primitiveCount = 0;
    uint64_t opaque = 0;
    uint64_t transparent = 0;
    uint64_t unknown = 0;
    float minKnownAreaMetric = 0.0f;
    uint64_t arrayBytes = 0;
    uint64_t indexBytes = 0;
    double bakeMilliseconds = 0.0;
};

// UNORM8 alpha, tight rows
struct OmmAlphaTexture
{
    std::vector<uint8_t> texels;
    uint32_t width = 0;
    uint32_t height = 0;
};

// alpha channel of RGBA8 pixels
[[nodiscard]] OmmAlphaTexture extractAlpha(std::span<const uint8_t> rgba, uint32_t width, uint32_t height);
// alpha of BC7 / BC3 blocks (row-major, 16 B each); throws when blocks is too small
[[nodiscard]] OmmAlphaTexture decodeBc7Alpha(std::span<const uint8_t> blocks, uint32_t width, uint32_t height);
[[nodiscard]] OmmAlphaTexture decodeBc3Alpha(std::span<const uint8_t> blocks, uint32_t width, uint32_t height);
// max(R, G, B) of RGBA8 / BC7 / BC1 / BC3 pixels (m8 I14 F27: zero-flux emissive triangle culling)
[[nodiscard]] OmmAlphaTexture extractMaxRgb(std::span<const uint8_t> rgba, uint32_t width, uint32_t height);
[[nodiscard]] OmmAlphaTexture decodeBc7MaxRgb(std::span<const uint8_t> blocks, uint32_t width, uint32_t height);
[[nodiscard]] OmmAlphaTexture decodeBc1MaxRgb(std::span<const uint8_t> blocks, uint32_t width, uint32_t height);
[[nodiscard]] OmmAlphaTexture decodeBc3MaxRgb(std::span<const uint8_t> blocks, uint32_t width, uint32_t height);

struct OmmBakeInput
{
    // primitive label for errors and debug image file names
    std::string_view name;
    // mip 0 of what the shader samples
    std::span<const uint8_t> alpha;
    uint32_t width = 0;
    uint32_t height = 0;
    ommTextureAddressMode addressMode = ommTextureAddressMode_Wrap;
    ommTextureFilterMode filter = ommTextureFilterMode_Linear;
    // texture alpha threshold: material cutoff / base color factor alpha
    float alphaCutoff = 0.5f;
    // uv matrix already applied
    std::span<const glm::vec2> uvs;
    // into uvs, final cluster triangle order
    std::span<const uint32_t> indices;
    // maxSubdivisionLevel already clamped to the device
    OmmBakeSettings settings;
};

// CPU OMM SDK baker for one load; no SDK handle leaves it
class OmmBaker
{
public:
    // debugImageDirectory: each bake also writes ommDebugSaveAsImages output there
    explicit OmmBaker(std::optional<std::filesystem::path> debugImageDirectory = std::nullopt);

    // thread-safe; throws std::runtime_error naming the SDK call and input.name
    [[nodiscard]] OmmBake bake(const OmmBakeInput& input) const;

private:
    struct BakerDeleter
    {
        void operator()(ommBaker baker) const noexcept;
    };

    std::unique_ptr<_ommBaker, BakerDeleter> baker;
    std::optional<std::filesystem::path> debugImageDirectory;
};
