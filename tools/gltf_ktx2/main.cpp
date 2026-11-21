// gltf_ktx2 — offline texture converter for glTF models.
//
// Converts every PNG/JPEG image a .gltf's materials reference (file URI, data URI or bufferView) to a
// KTX2 file holding BC7 with a full mip chain (zstd-supercompressed), written next to its source image
// (embedded images: next to the .gltf), and rewrites the .gltf in place to point at them. The first change
// keeps <name>.gltf.bak; each converted image records its original URI in extras.gltf_ktx2, so re-runs
// find the PNG/JPEG again (up-to-date check, --force) and keep the same file names.
//
// Pipeline per image: stb decode -> CPU mip chain (sRGB-correct box filter, normal maps renormalized)
// -> UASTC encode (libktx/Basis) -> transcode to BC7 -> zstd -> write. BC7 needs no transcoding at load:
// the engine's KTX path uploads it as-is.
//
// DDS sources (DX10 header, BCn) are already compressed with their own mips: their blocks are copied into
// the KTX2 unchanged (lossless), and only the sRGB/UNORM variant follows the material usage. A texture's
// MSFT_texture_dds image becomes its source and the extension is dropped; images left unreferenced by that
// are removed.
//
// Color space comes from how materials sample the image (rules shared with the engine loader). An image
// sampled as both sRGB and linear gets two files, and the linear uses are pointed at a cloned image.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

#include "json.hpp"

#include "core/texture_color_space.hpp"

#include <ktx.h>
#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;

namespace
{
    constexpr uint32_t kDefaultZstdLevel = 18;
    constexpr uint32_t kMaxZstdLevel = 22;
    constexpr double kMiB = 1048576.0;

    struct Options
    {
        fs::path input;
        bool force = false;
        uint32_t jobs = 0;
        uint32_t uastcLevel = KTX_PACK_UASTC_LEVEL_DEFAULT;
        uint32_t zstdLevel = kDefaultZstdLevel;
    };

    // ── URI helpers ──────────────────────────────────────────────

    std::string percentDecode(std::string_view in)
    {
        std::string out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size(); ++i) {
            if (in[i] == '%' && i + 2 < in.size()) {
                const std::string hex(in.substr(i + 1, 2));
                char* end = nullptr;
                const long value = std::strtol(hex.c_str(), &end, 16);
                if (end == hex.c_str() + 2) {
                    out += static_cast<char>(value);
                    i += 2;
                    continue;
                }
            }
            out += in[i];
        }
        return out;
    }

    // output names need no URI escaping (the engine joins image URIs onto paths without decoding)
    std::string safeFileStem(std::string_view in)
    {
        std::string out;
        for (const unsigned char c : in) {
            out += std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' ? static_cast<char>(c) : '_';
        }
        return out.empty() ? std::string("image") : out;
    }

    bool isDataUri(std::string_view uri) { return uri.starts_with("data:"); }

    // scheme:... (http:, file:, ...) but not a Windows drive letter
    bool hasScheme(std::string_view uri)
    {
        const auto colon = uri.find(':');
        return colon != std::string_view::npos && colon > 1 && uri.find('/') > colon;
    }

    std::vector<uint8_t> decodeBase64(std::string_view in)
    {
        const auto value = [](char c) -> int
        {
            if (c >= 'A' && c <= 'Z')
                return c - 'A';
            if (c >= 'a' && c <= 'z')
                return c - 'a' + 26;
            if (c >= '0' && c <= '9')
                return c - '0' + 52;
            if (c == '+')
                return 62;
            if (c == '/')
                return 63;
            return -1;
        };
        std::vector<uint8_t> out;
        out.reserve(in.size() * 3 / 4);
        int acc = 0;
        int bits = -8;
        for (const char c : in) {
            const int d = value(c);
            if (d < 0) {
                continue;
            }
            acc = (acc << 6) + d;
            bits += 6;
            if (bits >= 0) {
                out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
                bits -= 8;
            }
        }
        return out;
    }

    // ── material usage ───────────────────────────────────────────

    bool isNormalSlot(std::string_view key) { return key == "normalTexture" || key == "clearcoatNormalTexture"; }

    struct TextureUse
    {
        Json* info = nullptr; // the texture-info object; its "index" is rewritten for split images
        bool srgb = false;
        bool normal = false;
    };

    // core slots and every extension's "*Texture" objects
    void collectTextureUses(Json& node, std::vector<TextureUse>& uses)
    {
        if (node.is_array()) {
            for (Json& child : node) {
                collectTextureUses(child, uses);
            }
            return;
        }
        if (!node.is_object()) {
            return;
        }
        for (auto it = node.begin(); it != node.end(); ++it) {
            const std::string& key = it.key();
            Json& value = it.value();
            if (key.ends_with("Texture") && value.is_object() && value.contains("index") &&
                value["index"].is_number_integer()) {
                uses.push_back({.info = &value,
                                .srgb = gltfTextureSlotColorSpace(key) == TextureColorSpace::Srgb,
                                .normal = isNormalSlot(key)});
            }
            collectTextureUses(value, uses);
        }
    }

    // ── image source ─────────────────────────────────────────────

    using Space = TextureColorSpace;

    struct ImageInfo
    {
        bool usedSrgb = false;
        bool usedLinear = false;
        bool usedAsNormal = false;
        bool isDds = false; // repacked, not decoded
        std::string label; // for logs
        std::string stem; // output base name
        std::optional<fs::path> file; // URI image
        std::vector<uint8_t> bytes; // data URI / bufferView image
        fs::file_time_type sourceTime{};
        std::string sourceUri; // recorded in extras ("embedded" for data URI / bufferView)
    };

    // images[i].extras key holding {source, colorSpace} of a converted image
    constexpr const char* kExtrasKey = "gltf_ktx2";

    struct Job
    {
        uint32_t image = 0;
        Space space = Space::Srgb;
        fs::path output;
        std::string uri; // as written into the .gltf (relative to it)
    };

    bool isPngOrJpeg(std::string_view mimeOrExt)
    {
        std::string lower(mimeOrExt);
        std::ranges::transform(lower, lower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower == "image/png" || lower == "image/jpeg" || lower == ".png" || lower == ".jpg" || lower == ".jpeg";
    }

    bool isDdsExtension(std::string_view ext)
    {
        return ext.size() == 4 && ext[0] == '.' && std::tolower(static_cast<unsigned char>(ext[1])) == 'd' &&
            std::tolower(static_cast<unsigned char>(ext[2])) == 'd' &&
            std::tolower(static_cast<unsigned char>(ext[3])) == 's';
    }

    std::vector<uint8_t> readFile(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            throw std::runtime_error("cannot open " + path.string());
        }
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    // ── DDS source ───────────────────────────────────────────────

    struct BcFormat
    {
        uint32_t dxgiUnorm = 0;
        uint32_t dxgiSrgb = 0; // 0: no sRGB variant
        VkFormat unorm = VK_FORMAT_UNDEFINED;
        VkFormat srgb = VK_FORMAT_UNDEFINED;
        uint32_t blockBytes = 0;
    };

    // DXGI_FORMAT values of the DX10 header; sRGB and UNORM variants share the block encoding
    constexpr std::array<BcFormat, 5> kBcFormats{{
        {71, 72, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 8},
        {77, 78, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK, 16},
        {80, 0, VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_UNDEFINED, 8},
        {83, 0, VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_UNDEFINED, 16},
        {98, 99, VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK, 16},
    }};

    // magic + DDS_HEADER + DDS_HEADER_DXT10
    constexpr size_t kDdsHeaderBytes = 4 + 124 + 20;

    struct DdsImage
    {
        const BcFormat* format = nullptr;
        bool labelledSrgb = false;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t levels = 0;
    };

    uint32_t readU32(std::span<const uint8_t> bytes, size_t offset)
    {
        uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

    uint64_t bcLevelBytes(const BcFormat& format, uint32_t width, uint32_t height, uint32_t level)
    {
        const uint64_t blocksX = (std::max(1u, width >> level) + 3) / 4;
        const uint64_t blocksY = (std::max(1u, height >> level) + 3) / 4;
        return blocksX * blocksY * format.blockBytes;
    }

    // a single 2D BCn image with a DX10 header whose mips fit in fileBytes; nullopt otherwise
    std::optional<DdsImage> parseDds(std::span<const uint8_t> header, uint64_t fileBytes)
    {
        constexpr uint32_t kDdsMagic = 0x20534444; // "DDS "
        constexpr uint32_t kFourCcDx10 = 0x30315844; // "DX10"
        constexpr uint32_t kPixelFormatFourCc = 0x4;
        constexpr uint32_t kFlagMipMapCount = 0x20000;
        constexpr uint32_t kDimensionTexture2D = 3;
        constexpr uint32_t kMiscTextureCube = 0x4;
        if (header.size() < kDdsHeaderBytes || readU32(header, 0) != kDdsMagic || readU32(header, 4) != 124 ||
            (readU32(header, 80) & kPixelFormatFourCc) == 0 || readU32(header, 84) != kFourCcDx10 ||
            readU32(header, 132) != kDimensionTexture2D || (readU32(header, 136) & kMiscTextureCube) != 0 ||
            readU32(header, 140) != 1) {
            return std::nullopt;
        }
        const uint32_t dxgi = readU32(header, 128);
        const auto format =
            std::ranges::find_if(kBcFormats, [&](const BcFormat& f) { return f.dxgiUnorm == dxgi || f.dxgiSrgb == dxgi; });
        DdsImage image{.format = format == kBcFormats.end() ? nullptr : &*format,
                       .labelledSrgb = format != kBcFormats.end() && format->dxgiSrgb == dxgi,
                       .width = readU32(header, 16),
                       .height = readU32(header, 12),
                       .levels = (readU32(header, 8) & kFlagMipMapCount) != 0 ? std::max(1u, readU32(header, 28)) : 1u};
        if (image.format == nullptr || image.width == 0 || image.height == 0 ||
            image.levels > static_cast<uint32_t>(std::bit_width(std::max(image.width, image.height)))) {
            return std::nullopt;
        }
        uint64_t dataBytes = 0;
        for (uint32_t level = 0; level < image.levels; ++level) {
            dataBytes += bcLevelBytes(*image.format, image.width, image.height, level);
        }
        if (kDdsHeaderBytes + dataBytes > fileBytes) {
            return std::nullopt;
        }
        return image;
    }

    std::optional<DdsImage> readDdsHeader(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        std::array<uint8_t, kDdsHeaderBytes> header{};
        if (!file.read(reinterpret_cast<char*>(header.data()), header.size())) {
            return std::nullopt;
        }
        std::error_code ec;
        const uint64_t fileBytes = fs::file_size(path, ec);
        return ec ? std::nullopt : parseDds(header, fileBytes);
    }

    // ── mip chain ────────────────────────────────────────────────

    struct Level
    {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> rgba;
    };

    float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

    uint8_t linearToSrgb8(float c)
    {
        c = std::clamp(c, 0.0f, 1.0f);
        const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        return static_cast<uint8_t>(std::lround(s * 255.0f));
    }

    uint8_t toUnorm8(float c) { return static_cast<uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f)); }

    // 2x2 box filter; sRGB color is averaged in linear light, normals are renormalized
    Level downsample(const Level& src, Space space, bool normalMap)
    {
        static const std::array<float, 256> kSrgbLut = []
        {
            std::array<float, 256> lut{};
            for (int i = 0; i < 256; ++i) {
                lut[i] = srgbToLinear(static_cast<float>(i) / 255.0f);
            }
            return lut;
        }();

        Level dst{.width = std::max(1u, src.width / 2), .height = std::max(1u, src.height / 2), .rgba = {}};
        dst.rgba.resize(static_cast<size_t>(dst.width) * dst.height * 4);
        for (uint32_t y = 0; y < dst.height; ++y) {
            for (uint32_t x = 0; x < dst.width; ++x) {
                const uint32_t x0 = std::min(2 * x, src.width - 1);
                const uint32_t x1 = std::min(2 * x + 1, src.width - 1);
                const uint32_t y0 = std::min(2 * y, src.height - 1);
                const uint32_t y1 = std::min(2 * y + 1, src.height - 1);
                const std::array<const uint8_t*, 4> taps = {
                    &src.rgba[(static_cast<size_t>(y0) * src.width + x0) * 4],
                    &src.rgba[(static_cast<size_t>(y0) * src.width + x1) * 4],
                    &src.rgba[(static_cast<size_t>(y1) * src.width + x0) * 4],
                    &src.rgba[(static_cast<size_t>(y1) * src.width + x1) * 4],
                };
                std::array<float, 4> sum{};
                for (const uint8_t* tap : taps) {
                    for (int c = 0; c < 4; ++c) {
                        const bool srgbChannel = space == Space::Srgb && c < 3;
                        const float unorm = static_cast<float>(tap[c]) / 255.0f;
                        sum[c] += srgbChannel ? kSrgbLut[tap[c]] : (normalMap && c < 3 ? unorm * 2.0f - 1.0f : unorm);
                    }
                }
                uint8_t* out = &dst.rgba[(static_cast<size_t>(y) * dst.width + x) * 4];
                if (normalMap) {
                    const float len = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
                    const float inv = len > 0.0f ? 1.0f / len : 0.0f;
                    for (int c = 0; c < 3; ++c) {
                        out[c] = toUnorm8(sum[c] * inv * 0.5f + 0.5f);
                    }
                } else {
                    for (int c = 0; c < 3; ++c) {
                        out[c] = space == Space::Srgb ? linearToSrgb8(sum[c] * 0.25f) : toUnorm8(sum[c] * 0.25f);
                    }
                }
                out[3] = toUnorm8(sum[3] * 0.25f);
            }
        }
        return dst;
    }

    // ── KTX2 encode ──────────────────────────────────────────────

    struct KtxDeleter
    {
        void operator()(ktxTexture2* texture) const { ktxTexture_Destroy(ktxTexture(texture)); }
    };
    using KtxPtr = std::unique_ptr<ktxTexture2, KtxDeleter>;

    void check(KTX_error_code code, std::string_view what)
    {
        if (code != KTX_SUCCESS) {
            throw std::runtime_error(std::format("{} failed: {}", what, ktxErrorString(code)));
        }
    }

    KtxPtr createKtx(VkFormat format, uint32_t width, uint32_t height, uint32_t levels)
    {
        ktxTextureCreateInfo createInfo{};
        createInfo.vkFormat = format;
        createInfo.baseWidth = width;
        createInfo.baseHeight = height;
        createInfo.baseDepth = 1;
        createInfo.numDimensions = 2;
        createInfo.numLevels = levels;
        createInfo.numLayers = 1;
        createInfo.numFaces = 1;
        createInfo.isArray = KTX_FALSE;
        createInfo.generateMipmaps = KTX_FALSE;

        ktxTexture2* raw = nullptr;
        check(ktxTexture2_Create(&createInfo, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &raw), "ktxTexture2_Create");
        return KtxPtr(raw);
    }

    // mip chain -> UASTC -> BC7 (sRGB or UNORM from the DFD) -> zstd
    KtxPtr encodeBc7(const std::vector<Level>& levels, Space space, const Options& options)
    {
        KtxPtr texture = createKtx(space == Space::Srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
                                   levels[0].width, levels[0].height, static_cast<uint32_t>(levels.size()));
        for (uint32_t level = 0; level < levels.size(); ++level) {
            check(ktxTexture_SetImageFromMemory(ktxTexture(texture.get()), level, 0, 0, levels[level].rgba.data(),
                                                levels[level].rgba.size()),
                  "ktxTexture_SetImageFromMemory");
        }

        ktxBasisParams params{};
        params.structSize = sizeof(params);
        params.uastc = KTX_TRUE;
        params.threadCount = 1; // parallelism is across images
        params.uastcFlags = options.uastcLevel;
        check(ktxTexture2_CompressBasisEx(texture.get(), &params), "ktxTexture2_CompressBasisEx");
        check(ktxTexture2_TranscodeBasis(texture.get(), KTX_TTF_BC7_RGBA, KTX_TF_HIGH_QUALITY),
              "ktxTexture2_TranscodeBasis");
        if (options.zstdLevel > 0) {
            check(ktxTexture2_DeflateZstd(texture.get(), options.zstdLevel), "ktxTexture2_DeflateZstd");
        }

        const VkFormat expected = space == Space::Srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
        if (texture->vkFormat != static_cast<uint32_t>(expected)) {
            throw std::runtime_error(std::format("unexpected output vkFormat {}", texture->vkFormat));
        }
        return texture;
    }

    // Basis encoder/transcoder init is guarded by a plain static bool in libktx: run once before workers
    void warmUpBasis(const Options& options)
    {
        Level tiny{.width = 4, .height = 4, .rgba = std::vector<uint8_t>(4 * 4 * 4, 128)};
        (void)encodeBc7({tiny}, Space::Linear, options);
    }

    struct Result
    {
        bool skipped = false;
        bool failed = false;
        std::string message;
        uint64_t sourceBytes = 0; // RGBA8 incl. mips, what the engine uploads today
        uint64_t gpuBytes = 0; // BC7 incl. mips
        uint64_t fileBytes = 0;
    };

    // BCn blocks copied unchanged; the sRGB/UNORM variant follows the material usage, not the DDS label
    KtxPtr repackDds(const fs::path& path, Space space, const Options& options, Result& result)
    {
        const std::vector<uint8_t> bytes = readFile(path);
        const std::optional<DdsImage> dds = parseDds(bytes, bytes.size());
        if (!dds) {
            throw std::runtime_error("unsupported DDS (expected one 2D BC1/3/4/5/7 image with a DX10 header)");
        }
        const VkFormat format = space == Space::Srgb ? dds->format->srgb : dds->format->unorm;
        if (format == VK_FORMAT_UNDEFINED) {
            throw std::runtime_error("sampled as sRGB, but its BC format has no sRGB variant");
        }

        KtxPtr texture = createKtx(format, dds->width, dds->height, dds->levels);
        size_t offset = kDdsHeaderBytes;
        for (uint32_t level = 0; level < dds->levels; ++level) {
            const uint64_t levelBytes = bcLevelBytes(*dds->format, dds->width, dds->height, level);
            check(ktxTexture_SetImageFromMemory(ktxTexture(texture.get()), level, 0, 0, bytes.data() + offset,
                                                levelBytes),
                  "ktxTexture_SetImageFromMemory");
            offset += levelBytes;
            result.sourceBytes +=
                uint64_t{std::max(1u, dds->width >> level)} * std::max(1u, dds->height >> level) * 4u;
            result.gpuBytes += levelBytes;
        }
        if (options.zstdLevel > 0) {
            check(ktxTexture2_DeflateZstd(texture.get(), options.zstdLevel), "ktxTexture2_DeflateZstd");
        }
        const bool relabelled = dds->labelledSrgb != (space == Space::Srgb);
        result.message = std::format("{}x{} {} mips, DDS repacked{}", dds->width, dds->height, dds->levels,
                                     relabelled ? (dds->labelledSrgb ? " (labelled sRGB)" : " (labelled UNORM)") : "");
        return texture;
    }

    // decode -> CPU mip chain -> BC7
    KtxPtr encodeDecoded(const ImageInfo& image, const Job& job, const Options& options, Result& result)
    {
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = image.file
            ? stbi_load(image.file->string().c_str(), &width, &height, &channels, STBI_rgb_alpha)
            : stbi_load_from_memory(image.bytes.data(), static_cast<int>(image.bytes.size()), &width, &height,
                                    &channels, STBI_rgb_alpha);
        if (pixels == nullptr) {
            throw std::runtime_error(std::format("decode failed: {}", stbi_failure_reason()));
        }
        std::vector<Level> levels;
        levels.push_back({.width = static_cast<uint32_t>(width),
                          .height = static_cast<uint32_t>(height),
                          .rgba = std::vector<uint8_t>(pixels, pixels + static_cast<size_t>(width) * height * 4)});
        stbi_image_free(pixels);

        const bool normalMap = image.usedAsNormal && job.space == Space::Linear;
        while (levels.back().width > 1 || levels.back().height > 1) {
            levels.push_back(downsample(levels.back(), job.space, normalMap));
        }
        for (const Level& level : levels) {
            result.sourceBytes += level.rgba.size();
            // BC7: 16 B per 4x4 block
            result.gpuBytes += static_cast<uint64_t>((level.width + 3) / 4) * ((level.height + 3) / 4) * 16u;
        }
        result.message = std::format("{}x{} {} mips", width, height, levels.size());
        return encodeBc7(levels, job.space, options);
    }

    Result convert(const ImageInfo& image, const Job& job, const Options& options)
    {
        Result result{};
        std::error_code ec;
        if (!options.force && fs::exists(job.output, ec) && fs::last_write_time(job.output, ec) >= image.sourceTime) {
            result.skipped = true;
            result.fileBytes = fs::file_size(job.output, ec);
            return result;
        }

        const KtxPtr texture = image.isDds ? repackDds(*image.file, job.space, options, result)
                                           : encodeDecoded(image, job, options, result);
        const fs::path temp = fs::path(job.output).concat(".tmp");
        check(ktxTexture_WriteToNamedFile(ktxTexture(texture.get()), temp.string().c_str()),
              "ktxTexture_WriteToNamedFile");
        fs::remove(job.output, ec);
        fs::rename(temp, job.output);
        result.fileBytes = fs::file_size(job.output, ec);
        return result;
    }

    // ── glTF ─────────────────────────────────────────────────────

    fs::path resolveModel(const fs::path& input)
    {
        if (!fs::is_directory(input)) {
            return input;
        }
        // same rule as the engine: a model folder holds one .gltf (non-recursive); pick the first by name
        std::vector<fs::path> candidates;
        for (const auto& entry : fs::directory_iterator(input)) {
            if (entry.is_regular_file() && entry.path().extension() == ".gltf") {
                candidates.push_back(entry.path());
            }
        }
        if (candidates.empty()) {
            throw std::runtime_error("no .gltf in " + input.string());
        }
        std::ranges::sort(candidates);
        return candidates.front();
    }

    // bytes of buffers[index], loading each external .bin once
    const std::vector<uint8_t>& bufferBytes(const Json& gltf, uint32_t index, const fs::path& modelDir,
                                            std::map<uint32_t, std::vector<uint8_t>>& cache)
    {
        if (const auto it = cache.find(index); it != cache.end()) {
            return it->second;
        }
        const Json& buffer = gltf.at("buffers").at(index);
        const std::string uri = buffer.value("uri", "");
        std::vector<uint8_t> bytes;
        if (isDataUri(uri)) {
            bytes = decodeBase64(std::string_view(uri).substr(uri.find(',') + 1));
        } else if (!uri.empty()) {
            bytes = readFile(modelDir / fs::path(percentDecode(uri)));
        } else {
            throw std::runtime_error("buffer without uri (GLB) is not supported");
        }
        return cache.emplace(index, std::move(bytes)).first->second;
    }

    // every images[] index a texture names: its source and each image extension's source
    template <typename Visit>
    void forEachImageRef(Json& textures, const Visit& visit)
    {
        for (Json& texture : textures) {
            if (texture.contains("source") && texture["source"].is_number_integer()) {
                visit(texture["source"]);
            }
            if (!texture.contains("extensions") || !texture["extensions"].is_object()) {
                continue;
            }
            for (auto it = texture["extensions"].begin(); it != texture["extensions"].end(); ++it) {
                Json& extension = it.value();
                if (extension.is_object() && extension.contains("source") && extension["source"].is_number_integer()) {
                    visit(extension["source"]);
                }
            }
        }
    }

    // MSFT_texture_dds: a repackable DDS replaces the texture's fallback source and the extension goes; fallback
    // images no texture names any more are removed
    void adoptDdsSources(Json& gltf, const fs::path& modelDir)
    {
        constexpr const char* kDdsExtension = "MSFT_texture_dds";
        if (!gltf.contains("images") || !gltf["images"].is_array() || !gltf.contains("textures") ||
            !gltf["textures"].is_array()) {
            return;
        }
        Json& images = gltf["images"];
        Json& textures = gltf["textures"];
        std::set<int64_t> replaced;
        bool isExtensionLeft = false;
        for (Json& texture : textures) {
            if (!texture.contains("extensions") || !texture["extensions"].is_object() ||
                !texture["extensions"].contains(kDdsExtension)) {
                continue;
            }
            Json& extensions = texture["extensions"];
            const Json& dds = extensions[kDdsExtension];
            const int64_t source =
                dds.is_object() && dds.contains("source") && dds["source"].is_number_integer() ? dds["source"].get<int64_t>()
                                                                                                : -1;
            const std::string uri = source >= 0 && static_cast<size_t>(source) < images.size()
                ? images[static_cast<size_t>(source)].value("uri", "")
                : std::string();
            if (uri.empty() || isDataUri(uri) || hasScheme(uri) ||
                !readDdsHeader(modelDir / fs::path(percentDecode(uri)))) {
                isExtensionLeft = true;
                continue;
            }
            if (texture.contains("source") && texture["source"].is_number_integer()) {
                replaced.insert(texture["source"].get<int64_t>());
            }
            texture["source"] = source;
            extensions.erase(kDdsExtension);
            if (extensions.empty()) {
                texture.erase("extensions");
            }
        }

        std::set<int64_t> referenced;
        forEachImageRef(textures, [&](const Json& ref) { referenced.insert(ref.get<int64_t>()); });
        std::vector<int64_t> remap(images.size());
        Json kept = Json::array();
        for (size_t i = 0; i < images.size(); ++i) {
            const auto index = static_cast<int64_t>(i);
            remap[i] = replaced.contains(index) && !referenced.contains(index) ? -1 : static_cast<int64_t>(kept.size());
            if (remap[i] >= 0) {
                kept.push_back(images[i]);
            }
        }
        if (kept.size() != images.size()) {
            images = std::move(kept);
            forEachImageRef(textures,
                            [&](Json& ref)
                            {
                                const auto index = ref.get<int64_t>();
                                if (index >= 0 && static_cast<size_t>(index) < remap.size()) {
                                    ref = remap[static_cast<size_t>(index)];
                                }
                            });
        }

        if (isExtensionLeft) {
            return;
        }
        for (const char* key : {"extensionsUsed", "extensionsRequired"}) {
            if (!gltf.contains(key) || !gltf[key].is_array()) {
                continue;
            }
            Json rest = Json::array();
            for (const Json& name : gltf[key]) {
                if (name != kDdsExtension) {
                    rest.push_back(name);
                }
            }
            if (rest.empty()) {
                gltf.erase(key);
            } else {
                gltf[key] = std::move(rest);
            }
        }
    }

    int run(const Options& options)
    {
        const fs::path modelPath = fs::absolute(resolveModel(options.input)).make_preferred();
        if (modelPath.extension() != ".gltf") {
            throw std::runtime_error("only .gltf is supported (got " + modelPath.string() + ")");
        }
        const fs::path modelDir = modelPath.parent_path();
        const fs::file_time_type modelTime = fs::last_write_time(modelPath);

        // parse from chars: libc++ has no char_traits<unsigned char> for a byte-vector input
        const std::vector<uint8_t> gltfBytes = readFile(modelPath);
        Json gltf = Json::parse(std::string(gltfBytes.begin(), gltfBytes.end()));
        const Json original = gltf;
        adoptDdsSources(gltf, modelDir);
        Json& images = gltf["images"];
        Json& textures = gltf["textures"];
        if (!images.is_array() || !textures.is_array()) {
            std::cout << "no images/textures in " << modelPath.string() << '\n';
            return 0;
        }

        // which images the materials sample, and how
        std::vector<TextureUse> uses;
        if (gltf.contains("materials")) {
            collectTextureUses(gltf["materials"], uses);
        }
        std::vector<ImageInfo> infos(images.size());
        const auto imageOfTexture = [&](int64_t textureIndex) -> std::optional<uint32_t>
        {
            if (textureIndex < 0 || static_cast<size_t>(textureIndex) >= textures.size()) {
                return std::nullopt;
            }
            const Json& texture = textures[static_cast<size_t>(textureIndex)];
            if (!texture.contains("source") || !texture["source"].is_number_integer()) {
                return std::nullopt;
            }
            const auto source = texture["source"].get<int64_t>();
            if (source < 0 || static_cast<size_t>(source) >= images.size()) {
                return std::nullopt;
            }
            return static_cast<uint32_t>(source);
        };
        for (const TextureUse& use : uses) {
            if (const auto image = imageOfTexture((*use.info)["index"].get<int64_t>())) {
                ImageInfo& info = infos[*image];
                (use.srgb ? info.usedSrgb : info.usedLinear) = true;
                info.usedAsNormal |= use.normal;
            }
        }

        // sources and output names
        std::map<uint32_t, std::vector<uint8_t>> buffers;
        std::set<fs::path> takenOutputs;
        std::vector<Job> jobs;
        // "<dir>/<base>.ktx2" next to the source; dir is the source URI's folder as written in the glTF
        const auto newOutput = [&](const std::string& uriDir, const std::string& base)
        {
            std::string name = base + ".ktx2";
            for (int n = 1; takenOutputs.contains(modelDir / fs::path(percentDecode(uriDir + name))); ++n) {
                name = std::format("{}_{}.ktx2", base, n);
            }
            const fs::path output = (modelDir / fs::path(percentDecode(uriDir + name))).make_preferred();
            takenOutputs.insert(output);
            return std::pair{output, uriDir + name};
        };
        for (uint32_t i = 0; i < images.size(); ++i) {
            ImageInfo& info = infos[i];
            if (!info.usedSrgb && !info.usedLinear) {
                continue;
            }
            const Json& image = images[i];
            const std::string uri = image.value("uri", "");
            const std::string mime = image.value("mimeType", "");

            // converted by an earlier run: the original URI is kept in extras
            std::string sourceUri = uri;
            std::string existingUri;
            if (image.contains("extras") && image["extras"].is_object() && image["extras"].contains(kExtrasKey)) {
                const Json& record = image["extras"][kExtrasKey];
                if (!record.is_object() || !record.contains("source") || !record["source"].is_string()) {
                    continue;
                }
                sourceUri = record["source"].get<std::string>();
                existingUri = uri;
            }

            std::string uriDir;
            info.label = !sourceUri.empty() && !isDataUri(sourceUri) ? sourceUri : std::format("image[{}]", i);
            if (!sourceUri.empty() && !isDataUri(sourceUri)) {
                if (hasScheme(sourceUri)) {
                    continue;
                }
                const fs::path file = modelDir / fs::path(percentDecode(sourceUri));
                std::error_code ec;
                const std::string extension = file.extension().string();
                info.isDds = isDdsExtension(extension);
                if ((!isPngOrJpeg(extension) && !info.isDds) || !fs::exists(file, ec)) {
                    continue; // already KTX2 / other format, or an embedded source that is gone
                }
                info.file = file;
                info.stem = safeFileStem(file.stem().string());
                info.sourceTime = fs::last_write_time(file);
                const auto slash = sourceUri.rfind('/');
                uriDir = slash == std::string::npos ? std::string() : sourceUri.substr(0, slash + 1);
            } else if (isDataUri(sourceUri)) {
                if (!existingUri.empty() || !isPngOrJpeg(sourceUri.substr(5, sourceUri.find(';') - 5))) {
                    continue;
                }
                info.bytes = decodeBase64(std::string_view(sourceUri).substr(sourceUri.find(',') + 1));
                info.stem = std::format("image{}", i);
                info.sourceTime = modelTime;
            } else if (image.contains("bufferView") && existingUri.empty()) {
                if (!isPngOrJpeg(mime)) {
                    continue;
                }
                const Json& view = gltf.at("bufferViews").at(image["bufferView"].get<size_t>());
                const std::vector<uint8_t>& buffer = bufferBytes(gltf, view.at("buffer").get<uint32_t>(), modelDir, buffers);
                const size_t offset = view.value("byteOffset", size_t{0});
                const auto length = view.at("byteLength").get<size_t>();
                if (offset + length > buffer.size()) {
                    throw std::runtime_error(std::format("image[{}] bufferView out of range", i));
                }
                info.bytes.assign(buffer.begin() + static_cast<std::ptrdiff_t>(offset),
                                  buffer.begin() + static_cast<std::ptrdiff_t>(offset + length));
                info.stem =
                    safeFileStem(image.contains("name") ? image["name"].get<std::string>() : std::format("image{}", i));
                info.sourceTime = modelTime;
            } else {
                continue;
            }
            info.sourceUri = isDataUri(sourceUri) || sourceUri.empty() ? std::string("embedded") : sourceUri;

            const bool both = info.usedSrgb && info.usedLinear;
            for (const Space space : {Space::Srgb, Space::Linear}) {
                if ((space == Space::Srgb && !info.usedSrgb) || (space == Space::Linear && !info.usedLinear)) {
                    continue;
                }
                Job job{.image = i, .space = space, .output = {}, .uri = {}};
                if (!both && !existingUri.empty()) {
                    // keep the name an earlier run chose, so re-runs are stable
                    job.uri = existingUri;
                    job.output = (modelDir / fs::path(percentDecode(existingUri))).make_preferred();
                    takenOutputs.insert(job.output);
                } else {
                    std::tie(job.output, job.uri) =
                        newOutput(uriDir, both ? info.stem + (space == Space::Srgb ? "_srgb" : "_linear") : info.stem);
                }
                jobs.push_back(std::move(job));
            }
        }

        // convert in parallel
        warmUpBasis(options);
        const uint32_t threadCount =
            std::max<uint32_t>(1,
                               std::min<uint32_t>(options.jobs > 0 ? options.jobs : std::thread::hardware_concurrency(),
                                                  static_cast<uint32_t>(jobs.size())));
        std::vector<Result> results(jobs.size());
        std::atomic<size_t> next{0};
        size_t done = 0;
        std::mutex printMutex;
        const auto start = std::chrono::steady_clock::now();
        {
            std::vector<std::jthread> workers;
            for (uint32_t w = 0; w < threadCount; ++w) {
                workers.emplace_back(
                    [&]
                    {
                        for (size_t k = next.fetch_add(1); k < jobs.size(); k = next.fetch_add(1)) {
                            const auto jobStart = std::chrono::steady_clock::now();
                            try {
                                results[k] = convert(infos[jobs[k].image], jobs[k], options);
                            } catch (const std::exception& error) {
                                results[k] = {.failed = true, .message = error.what()};
                            }
                            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::steady_clock::now() - jobStart)
                                                .count();
                            const Result& r = results[k];
                            const std::string status = r.failed ? std::string("FAILED")
                                : r.skipped                     ? std::string("up-to-date")
                                                                : std::format("{} ms", ms);
                            const std::lock_guard lock(printMutex);
                            std::cout << std::format("[{}/{}] {:<10} {} -> {} {}\n", ++done, jobs.size(), status,
                                                     infos[jobs[k].image].label, jobs[k].uri, r.message);
                        }
                    });
            }
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        // point converted images at their KTX2; a split image's linear file gets a cloned image entry
        std::map<uint32_t, uint32_t> linearImage; // image -> image entry used by its linear slots
        for (size_t k = 0; k < jobs.size(); ++k) {
            if (results[k].failed) {
                continue;
            }
            const Job& job = jobs[k];
            const bool both = infos[job.image].usedSrgb && infos[job.image].usedLinear;
            uint32_t target = job.image;
            if (both && job.space == Space::Linear) {
                target = static_cast<uint32_t>(images.size());
                images.push_back(images[job.image]);
                linearImage[job.image] = target;
            }
            Json& image = images[target];
            image.erase("bufferView");
            image["uri"] = job.uri;
            image["mimeType"] = "image/ktx2";
            if (!image.contains("extras") || !image["extras"].is_object()) {
                image["extras"] = Json::object();
            }
            image["extras"][kExtrasKey] = {
                {"source", infos[job.image].sourceUri},
                {"colorSpace", job.space == Space::Srgb ? "srgb" : "linear"},
            };
        }
        std::map<int64_t, int64_t> linearTextureClone;
        for (const TextureUse& use : uses) {
            const int64_t textureIndex = (*use.info)["index"].get<int64_t>();
            const auto image = imageOfTexture(textureIndex);
            if (use.srgb || !image || !linearImage.contains(*image)) {
                continue;
            }
            // linear slot of a split image: point it at a texture whose source is the linear clone
            auto [it, inserted] = linearTextureClone.try_emplace(textureIndex, static_cast<int64_t>(textures.size()));
            if (inserted) {
                Json clone = textures[static_cast<size_t>(textureIndex)];
                clone["source"] = linearImage[*image];
                textures.push_back(std::move(clone));
            }
            (*use.info)["index"] = it->second;
        }

        // rewrite the .gltf in place (first change keeps a .bak of the untouched file)
        const bool changed = gltf != original;
        if (changed) {
            const fs::path backup = fs::path(modelPath).concat(".bak");
            if (!fs::exists(backup)) {
                fs::copy_file(modelPath, backup);
            }
            const fs::path temp = fs::path(modelPath).concat(".tmp");
            {
                std::ofstream out(temp, std::ios::binary);
                out << gltf.dump(2) << '\n';
            }
            std::error_code ec;
            fs::remove(modelPath, ec);
            fs::rename(temp, modelPath);
        }

        uint64_t sourceBytes = 0;
        uint64_t gpuBytes = 0;
        uint64_t fileBytes = 0;
        size_t failed = 0;
        size_t skipped = 0;
        for (const Result& r : results) {
            sourceBytes += r.sourceBytes;
            gpuBytes += r.gpuBytes;
            fileBytes += r.fileBytes;
            failed += r.failed ? 1 : 0;
            skipped += r.skipped ? 1 : 0;
        }
        std::cout << std::format("\n{} textures ({} converted, {} up-to-date, {} failed) in {:.1f} s on {} threads\n",
                                 jobs.size(), jobs.size() - skipped - failed, skipped, failed, seconds, threadCount);
        if (sourceBytes > 0) {
            std::cout << std::format("GPU memory for converted textures: RGBA8 {:.1f} MiB -> BC {:.1f} MiB\n",
                                     sourceBytes / kMiB, gpuBytes / kMiB);
        }
        std::cout << std::format("KTX2 files: {:.1f} MiB\n{} {}\n", fileBytes / kMiB,
                                 changed ? "updated" : "unchanged", modelPath.string());
        return failed > 0 ? 1 : 0;
    }

    void printUsage()
    {
        std::cout << "usage: gltf_ktx2 <model.gltf | model folder> [--force] [--jobs N] [--uastc-level 0-4] "
                     "[--zstd 0-22]\n"
                     "  Converts PNG/JPEG textures to BC7 KTX2 (DDS: BCn repacked) next to their source images and\n"
                     "  updates the .gltf\n"
                     "  in place (the first change keeps <name>.gltf.bak).\n"
                     "  --force        reconvert even when the .ktx2 is newer than its source\n"
                     "  --jobs N       worker threads (default: all cores)\n"
                     "  --uastc-level  UASTC quality/speed 0 (fastest) .. 4 (slowest), default 2\n"
                     "  --zstd N       zstd level for the KTX2 payload, 0 = none, default 18\n";
    }
} // namespace

int main(int argc, char** argv)
{
    Options options{};
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto nextValue = [&]() -> uint32_t
        {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::format("{} needs a value", arg));
            }
            return static_cast<uint32_t>(std::stoul(argv[++i]));
        };
        try {
            if (arg == "--force") {
                options.force = true;
            } else if (arg == "--jobs") {
                options.jobs = nextValue();
            } else if (arg == "--uastc-level") {
                options.uastcLevel = std::min<uint32_t>(nextValue(), KTX_PACK_UASTC_MAX_LEVEL);
            } else if (arg == "--zstd") {
                options.zstdLevel = std::min<uint32_t>(nextValue(), kMaxZstdLevel);
            } else if (arg == "-h" || arg == "--help") {
                printUsage();
                return 0;
            } else if (options.input.empty()) {
                options.input = fs::path(arg);
            } else {
                throw std::runtime_error(std::format("unexpected argument '{}'", arg));
            }
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            printUsage();
            return 2;
        }
    }
    if (options.input.empty()) {
        printUsage();
        return 2;
    }
    try {
        return run(options);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
