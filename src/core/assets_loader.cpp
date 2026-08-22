#include "assets_loader.hpp"
#include "../Constants.h"
#include "../static_headers/logger.hpp"
#include "../util/vk_tracy.hpp"

#include <algorithm>
#include <glm/gtc/quaternion.hpp>

// ── glTF external-reference detection ───────────────────────

namespace
{

    struct GltfExternalRef
    {
        std::string_view element;
        uint32_t index;
        std::string_view uri;
    };



    // ── glTF accessor helpers ───────────────────────────────────

    // Read float data from a glTF accessor. Returns an empty vector
    // on any error (missing buffer, unsupported type, etc.).
    static std::vector<float> readAccessorFloats(const tg3_model& model, int32_t accessorIdx)
    {
        if (accessorIdx < 0 || static_cast<uint32_t>(accessorIdx) >= model.accessors_count)
            return {};

        const tg3_accessor& acc = model.accessors[accessorIdx];
        if (acc.buffer_view < 0 || static_cast<uint32_t>(acc.buffer_view) >= model.buffer_views_count)
            return {};

        const tg3_buffer_view& bv = model.buffer_views[acc.buffer_view];
        if (bv.buffer < 0 || static_cast<uint32_t>(bv.buffer) >= model.buffers_count)
            return {};

        const tg3_buffer& buf = model.buffers[bv.buffer];
        if (!buf.data.data)
            return {};

        const int32_t compSize = tg3_component_size(acc.component_type);
        const int32_t numComp = tg3_num_components(acc.type);
        if (compSize < 0 || numComp < 0)
            return {};

        const int32_t stride = tg3_accessor_byte_stride(&acc, &bv);
        if (stride < 0)
            return {};

        const auto offset = static_cast<size_t>(bv.byte_offset) + static_cast<size_t>(acc.byte_offset);
        const uint8_t* src = buf.data.data + offset;
        const auto elemCount = static_cast<size_t>(acc.count);

        std::vector<float> result;
        result.reserve(elemCount * static_cast<size_t>(numComp));

        // Fast path: tightly-packed float data
        if (acc.component_type == TG3_COMPONENT_TYPE_FLOAT && compSize == 4 && stride == compSize * numComp) {
            const auto* floats = reinterpret_cast<const float*>(src);
            result.assign(floats, floats + elemCount * numComp);
            return result;
        }

        // General path — per-element, per-component read with normalisation
        for (size_t elem = 0; elem < elemCount; ++elem) {
            const uint8_t* elemSrc = src + elem * static_cast<size_t>(stride);
            for (int32_t c = 0; c < numComp; ++c) {
                const uint8_t* compSrc = elemSrc + static_cast<size_t>(c) * compSize;
                float val = 0.0f;
                switch (acc.component_type) {
                case TG3_COMPONENT_TYPE_FLOAT:
                    val = *reinterpret_cast<const float*>(compSrc);
                    break;
                case TG3_COMPONENT_TYPE_UNSIGNED_SHORT:
                    val = static_cast<float>(*reinterpret_cast<const uint16_t*>(compSrc)) / 65535.0f;
                    break;
                case TG3_COMPONENT_TYPE_UNSIGNED_BYTE:
                    val = static_cast<float>(*reinterpret_cast<const uint8_t*>(compSrc)) / 255.0f;
                    break;
                case TG3_COMPONENT_TYPE_SHORT:
                    val = static_cast<float>(*reinterpret_cast<const int16_t*>(compSrc)) / 32767.0f;
                    break;
                case TG3_COMPONENT_TYPE_BYTE: {
                    const float c = static_cast<float>(*reinterpret_cast<const int8_t*>(compSrc));
                    val = std::max(c / 127.0f, -1.0f);
                    break;
                }
                default:
                    // unsupported component type — return what we have so far
                    return result;
                }
                result.push_back(val);
            }
        }

        return result;
    }

    // Read index data from a glTF accessor. Supports UINT32, UINT16, and UINT8.
    static std::vector<uint32_t> readAccessorIndices(const tg3_model& model, int32_t accessorIdx)
    {
        if (accessorIdx < 0 || static_cast<uint32_t>(accessorIdx) >= model.accessors_count)
            return {};

        const tg3_accessor& acc = model.accessors[accessorIdx];
        if (acc.buffer_view < 0 || static_cast<uint32_t>(acc.buffer_view) >= model.buffer_views_count)
            return {};

        const tg3_buffer_view& bv = model.buffer_views[acc.buffer_view];
        if (bv.buffer < 0 || static_cast<uint32_t>(bv.buffer) >= model.buffers_count)
            return {};

        const tg3_buffer& buf = model.buffers[bv.buffer];
        if (!buf.data.data)
            return {};

        const auto offset = static_cast<size_t>(bv.byte_offset) + static_cast<size_t>(acc.byte_offset);
        const uint8_t* src = buf.data.data + offset;

        std::vector<uint32_t> result;
        result.reserve(static_cast<size_t>(acc.count));

        for (uint32_t i = 0; i < acc.count; ++i) {
            switch (acc.component_type) {
            case TG3_COMPONENT_TYPE_UNSIGNED_INT:
                result.push_back(reinterpret_cast<const uint32_t*>(src)[i]);
                break;
            case TG3_COMPONENT_TYPE_UNSIGNED_SHORT:
                result.push_back(static_cast<uint32_t>(reinterpret_cast<const uint16_t*>(src)[i]));
                break;
            case TG3_COMPONENT_TYPE_UNSIGNED_BYTE:
                result.push_back(static_cast<uint32_t>(src[i]));
                break;
            default:
                return {}; // unsupported index type
            }
        }

        return result;
    }

    static std::vector<uint32_t> readAccessorU32(const tg3_model& model, int32_t accessorIdx)
    {
        if (accessorIdx < 0 || static_cast<uint32_t>(accessorIdx) >= model.accessors_count)
            return {};

        const tg3_accessor& acc = model.accessors[accessorIdx];
        if (acc.buffer_view < 0 || static_cast<uint32_t>(acc.buffer_view) >= model.buffer_views_count)
            return {};

        const tg3_buffer_view& bv = model.buffer_views[acc.buffer_view];
        if (bv.buffer < 0 || static_cast<uint32_t>(bv.buffer) >= model.buffers_count)
            return {};

        const tg3_buffer& buf = model.buffers[bv.buffer];
        if (!buf.data.data)
            return {};

        const int32_t compSize = tg3_component_size(acc.component_type);
        const int32_t numComp = tg3_num_components(acc.type);
        if (compSize < 0 || numComp < 0)
            return {};

        const int32_t stride = tg3_accessor_byte_stride(&acc, &bv);
        if (stride < 0)
            return {};

        const auto offset = static_cast<size_t>(bv.byte_offset) + static_cast<size_t>(acc.byte_offset);
        const uint8_t* src = buf.data.data + offset;
        const auto elemCount = static_cast<size_t>(acc.count);

        std::vector<uint32_t> result;
        result.reserve(elemCount * static_cast<size_t>(numComp));

        for (size_t elem = 0; elem < elemCount; ++elem) {
            const uint8_t* elemSrc = src + elem * static_cast<size_t>(stride);
            for (int32_t c = 0; c < numComp; ++c) {
                const uint8_t* compSrc = elemSrc + static_cast<size_t>(c) * compSize;
                switch (acc.component_type) {
                case TG3_COMPONENT_TYPE_UNSIGNED_INT:
                    result.push_back(*reinterpret_cast<const uint32_t*>(compSrc));
                    break;
                case TG3_COMPONENT_TYPE_UNSIGNED_SHORT:
                    result.push_back(*reinterpret_cast<const uint16_t*>(compSrc));
                    break;
                case TG3_COMPONENT_TYPE_UNSIGNED_BYTE:
                    result.push_back(*compSrc);
                    break;
                default:
                    return {};
                }
            }
        }

        return result;
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

        const uint64_t offset = bv.byte_offset;
        if (offset + bv.byte_length > buf.data.count)
            return {};

        return tg3_span_u8{.data = buf.data.data + offset, .count = bv.byte_length};
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

    struct GltfImageSrc
    {
        std::string cacheKey;
        std::string path;
        std::vector<uint8_t> encoded;
        std::vector<uint8_t> decodedRgba;
        int width = 0;
        int height = 0;
        std::string mime;
        uint32_t heapSrgb = kNoneIndex;
        uint32_t heapLinear = kNoneIndex;
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
        std::vector<GltfImageSrc> images;
        std::vector<GltfResolvedTexture> gltfTextures;
        std::vector<uint32_t> materialIds;
        uint32_t defaultSamplerHeap = 0;
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

    static void storeAux(GeometryStore& geometry, uint32_t kind, uint32_t index, const tg3_extras_ext& ext)
    {
        AuxBlob blob{.ownerKind = kind, .ownerIndex = index, .extrasJson = extrasJsonOf(ext)};
        for (uint32_t i = 0; i < ext.extensions_count; ++i) {
            std::string json;
            if (ext.extensions_json.data != nullptr && ext.extensions_json.len > 0) {
                json.assign(ext.extensions_json.data, ext.extensions_json.len);
            }
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
        if (heap != kNoneIndex) {
            return heap;
        }
        if (!image.decodedRgba.empty() && image.width > 0 && image.height > 0) {
            heap = ctx.textures.loadTextureFromPixels(image.cacheKey, image.decodedRgba,
                                                      static_cast<uint32_t>(image.width),
                                                      static_cast<uint32_t>(image.height), colorSpace);
        } else if (!image.encoded.empty()) {
            heap = ctx.textures.loadTextureFromMemory(image.cacheKey, image.encoded, image.mime, colorSpace);
        } else if (!image.path.empty()) {
            heap = ctx.textures.loadTexture(image.path, colorSpace);
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
        return loadGltfImage(ctx, ctx.images[static_cast<uint32_t>(imageIndex)], colorSpace);
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
            log_info(std::format("glTF extras on {}", owner), "AssetLoader");
        }
        for (uint32_t i = 0; i < ext.extensions_count; ++i) {
            const std::string_view name = strView(ext.extensions[i].name);
            log_info(std::format("glTF extension '{}' on {}", name, owner), "AssetLoader");
        }
        storeAux(geometry, kind, index, ext);
    }

    static void parseGltfRootExtensions(GltfLoadCtx& ctx, const tg3_model& model)
    {
        log_info(std::format("glTF extensionsUsed={} extensionsRequired={}", model.extensions_used_count,
                             model.extensions_required_count),
                 "AssetLoader");
        for (uint32_t i = 0; i < model.extensions_used_count; ++i) {
            const std::string_view name = strView(model.extensions_used[i]);
            log_info(std::format("glTF extensionsUsed[{}]='{}'", i, name), "AssetLoader");
            ctx.geometry.extensionsUsed.emplace_back(name);
        }
        for (uint32_t i = 0; i < model.extensions_required_count; ++i) {
            const std::string_view name = strView(model.extensions_required[i]);
            log_info(std::format("glTF extensionsRequired[{}]='{}'", i, name), "AssetLoader");
            ctx.geometry.extensionsRequired.emplace_back(name);
        }
        parseGltfExtras(ctx.geometry, AuxOwnerKind::Model, 0, model.ext, "model");
        parseGltfExtras(ctx.geometry, AuxOwnerKind::Asset, 0, model.asset.ext, "asset");
    }

    static std::vector<uint32_t> parseGltfSamplers(GltfLoadCtx& ctx, const tg3_model& model)
    {
        log_info(std::format("glTF samplers: {}", model.samplers_count), "AssetLoader");
        std::vector<uint32_t> heapIndices(model.samplers_count, ctx.defaultSamplerHeap);
        for (uint32_t i = 0; i < model.samplers_count; ++i) {
            const tg3_sampler& sampler = model.samplers[i];
            log_info(std::format("glTF sampler[{}] name='{}' minFilter={} magFilter={} wrapS={} wrapT={}", i,
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
        log_info(std::format("glTF textures: {}", model.textures_count), "AssetLoader");
        ctx.gltfTextures.resize(model.textures_count);
        for (uint32_t i = 0; i < model.textures_count; ++i) {
            const tg3_texture& texture = model.textures[i];
            log_info(std::format("glTF texture[{}] name='{}' source={} sampler={}", i, strView(texture.name),
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
        log_info(std::format("glTF {} index={} texCoord={}", slot, info.index, info.tex_coord), "AssetLoader");
        parseGltfExtras(ctx.geometry, AuxOwnerKind::Texture, info.index < 0 ? 0 : static_cast<uint32_t>(info.index),
                        info.ext, slot);
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
        log_info(std::format("glTF materials: {}", model.materials_count), "AssetLoader");
        ctx.materialIds.resize(model.materials_count);
        for (uint32_t i = 0; i < model.materials_count; ++i) {
            const tg3_material& material = model.materials[i];
            const tg3_pbr_metallic_roughness& pbr = material.pbr_metallic_roughness;
            const std::string owner = std::format("material[{}]", i);
            log_info(std::format("glTF {} name='{}' baseColor=({}, {}, {}, {}) metallic={} roughness={} "
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
            log_info(std::format("glTF {}.normalTexture index={} texCoord={} scale={}", owner,
                                 material.normal_texture.index, material.normal_texture.tex_coord,
                                 material.normal_texture.scale),
                     "AssetLoader");
            parseGltfExtras(ctx.geometry, AuxOwnerKind::Material, i, material.normal_texture.ext,
                            owner + ".normalTexture");
            log_info(std::format("glTF {}.occlusionTexture index={} texCoord={} strength={}", owner,
                                 material.occlusion_texture.index, material.occlusion_texture.tex_coord,
                                 material.occlusion_texture.strength),
                     "AssetLoader");
            parseGltfExtras(ctx.geometry, AuxOwnerKind::Material, i, material.occlusion_texture.ext,
                            owner + ".occlusionTexture");
            parseGltfTextureInfo(ctx, material.emissive_texture, owner + ".emissiveTexture");
            parseGltfExtras(ctx.geometry, AuxOwnerKind::Material, i, material.ext, owner);
            parseGltfExtras(ctx.geometry, AuxOwnerKind::Material, i, pbr.ext, owner + ".pbr");

            ctx.materialIds[i] = ctx.materials.add(gpu);
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
        log_info(std::format("glTF lights: {}", model.lights_count), "AssetLoader");
        const uint32_t defBase = static_cast<uint32_t>(ctx.lights.defs.size());
        for (uint32_t i = 0; i < model.lights_count; ++i) {
            const tg3_light& light = model.lights[i];
            log_info(std::format("glTF light[{}] name='{}' type='{}' color=({}, {}, {}) intensity={} range={} "
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

        for (uint32_t ni = 0; ni < model.nodes_count; ++ni) {
            const int32_t lightIndex = model.nodes[ni].light;
            if (lightIndex < 0) {
                continue;
            }
            log_info(std::format("glTF node[{}] name='{}' light={}", ni, strView(model.nodes[ni].name), lightIndex),
                     "AssetLoader");
            if (static_cast<uint32_t>(lightIndex) >= model.lights_count) {
                continue;
            }
            const glm::mat4 world = gltfNodeWorldMatrix(model, ni, parent);
            LightInstance instance{};
            instance.defIndex = defBase + static_cast<uint32_t>(lightIndex);
            instance.worldPos = glm::vec3(world[3]);
            const glm::vec3 dir = glm::mat3(world) * glm::vec3{0.0f, 0.0f, -1.0f};
            instance.worldDir = glm::length(dir) > 0.0f ? glm::normalize(dir) : glm::vec3{0.0f, 0.0f, -1.0f};
            ctx.lights.addInstance(instance);
        }
    }

    static void parseGltfImages(GltfLoadCtx& ctx, const tg3_model& model, const std::filesystem::path& modelDir)
    {
        log_info(std::format("glTF images: {}", model.images_count), "AssetLoader");
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
                    if (srcOff >= image.image.count) {
                        break;
                    }
                    src.decodedRgba[p * 4u + 0] = image.image.data[srcOff];
                    src.decodedRgba[p * 4u + 1] = channels > 1 ? image.image.data[srcOff + 1] : 0;
                    src.decodedRgba[p * 4u + 2] = channels > 2 ? image.image.data[srcOff + 2] : 0;
                    src.decodedRgba[p * 4u + 3] = channels > 3 ? image.image.data[srcOff + 3] : 255;
                }
            }

            log_info(std::format("glTF image[{}] name='{}' source={} uri='{}' mime='{}' {}x{} bufferView={} bytes={}",
                                 i, strView(image.name), source, uri, mime, image.width, image.height,
                                 image.buffer_view, embeddedBytes),
                     "AssetLoader");
            parseGltfExtras(ctx.geometry, AuxOwnerKind::Image, i, image.ext, std::format("image[{}]", i));
            ctx.images[i] = std::move(src);
        }
    }

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

    static void fillUvRange(std::vector<glm::vec2>& dst, uint32_t first, uint32_t count, const std::vector<float>& src,
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

    static void storeMorphTargets(GeometryStore& geometry, const tg3_model& model, const tg3_primitive& prim,
                                  uint32_t vertexCount)
    {
        if (prim.targets_count == 0) {
            return;
        }
        log_info(std::format("glTF morph targets: {}", prim.targets_count), "AssetLoader");
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
                log_info(std::format("glTF morph[{}] attr='{}' accessor={} floats={}", t, key, attrs[a].value,
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
            log_info(std::format("glTF attr '{}' accessor={} floats={}", name, accessorIdx, normals.size()),
                     "AssetLoader");
            fillVec3Range(geometry.normals, firstVertex, vertexCount, normals, accessorCompCount(model, accessorIdx));
            return;
        }
        if (name == "TANGENT") {
            const std::vector<float> tangents = readAccessorFloats(model, accessorIdx);
            log_info(std::format("glTF attr '{}' accessor={} floats={}", name, accessorIdx, tangents.size()),
                     "AssetLoader");
            fillVec4Range(geometry.tangents, firstVertex, vertexCount, tangents, accessorCompCount(model, accessorIdx),
                          glm::vec4{0.0f, 0.0f, 0.0f, 1.0f});
            return;
        }
        if (name == "COLOR_0") {
            const std::vector<float> colors = readAccessorFloats(model, accessorIdx);
            log_info(std::format("glTF attr '{}' accessor={} floats={}", name, accessorIdx, colors.size()),
                     "AssetLoader");
            fillVec4Range(geometry.colors, firstVertex, vertexCount, colors, accessorCompCount(model, accessorIdx),
                          glm::vec4{1.0f});
            return;
        }
        if (name == "TEXCOORD_1") {
            const std::vector<float> uvs = readAccessorFloats(model, accessorIdx);
            log_info(std::format("glTF attr '{}' accessor={} floats={}", name, accessorIdx, uvs.size()),
                     "AssetLoader");
            fillUvRange(geometry.uv1, firstVertex, vertexCount, uvs, accessorCompCount(model, accessorIdx), true);
            return;
        }
        if (name.starts_with("TEXCOORD_") && name != "TEXCOORD_0") {
            log_info(std::format("glTF attr '{}' accessor={} skipped (uv2+)", name, accessorIdx), "AssetLoader");
            return;
        }
        if (name.starts_with("COLOR_")) {
            log_info(std::format("glTF attr '{}' accessor={} skipped (COLOR_n>0)", name, accessorIdx), "AssetLoader");
            return;
        }
        if (name == "JOINTS_0") {
            const std::vector<uint32_t> joints = readAccessorU32(model, accessorIdx);
            log_info(std::format("glTF attr '{}' accessor={} u32={}", name, accessorIdx, joints.size()),
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
            return;
        }
        if (name == "WEIGHTS_0") {
            const std::vector<float> weights = readAccessorFloats(model, accessorIdx);
            log_info(std::format("glTF attr '{}' accessor={} floats={}", name, accessorIdx, weights.size()),
                     "AssetLoader");
            fillVec4Range(geometry.weights0, firstVertex, vertexCount, weights, accessorCompCount(model, accessorIdx),
                          glm::vec4{0.0f});
            return;
        }
        if (name.starts_with("JOINTS_") || name.starts_with("WEIGHTS_")) {
            log_info(std::format("glTF attr '{}' accessor={} skipped (set > 0)", name, accessorIdx), "AssetLoader");
            return;
        }
        log_info(std::format("glTF attr '{}' accessor={} skipped", name, accessorIdx), "AssetLoader");
    }

    static uint32_t appendGltfGeometry(GltfLoadCtx& ctx, const tg3_model& model)
    {
        log_info(std::format("glTF meshes: {}", model.meshes_count), "AssetLoader");
        GeometryStore& geometry = ctx.geometry;
        uint32_t storedPrimitives = 0;

        for (uint32_t mi = 0; mi < model.meshes_count; ++mi) {
            const tg3_mesh& mesh = model.meshes[mi];
            log_info(std::format("glTF mesh[{}] name='{}' primitives={} morphWeights={}", mi, strView(mesh.name),
                                 mesh.primitives_count, mesh.weights_count),
                     "AssetLoader");
            const uint32_t morphWeightFirst = static_cast<uint32_t>(geometry.morphWeights.size());
            for (uint32_t wi = 0; wi < mesh.weights_count; ++wi) {
                geometry.morphWeights.push_back(static_cast<float>(mesh.weights[wi]));
            }
            parseGltfExtras(geometry, AuxOwnerKind::Mesh, mi, mesh.ext, std::format("mesh[{}]", mi));

            for (uint32_t pi = 0; pi < mesh.primitives_count; ++pi) {
                const tg3_primitive& prim = mesh.primitives[pi];
                const int32_t mode = prim.mode < 0 ? TG3_MODE_TRIANGLES : prim.mode;
                log_info(std::format("glTF mesh[{}].prim[{}] mode={} material={} attrs={} morphTargets={} indicesAcc={}",
                                     mi, pi, primitiveModeName(mode), prim.material, prim.attributes_count,
                                     prim.targets_count, prim.indices),
                         "AssetLoader");
                parseGltfExtras(geometry, AuxOwnerKind::Primitive, static_cast<uint32_t>(geometry.primitiveDraws.size()),
                                prim.ext, std::format("mesh[{}].prim[{}]", mi, pi));

                if (mode != TG3_MODE_TRIANGLES && mode != TG3_MODE_TRIANGLE_STRIP && mode != TG3_MODE_TRIANGLE_FAN) {
                    log_info(std::format("glTF mesh[{}].prim[{}] skipped: non-triangle mode", mi, pi), "AssetLoader");
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
                    log_info(std::format("glTF mesh[{}].prim[{}] skipped: no POSITION", mi, pi), "AssetLoader");
                    continue;
                }

                const std::vector<float> positions = readAccessorFloats(model, posAcc);
                if (positions.empty()) {
                    log_info(std::format("glTF mesh[{}].prim[{}] skipped: empty POSITION accessor {}", mi, pi, posAcc),
                             "AssetLoader");
                    continue;
                }

                const uint32_t posComps = accessorCompCount(model, posAcc);
                const uint32_t vertexCount = model.accessors[posAcc].count;
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

                std::vector<uint32_t> srcIndices = readAccessorIndices(model, prim.indices);
                if (srcIndices.empty()) {
                    srcIndices.resize(vertexCount);
                    for (uint32_t v = 0; v < vertexCount; ++v) {
                        srcIndices[v] = v;
                    }
                }
                const std::vector<uint32_t> triIndices = toTriangleIndices(mode, srcIndices);
                if (triIndices.empty()) {
                    log_info(std::format("glTF mesh[{}].prim[{}] skipped: no triangles", mi, pi), "AssetLoader");
                    continue;
                }

                const uint32_t firstIndex = static_cast<uint32_t>(geometry.indices.size());
                geometry.indices.reserve(firstIndex + triIndices.size());
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
                draw.morphWeightFirst = mesh.weights_count > 0 ? morphWeightFirst : 0;
                draw.meshlets = geometry.buildMeshletsForRange(firstIndex, indexCount);
                geometry.primitiveDraws.push_back(draw);
                ++storedPrimitives;

                log_info(std::format("glTF mesh[{}].prim[{}] POSITION verts={} TEXCOORD_0 floats={} indices={} stored={}",
                                     mi, pi, vertexCount, texcoords.size(), indexCount, indexCount),
                         "AssetLoader");
            }
        }

        log_info(std::format("glTF geometry appended: verts={} indices={} primitives={}", geometry.vertices.size(),
                             geometry.indices.size(), storedPrimitives),
                 "AssetLoader");
        return storedPrimitives;
    }

} // anonymous namespace

static std::vector<char> readFile(const std::string& filename)
{
    std::ifstream file(filename, std::ios::ate | std::ios::binary);

    if (!file.is_open()) {
        throw std::runtime_error("failed to open file!");
    }
    std::vector<char> buffer(file.tellg());
    file.seekg(0, std::ios::beg);
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    file.close();
    return buffer;
}


AssetsLoader::AssetsLoader(ObjectStorage& objectStorageIn, TextureManager& textureManagerIn,
                           GeometryStore& geometryStoreIn, MaterialStore& materialStoreIn, LightStore& lightStoreIn) :
    objectStorage(objectStorageIn), textureManager(textureManagerIn), geometryStore(geometryStoreIn),
    materialStore(materialStoreIn), lightStore(lightStoreIn)
{
    log_info("AssetsLoader initialized", "AssetLoader");
}


void AssetsLoader::loadModel(std::string modelPath, glm::vec3 xyz)
{
    ZoneScopedN("AssetsLoader::loadModel");
    // Normalise to native separators once so every loader receives a
    // clean, OS-consistent path regardless of how it was supplied.
    const std::string path = std::filesystem::path(modelPath).make_preferred().string();

    const bool isGltf = path.ends_with(".gltf") || path.ends_with(".glb");
    const bool isObj = path.ends_with(".obj");

    if (isGltf) {
        loadGltfModel(path, xyz);
        return;
    }

    if (isObj) {
        loadObjModel(path, xyz);
        return;
    }

    assert(false && "Unsupported model format");
}

bool AssetsLoader::loadGltfModel(const std::string& modelPath, glm::vec3 xyz)
{
    ZoneScopedN("AssetsLoader::loadGltfModel");
    // glTF uses forward-slash URIs internally; normalise the base path
    // to avoid mixed separators when the library resolves external .bin
    // references (e.g. "models/AnimatedCube.bin" under "models\" on Windows).
    const std::string normalizedPath = std::filesystem::path(modelPath).generic_string();

    tg3_model model{};
    tg3_error_stack errors;
    tg3_error_stack_init(&errors);

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
            log_error(std::format("Failed to parse glTF (rc={}):\n{}", static_cast<int>(rc), details), "AssetLoader");
        } else {
            log_error(std::format("Failed to parse glTF: rc={}", static_cast<int>(rc)), "AssetLoader");
        }
        tg3_model_free(&model);
        tg3_error_stack_free(&errors);
        return false;
    }

    log_info(std::format("Loading glTF: {} meshes, {} nodes", model.meshes_count, model.nodes_count), "AssetLoader");

    GltfLoadCtx ctx{
        .geometry = geometryStore,
        .materials = materialStore,
        .lights = lightStore,
        .textures = textureManager,
        .defaultSamplerHeap = textureManager.getOrCreateSampler(-1, -1, 10497, 10497),
    };

    const uint32_t firstPrimitive = static_cast<uint32_t>(geometryStore.primitiveDraws.size());
    parseGltfRootExtensions(ctx, model);
    const std::vector<uint32_t> samplerHeaps = parseGltfSamplers(ctx, model);
    parseGltfImages(ctx, model, std::filesystem::path(modelPath).parent_path());
    parseGltfTextures(ctx, model, samplerHeaps);
    parseGltfMaterials(ctx, model);
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

    uint32_t previewTex = 0;
    uint32_t previewMat = materialStore.defaultMaterialId();
    if (primitiveCount > 0) {
        const PrimitiveDraw& first = geometryStore.primitiveDraws[firstPrimitive];
        previewMat = first.materialId;
        if (first.materialId < materialStore.size()) {
            const GpuMaterial& gpu = materialStore.gpuMaterials[first.materialId];
            if (gpu.baseColorTex != kNoneIndex) {
                previewTex = gpu.baseColorTex;
            }
        }
    }

    const Transform transform{.position = glm::vec3{xyz[0], xyz[1], xyz[2]}};
    const MaterialRef material{.textureIndex = previewTex, .materialId = previewMat};
    const EntityId id = objectStorage.create(transform, unionDraw, material, firstPrimitive, primitiveCount, modelPath);
    log_info(std::format("Loaded model entity {} | primitives=[{}, {}) | meshlets: {} (first {})", id, firstPrimitive,
                         firstPrimitive + primitiveCount, unionDraw.meshletCount, unionDraw.firstMeshlet),
             "AssetLoader");

    log_info(std::format("Model loaded (glTF): {} | vertices: {} | indices: {} | total meshlets: {}", modelPath,
                         geometryStore.vertices.size(), geometryStore.indices.size(), geometryStore.meshlets.size()),
             "AssetLoader");

    tg3_model_free(&model);
    tg3_error_stack_free(&errors);
    return true;
}

bool AssetsLoader::loadObjModel(const std::string& modelPath, glm::vec3 xyz)
{
    ZoneScopedN("AssetsLoader::loadObjModel");
    log_info(std::format("Loading OBJ: {}", modelPath), "AssetLoader");
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string err;

    // tinyobj wraps standard C file I/O — native separators are correct.
    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &err, modelPath.c_str())) {
        log_error(std::format("Failed to load OBJ: {}", err), "AssetLoader");
        return false;
    }

    std::unordered_map<GpuVertex, uint32_t> uniqueVertices{};
    uint32_t indexCount = 0;
    const uint32_t firstVertex = static_cast<uint32_t>(geometryStore.positions.size());
    const uint32_t firstIndex = static_cast<uint32_t>(geometryStore.indices.size());
    const uint32_t firstPrimitive = static_cast<uint32_t>(geometryStore.primitiveDraws.size());

    for (const auto& [name, mesh] : shapes) {
        for (const auto& index : mesh.indices) {
            GpuVertex vertex{};

            vertex.pos = {attrib.vertices[3 * index.vertex_index + 0], attrib.vertices[3 * index.vertex_index + 1],
                          attrib.vertices[3 * index.vertex_index + 2]};

            vertex.texCoord = {attrib.texcoords[2 * index.texcoord_index + 0],
                               1.0f - attrib.texcoords[2 * index.texcoord_index + 1]};

            vertex.color = {1.0f, 1.0f, 1.0f};

            if (!uniqueVertices.contains(vertex)) {
                const uint32_t id = static_cast<uint32_t>(geometryStore.positions.size());
                uniqueVertices[vertex] = id;
                geometryStore.resizeVertices(id + 1);
                geometryStore.positions[id] = vertex.pos;
                geometryStore.uv0[id] = vertex.texCoord;
                geometryStore.colors[id] = glm::vec4{1.0f};
                geometryStore.packVertex(id);
            }
            geometryStore.indices.push_back(uniqueVertices[vertex]);
            indexCount++;
        }
    }

    GpuMaterial gpu{};
    gpu.baseColorTex = textureManager.loadTexture(TEXTURE_PATH.string());
    gpu.baseColorSamp = textureManager.getOrCreateSampler(-1, -1, 10497, 10497);
    const uint32_t materialId = materialStore.add(gpu);

    PrimitiveDraw draw{};
    draw.materialId = materialId;
    draw.firstVertex = firstVertex;
    draw.vertexCount = static_cast<uint32_t>(geometryStore.positions.size()) - firstVertex;
    draw.firstIndex = firstIndex;
    draw.indexCount = indexCount;
    draw.meshlets = geometryStore.buildMeshletsForRange(firstIndex, indexCount);
    geometryStore.primitiveDraws.push_back(draw);

    const Transform transform{.position = glm::vec3{xyz[0], xyz[1], xyz[2]}};
    const MaterialRef material{.textureIndex = gpu.baseColorTex, .materialId = materialId};
    const EntityId id = objectStorage.create(transform, draw.meshlets, material, firstPrimitive, 1, modelPath);
    log_info(std::format("Loaded model entity {} | primitives=[{}, {}) | meshlets: {} (first {})", id, firstPrimitive,
                         firstPrimitive + 1, draw.meshlets.meshletCount, draw.meshlets.firstMeshlet),
             "AssetLoader");
    log_info(std::format("Model loaded (OBJ): {} | vertices: {} | indices: {} | total meshlets: {}", modelPath,
                         geometryStore.vertices.size(), geometryStore.indices.size(), geometryStore.meshlets.size()),
             "AssetLoader");
    return true;
}
