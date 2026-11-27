import types;
import core.binding;
import core.frame;
import core.math;
import geometry.hit;
import material.brdf;
import material.pbr;
import lighting.glass;
import raytracing.trace;

#include "lighting/rtxdi_bridge.slang"
#include "lighting/path_shading.slang"
#include "Rtxdi/GI/Reservoir.hlsli"
#include "Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli"
// NRC_QUERY=1, SHARC_QUERY=1 (CMakeLists.txt); mutually exclusive at runtime
#include "gi/nrc_bridge.slang"
#include "gi/sharc_bridge.slang"

// indirect pass (spec/m4/g2_indirect_pass.md): one BRDF-sampled ray query per pixel from the primary guide surface,
// shaded at its hit X2 with emission and one RIS-sampled light, stored as the pixel's initial ReSTIR GI reservoir. It
// is also NRC's query pass (spec/m4/g3_nrc.md): X2 is the path's first cache vertex, and passes/nrc_resolve.slang adds
// the cache's bounces 2..inf to the reservoir. With SHaRC active (M8.4), eligible X2 hits query cached total outgoing
// radiance inline and skip secondaryDirectLight + its shadow ray. With giReferenceBounces the pass traces those bounces
// itself instead and both caches are paused (engine spec §14 ground truth).

static const uint kIndirectTile = 8; // matches src/render/vk_renderer.cpp kFullscreenTile

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

// RTXDI's per-pass seeds stop at 31; NRC update takes 41..42, SHaRC update takes 44..45
static const uint kReferencePathSeed = 43;
static const uint kSharcQueryPathSeed = 46;

// the primary surface's BRDF ray. isSampled: a lit primary drew a direction; isActive: it is traced (pdf > 0)
struct PrimaryBounce
{
    RayDesc ray;
    float3 weight;
    float pdf;
    float primaryRoughness;
    bool isSampled;
    bool isActive;
    bool isSpecularLobe;
};

PrimaryBounce samplePrimaryBounce(GpuFrameRoot root, uint2 pixel)
{
    PrimaryBounce bounce;
    bounce.weight = float3(0.0, 0.0, 0.0);
    bounce.pdf = 0.0;
    bounce.primaryRoughness = 0.0;
    bounce.ray.Origin = float3(0.0, 0.0, 0.0);
    bounce.ray.Direction = float3(0.0, 0.0, 1.0);
    bounce.ray.TMin = 0.0;
    bounce.ray.TMax = kMissDistance;
    // unlit primaries take no light, as in ReSTIR DI
    const RAB_Surface primary = RAB_GetGBufferSurface(int2(pixel), false);
    bounce.isSampled = RAB_IsSurfaceValid(primary) && primary.isLit;
    bounce.isActive = false;
    bounce.isSpecularLobe = false;
    if (bounce.isSampled)
    {
        const uint frameIndex = bdaPtr<GpuRtxdiConstants>(root.rtxdiConstants)[0].runtime.frameIndex;
        RTXDI_RandomSamplerState rng =
            RTXDI_InitRandomSampler(pixel, frameIndex, RTXDI_GI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED);
        const float3 u = float3(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
        const BrdfSample brdf = sampleBrdf(primary.normal, primary.diffuseAlbedo, primary.specularF0,
                                           primary.roughness, primary.viewDir, u);
        bounce.weight = brdf.weight;
        bounce.pdf = brdf.pdf;
        bounce.primaryRoughness = primary.roughness;
        // normal maps can tilt n so a sample lies below the triangle's geometric plane (RTXDI BrdfRayTracing.hlsl)
        bounce.isActive = brdf.pdf > 0.0 && dot(primary.geoNormal, brdf.l) > 0.0;
        // f0 == 0 surfaces write zero SpecAlbedo (envBrdfApprox), so report no specular hit distance
        bounce.isSpecularLobe = brdf.isSpecularLobe && any(primary.specularF0 > 0.0);
        bounce.ray.Origin = rasterRayOrigin(primary);
        bounce.ray.Direction = brdf.l;
    }
    return bounce;
}

// m8 I13 §3: glass transmittance on the primary-to-X2 segment, matching rtxdi_gi_shade.slang
float3 rawReconnectionTransmittance(GpuFrameRoot root, float3 origin, float3 samplePosition)
{
    float3 l;
    float tMax;
    if (!visibilitySegment(origin, samplePosition, true, l, tMax))
    {
        return float3(1.0, 1.0, 1.0);
    }
    return glassTransmittance(root, origin, l, tMax);
}

// RTXDI sample c_MaxIndirectRadiance (10) as displayed luminance: scene luminance times last frame's exposure
static const float kMaxIndirectRadiance = 10.0;
// RTXDI sample minSecondaryRoughness (LightingPasses.cpp)
static const float kMinSecondaryRoughness = 0.5;

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
    const GpuTargets targets = loadTargets(root);
    const GpuRtxdiConstants constants = loadRtxdiConstants(root);
    const bool isRawIndirect = (root.frameFlags & kFrameFlagRawIndirect) != 0;
    const NrcContext nrc = nrcQueryContext(root, pixel);
    const bool isCacheView = (loadNrcConstants(root).flags & kNrcFlagCacheView) != 0;
    NrcPathState nrcPath = NrcCreatePathState(nrc.constants, 0.0);
    float3 nrcThroughput = float3(1.0, 1.0, 1.0);
    float3 nrcRadiance = float3(0.0, 0.0, 0.0);

    // M8.1 BUG-E3 / PERF-E2: cache view queries vertex 0 directly without tracing bounce-1 rays
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
        else if (gNrcBridge.isActive)
        {
            NrcUpdateOnMiss(nrcPath);
        }
        if (isRawIndirect)
        {
            storageTex2D<float4, kImageFormatRgba16f>(targets.indirectRadianceStorage)[pixel] =
                float4(0.0, 0.0, 0.0, 1.0);
            if (targets.rawIndirectWeightStorage != kNoneIndex)
            {
                storageTex2D<float4, kImageFormatRgba16f>(targets.rawIndirectWeightStorage)[pixel] =
                    float4(0.0, 0.0, 0.0, 0.0);
            }
        }
        else
        {
            RTXDI_StoreGIReservoir(RTXDI_EmptyGIReservoir(), constants.giReservoirBuffer,
                                   RTXDI_PixelPosToReservoirPos(pixel, constants.runtime.activeCheckerboardField),
                                   kGiInitialReservoirArray);
        }
        NrcWriteFinalPathInfo(nrc, nrcPath, nrcThroughput, nrcRadiance);
        if (gRtxdiBridge.specularHitDistanceStorage != kNoneIndex)
        {
            storageTex2D<float, kImageFormatR16f>(gRtxdiBridge.specularHitDistanceStorage)[pixel] = 0.0;
        }
        return;
    }

    const PrimaryBounce bounce = samplePrimaryBounce(root, pixel);
    const RayDesc ray = bounce.ray;
    const float pdf = bounce.pdf;
    const bool isActive = bounce.isActive;

    RayHit hit = {};
    const bool isHit = isActive && traceClosest(root, ray, hit);

    RTXDI_GIReservoir reservoir = RTXDI_EmptyGIReservoir();
    float3 rawRadiance = float3(0.0, 0.0, 0.0);
    float4 rawWeightAndLum = float4(0.0, 0.0, 0.0, 0.0);
    if (isHit)
    {
        const PathHit secondary = shadeRayHit(root, hit, ray, length(ray.Origin - gRtxdiBridge.cameraPos));
        RTXDI_RandomSamplerState rng =
            RTXDI_InitRandomSampler(pixel, constants.runtime.frameIndex, RTXDI_SECONDARY_DI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED);
        RTXDI_RandomSamplerState coherentRng = initCoherentRandomSampler(
            constants, pixel, constants.runtime.frameIndex, RTXDI_SECONDARY_DI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED);
        HitOrigin origin;
        origin.hit = hit;
        origin.geoNormal = secondary.surface.geoNormal;
        // m8 O9: the reservoir stores X2's radiance as non-directional, so a sharp glossy lobe at X2 becomes a
        // caustic outlier that reuse spreads (Doc/RestirGI.md). X2's own shading only: the NRC query keeps the
        // roughness the cache trains on
        RAB_Surface shadedSecondary = secondary.surface;
        if ((constants.giFlags & kGiFlagSecondaryRoughnessFloor) != 0)
        {
            shadedSecondary.roughness = max(shadedSecondary.roughness, kMinSecondaryRoughness);
        }
        float3 radiance = float3(0.0, 0.0, 0.0);
        const bool isSharcActive = root.sharcConstants != 0 && constants.giReferenceBounces == 0;
        if (isSharcActive)
        {
            const GpuSharcConstants sharc = loadSharcConstants(root);
            const SharcSdkParams sharcParams = sharcSdkParamsFrom(sharc);
            bool isSharcCacheHit = false;
            if (secondary.surface.isLit &&
                sharcCanQueryVoxel(sharcParams, secondary.surface.position, hit.t, bounce.isSpecularLobe,
                                   bounce.primaryRoughness))
            {
                const SharcSdkHit hitData = sharcMakeHitData(sharc, secondary.surface.position,
                                                             secondary.surface.geoNormal,
                                                             secondary.surface.diffuseAlbedo,
                                                             secondary.surface.specularF0, secondary.emissive);
                float3 cachedRadiance = float3(0.0, 0.0, 0.0);
                if (sharcQueryCachedRadiance(sharcParams, hitData, cachedRadiance) &&
                    sharcIsFiniteFloat3(cachedRadiance))
                {
                    // SHaRC stores total outgoing radiance (direct + multi-bounce): skips secondaryDirectLight shadow ray
                    radiance = max(cachedRadiance, 0.0);
                    isSharcCacheHit = true;
                }
            }
            if (!isSharcCacheHit)
            {
                radiance = secondary.emissive +
                           secondaryDirectLight(root, shadedSecondary, origin, constants, rng, coherentRng);
                if (secondary.surface.isLit && sharc.maxQueryBounces > 1u)
                {
                    RTXDI_RandomSamplerState pathRng =
                        RTXDI_InitRandomSampler(pixel, constants.runtime.frameIndex, kSharcQueryPathSeed);
                    RAB_Surface curSurface = shadedSecondary;
                    HitOrigin curOrigin = origin;
                    float curPathLength = length(ray.Origin - gRtxdiBridge.cameraPos) + hit.t;
                    float3 throughput = float3(1.0, 1.0, 1.0);
                    RayDesc nextRay;
                    nextRay.Origin = float3(0.0, 0.0, 0.0);
                    nextRay.Direction = float3(0.0, 0.0, 1.0);
                    nextRay.TMin = 0.0;
                    nextRay.TMax = kMissDistance;
                    for (uint bounceIdx = 2u; bounceIdx <= sharc.maxQueryBounces; ++bounceIdx)
                    {
                        if (!curSurface.isLit)
                        {
                            break;
                        }
                        const float3 u = float3(RTXDI_GetNextRandom(pathRng), RTXDI_GetNextRandom(pathRng),
                                                RTXDI_GetNextRandom(pathRng));
                        const BrdfSample brdf =
                            sampleBrdf(curSurface.normal, curSurface.diffuseAlbedo, curSurface.specularF0,
                                       curSurface.roughness, curSurface.viewDir, u);
                        if (brdf.pdf <= 0.0 || dot(curSurface.geoNormal, brdf.l) <= 0.0)
                        {
                            break;
                        }
                        throughput *= brdf.weight;
                        nextRay.Origin = curOrigin.spawn();
                        nextRay.Direction = brdf.l;
                        RayHit nextHit;
                        if (!traceClosest(root, nextRay, nextHit))
                        {
                            break;
                        }
                        const PathHit nextShaded = shadeRayHit(root, nextHit, nextRay, curPathLength);
                        curPathLength += nextHit.t;
                        const bool isNextSpecular = brdf.isSpecularLobe && any(curSurface.specularF0 > 0.0);
                        if (nextShaded.surface.isLit &&
                            sharcCanQueryVoxel(sharcParams, nextShaded.surface.position, nextHit.t, isNextSpecular,
                                               curSurface.roughness))
                        {
                            const SharcSdkHit nextHitData = sharcMakeHitData(
                                sharc, nextShaded.surface.position, nextShaded.surface.geoNormal,
                                nextShaded.surface.diffuseAlbedo, nextShaded.surface.specularF0, nextShaded.emissive);
                            float3 nextCached = float3(0.0, 0.0, 0.0);
                            if (sharcQueryCachedRadiance(sharcParams, nextHitData, nextCached) &&
                                sharcIsFiniteFloat3(nextCached))
                            {
                                radiance += throughput * max(nextCached, 0.0);
                                break;
                            }
                        }
                        curOrigin.hit = nextHit;
                        curOrigin.geoNormal = nextShaded.surface.geoNormal;
                        radiance += throughput *
                                    (nextShaded.emissive +
                                     directLightAt(root, nextShaded.surface, curOrigin, constants, rng, coherentRng));
                        curSurface = nextShaded.surface;
                    }
                }
            }
        }
        else
        {
            radiance =
                secondary.emissive + secondaryDirectLight(root, shadedSecondary, origin, constants, rng, coherentRng);
            if (constants.giReferenceBounces > 0)
            {
                RTXDI_RandomSamplerState pathRng =
                    RTXDI_InitRandomSampler(pixel, constants.runtime.frameIndex, kReferencePathSeed);
                radiance += referencePathRadiance(root, shadedSecondary, origin, constants.giReferenceBounces,
                                                  length(ray.Origin - gRtxdiBridge.cameraPos) + hit.t, constants,
                                                  pathRng, rng, coherentRng);
            }
        }
        // scene-unit firefly suppression (m8 O6)
        if ((constants.giFlags & kGiFlagFireflyClamp) != 0)
        {
            const float secondaryLuminance = luminance(radiance);
            if (secondaryLuminance > kMaxIndirectRadiance)
            {
                radiance *= kMaxIndirectRadiance / secondaryLuminance;
            }
        }
        // an unlit X2 reflects no light, so the cache has nothing to add
        const bool hasNrcQuery = secondary.surface.isLit && gNrcBridge.isActive;
        if (isRawIndirect)
        {
            // M8.3 PERF-D1: write indirectRadiance directly without GI reservoir LogLuv round-trip; AO applies to the
            // NRC cache term only inside nrc_resolve.slang, never to the traced bounce
            float3 primaryWeight = bounce.weight;
            if ((root.glassFlags & kGlassFlagShadowTransmittance) != 0 && (any(radiance > 0.0) || hasNrcQuery))
            {
                primaryWeight *= rawReconnectionTransmittance(root, ray.Origin, secondary.surface.position);
            }
            rawRadiance = radiance * primaryWeight;
            if (hasNrcQuery)
            {
                rawWeightAndLum = float4(min(primaryWeight, float3(65504.0, 65504.0, 65504.0)),
                                         min(luminance(radiance), 65504.0));
            }
        }
        else
        {
            // m8 I2 F13 + M8.1 BUG-D6: zero-radiance hits (matching RTXDI_EncodeRGBToLogLuv's 1e-6 floor) with no
            // pending NRC cache term store the far miss placeholder so their Jacobian stays ~1 during reuse
            const bool isZeroPlaceholder = luminance(radiance) < 1.0e-6 && !hasNrcQuery;
            const float3 samplePos =
                isZeroPlaceholder ? ray.Origin + ray.Direction * kMissDistance : secondary.surface.position;
            const float3 sampleNormal = isZeroPlaceholder ? -ray.Direction : secondary.surface.geoNormal;
            reservoir = RTXDI_MakeGIReservoir(samplePos, sampleNormal, radiance, pdf);
        }
        if (hasNrcQuery)
        {
            const RAB_Surface s = secondary.surface;
            NrcUpdateOnHit(nrc, nrcPath,
                           nrcSurfaceAttributes(nrc.constants, s.position, s.normal, s.viewDir, s.roughness,
                                                s.diffuseAlbedo, s.specularF0),
                           hit.t, 1, nrcThroughput, nrcRadiance);
        }
    }
    else if (bounce.isSampled)
    {
        if (!isRawIndirect)
        {
            // a miss, or a direction the BRDF cannot reflect into: still a sample, of zero radiance. M counts it, else
            // temporal reuse keeps an older sample at full weight where the 1-sample estimate reads 0 (+15% on the
            // gallery, spec/m4/g5_restir_gi.md §3; the RTXDI sample stores an empty reservoir here)
            reservoir = RTXDI_MakeGIReservoir(ray.Origin + ray.Direction * kMissDistance, -ray.Direction,
                                              float3(0.0, 0.0, 0.0), pdf);
        }
        if (isActive)
        {
            NrcUpdateOnMiss(nrcPath);
        }
    }
    if (isRawIndirect)
    {
        storageTex2D<float4, kImageFormatRgba16f>(targets.indirectRadianceStorage)[pixel] = float4(rawRadiance, 1.0);
        if (targets.rawIndirectWeightStorage != kNoneIndex)
        {
            storageTex2D<float4, kImageFormatRgba16f>(targets.rawIndirectWeightStorage)[pixel] = rawWeightAndLum;
        }
    }
    else
    {
        // every pixel: the GI passes and the debug view load each pixel's reservoir, the NRC resolve each pixel's query
        // path
        RTXDI_StoreGIReservoir(reservoir, constants.giReservoirBuffer,
                               RTXDI_PixelPosToReservoirPos(pixel, constants.runtime.activeCheckerboardField),
                               kGiInitialReservoirArray);
    }
    NrcWriteFinalPathInfo(nrc, nrcPath, nrcThroughput, nrcRadiance);
    // DLSS-RR's initial specular hit distance (m8 I16 phase 3): clamped to R16F max so hits in (65504, kMissDistance]
    // never overflow to +Inf; rtxdi_gi_shade and glass refine it when reuse or glass reflections take over
    if (gRtxdiBridge.specularHitDistanceStorage != kNoneIndex)
    {
        float specularHitDistance = 0.0;
        if (isActive && bounce.isSpecularLobe)
        {
            specularHitDistance = isHit ? min(hit.t, kMaxSpecularHitDistance) : kMaxSpecularHitDistance;
        }
        storageTex2D<float, kImageFormatR16f>(gRtxdiBridge.specularHitDistanceStorage)[pixel] = specularHitDistance;
    }
}
