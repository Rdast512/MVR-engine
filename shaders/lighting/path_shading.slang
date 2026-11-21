#ifndef ENGINE_LIGHTING_PATH_SHADING_SLANG
#define ENGINE_LIGHTING_PATH_SHADING_SLANG

// path vertices of the shaded-ray passes (indirect query, NRC update): a ray hit as an RAB_Surface, and one RIS-sampled
// light at a vertex through the same RTXDI sampling and bridge as ReSTIR DI's initial pass. Textual, after
// lighting/rtxdi_bridge.slang
#include "Rtxdi/DI/InitialSampling.hlsli"

// where a vertex's rays start: the traced triangle or the raster pixel
interface IRayOrigin
{
    float3 spawn(float3 direction);
};

struct HitOrigin : IRayOrigin
{
    RayHit hit;
    float3 spawn(float3 direction) { return safeSpawnPoint(hit, direction); }
};

// the primary surface is a raster pixel, no traced triangle
struct RasterOrigin : IRayOrigin
{
    RAB_Surface surface;
    float3 spawn(float3 direction) { return rasterRayOrigin(surface, direction); }
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
    if (all(shade <= 0.0) ||
        !traceVisibility(gRtxdiBridge.rays, origin.spawn(lightSample.l), lightSample.l, 0.0, lightSample.distance))
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

struct PathHit
{
    RAB_Surface surface;
    float3 emissive; // after the one emission rule (hitEmission)
};

// the one emission rule (spec/m7/e2_emissive_triangles.md): a light triangle's light reaches every vertex through
// light sampling, so its hit adds no emission; other emitters (unlit) add theirs. Counting both would double it
float3 hitEmission(GpuFrameRoot root, RayHit hit, float3 emissive)
{
    return isLightHit(loadTriangleLightTable(root), hit) ? float3(0.0, 0.0, 0.0) : emissive;
}

// decode + material at a hit. coneDistance is camera to ray.Origin; decodeHit adds cone.spread * hit.t. No DLSS LOD
// bias: indirect texture detail is never reconstructed at display resolution
PathHit shadeRayHit(GpuFrameRoot root, RayHit hit, RayDesc ray, float coneDistance)
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
    result.emissive = hitEmission(root, hit, material.emissive);
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
    ray.Origin = origin.spawn(brdf.l);
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
        radiance += throughput * (next.emissive + directLightAt(next.surface, origin, constants, lightRng, coherentRng));
        vertex = next.surface;
    }
    return radiance;
}

#endif
