import types;
import core.binding;
import core.frame;
import core.math;
import geometry.hit;
import material.brdf;
import material.pbr;
import raytracing.trace;

#include "lighting/rtxdi_bridge.slang"
#include "lighting/path_shading.slang"
#include "Rtxdi/DI/SpatialResampling.hlsli"
#include "Rtxdi/GI/Reservoir.hlsli"
#include "Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli"
// NRC_QUERY=1 (CMakeLists.txt)
#include "gi/nrc_bridge.slang"

// indirect pass (spec/m4/g2_indirect_pass.md): one BRDF-sampled ray query per pixel from the primary guide surface,
// shaded at its hit X2 with emission and one RIS-sampled light, stored as the pixel's initial ReSTIR GI reservoir. It
// is also NRC's query pass (spec/m4/g3_nrc.md): X2 is the path's first cache vertex, and passes/nrc_resolve.slang adds
// the cache's bounces 2..inf to the reservoir. With giReferenceBounces the pass traces those bounces itself instead
// and NRC is paused (the engine spec §14 ground truth, spec/m4/g5_restir_gi.md). Compute with traceClosest while SER
// is shelved (considerations C-G1-3)

static const uint kIndirectTile = 8; // matches src/render/vk_renderer.cpp kFullscreenTile

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

// RTXDI's per-pass seeds stop at 31; NRC's update pass takes 41 and 42
static const uint kReferencePathSeed = 43;

// the primary surface's BRDF ray. isSampled: a lit primary drew a direction; isActive: it is traced (pdf > 0)
struct PrimaryBounce
{
    RayDesc ray;
    float pdf;
    bool isSampled;
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
    bounce.isSampled = RAB_IsSurfaceValid(primary) && primary.isLit;
    bounce.isActive = false;
    if (bounce.isSampled)
    {
        const uint frameIndex = bdaPtr<GpuRtxdiConstants>(root.rtxdiConstants)[0].runtime.frameIndex;
        RTXDI_RandomSamplerState rng =
            RTXDI_InitRandomSampler(pixel, frameIndex, RTXDI_GI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED);
        const float3 u = float3(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
        const BrdfSample brdf = sampleBrdf(primary.normal, primary.diffuseAlbedo, primary.specularF0,
                                           primary.roughness, primary.viewDir, u);
        bounce.pdf = brdf.pdf;
        // normal maps can tilt n so a sample lies below the triangle's geometric plane (RTXDI BrdfRayTracing.hlsl)
        bounce.isActive = brdf.pdf > 0.0 && dot(primary.geoNormal, brdf.l) > 0.0;
        bounce.ray.Origin = rasterRayOrigin(primary, brdf.l);
        bounce.ray.Direction = brdf.l;
    }
    return bounce;
}

// RTXDI sample Ultra preset secondarySurfaceReSTIRDIParams: 2 samples, 4 px (1 px truncates all 8192 offsets to
// (0, 0)), ray-traced. Unbiased only on unbiased DI reservoirs: NVIDIA's Medium DI darkens them (m8 O2, §2.7)
RTXDI_DISpatialResamplingParameters secondaryResamplingParams(GpuRtxdiConstants constants)
{
    RTXDI_DISpatialResamplingParameters params = constants.spatialResampling;
    params.numSamples = 2;
    params.numDisocclusionBoostSamples = 0;
    params.samplingRadius = 4.0;
    params.biasCorrectionMode = RTXDI_BIAS_CORRECTION_RAY_TRACED;
    params.depthThreshold = 0.1;
    params.normalThreshold = 0.9;
    params.enableMaterialSimilarityTest = 0;
    params.discountNaiveSamples = 0;
    params.targetHistoryLength = 0;
    return params;
}

// X2's direct light; with kGiFlagSecondaryResampling the sampled light is resampled against this frame's final ReSTIR
// DI reservoirs around X2's pixel when X2 is on screen, as the RTXDI sample's ShadeSecondarySurfaces
float3 secondaryDirectLight(GpuFrameRoot root, RAB_Surface surface, HitOrigin origin, GpuRtxdiConstants constants,
                            inout RTXDI_RandomSamplerState rng, inout RTXDI_RandomSamplerState coherentRng)
{
    if (gRtxdiBridge.lightCount == 0 || !surface.isLit)
    {
        return float3(0.0, 0.0, 0.0);
    }
    RAB_LightSample lightSample;
    RTXDI_DIReservoir reservoir = sampleLightsAt(surface, constants, rng, coherentRng, lightSample);
    if ((constants.giFlags & kGiFlagSecondaryResampling) != 0)
    {
        const GpuCameraData camera = loadCamera(root);
        const float4 clip = mul(camera.viewProj, float4(surface.position, 1.0));
        // the guides were rasterized jittered
        const float2 ndc = clip.xy / max(clip.w, 1e-6) + camera.jitterOffset;
        if (clip.w > 0.0 && all(abs(ndc) < 1.0))
        {
            const uint2 pixel = min(uint2((ndc * 0.5 + 0.5) * float2(gRtxdiBridge.renderSize)),
                                    gRtxdiBridge.renderSize - 1);
            // the neighbor tests compare view depths
            RAB_Surface center = surface;
            center.viewDepth = clip.w;
            reservoir = RTXDI_DISpatialResampling(pixel, center, reservoir, rng, constants.runtime,
                                                  constants.reservoirBuffer,
                                                  constants.bufferIndices.shadingInputBufferIndex,
                                                  secondaryResamplingParams(constants), lightSample);
            if (RTXDI_IsValidDIReservoir(reservoir))
            {
                // the resampling may have taken a neighbor's light: evaluate it at X2
                lightSample =
                    RAB_SamplePolymorphicLight(RAB_LoadLightInfo(RTXDI_GetDIReservoirLightIndex(reservoir), false),
                                               surface, RTXDI_GetDIReservoirSampleUV(reservoir));
            }
        }
    }
    return shadeLightReservoir(surface, origin, reservoir, lightSample);
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
        // m8 O9: the reservoir stores X2's radiance as non-directional, so a sharp glossy lobe at X2 becomes a
        // caustic outlier that reuse spreads (Doc/RestirGI.md). X2's own shading only: the NRC query keeps the
        // roughness the cache trains on
        RAB_Surface shadedSecondary = secondary.surface;
        if ((constants.giFlags & kGiFlagSecondaryRoughnessFloor) != 0)
        {
            shadedSecondary.roughness = max(shadedSecondary.roughness, kMinSecondaryRoughness);
        }
        float3 radiance =
            secondary.emissive + secondaryDirectLight(root, shadedSecondary, origin, constants, rng, coherentRng);
        if (constants.giReferenceBounces > 0)
        {
            RTXDI_RandomSamplerState pathRng =
                RTXDI_InitRandomSampler(pixel, constants.runtime.frameIndex, kReferencePathSeed);
            radiance += referencePathRadiance(root, shadedSecondary, origin, constants.giReferenceBounces,
                                              length(ray.Origin - gRtxdiBridge.cameraPos) + hit.t, constants, pathRng,
                                              rng, coherentRng);
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
    else if (bounce.isSampled)
    {
        // a miss, or a direction the BRDF cannot reflect into: still a sample, of zero radiance. M counts it, else
        // temporal reuse keeps an older sample at full weight where the 1-sample estimate reads 0 (+15% on the
        // gallery, spec/m4/g5_restir_gi.md §3; the RTXDI sample stores an empty reservoir here)
        reservoir = RTXDI_MakeGIReservoir(ray.Origin + ray.Direction * kMissDistance, -ray.Direction,
                                          float3(0.0, 0.0, 0.0), pdf);
        if (isActive)
        {
            NrcUpdateOnMiss(nrcPath);
        }
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
    // every pixel: the GI passes and the debug view load each pixel's reservoir, the NRC resolve each pixel's query
    // path
    RTXDI_StoreGIReservoir(reservoir, constants.giReservoirBuffer,
                           RTXDI_PixelPosToReservoirPos(pixel, constants.runtime.activeCheckerboardField),
                           kGiInitialReservoirArray);
    NrcWriteFinalPathInfo(nrc, nrcPath, nrcThroughput, nrcRadiance);
}
