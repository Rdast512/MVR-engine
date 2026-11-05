// runs core/culling.slang on known spheres, through both the sphere and the box path; tests/hiz_footprint_test.cpp
// checks the results
#include "core/culling.slang"

// scalar layout: must match FootprintPush in tests/hiz_footprint_test.cpp
struct FootprintPush
{
    float4x4 viewProj;
    float4* spheres; // xyz center, w radius
    float4* results; // kResultRows per sphere, see writeBounds
    uint2 pyramidSize; // also the viewport of the sub-pixel test
    uint sphereCount;
    float subpixelSnap;
};

static const uint kResultRows = 4;

[[vk::push_constant]] ConstantBuffer<FootprintPush> push;

// uvRect, then (nearest depth, mip, isValid, isSubPixel)
void writeBounds(uint row, ClipBounds bounds)
{
    const float4 uvRect = clipUvRect(bounds);
    push.results[row] = uvRect;
    push.results[row + 1] = float4(clipNearestDepth(bounds), hizMip(uvRect, push.pyramidSize),
                                   bounds.isValid ? 1.0 : 0.0,
                                   isSubPixel(bounds, float2(push.pyramidSize), push.subpixelSnap) ? 1.0 : 0.0);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void footprintMain(uint3 id: SV_DispatchThreadID)
{
    if (id.x >= push.sphereCount)
    {
        return;
    }
    const float4 sphere = push.spheres[id.x];

    ClipBounds sphereBounds;
    intersectFrustumSphere(push.viewProj, sphere.xyz, sphere.w, sphereBounds);
    writeBounds(id.x * kResultRows, sphereBounds);

    // the sphere's world AABB as an object box under an identity world: must give the same bounds
    const float4x4 identity = float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    ClipBounds boxBounds;
    intersectFrustum(push.viewProj, sphere.xyz - sphere.w, sphere.xyz + sphere.w, identity, boxBounds);
    writeBounds(id.x * kResultRows + 2, boxBounds);
}
