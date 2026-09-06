#pragma once

#include "types.hpp"

#include <vector>

class MaterialStore
{
public:
    // CPU cache of GPU material rows. ids are indices into these vectors / the GPU SSBO.
    std::vector<GpuMaterial> gpuMaterials;
    std::vector<MaterialPbrExtension> pbrExtensions;
    uint32_t uploadedCount = 0;
    uint32_t cacheHits = 0;
    uint32_t cacheMisses = 0;

    MaterialStore()
    {
        gpuMaterials.push_back(GpuMaterial{});
        pbrExtensions.push_back(MaterialPbrExtension{});
        ++cacheMisses;
    }

    [[nodiscard]] uint32_t defaultMaterialId() const noexcept { return 0; }

    uint32_t add(const GpuMaterial& material, const MaterialPbrExtension& ext = {})
    {
        for (uint32_t i = 0; i < gpuMaterials.size(); ++i) {
            if (gpuMaterials[i] == material && pbrExtensions[i] == ext) {
                ++cacheHits;
                return i;
            }
        }
        gpuMaterials.push_back(material);
        pbrExtensions.push_back(ext);
        ++cacheMisses;
        return static_cast<uint32_t>(gpuMaterials.size() - 1);
    }

    [[nodiscard]] uint32_t size() const noexcept { return static_cast<uint32_t>(gpuMaterials.size()); }

    [[nodiscard]] uint32_t pendingCount() const noexcept
    {
        return size() > uploadedCount ? size() - uploadedCount : 0;
    }

    [[nodiscard]] const GpuMaterial* get(uint32_t id) const noexcept
    {
        if (id >= gpuMaterials.size()) {
            return nullptr;
        }
        return &gpuMaterials[id];
    }

    [[nodiscard]] const GpuMaterial* scratchMaterial(uint32_t id) const noexcept { return get(id); }

    void markUploaded() noexcept { uploadedCount = size(); }
};
