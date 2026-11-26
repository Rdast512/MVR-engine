import types;
import core.binding;
import core.frame;
import geometry.visbuffer;
import material.pbr;

// material binning (spec/m7/e1_material_binning.md, engine spec §9.1): a kMaterialBin* per pixel into binIds and
// each resolve tile appended once to the list of every bin it holds; binArgsMain then writes one resolve DGC
// sequence per non-empty bin

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

// bit per kMaterialBin* present in this group's tile
groupshared uint tileBins;

// every bin's list holds one entry per render tile, so no append can overflow
uint tileCapacity(uint2 renderSize)
{
    const uint2 tiles = (renderSize + kMaterialTileSize - 1) / kMaterialTileSize;
    return tiles.x * tiles.y;
}

// the lists follow the GpuMaterialBins head, one after another in kMaterialBin* order
uint* tileList(GpuMaterialBins* bins, uint2 renderSize, uint bin)
{
    return (uint*)(bins + 1) + bin * tileCapacity(renderSize);
}

// payload chain visibleRow -> meshletRow -> primitive -> material -> bin; depth 0 is Background
uint pixelBin(GpuFrameRoot root, GpuTargets targets, uint2 pixel)
{
    // reverse Z: the clear value 0 is empty sky; the payload of an empty pixel is undefined
    if (tex2D(targets.depth).Load(int3(pixel, 0)).r == 0.0)
    {
        return kMaterialBinBackground;
    }
    const uint payload = uintTex2D(targets.visBuffer).Load(int3(pixel, 0));
    const DrawContext draw = loadDrawContext(root, visPayloadRow(payload));
    return materialBin(bdaPtr<GpuMaterial>(root.materials)[materialRow(draw.primitive)].flags);
}

[shader("compute")]
[numthreads(kMaterialTileSize, kMaterialTileSize, 1)]
void classifyMain(uint3 id : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint index : SV_GroupIndex)
{
    if (index == 0)
    {
        tileBins = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    const GpuTargets targets = loadTargets(root);
    uint binBit = 0;
    if (all(id.xy < targets.renderSize))
    {
        const uint bin = pixelBin(root, targets, id.xy);
        storageTex2D<uint, kImageFormatR8ui>(targets.binIdsStorage)[id.xy] = bin;
        binBit = 1u << bin;
    }
    // one groupshared atomic per wave, any wave size
    const uint waveBins = WaveActiveBitOr(binBit);
    if (WaveIsFirstLane())
    {
        InterlockedOr(tileBins, waveBins);
    }
    GroupMemoryBarrierWithGroupSync();

    // one append per (tile, bin), by the lane of that bin
    if (index < kMaterialBinCount && (tileBins & (1u << index)) != 0)
    {
        GpuMaterialBins* bins = bdaPtr<GpuMaterialBins>(root.materialBins);
        uint slot;
        InterlockedAdd(bins.tileCount[index], 1u, slot);
        tileList(bins, targets.renderSize, index)[slot] = groupId.x | (groupId.y << 16);
    }
}

// one group, one thread: kMaterialBinCount sequences at most, in bin order
ResolveDgcSequence binSequence(GpuMaterialBins* bins, uint2 renderSize, uint bin, uint tileCount)
{
    ResolveDgcSequence sequence;
    sequence.executionSetIndex = bin;
    sequence.pad0 = 0;
    sequence.push.frameRoot = push.frameRoot;
    sequence.push.tileList = (uint64_t)tileList(bins, renderSize, bin);
    sequence.groupCount = uint3(tileCount, 1, 1);
    sequence.pad1 = 0;
    return sequence;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void binArgsMain()
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    const uint2 renderSize = loadTargets(root).renderSize;
    GpuMaterialBins* bins = bdaPtr<GpuMaterialBins>(root.materialBins);
    uint sequenceCount = 0;
    for (uint bin = 0; bin < kMaterialBinCount; ++bin)
    {
        const uint tileCount = bins.tileCount[bin];
        if (tileCount == 0)
        {
            continue;
        }
        bins.sequences[sequenceCount] = binSequence(bins, renderSize, bin, tileCount);
        ++sequenceCount;
    }
    bins.sequenceCount = sequenceCount;
}

// no DGC, or GPU-AV (considerations C-E1-3, ENGINE_GPUAV_RESOLVE_PER_BIN): sequence slot = bin, 0 groups when
// empty, for one vkCmdDispatchIndirect per bin instead of the DGC execute
[shader("compute")]
[numthreads(1, 1, 1)]
void binArgsPerBinMain()
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    const uint2 renderSize = loadTargets(root).renderSize;
    GpuMaterialBins* bins = bdaPtr<GpuMaterialBins>(root.materialBins);
    for (uint bin = 0; bin < kMaterialBinCount; ++bin)
    {
        bins.sequences[bin] = binSequence(bins, renderSize, bin, bins.tileCount[bin]);
    }
    bins.sequenceCount = kMaterialBinCount;
}
