import types;
import core.binding;
import core.frame;

// ReSTIR GI duplication map (m8 O11): for each pixel, how many of its 17x17 neighbors share its final sample, read by
// next frame's temporal pass to cap history where samples have spread. RTXDI_PTComputeDuplicationMap
// (Rtxdi/PT/DuplicationMap.hlsli, RTXDI 3.1) over the sample ids passes/rtxdi_gi_shade.slang writes; only dispatched
// with kGiFlagDuplicationHistoryReduction

// RTXDI_PT_DUPMAP_GROUP_SIZE / _RADIUS / _MAX_COUNT; the group matches src/render/vk_renderer.cpp
static const int kDuplicationGroup = 16;
static const int kDuplicationRadius = 8;
static const int kDuplicationTile = kDuplicationGroup + 2 * kDuplicationRadius;
static const uint kDuplicationMaxCount = 255u;

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

// [y][x]: lane-varying x maps to different banks
groupshared uint sampleIds[kDuplicationTile][kDuplicationTile];

[shader("compute")]
[numthreads(kDuplicationGroup, kDuplicationGroup, 1)]
void rtxdiGiDuplicationMain(uint3 groupId : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    const GpuTargets targets = loadTargets(root);
    const int2 size = int2(targets.renderSize);
    const RWTexture2D<uint, 0, kImageFormatR32ui> ids = storageTex2D<uint, kImageFormatR32ui>(targets.giSampleIdsStorage);
    const int2 base = int2(groupId.xy) * kDuplicationGroup - kDuplicationRadius;
    // each lane loads a 2x2 block of the 32x32 tile; outside the frame no sample
    [unroll]
    for (int dy = 0; dy < 2; ++dy)
    {
        [unroll]
        for (int dx = 0; dx < 2; ++dx)
        {
            const int2 tile = int2(localId.xy) * 2 + int2(dx, dy);
            const int2 pixel = base + tile;
            sampleIds[tile.y][tile.x] = all(pixel >= 0) && all(pixel < size) ? ids[pixel] : 0u;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    const int2 pixel = int2(groupId.xy) * kDuplicationGroup + int2(localId.xy);
    if (any(pixel >= size))
    {
        return;
    }
    const int2 center = int2(localId.xy) + kDuplicationRadius;
    const uint ownSample = sampleIds[center.y][center.x];
    uint count = 0u;
    if (ownSample != 0u)
    {
        [unroll]
        for (int ny = -kDuplicationRadius; ny <= kDuplicationRadius; ++ny)
        {
            [unroll]
            for (int nx = -kDuplicationRadius; nx <= kDuplicationRadius; ++nx)
            {
                const bool isShared = sampleIds[center.y + ny][center.x + nx] == ownSample && (nx | ny) != 0;
                count += isShared ? 1u : 0u;
            }
        }
    }
    storageTex2D<float, kImageFormatR32f>(targets.giDuplicationStorage)[pixel] = float(min(count, kDuplicationMaxCount));
}
