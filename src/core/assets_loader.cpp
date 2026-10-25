#include "assets_loader.hpp"
#include "../Constants.h"
#include "../static_headers/logger.hpp"
#include "../util/vk_tracy.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <optional>
#include <thread>
#include <glm/gtc/quaternion.hpp>
#include <meshoptimizer.h>

namespace
{

    // ── glTF accessor helpers ───────────────────────────────────

    struct AccessorView
    {
        const uint8_t* data = nullptr; // null: no bufferView, elements are zero
        size_t count = 0;
        size_t stride = 0;
        int32_t componentType = 0;
        uint32_t compSize = 0;
        uint32_t numComp = 0;
    };

    // validates the accessor's byte range against its bufferView and buffer
    static std::optional<AccessorView> viewAccessor(const tg3_model& model, int32_t accessorIdx)
    {
        if (accessorIdx < 0 || static_cast<uint32_t>(accessorIdx) >= model.accessors_count) {
            return std::nullopt;
        }
        const tg3_accessor& acc = model.accessors[accessorIdx];
        const int32_t compSize = tg3_component_size(acc.component_type);
        const int32_t numComp = tg3_num_components(acc.type);
        if (compSize <= 0 || numComp <= 0) {
            return std::nullopt;
        }

        AccessorView view{
            .count = static_cast<size_t>(acc.count),
            .componentType = acc.component_type,
            .compSize = static_cast<uint32_t>(compSize),
            .numComp = static_cast<uint32_t>(numComp),
        };
        if (acc.buffer_view < 0) {
            return view;
        }
        if (static_cast<uint32_t>(acc.buffer_view) >= model.buffer_views_count) {
            return std::nullopt;
        }

        const tg3_buffer_view& bv = model.buffer_views[acc.buffer_view];
        if (bv.buffer < 0 || static_cast<uint32_t>(bv.buffer) >= model.buffers_count) {
            return std::nullopt;
        }
        const tg3_buffer& buf = model.buffers[bv.buffer];
        if (buf.data.data == nullptr || bv.byte_offset > buf.data.count ||
            bv.byte_length > buf.data.count - bv.byte_offset) {
            return std::nullopt;
        }

        const int32_t stride = tg3_accessor_byte_stride(&acc, &bv);
        if (stride <= 0) {
            return std::nullopt;
        }
        view.stride = static_cast<size_t>(stride);

        // count bounded by byte_length first so the span math cannot overflow
        const uint64_t elemBytes = static_cast<uint64_t>(compSize) * static_cast<uint64_t>(numComp);
        if (acc.count > bv.byte_length || acc.byte_offset > bv.byte_length) {
            return std::nullopt;
        }
        const uint64_t span = acc.count == 0 ? 0 : (acc.count - 1) * view.stride + elemBytes;
        if (span > bv.byte_length - acc.byte_offset) {
            return std::nullopt;
        }

        view.data = buf.data.data + bv.byte_offset + acc.byte_offset;
        return view;
    }

    // memcpy reads: bufferView offsets are not guaranteed aligned in malformed files
    template <typename T>
    static T loadUnaligned(const uint8_t* src)
    {
        T value;
        std::memcpy(&value, src, sizeof(T));
        return value;
    }

    // integer types are always treated as normalized
    static bool readFloatComponent(const uint8_t* src, int32_t componentType, float& out)
    {
        switch (componentType) {
        case TG3_COMPONENT_TYPE_FLOAT:
            out = loadUnaligned<float>(src);
            return true;
        case TG3_COMPONENT_TYPE_UNSIGNED_SHORT:
            out = static_cast<float>(loadUnaligned<uint16_t>(src)) / 65535.0f;
            return true;
        case TG3_COMPONENT_TYPE_UNSIGNED_BYTE:
            out = static_cast<float>(*src) / 255.0f;
            return true;
        case TG3_COMPONENT_TYPE_SHORT:
            out = glm::max(static_cast<float>(loadUnaligned<int16_t>(src)) / 32767.0f, -1.0f);
            return true;
        case TG3_COMPONENT_TYPE_BYTE:
            out = glm::max(static_cast<float>(loadUnaligned<int8_t>(src)) / 127.0f, -1.0f);
            return true;
        default:
            return false;
        }
    }

    static bool readUintComponent(const uint8_t* src, int32_t componentType, uint32_t& out)
    {
        switch (componentType) {
        case TG3_COMPONENT_TYPE_UNSIGNED_INT:
            out = loadUnaligned<uint32_t>(src);
            return true;
        case TG3_COMPONENT_TYPE_UNSIGNED_SHORT:
            out = loadUnaligned<uint16_t>(src);
            return true;
        case TG3_COMPONENT_TYPE_UNSIGNED_BYTE:
            out = *src;
            return true;
        default:
            return false;
        }
    }

    static tg3_span_u8 readBufferViewBytes(const tg3_model& model, int32_t bufferViewIdx);

    // overwrite sparse-substituted elements; sparse indices/values are tightly packed
    static bool applySparseFloats(const tg3_model& model, int32_t accessorIdx, const AccessorView& view,
                                  std::vector<float>& values)
    {
        const tg3_accessor_sparse& sparse = model.accessors[accessorIdx].sparse;
        if (sparse.is_sparse == 0 || sparse.count <= 0) {
            return true;
        }
        const tg3_span_u8 indexBytes = readBufferViewBytes(model, sparse.indices.buffer_view);
        const tg3_span_u8 valueBytes = readBufferViewBytes(model, sparse.values.buffer_view);
        const int32_t indexSize = tg3_component_size(sparse.indices.component_type);
        if (indexBytes.data == nullptr || valueBytes.data == nullptr || indexSize <= 0) {
            return false;
        }

        const auto count = static_cast<uint64_t>(sparse.count);
        const uint64_t elemBytes = static_cast<uint64_t>(view.compSize) * view.numComp;
        if (sparse.indices.byte_offset > indexBytes.count ||
            count * static_cast<uint64_t>(indexSize) > indexBytes.count - sparse.indices.byte_offset ||
            sparse.values.byte_offset > valueBytes.count ||
            count * elemBytes > valueBytes.count - sparse.values.byte_offset) {
            return false;
        }

        const uint8_t* indexSrc = indexBytes.data + sparse.indices.byte_offset;
        const uint8_t* valueSrc = valueBytes.data + sparse.values.byte_offset;
        for (uint64_t i = 0; i < count; ++i) {
            uint32_t target = 0;
            if (!readUintComponent(indexSrc + i * static_cast<uint64_t>(indexSize), sparse.indices.component_type,
                                   target) ||
                target >= view.count) {
                return false;
            }
            for (uint32_t c = 0; c < view.numComp; ++c) {
                const uint8_t* src = valueSrc + i * elemBytes + static_cast<uint64_t>(c) * view.compSize;
                if (!readFloatComponent(src, view.componentType,
                                        values[static_cast<size_t>(target) * view.numComp + c])) {
                    return false;
                }
            }
        }
        return true;
    }

    // empty on any error (out-of-range buffer, unsupported type)
    static std::vector<float> readAccessorFloats(const tg3_model& model, int32_t accessorIdx)
    {
        const std::optional<AccessorView> view = viewAccessor(model, accessorIdx);
        if (!view) {
            return {};
        }

        std::vector<float> result(view->count * view->numComp, 0.0f);
        if (view->data != nullptr) {
            const size_t elemBytes = static_cast<size_t>(view->compSize) * view->numComp;
            if (view->componentType == TG3_COMPONENT_TYPE_FLOAT && view->stride == elemBytes) {
                std::memcpy(result.data(), view->data, result.size() * sizeof(float));
            } else {
                for (size_t elem = 0; elem < view->count; ++elem) {
                    const uint8_t* elemSrc = view->data + elem * view->stride;
                    for (uint32_t c = 0; c < view->numComp; ++c) {
                        if (!readFloatComponent(elemSrc + static_cast<size_t>(c) * view->compSize,
                                                view->componentType, result[elem * view->numComp + c])) {
                            return {};
                        }
                    }
                }
            }
        }

        if (!applySparseFloats(model, accessorIdx, *view, result)) {
            return {};
        }
        return result;
    }

    // unsigned data (indices, JOINTS_n): UINT32, UINT16, UINT8
    static std::vector<uint32_t> readAccessorU32(const tg3_model& model, int32_t accessorIdx)
    {
        const std::optional<AccessorView> view = viewAccessor(model, accessorIdx);
        if (!view) {
            return {};
        }

        std::vector<uint32_t> result(view->count * view->numComp, 0u);
        if (view->data == nullptr) {
            return result;
        }
        for (size_t elem = 0; elem < view->count; ++elem) {
            const uint8_t* elemSrc = view->data + elem * view->stride;
            for (uint32_t c = 0; c < view->numComp; ++c) {
                if (!readUintComponent(elemSrc + static_cast<size_t>(c) * view->compSize, view->componentType,
                                       result[elem * view->numComp + c])) {
                    return {};
                }
            }
        }
        return result;
    }

    static std::vector<uint32_t> readAccessorIndices(const tg3_model& model, int32_t accessorIdx)
    {
        if (accessorIdx < 0 || static_cast<uint32_t>(accessorIdx) >= model.accessors_count ||
            model.accessors[accessorIdx].buffer_view < 0 || tg3_num_components(model.accessors[accessorIdx].type) != 1) {
            return {};
        }
        return readAccessorU32(model, accessorIdx);
    }

    static tg3_span_u8 readBufferViewBytes(const tg3_model& model, int32_t bufferViewIdx)
    {
        if (bufferViewIdx < 0 || static_cast<uint32_t>(bufferViewIdx) >= model.buffer_views_count)
            return {};

        const tg3_buffer_view& bv = model.buffer_views[bufferViewIdx];
        if (bv.buffer < 0 || static_cast<uint32_t>(bv.buffer) >= model.buffers_count)
            return {};

        const tg3_buffer& buf = model.buffers[bv.buffer];
        if (!buf.data.data)
            return {};

        if (bv.byte_offset > buf.data.count || bv.byte_length > buf.data.count - bv.byte_offset)
            return {};

        return tg3_span_u8{.data = buf.data.data + bv.byte_offset, .count = bv.byte_length};
    }

    static std::string_view strView(tg3_str s)
    {
        return s.data ? std::string_view(s.data, s.len) : std::string_view{};
    }

    static const char* primitiveModeName(int32_t mode)
    {
        switch (mode) {
        case TG3_MODE_POINTS:
            return "POINTS";
        case TG3_MODE_LINE:
            return "LINE";
        case TG3_MODE_LINE_LOOP:
            return "LINE_LOOP";
        case TG3_MODE_LINE_STRIP:
            return "LINE_STRIP";
        case TG3_MODE_TRIANGLE_STRIP:
            return "TRIANGLE_STRIP";
        case TG3_MODE_TRIANGLE_FAN:
            return "TRIANGLE_FAN";
        case TG3_MODE_TRIANGLES:
        default:
            return "TRIANGLES";
        }
    }

    // Runs body(i) for every i in [0, count) on the calling thread plus up to hardware_concurrency-1 workers.
    // body must not throw. Returns the number of threads used.
    template <typename Body>
    static size_t parallelFor(size_t count, const Body& body)
    {
        if (count == 0) {
            return 0;
        }
        std::atomic<size_t> next{0};
        const auto loop = [&] {
            for (size_t i = next.fetch_add(1); i < count; i = next.fetch_add(1)) {
                body(i);
            }
        };
        const size_t workerCount = std::min<size_t>(count, std::max(1u, std::thread::hardware_concurrency())) - 1;
        std::vector<std::jthread> workers;
        workers.reserve(workerCount);
        for (size_t w = 0; w < workerCount; ++w) {
            workers.emplace_back(loop);
        }
        loop();
        return workerCount + 1;
    }

    struct GltfImageSrc
    {
        std::string cacheKey;
        std::string path;
        std::vector<uint8_t> encoded;
        std::vector<uint8_t> decodedRgba;
        // filled by uploadGltfImages from encoded / path; released right after its group is uploaded
        TextureManager::DecodedImage predecoded;
        // color spaces the model's materials sample this image in (collectGltfImageUses)
        bool usedSrgb = false;
        bool usedLinear = false;
        int width = 0;
        int height = 0;
        std::string mime;
        uint32_t heapSrgb = kNoneIndex;
        uint32_t heapLinear = kNoneIndex;
        bool isBroken = false;
    };

    struct GltfResolvedTexture
    {
        int32_t imageIndex = -1;
        uint32_t samplerHeap = 0;
    };

    struct GltfLoadCtx
    {
        GeometryStore& geometry;
        MaterialStore& materials;
        LightStore& lights;
        TextureManager& textures;
        std::vector<GltfImageSrc> images{};
        std::vector<GltfResolvedTexture> gltfTextures{};
        std::vector<uint32_t> materialIds{};
        uint32_t defaultSamplerHeap = 0;
        // true while collectGltfImageUses walks the materials: texture lookups only record usage
        bool collectImageUses = false;
        // every heap index the model got from a texture load (may repeat)
        std::vector<uint32_t> textureRefs{};
    };

    static std::string extrasJsonOf(const tg3_extras_ext& ext)
    {
        if (ext.extras_json.data != nullptr && ext.extras_json.len > 0) {
            return {ext.extras_json.data, ext.extras_json.len};
        }
        if (ext.extras != nullptr) {
            return "{}";
        }
        return {};
    }

    static void appendJsonString(std::string& out, std::string_view text)
    {
        out += '"';
        for (const char c : text) {
            switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
                } else {
                    out += c;
                }
            }
        }
        out += '"';
    }

    // extensions_json holds the whole "extensions" object; each entry needs only its own value
    static void appendJsonValue(std::string& out, const tg3_value& value)
    {
        switch (value.type) {
        case TG3_VALUE_BOOL:
            out += value.bool_val != 0 ? "true" : "false";
            return;
        case TG3_VALUE_INT:
            out += std::to_string(value.int_val);
            return;
        case TG3_VALUE_REAL:
            out += std::isfinite(value.real_val) ? std::format("{}", value.real_val) : "null";
            return;
        case TG3_VALUE_STRING:
            appendJsonString(out, strView(value.string_val));
            return;
        case TG3_VALUE_ARRAY:
            out += '[';
            for (uint32_t i = 0; i < value.array_count && value.array_data != nullptr; ++i) {
                if (i > 0) {
                    out += ',';
                }
                appendJsonValue(out, value.array_data[i]);
            }
            out += ']';
            return;
        case TG3_VALUE_OBJECT:
            out += '{';
            for (uint32_t i = 0; i < value.object_count && value.object_data != nullptr; ++i) {
                if (i > 0) {
                    out += ',';
                }
                appendJsonString(out, strView(value.object_data[i].key));
                out += ':';
                appendJsonValue(out, value.object_data[i].value);
            }
            out += '}';
            return;
        default:
            out += "null";
            return;
        }
    }

    static void storeAux(GeometryStore& geometry, uint32_t kind, uint32_t index, const tg3_extras_ext& ext)
    {
        AuxBlob blob{.ownerKind = kind, .ownerIndex = index, .extrasJson = extrasJsonOf(ext), .extensions = {}};
        for (uint32_t i = 0; i < ext.extensions_count; ++i) {
            std::string json;
            appendJsonValue(json, ext.extensions[i].value);
            blob.extensions.emplace_back(std::string(strView(ext.extensions[i].name)), std::move(json));
        }
        if (blob.extrasJson.empty() && blob.extensions.empty()) {
            return;
        }
        geometry.auxBlobs.push_back(std::move(blob));
    }

    static int base64Value(char c)
    {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '+') {
            return 62;
        }
        if (c == '/') {
            return 63;
        }
        return -1;
    }

    static std::vector<uint8_t> decodeBase64(std::string_view in)
    {
        std::vector<uint8_t> out;
        out.reserve(in.size() * 3 / 4);
        int val = 0;
        int valb = -8;
        for (const unsigned char c : in) {
            if (c == '=' || c == '\n' || c == '\r') {
                continue;
            }
            const int d = base64Value(static_cast<char>(c));
            if (d < 0) {
                continue;
            }
            val = (val << 6) + d;
            valb += 6;
            if (valb >= 0) {
                out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        return out;
    }

    static std::vector<uint8_t> decodeDataUri(std::string_view uri)
    {
        const auto comma = uri.find(',');
        if (comma == std::string_view::npos) {
            return {};
        }
        const std::string_view header = uri.substr(0, comma);
        const std::string_view payload = uri.substr(comma + 1);
        if (header.find("base64") == std::string_view::npos) {
            return {payload.begin(), payload.end()};
        }
        return decodeBase64(payload);
    }

    static uint32_t loadGltfImage(GltfLoadCtx& ctx, GltfImageSrc& image, TextureColorSpace colorSpace)
    {
        uint32_t& heap = colorSpace == TextureColorSpace::Srgb ? image.heapSrgb : image.heapLinear;
        if (heap != kNoneIndex || image.isBroken) {
            return heap;
        }
        // one undecodable image must not abort the model; material falls back to no texture
        try {
            if (image.predecoded.pixels) {
                heap = ctx.textures.loadTextureFromPixels(image.cacheKey, image.predecoded.rgba(),
                                                          static_cast<uint32_t>(image.predecoded.width),
                                                          static_cast<uint32_t>(image.predecoded.height), colorSpace);
            } else if (!image.decodedRgba.empty() && image.width > 0 && image.height > 0) {
                heap = ctx.textures.loadTextureFromPixels(image.cacheKey, image.decodedRgba,
                                                          static_cast<uint32_t>(image.width),
                                                          static_cast<uint32_t>(image.height), colorSpace);
            } else if (!image.encoded.empty()) {
                heap = ctx.textures.loadTextureFromMemory(image.cacheKey, image.encoded, image.mime, colorSpace);
            } else if (!image.path.empty()) {
                heap = ctx.textures.loadTexture(image.path, colorSpace);
            }
        } catch (const std::runtime_error& error) {
            image.isBroken = true;
            logError(std::format("glTF image '{}' failed to load: {}", image.cacheKey, error.what()), "AssetLoader");
        }
        if (heap != kNoneIndex) {
            ctx.textureRefs.push_back(heap);
        }
        return heap;
    }

    static uint32_t resolveTextureImage(GltfLoadCtx& ctx, int32_t textureIndex, TextureColorSpace colorSpace)
    {
        if (textureIndex < 0 || static_cast<uint32_t>(textureIndex) >= ctx.gltfTextures.size()) {
            return kNoneIndex;
        }
        const int32_t imageIndex = ctx.gltfTextures[static_cast<uint32_t>(textureIndex)].imageIndex;
        if (imageIndex < 0 || static_cast<uint32_t>(imageIndex) >= ctx.images.size()) {
            return kNoneIndex;
        }
        GltfImageSrc& image = ctx.images[static_cast<uint32_t>(imageIndex)];
        if (ctx.collectImageUses) {
            (colorSpace == TextureColorSpace::Srgb ? image.usedSrgb : image.usedLinear) = true;
            return kNoneIndex;
        }
        return loadGltfImage(ctx, image, colorSpace);
    }

    static uint32_t resolveTextureSampler(const GltfLoadCtx& ctx, int32_t textureIndex)
    {
        if (textureIndex < 0 || static_cast<uint32_t>(textureIndex) >= ctx.gltfTextures.size()) {
            return ctx.defaultSamplerHeap;
        }
        return ctx.gltfTextures[static_cast<uint32_t>(textureIndex)].samplerHeap;
    }

    static void parseGltfExtras(GeometryStore& geometry, uint32_t kind, uint32_t index, const tg3_extras_ext& ext,
                                std::string_view owner)
    {
        if (ext.extras != nullptr) {
            logDebug(std::format("glTF extras on {}", owner), "AssetLoader");
        }
        for (uint32_t i = 0; i < ext.extensions_count; ++i) {
            const std::string_view name = strView(ext.extensions[i].name);
            logDebug(std::format("glTF extension '{}' on {}", name, owner), "AssetLoader");
        }
        storeAux(geometry, kind, index, ext);
    }

    static void parseGltfRootExtensions(GltfLoadCtx& ctx, const tg3_model& model)
    {
        logDebug(std::format("glTF extensionsUsed={} extensionsRequired={}", model.extensions_used_count,
                              model.extensions_required_count),
                  "AssetLoader");
        for (uint32_t i = 0; i < model.extensions_used_count; ++i) {
            const std::string_view name = strView(model.extensions_used[i]);
            logDebug(std::format("glTF extensionsUsed[{}]='{}'", i, name), "AssetLoader");
            ctx.geometry.extensionsUsed.emplace_back(name);
        }
        for (uint32_t i = 0; i < model.extensions_required_count; ++i) {
            const std::string_view name = strView(model.extensions_required[i]);
            logDebug(std::format("glTF extensionsRequired[{}]='{}'", i, name), "AssetLoader");
            ctx.geometry.extensionsRequired.emplace_back(name);
        }
        parseGltfExtras(ctx.geometry, AuxOwnerKind::Model, 0, model.ext, "model");
        parseGltfExtras(ctx.geometry, AuxOwnerKind::Asset, 0, model.asset.ext, "asset");
    }

    static std::vector<uint32_t> parseGltfSamplers(GltfLoadCtx& ctx, const tg3_model& model)
    {
        logDebug(std::format("glTF samplers: {}", model.samplers_count), "AssetLoader");
        std::vector<uint32_t> heapIndices(model.samplers_count, ctx.defaultSamplerHeap);
        for (uint32_t i = 0; i < model.samplers_count; ++i) {
            const tg3_sampler& sampler = model.samplers[i];
            logDebug(std::format("glTF sampler[{}] name='{}' minFilter={} magFilter={} wrapS={} wrapT={}", i,
                                  strView(sampler.name), sampler.min_filter, sampler.mag_filter, sampler.wrap_s,
                                  sampler.wrap_t),
                      "AssetLoader");
            heapIndices[i] =
                ctx.textures.getOrCreateSampler(sampler.min_filter, sampler.mag_filter, sampler.wrap_s, sampler.wrap_t);
            parseGltfExtras(ctx.geometry, AuxOwnerKind::Sampler, i, sampler.ext, std::format("sampler[{}]", i));
        }
        return heapIndices;
    }

    static void parseGltfTextures(GltfLoadCtx& ctx, const tg3_model& model, const std::vector<uint32_t>& samplerHeaps)
    {
        logDebug(std::format("glTF textures: {}", model.textures_count), "AssetLoader");
        ctx.gltfTextures.resize(model.textures_count);
        for (uint32_t i = 0; i < model.textures_count; ++i) {
            const tg3_texture& texture = model.textures[i];
            logDebug(std::format("glTF texture[{}] name='{}' source={} sampler={}", i, strView(texture.name),
                                  texture.source, texture.sampler),
                      "AssetLoader");
            uint32_t samplerHeap = ctx.defaultSamplerHeap;
            if (texture.sampler >= 0 && static_cast<uint32_t>(texture.sampler) < samplerHeaps.size()) {
                samplerHeap = samplerHeaps[static_cast<uint32_t>(texture.sampler)];
            }
            ctx.gltfTextures[i] = GltfResolvedTexture{.imageIndex = texture.source, .samplerHeap = samplerHeap};
            parseGltfExtras(ctx.geometry, AuxOwnerKind::Texture, i, texture.ext, std::format("texture[{}]", i));
        }
    }

    static void parseGltfTextureInfo(GltfLoadCtx& ctx, const tg3_texture_info& info, std::string_view slot)
    {
        logDebug(std::format("glTF {} index={} texCoord={}", slot, info.index, info.tex_coord), "AssetLoader");
        parseGltfExtras(ctx.geometry, AuxOwnerKind::Texture, info.index < 0 ? 0 : static_cast<uint32_t>(info.index),
                        info.ext, slot);
    }

    static const tg3_value* objectField(const tg3_value* obj, std::string_view key)
    {
        if (obj == nullptr || obj->type != TG3_VALUE_OBJECT || obj->object_data == nullptr) {
            return nullptr;
        }
        for (uint32_t i = 0; i < obj->object_count; ++i) {
            if (strView(obj->object_data[i].key) == key) {
                return &obj->object_data[i].value;
            }
        }
        return nullptr;
    }

    static const tg3_value* findExtensionValue(const tg3_extras_ext& ext, std::string_view name)
    {
        for (uint32_t i = 0; i < ext.extensions_count; ++i) {
            if (strView(ext.extensions[i].name) == name) {
                return &ext.extensions[i].value;
            }
        }
        return nullptr;
    }

    static float valueAsFloat(const tg3_value* value, float fallback)
    {
        if (value == nullptr) {
            return fallback;
        }
        if (value->type == TG3_VALUE_REAL) {
            return static_cast<float>(value->real_val);
        }
        if (value->type == TG3_VALUE_INT) {
            return static_cast<float>(value->int_val);
        }
        return fallback;
    }

    static int32_t valueAsInt(const tg3_value* value, int32_t fallback)
    {
        if (value == nullptr) {
            return fallback;
        }
        if (value->type == TG3_VALUE_INT) {
            return static_cast<int32_t>(value->int_val);
        }
        if (value->type == TG3_VALUE_REAL) {
            return static_cast<int32_t>(value->real_val);
        }
        return fallback;
    }

    static glm::vec2 valueAsVec2(const tg3_value* value, glm::vec2 fallback)
    {
        if (value == nullptr || value->type != TG3_VALUE_ARRAY || value->array_data == nullptr || value->array_count < 2) {
            return fallback;
        }
        return {valueAsFloat(&value->array_data[0], fallback.x), valueAsFloat(&value->array_data[1], fallback.y)};
    }

    static glm::vec3 valueAsVec3(const tg3_value* value, glm::vec3 fallback)
    {
        if (value == nullptr || value->type != TG3_VALUE_ARRAY || value->array_data == nullptr || value->array_count < 3) {
            return fallback;
        }
        return {valueAsFloat(&value->array_data[0], fallback.x), valueAsFloat(&value->array_data[1], fallback.y),
                valueAsFloat(&value->array_data[2], fallback.z)};
    }

    static void parseTextureTransform(const tg3_value* transform, uint8_t& uv, MaterialUvTransform& xform)
    {
        if (transform == nullptr) {
            return;
        }
        xform.offset = valueAsVec2(objectField(transform, "offset"), xform.offset);
        xform.scale = valueAsVec2(objectField(transform, "scale"), xform.scale);
        xform.rotation = valueAsFloat(objectField(transform, "rotation"), xform.rotation);
        const int32_t texCoord = valueAsInt(objectField(transform, "texCoord"), -1);
        if (texCoord >= 0) {
            uv = static_cast<uint8_t>(texCoord);
        }
    }

    static const tg3_value* nestedExtension(const tg3_value* obj, std::string_view name)
    {
        return objectField(objectField(obj, "extensions"), name);
    }

    static void parseCoreTextureTransform(const tg3_extras_ext& ext, uint8_t& uv, MaterialUvTransform& xform)
    {
        parseTextureTransform(findExtensionValue(ext, "KHR_texture_transform"), uv, xform);
    }

    static MaterialTextureRef parseExtensionTexture(GltfLoadCtx& ctx, const tg3_value* textureInfo,
                                                    TextureColorSpace colorSpace, std::string_view slot)
    {
        MaterialTextureRef ref{};
        if (textureInfo == nullptr || textureInfo->type != TG3_VALUE_OBJECT) {
            return ref;
        }
        const int32_t index = valueAsInt(objectField(textureInfo, "index"), -1);
        const int32_t texCoord = valueAsInt(objectField(textureInfo, "texCoord"), 0);
        ref.uv = static_cast<uint8_t>(std::max(texCoord, 0));
        ref.normalScale = valueAsFloat(objectField(textureInfo, "scale"), 1.0f);
        parseTextureTransform(nestedExtension(textureInfo, "KHR_texture_transform"), ref.uv, ref.uvXform);
        if (index >= 0) {
            ref.tex = resolveTextureImage(ctx, index, colorSpace);
            ref.samp = resolveTextureSampler(ctx, index);
        }
        logDebug(
            std::format("glTF {} index={} texCoord={} heap={} sampler={}", slot, index, ref.uv, ref.tex, ref.samp),
            "AssetLoader");
        return ref;
    }

    static void parseGltfMaterialPbrExtensions(GltfLoadCtx& ctx, const tg3_material& material, GpuMaterial& gpu,
                                               MaterialPbrExtension& ext, std::string_view owner)
    {
        const auto slot = [owner](std::string_view name) { return std::format("{}.{}", owner, name); };

        parseCoreTextureTransform(material.pbr_metallic_roughness.base_color_texture.ext, gpu.baseColorUv, ext.baseColorUv);
        parseCoreTextureTransform(material.pbr_metallic_roughness.metallic_roughness_texture.ext, gpu.metalRoughUv,
                                  ext.metalRoughUv);
        parseCoreTextureTransform(material.normal_texture.ext, gpu.normalUv, ext.normalUv);
        parseCoreTextureTransform(material.occlusion_texture.ext, gpu.occlusionUv, ext.occlusionUv);
        parseCoreTextureTransform(material.emissive_texture.ext, gpu.emissiveUv, ext.emissiveUv);

        for (uint32_t i = 0; i < material.ext.extensions_count; ++i) {
            const std::string_view name = strView(material.ext.extensions[i].name);
            const tg3_value* value = &material.ext.extensions[i].value;

            if (name == "KHR_materials_unlit") {
                ext.flags |= MaterialExtFlag::Unlit;
                gpu.flags |= GpuMaterialFlag::Unlit;
                logDebug(std::format("glTF {} KHR_materials_unlit", owner), "AssetLoader");
            } else if (name == "KHR_materials_emissive_strength") {
                ext.flags |= MaterialExtFlag::EmissiveStrength;
                ext.emissiveStrength = valueAsFloat(objectField(value, "emissiveStrength"), 1.0f);
                logDebug(std::format("glTF {} KHR_materials_emissive_strength={}", owner, ext.emissiveStrength),
                          "AssetLoader");
            } else if (name == "KHR_materials_ior") {
                ext.flags |= MaterialExtFlag::Ior;
                ext.ior = valueAsFloat(objectField(value, "ior"), 1.5f);
                logDebug(std::format("glTF {} KHR_materials_ior={}", owner, ext.ior), "AssetLoader");
            } else if (name == "KHR_materials_dispersion") {
                ext.flags |= MaterialExtFlag::Dispersion;
                ext.dispersion = valueAsFloat(objectField(value, "dispersion"), 0.0f);
                logDebug(std::format("glTF {} KHR_materials_dispersion={}", owner, ext.dispersion), "AssetLoader");
            } else if (name == "KHR_materials_specular") {
                ext.flags |= MaterialExtFlag::Specular;
                ext.specularFactor = valueAsFloat(objectField(value, "specularFactor"), 1.0f);
                ext.specularColorFactor = valueAsVec3(objectField(value, "specularColorFactor"), glm::vec3{1.0f});
                ext.specular = parseExtensionTexture(ctx, objectField(value, "specularTexture"), TextureColorSpace::Linear,
                                                     slot("specularTexture"));
                ext.specularColor = parseExtensionTexture(ctx, objectField(value, "specularColorTexture"),
                                                          TextureColorSpace::Srgb, slot("specularColorTexture"));
                logDebug(std::format("glTF {} KHR_materials_specular factor={} color=({}, {}, {})", owner,
                                      ext.specularFactor, ext.specularColorFactor.x, ext.specularColorFactor.y,
                                      ext.specularColorFactor.z),
                          "AssetLoader");
            } else if (name == "KHR_materials_clearcoat") {
                ext.flags |= MaterialExtFlag::Clearcoat;
                ext.clearcoatFactor = valueAsFloat(objectField(value, "clearcoatFactor"), 0.0f);
                ext.clearcoatRoughnessFactor = valueAsFloat(objectField(value, "clearcoatRoughnessFactor"), 0.0f);
                ext.clearcoat = parseExtensionTexture(ctx, objectField(value, "clearcoatTexture"),
                                                      TextureColorSpace::Linear, slot("clearcoatTexture"));
                ext.clearcoatRoughness =
                    parseExtensionTexture(ctx, objectField(value, "clearcoatRoughnessTexture"), TextureColorSpace::Linear,
                                          slot("clearcoatRoughnessTexture"));
                ext.clearcoatNormal = parseExtensionTexture(ctx, objectField(value, "clearcoatNormalTexture"),
                                                            TextureColorSpace::Linear, slot("clearcoatNormalTexture"));
                logDebug(std::format("glTF {} KHR_materials_clearcoat factor={} roughness={}", owner,
                                      ext.clearcoatFactor, ext.clearcoatRoughnessFactor),
                          "AssetLoader");
            } else if (name == "KHR_materials_sheen") {
                ext.flags |= MaterialExtFlag::Sheen;
                ext.sheenColorFactor = valueAsVec3(objectField(value, "sheenColorFactor"), glm::vec3{0.0f});
                ext.sheenRoughnessFactor = valueAsFloat(objectField(value, "sheenRoughnessFactor"), 0.0f);
                ext.sheenColor = parseExtensionTexture(ctx, objectField(value, "sheenColorTexture"), TextureColorSpace::Srgb,
                                                       slot("sheenColorTexture"));
                ext.sheenRoughness = parseExtensionTexture(ctx, objectField(value, "sheenRoughnessTexture"),
                                                           TextureColorSpace::Linear, slot("sheenRoughnessTexture"));
                logDebug(std::format("glTF {} KHR_materials_sheen color=({}, {}, {}) roughness={}", owner,
                                      ext.sheenColorFactor.x, ext.sheenColorFactor.y, ext.sheenColorFactor.z,
                                      ext.sheenRoughnessFactor),
                          "AssetLoader");
            } else if (name == "KHR_materials_transmission") {
                ext.flags |= MaterialExtFlag::Transmission;
                ext.transmissionFactor = valueAsFloat(objectField(value, "transmissionFactor"), 0.0f);
                ext.transmission = parseExtensionTexture(ctx, objectField(value, "transmissionTexture"),
                                                         TextureColorSpace::Linear, slot("transmissionTexture"));
                logDebug(std::format("glTF {} KHR_materials_transmission={}", owner, ext.transmissionFactor),
                          "AssetLoader");
            } else if (name == "KHR_materials_volume") {
                ext.flags |= MaterialExtFlag::Volume;
                ext.thicknessFactor = valueAsFloat(objectField(value, "thicknessFactor"), 0.0f);
                ext.attenuationDistance = valueAsFloat(objectField(value, "attenuationDistance"), 0.0f);
                ext.attenuationColor = valueAsVec3(objectField(value, "attenuationColor"), glm::vec3{1.0f});
                ext.thickness = parseExtensionTexture(ctx, objectField(value, "thicknessTexture"), TextureColorSpace::Linear,
                                                      slot("thicknessTexture"));
                logDebug(std::format("glTF {} KHR_materials_volume thickness={} attenDist={} color=({}, {}, {})",
                                      owner, ext.thicknessFactor, ext.attenuationDistance, ext.attenuationColor.x,
                                      ext.attenuationColor.y, ext.attenuationColor.z),
                          "AssetLoader");
            } else if (name == "KHR_materials_iridescence") {
                ext.flags |= MaterialExtFlag::Iridescence;
                ext.iridescenceFactor = valueAsFloat(objectField(value, "iridescenceFactor"), 0.0f);
                ext.iridescenceIor = valueAsFloat(objectField(value, "iridescenceIor"), 1.3f);
                ext.iridescenceThicknessMin = valueAsFloat(objectField(value, "iridescenceThicknessMinimum"), 100.0f);
                ext.iridescenceThicknessMax = valueAsFloat(objectField(value, "iridescenceThicknessMaximum"), 400.0f);
                ext.iridescence = parseExtensionTexture(ctx, objectField(value, "iridescenceTexture"),
                                                        TextureColorSpace::Linear, slot("iridescenceTexture"));
                ext.iridescenceThickness =
                    parseExtensionTexture(ctx, objectField(value, "iridescenceThicknessTexture"), TextureColorSpace::Linear,
                                          slot("iridescenceThicknessTexture"));
                logDebug(std::format("glTF {} KHR_materials_iridescence factor={} ior={} thickness=[{}, {}]", owner,
                                      ext.iridescenceFactor, ext.iridescenceIor, ext.iridescenceThicknessMin,
                                      ext.iridescenceThicknessMax),
                          "AssetLoader");
            } else if (name == "KHR_materials_anisotropy") {
                ext.flags |= MaterialExtFlag::Anisotropy;
                ext.anisotropyStrength = valueAsFloat(objectField(value, "anisotropyStrength"), 0.0f);
                ext.anisotropyRotation = valueAsFloat(objectField(value, "anisotropyRotation"), 0.0f);
                ext.anisotropy = parseExtensionTexture(ctx, objectField(value, "anisotropyTexture"),
                                                       TextureColorSpace::Linear, slot("anisotropyTexture"));
                logDebug(std::format("glTF {} KHR_materials_anisotropy strength={} rotation={}", owner,
                                      ext.anisotropyStrength, ext.anisotropyRotation),
                          "AssetLoader");
            } else if (name == "KHR_materials_diffuse_transmission") {
                ext.flags |= MaterialExtFlag::DiffuseTransmission;
                ext.diffuseTransmissionFactor = valueAsFloat(objectField(value, "diffuseTransmissionFactor"), 0.0f);
                ext.diffuseTransmissionColorFactor =
                    valueAsVec3(objectField(value, "diffuseTransmissionColorFactor"), glm::vec3{1.0f});
                ext.diffuseTransmission =
                    parseExtensionTexture(ctx, objectField(value, "diffuseTransmissionTexture"), TextureColorSpace::Linear,
                                          slot("diffuseTransmissionTexture"));
                ext.diffuseTransmissionColor =
                    parseExtensionTexture(ctx, objectField(value, "diffuseTransmissionColorTexture"), TextureColorSpace::Srgb,
                                          slot("diffuseTransmissionColorTexture"));
                logDebug(std::format("glTF {} KHR_materials_diffuse_transmission factor={} color=({}, {}, {})", owner,
                                      ext.diffuseTransmissionFactor, ext.diffuseTransmissionColorFactor.x,
                                      ext.diffuseTransmissionColorFactor.y, ext.diffuseTransmissionColorFactor.z),
                          "AssetLoader");
            } else {
                logDebug(std::format("glTF {} skipped non-PBR extension '{}'", owner, name), "AssetLoader");
            }
        }
    }

    static uint32_t alphaModeFlags(std::string_view mode, int32_t doubleSided)
    {
        uint32_t flags = GpuMaterialFlag::AlphaOpaque;
        if (mode == "MASK") {
            flags = GpuMaterialFlag::AlphaMask;
        } else if (mode == "BLEND") {
            flags = GpuMaterialFlag::AlphaBlend;
        }
        if (doubleSided != 0) {
            flags |= GpuMaterialFlag::DoubleSided;
        }
        return flags;
    }

    static void parseGltfMaterials(GltfLoadCtx& ctx, const tg3_model& model)
    {
        logDebug(std::format("glTF materials: {}", model.materials_count), "AssetLoader");
        ctx.materialIds.resize(model.materials_count);
        for (uint32_t i = 0; i < model.materials_count; ++i) {
            const tg3_material& material = model.materials[i];
            const tg3_pbr_metallic_roughness& pbr = material.pbr_metallic_roughness;
            const std::string owner = std::format("material[{}]", i);
            logDebug(std::format("glTF {} name='{}' baseColor=({}, {}, {}, {}) metallic={} roughness={} "
                                  "emissive=({}, {}, {}) alphaMode='{}' alphaCutoff={} doubleSided={}",
                                  owner, strView(material.name), pbr.base_color_factor[0], pbr.base_color_factor[1],
                                  pbr.base_color_factor[2], pbr.base_color_factor[3], pbr.metallic_factor,
                                  pbr.roughness_factor, material.emissive_factor[0], material.emissive_factor[1],
                                  material.emissive_factor[2], strView(material.alpha_mode), material.alpha_cutoff,
                                  material.double_sided),
                      "AssetLoader");

            GpuMaterial gpu{};
            gpu.baseColorFactor = {static_cast<float>(pbr.base_color_factor[0]),
                                   static_cast<float>(pbr.base_color_factor[1]),
                                   static_cast<float>(pbr.base_color_factor[2]),
                                   static_cast<float>(pbr.base_color_factor[3])};
            gpu.metallicFactor = static_cast<float>(pbr.metallic_factor);
            gpu.roughnessFactor = static_cast<float>(pbr.roughness_factor);
            gpu.emissiveFactor = {static_cast<float>(material.emissive_factor[0]),
                                  static_cast<float>(material.emissive_factor[1]),
                                  static_cast<float>(material.emissive_factor[2])};
            gpu.alphaCutoff = static_cast<float>(material.alpha_cutoff);
            gpu.normalScale = static_cast<float>(material.normal_texture.scale);
            gpu.occlusionStrength = static_cast<float>(material.occlusion_texture.strength);
            gpu.flags = alphaModeFlags(strView(material.alpha_mode), material.double_sided);

            gpu.baseColorTex = resolveTextureImage(ctx, pbr.base_color_texture.index, TextureColorSpace::Srgb);
            gpu.baseColorSamp = resolveTextureSampler(ctx, pbr.base_color_texture.index);
            gpu.baseColorUv = static_cast<uint8_t>(std::max(pbr.base_color_texture.tex_coord, 0));

            gpu.metalRoughTex =
                resolveTextureImage(ctx, pbr.metallic_roughness_texture.index, TextureColorSpace::Linear);
            gpu.metalRoughSamp = resolveTextureSampler(ctx, pbr.metallic_roughness_texture.index);
            gpu.metalRoughUv = static_cast<uint8_t>(std::max(pbr.metallic_roughness_texture.tex_coord, 0));

            gpu.normalTex = resolveTextureImage(ctx, material.normal_texture.index, TextureColorSpace::Linear);
            gpu.normalSamp = resolveTextureSampler(ctx, material.normal_texture.index);
            gpu.normalUv = static_cast<uint8_t>(std::max(material.normal_texture.tex_coord, 0));

            gpu.occlusionTex = resolveTextureImage(ctx, material.occlusion_texture.index, TextureColorSpace::Linear);
            gpu.occlusionSamp = resolveTextureSampler(ctx, material.occlusion_texture.index);
            gpu.occlusionUv = static_cast<uint8_t>(std::max(material.occlusion_texture.tex_coord, 0));

            gpu.emissiveTex = resolveTextureImage(ctx, material.emissive_texture.index, TextureColorSpace::Srgb);
            gpu.emissiveSamp = resolveTextureSampler(ctx, material.emissive_texture.index);
            gpu.emissiveUv = static_cast<uint8_t>(std::max(material.emissive_texture.tex_coord, 0));

            parseGltfTextureInfo(ctx, pbr.base_color_texture, owner + ".baseColorTexture");
            parseGltfTextureInfo(ctx, pbr.metallic_roughness_texture, owner + ".metallicRoughnessTexture");
            logDebug(std::format("glTF {}.normalTexture index={} texCoord={} scale={}", owner,
                                  material.normal_texture.index, material.normal_texture.tex_coord,
                                  material.normal_texture.scale),
                      "AssetLoader");
            parseGltfExtras(ctx.geometry, AuxOwnerKind::Material, i, material.normal_texture.ext,
                            owner + ".normalTexture");
            logDebug(std::format("glTF {}.occlusionTexture index={} texCoord={} strength={}", owner,
                                  material.occlusion_texture.index, material.occlusion_texture.tex_coord,
                                  material.occlusion_texture.strength),
                      "AssetLoader");
            parseGltfExtras(ctx.geometry, AuxOwnerKind::Material, i, material.occlusion_texture.ext,
                            owner + ".occlusionTexture");
            parseGltfTextureInfo(ctx, material.emissive_texture, owner + ".emissiveTexture");
            parseGltfExtras(ctx.geometry, AuxOwnerKind::Material, i, material.ext, owner);
            parseGltfExtras(ctx.geometry, AuxOwnerKind::Material, i, pbr.ext, owner + ".pbr");

            MaterialPbrExtension pbrExt{};
            parseGltfMaterialPbrExtensions(ctx, material, gpu, pbrExt, owner);
            ctx.materialIds[i] = ctx.materials.add(gpu, pbrExt, strView(material.name));
        }
    }

    static uint8_t lightTypeFromName(std::string_view type)
    {
        if (type == "directional") {
            return 0;
        }
        if (type == "spot") {
            return 2;
        }
        return 1;
    }

    static glm::mat4 gltfNodeLocalMatrix(const tg3_node& node)
    {
        if (node.has_matrix != 0) {
            glm::mat4 matrix{1.0f};
            for (int column = 0; column < 4; ++column) {
                for (int row = 0; row < 4; ++row) {
                    matrix[column][row] = static_cast<float>(node.matrix[column * 4 + row]);
                }
            }
            return matrix;
        }

        const glm::vec3 translation{static_cast<float>(node.translation[0]), static_cast<float>(node.translation[1]),
                                    static_cast<float>(node.translation[2])};
        const glm::quat rotation{static_cast<float>(node.rotation[3]), static_cast<float>(node.rotation[0]),
                                 static_cast<float>(node.rotation[1]), static_cast<float>(node.rotation[2])};
        const glm::vec3 scale{static_cast<float>(node.scale[0]), static_cast<float>(node.scale[1]),
                              static_cast<float>(node.scale[2])};
        return glm::translate(glm::mat4{1.0f}, translation) * glm::mat4_cast(rotation) *
               glm::scale(glm::mat4{1.0f}, scale);
    }

    static std::vector<int32_t> buildNodeParents(const tg3_model& model)
    {
        std::vector<int32_t> parent(model.nodes_count, -1);
        for (uint32_t ni = 0; ni < model.nodes_count; ++ni) {
            const tg3_node& node = model.nodes[ni];
            for (uint32_t ci = 0; ci < node.children_count; ++ci) {
                const int32_t child = node.children[ci];
                if (child >= 0 && static_cast<uint32_t>(child) < model.nodes_count) {
                    parent[static_cast<uint32_t>(child)] = static_cast<int32_t>(ni);
                }
            }
        }
        return parent;
    }

    static glm::mat4 gltfNodeWorldMatrix(const tg3_model& model, uint32_t nodeIndex, const std::vector<int32_t>& parent)
    {
        std::vector<uint32_t> chain;
        int32_t current = static_cast<int32_t>(nodeIndex);
        while (current >= 0) {
            chain.push_back(static_cast<uint32_t>(current));
            current = parent[static_cast<uint32_t>(current)];
            if (chain.size() > model.nodes_count) {
                break;
            }
        }
        glm::mat4 world{1.0f};
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            world *= gltfNodeLocalMatrix(model.nodes[*it]);
        }
        return world;
    }

    static void parseGltfLights(GltfLoadCtx& ctx, const tg3_model& model)
    {
        logDebug(std::format("glTF lights: {}", model.lights_count), "AssetLoader");
        const uint32_t defBase = static_cast<uint32_t>(ctx.lights.defs.size());
        for (uint32_t i = 0; i < model.lights_count; ++i) {
            const tg3_light& light = model.lights[i];
            logDebug(std::format("glTF light[{}] name='{}' type='{}' color=({}, {}, {}) intensity={} range={} "
                                  "spotInner={} spotOuter={}",
                                  i, strView(light.name), strView(light.type), light.color[0], light.color[1],
                                  light.color[2], light.intensity, light.range, light.spot.inner_cone_angle,
                                  light.spot.outer_cone_angle),
                      "AssetLoader");
            LightDef def{};
            def.type = lightTypeFromName(strView(light.type));
            def.color = {static_cast<float>(light.color[0]), static_cast<float>(light.color[1]),
                         static_cast<float>(light.color[2])};
            def.intensity = static_cast<float>(light.intensity);
            def.range = static_cast<float>(light.range);
            def.innerCone = static_cast<float>(light.spot.inner_cone_angle);
            def.outerCone = static_cast<float>(light.spot.outer_cone_angle);
            ctx.lights.addDef(def);
            parseGltfExtras(ctx.geometry, AuxOwnerKind::Light, defBase + i, light.spot.ext,
                            std::format("light[{}].spot", i));
            parseGltfExtras(ctx.geometry, AuxOwnerKind::Light, defBase + i, light.ext, std::format("light[{}]", i));
        }

        const std::vector<int32_t> parent = buildNodeParents(model);
        for (uint32_t ni = 0; ni < model.nodes_count; ++ni) {
            const int32_t lightIndex = model.nodes[ni].light;
            if (lightIndex < 0) {
                continue;
            }
            logDebug(std::format("glTF node[{}] name='{}' light={}", ni, strView(model.nodes[ni].name), lightIndex),
                      "AssetLoader");
            if (static_cast<uint32_t>(lightIndex) >= model.lights_count) {
                continue;
            }
            const glm::mat4 world = gltfNodeWorldMatrix(model, ni, parent);
            LightInstance instance{};
            instance.defIndex = defBase + static_cast<uint32_t>(lightIndex);
            instance.position = glm::vec3(world[3]);
            const glm::vec3 dir = glm::mat3(world) * glm::vec3{0.0f, 0.0f, -1.0f};
            instance.direction = glm::length(dir) > 0.0f ? glm::normalize(dir) : glm::vec3{0.0f, 0.0f, -1.0f};
            ctx.lights.addInstance(instance);
        }
    }

    static void parseGltfImages(GltfLoadCtx& ctx, const tg3_model& model, const std::filesystem::path& modelDir)
    {
        logDebug(std::format("glTF images: {}", model.images_count), "AssetLoader");
        ctx.images.resize(model.images_count);

        for (uint32_t i = 0; i < model.images_count; ++i) {
            const tg3_image& image = model.images[i];
            const std::string_view uri = strView(image.uri);
            const std::string_view mime = strView(image.mime_type);
            uint64_t embeddedBytes = 0;
            const char* source = "none";
            GltfImageSrc src{};
            src.cacheKey = std::format("gltf-image-{}-{}", modelDir.string(), i);
            src.mime = std::string(mime);

            if (image.buffer_view >= 0) {
                const tg3_span_u8 bytes = readBufferViewBytes(model, image.buffer_view);
                embeddedBytes = bytes.count;
                source = "bufferView";
                if (bytes.data != nullptr && bytes.count > 0) {
                    src.encoded.assign(bytes.data, bytes.data + bytes.count);
                }
            } else if (!uri.empty() && uri.starts_with("data:")) {
                source = "dataUri";
                src.encoded = decodeDataUri(uri);
                embeddedBytes = src.encoded.size();
            } else if (!uri.empty()) {
                source = "uri";
                src.path = (modelDir / uri).string();
                src.cacheKey = src.path;
            }

            if (image.image.data != nullptr && image.image.count > 0 && image.width > 0 && image.height > 0 &&
                image.bits == 8) {
                source = "decoded";
                embeddedBytes = image.image.count;
                src.width = image.width;
                src.height = image.height;
                const uint32_t pixelCount = static_cast<uint32_t>(image.width) * static_cast<uint32_t>(image.height);
                src.decodedRgba.resize(static_cast<size_t>(pixelCount) * 4u, 255);
                const int channels = image.component > 0 ? image.component : 4;
                for (uint32_t p = 0; p < pixelCount; ++p) {
                    const size_t srcOff = static_cast<size_t>(p) * static_cast<size_t>(channels);
                    if (srcOff + static_cast<size_t>(channels) > image.image.count) {
                        break;
                    }
                    const uint8_t* px = image.image.data + srcOff;
                    uint8_t* dst = &src.decodedRgba[static_cast<size_t>(p) * 4u];
                    // 1 = L, 2 = LA, 3 = RGB, 4 = RGBA
                    const bool isGrey = channels < 3;
                    dst[0] = px[0];
                    dst[1] = isGrey ? px[0] : px[1];
                    dst[2] = isGrey ? px[0] : px[2];
                    dst[3] = channels == 2 ? px[1] : (channels > 3 ? px[3] : 255);
                }
            }

            logDebug(std::format("glTF image[{}] name='{}' source={} uri='{}' mime='{}' {}x{} bufferView={} bytes={}",
                                  i, strView(image.name), source, uri, mime, image.width, image.height,
                                  image.buffer_view, embeddedBytes),
                      "AssetLoader");
            parseGltfExtras(ctx.geometry, AuxOwnerKind::Image, i, image.ext, std::format("image[{}]", i));
            ctx.images[i] = std::move(src);
        }
    }

    // Dry run of the material texture lookups (same code paths as parseGltfMaterials) that only records
    // which images are sampled and in which color space.
    static void collectGltfImageUses(GltfLoadCtx& ctx, const tg3_model& model)
    {
        ZoneScopedN("AssetLoader::collectGltfImageUses");
        ctx.collectImageUses = true;
        for (uint32_t i = 0; i < model.materials_count; ++i) {
            const tg3_material& material = model.materials[i];
            const tg3_pbr_metallic_roughness& pbr = material.pbr_metallic_roughness;
            (void)resolveTextureImage(ctx, pbr.base_color_texture.index, TextureColorSpace::Srgb);
            (void)resolveTextureImage(ctx, pbr.metallic_roughness_texture.index, TextureColorSpace::Linear);
            (void)resolveTextureImage(ctx, material.normal_texture.index, TextureColorSpace::Linear);
            (void)resolveTextureImage(ctx, material.occlusion_texture.index, TextureColorSpace::Linear);
            (void)resolveTextureImage(ctx, material.emissive_texture.index, TextureColorSpace::Srgb);
            GpuMaterial gpu{};
            MaterialPbrExtension ext{};
            parseGltfMaterialPbrExtensions(ctx, material, gpu, ext, std::format("material[{}]", i));
        }
        ctx.collectImageUses = false;
    }

    // Pixels decoded at once; bounds the load-time RAM peak (a larger single image forms its own group).
    constexpr size_t kImageDecodeBudgetBytes = size_t{256} << 20;

    // Decodes the used images in groups of at most kImageDecodeBudgetBytes on a worker pool, uploads each
    // group on the main thread in image order, then frees its pixels before the next group. Images left
    // out (KTX, already cached, failed decodes) go through loadGltfImage's own path when materials ask.
    static void uploadGltfImages(GltfLoadCtx& ctx)
    {
        ZoneScopedN("AssetLoader::uploadGltfImages");
        struct Pending
        {
            GltfImageSrc* image;
            size_t bytes;
        };
        std::vector<Pending> pending;
        for (GltfImageSrc& image : ctx.images) {
            const bool used = image.usedSrgb || image.usedLinear;
            const bool decodable =
                !image.encoded.empty() || (!image.path.empty() && !TextureManager::isKtxPath(image.path));
            const bool cached = (!image.usedSrgb || ctx.textures.isCached(image.cacheKey, TextureColorSpace::Srgb)) &&
                (!image.usedLinear || ctx.textures.isCached(image.cacheKey, TextureColorSpace::Linear));
            if (used && decodable && image.decodedRgba.empty() && !cached) {
                const size_t bytes = image.encoded.empty() ? TextureManager::decodedRgba8FileBytes(image.path)
                                                           : TextureManager::decodedRgba8Bytes(image.encoded);
                pending.push_back({.image = &image, .bytes = bytes});
            }
        }

        size_t groups = 0;
        size_t threads = 0;
        size_t peakBytes = 0;
        for (size_t first = 0; first < pending.size();) {
            size_t last = first;
            size_t groupBytes = 0;
            while (last < pending.size() &&
                   (last == first || groupBytes + pending[last].bytes <= kImageDecodeBudgetBytes)) {
                groupBytes += pending[last].bytes;
                ++last;
            }
            const std::span<Pending> group(pending.data() + first, last - first);

            const auto decode = [&](size_t k) {
                GltfImageSrc& image = *group[k].image;
                image.predecoded = image.encoded.empty() ? TextureManager::decodeRgba8File(image.path)
                                                         : TextureManager::decodeRgba8(image.encoded);
            };
            threads = std::max(threads, parallelFor(group.size(), decode));
            for (const Pending& entry : group) {
                GltfImageSrc& image = *entry.image;
                if (image.usedSrgb) {
                    (void)loadGltfImage(ctx, image, TextureColorSpace::Srgb);
                }
                if (image.usedLinear) {
                    (void)loadGltfImage(ctx, image, TextureColorSpace::Linear);
                }
                // host image copy has consumed the pixels synchronously
                image.predecoded = {};
            }
            peakBytes = std::max(peakBytes, groupBytes);
            ++groups;
            first = last;
        }
        if (!pending.empty()) {
            logInfo(std::format("glTF uploaded {} images in {} groups (peak {} MiB decoded) on {} threads",
                                 pending.size(), groups, peakBytes >> 20, threads),
                     "AssetLoader");
        }
    }

    // Keeps a texture upload batch open for a scope; flushes on exit, including when parsing throws.
    struct TextureUploadBatch
    {
        TextureManager& textures;

        explicit TextureUploadBatch(TextureManager& texturesIn) : textures(texturesIn) { textures.beginUploadBatch(); }
        TextureUploadBatch(const TextureUploadBatch&) = delete;
        TextureUploadBatch& operator=(const TextureUploadBatch&) = delete;
        ~TextureUploadBatch()
        {
            try {
                textures.flushUploadBatch();
            } catch (const std::exception& error) {
                logError(std::format("Texture upload batch failed: {}", error.what()), "AssetLoader");
            }
        }
    };

    static uint32_t accessorCompCount(const tg3_model& model, int32_t accessorIdx)
    {
        if (accessorIdx < 0 || static_cast<uint32_t>(accessorIdx) >= model.accessors_count) {
            return 0;
        }
        const int32_t comps = tg3_num_components(model.accessors[accessorIdx].type);
        return comps > 0 ? static_cast<uint32_t>(comps) : 0;
    }

    static void fillVec3Range(std::vector<glm::vec3>& dst, uint32_t first, uint32_t count, const std::vector<float>& src,
                              uint32_t comps)
    {
        if (src.empty() || comps == 0) {
            return;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t base = i * comps;
            if (base >= src.size()) {
                break;
            }
            dst[first + i] = {src[base], comps > 1 && base + 1 < src.size() ? src[base + 1] : 0.0f,
                              comps > 2 && base + 2 < src.size() ? src[base + 2] : 0.0f};
        }
    }

    static void fillVec4Range(std::vector<glm::vec4>& dst, uint32_t first, uint32_t count, const std::vector<float>& src,
                              uint32_t comps, const glm::vec4& fallback)
    {
        if (src.empty() || comps == 0) {
            return;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t base = i * comps;
            if (base >= src.size()) {
                break;
            }
            dst[first + i] = {src[base], comps > 1 && base + 1 < src.size() ? src[base + 1] : fallback.y,
                              comps > 2 && base + 2 < src.size() ? src[base + 2] : fallback.z,
                              comps > 3 && base + 3 < src.size() ? src[base + 3] : fallback.w};
        }
    }

    void fillUvRange(std::vector<glm::vec2>& dst, uint32_t first, uint32_t count, const std::vector<float>& src,
                            uint32_t comps, bool flipV)
    {
        if (src.empty() || comps < 2) {
            return;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t base = i * comps;
            if (base + 1 >= src.size()) {
                break;
            }
            dst[first + i] = {src[base], flipV ? 1.0f - src[base + 1] : src[base + 1]};
        }
    }

    // drops zero-area triangles (equal positions) and repeats; opposite windings stay for double-sided use
    static void filterRedundantTriangles(std::vector<uint32_t>& triIndices, const float* positions,
                                         size_t vertexCount, size_t positionStride)
    {
        // in place is safe: the write cursor never passes the read cursor
        const size_t kept = meshopt_filterIndexBuffer(triIndices.data(), triIndices.data(), triIndices.size(),
                                                      positions, vertexCount, sizeof(glm::vec3), positionStride);
        triIndices.resize(kept);
    }

    static std::vector<uint32_t> toTriangleIndices(int32_t mode, const std::vector<uint32_t>& src)
    {
        if (src.size() < 3) {
            return {};
        }
        const int32_t resolved = mode < 0 ? TG3_MODE_TRIANGLES : mode;
        if (resolved == TG3_MODE_TRIANGLES) {
            return src;
        }

        std::vector<uint32_t> out;
        auto emit = [&](uint32_t a, uint32_t b, uint32_t c) {
            if (a == b || b == c || c == a) {
                return;
            }
            out.push_back(a);
            out.push_back(b);
            out.push_back(c);
        };

        if (resolved == TG3_MODE_TRIANGLE_STRIP) {
            for (size_t i = 0; i + 2 < src.size(); ++i) {
                if ((i % 2) == 0) {
                    emit(src[i], src[i + 1], src[i + 2]);
                } else {
                    emit(src[i + 1], src[i], src[i + 2]);
                }
            }
            return out;
        }
        if (resolved == TG3_MODE_TRIANGLE_FAN) {
            for (size_t i = 1; i + 1 < src.size(); ++i) {
                emit(src[0], src[i], src[i + 1]);
            }
            return out;
        }
        return {};
    }

    // mesh-local deltas: nodes place the mesh at draw time, so nothing is transformed here
    static void storeMorphTargets(GeometryStore& geometry, const tg3_model& model, const tg3_primitive& prim,
                                  uint32_t vertexCount)
    {
        if (prim.targets_count == 0) {
            return;
        }
        logDebug(std::format("glTF morph targets: {}", prim.targets_count), "AssetLoader");
        for (uint32_t t = 0; t < prim.targets_count; ++t) {
            if (prim.targets == nullptr || prim.target_attribute_counts == nullptr) {
                break;
            }
            const tg3_str_int_pair* attrs = prim.targets[t];
            const uint32_t attrCount = prim.target_attribute_counts[t];
            if (attrs == nullptr) {
                continue;
            }
            MorphTarget target{};
            for (uint32_t a = 0; a < attrCount; ++a) {
                const std::string_view key = strView(attrs[a].key);
                const std::vector<float> delta = readAccessorFloats(model, attrs[a].value);
                const uint32_t comps = accessorCompCount(model, attrs[a].value);
                logDebug(std::format("glTF morph[{}] attr='{}' accessor={} floats={}", t, key, attrs[a].value,
                                      delta.size()),
                          "AssetLoader");
                if (key == "POSITION") {
                    target.posOffset = static_cast<uint32_t>(geometry.morphPos.size());
                    geometry.morphPos.resize(target.posOffset + vertexCount, glm::vec3{0.0f});
                    fillVec3Range(geometry.morphPos, target.posOffset, vertexCount, delta, comps);
                } else if (key == "NORMAL") {
                    target.nrmOffset = static_cast<uint32_t>(geometry.morphNrm.size());
                    geometry.morphNrm.resize(target.nrmOffset + vertexCount, glm::vec3{0.0f});
                    fillVec3Range(geometry.morphNrm, target.nrmOffset, vertexCount, delta, comps);
                } else if (key == "TANGENT") {
                    target.tanOffset = static_cast<uint32_t>(geometry.morphTan.size());
                    geometry.morphTan.resize(target.tanOffset + vertexCount, glm::vec4{0.0f});
                    fillVec4Range(geometry.morphTan, target.tanOffset, vertexCount, delta, comps, glm::vec4{0.0f});
                }
            }
            geometry.morphTargets.push_back(target);
        }
    }

    static void storeVertexAttribute(GeometryStore& geometry, uint32_t firstVertex, uint32_t vertexCount,
                                     const tg3_model& model, std::string_view name, int32_t accessorIdx)
    {
        if (name == "NORMAL") {
            const std::vector<float> normals = readAccessorFloats(model, accessorIdx);
            logDebug(std::format("glTF attr '{}' accessor={} floats={}", name, accessorIdx, normals.size()),
                      "AssetLoader");
            fillVec3Range(geometry.normals, firstVertex, vertexCount, normals, accessorCompCount(model, accessorIdx));
            geometry.presentStreams |= GpuModelAttribute::Normals;
            return;
        }
        if (name == "TANGENT") {
            const std::vector<float> tangents = readAccessorFloats(model, accessorIdx);
            logDebug(std::format("glTF attr '{}' accessor={} floats={}", name, accessorIdx, tangents.size()),
                      "AssetLoader");
            fillVec4Range(geometry.tangents, firstVertex, vertexCount, tangents, accessorCompCount(model, accessorIdx),
                          glm::vec4{0.0f, 0.0f, 0.0f, 1.0f});
            geometry.presentStreams |= GpuModelAttribute::Tangents;
            return;
        }
        if (name == "COLOR_0") {
            const std::vector<float> colors = readAccessorFloats(model, accessorIdx);
            logDebug(std::format("glTF attr '{}' accessor={} floats={}", name, accessorIdx, colors.size()),
                      "AssetLoader");
            fillVec4Range(geometry.colors, firstVertex, vertexCount, colors, accessorCompCount(model, accessorIdx),
                          glm::vec4{1.0f});
            return;
        }
        if (name == "TEXCOORD_1") {
            const std::vector<float> uvs = readAccessorFloats(model, accessorIdx);
            logDebug(std::format("glTF attr '{}' accessor={} floats={}", name, accessorIdx, uvs.size()),
                      "AssetLoader");
            fillUvRange(geometry.uv1, firstVertex, vertexCount, uvs, accessorCompCount(model, accessorIdx), true);
            geometry.presentStreams |= GpuModelAttribute::Uv1;
            return;
        }
        if (name.starts_with("TEXCOORD_") && name != "TEXCOORD_0") {
            logDebug(std::format("glTF attr '{}' accessor={} skipped (uv2+)", name, accessorIdx), "AssetLoader");
            return;
        }
        if (name.starts_with("COLOR_")) {
            logDebug(std::format("glTF attr '{}' accessor={} skipped (COLOR_n>0)", name, accessorIdx), "AssetLoader");
            return;
        }
        if (name == "JOINTS_0") {
            const std::vector<uint32_t> joints = readAccessorU32(model, accessorIdx);
            logDebug(std::format("glTF attr '{}' accessor={} u32={}", name, accessorIdx, joints.size()),
                      "AssetLoader");
            const uint32_t comps = accessorCompCount(model, accessorIdx);
            for (uint32_t i = 0; i < vertexCount && comps > 0; ++i) {
                const uint32_t base = i * comps;
                std::array<uint16_t, 4> packed{0, 0, 0, 0};
                for (uint32_t c = 0; c < comps && c < 4 && base + c < joints.size(); ++c) {
                    packed[c] = static_cast<uint16_t>(std::min(joints[base + c], 65535u));
                }
                geometry.joints0[firstVertex + i] = packed;
            }
            geometry.presentStreams |= GpuModelAttribute::Joints;
            return;
        }
        if (name == "WEIGHTS_0") {
            const std::vector<float> weights = readAccessorFloats(model, accessorIdx);
            logDebug(std::format("glTF attr '{}' accessor={} floats={}", name, accessorIdx, weights.size()),
                      "AssetLoader");
            fillVec4Range(geometry.weights0, firstVertex, vertexCount, weights, accessorCompCount(model, accessorIdx),
                          glm::vec4{0.0f});
            geometry.presentStreams |= GpuModelAttribute::Weights;
            return;
        }
        if (name.starts_with("JOINTS_") || name.starts_with("WEIGHTS_")) {
            logDebug(std::format("glTF attr '{}' accessor={} skipped (set > 0)", name, accessorIdx), "AssetLoader");
            return;
        }
        logDebug(std::format("glTF attr '{}' accessor={} skipped", name, accessorIdx), "AssetLoader");
    }

    struct GltfMeshInstance
    {
        uint32_t mesh = 0;
        glm::mat4 world{1.0f};
    };

    // EXT_mesh_gpu_instancing: per-instance TRS in the node's local space; empty when the node has none
    static std::vector<glm::mat4> gpuInstanceTransforms(const tg3_model& model, const tg3_node& node)
    {
        const tg3_value* attributes = objectField(findExtensionValue(node.ext, "EXT_mesh_gpu_instancing"), "attributes");
        if (attributes == nullptr) {
            return {};
        }
        // normalized integer rotations are allowed; readAccessorFloats unpacks them
        const auto read = [&](std::string_view key, uint32_t comps) -> std::vector<float> {
            const int32_t accessor = valueAsInt(objectField(attributes, key), -1);
            if (accessor < 0 || accessorCompCount(model, accessor) != comps) {
                return {};
            }
            return readAccessorFloats(model, accessor);
        };
        const std::vector<float> translations = read("TRANSLATION", 3);
        const std::vector<float> rotations = read("ROTATION", 4);
        const std::vector<float> scales = read("SCALE", 3);
        const size_t count = std::max({translations.size() / 3, rotations.size() / 4, scales.size() / 3});

        // the spec requires equal counts; a short attribute leaves its part identity
        std::vector<glm::mat4> transforms(count, glm::mat4{1.0f});
        for (size_t i = 0; i < count; ++i) {
            glm::mat4& transform = transforms[i];
            if (i * 3 + 2 < translations.size()) {
                transform = glm::translate(transform, {translations[i * 3], translations[i * 3 + 1], translations[i * 3 + 2]});
            }
            if (i * 4 + 3 < rotations.size()) {
                const glm::quat rotation{rotations[i * 4 + 3], rotations[i * 4], rotations[i * 4 + 1], rotations[i * 4 + 2]};
                transform *= glm::mat4_cast(glm::normalize(rotation));
            }
            if (i * 3 + 2 < scales.size()) {
                transform = glm::scale(transform, {scales[i * 3], scales[i * 3 + 1], scales[i * 3 + 2]});
            }
        }
        logDebug(std::format("glTF EXT_mesh_gpu_instancing: mesh {} x{} (T={} R={} S={})", node.mesh, count,
                              translations.size() / 3, rotations.size() / 4, scales.size() / 3),
                  "AssetLoader");
        return transforms;
    }

    // walk the active scene; meshes are placed per referencing node, unreferenced meshes are not drawn
    static std::vector<GltfMeshInstance> collectMeshInstances(const tg3_model& model)
    {
        std::vector<GltfMeshInstance> instances;
        if (model.nodes_count == 0) {
            for (uint32_t mi = 0; mi < model.meshes_count; ++mi) {
                instances.push_back(GltfMeshInstance{.mesh = mi});
            }
            return instances;
        }

        std::vector<int32_t> roots;
        const uint32_t sceneIndex = model.default_scene >= 0 ? static_cast<uint32_t>(model.default_scene) : 0u;
        if (sceneIndex < model.scenes_count) {
            const tg3_scene& scene = model.scenes[sceneIndex];
            roots.assign(scene.nodes, scene.nodes + scene.nodes_count);
        } else {
            const std::vector<int32_t> parent = buildNodeParents(model);
            for (uint32_t ni = 0; ni < model.nodes_count; ++ni) {
                if (parent[ni] < 0) {
                    roots.push_back(static_cast<int32_t>(ni));
                }
            }
        }

        // visited guards malformed graphs with cycles or shared children
        std::vector<uint8_t> visited(model.nodes_count, 0);
        std::vector<std::pair<int32_t, glm::mat4>> stack;
        for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
            stack.emplace_back(*it, glm::mat4{1.0f});
        }
        while (!stack.empty()) {
            const auto [nodeIndex, parentWorld] = stack.back();
            stack.pop_back();
            if (nodeIndex < 0 || static_cast<uint32_t>(nodeIndex) >= model.nodes_count || visited[nodeIndex] != 0) {
                continue;
            }
            visited[nodeIndex] = 1;

            const tg3_node& node = model.nodes[nodeIndex];
            const glm::mat4 world = parentWorld * gltfNodeLocalMatrix(node);
            if (node.mesh >= 0 && static_cast<uint32_t>(node.mesh) < model.meshes_count) {
                const auto mesh = static_cast<uint32_t>(node.mesh);
                const std::vector<glm::mat4> gpuInstances = gpuInstanceTransforms(model, node);
                // with the extension the node draws only its instances
                if (gpuInstances.empty()) {
                    instances.push_back(GltfMeshInstance{.mesh = mesh, .world = world});
                }
                for (const glm::mat4& local : gpuInstances) {
                    instances.push_back(GltfMeshInstance{.mesh = mesh, .world = world * local});
                }
            }
            for (uint32_t ci = node.children_count; ci > 0; --ci) {
                stack.emplace_back(node.children[ci - 1], world);
            }
        }
        return instances;
    }

    static uint32_t appendGltfMesh(GltfLoadCtx& ctx, const tg3_model& model, uint32_t mi)
    {
        GeometryStore& geometry = ctx.geometry;
        const tg3_mesh& mesh = model.meshes[mi];
        uint32_t storedPrimitives = 0;

        const uint32_t morphWeightFirst = static_cast<uint32_t>(geometry.morphWeights.size());
        for (uint32_t wi = 0; wi < mesh.weights_count; ++wi) {
            geometry.morphWeights.push_back(static_cast<float>(mesh.weights[wi]));
        }

        for (uint32_t pi = 0; pi < mesh.primitives_count; ++pi) {
            const tg3_primitive& prim = mesh.primitives[pi];
            const int32_t mode = prim.mode < 0 ? TG3_MODE_TRIANGLES : prim.mode;
            logDebug(std::format("glTF mesh[{}].prim[{}] mode={} material={} attrs={} morphTargets={} indicesAcc={}",
                                  mi, pi, primitiveModeName(mode), prim.material, prim.attributes_count,
                                  prim.targets_count, prim.indices),
                      "AssetLoader");

            if (mode != TG3_MODE_TRIANGLES && mode != TG3_MODE_TRIANGLE_STRIP && mode != TG3_MODE_TRIANGLE_FAN) {
                logInfo(std::format("glTF mesh[{}].prim[{}] skipped: non-triangle mode", mi, pi), "AssetLoader");
                continue;
            }

            int32_t posAcc = -1;
            int32_t tc0Acc = -1;
            std::vector<std::pair<std::string_view, int32_t>> extraAttrs;
            extraAttrs.reserve(prim.attributes_count);

            for (uint32_t ai = 0; ai < prim.attributes_count; ++ai) {
                const tg3_str_int_pair& attr = prim.attributes[ai];
                const std::string_view name = strView(attr.key);
                if (name == "POSITION") {
                    posAcc = attr.value;
                } else if (name == "TEXCOORD_0") {
                    tc0Acc = attr.value;
                } else {
                    extraAttrs.emplace_back(name, attr.value);
                }
            }

            if (posAcc < 0) {
                logInfo(std::format("glTF mesh[{}].prim[{}] skipped: no POSITION", mi, pi), "AssetLoader");
                continue;
            }

            const std::vector<float> positions = readAccessorFloats(model, posAcc);
            if (positions.empty()) {
                logInfo(std::format("glTF mesh[{}].prim[{}] skipped: empty POSITION accessor {}", mi, pi, posAcc),
                         "AssetLoader");
                continue;
            }
            const uint32_t posComps = accessorCompCount(model, posAcc);
            const auto vertexCount = static_cast<uint32_t>(positions.size() / posComps);

            // resolve triangles before appending vertices so skipped primitives leave no orphans
            std::vector<uint32_t> srcIndices;
            if (prim.indices >= 0) {
                srcIndices = readAccessorIndices(model, prim.indices);
                if (srcIndices.empty()) {
                    logInfo(std::format("glTF mesh[{}].prim[{}] skipped: unreadable indices accessor {}", mi, pi,
                                         prim.indices),
                             "AssetLoader");
                    continue;
                }
                if (std::ranges::any_of(srcIndices, [vertexCount](uint32_t index) { return index >= vertexCount; })) {
                    logInfo(std::format("glTF mesh[{}].prim[{}] skipped: index out of range (verts={})", mi, pi,
                                         vertexCount),
                             "AssetLoader");
                    continue;
                }
            } else {
                srcIndices.resize(vertexCount);
                for (uint32_t v = 0; v < vertexCount; ++v) {
                    srcIndices[v] = v;
                }
            }
            std::vector<uint32_t> triIndices = toTriangleIndices(mode, srcIndices);
            // a morph target can open a triangle that is degenerate in the base pose
            if (posComps == 3 && prim.targets_count == 0) {
                filterRedundantTriangles(triIndices, positions.data(), vertexCount, sizeof(glm::vec3));
            }
            if (triIndices.empty()) {
                logInfo(std::format("glTF mesh[{}].prim[{}] skipped: no triangles", mi, pi), "AssetLoader");
                continue;
            }

            const uint32_t firstVertex = static_cast<uint32_t>(geometry.positions.size());
            geometry.resizeVertices(firstVertex + vertexCount);
            fillVec3Range(geometry.positions, firstVertex, vertexCount, positions, posComps);

            const std::vector<float> texcoords = readAccessorFloats(model, tc0Acc);
            fillUvRange(geometry.uv0, firstVertex, vertexCount, texcoords, accessorCompCount(model, tc0Acc), true);

            for (const auto& [name, acc] : extraAttrs) {
                storeVertexAttribute(geometry, firstVertex, vertexCount, model, name, acc);
            }

            for (uint32_t v = 0; v < vertexCount; ++v) {
                geometry.packVertex(firstVertex + v);
            }

            const uint32_t firstIndex = static_cast<uint32_t>(geometry.indices.size());
            for (const uint32_t local : triIndices) {
                geometry.indices.push_back(firstVertex + local);
            }
            const uint32_t indexCount = static_cast<uint32_t>(triIndices.size());

            const uint32_t morphFirst = static_cast<uint32_t>(geometry.morphTargets.size());
            storeMorphTargets(geometry, model, prim, vertexCount);
            const uint32_t morphCount = static_cast<uint32_t>(geometry.morphTargets.size()) - morphFirst;

            uint32_t materialId = ctx.materials.defaultMaterialId();
            if (prim.material >= 0 && static_cast<uint32_t>(prim.material) < ctx.materialIds.size()) {
                materialId = ctx.materialIds[static_cast<uint32_t>(prim.material)];
            }

            PrimitiveDraw draw{};
            draw.materialId = materialId;
            draw.firstVertex = firstVertex;
            draw.vertexCount = vertexCount;
            draw.firstIndex = firstIndex;
            draw.indexCount = indexCount;
            draw.morphFirst = morphCount > 0 ? morphFirst : 0;
            draw.morphCount = morphCount;
            // a short weight list would let the shader read past the mesh's weights
            draw.morphWeightFirst = morphCount > 0 && mesh.weights_count >= morphCount ? morphWeightFirst : kNoneIndex;
            // draw.meshlets is filled by buildGltfMeshlets once every primitive is appended
            parseGltfExtras(geometry, AuxOwnerKind::Primitive, static_cast<uint32_t>(geometry.primitiveDraws.size()),
                            prim.ext, std::format("mesh[{}].prim[{}]", mi, pi));
            geometry.primitiveDraws.push_back(draw);
            ++storedPrimitives;

            logDebug(std::format("glTF mesh[{}].prim[{}] POSITION verts={} TEXCOORD_0 floats={} indices={} stored={}",
                                  mi, pi, vertexCount, texcoords.size(), indexCount, indexCount),
                      "AssetLoader");
        }
        return storedPrimitives;
    }

    static uint64_t accessorCount(const tg3_model& model, int32_t accessorIdx)
    {
        if (accessorIdx < 0 || static_cast<uint32_t>(accessorIdx) >= model.accessors_count) {
            return 0;
        }
        return model.accessors[accessorIdx].count;
    }

    // Upper bound from accessor counts, so the per-vertex arrays and indices allocate once per model.
    static void reserveGltfGeometry(GeometryStore& geometry, const tg3_model& model,
                                    const std::vector<uint32_t>& meshes)
    {
        uint64_t vertexCount = 0;
        uint64_t indexCount = 0;
        for (const uint32_t mi : meshes) {
            const tg3_mesh& mesh = model.meshes[mi];
            for (uint32_t pi = 0; pi < mesh.primitives_count; ++pi) {
                const tg3_primitive& prim = mesh.primitives[pi];
                int32_t posAcc = -1;
                for (uint32_t ai = 0; ai < prim.attributes_count; ++ai) {
                    if (strView(prim.attributes[ai].key) == "POSITION") {
                        posAcc = prim.attributes[ai].value;
                    }
                }
                const uint64_t verts = accessorCount(model, posAcc);
                const uint64_t srcIndices = prim.indices >= 0 ? accessorCount(model, prim.indices) : verts;
                vertexCount += verts;
                // strips and fans expand to at most 3 corners per source index
                const int32_t mode = prim.mode < 0 ? TG3_MODE_TRIANGLES : prim.mode;
                indexCount += mode == TG3_MODE_TRIANGLES ? srcIndices : srcIndices * 3;
            }
        }
        geometry.reserveGeometry(static_cast<size_t>(vertexCount), static_cast<size_t>(indexCount));
    }

    // Primitives are independent: build each on a worker, then append in primitive order so offsets
    // match a serial build exactly.
    static void buildGltfMeshlets(GeometryStore& geometry, uint32_t firstPrimitive)
    {
        ZoneScopedN("AssetLoader::buildGltfMeshlets");
        const size_t count = geometry.primitiveDraws.size() - firstPrimitive;
        const size_t firstMeshlet = geometry.meshlets.size();
        std::vector<MeshletBuild> builds(count);
        const size_t threads = parallelFor(count, [&](size_t i) {
            const PrimitiveDraw& draw = geometry.primitiveDraws[firstPrimitive + i];
            builds[i] = geometry.buildMeshlets(draw.firstIndex, draw.indexCount, draw.firstVertex, draw.vertexCount);
        });
        for (size_t i = 0; i < count; ++i) {
            geometry.primitiveDraws[firstPrimitive + i].meshlets = geometry.appendMeshlets(std::move(builds[i]));
        }
        logInfo(std::format("glTF built {} meshlets for {} primitives on {} threads",
                             geometry.meshlets.size() - firstMeshlet, count, threads),
                 "AssetLoader");
    }

    static uint32_t appendGltfGeometry(GltfLoadCtx& ctx, const tg3_model& model)
    {
        logDebug(std::format("glTF meshes: {}", model.meshes_count), "AssetLoader");
        GeometryStore& geometry = ctx.geometry;
        for (uint32_t mi = 0; mi < model.meshes_count; ++mi) {
            const tg3_mesh& mesh = model.meshes[mi];
            logDebug(std::format("glTF mesh[{}] name='{}' primitives={} morphWeights={}", mi, strView(mesh.name),
                                  mesh.primitives_count, mesh.weights_count),
                      "AssetLoader");
            parseGltfExtras(geometry, AuxOwnerKind::Mesh, mi, mesh.ext, std::format("mesh[{}]", mi));
        }

        const std::vector<GltfMeshInstance> instances = collectMeshInstances(model);
        logDebug(std::format("glTF mesh instances: {}", instances.size()), "AssetLoader");

        // each referenced mesh is stored once, in first-reference order
        std::vector<uint32_t> uniqueMeshes;
        std::vector<uint8_t> isReferenced(model.meshes_count, 0);
        for (const GltfMeshInstance& instance : instances) {
            if (std::exchange(isReferenced[instance.mesh], uint8_t{1}) == 0) {
                uniqueMeshes.push_back(instance.mesh);
            }
        }
        reserveGltfGeometry(geometry, model, uniqueMeshes);

        struct MeshPrimitives
        {
            uint32_t first = 0;
            uint32_t count = 0;
            glm::vec4 boundingSphere{0.0f}; // mesh space
        };
        std::vector<MeshPrimitives> meshPrimitives(model.meshes_count);
        const uint32_t firstPrimitive = static_cast<uint32_t>(geometry.primitiveDraws.size());
        for (const uint32_t mi : uniqueMeshes) {
            meshPrimitives[mi].first = static_cast<uint32_t>(geometry.primitiveDraws.size());
            meshPrimitives[mi].count = appendGltfMesh(ctx, model, mi);
        }
        const uint32_t storedPrimitives = static_cast<uint32_t>(geometry.primitiveDraws.size()) - firstPrimitive;
        buildGltfMeshlets(geometry, firstPrimitive);

        // a mesh's primitives append their vertices back to back
        for (const uint32_t mi : uniqueMeshes) {
            MeshPrimitives& range = meshPrimitives[mi];
            if (range.count == 0) {
                continue;
            }
            const PrimitiveDraw& first = geometry.primitiveDraws[range.first];
            const PrimitiveDraw& last = geometry.primitiveDraws[range.first + range.count - 1];
            range.boundingSphere =
                geometry.boundingSphere(first.firstVertex, last.firstVertex + last.vertexCount - first.firstVertex);
        }
        for (const GltfMeshInstance& instance : instances) {
            const MeshPrimitives& range = meshPrimitives[instance.mesh];
            if (range.count == 0) {
                continue;
            }
            geometry.nodeDraws.push_back(NodeDraw{
                .localToModel = instance.world,
                .boundingSphere = transformBoundingSphere(instance.world, range.boundingSphere),
                .firstPrimitive = range.first,
                .primitiveCount = range.count,
                .flags = glm::determinant(glm::mat3(instance.world)) < 0.0f ? NodeFlag::FlipWinding : 0u,
            });
        }

        logInfo(std::format("glTF geometry appended: verts={} indices={} meshes={} primitives={} nodes={}",
                             geometry.vertices.size(), geometry.indices.size(), uniqueMeshes.size(), storedPrimitives,
                             instances.size()),
                 "AssetLoader");
        return storedPrimitives;
    }

    // Textures a load picked up. Until commit, a failed load frees the ones no other model retains.
    struct ModelTextureRefs
    {
        TextureManager& textures;
        std::vector<uint32_t>& heapIndices;
        bool isCommitted = false;

        ModelTextureRefs(TextureManager& texturesIn, std::vector<uint32_t>& heapIndicesIn) :
            textures(texturesIn), heapIndices(heapIndicesIn)
        {
        }
        ModelTextureRefs(const ModelTextureRefs&) = delete;
        ModelTextureRefs& operator=(const ModelTextureRefs&) = delete;
        ~ModelTextureRefs()
        {
            if (!isCommitted) {
                textures.releaseUnreferenced(heapIndices);
            }
        }

        // retained on behalf of the new model; returns the deduplicated list
        [[nodiscard]] std::vector<uint32_t> commit()
        {
            std::ranges::sort(heapIndices);
            const auto [first, last] = std::ranges::unique(heapIndices);
            heapIndices.erase(first, last);
            textures.retain(heapIndices);
            isCommitted = true;
            return heapIndices;
        }
    };

    struct Tg3ParseGuard
    {
        tg3_model& model;
        tg3_error_stack& errors;

        ~Tg3ParseGuard()
        {
            tg3_model_free(&model);
            tg3_error_stack_free(&errors);
        }
    };

} // anonymous namespace

AssetsLoader::AssetsLoader(ObjectStorage& objectStorageIn, TextureManager& textureManagerIn,
                           GeometryStore& geometryStoreIn, MaterialStore& materialStoreIn, LightStore& lightStoreIn) :
    objectStorage(objectStorageIn), textureManager(textureManagerIn), geometryStore(geometryStoreIn),
    materialStore(materialStoreIn), lightStore(lightStoreIn)
{
    logInfo("AssetsLoader initialized", "AssetLoader");
}

std::optional<std::filesystem::path> AssetsLoader::resolveModelPath(const std::filesystem::path& modelPath)
{
    // Folder packs (models/<name>/*) or a direct file. Prefer .gltf, then .glb, then .obj.
    std::error_code errorCode;
    // one spelling per file, so the model cache key matches however the path was written
    std::filesystem::path path = std::filesystem::weakly_canonical(modelPath, errorCode);
    if (errorCode) {
        path = modelPath;
    }
    path.make_preferred();
    if (std::filesystem::is_directory(path, errorCode)) {
        std::filesystem::path gltf;
        std::filesystem::path glb;
        std::filesystem::path obj;
        for (const auto& entry : std::filesystem::directory_iterator(
                 path, std::filesystem::directory_options::skip_permission_denied, errorCode)) {
            if (errorCode || !entry.is_regular_file(errorCode)) {
                continue;
            }
            std::string extension = entry.path().extension().string();
            std::ranges::transform(extension, extension.begin(),
                                   [](unsigned char character) -> char { return static_cast<char>(std::tolower(character)); });
            if (extension == ".gltf" && gltf.empty()) {
                gltf = entry.path();
            } else if (extension == ".glb" && glb.empty()) {
                glb = entry.path();
            } else if (extension == ".obj" && obj.empty()) {
                obj = entry.path();
            }
        }
        if (!gltf.empty()) {
            path = std::move(gltf);
        } else if (!glb.empty()) {
            path = std::move(glb);
        } else if (!obj.empty()) {
            path = std::move(obj);
        } else {
            logError(std::format("Model folder has no .gltf/.glb/.obj: {}", path.string()), "AssetLoader");
            return std::nullopt;
        }
        path.make_preferred();
    }
    return path;
}

std::optional<EntityId> AssetsLoader::loadModel(std::string modelPath, glm::vec3 xyz)
{
    ZoneScopedN("AssetsLoader::loadModel");
    const std::optional<std::filesystem::path> path = resolveModelPath(modelPath);
    if (!path) {
        return std::nullopt;
    }

    const std::string pathString = path->string();
    const bool isGltf = pathString.ends_with(".gltf") || pathString.ends_with(".glb");
    const bool isObj = pathString.ends_with(".obj");

    materialStore.beginModel();
    if (isGltf) {
        return loadGltfModel(pathString, xyz);
    }

    if (isObj) {
        return loadObjModel(pathString, xyz);
    }

    logError(std::format("Unsupported model format: {}", pathString), "AssetLoader");
    return std::nullopt;
}

std::optional<EntityId> AssetsLoader::loadGltfModel(const std::string& modelPath, glm::vec3 xyz)
{
    ZoneScopedN("AssetsLoader::loadGltfModel");
    // glTF uses forward-slash URIs internally; normalise the base path
    // to avoid mixed separators when the library resolves external .bin
    // references (e.g. "models/AnimatedCube.bin" under "models\" on Windows).
    const std::string normalizedPath = std::filesystem::path(modelPath).generic_string();

    tg3_model model{};
    tg3_error_stack errors;
    tg3_error_stack_init(&errors);
    // frees on every exit, including exceptions thrown mid-load
    const Tg3ParseGuard parseGuard{.model = model, .errors = errors};

    tg3_parse_options opts;
    tg3_parse_options_init(&opts);
    opts.parse_float32 = 1;
    opts.store_original_json = 1;

    const tg3_error_code rc =
        tg3_parse_file(&model, &errors, normalizedPath.c_str(), static_cast<uint32_t>(normalizedPath.size()), &opts);

    if (rc != TG3_OK || model.meshes_count == 0) {
        const uint32_t errorCount = tg3_errors_count(&errors);
        if (errorCount > 0) {
            std::string details;
            for (uint32_t i = 0; i < errorCount; ++i) {
                const tg3_error_entry* entry = tg3_errors_get(&errors, i);
                details += std::format("  [{}/{}] {}", static_cast<int>(entry->severity), static_cast<int>(entry->code),
                                       entry->message);
                if (entry->json_path && entry->json_path[0] != '\0')
                    details += std::format(" (at {})", entry->json_path);
                details += '\n';
            }
            logError(std::format("Failed to parse glTF (rc={}):\n{}", static_cast<int>(rc), details), "AssetLoader");
        } else {
            logError(std::format("Failed to parse glTF: rc={}", static_cast<int>(rc)), "AssetLoader");
        }
        return std::nullopt;
    }

    logInfo(std::format("Loading glTF: {} meshes, {} nodes", model.meshes_count, model.nodes_count), "AssetLoader");

    GltfLoadCtx ctx{
        .geometry = geometryStore,
        .materials = materialStore,
        .lights = lightStore,
        .textures = textureManager,
        .defaultSamplerHeap =
            textureManager.getOrCreateSampler(-1, -1, TG3_TEXTURE_WRAP_REPEAT, TG3_TEXTURE_WRAP_REPEAT),
    };
    ModelTextureRefs textureRefs(textureManager, ctx.textureRefs);

    const uint32_t firstPrimitive = static_cast<uint32_t>(geometryStore.primitiveDraws.size());
    parseGltfRootExtensions(ctx, model);
    const std::vector<uint32_t> samplerHeaps = parseGltfSamplers(ctx, model);
    parseGltfImages(ctx, model, std::filesystem::path(modelPath).parent_path());
    parseGltfTextures(ctx, model, samplerHeaps);
    {
        const TextureUploadBatch uploadBatch(textureManager);
        collectGltfImageUses(ctx, model);
        uploadGltfImages(ctx);
        // every decodable image is cached now; this only picks up heap indices (and KTX / fallbacks)
        parseGltfMaterials(ctx, model);
    }
    parseGltfLights(ctx, model);
    appendGltfGeometry(ctx, model);
    const uint32_t primitiveCount = static_cast<uint32_t>(geometryStore.primitiveDraws.size()) - firstPrimitive;

    MeshletDraw unionDraw{};
    if (primitiveCount > 0) {
        const PrimitiveDraw& first = geometryStore.primitiveDraws[firstPrimitive];
        const PrimitiveDraw& last = geometryStore.primitiveDraws[firstPrimitive + primitiveCount - 1];
        unionDraw.firstMeshlet = first.meshlets.firstMeshlet;
        unionDraw.meshletCount = (last.meshlets.firstMeshlet + last.meshlets.meshletCount) - unionDraw.firstMeshlet;
    }

    const Transform transform{.position = xyz};
    const EntityId id = objectStorage.create(transform, unionDraw, firstPrimitive, primitiveCount, modelPath);
    geometryStore.textureRefs = textureRefs.commit();
    logInfo(std::format("Loaded model entity {} | primitives=[{}, {}) | meshlets: {} (first {})", id, firstPrimitive,
                         firstPrimitive + primitiveCount, unionDraw.meshletCount, unionDraw.firstMeshlet),
             "AssetLoader");

    logInfo(std::format("Model loaded (glTF): {} | vertices: {} | indices: {} | total meshlets: {}", modelPath,
                         geometryStore.vertices.size(), geometryStore.indices.size(), geometryStore.meshlets.size()),
             "AssetLoader");
    return id;
}

std::optional<EntityId> AssetsLoader::loadObjModel(const std::string& modelPath, glm::vec3 xyz)
{
    ZoneScopedN("AssetsLoader::loadObjModel");
    logInfo(std::format("Loading OBJ: {}", modelPath), "AssetLoader");
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string err;

    // tinyobj wraps standard C file I/O — native separators are correct.
    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &err, modelPath.c_str())) {
        logError(std::format("Failed to load OBJ: {}", err), "AssetLoader");
        return std::nullopt;
    }

    const uint32_t firstVertex = static_cast<uint32_t>(geometryStore.positions.size());
    const uint32_t firstIndex = static_cast<uint32_t>(geometryStore.indices.size());
    const uint32_t firstPrimitive = static_cast<uint32_t>(geometryStore.primitiveDraws.size());

    // one vertex per face corner; meshopt welds bitwise-identical corners in first-use order
    std::vector<GpuVertex> corners;
    for (const auto& [name, mesh] : shapes) {
        for (const auto& index : mesh.indices) {
            corners.push_back(GpuVertex{
                .pos = {attrib.vertices[3 * index.vertex_index + 0], attrib.vertices[3 * index.vertex_index + 1],
                        attrib.vertices[3 * index.vertex_index + 2]},
                .color = {1.0f, 1.0f, 1.0f},
                .texCoord = {attrib.texcoords[2 * index.texcoord_index + 0],
                             1.0f - attrib.texcoords[2 * index.texcoord_index + 1]},
            });
        }
    }

    std::vector<uint32_t> triIndices(corners.size());
    const size_t uniqueCount = meshopt_generateVertexRemap(triIndices.data(), nullptr, corners.size(), corners.data(),
                                                           corners.size(), sizeof(GpuVertex));
    std::vector<GpuVertex> unique(uniqueCount);
    meshopt_remapVertexBuffer(unique.data(), corners.data(), corners.size(), sizeof(GpuVertex), triIndices.data());
    // unindexed input: the remap table is the welded index buffer
    // pos is the first GpuVertex member
    filterRedundantTriangles(triIndices, reinterpret_cast<const float*>(unique.data()), unique.size(),
                             sizeof(GpuVertex));

    geometryStore.resizeVertices(firstVertex + static_cast<uint32_t>(uniqueCount));
    for (uint32_t v = 0; v < uniqueCount; ++v) {
        const uint32_t id = firstVertex + v;
        geometryStore.positions[id] = unique[v].pos;
        geometryStore.uv0[id] = unique[v].texCoord;
        geometryStore.colors[id] = glm::vec4{1.0f};
        geometryStore.packVertex(id);
    }
    for (const uint32_t local : triIndices) {
        geometryStore.indices.push_back(firstVertex + local);
    }
    const auto indexCount = static_cast<uint32_t>(triIndices.size());

    GpuMaterial gpu{};
    const auto objTexture = (std::filesystem::path(modelPath).parent_path() / TEXTURE_PATH.filename()).string();
    std::vector<uint32_t> heapIndices;
    ModelTextureRefs textureRefs(textureManager, heapIndices);
    gpu.baseColorTex = textureManager.loadTexture(objTexture);
    heapIndices.push_back(gpu.baseColorTex);
    gpu.baseColorSamp = textureManager.getOrCreateSampler(-1, -1, TG3_TEXTURE_WRAP_REPEAT, TG3_TEXTURE_WRAP_REPEAT);
    const uint32_t materialId = materialStore.add(gpu, {}, std::filesystem::path(modelPath).stem().string());

    PrimitiveDraw draw{};
    draw.materialId = materialId;
    draw.firstVertex = firstVertex;
    draw.vertexCount = static_cast<uint32_t>(geometryStore.positions.size()) - firstVertex;
    draw.firstIndex = firstIndex;
    draw.indexCount = indexCount;
    draw.meshlets = geometryStore.buildMeshletsForRange(firstIndex, indexCount, firstVertex, draw.vertexCount);
    geometryStore.primitiveDraws.push_back(draw);
    geometryStore.nodeDraws.push_back(NodeDraw{
        .localToModel = glm::mat4{1.0f},
        .boundingSphere = geometryStore.boundingSphere(firstVertex, draw.vertexCount),
        .firstPrimitive = firstPrimitive,
        .primitiveCount = 1,
        .flags = 0,
    });

    const Transform transform{.position = xyz};
    const EntityId id = objectStorage.create(transform, draw.meshlets, firstPrimitive, 1, modelPath);
    geometryStore.textureRefs = textureRefs.commit();
    logInfo(std::format("Loaded model entity {} | primitives=[{}, {}) | meshlets: {} (first {})", id, firstPrimitive,
                         firstPrimitive + 1, draw.meshlets.meshletCount, draw.meshlets.firstMeshlet),
             "AssetLoader");
    logInfo(std::format("Model loaded (OBJ): {} | vertices: {} | indices: {} | total meshlets: {}", modelPath,
                         geometryStore.vertices.size(), geometryStore.indices.size(), geometryStore.meshlets.size()),
             "AssetLoader");
    return id;
}
