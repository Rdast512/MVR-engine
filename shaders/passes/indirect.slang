import types;
import core.binding;
import core.frame;
import geometry.hit;
import material.brdf;
import material.pbr;
import raytracing.ser;
import raytracing.trace;

#include "lighting/rtxdi_bridge.slang"
#include "Rtxdi/DI/InitialSampling.hlsli"
#include "Rtxdi/GI/Reservoir.hlsli"
#include "Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli"

// indirect pass (spec/m4/g2_indirect_pass.md): one BRDF-sampled ray per pixel from the primary guide surface through
// the SER pipeline, shaded at its hit X2 with emission and one RIS-sampled light, stored as the pixel's initial ReSTIR
// GI reservoir. No radiance cache before G3: Lo(X2) holds the first bounce only. No subgroup ops after the reorder
// (considerations C-G1-2)

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

// a miss stays a sample with zero radiance this far along the ray: M counts it for G5's normalization. No environment
// light before M7
static const float kMissDistance = 1.0e5;

// one RIS-sampled light at X2 without reuse, through the same RTXDI sampling and bridge as ReSTIR DI's initial pass,
// with one visibility ray from the traced triangle
float3 directLightAt(RAB_Surface surface, RayHit hit, GpuRtxdiConstants constants, uint2 pixel)
{
    if (gRtxdiBridge.lightCount == 0 || !surface.isLit)
    {
        return float3(0.0, 0.0, 0.0);
    }
    const uint frameIndex = constants.runtime.frameIndex;
    RTXDI_RandomSamplerState rng =
        RTXDI_InitRandomSampler(pixel, frameIndex, RTXDI_SECONDARY_DI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED);
    RTXDI_RandomSamplerState coherentRng = RTXDI_InitRandomSampler(
        pixel / RTXDI_TILE_SIZE_IN_PIXELS, frameIndex, RTXDI_SECONDARY_DI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED);
    // no geometry is a light before emissive triangles (M7): a BRDF candidate would only spend a ray
    RTXDI_DIInitialSamplingParameters sampling = constants.initialSampling;
    sampling.numBrdfSamples = 0;
    RAB_LightSample lightSample;
    const RTXDI_DIReservoir reservoir =
        RTXDI_SampleLightsForSurface(rng, coherentRng, surface, sampling, constants.lightBuffer, lightSample);
    if (!RTXDI_IsValidDIReservoir(reservoir))
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 shade = shadeLightSample(surface, lightSample);
    if (all(shade <= 0.0) ||
        !traceVisibility(gRtxdiBridge.tlas, safeSpawnPoint(hit, lightSample.l), lightSample.l, 0.0,
                         lightSample.distance))
    {
        return float3(0.0, 0.0, 0.0);
    }
    return shade * RTXDI_GetDIReservoirInvPdf(reservoir);
}

// the primary surface's BRDF ray; isActive false when there is nothing to shade from
struct PrimaryBounce
{
    RayDesc ray;
    float pdf;
    bool isActive;
    uint64_t tlas;
};

PrimaryBounce samplePrimaryBounce(uint2 pixel)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    rtxdiBridgeInit(root);
    PrimaryBounce bounce;
    bounce.tlas = root.tlas;
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

[shader("raygeneration")]
void indirectRaygen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const PrimaryBounce bounce = samplePrimaryBounce(pixel);
    const RayDesc ray = bounce.ray;
    const float pdf = bounce.pdf;
    const bool isActive = bounce.isActive;

    RayHit hit;
    const bool isHit = traceShaded(bounce.tlas, ray, hit, isActive);

    // frame data reloaded after the reorder: only the pixel and the bounce above cross it
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    rtxdiBridgeInit(root);
    const GpuRtxdiConstants constants = loadRtxdiConstants(root);

    RTXDI_GIReservoir reservoir = RTXDI_EmptyGIReservoir();
    if (isHit)
    {
        // ray cone from the primary pixel's footprint; the primary surface's curvature is ignored
        const GpuCameraData camera = loadCamera(root);
        RayCone cone;
        cone.spread = pixelSpreadAngle(camera, float2(gRtxdiBridge.renderSize));
        cone.width = cone.spread * length(ray.Origin - camera.cameraPos);
        const DecodedHit decoded = decodeHit(root, hit, ray.Direction, cone);
        // no DLSS LOD bias: indirect texture detail is never reconstructed at display resolution
        const SampledMaterial material =
            sampleMaterial(loadMaterial(root, materialRow(decoded.draw.primitive)), decoded.surface, 0.0);
        const RAB_Surface secondary = rayHitSurface(decoded, material, ray.Direction, hit.t);
        const float3 radiance = material.emissive + directLightAt(secondary, hit, constants, pixel);
        reservoir = RTXDI_MakeGIReservoir(secondary.position, secondary.geoNormal, radiance, pdf);
    }
    else if (isActive)
    {
        reservoir = RTXDI_MakeGIReservoir(ray.Origin + ray.Direction * kMissDistance, -ray.Direction,
                                          float3(0.0, 0.0, 0.0), pdf);
    }
    // every pixel: G5 and the debug view load each pixel's reservoir
    RTXDI_StoreGIReservoir(reservoir, constants.giReservoirBuffer,
                           RTXDI_PixelPosToReservoirPos(pixel, constants.runtime.activeCheckerboardField),
                           constants.giBufferIndices.secondarySurfaceReSTIRDIOutputBufferIndex);
}

[shader("closesthit")]
void indirectClosestHit(inout ClusterPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    writeClusterPayload(payload);
}

// misses are not invoked; the SBT still needs the record
[shader("miss")]
void indirectMiss(inout ClusterPayload payload)
{
}
