#ifndef ENGINE_GI_NRC_CONSTANTS_SLANG
#define ENGINE_GI_NRC_CONSTANTS_SLANG

// textual: NrcConstants comes from the SDK's shared C/HLSL header, so this mirror cannot live in the types module.
// Matches GpuNrcConstants in src/render/vk_nrc.hpp; tests/layout_test.cpp checks it. Included through gi/nrc_bridge.slang,
// which sets the SDK's buffer-accessor macros first
#ifndef NRC_USE_CUSTOM_BUFFER_ACCESSORS
// NrcBuffers stays empty: the bridge names its own buffers (and the layout test needs no buffer type)
#define NRC_USE_CUSTOM_BUFFER_ACCESSORS 1
#endif
#include "NrcStructures.h"

// GpuNrcConstants.flags
static const uint kNrcFlagCacheView = 1u; // the query pass queries at the primary surface; the resolve is skipped

// GpuFrameRoot.cacheConstants (M4 G3): the SDK's constants and the device addresses of its buffers. Row counts are the
// SDK's allocation sizes; the bridge drops accesses past them
struct GpuNrcConstants
{
    NrcConstants nrc;
    uint64_t queryPathInfo;        // NrcPackedQueryPathInfo[frame pixels * spp]
    uint64_t trainingPathInfo;     // NrcPackedTrainingPathInfo[training pixels]
    uint64_t trainingPathVertices; // NrcPackedPathVertex[training pixels * maxPathVertices]
    uint64_t queryRadianceParams;  // NrcRadianceParams[queryRadianceCount]
    uint64_t queryRadiance;        // float3[queryRadianceCount], written by QueryAndTrain
    uint64_t counters;             // uint[NrcCounter::Count]
    uint queryPathInfoCount;
    uint trainingPathInfoCount;
    uint trainingPathVertexCount;
    uint queryRadianceCount;
    uint flags;
    uint trainingLaunch; // used training dimensions, width | height << 16: the update dispatch
    uint64_t health;     // GpuNrcHealth: queries and diverged answers, counted by passes/nrc_resolve.slang
};

// GpuNrcHealth in src/render/vk_nrc.hpp
static const uint kNrcHealthQueried = 0;
static const uint kNrcHealthDiverged = 1;
static const uint kNrcHealthMaxRatio = 2; // float bits

#endif
