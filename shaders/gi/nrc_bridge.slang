#ifndef ENGINE_GI_NRC_BRIDGE_SLANG
#define ENGINE_GI_NRC_BRIDGE_SLANG

// NRC shader bridge (M4 G3, spec/m4/g3_nrc.md): the pinned NRC 0.16 HLSL API over the SDK's buffers through BDA.
// Textual, once per compile unit. Path passes compile with -DNRC_UPDATE=1 or -DNRC_QUERY=1; without either the NRC
// path functions are stubs and only the structures and helpers serve (resolve, debug view). Vendor headers are not
// edited: the SDK's accessor hooks are macros set here.
//
// Call nrcBridgeInit before the first NRC call

import core.binding;
import core.frame;

// NrcBuffers stays empty (gi/nrc_constants.slang sets NRC_USE_CUSTOM_BUFFER_ACCESSORS): the macros below name the
// bridge state instead of buffers inside the context. The counter is passed by parameter and needs an atomic lvalue: a plain pointer (two rows)
#define NRC_RW_STRUCTURED_BUFFER(T) T*
// NRC reads its buffers as structured buffers under robust buffer access; the others are bounds-checked
#define NRC_BUFFER_QUERY_PATH_INFO gNrcBridge.queryPathInfo
#define NRC_BUFFER_TRAINING_PATH_INFO gNrcBridge.trainingPathInfo
#define NRC_BUFFER_TRAINING_PATH_VERTICES gNrcBridge.trainingPathVertices
#define NRC_BUFFER_QUERY_RADIANCE_PARAMS gNrcBridge.queryRadianceParams
#define NRC_BUFFER_QUERY_COUNTERS_DATA gNrcBridge.counters

#include "gi/nrc_constants.slang"

struct NrcBridgeState
{
    CheckedBuffer<NrcPackedQueryPathInfo> queryPathInfo;
    CheckedBuffer<NrcPackedTrainingPathInfo> trainingPathInfo;
    CheckedBuffer<NrcPackedPathVertex> trainingPathVertices;
    CheckedBuffer<NrcRadianceParams> queryRadianceParams;
    uint* counters;
    // false until the host configured the cache (zero constants): no query may be created, it would count through a
    // null counter
    bool isActive;
};

static NrcBridgeState gNrcBridge;

#if defined(NRC_UPDATE) || defined(NRC_QUERY)
#include "Nrc.hlsli"
#else
// no path mode: Nrc.hlsli's disabled stubs return uninitialized values (E41016); the helpers are all a reader needs
#include "NrcHelpers.hlsli"
#endif

GpuNrcConstants loadNrcConstants(GpuFrameRoot root)
{
    return bdaPtr<GpuNrcConstants>(root.cacheConstants)[0];
}

// fills the bridge state; returns the SDK's constants for NrcCreateContext
NrcConstants nrcBridgeInit(GpuFrameRoot root)
{
    const GpuNrcConstants constants = loadNrcConstants(root);
    gNrcBridge.queryPathInfo.address = constants.queryPathInfo;
    gNrcBridge.queryPathInfo.count = constants.queryPathInfoCount;
    gNrcBridge.trainingPathInfo.address = constants.trainingPathInfo;
    gNrcBridge.trainingPathInfo.count = constants.trainingPathInfoCount;
    gNrcBridge.trainingPathVertices.address = constants.trainingPathVertices;
    gNrcBridge.trainingPathVertices.count = constants.trainingPathVertexCount;
    gNrcBridge.queryRadianceParams.address = constants.queryRadianceParams;
    gNrcBridge.queryRadianceParams.count = constants.queryRadianceCount;
    gNrcBridge.counters = bdaPtr<uint>(constants.counters);
    gNrcBridge.isActive = constants.counters != 0;
    return constants.nrc;
}

struct NrcQueryAnswer
{
    float3 radiance;         // predicted radiance at the query vertex, unclamped
    float3 prefixThroughput;
};

// the cache's answer for a pixel's query path; false without a query
bool nrcLoadQueryAnswer(GpuNrcConstants constants, uint2 pixel, out NrcQueryAnswer answer)
{
    answer.radiance = float3(0.0, 0.0, 0.0);
    answer.prefixThroughput = float3(0.0, 0.0, 0.0);
    const uint pathIndex = NrcCalculateQueryPathIndex(constants.nrc.frameDimensions, pixel, 0, 1);
    if (pathIndex >= constants.queryPathInfoCount)
    {
        return false;
    }
    const NrcQueryPathInfo path = NrcUnpackQueryPathInfo(bdaPtr<NrcPackedQueryPathInfo>(constants.queryPathInfo)[pathIndex]);
    if (path.queryBufferIndex >= constants.queryRadianceCount)
    {
        return false;
    }
    answer.radiance =
        NrcUnpackQueryRadiance(constants.nrc, bdaPtr<float3>(constants.queryRadiance)[path.queryBufferIndex]);
    answer.prefixThroughput = path.prefixThroughput;
    return true;
}

bool isFiniteFloat3(float3 value)
{
    return !any(isnan(value)) && !any(isinf(value));
}

bool isNrcAnswerFinite(NrcQueryAnswer answer)
{
    return isFiniteFloat3(answer.radiance) && isFiniteFloat3(answer.prefixThroughput) &&
           isFiniteFloat3(answer.radiance * answer.prefixThroughput);
}

// prefix throughput x predicted radiance, zero without a query or when non-finite. An untrained network predicts
// negative radiance (measured on load), which the reservoirs must not take
float3 nrcPathRadiance(NrcQueryAnswer answer)
{
    return isNrcAnswerFinite(answer) ? max(answer.radiance * answer.prefixThroughput, 0.0) : float3(0.0, 0.0, 0.0);
}

float3 nrcQueryResult(GpuNrcConstants constants, uint2 pixel)
{
    NrcQueryAnswer answer;
    return nrcLoadQueryAnswer(constants, pixel, answer) ? nrcPathRadiance(answer) : float3(0.0, 0.0, 0.0);
}

// what NRC knows about a surface; no material is a delta lobe (material.brdf clamps alpha >= kMinAlpha)
NrcSurfaceAttributes nrcSurfaceAttributes(NrcConstants constants, float3 position, float3 shadingNormal,
                                          float3 viewDir, float roughness, float3 diffuseAlbedo, float3 specularF0)
{
    NrcSurfaceAttributes attributes;
    attributes.encodedPosition = NrcEncodePosition(position, constants);
    attributes.roughness = roughness;
    attributes.specularF0 = specularF0;
    attributes.diffuseReflectance = diffuseAlbedo;
    attributes.shadingNormal = shadingNormal;
    attributes.viewVector = viewDir;
    attributes.isDeltaLobe = false;
    return attributes;
}

#endif
