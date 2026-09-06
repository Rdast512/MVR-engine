#ifndef ENGINE_MATERIAL_BRDF_SLANG
#define ENGINE_MATERIAL_BRDF_SLANG

#include "types.slang"

// rt/pt brdf. raster preview uses material/pbr.slang sampleAlbedo.

float3 evalLambert(float3 albedo, float3 n, float3 l)
{
    float ndotl = saturate(dot(n, l));
    return albedo * ndotl / 3.14159265;
}

#endif
