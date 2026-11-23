#include "omm_baker.hpp"
#include "../static_headers/logger.hpp"
#include "../util/vk_tracy.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string>

// static: the decoder stays private to this file; only BC7 is used
#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include <bcdec.h>
#pragma clang diagnostic pop

// the SDK's outputs are Vulkan's build inputs: descriptors are copied as bytes, special indices pass through
static_assert(sizeof(ommCpuOpacityMicromapDesc) == sizeof(VkMicromapTriangleEXT));
static_assert(offsetof(ommCpuOpacityMicromapDesc, offset) == offsetof(VkMicromapTriangleEXT, dataOffset));
static_assert(offsetof(ommCpuOpacityMicromapDesc, subdivisionLevel) ==
              offsetof(VkMicromapTriangleEXT, subdivisionLevel));
static_assert(offsetof(ommCpuOpacityMicromapDesc, format) == offsetof(VkMicromapTriangleEXT, format));
static_assert(int{ommFormat_OC1_2_State} == int{VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT});
static_assert(int{ommFormat_OC1_4_State} == int{VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT});
static_assert(int{ommSpecialIndex_FullyTransparent} == int{VK_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_TRANSPARENT_EXT});
static_assert(int{ommSpecialIndex_FullyOpaque} == int{VK_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_OPAQUE_EXT});
static_assert(int{ommSpecialIndex_FullyUnknownTransparent} ==
              int{VK_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_UNKNOWN_TRANSPARENT_EXT});
static_assert(int{ommSpecialIndex_FullyUnknownOpaque} ==
              int{VK_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_UNKNOWN_OPAQUE_EXT});

namespace
{
    constexpr uint32_t kBcBlockExtent = 4;
    constexpr uint32_t kRgbaBytes = 4;
    constexpr uint32_t kAlphaChannel = 3;

    // EnableInternalThreads needs OpenMP (off in the build); 8-bit indices have no Vulkan index type
#ifdef NDEBUG
    constexpr ommCpuBakeFlags kBakeFlags = ommCpuBakeFlags_None;
#else
    constexpr ommCpuBakeFlags kBakeFlags = ommCpuBakeFlags_EnableValidation;
#endif

    const char* resultName(ommResult result)
    {
        switch (result) {
        case ommResult_SUCCESS:
            return "SUCCESS";
        case ommResult_FAILURE:
            return "FAILURE";
        case ommResult_INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";
        case ommResult_INSUFFICIENT_SCRATCH_MEMORY:
            return "INSUFFICIENT_SCRATCH_MEMORY";
        case ommResult_NOT_IMPLEMENTED:
            return "NOT_IMPLEMENTED";
        case ommResult_WORKLOAD_TOO_BIG:
            return "WORKLOAD_TOO_BIG";
        default:
            return "unknown";
        }
    }

    void check(ommResult result, std::string_view call, std::string_view name)
    {
        if (result != ommResult_SUCCESS) {
            throw std::runtime_error(std::format("{} failed for '{}': {}", call, name, resultName(result)));
        }
    }

    void logOmmMessage(ommMessageSeverity severity, const char* message, [[maybe_unused]] void* userArg)
    {
        switch (severity) {
        case ommMessageSeverity_Info:
            logDebug(message, "Omm");
            return;
        case ommMessageSeverity_PerfWarning:
            logInfo(std::format("perf warning: {}", message), "Omm");
            return;
        default:
            logError(message, "Omm");
            return;
        }
    }

    struct TextureDeleter
    {
        ommBaker baker = nullptr;
        void operator()(ommCpuTexture texture) const noexcept { (void)ommCpuDestroyTexture(baker, texture); }
    };
    using TextureHandle = std::unique_ptr<_ommCpuTexture, TextureDeleter>;

    struct BakeResultDeleter
    {
        void operator()(ommCpuBakeResult result) const noexcept { (void)ommCpuDestroyBakeResult(result); }
    };
    using BakeResultHandle = std::unique_ptr<_ommCpuBakeResult, BakeResultDeleter>;

    // SDK arrays may be null when empty
    template <typename T>
    std::vector<T> copyArray(const void* source, size_t count)
    {
        std::vector<T> result(count);
        if (count > 0) {
            std::memcpy(result.data(), source, count * sizeof(T));
        }
        return result;
    }

    uint32_t indexBytes(ommIndexFormat format, std::string_view name)
    {
        switch (format) {
        case ommIndexFormat_UINT_16:
            return sizeof(uint16_t);
        case ommIndexFormat_UINT_32:
            return sizeof(uint32_t);
        default:
            throw std::runtime_error(std::format("ommCpuBake returned 8-bit indices for '{}'", name));
        }
    }
} // namespace

OmmAlphaTexture extractAlpha(std::span<const uint8_t> rgba, uint32_t width, uint32_t height)
{
    const size_t texelCount = size_t{width} * height;
    if (rgba.size() < texelCount * kRgbaBytes) {
        throw std::runtime_error(std::format("RGBA8 pixels too small for {}x{}", width, height));
    }
    OmmAlphaTexture alpha{.texels = std::vector<uint8_t>(texelCount), .width = width, .height = height};
    for (size_t i = 0; i < texelCount; ++i) {
        alpha.texels[i] = rgba[i * kRgbaBytes + kAlphaChannel];
    }
    return alpha;
}

OmmAlphaTexture decodeBc7Alpha(std::span<const uint8_t> blocks, uint32_t width, uint32_t height)
{
    ZoneScopedN("decodeBc7Alpha");
    const uint32_t blocksX = (width + kBcBlockExtent - 1) / kBcBlockExtent;
    const uint32_t blocksY = (height + kBcBlockExtent - 1) / kBcBlockExtent;
    if (blocks.size() < size_t{blocksX} * blocksY * BCDEC_BC7_BLOCK_SIZE) {
        throw std::runtime_error(std::format("BC7 data too small for {}x{}", width, height));
    }

    OmmAlphaTexture alpha{.texels = std::vector<uint8_t>(size_t{width} * height), .width = width, .height = height};
    std::array<uint8_t, kBcBlockExtent * kBcBlockExtent * kRgbaBytes> block{};
    for (uint32_t by = 0; by < blocksY; ++by) {
        for (uint32_t bx = 0; bx < blocksX; ++bx) {
            bcdec_bc7(blocks.data() + (size_t{by} * blocksX + bx) * BCDEC_BC7_BLOCK_SIZE, block.data(),
                      kBcBlockExtent * kRgbaBytes);
            // edge blocks overhang a size that is not a multiple of 4
            const uint32_t rows = std::min(kBcBlockExtent, height - by * kBcBlockExtent);
            const uint32_t columns = std::min(kBcBlockExtent, width - bx * kBcBlockExtent);
            for (uint32_t y = 0; y < rows; ++y) {
                const size_t row = size_t{by * kBcBlockExtent + y} * width + bx * kBcBlockExtent;
                for (uint32_t x = 0; x < columns; ++x) {
                    alpha.texels[row + x] = block[(y * kBcBlockExtent + x) * kRgbaBytes + kAlphaChannel];
                }
            }
        }
    }
    return alpha;
}

OmmAlphaTexture extractMaxRgb(std::span<const uint8_t> rgba, uint32_t width, uint32_t height)
{
    const size_t texelCount = size_t{width} * height;
    if (rgba.size() < texelCount * kRgbaBytes) {
        throw std::runtime_error(std::format("RGBA8 pixels too small for {}x{}", width, height));
    }
    OmmAlphaTexture mask{.texels = std::vector<uint8_t>(texelCount), .width = width, .height = height};
    for (size_t i = 0; i < texelCount; ++i) {
        const size_t base = i * kRgbaBytes;
        mask.texels[i] = std::max({rgba[base], rgba[base + 1], rgba[base + 2]});
    }
    return mask;
}

namespace
{
    template <size_t BlockBytes, typename DecodeFn>
    OmmAlphaTexture decodeBcMaxRgb(std::span<const uint8_t> blocks, uint32_t width, uint32_t height,
                                   const char* formatName, DecodeFn&& decodeBlock)
    {
        const uint32_t blocksX = (width + kBcBlockExtent - 1) / kBcBlockExtent;
        const uint32_t blocksY = (height + kBcBlockExtent - 1) / kBcBlockExtent;
        if (blocks.size() < size_t{blocksX} * blocksY * BlockBytes) {
            throw std::runtime_error(std::format("{} data too small for {}x{}", formatName, width, height));
        }
        OmmAlphaTexture mask{.texels = std::vector<uint8_t>(size_t{width} * height), .width = width, .height = height};
        std::array<uint8_t, kBcBlockExtent * kBcBlockExtent * kRgbaBytes> block{};
        for (uint32_t by = 0; by < blocksY; ++by) {
            for (uint32_t bx = 0; bx < blocksX; ++bx) {
                decodeBlock(blocks.data() + (size_t{by} * blocksX + bx) * BlockBytes, block.data(),
                            kBcBlockExtent * kRgbaBytes);
                const uint32_t rows = std::min(kBcBlockExtent, height - by * kBcBlockExtent);
                const uint32_t columns = std::min(kBcBlockExtent, width - bx * kBcBlockExtent);
                for (uint32_t y = 0; y < rows; ++y) {
                    const size_t row = size_t{by * kBcBlockExtent + y} * width + bx * kBcBlockExtent;
                    for (uint32_t x = 0; x < columns; ++x) {
                        const size_t base = (y * kBcBlockExtent + x) * kRgbaBytes;
                        mask.texels[row + x] = std::max({block[base], block[base + 1], block[base + 2]});
                    }
                }
            }
        }
        return mask;
    }
} // namespace

OmmAlphaTexture decodeBc7MaxRgb(std::span<const uint8_t> blocks, uint32_t width, uint32_t height)
{
    ZoneScopedN("decodeBc7MaxRgb");
    return decodeBcMaxRgb<BCDEC_BC7_BLOCK_SIZE>(blocks, width, height, "BC7", bcdec_bc7);
}

OmmAlphaTexture decodeBc1MaxRgb(std::span<const uint8_t> blocks, uint32_t width, uint32_t height)
{
    ZoneScopedN("decodeBc1MaxRgb");
    return decodeBcMaxRgb<BCDEC_BC1_BLOCK_SIZE>(blocks, width, height, "BC1", bcdec_bc1);
}

OmmAlphaTexture decodeBc3MaxRgb(std::span<const uint8_t> blocks, uint32_t width, uint32_t height)
{
    ZoneScopedN("decodeBc3MaxRgb");
    return decodeBcMaxRgb<BCDEC_BC3_BLOCK_SIZE>(blocks, width, height, "BC3", bcdec_bc3);
}

void OmmBaker::BakerDeleter::operator()(ommBaker handle) const noexcept
{
    (void)ommDestroyBaker(handle);
}

OmmBaker::OmmBaker(std::optional<std::filesystem::path> debugImageDirectoryIn) :
    debugImageDirectory(std::move(debugImageDirectoryIn))
{
    ommBakerCreationDesc desc = ommBakerCreationDescDefault();
    desc.type = ommBakerType_CPU;
    // EnableValidation reports through it
    desc.messageInterface.messageCallback = &logOmmMessage;
    ommBaker rawBaker = nullptr;
    check(ommCreateBaker(&desc, &rawBaker), "ommCreateBaker", "load");
    baker.reset(rawBaker);
}

OmmBake OmmBaker::bake(const OmmBakeInput& input) const
{
    ZoneScopedN("OmmBaker::bake");
    if (input.alpha.size() < size_t{input.width} * input.height || input.indices.empty() ||
        input.indices.size() % 3 != 0) {
        throw std::runtime_error(std::format("invalid OMM bake input for '{}'", input.name));
    }

    ommCpuTextureMipDesc mip = ommCpuTextureMipDescDefault();
    mip.width = input.width;
    mip.height = input.height;
    mip.textureData = input.alpha.data();
    ommCpuTextureDesc textureDesc = ommCpuTextureDescDefault();
    textureDesc.format = ommCpuTextureFormat_UNORM8;
    textureDesc.mips = &mip;
    textureDesc.mipCount = 1;
    // must equal the bake's cutoff; lets the SDK precompute its coverage table
    textureDesc.alphaCutoff = input.alphaCutoff;
    ommCpuTexture rawTexture = nullptr;
    check(ommCpuCreateTexture(baker.get(), &textureDesc, &rawTexture), "ommCpuCreateTexture", input.name);
    const TextureHandle texture(rawTexture, TextureDeleter{baker.get()});

    ommCpuBakeInputDesc bakeDesc = ommCpuBakeInputDescDefault();
    bakeDesc.bakeFlags = kBakeFlags;
    bakeDesc.texture = texture.get();
    bakeDesc.runtimeSamplerDesc.addressingMode = input.addressMode;
    bakeDesc.runtimeSamplerDesc.filter = input.filter;
    bakeDesc.alphaMode = ommAlphaMode_Test;
    bakeDesc.texCoordFormat = ommTexCoordFormat_UV32_FLOAT;
    bakeDesc.texCoords = input.uvs.data();
    bakeDesc.texCoordStrideInBytes = sizeof(glm::vec2);
    bakeDesc.indexFormat = ommIndexFormat_UINT_32;
    bakeDesc.indexBuffer = input.indices.data();
    bakeDesc.indexCount = static_cast<uint32_t>(input.indices.size());
    bakeDesc.dynamicSubdivisionScale = input.dynamicSubdivisionScale;
    bakeDesc.alphaCutoff = input.alphaCutoff;
    bakeDesc.format = ommFormat_OC1_4_State;
    bakeDesc.unknownStatePromotion = ommUnknownStatePromotion_ForceOpaque;
    bakeDesc.maxSubdivisionLevel = input.maxSubdivisionLevel;

    ommCpuBakeResult rawResult = nullptr;
    check(ommCpuBake(baker.get(), &bakeDesc, &rawResult), "ommCpuBake", input.name);
    const BakeResultHandle result(rawResult);
    const ommCpuBakeResultDesc* resultDesc = nullptr;
    check(ommCpuGetBakeResultDesc(result.get(), &resultDesc), "ommCpuGetBakeResultDesc", input.name);

    const uint32_t triangleCount = bakeDesc.indexCount / 3;
    if (resultDesc->indexCount != triangleCount) {
        throw std::runtime_error(std::format("ommCpuBake returned {} indices for {} triangles of '{}'",
                                             resultDesc->indexCount, triangleCount, input.name));
    }

    OmmBake bake{
        .arrayData = copyArray<uint8_t>(resultDesc->arrayData, resultDesc->arrayDataSize),
        .triangles = copyArray<VkMicromapTriangleEXT>(resultDesc->descArray, resultDesc->descArrayCount),
        .usages = {},
        .indices = copyArray<uint8_t>(resultDesc->indexBuffer,
                                      size_t{resultDesc->indexCount} * indexBytes(resultDesc->indexFormat, input.name)),
        .indexType = resultDesc->indexFormat == ommIndexFormat_UINT_16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32,
        .triangleCount = triangleCount,
        .stats = {},
    };
    // 8-byte SDK rows, 12-byte Vulkan rows
    bake.usages.reserve(resultDesc->descArrayHistogramCount);
    for (uint32_t i = 0; i < resultDesc->descArrayHistogramCount; ++i) {
        const ommCpuOpacityMicromapUsageCount& usage = resultDesc->descArrayHistogram[i];
        bake.usages.push_back({.count = usage.count, .subdivisionLevel = usage.subdivisionLevel, .format = usage.format});
    }

    // the result handle carries the uv areas knownAreaMetric weights by; the desc overload reports 0
    ommDebugStats stats = ommDebugStatsDefault();
    check(ommDebugGetStats2(baker.get(), result.get(), &stats), "ommDebugGetStats2", input.name);
    bake.stats = {
        .opaque = stats.totalOpaque,
        .transparent = stats.totalTransparent,
        .unknown = stats.totalUnknownOpaque + stats.totalUnknownTransparent,
        .knownAreaMetric = stats.knownAreaMetric,
    };

    if (debugImageDirectory) {
        const std::string path = debugImageDirectory->string();
        const std::string postfix(input.name);
        ommDebugSaveImagesDesc imagesDesc = ommDebugSaveImagesDescDefault();
        imagesDesc.path = path.c_str();
        imagesDesc.filePostfix = postfix.c_str();
        imagesDesc.oneFile = 1;
        check(ommDebugSaveAsImages(baker.get(), &bakeDesc, resultDesc, &imagesDesc), "ommDebugSaveAsImages",
              input.name);
    }
    return bake;
}
