module sharc_update_sdk;

// Import-free Slang wrapper around vendor SharcCommon.h for SHARC_UPDATE=1. Keeping this module free of `import`
// declarations scopes `#pragma warning(disable: 41018)` strictly to vendor SharcCommon.h so sharc_update.slang keeps
// full uninitialized-out-parameter checking.

#ifndef SHARC_UPDATE
#define SHARC_UPDATE 1
#endif
#ifndef SHARC_QUERY
#define SHARC_QUERY 0
#endif
#define SHARC_ENABLE_GLSL 0
#define SHARC_MATERIAL_DEMODULATION 1
#define SHARC_SEPARATE_EMISSIVE 1
#define SHARC_ENABLE_RESPONSIVE_LIGHTING 0
#define SHARC_ENABLE_SH_ENCODING 0
#define SHARC_ENABLE_FADE_ACCELERATION 1
#define HASH_GRID_ENABLE_64_BIT_ATOMICS 1
#define RW_STRUCTURED_BUFFER(name, type) type* name
#define BUFFER_AT_OFFSET(name, offset) name[offset]

#pragma warning(push)
#pragma warning(disable: 41018)
#include "SharcCommon.h"
#pragma warning(pop)

public struct SharcUpdateParams
{
    public uint64_t hashEntries;
    public uint64_t accumulation;
    public uint64_t resolved;
    public float3 cameraPos;
    public float sceneScale;
    public float radianceScale;
    public uint capacity;
};

public struct SharcUpdateHitData
{
    public float3 positionWorld;
    public float3 normalWorld;
    public float3 materialDemodulation;
    public float3 emissive;
};

public struct SharcUpdatePath
{
    public uint cacheIndices[SHARC_PROPAGATION_DEPTH];
    public float3 sampleWeights[SHARC_PROPAGATION_DEPTH];
    public uint pathLength;
};

SharcParameters toVendorParams(SharcUpdateParams p)
{
    SharcParameters outParams;
    outParams.hashGridParameters.cameraPosition = p.cameraPos;
    outParams.hashGridParameters.logarithmBase = SHARC_GRID_LOGARITHM_BASE;
    outParams.hashGridParameters.sceneScale = p.sceneScale;
    outParams.hashGridParameters.levelBias = SHARC_GRID_LEVEL_BIAS;
    outParams.hashGridData.capacity = p.capacity;
    outParams.hashGridData.hashEntriesBuffer = (uint64_t*)p.hashEntries;
    outParams.radianceScale = p.radianceScale;
    outParams.accumulationBuffer = (SharcAccumulationData*)p.accumulation;
    outParams.resolvedBuffer = (SharcPackedData*)p.resolved;
    return outParams;
}

SharcState toVendorState(SharcUpdatePath path)
{
    SharcState state;
    for (uint i = 0; i < SHARC_PROPAGATION_DEPTH; ++i)
    {
        state.cacheIndices[i] = path.cacheIndices[i];
        state.sampleWeights[i] = path.sampleWeights[i];
    }
    state.pathLength = path.pathLength;
    return state;
}

SharcUpdatePath fromVendorState(SharcState state)
{
    SharcUpdatePath path;
    for (uint i = 0; i < SHARC_PROPAGATION_DEPTH; ++i)
    {
        path.cacheIndices[i] = state.cacheIndices[i];
        path.sampleWeights[i] = state.sampleWeights[i];
    }
    path.pathLength = state.pathLength;
    return path;
}

public SharcUpdatePath sharcUpdateInit()
{
    SharcState state;
    SharcInit(state);
    return fromVendorState(state);
}

public void sharcUpdateSetThroughput(inout SharcUpdatePath path, float3 throughput)
{
    SharcState state = toVendorState(path);
    SharcSetThroughput(state, throughput);
    path = fromVendorState(state);
}

public void sharcUpdateMiss(SharcUpdateParams p, SharcUpdatePath path, float3 radiance)
{
    SharcUpdateMiss(toVendorParams(p), toVendorState(path), radiance);
}

// Returns true to continue tracing. Sets isOverflow = true when HashGridInsertEntry failed (bucket full), distinguishing
// bucket overflow from normal SHARC_ENABLE_CACHE_RESAMPLING early path termination with zero fast-path overhead.
public bool sharcUpdateHit(SharcUpdateParams p, inout SharcUpdatePath path, SharcUpdateHitData h, float3 directLighting,
                           float random, out bool isOverflow)
{
    const SharcParameters vp = toVendorParams(p);
    SharcHitData vh;
    vh.positionWorld = h.positionWorld;
    vh.normalWorld = h.normalWorld;
    vh.materialDemodulation = h.materialDemodulation;
    vh.emissive = h.emissive;

    SharcState state = toVendorState(path);
    const uint prevPathLength = state.pathLength;
    const bool keepGoing = SharcUpdateHit(vp, state, vh, directLighting, random);
    path = fromVendorState(state);

    isOverflow = false;
    if (!keepGoing && state.pathLength == prevPathLength)
    {
        const HashGridKey key =
            HashGridComputeSpatialHash(vh.positionWorld, vh.normalWorld, vp.hashGridParameters);
        const uint baseSlot = HashGridGetBaseSlot(key, vp.hashGridData.capacity);
        HashGridIndex foundIndex;
        uint bucketOffset;
        isOverflow = !HashGridFind(vp.hashGridData, key, baseSlot, HASH_GRID_HASH_MAP_BUCKET_SIZE, foundIndex,
                                   bucketOffset);
    }
    return keepGoing;
}
