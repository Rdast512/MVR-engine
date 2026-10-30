// runs core/hiz.slang hizFootprint on known spheres; tests/hiz_footprint_test.cpp checks the results
#include "core/hiz.slang"

// scalar layout: must match FootprintPush in tests/hiz_footprint_test.cpp
struct FootprintPush
{
    float4x4 viewProj;
    float4* spheres; // xyz center, w radius
    float4* results; // 2 rows per sphere: uvRect, (nearestDepth, lod, isTestable, 0)
    uint2 pyramidSize;
    uint sphereCount;
};

[[vk::push_constant]] ConstantBuffer<FootprintPush> push;

[shader("compute")]
[numthreads(64, 1, 1)]
void footprintMain(uint3 id: SV_DispatchThreadID)
{
    if (id.x >= push.sphereCount)
    {
        return;
    }
    const float4 sphere = push.spheres[id.x];
    const HiZFootprint footprint = hizFootprint(push.viewProj, sphere.xyz, sphere.w, push.pyramidSize);
    push.results[id.x * 2] = footprint.uvRect;
    push.results[id.x * 2 + 1] =
        float4(footprint.nearestDepth, footprint.lod, footprint.isTestable ? 1.0 : 0.0, 0.0);
}
