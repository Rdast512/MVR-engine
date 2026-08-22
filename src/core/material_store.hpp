#pragma once

#include "types.hpp"

#include <vector>

class MaterialStore
{
public:
    std::vector<GpuMaterial> gpuMaterials;

    MaterialStore() { gpuMaterials.push_back(GpuMaterial{}); }

    [[nodiscard]] uint32_t defaultMaterialId() const noexcept { return 0; }

    uint32_t add(const GpuMaterial& material)
    {
        gpuMaterials.push_back(material);
        return static_cast<uint32_t>(gpuMaterials.size() - 1);
    }

    [[nodiscard]] uint32_t size() const noexcept { return static_cast<uint32_t>(gpuMaterials.size()); }
};
