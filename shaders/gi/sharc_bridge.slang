#ifndef ENGINE_GI_SHARC_BRIDGE_SLANG
#define ENGINE_GI_SHARC_BRIDGE_SLANG

// SHaRC v1.8.3 shader bridge (M8.4, spec/m8/m8_4_sharc_cache.md): pure Vulkan 1.4 BDA + native 64-bit atomics.
// Vendor SharcCommon.h is isolated in import-free modules gi.sharc_sdk / gi.sharc_update_sdk so warning 41018 remains
// enabled for all engine code. Emissive is always kept separate so vertex 0 never violates the one-emission rule.

import core.binding;
import core.frame;
import core.math;
#ifdef SHARC_UPDATE
import gi.sharc_update_sdk;
#else
import gi.sharc_sdk;
#endif

#include "gi/sharc_constants.slang"

GpuSharcConstants loadSharcConstants(GpuFrameRoot root)
{
    return bdaPtr<GpuSharcConstants>(root.sharcConstants)[0];
}

// RTXPT PathTracerSHaRC.hlsli:232-233 & RTXGI SharcIndirectDiffuse.hlsli:50-51 demodulation factor
float3 sharcMaterialDemodulation(float3 diffuseAlbedo, float3 specularF0)
{
    const float3 specularFAvg = specularF0 + (1.0 - specularF0) * (1.0 / 21.0);
    return max(diffuseAlbedo, 0.05) + max(specularF0, 0.02) * luminance(specularFAvg);
}

#ifdef SHARC_UPDATE
SharcUpdateParams sharcUpdateParamsFrom(GpuSharcConstants c)
{
    SharcUpdateParams params;
    params.hashEntries = c.hashEntries;
    params.accumulation = c.accumulation;
    params.resolved = c.resolved;
    params.cameraPos = c.cameraPos;
    params.sceneScale = c.sceneScale;
    params.radianceScale = c.radianceScale;
    params.capacity = c.capacity;
    return params;
}

SharcUpdateHitData sharcMakeUpdateHitData(GpuSharcConstants c, float3 positionWorld, float3 geoNormal,
                                          float3 diffuseAlbedo, float3 specularF0, float3 emissive)
{
    SharcUpdateHitData hitData;
    hitData.positionWorld = positionWorld;
    hitData.normalWorld = geoNormal;
    hitData.materialDemodulation = (c.flags & kSharcFlagMaterialDemodulation) != 0
                                       ? sharcMaterialDemodulation(diffuseAlbedo, specularF0)
                                       : float3(1.0, 1.0, 1.0);
    hitData.emissive = emissive;
    return hitData;
}
#else
SharcSdkParams sharcSdkParamsFrom(GpuSharcConstants c)
{
    SharcSdkParams params;
    params.hashEntries = c.hashEntries;
    params.accumulation = c.accumulation;
    params.resolved = c.resolved;
    params.cameraPos = c.cameraPos;
    params.sceneScale = c.sceneScale;
    params.radianceScale = c.radianceScale;
    params.capacity = c.capacity;
    return params;
}

SharcSdkHit sharcMakeHitData(GpuSharcConstants c, float3 positionWorld, float3 geoNormal, float3 diffuseAlbedo,
                             float3 specularF0, float3 emissive)
{
    SharcSdkHit hitData;
    hitData.positionWorld = positionWorld;
    hitData.normalWorld = geoNormal;
    hitData.materialDemodulation = (c.flags & kSharcFlagMaterialDemodulation) != 0
                                       ? sharcMaterialDemodulation(diffuseAlbedo, specularF0)
                                       : float3(1.0, 1.0, 1.0);
    hitData.emissive = emissive;
    return hitData;
}
#endif

bool sharcIsFiniteFloat3(float3 value)
{
    return !any(isnan(value)) && !any(isinf(value));
}

#endif
