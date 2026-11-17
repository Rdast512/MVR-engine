import types;
import core.binding;
import core.frame;
import geometry.hit;
import material.brdf;
import material.pbr;
import raytracing.trace;

#include "lighting/rtxdi_bridge.slang"
#include "lighting/path_shading.slang"
#include "Rtxdi/GI/Reservoir.hlsli"
#include "Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli"
// NRC_QUERY=1 (CMakeLists.txt)
#include "gi/nrc_bridge.slang"

// indirect pass (spec/m4/g2_indirect_pass.md): one BRDF-sampled ray query per pixel from the primary guide surface,
// shaded at its hit X2 with emission and one RIS-sampled light, stored as the pixel's initial ReSTIR GI reservoir. It
// is also NRC's query pass (spec/m4/g3_nrc.md): X2 is the path's first cache vertex, and passes/nrc_resolve.slang adds
// the cache's bounces 2..inf to the reservoir. Compute with traceClosest while SER is shelved (considerations C-G1-3)

static const uint kIndirectTile = 8; // matches src/render/vk_renderer.cpp kFullscreenTile

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

// a miss stays a sample with zero radiance this far along the ray: M counts it for G5's normalization. No environment
// light before M7
static const float kMissDistance = 1.0e5;

// the primary surface's BRDF ray; isActive false when there is nothing to shade from
struct PrimaryBounce
{
    RayDesc ray;
    float pdf;
    bool isActive;
};

PrimaryBounce samplePrimaryBounce(GpuFrameRoot root, uint2 pixel)
{
    PrimaryBounce bounce;
    bounce.pdf = 0.0;
    bounce.ray.Origin = float3(0.0, 0.0, 0.0);
    bounce.ray.Direction = float3(0.0, 0.0, 1.0);
    bounce.ray.TMin = 0.0;
    bounce.ray.TMax = kMissDistance;
    // unlit primaries take no light, as in ReSTIR DI
    const RAB_Surface primary = RAB_GetGBufferSurface(int2(pixel), false);
    bounce.isActive = RAB_IsSurfaceValid(primary) && primary.isLit;
    if (bounce.isActive)
    {
        const uint frameIndex = bdaPtr<GpuRtxdiConstants>(root.rtxdiConstants)[0].runtime.frameIndex;
        RTXDI_RandomSamplerState rng =
            RTXDI_InitRandomSampler(pixel, frameIndex, RTXDI_GI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED);
        const float3 u = float3(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
        const BrdfSample brdf = sampleBrdf(primary.normal, primary.diffuseAlbedo, primary.specularF0,
                                           primary.roughness, primary.viewDir, u);
        bounce.pdf = brdf.pdf;
        bounce.isActive = brdf.pdf > 0.0;
        bounce.ray.Origin = rasterRayOrigin(primary, brdf.l);
        bounce.ray.Direction = brdf.l;
    }
    return bounce;
}

// the query sits on the path's first cache vertex: the termination heuristic is off in this pass's copy of the
// constants (one ray per pixel, spec §11.3)
NrcContext nrcQueryContext(GpuFrameRoot root, uint2 pixel)
{
    NrcConstants constants = nrcBridgeInit(root);
    constants.enableTerminationHeuristic = 0;
    const NrcBuffers buffers = {};
    return NrcCreateContext(constants, buffers, pixel);
}

[shader("compute")]
[numthreads(kIndirectTile, kIndirectTile, 1)]
void indirectMain(uint3 id : SV_DispatchThreadID)
{
    const uint2 pixel = id.xy;
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    rtxdiBridgeInit(root);
    if (any(pixel >= gRtxdiBridge.renderSize))
    {
        return;
    }
    const PrimaryBounce bounce = samplePrimaryBounce(root, pixel);
    const RayDesc ray = bounce.ray;
    const float pdf = bounce.pdf;
    const bool isActive = bounce.isActive;

    RayHit hit;
    const bool isHit = isActive && traceClosest(root, ray, hit);

    const GpuRtxdiConstants constants = loadRtxdiConstants(root);
    const NrcContext nrc = nrcQueryContext(root, pixel);
    const bool isCacheView = (loadNrcConstants(root).flags & kNrcFlagCacheView) != 0;
    NrcPathState nrcPath = NrcCreatePathState(nrc.constants, 0.0);
    float3 nrcThroughput = float3(1.0, 1.0, 1.0);
    float3 nrcRadiance = float3(0.0, 0.0, 0.0);

    RTXDI_GIReservoir reservoir = RTXDI_EmptyGIReservoir();
    if (isHit)
    {
        const PathHit secondary = shadeRayHit(root, hit, ray, length(ray.Origin - gRtxdiBridge.cameraPos));
        RTXDI_RandomSamplerState rng =
            RTXDI_InitRandomSampler(pixel, constants.runtime.frameIndex, RTXDI_SECONDARY_DI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED);
        RTXDI_RandomSamplerState coherentRng = RTXDI_InitRandomSampler(
            pixel / RTXDI_TILE_SIZE_IN_PIXELS, constants.runtime.frameIndex,
            RTXDI_SECONDARY_DI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED);
        HitOrigin origin;
        origin.hit = hit;
        const float3 radiance =
            secondary.emissive + directLightAt(secondary.surface, origin, constants, rng, coherentRng);
        reservoir = RTXDI_MakeGIReservoir(secondary.surface.position, secondary.surface.geoNormal, radiance, pdf);
        // an unlit X2 reflects no light, so the cache has nothing to add
        if (!isCacheView && secondary.surface.isLit && gNrcBridge.isActive)
        {
            const RAB_Surface s = secondary.surface;
            NrcUpdateOnHit(nrc, nrcPath,
                           nrcSurfaceAttributes(nrc.constants, s.position, s.normal, s.viewDir, s.roughness,
                                                s.diffuseAlbedo, s.specularF0),
                           hit.t, 1, nrcThroughput, nrcRadiance);
        }
    }
    else if (isActive)
    {
        reservoir = RTXDI_MakeGIReservoir(ray.Origin + ray.Direction * kMissDistance, -ray.Direction,
                                          float3(0.0, 0.0, 0.0), pdf);
        NrcUpdateOnMiss(nrcPath);
    }
    // NRC cache view: the cache at vertex zero, the SDK's DirectCacheView through the custom path
    if (isCacheView)
    {
        const RAB_Surface primary = RAB_GetGBufferSurface(int2(pixel), false);
        if (RAB_IsSurfaceValid(primary) && primary.isLit && gNrcBridge.isActive)
        {
            NrcUpdateOnHit(nrc, nrcPath,
                           nrcSurfaceAttributes(nrc.constants, primary.position, primary.normal, primary.viewDir,
                                                primary.roughness, primary.diffuseAlbedo, primary.specularF0),
                           length(primary.position - gRtxdiBridge.cameraPos), 0, nrcThroughput, nrcRadiance);
        }
    }
    // every pixel: G5 and the debug view load each pixel's reservoir, the NRC resolve each pixel's query path
    RTXDI_StoreGIReservoir(reservoir, constants.giReservoirBuffer,
                           RTXDI_PixelPosToReservoirPos(pixel, constants.runtime.activeCheckerboardField),
                           constants.giBufferIndices.secondarySurfaceReSTIRDIOutputBufferIndex);
    NrcWriteFinalPathInfo(nrc, nrcPath, nrcThroughput, nrcRadiance);
}
