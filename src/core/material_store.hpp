#pragma once

#include "types.hpp"

#include <vector>

class MaterialStore
{
public:
    // load scratch; cleared after GPU append. ids are uploadedCount + scratch index
    std::vector<GpuMaterial> gpuMaterials;
    std::vector<MaterialPbrExtension> pbrExtensions;
    uint32_t uploadedCount = 0;

    MaterialStore()
    {
        gpuMaterials.push_back(GpuMaterial{});
        pbrExtensions.push_back(MaterialPbrExtension{});
    }

    [[nodiscard]] uint32_t defaultMaterialId() const noexcept { return 0; }

    uint32_t add(const GpuMaterial& material, const MaterialPbrExtension& ext = {})
    {
        gpuMaterials.push_back(material);
        pbrExtensions.push_back(ext);
        return uploadedCount + static_cast<uint32_t>(gpuMaterials.size() - 1);
    }

    [[nodiscard]] uint32_t size() const noexcept
    {
        return uploadedCount + static_cast<uint32_t>(gpuMaterials.size());
    }

    [[nodiscard]] const GpuMaterial* scratchMaterial(uint32_t id) const noexcept
    {
        if (id < uploadedCount) {
            return nullptr;
        }
        const uint32_t i = id - uploadedCount;
        if (i >= gpuMaterials.size()) {
            return nullptr;
        }
        return &gpuMaterials[i];
    }

    void clearScratch()
    {
        gpuMaterials.clear();
        pbrExtensions.clear();
        gpuMaterials.shrink_to_fit();
        pbrExtensions.shrink_to_fit();
    }
};
