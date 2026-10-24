#pragma once

#include "types.hpp"

#include <algorithm>
#include <cassert>
#include <string>
#include <string_view>
#include <vector>

// Material rows live on the GPU. The CPU only holds rows added since the last flush; ids are absolute rows of the GPU
// table, so pending row i has id uploadedCount + i.
class MaterialStore
{
public:
    // inspection mirror of every row, uploaded and pending, indexed by id
    struct MaterialInfo
    {
        std::string name;
        GpuMaterial material;
        MaterialPbrExtension extension;
        // glTF materials add() folded into this row
        uint32_t dedupHits = 0;
    };

    std::vector<GpuMaterial> pendingMaterials;
    std::vector<MaterialPbrExtension> pendingExtensions;
    std::vector<MaterialInfo> infos;
    uint32_t uploadedCount = 0;
    // add() dedups only from here, so every row belongs to one model and unloads with it
    uint32_t modelFirst = 1;
    uint32_t cacheHits = 0;
    uint32_t cacheMisses = 0;

    MaterialStore() { addDefaultRow(); }

    [[nodiscard]] uint32_t defaultMaterialId() const noexcept { return 0; }

    void beginModel() noexcept { modelFirst = size(); }

    uint32_t add(const GpuMaterial& material, const MaterialPbrExtension& ext = {}, std::string_view name = {})
    {
        for (uint32_t id = std::max(modelFirst, uploadedCount); id < size(); ++id) {
            const uint32_t row = id - uploadedCount;
            if (pendingMaterials[row] == material && pendingExtensions[row] == ext) {
                ++cacheHits;
                ++infos[id].dedupHits;
                return id;
            }
        }
        pendingMaterials.push_back(material);
        pendingExtensions.push_back(ext);
        infos.push_back({.name = std::string(name), .material = material, .extension = ext});
        ++cacheMisses;
        return size() - 1;
    }

    [[nodiscard]] uint32_t size() const noexcept
    {
        return uploadedCount + static_cast<uint32_t>(pendingMaterials.size());
    }

    [[nodiscard]] uint32_t pendingCount() const noexcept { return static_cast<uint32_t>(pendingMaterials.size()); }

    // the GPU now owns the pending rows
    void markUploaded()
    {
        uploadedCount = size();
        dropPending();
    }

    // rows a failed load added; they never reached the GPU
    void discardPending()
    {
        dropPending();
        infos.resize(uploadedCount);
        if (uploadedCount == 0) {
            addDefaultRow();
        }
        modelFirst = size();
    }

    // rows [first, first + count) of one uploaded model; the default row 0 is never erased
    void erase(uint32_t first, uint32_t count)
    {
        assert(first >= 1 && first + count <= uploadedCount);
        infos.erase(infos.begin() + first, infos.begin() + first + count);
        uploadedCount -= count;
        modelFirst = size();
    }

private:
    // row 0, uploaded with the first model
    void addDefaultRow()
    {
        pendingMaterials.push_back(GpuMaterial{});
        pendingExtensions.push_back(MaterialPbrExtension{});
        infos.push_back({.name = "engine:default", .material = {}, .extension = {}});
        ++cacheMisses;
    }

    void dropPending()
    {
        pendingMaterials = {};
        pendingExtensions = {};
    }
};
