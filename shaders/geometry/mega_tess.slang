#ifndef ENGINE_GEOMETRY_MEGA_TESS_SLANG
#define ENGINE_GEOMETRY_MEGA_TESS_SLANG

#include "types.slang"
#include "geometry/mega_geometry.slang"

struct TessTile
{
    uint clusterId;
    uint lod;
    float2 uv0;
    float2 uv1;
};

#endif
