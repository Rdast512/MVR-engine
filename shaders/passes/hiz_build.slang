/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Level grouping after nvpro-samples/vk_lod_clusters shaders/nvhiz-update.comp.glsl, modified for this engine:
 * - groupshared reductions instead of subgroupShuffle, so no wave size is assumed
 * - an 8x8 tile of the first level per workgroup, its 4x4 and 2x2 levels from groupshared
 * - level 0 reduces the 1 to 3 depth texels per axis of the previous-power-of-two pyramid; later levels halve exactly
 * - far pyramid only (NV_HIZ_OUTPUT_NEAR 0), reverse Z (NV_HIZ_REVERSED_Z 1) from core/culling.slang
 * License text: ThirdParty/licenses/nvpro-samples-vk_lod_clusters-Apache-2.0.txt
 */
import types;
import core.binding;
import core.culling;

// far-depth pyramid: a texel keeps the farthest depth of its footprint, up to kHiZLevelsPerDispatch mips per dispatch

static const uint kHiZTile = 8; // matches src/render/vk_hiz.cpp kGroupSize

[[vk::push_constant]] ConstantBuffer<HiZPushData> push;

groupshared float level0[kHiZTile][kHiZTile];
groupshared float level1[kHiZTile / 2][kHiZTile / 2];

float farthest4(float a, float b, float c, float d)
{
    return farthestDepth(farthestDepth(a, b), farthestDepth(c, d));
}

// pyramid is the previous power of two of the depth size: [floor(i * src / dst), ceil((i + 1) * src / dst))
float reduceDepthFootprint(uint2 texel)
{
    const Texture2D depth = tex2D(push.srcImage);
    const uint2 first = texel * push.srcSize / push.dstSize;
    const uint2 last = ((texel + 1) * push.srcSize + push.dstSize - 1) / push.dstSize;
    float farthest = kNearestDepth;
    for (uint y = first.y; y < last.y; ++y)
    {
        for (uint x = first.x; x < last.x; ++x)
        {
            farthest = farthestDepth(farthest, depth.Load(int3(x, y, 0)).r);
        }
    }
    return farthest;
}

// sizes halve exactly or stay 1; clamping repeats the edge, which leaves the reduction unchanged
float reduceMip(uint2 texel)
{
    const Texture2D pyramid = tex2D(push.srcImage);
    const int2 last = int2(push.srcSize) - 1;
    const int2 base = int2(texel * 2);
    const int mip = int(push.srcMip);
    return farthest4(pyramid.Load(int3(min(base, last), mip)).r,
                     pyramid.Load(int3(min(base + int2(1, 0), last), mip)).r,
                     pyramid.Load(int3(min(base + int2(0, 1), last), mip)).r,
                     pyramid.Load(int3(min(base + int2(1, 1), last), mip)).r);
}

void storeLevel(uint level, uint2 texel, float value)
{
    const uint2 size = max(push.dstSize >> level, uint2(1, 1));
    if (level < push.levelCount && all(texel < size))
    {
        // HiZPyramid kFormat
        storageTex2D<float, kImageFormatR32f>(push.dstImages[level])[texel] = value;
    }
}

// texels past the image hold the clamped edge value, so the next level reads a full tile; barriers stay uniform
[shader("compute")]
[numthreads(kHiZTile, kHiZTile, 1)]
void hizBuild(uint3 groupId : SV_GroupID, uint3 local : SV_GroupThreadID)
{
    const uint2 texel = groupId.xy * kHiZTile + local.xy;
    const uint2 clamped = min(texel, push.dstSize - 1);
    const float value = push.isFromDepth != 0 ? reduceDepthFootprint(clamped) : reduceMip(clamped);
    storeLevel(0, texel, value);
    level0[local.y][local.x] = value;
    GroupMemoryBarrierWithGroupSync();

    const uint2 half = local.xy * 2;
    if (all(local.xy < kHiZTile / 2))
    {
        const float value1 = farthest4(level0[half.y][half.x], level0[half.y][half.x + 1],
                                       level0[half.y + 1][half.x], level0[half.y + 1][half.x + 1]);
        storeLevel(1, groupId.xy * (kHiZTile / 2) + local.xy, value1);
        level1[local.y][local.x] = value1;
    }
    GroupMemoryBarrierWithGroupSync();

    if (all(local.xy < kHiZTile / 4))
    {
        const float value2 = farthest4(level1[half.y][half.x], level1[half.y][half.x + 1],
                                       level1[half.y + 1][half.x], level1[half.y + 1][half.x + 1]);
        storeLevel(2, groupId.xy * (kHiZTile / 4) + local.xy, value2);
    }
}
