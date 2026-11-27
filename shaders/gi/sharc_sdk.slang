module sharc_sdk;

// Import-free Slang wrapper around vendor SharcCommon.h (query + resolve + debug). Keeping this module free of
// `import` declarations lets `#pragma warning(disable: 41018)` scope strictly to vendor SharcCommon.h so engine code in
// indirect.slang, debug_view.slang and sharc_resolve.slang retains full uninitialized-out-parameter checking.

#define SHARC_UPDATE 0
#define SHARC_QUERY 1
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

public struct SharcSdkParams
{
    public uint64_t hashEntries;
    public uint64_t accumulation;
    public uint64_t resolved;
    public float3 cameraPos;
    public float sceneScale;
    public float radianceScale;
    public uint capacity;
};

public struct SharcSdkHit
{
    public float3 positionWorld;
    public float3 normalWorld;
    public float3 materialDemodulation;
    public float3 emissive;
};

SharcParameters toVendorParams(SharcSdkParams p)
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

SharcHitData toVendorHit(SharcSdkHit h)
{
    SharcHitData outHit;
    outHit.positionWorld = h.positionWorld;
    outHit.normalWorld = h.normalWorld;
    outHit.materialDemodulation = h.materialDemodulation;
    outHit.emissive = h.emissive;
    return outHit;
}

public bool sharcQueryCachedRadiance(SharcSdkParams p, SharcSdkHit h, out float3 radiance)
{
    radiance = float3(0.0, 0.0, 0.0);
    return SharcGetCachedRadiance(toVendorParams(p), toVendorHit(h), radiance, false);
}

// RTXGI SharcIndirectDiffuse.hlsli:65-90 & SHaRC Integration.md:145 query eligibility test
public bool sharcCanQueryVoxel(SharcSdkParams p, float3 hitPositionWorld, float segmentLength, bool isSpecularLobe,
                               float launchRoughness)
{
    const SharcParameters vp = toVendorParams(p);
    const uint gridLevel = HashGridGetLevel(hitPositionWorld, vp.hashGridParameters);
    const float voxelSize = HashGridGetVoxelSize(gridLevel, vp.hashGridParameters);
    if (segmentLength <= voxelSize * 1.73205081)
    {
        return false;
    }
    if (!isSpecularLobe)
    {
        return true;
    }
    const float alpha = min(launchRoughness, 0.99) * min(launchRoughness, 0.99);
    const float alpha2 = alpha * alpha;
    const float footprint = 2.0 * segmentLength * sqrt(0.5 * alpha2 / max(1.0 - alpha2, 1e-6));
    return footprint > voxelSize;
}

public bool sharcResolveVoxel(uint entryIndex, SharcSdkParams p, float3 prevCameraPos, uint accumulationFrameNum,
                              uint staleFrameNumMax, uint frameIndex)
{
    if (entryIndex >= p.capacity)
    {
        return false;
    }
    const SharcParameters vp = toVendorParams(p);
    SharcResolveParameters resolveParams;
    resolveParams.cameraPositionPrev = prevCameraPos;
    resolveParams.accumulationFrameNum = accumulationFrameNum;
    resolveParams.responsiveFrameNum = 1u;
    resolveParams.staleFrameNumMax = staleFrameNumMax;
    resolveParams.frameIndex = frameIndex;
    SharcResolveEntry(entryIndex, vp, resolveParams);
    return vp.hashGridData.hashEntriesBuffer[entryIndex] != HASH_GRID_INVALID_HASH_KEY;
}

public float3 sharcDebugOccupancy(uint2 pixel, uint2 renderSize, SharcSdkParams p)
{
    return HashGridDebugOccupancy(pixel, renderSize, toVendorParams(p).hashGridData);
}

public float3 sharcDebugColoredHash(float3 pos, float3 normal, SharcSdkParams p)
{
    return HashGridDebugColoredHash(pos, normal, toVendorParams(p).hashGridParameters);
}

public float3 sharcDebugHashCollisions(float3 pos, float3 normal, SharcSdkParams p)
{
    const SharcParameters vp = toVendorParams(p);
    return HashGridDebugHashCollisions(pos, normal, vp.hashGridParameters, vp.hashGridData);
}

public float sharcGetVoxelSize(float3 pos, SharcSdkParams p)
{
    const SharcParameters vp = toVendorParams(p);
    return HashGridGetVoxelSize(HashGridGetLevel(pos, vp.hashGridParameters), vp.hashGridParameters);
}

public uint sharcGetGridLevel(float3 pos, SharcSdkParams p)
{
    return HashGridGetLevel(pos, toVendorParams(p).hashGridParameters);
}
