#pragma once

#include "types.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <format>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// M = T * R * S. v grows down: the rotation sign follows the Khronos sample renderer and the extension's example
[[nodiscard]] inline GpuUvMatrix packUvMatrix(const MaterialUvTransform& uv)
{
    const float c = std::cos(uv.rotation);
    const float s = std::sin(uv.rotation);
    return {.row0 = {c * uv.scale.x, s * uv.scale.y, uv.offset.x}, .row1 = {-s * uv.scale.x, c * uv.scale.y, uv.offset.y}};
}

// the store keeps authored values for dedup and the UI; the GPU row is precomputed
[[nodiscard]] inline GpuMaterial packMaterial(GpuMaterial material, const MaterialPbrExtension& ext)
{
    material.emissiveFactor *= ext.emissiveStrength;
    const std::array<MaterialUvTransform, kMaterialUvSlotCount> transforms = {
        ext.baseColorUv, ext.metalRoughUv, ext.normalUv, ext.occlusionUv, ext.emissiveUv};
    for (uint32_t slot = 0; slot < kMaterialUvSlotCount; ++slot) {
        if (transforms[slot] != MaterialUvTransform{}) {
            material.uvMatrices[slot] = packUvMatrix(transforms[slot]);
            material.flags |= GpuMaterialFlag::UvTransformBaseColor << slot;
        }
    }
    return material;
}

// Blend is cut like Mask at this alpha: no transparency before 1.0 (plan D)
inline constexpr float kBlendAlphaCutoff = 0.5f;

// factor.a x texture.a below it is cut
[[nodiscard]] inline float alphaTestCutoff(const GpuMaterial& material)
{
    return (material.flags & GpuMaterialFlag::AlphaModeMask) == GpuMaterialFlag::AlphaMask ? material.alphaCutoff
                                                                                          : kBlendAlphaCutoff;
}

// one rule for the raster alpha bin (spec/m6/d3) and the OMM bake (d1), so raster and rays cut the same surfaces: a
// Mask or Blend material whose base-color texture decides the cut. Untextured alpha is constant, a cutoff <= 0 cuts
// nothing, and a factor alpha below the cutoff would cut everything: all of these stay opaque
[[nodiscard]] inline bool isAlphaTested(const GpuMaterial& material)
{
    if ((material.flags & GpuMaterialFlag::AlphaModeMask) == GpuMaterialFlag::AlphaOpaque ||
        material.baseColorTex == kNoneIndex || material.baseColorSamp == kNoneIndex) {
        return false;
    }
    const float cutoff = alphaTestCutoff(material);
    return cutoff > 0.0f && material.baseColorFactor.a >= cutoff;
}

// the resolve bin of a surface pixel; matches materialBin in shaders/material/pbr.slang. Background is never a
// material's: it comes from depth 0
[[nodiscard]] inline MaterialBin materialBin(const GpuMaterial& material)
{
    return (material.flags & GpuMaterialFlag::Unlit) != 0 ? MaterialBin::Unlit : MaterialBin::Standard;
}

// spec/m7/e2_emissive_triangles.md: every triangle of a primitive with this packed row is a light. An unlit color is
// a display rule, not emission into the scene
[[nodiscard]] inline bool isLightEmitter(const GpuMaterial& packed)
{
    return (packed.flags & GpuMaterialFlag::Unlit) == 0 &&
        (packed.emissiveFactor.r > 0.0f || packed.emissiveFactor.g > 0.0f || packed.emissiveFactor.b > 0.0f);
}

// Shared material table. Rows are deduplicated by value across every loaded model and refcounted by the models that
// reference them. Ids are absolute GPU rows and never shift: a freed row's slot is reused by the next new material.
// Row 0 is the default material, permanent and not refcounted.
class MaterialStore
{
public:
    struct MaterialInfo
    {
        std::string name;
        GpuMaterial material;
        MaterialPbrExtension extension;
        // models holding the row; the row is freed when this reaches 0
        uint32_t refCount = 0;
        // loads answered by this row after it was created: add() dedup and model instancing
        uint32_t dedupHits = 0;
        bool isFree = false;
    };

    // indexed by row id; free rows keep their slot until reused
    std::vector<MaterialInfo> infos;
    // rows created since the last flush; their GPU slot still holds stale data
    std::vector<uint32_t> dirtyRows;
    uint32_t cacheHits = 0;
    uint32_t cacheMisses = 0;

    MaterialStore()
    {
        infos.push_back({.name = "engine:default", .material = {}, .extension = {}});
        dirtyRows.push_back(kDefaultId);
        lookup.emplace(materialHash(infos[kDefaultId].material), kDefaultId);
    }

    [[nodiscard]] uint32_t defaultMaterialId() const noexcept { return kDefaultId; }

    // starts collecting the rows the next model references
    void beginModel() { loadRows.clear(); }

    uint32_t add(const GpuMaterial& material, const MaterialPbrExtension& ext = {}, std::string_view name = {})
    {
        const size_t hash = materialHash(material);
        const auto [first, last] = lookup.equal_range(hash);
        for (auto it = first; it != last; ++it) {
            MaterialInfo& info = infos[it->second];
            if (info.material == material && info.extension == ext) {
                ++cacheHits;
                ++info.dedupHits;
                noteLoadRow(it->second);
                return it->second;
            }
        }

        uint32_t id = static_cast<uint32_t>(infos.size());
        if (freeRows.empty()) {
            infos.emplace_back();
        } else {
            id = freeRows.back();
            freeRows.pop_back();
        }
        infos[id] = {.name = std::string(name), .material = material, .extension = ext};
        lookup.emplace(hash, id);
        dirtyRows.push_back(id);
        ++cacheMisses;
        noteLoadRow(id);
        return id;
    }

    // GPU table rows, free slots included
    [[nodiscard]] uint32_t size() const noexcept { return static_cast<uint32_t>(infos.size()); }
    [[nodiscard]] uint32_t liveCount() const noexcept { return size() - static_cast<uint32_t>(freeRows.size()); }

    // the model being flushed takes one reference to every row its load touched; returns those rows
    [[nodiscard]] std::vector<uint32_t> commitModel()
    {
        retain(loadRows);
        return std::exchange(loadRows, {});
    }

    // failed load: rows only it created go back to the free list
    void discardModel()
    {
        for (const uint32_t id : loadRows) {
            if (id != kDefaultId && infos[id].refCount == 0) {
                freeRow(id);
            }
        }
        loadRows.clear();
        std::erase_if(dirtyRows, [this](uint32_t id) { return infos[id].isFree; });
    }

    // cached model instanced: its rows are reused without passing through add()
    void noteReuse(std::span<const uint32_t> rows)
    {
        for (const uint32_t id : rows) {
            if (id != kDefaultId) {
                ++cacheHits;
                ++infos[id].dedupHits;
            }
        }
    }

    void retain(std::span<const uint32_t> rows)
    {
        for (const uint32_t id : rows) {
            if (id != kDefaultId) {
                ++infos[id].refCount;
            }
        }
    }

    // rows reaching 0 are freed; their textures stay valid while the releasing model still holds them
    // throws on a row that is not held, before any refcount changes
    void release(std::span<const uint32_t> rows)
    {
        for (const uint32_t id : rows) {
            if (id != kDefaultId && (id >= infos.size() || infos[id].refCount == 0)) {
                throw std::logic_error(std::format("MaterialStore::release: row {} is not held", id));
            }
        }
        for (const uint32_t id : rows) {
            if (id == kDefaultId) {
                continue;
            }
            if (--infos[id].refCount == 0) {
                freeRow(id);
            }
        }
    }

private:
    static constexpr uint32_t kDefaultId = 0;

    // buckets by the fields that differ most between materials; equality decides
    [[nodiscard]] static size_t materialHash(const GpuMaterial& material) noexcept
    {
        size_t hash = 0;
        const auto mix = [&hash](uint32_t value) { hash = (hash ^ value) * 0x100000001b3ull; };
        mix(material.baseColorTex);
        mix(material.normalTex);
        mix(material.metalRoughTex);
        mix(material.flags);
        mix(std::bit_cast<uint32_t>(material.baseColorFactor.r));
        mix(std::bit_cast<uint32_t>(material.baseColorFactor.g));
        mix(std::bit_cast<uint32_t>(material.baseColorFactor.b));
        mix(std::bit_cast<uint32_t>(material.roughnessFactor));
        return hash;
    }

    void noteLoadRow(uint32_t id)
    {
        if (std::ranges::find(loadRows, id) == loadRows.end()) {
            loadRows.push_back(id);
        }
    }

    void freeRow(uint32_t id)
    {
        const auto [first, last] = lookup.equal_range(materialHash(infos[id].material));
        for (auto it = first; it != last; ++it) {
            if (it->second == id) {
                lookup.erase(it);
                break;
            }
        }
        infos[id] = {.name = {}, .material = {}, .extension = {}, .refCount = 0, .dedupHits = 0, .isFree = true};
        freeRows.push_back(id);
    }

    std::unordered_multimap<size_t, uint32_t> lookup;
    std::vector<uint32_t> freeRows;
    // distinct rows add() returned since beginModel
    std::vector<uint32_t> loadRows;
};
