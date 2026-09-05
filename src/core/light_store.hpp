#pragma once

#include "types.hpp"

#include <vector>

class LightStore
{
public:
    // load scratch; cleared after GPU append
    std::vector<LightDef> defs;
    std::vector<LightInstance> instances;
    uint32_t uploadedCount = 0;

    uint32_t addDef(const LightDef& def)
    {
        defs.push_back(def);
        return static_cast<uint32_t>(defs.size() - 1);
    }

    uint32_t addInstance(const LightInstance& instance)
    {
        instances.push_back(instance);
        return static_cast<uint32_t>(instances.size() - 1);
    }

    void clearScratch()
    {
        defs.clear();
        instances.clear();
        defs.shrink_to_fit();
        instances.shrink_to_fit();
    }
};
