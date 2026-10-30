#ifndef ENGINE_CORE_HIZ_SLANG
#define ENGINE_CORE_HIZ_SLANG

#include "core/binding.slang"

// hi-z occlusion of a world-space sphere. standard Z (near 0, far 1); the pyramid keeps the farthest depth

struct HiZFootprint
{
    float4 uvRect; // min.xy, max.xy, clamped to [0, 1]
    float nearestDepth;
    float lod; // finest mip where the rect spans at most 2 texels per axis
    bool isTestable; // false: crosses the near plane or sits behind the camera, so always visible
};

// the sphere's world AABB corners bound both its screen rect and its nearest depth, which keeps the test conservative
HiZFootprint hizFootprint(float4x4 viewProj, float3 center, float radius, uint2 pyramidSize)
{
    const float4 clipCenter = mul(viewProj, float4(center, 1.0));
    // clip(center + r * e_i) = clipCenter + r * column i
    const float4 dx = radius * float4(viewProj[0][0], viewProj[1][0], viewProj[2][0], viewProj[3][0]);
    const float4 dy = radius * float4(viewProj[0][1], viewProj[1][1], viewProj[2][1], viewProj[3][1]);
    const float4 dz = radius * float4(viewProj[0][2], viewProj[1][2], viewProj[2][2], viewProj[3][2]);

    HiZFootprint footprint;
    footprint.isTestable = true;
    float2 ndcMin = float2(1.0, 1.0);
    float2 ndcMax = float2(-1.0, -1.0);
    footprint.nearestDepth = 1.0;
    [unroll]
    for (uint i = 0; i < 8; ++i)
    {
        const float4 corner = clipCenter + ((i & 1) != 0 ? dx : -dx) + ((i & 2) != 0 ? dy : -dy) +
                              ((i & 4) != 0 ? dz : -dz);
        // z < 0 is in front of the near plane; w <= 0 is behind the eye
        footprint.isTestable = footprint.isTestable && corner.w > 0.0 && corner.z >= 0.0;
        const float3 ndc = corner.xyz / corner.w;
        ndcMin = min(ndcMin, ndc.xy);
        ndcMax = max(ndcMax, ndc.xy);
        footprint.nearestDepth = min(footprint.nearestDepth, ndc.z);
    }

    footprint.uvRect = saturate(float4(ndcMin, ndcMax) * 0.5 + 0.5);
    const float2 extent = (footprint.uvRect.zw - footprint.uvRect.xy) * float2(pyramidSize);
    // extent <= 2 texels spans at most 3, which the four 2x2 max footprints at the corners cover
    footprint.lod = max(ceil(log2(max(extent.x, extent.y))) - 1.0, 0.0);
    return footprint;
}

// samples past the last mip clamp to the 1x1 level, which covers the whole screen
bool isHiZOccluded(HiZFootprint footprint, uint pyramidIndex, uint maxSamplerIndex)
{
    if (!footprint.isTestable)
    {
        return false;
    }
    const Texture2D pyramid = tex2D(pyramidIndex);
    const SamplerState maxSampler = samplerOf(maxSamplerIndex);
    const float4 rect = footprint.uvRect;
    const float farthest = max(max(pyramid.SampleLevel(maxSampler, rect.xy, footprint.lod).r,
                                   pyramid.SampleLevel(maxSampler, rect.zy, footprint.lod).r),
                               max(pyramid.SampleLevel(maxSampler, rect.xw, footprint.lod).r,
                                   pyramid.SampleLevel(maxSampler, rect.zw, footprint.lod).r));
    return footprint.nearestDepth > farthest;
}

#endif
