#ifndef ENGINE_LIGHTING_PATH_SHADING_SLANG
#define ENGINE_LIGHTING_PATH_SHADING_SLANG

// path vertices of the shaded-ray passes (indirect query, NRC update, glass): a ray hit as an RAB_Surface, and one
// RIS-sampled light at a vertex through the same RTXDI sampling and bridge as ReSTIR DI's initial pass. Textual, after
// lighting/rtxdi_bridge.slang; the including pass imports lighting.glass and material.brdf
#include "Rtxdi/DI/InitialSampling.hlsli"
#include "Rtxdi/DI/SpatialResampling.hlsli"

// where a vertex's rays start: the traced triangle or the raster pixel (strictly front-side along geoNormal, m8 I14)
interface IRayOrigin
{
    float3 spawn();
};

struct HitOrigin : IRayOrigin
{
    RayHit hit;
    float3 geoNormal;
    float3 spawn() { return safeSpawnPoint(hit, geoNormal); }
};

// the primary surface is a raster pixel, no traced triangle
struct RasterOrigin : IRayOrigin
{
    RAB_Surface surface;
    float3 spawn() { return rasterRayOrigin(surface); }
};

// one RIS-sampled light at a lit vertex, without reuse. Light candidates alone reach every light, triangles included;
// a BRDF candidate would spend one more ray per vertex. Local lights are sampled in the primary surface's mode and
// count (neeLocalLightSampling of the RTXDI sample), from the same presampled tiles and ReGIR cells
RTXDI_DIReservoir sampleLightsAt(RAB_Surface surface, GpuRtxdiConstants constants, inout RTXDI_RandomSamplerState rng,
                                 inout RTXDI_RandomSamplerState coherentRng, out RAB_LightSample lightSample)
{
    RTXDI_DIInitialSamplingParameters sampling = constants.initialSampling;
    sampling.numBrdfSamples = 0;
    // m8 I2 F18: shadeLightReservoir traces visibility from origin.spawn (safeSpawnPoint for ray hits); leaving
    // enableInitialVisibility on traces an extra ray from rasterRayOrigin (RTXDI sample LightingPasses.cpp:64)
    sampling.enableInitialVisibility = 0;
    return RTXDI_SampleLightsForSurface(rng, coherentRng, surface, sampling, constants.lightBuffer,
                                        constants.localLightRisBuffer, constants.environmentLightRisBuffer,
                                        regirParameters(), lightSample);
}

// a sampled light's contribution with one visibility ray
float3 shadeLightReservoir<Origin : IRayOrigin>(RAB_Surface surface, Origin origin, RTXDI_DIReservoir reservoir,
                                                 RAB_LightSample lightSample)
{
    if (!RTXDI_IsValidDIReservoir(reservoir))
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 shade = shadeLightSample(surface, lightSample);
    if (all(shade <= 0.0) || !traceLightVisibility(origin.spawn(), lightSample))
    {
        return float3(0.0, 0.0, 0.0);
    }
    return shade * RTXDI_GetDIReservoirInvPdf(reservoir);
}

// one RIS-sampled light without reuse and one visibility ray
float3 directLightAt<Origin : IRayOrigin>(RAB_Surface surface, Origin origin, GpuRtxdiConstants constants,
                                          inout RTXDI_RandomSamplerState rng,
                                          inout RTXDI_RandomSamplerState coherentRng)
{
    if (gRtxdiBridge.lightCount == 0 || !surface.isLit)
    {
        return float3(0.0, 0.0, 0.0);
    }
    RAB_LightSample lightSample;
    const RTXDI_DIReservoir reservoir = sampleLightsAt(surface, constants, rng, coherentRng, lightSample);
    return shadeLightReservoir(surface, origin, reservoir, lightSample);
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

// X2's direct light (the indirect pass, the glass pass's reflections); with kGiFlagSecondaryResampling the sampled
// light is resampled against this frame's final ReSTIR DI reservoirs around X2's pixel when X2 is on screen, as the
// RTXDI sample's ShadeSecondarySurfaces. Light through glass is attenuated here, not in the resampling (m8 I13 §3)
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
    const float3 radiance = shadeLightReservoir(surface, origin, reservoir, lightSample);
    if (!any(radiance > 0.0))
    {
        return radiance;
    }
    const float3 spawn = origin.spawn();
    float3 dir;
    float tMax;
    if (!lightVisibilitySegment(spawn, lightSample, dir, tMax))
    {
        return radiance;
    }
    return radiance * shadowTransmittance(root, spawn, dir, tMax);
}

// the glass's diffuse share (m8 I13 §2): one RIS-sampled light with its visibility and shadow transmittance, shaded
// Lambert only. The RIS target is the bridge BRDF of the same surface, non-zero wherever Lambert is
float3 lambertLightAt(GpuFrameRoot root, RAB_Surface surface, HitOrigin origin, GpuRtxdiConstants constants,
                      inout RTXDI_RandomSamplerState rng, inout RTXDI_RandomSamplerState coherentRng)
{
    if (gRtxdiBridge.lightCount == 0 || !surface.isLit)
    {
        return float3(0.0, 0.0, 0.0);
    }
    RAB_LightSample lightSample;
    const RTXDI_DIReservoir reservoir = sampleLightsAt(surface, constants, rng, coherentRng, lightSample);
    if (!RTXDI_IsValidDIReservoir(reservoir))
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float nDotL = dot(surface.normal, lightSample.l);
    if (nDotL <= 0.0 || dot(surface.geoNormal, lightSample.l) <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 spawn = origin.spawn();
    float3 dir;
    float tMax;
    const bool hasSegment = lightVisibilitySegment(spawn, lightSample, dir, tMax);
    if (hasSegment && !traceVisibility(gRtxdiBridge.rays, spawn, dir, 0.0, tMax))
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 transmittance =
        hasSegment ? shadowTransmittance(root, spawn, dir, tMax) : float3(1.0, 1.0, 1.0);
    return surface.diffuseAlbedo * (nDotL / kPi) * lightSample.radiance * RTXDI_GetDIReservoirInvPdf(reservoir) *
           transmittance;
}

struct PathHit
{
    RAB_Surface surface;
    float3 emissive; // after the one emission rule (hitEmission), unless isLightEmissionKept
};

// the one emission rule (spec/m7/e2_emissive_triangles.md): a light triangle's light reaches every vertex through
// light sampling, so its hit adds no emission; other emitters (unlit, or baked zero-power triangles that NEE cannot
// sample, m8 I14 F27) add theirs. Counting both would double it
float3 hitEmission(GpuFrameRoot root, RayHit hit, float3 emissive)
{
    const TriangleLightTable table = loadTriangleLightTable(root);
    const uint lightIndex = hitLightIndex(table, hit);
    if (lightIndex == kNoneIndex)
    {
        return emissive;
    }
    const GpuTriangleLight light = asTriangleLight(bdaPtr<GpuLight>(root.worldLights)[lightIndex]);
    return any(light.radiance > 0.0) ? float3(0.0, 0.0, 0.0) : emissive;
}

// decode + material at a hit. coneDistance is camera to ray.Origin; decodeHit adds cone.spread * hit.t. No DLSS LOD
// bias: indirect texture detail is never reconstructed at display resolution. isLightEmissionKept: delta mirror rays
// (glass reflection) never sample specular light at the glass surface, so light-triangle emission stays intact
PathHit shadeRayHit(GpuFrameRoot root, RayHit hit, RayDesc ray, float coneDistance, bool isLightEmissionKept = false)
{
    const GpuCameraData camera = loadCamera(root);
    RayCone cone;
    cone.spread = pixelSpreadAngle(camera, float2(gRtxdiBridge.renderSize));
    cone.width = cone.spread * coneDistance;
    const DecodedHit decoded = decodeHit(root, hit, ray.Direction, cone);
    const SampledMaterial material =
        sampleMaterial(loadMaterial(root, materialRow(decoded.draw.primitive)), decoded.surface, 0.0);
    PathHit result;
    result.surface = rayHitSurface(decoded, material, ray.Direction, hit.t);
    result.emissive = isLightEmissionKept ? material.emissive : hitEmission(root, hit, material.emissive);
    return result;
}

// the next segment from a lit vertex along a full-BRDF sample; false: absorbed. pdf: the sample's solid-angle pdf
bool scatterBrdf<Origin : IRayOrigin>(RAB_Surface surface, Origin origin, inout RTXDI_RandomSamplerState rng,
                                       inout float3 throughput, inout RayDesc ray, out float pdf)
{
    pdf = 0.0;
    if (!surface.isLit)
    {
        return false;
    }
    const float3 u = float3(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
    const BrdfSample brdf =
        sampleBrdf(surface.normal, surface.diffuseAlbedo, surface.specularF0, surface.roughness, surface.viewDir, u);
    // reject directions tilted below the geometric plane by normal mapping (RTXDI BrdfRayTracing.hlsl)
    if (brdf.pdf <= 0.0 || dot(surface.geoNormal, brdf.l) <= 0.0)
    {
        return false;
    }
    throughput *= brdf.weight;
    pdf = brdf.pdf;
    ray.Origin = origin.spawn();
    ray.Direction = brdf.l;
    return true;
}

// engine spec §14 ground truth: the radiance leaving vertex toward the previous one from `bounces` further vertices,
// each adding its emission and one sampled light, with no radiance cache and no roulette. pathLength: camera to vertex
float3 referencePathRadiance(GpuFrameRoot root, RAB_Surface start, HitOrigin startOrigin, uint bounces,
                             float startPathLength, GpuRtxdiConstants constants,
                             inout RTXDI_RandomSamplerState pathRng, inout RTXDI_RandomSamplerState lightRng,
                             inout RTXDI_RandomSamplerState coherentRng)
{
    RAB_Surface vertex = start;
    HitOrigin origin = startOrigin;
    float pathLength = startPathLength;
    float3 radiance = float3(0.0, 0.0, 0.0);
    float3 throughput = float3(1.0, 1.0, 1.0);
    RayDesc ray;
    ray.Origin = float3(0.0, 0.0, 0.0);
    ray.Direction = float3(0.0, 0.0, 1.0);
    ray.TMin = 0.0;
    ray.TMax = kMissDistance;
    for (uint bounce = 0; bounce < bounces; ++bounce)
    {
        float pdf;
        if (!scatterBrdf(vertex, origin, pathRng, throughput, ray, pdf))
        {
            break;
        }
        RayHit hit;
        if (!traceClosest(root, ray, hit))
        {
            break;
        }
        const PathHit next = shadeRayHit(root, hit, ray, pathLength);
        pathLength += hit.t;
        origin.hit = hit;
        origin.geoNormal = next.surface.geoNormal;
        radiance += throughput * (next.emissive + directLightAt(next.surface, origin, constants, lightRng, coherentRng));
        vertex = next.surface;
    }
    return radiance;
}

#endif
