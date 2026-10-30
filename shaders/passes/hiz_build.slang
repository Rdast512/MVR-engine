#include "types.slang"
#include "core/binding.slang"

// max-depth pyramid (standard Z: a texel keeps the farthest depth of its footprint), one dispatch per mip

static const uint kHiZGroupSize = 8; // matches src/render/vk_hiz.cpp

[[vk::push_constant]] ConstantBuffer<HiZPushData> push;

// the pyramid is the previous power of two of the depth size, so a texel spans 1 to 3 depth texels per axis
[shader("compute")]
[numthreads(kHiZGroupSize, kHiZGroupSize, 1)]
void hizFromDepth(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= push.dstSize))
    {
        return;
    }
    const Texture2D depth = tex2D(push.srcImage);
    // [floor(i * src / dst), ceil((i + 1) * src / dst))
    const uint2 first = id.xy * push.srcSize / push.dstSize;
    const uint2 last = ((id.xy + 1) * push.srcSize + push.dstSize - 1) / push.dstSize;
    float farthest = 0.0;
    for (uint y = first.y; y < last.y; ++y)
    {
        for (uint x = first.x; x < last.x; ++x)
        {
            farthest = max(farthest, depth.Load(int3(x, y, 0)).r);
        }
    }
    storageTex2D(push.dstImage)[id.xy] = farthest;
}

// sizes halve exactly (or stay 1), so one max-reduction fetch at the 2x2 block's shared corner covers it
[shader("compute")]
[numthreads(kHiZGroupSize, kHiZGroupSize, 1)]
void hizDownsample(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= push.dstSize))
    {
        return;
    }
    const float2 uv = (float2(id.xy) + 0.5) / float2(push.dstSize);
    const float farthest =
        tex2D(push.srcImage).SampleLevel(samplerOf(push.maxSampler), uv, float(push.srcMip)).r;
    storageTex2D(push.dstImage)[id.xy] = farthest;
}
