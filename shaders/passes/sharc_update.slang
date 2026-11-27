import types;
import core.binding;
import core.counters;
import core.frame;
import core.math;
import geometry.hit;
import material.brdf;
import material.pbr;
import lighting.glass;
import raytracing.trace;

#include "lighting/rtxdi_bridge.slang"
#include "lighting/path_shading.slang"
// SHARC_UPDATE=1 (CMakeLists.txt)
#include "gi/sharc_bridge.slang"

// SHaRC sparse update pass (M8.4, spec/m8/m8_4_sharc_cache.md §4.1): 1/(downscale^2) jittered G-buffer pixels trace
// inline compute rayQueryEXT paths keeping SharcUpdatePath in registers (0 B path-state buffer vs RTXPT's 33-133 MiB).
// Emissive is always kept separate from cached radiance so vertex 0 never double-counts sampled light triangles.

static const uint kSharcUpdateTile = 8;

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

// Disjoint from RTXDI (0..31), NRC update (41..42), reference (43), and SHaRC query continuation (46)
static const uint kSharcUpdatePathSeed = 44;
static const uint kSharcUpdateLightSeed = 45;

float3 clampSharcDirect(float3 directLighting, float clampLimit)
{
    if (clampLimit <= 0.0)
    {
        return directLighting;
    }
    const float luma = luminance(directLighting);
    return luma > clampLimit ? directLighting * (clampLimit / luma) : directLighting;
}

void recordSharcOverflow(GpuSharcConstants sharc, bool isOverflow)
{
    if (sharc.counters != 0 && WaveActiveAnyTrue(isOverflow))
    {
        GpuSharcCounters* counters = bdaPtr<GpuSharcCounters>(sharc.counters);
        waveCount(counters.insertFailures, isOverflow);
    }
}

[shader("compute")]
[numthreads(kSharcUpdateTile, kSharcUpdateTile, 1)]
void sharcUpdateMain(uint3 id : SV_DispatchThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    if (root.sharcConstants == 0)
    {
        return;
    }
    const GpuSharcConstants sharc = loadSharcConstants(root);
    const uint2 launchSize = uint2(sharc.updateLaunch & 0xFFFFu, sharc.updateLaunch >> 16);
    const uint2 launch = id.xy;
    if (any(launch >= launchSize))
    {
        return;
    }
    rtxdiBridgeInit(root);
    const GpuRtxdiConstants rtxdiConstants = loadRtxdiConstants(root);
    const SharcUpdateParams sharcParams = sharcUpdateParamsFrom(sharc);
    const uint frameIndex = root.frameIndex;

    RTXDI_RandomSamplerState pathRng = RTXDI_InitRandomSampler(launch, frameIndex, kSharcUpdatePathSeed);
    RTXDI_RandomSamplerState lightRng = RTXDI_InitRandomSampler(launch, frameIndex, kSharcUpdateLightSeed);
    RTXDI_RandomSamplerState coherentRng =
        initCoherentRandomSampler(rtxdiConstants, launch, frameIndex, kSharcUpdateLightSeed);

    const uint downscale = max(sharc.downscaleFactor, 1u);
    const uint offsetX = min(uint(RTXDI_GetNextRandom(pathRng) * float(downscale)), downscale - 1u);
    const uint offsetY = min(uint(RTXDI_GetNextRandom(pathRng) * float(downscale)), downscale - 1u);
    const uint2 pixel = min(launch * downscale + uint2(offsetX, offsetY), gRtxdiBridge.renderSize - 1u);

    RAB_Surface primary = RAB_GetGBufferSurface(int2(pixel), false);
    bool isOverflow = false;
    bool isActive = RAB_IsSurfaceValid(primary) && primary.isLit;
    SharcUpdatePath sharcState = sharcUpdateInit();
    RayDesc ray;
    ray.Origin = float3(0.0, 0.0, 0.0);
    ray.Direction = float3(0.0, 0.0, 1.0);
    ray.TMin = 0.0;
    ray.TMax = kMissDistance;
    float pathLength = 0.0;

    if (isActive)
    {
        primary.roughness = max(primary.roughness, sharc.minRoughnessOverride);
        RasterOrigin primaryOrigin;
        primaryOrigin.surface = primary;
        const float3 direct0 =
            clampSharcDirect(directLightAt(root, primary, primaryOrigin, rtxdiConstants, lightRng, coherentRng),
                             sharc.fireflyClamp);

        // Vertex 0 has no predecessor in sharcState, so separate emissive has nothing to back-propagate to
        const SharcUpdateHitData hitData0 = sharcMakeUpdateHitData(
            sharc, primary.position, primary.geoNormal, primary.diffuseAlbedo, primary.specularF0,
            float3(0.0, 0.0, 0.0));
        bool hitOverflow = false;
        isActive = sharcUpdateHit(sharcParams, sharcState, hitData0, direct0, RTXDI_GetNextRandom(pathRng),
                                  hitOverflow);
        isOverflow = hitOverflow;
        if (isActive)
        {
            float3 segmentThroughput = float3(1.0, 1.0, 1.0);
            float pdf = 0.0;
            isActive = scatterBrdf(primary, primaryOrigin, pathRng, segmentThroughput, ray, pdf);
            if (isActive)
            {
                sharcUpdateSetThroughput(sharcState, segmentThroughput);
                pathLength = length(primary.position - gRtxdiBridge.cameraPos);
            }
        }
    }

    const uint maxBounces = sharc.maxUpdateBounces;
    for (uint bounce = 1; isActive && bounce < maxBounces; ++bounce)
    {
        RayHit hit;
        if (!traceClosest(root, ray, hit))
        {
            sharcUpdateMiss(sharcParams, sharcState, float3(0.0, 0.0, 0.0));
            break;
        }
        PathHit vertex = shadeRayHit(root, hit, ray, pathLength);
        pathLength += hit.t;
        if (!vertex.surface.isLit)
        {
            if (any(vertex.emissive > 0.0))
            {
                sharcUpdateMiss(sharcParams, sharcState, vertex.emissive);
            }
            break;
        }
        vertex.surface.roughness = max(vertex.surface.roughness, sharc.minRoughnessOverride);
        HitOrigin origin;
        origin.hit = hit;
        origin.geoNormal = vertex.surface.geoNormal;
        const float3 direct =
            clampSharcDirect(directLightAt(root, vertex.surface, origin, rtxdiConstants, lightRng, coherentRng),
                             sharc.fireflyClamp);

        const SharcUpdateHitData hitData = sharcMakeUpdateHitData(
            sharc, vertex.surface.position, vertex.surface.geoNormal, vertex.surface.diffuseAlbedo,
            vertex.surface.specularF0, vertex.emissive);
        bool hitOverflow = false;
        if (!sharcUpdateHit(sharcParams, sharcState, hitData, direct, RTXDI_GetNextRandom(pathRng), hitOverflow))
        {
            isOverflow = isOverflow || hitOverflow;
            break;
        }
        if (bounce + 1u >= maxBounces)
        {
            break;
        }
        float3 segmentThroughput = float3(1.0, 1.0, 1.0);
        float pdf = 0.0;
        if (!scatterBrdf(vertex.surface, origin, pathRng, segmentThroughput, ray, pdf))
        {
            break;
        }
        sharcUpdateSetThroughput(sharcState, segmentThroughput);
    }

    recordSharcOverflow(sharc, isOverflow);
}
