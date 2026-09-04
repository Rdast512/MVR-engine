#pragma once

#include "types.hpp"

#include <vector>

class MaterialStore
{
public:
    std::vector<GpuMaterial> gpuMaterials;
    std::vector<MaterialPbrExtension> pbrExtensions;

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
        return static_cast<uint32_t>(gpuMaterials.size() - 1);
    }

    [[nodiscard]] uint32_t size() const noexcept { return static_cast<uint32_t>(gpuMaterials.size()); }
};
