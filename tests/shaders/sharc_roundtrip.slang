// SHaRC update -> resolve -> query over BDA pointers, matching the engine binding model.
// built once per pass: SHARC_UPDATE=1 (updateMain), SHARC_QUERY=1 (queryMain), neither (resolveMain)
#ifndef SHARC_UPDATE
#define SHARC_UPDATE 0
#endif
#ifndef SHARC_QUERY
#define SHARC_QUERY 0
#endif
#define SHARC_ENABLE_GLSL 0
#define SHARC_ENABLE_RESPONSIVE_LIGHTING 0
#define RW_STRUCTURED_BUFFER(name, type) type* name
#include "SharcCommon.h"

// scalar layout: must match SharcPush in tests/sharc_test.cpp
struct SharcPush
{
    uint64_t* hashEntries;
    SharcAccumulationData* accumulation;
    SharcPackedData* resolved;
    float4* results; // query: xyz radiance, w = 1 if cached
    float3 cameraPosition;
    uint capacity;
    uint probeCount;
};

[[vk::push_constant]] ConstantBuffer<SharcPush> push;

static const float kSceneScale = 50.0f;
static const float kRadianceScale = 1e3f;
static const float kSecondHitThroughput = 0.5f;
static const float3 kNormal = float3(0.0f, 0.0f, -1.0f);

// first hits on z = 0, second hits on z = 5; offset keeps probes off voxel boundaries
float3 probePosition(uint probe, float z)
{
    return float3(float(probe % 8u), float(probe / 8u), z) + 0.37f;
}

float3 directLighting(uint probe, uint hit)
{
    return float3(float(probe + 1u), float(probe % 5u + 1u), float(hit + 1u)) * 0.1f;
}

SharcParameters makeParameters()
{
    SharcParameters parameters;
    parameters.hashGridParameters.cameraPosition = push.cameraPosition;
    parameters.hashGridParameters.logarithmBase = SHARC_GRID_LOGARITHM_BASE;
    parameters.hashGridParameters.sceneScale = kSceneScale;
    parameters.hashGridParameters.levelBias = SHARC_GRID_LEVEL_BIAS;
    parameters.hashGridData.capacity = push.capacity;
    parameters.hashGridData.hashEntriesBuffer = push.hashEntries;
    parameters.radianceScale = kRadianceScale;
    parameters.accumulationBuffer = push.accumulation;
    parameters.resolvedBuffer = push.resolved;
    return parameters;
}

SharcHitData makeHit(float3 position)
{
    SharcHitData hit;
    hit.positionWorld = position;
    hit.normalWorld = kNormal;
    return hit;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void updateMain(uint3 id: SV_DispatchThreadID)
{
    const uint probe = id.x;
    if (probe >= push.probeCount)
        return;

    SharcParameters parameters = makeParameters();
    SharcState state;
    SharcInit(state);
    SharcUpdateHit(parameters, state, makeHit(probePosition(probe, 0.0f)), directLighting(probe, 0u), 0.5f);
    SharcSetThroughput(state, kSecondHitThroughput);
    SharcUpdateHit(parameters, state, makeHit(probePosition(probe, 5.0f)), directLighting(probe, 1u), 0.5f);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void resolveMain(uint3 id: SV_DispatchThreadID)
{
    SharcResolveParameters resolveParameters;
    resolveParameters.cameraPositionPrev = push.cameraPosition;
    resolveParameters.accumulationFrameNum = 32u;
    resolveParameters.responsiveFrameNum = 4u;
    resolveParameters.staleFrameNumMax = 64u;
    resolveParameters.frameIndex = 0u;
    SharcResolveEntry(id.x, makeParameters(), resolveParameters);
}

// probes [0, probeCount) query first hits, [probeCount, 2 * probeCount) second hits
[shader("compute")]
[numthreads(64, 1, 1)]
void queryMain(uint3 id: SV_DispatchThreadID)
{
    if (id.x >= push.probeCount * 2u)
        return;

    const bool isSecondHit = id.x >= push.probeCount;
    const uint probe = isSecondHit ? id.x - push.probeCount : id.x;
    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    const bool isCached = SharcGetCachedRadiance(makeParameters(), makeHit(probePosition(probe, isSecondHit ? 5.0f : 0.0f)),
                                                 radiance, false);
    push.results[id.x] = float4(radiance, isCached ? 1.0f : 0.0f);
}
