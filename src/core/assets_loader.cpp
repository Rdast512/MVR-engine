#include "assets_loader.hpp"
#include "../Constants.h"
#include "../static_headers/logger.hpp"
#include "../util/vk_tracy.hpp"

#include <meshoptimizer.h>

// ── glTF external-reference detection ───────────────────────

namespace
{

    // meshopt defaults suited to EXT_mesh_shader (clamp later against device props).
    constexpr size_t kMeshletMaxVertices = 64;
    constexpr size_t kMeshletMaxTriangles = 126;
    constexpr float kMeshletConeWeight = 0.0f;


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

    static void parseGltfExtras(const tg3_extras_ext& ext, std::string_view owner)
    {
        if (ext.extras != nullptr) {
            log_info(std::format("glTF extras on {}", owner), "AssetLoader");
            // TODO: store extras value
        }
        for (uint32_t i = 0; i < ext.extensions_count; ++i) {
            const std::string_view name = strView(ext.extensions[i].name);
            log_info(std::format("glTF extension '{}' on {}", name, owner), "AssetLoader");
            // TODO: store named extension
            // TODO: store extension[i].value
        }
    }

    static void parseGltfRootExtensions(const tg3_model& model)
    {
        log_info(std::format("glTF extensionsUsed={} extensionsRequired={}", model.extensions_used_count,
                             model.extensions_required_count),
                 "AssetLoader");
        for (uint32_t i = 0; i < model.extensions_used_count; ++i) {
            const std::string_view name = strView(model.extensions_used[i]);
            log_info(std::format("glTF extensionsUsed[{}]='{}'", i, name), "AssetLoader");
            // TODO: store extensionsUsed
        }
        for (uint32_t i = 0; i < model.extensions_required_count; ++i) {
            const std::string_view name = strView(model.extensions_required[i]);
            log_info(std::format("glTF extensionsRequired[{}]='{}'", i, name), "AssetLoader");
            // TODO: store extensionsRequired
        }
        parseGltfExtras(model.ext, "model");
        parseGltfExtras(model.asset.ext, "asset");
    }

    static void parseGltfSamplers(const tg3_model& model)
    {
        log_info(std::format("glTF samplers: {}", model.samplers_count), "AssetLoader");
        for (uint32_t i = 0; i < model.samplers_count; ++i) {
            const tg3_sampler& sampler = model.samplers[i];
            log_info(std::format("glTF sampler[{}] name='{}' minFilter={} magFilter={} wrapS={} wrapT={}", i,
                                 strView(sampler.name), sampler.min_filter, sampler.mag_filter, sampler.wrap_s,
                                 sampler.wrap_t),
                     "AssetLoader");
            // TODO: store sampler filters + wrap
            parseGltfExtras(sampler.ext, std::format("sampler[{}]", i));
        }
    }

    static void parseGltfTextures(const tg3_model& model)
    {
        log_info(std::format("glTF textures: {}", model.textures_count), "AssetLoader");
        for (uint32_t i = 0; i < model.textures_count; ++i) {
            const tg3_texture& texture = model.textures[i];
            log_info(std::format("glTF texture[{}] name='{}' source={} sampler={}", i, strView(texture.name),
                                 texture.source, texture.sampler),
                     "AssetLoader");
            // TODO: store texture source + sampler indices
            parseGltfExtras(texture.ext, std::format("texture[{}]", i));
        }
    }

    static void parseGltfTextureInfo(const tg3_texture_info& info, std::string_view slot)
    {
        log_info(std::format("glTF {} index={} texCoord={}", slot, info.index, info.tex_coord), "AssetLoader");
        // TODO: store textureInfo index + texCoord
        parseGltfExtras(info.ext, slot);
    }

    static void parseGltfMaterials(const tg3_model& model)
    {
        log_info(std::format("glTF materials: {}", model.materials_count), "AssetLoader");
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
            // TODO: store PBR baseColorFactor / metallicFactor / roughnessFactor
            parseGltfTextureInfo(pbr.base_color_texture, owner + ".baseColorTexture");
            parseGltfTextureInfo(pbr.metallic_roughness_texture, owner + ".metallicRoughnessTexture");

            log_info(std::format("glTF {}.normalTexture index={} texCoord={} scale={}", owner,
                                 material.normal_texture.index, material.normal_texture.tex_coord,
                                 material.normal_texture.scale),
                     "AssetLoader");
            // TODO: store normalTexture index, texCoord, scale
            parseGltfExtras(material.normal_texture.ext, owner + ".normalTexture");

            log_info(std::format("glTF {}.occlusionTexture index={} texCoord={} strength={}", owner,
                                 material.occlusion_texture.index, material.occlusion_texture.tex_coord,
                                 material.occlusion_texture.strength),
                     "AssetLoader");
            // TODO: store occlusionTexture index, texCoord, strength
            parseGltfExtras(material.occlusion_texture.ext, owner + ".occlusionTexture");

            parseGltfTextureInfo(material.emissive_texture, owner + ".emissiveTexture");
            // TODO: store emissiveFactor
            // TODO: store alphaMode / alphaCutoff / doubleSided

            parseGltfExtras(material.ext, owner);
            parseGltfExtras(pbr.ext, owner + ".pbr");
        }
    }

    static void parseGltfLights(const tg3_model& model)
    {
        log_info(std::format("glTF lights: {}", model.lights_count), "AssetLoader");
        for (uint32_t i = 0; i < model.lights_count; ++i) {
            const tg3_light& light = model.lights[i];
            log_info(std::format("glTF light[{}] name='{}' type='{}' color=({}, {}, {}) intensity={} range={} "
                                 "spotInner={} spotOuter={}",
                                 i, strView(light.name), strView(light.type), light.color[0], light.color[1],
                                 light.color[2], light.intensity, light.range, light.spot.inner_cone_angle,
                                 light.spot.outer_cone_angle),
                     "AssetLoader");
            // TODO: store punctual light type, color, intensity, range
            // TODO: store spot innerConeAngle / outerConeAngle
            parseGltfExtras(light.spot.ext, std::format("light[{}].spot", i));
            parseGltfExtras(light.ext, std::format("light[{}]", i));
        }

        for (uint32_t ni = 0; ni < model.nodes_count; ++ni) {
            const int32_t lightIndex = model.nodes[ni].light;
            if (lightIndex < 0)
                continue;
            log_info(std::format("glTF node[{}] name='{}' light={}", ni, strView(model.nodes[ni].name), lightIndex),
                     "AssetLoader");
            // TODO: attach lights[node.light] to this node
        }
    }

    static std::string parseGltfImages(const tg3_model& model, const std::filesystem::path& modelDir)
    {
        log_info(std::format("glTF images: {}", model.images_count), "AssetLoader");
        std::string firstExternalPath;

        for (uint32_t i = 0; i < model.images_count; ++i) {
            const tg3_image& image = model.images[i];
            const std::string_view uri = strView(image.uri);
            const std::string_view mime = strView(image.mime_type);
            uint64_t embeddedBytes = 0;
            const char* source = "none";

            if (image.buffer_view >= 0) {
                const tg3_span_u8 bytes = readBufferViewBytes(model, image.buffer_view);
                embeddedBytes = bytes.count;
                source = "bufferView";
                if (bytes.data != nullptr && bytes.count > 0) {
                    // TODO: store embedded image bytes (GLB bufferView)
                }
            } else if (!uri.empty() && uri.starts_with("data:")) {
                source = "dataUri";
                // TODO: decode data-URI image bytes
            } else if (!uri.empty()) {
                source = "uri";
                const std::string path = (modelDir / uri).string();
                if (i == 0)
                    firstExternalPath = path;
                // TODO: store external image path
            }

            if (image.image.data != nullptr && image.image.count > 0) {
                source = "decoded";
                embeddedBytes = image.image.count;
                // TODO: store decoded image pixels
            }

            log_info(std::format("glTF image[{}] name='{}' source={} uri='{}' mime='{}' {}x{} bufferView={} bytes={}",
                                 i, strView(image.name), source, uri, mime, image.width, image.height,
                                 image.buffer_view, embeddedBytes),
                     "AssetLoader");
            // TODO: store image mimeType / size
            parseGltfExtras(image.ext, std::format("image[{}]", i));
        }

        return firstExternalPath;
    }

    static void parseGltfVertexAttribute(const tg3_model& model, std::string_view name, int32_t accessorIdx)
    {
        if (name == "NORMAL") {
            const std::vector<float> normals = readAccessorFloats(model, accessorIdx);
            log_info(std::format("glTF attr '{}' accessor={} floats={}", name, accessorIdx, normals.size()),
                     "AssetLoader");
            // TODO: store NORMAL
            return;
        }
        if (name == "TANGENT") {
            const std::vector<float> tangents = readAccessorFloats(model, accessorIdx);
            log_info(std::format("glTF attr '{}' accessor={} floats={}", name, accessorIdx, tangents.size()),
                     "AssetLoader");
            // TODO: store TANGENT
            return;
        }
        if (name.starts_with("COLOR_")) {
            const std::vector<float> colors = readAccessorFloats(model, accessorIdx);
            log_info(std::format("glTF attr '{}' accessor={} floats={}", name, accessorIdx, colors.size()),
                     "AssetLoader");
            // TODO: store COLOR_n
            return;
        }
        if (name.starts_with("TEXCOORD_") && name != "TEXCOORD_0") {
            const std::vector<float> uvs = readAccessorFloats(model, accessorIdx);
            log_info(std::format("glTF attr '{}' accessor={} floats={}", name, accessorIdx, uvs.size()),
                     "AssetLoader");
            // TODO: store extra TEXCOORD_n
            return;
        }
        if (name.starts_with("JOINTS_")) {
            const std::vector<uint32_t> joints = readAccessorU32(model, accessorIdx);
            log_info(std::format("glTF attr '{}' accessor={} u32={}", name, accessorIdx, joints.size()),
                     "AssetLoader");
            // TODO: store JOINTS_n
            return;
        }
        if (name.starts_with("WEIGHTS_")) {
            const std::vector<float> weights = readAccessorFloats(model, accessorIdx);
            log_info(std::format("glTF attr '{}' accessor={} floats={}", name, accessorIdx, weights.size()),
                     "AssetLoader");
            // TODO: store WEIGHTS_n
            return;
        }
        log_info(std::format("glTF attr '{}' accessor={} skipped", name, accessorIdx), "AssetLoader");
    }

    static void parseGltfMorphTargets(const tg3_model& model, const tg3_primitive& prim)
    {
        if (prim.targets_count == 0)
            return;
        log_info(std::format("glTF morph targets: {}", prim.targets_count), "AssetLoader");
        for (uint32_t t = 0; t < prim.targets_count; ++t) {
            if (!prim.targets || !prim.target_attribute_counts)
                break;
            const tg3_str_int_pair* attrs = prim.targets[t];
            const uint32_t attrCount = prim.target_attribute_counts[t];
            if (!attrs)
                continue;
            for (uint32_t a = 0; a < attrCount; ++a) {
                const std::vector<float> delta = readAccessorFloats(model, attrs[a].value);
                log_info(std::format("glTF morph[{}] attr='{}' accessor={} floats={}", t, strView(attrs[a].key),
                                     attrs[a].value, delta.size()),
                         "AssetLoader");
                // TODO: store morph-target delta for attrs[a].key
            }
        }
    }

    static uint32_t appendGltfGeometry(const tg3_model& model, std::vector<Vertex>& vertices,
                                       std::vector<uint32_t>& indices)
    {
        log_info(std::format("glTF meshes: {}", model.meshes_count), "AssetLoader");
        std::unordered_map<Vertex, uint32_t> uniqueVertices{};
        uint32_t indexCount = 0;

        for (uint32_t mi = 0; mi < model.meshes_count; ++mi) {
            const tg3_mesh& mesh = model.meshes[mi];
            log_info(std::format("glTF mesh[{}] name='{}' primitives={} morphWeights={}", mi, strView(mesh.name),
                                 mesh.primitives_count, mesh.weights_count),
                     "AssetLoader");
            // TODO: store mesh morph weights
            parseGltfExtras(mesh.ext, std::format("mesh[{}]", mi));

            for (uint32_t pi = 0; pi < mesh.primitives_count; ++pi) {
                const tg3_primitive& prim = mesh.primitives[pi];
                const int32_t mode = prim.mode < 0 ? TG3_MODE_TRIANGLES : prim.mode;
                const uint32_t indexBefore = indexCount;
                log_info(std::format("glTF mesh[{}].prim[{}] mode={} material={} attrs={} morphTargets={} indicesAcc={}",
                                     mi, pi, primitiveModeName(mode), prim.material, prim.attributes_count,
                                     prim.targets_count, prim.indices),
                         "AssetLoader");
                // TODO: store primitive.mode (geometry still emitted as triangle list)
                // TODO: store primitive.material index
                parseGltfExtras(prim.ext, std::format("mesh[{}].prim[{}]", mi, pi));
                parseGltfMorphTargets(model, prim);

                int32_t posAcc = -1;
                int32_t tcAcc = -1;
                const int32_t idxAcc = prim.indices;

                for (uint32_t ai = 0; ai < prim.attributes_count; ++ai) {
                    const tg3_str_int_pair& attr = prim.attributes[ai];
                    const std::string_view name = strView(attr.key);
                    if (name == "POSITION")
                        posAcc = attr.value;
                    else if (name == "TEXCOORD_0")
                        tcAcc = attr.value;
                    else
                        parseGltfVertexAttribute(model, name, attr.value);
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

                const std::vector<float> texcoords = readAccessorFloats(model, tcAcc);
                const std::vector<uint32_t> idxData = readAccessorIndices(model, idxAcc);

                const uint32_t posComps = static_cast<uint32_t>(tg3_num_components(model.accessors[posAcc].type));
                const uint32_t tcComps =
                    tcAcc >= 0 ? static_cast<uint32_t>(tg3_num_components(model.accessors[tcAcc].type)) : 0;
                const uint32_t vertexCount = model.accessors[posAcc].count;

                auto emitVertex = [&](uint32_t vi) -> void
                {
                    if (vi >= vertexCount)
                        return;
                    Vertex vertex{};
                    vertex.pos = {positions[vi * posComps + 0], positions[vi * posComps + 1],
                                  posComps >= 3 ? positions[vi * posComps + 2] : 0.0f};
                    if (tcComps >= 2) {
                        vertex.texCoord = {texcoords[vi * tcComps + 0], 1.0f - texcoords[vi * tcComps + 1]};
                    }
                    vertex.color = {1.0f, 1.0f, 1.0f};

                    if (!uniqueVertices.contains(vertex)) {
                        uniqueVertices[vertex] = static_cast<uint32_t>(vertices.size());
                        vertices.push_back(vertex);
                    }
                    indices.push_back(uniqueVertices[vertex]);
                    ++indexCount;
                };

                if (!idxData.empty()) {
                    for (const uint32_t vid : idxData) {
                        emitVertex(vid);
                    }
                } else {
                    for (uint32_t vid = 0; vid < vertexCount; ++vid) {
                        emitVertex(vid);
                    }
                }

                log_info(std::format("glTF mesh[{}].prim[{}] POSITION verts={} TEXCOORD_0 floats={} indices={} emitted={}",
                                     mi, pi, vertexCount, texcoords.size(), idxData.size(), indexCount - indexBefore),
                         "AssetLoader");
            }
        }

        log_info(std::format("glTF geometry appended: uniqueVerts={} indices={}", vertices.size(), indexCount),
                 "AssetLoader");
        return indexCount;
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


AssetsLoader::AssetsLoader(ObjectStorage& objectStorageIn, TextureManager& textureManagerIn) :
    vertices(), indices(), objectStorage(objectStorageIn), textureManager(textureManagerIn)
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

MeshletDraw AssetsLoader::buildMeshletsForRange(uint32_t firstIndex, uint32_t indexCount)
{
    ZoneScopedN("AssetsLoader::buildMeshletsForRange");
    if (indexCount == 0 || vertices.empty() || firstIndex + indexCount > indices.size()) {
        return {};
    }

    const size_t maxMeshlets = meshopt_buildMeshletsBound(indexCount, kMeshletMaxVertices, kMeshletMaxTriangles);
    std::vector<meshopt_Meshlet> built(maxMeshlets);
    std::vector<unsigned int> localVertices(indexCount);
    std::vector<unsigned char> localTriangles(indexCount);

    const size_t meshletCount =
        meshopt_buildMeshlets(built.data(), localVertices.data(), localTriangles.data(), indices.data() + firstIndex,
                              indexCount, &vertices[0].pos.x, vertices.size(), sizeof(Vertex), kMeshletMaxVertices,
                              kMeshletMaxTriangles, kMeshletConeWeight);

    if (meshletCount == 0) {
        return {};
    }

    const meshopt_Meshlet& last = built[meshletCount - 1];
    localVertices.resize(last.vertex_offset + last.vertex_count);
    localTriangles.resize(last.triangle_offset + last.triangle_count * 3);
    built.resize(meshletCount);

    const uint32_t baseVertexOffset = static_cast<uint32_t>(meshletVertices.size());
    const uint32_t baseTriangleOffset = static_cast<uint32_t>(meshletTriangles.size());
    const uint32_t baseMeshlet = static_cast<uint32_t>(meshlets.size());

    meshletVertices.insert(meshletVertices.end(), localVertices.begin(), localVertices.end());
    meshletTriangles.insert(meshletTriangles.end(), localTriangles.begin(), localTriangles.end());
    meshlets.reserve(meshlets.size() + meshletCount);

    for (size_t i = 0; i < meshletCount; ++i) {
        const meshopt_Meshlet& m = built[i];
        const uint32_t vertexOffset = baseVertexOffset + m.vertex_offset;
        const uint32_t triangleOffset = baseTriangleOffset + m.triangle_offset;

        meshopt_optimizeMeshlet(meshletVertices.data() + vertexOffset, meshletTriangles.data() + triangleOffset,
                                m.triangle_count, m.vertex_count);

        const meshopt_Bounds bounds = meshopt_computeMeshletBounds(
            meshletVertices.data() + vertexOffset, meshletTriangles.data() + triangleOffset, m.triangle_count,
            &vertices[0].pos.x, vertices.size(), sizeof(Vertex));

        meshlets.push_back(MeshletDesc{
            .vertexOffset = vertexOffset,
            .triangleOffset = triangleOffset,
            .vertexCount = m.vertex_count,
            .triangleCount = m.triangle_count,
            .boundingSphere = glm::vec4{bounds.center[0], bounds.center[1], bounds.center[2], bounds.radius},
        });
    }

    const MeshletDraw draw{
        .firstMeshlet = baseMeshlet,
        .meshletCount = static_cast<uint32_t>(meshletCount),
    };

    log_info(std::format("Built {} meshlets for index range [{}, {}) ({} meshlet verts, {} local tri corners)",
                         draw.meshletCount, firstIndex, firstIndex + indexCount, localVertices.size(),
                         localTriangles.size()),
             "AssetLoader");

    return draw;
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

    parseGltfRootExtensions(model);
    parseGltfSamplers(model);
    parseGltfTextures(model);
    parseGltfMaterials(model);
    parseGltfLights(model);
    const std::string imagePath = parseGltfImages(model, std::filesystem::path(modelPath).parent_path());
    const uint32_t indexCount = appendGltfGeometry(model, vertices, indices);

    const Transform transform{.position = glm::vec3{xyz[0], xyz[1], xyz[2]}};
    const MeshletDraw meshletDraw = buildMeshletsForRange(currentIndex, indexCount);
    const MaterialRef material{.textureIndex = textureManager.loadTexture(imagePath), .materialId = 0};
    const EntityId id = objectStorage.create(transform, meshletDraw, material, modelPath);
    log_info(std::format("Loaded model entity {} | index range [{}, {}) | meshlets: {} (first {})", id, currentIndex,
                         currentIndex + indexCount, meshletDraw.meshletCount, meshletDraw.firstMeshlet),
             "AssetLoader");
    currentIndex += indexCount;

    log_info(std::format("Model loaded (glTF): {} | vertices: {} | indices: {} | total meshlets: {}", modelPath,
                         vertices.size(), indices.size(), meshlets.size()),
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

    std::unordered_map<Vertex, uint32_t> uniqueVertices{};
    uint32_t indexCount = 0;

    for (const auto& [name, mesh] : shapes) {
        for (const auto& index : mesh.indices) {
            Vertex vertex{};

            vertex.pos = {attrib.vertices[3 * index.vertex_index + 0], attrib.vertices[3 * index.vertex_index + 1],
                          attrib.vertices[3 * index.vertex_index + 2]};

            vertex.texCoord = {attrib.texcoords[2 * index.texcoord_index + 0],
                               1.0f - attrib.texcoords[2 * index.texcoord_index + 1]};

            vertex.color = {1.0f, 1.0f, 1.0f};

            if (!uniqueVertices.contains(vertex)) {
                uniqueVertices[vertex] = static_cast<uint32_t>(vertices.size());
                vertices.push_back(vertex);
            }
            indices.push_back(uniqueVertices[vertex]);
            indexCount++;
        }
    }
    const Transform transform{.position = glm::vec3{xyz[0], xyz[1], xyz[2]}};
    const MeshletDraw meshletDraw = buildMeshletsForRange(currentIndex, indexCount);
    const MaterialRef material{.textureIndex = textureManager.loadTexture(TEXTURE_PATH.string()), .materialId = 0};
    const EntityId id = objectStorage.create(transform, meshletDraw, material, modelPath);
    log_info(std::format("Loaded model entity {} | index range [{}, {}) | meshlets: {} (first {})", id, currentIndex,
                         currentIndex + indexCount, meshletDraw.meshletCount, meshletDraw.firstMeshlet),
             "AssetLoader");
    currentIndex += indexCount;
    log_info(std::format("Model loaded (OBJ): {} | vertices: {} | indices: {} | total meshlets: {}", modelPath,
                         vertices.size(), indices.size(), meshlets.size()),
             "AssetLoader");
    return true;
}
