#ifndef ENGINE_POST_GBUFFER_SLANG
#define ENGINE_POST_GBUFFER_SLANG

#include "types.slang"

struct GbufferGuides
{
    float3 color;
    float depth;
    float2 motion;
    float3 albedo;
    float3 specAlbedo;
    float3 normal;
    float roughness;
    float hitT;
};

#endif
