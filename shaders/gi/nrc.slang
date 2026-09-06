#ifndef ENGINE_GI_NRC_SLANG
#define ENGINE_GI_NRC_SLANG

#include "types.slang"
#include "gi/rtxgi.slang"

struct NrcTrainRecord
{
    float3 radiance;
    float3 throughput;
    float2 uv;
};

#endif
