import types;
import core.binding;
import core.frame;
import lighting.glass;

static const uint kRtxdiTile = 8; // matches src/render/vk_renderer.cpp

#include "lighting/rtxdi_bridge.slang"
#include "Rtxdi/DI/SpatialResampling.hlsli"
#include "Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli"

// ReSTIR DI spatial resampling and shading (spec/features/done/rtxdi_3_2_update.md §3.1): this pixel's temporal reservoir
// against this frame's neighbors, the final visibility, then directRadiance = evalBrdf · L_i · NdotL · W. Without
// spatial resampling (the reference) it shades the reservoir it loads. Neighbors are read from the temporal output
// and the result is written to the shading buffer, a different one, which is next frame's temporal input. The final
// visibility is stored in the reservoir; with reuseFinalVisibility a young enough stored one replaces the ray, as in
// the RTXDI sample's ShadingHelpers.hlsli

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

// m8 I13 §3: light through glass; the reservoir's visibility and the resampling stay glass-free
float3 glassTransmittance(GpuFrameRoot root, RAB_Surface surface, RAB_LightSample sample)
{
    const float3 origin = rasterRayOrigin(surface);
    float3 dir;
    float tMax;
    return lightVisibilitySegment(origin, sample, dir, tMax) ? shadowTransmittance(root, origin, dir, tMax)
                                                             : float3(1.0, 1.0, 1.0);
}

// directional lights are deltas: one shadow ray each shades them exactly, without RIS noise (m8 I16 F28)
float3 shadeInfiniteLightsExactly(GpuFrameRoot root, RAB_Surface surface, RTXDI_LightBufferRegion region)
{
    float3 radiance = float3(0.0, 0.0, 0.0);
    for (uint i = 0; i < region.numLights; ++i)
    {
        const RAB_LightInfo light = RAB_LoadLightInfo(region.firstLightIndex + i, false);
        const RAB_LightSample sample = RAB_SamplePolymorphicLight(light, surface, float2(0.0, 0.0));
        const float3 shade = shadeLightSample(surface, sample);
        if (any(shade > 0.0) && RAB_GetConservativeVisibility(surface, sample))
        {
            radiance += shade * glassTransmittance(root, surface, sample);
        }
    }
    return radiance;
}

[shader("compute")]
[numthreads(kRtxdiTile, kRtxdiTile, 1)]
void rtxdiShadeMain(uint3 id : SV_DispatchThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    rtxdiBridgeInit(root);
    // RenderTargets::kDirectRadianceFormat
    const uint output = bdaPtr<GpuTargets>(root.targets)[0].directRadianceStorage;
    // uniform: with no lights the host skips rtxdi_initial, and this pass only clears
    if (gRtxdiBridge.lightCount == 0)
    {
        if (all(id.xy < gRtxdiBridge.renderSize))
        {
            storageTex2D<float4, kImageFormatRgba16f>(output)[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
        }
        return;
    }
    if (any(id.xy >= gRtxdiBridge.renderSize))
    {
        return;
    }
    const GpuRtxdiConstants constants = loadRtxdiConstants(root);
    const RAB_Surface surface = RAB_GetGBufferSurface(int2(id.xy), false);
    const uint2 reservoirPosition = RTXDI_PixelPosToReservoirPos(id.xy, constants.runtime.activeCheckerboardField);
    const bool isInfiniteExact = (constants.diFlags & kDiFlagExactInfiniteLights) != 0;
    RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();
    // uniform: without candidates the host skips rtxdi_initial and rtxdi_temporal, so the input is stale and the
    // disocclusion boost would resample empty neighbors
    if (RAB_IsSurfaceValid(surface) && hasRisLightCandidates(constants, isInfiniteExact))
    {
        reservoir = RTXDI_LoadDIReservoir(constants.reservoirBuffer, reservoirPosition,
                                          constants.bufferIndices.spatialResamplingInputBufferIndex);
        // after a reset too: the neighbors are this frame's
        if ((constants.resamplingMode & 2u) != 0)
        {
            RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(id.xy, constants.runtime.frameIndex,
                                                                   RTXDI_DI_SPATIAL_RESAMPLING_RANDOM_SEED);
            // re-evaluated below from the final reservoir
            RAB_LightSample selectedSample = RAB_EmptyLightSample();
            reservoir = RTXDI_DISpatialResampling(id.xy, surface, reservoir, rng, constants.runtime,
                                                  constants.reservoirBuffer,
                                                  constants.bufferIndices.spatialResamplingInputBufferIndex,
                                                  constants.spatialResampling, selectedSample);
        }
    }

    float3 radiance = float3(0.0, 0.0, 0.0);
    if (RTXDI_IsValidDIReservoir(reservoir))
    {
        // the resampling may have taken a neighbor's light: evaluate it at this surface
        const RAB_LightInfo lightInfo = RAB_LoadLightInfo(RTXDI_GetDIReservoirLightIndex(reservoir), false);
        const float2 lightUv = RTXDI_GetDIReservoirSampleUV(reservoir);
        RAB_LightSample sample = RAB_SamplePolymorphicLight(lightInfo, surface, lightUv);
        const float3 shade = shadeLightSample(surface, sample);
        if (any(shade > 0.0))
        {
            float3 visibility = float3(0.0, 0.0, 0.0);
            bool isReused = false;
            if (constants.shading.reuseFinalVisibility != 0)
            {
                RTXDI_VisibilityReuseParameters reuse;
                reuse.maxAge = constants.shading.finalVisibilityMaxAge;
                reuse.maxDistance = constants.shading.finalVisibilityMaxDistance;
                isReused = RTXDI_GetDIReservoirVisibility(reservoir, reuse, visibility);
            }
            if (!isReused)
            {
                visibility = RAB_GetConservativeVisibility(surface, sample) ? float3(1.0, 1.0, 1.0)
                                                                           : float3(0.0, 0.0, 0.0);
                // the visibility shortcut also drops an occluded sample here, keeping M
                RTXDI_StoreVisibilityInDIReservoir(reservoir, visibility,
                                                   constants.temporalResampling.enableVisibilityShortcut != 0);
            }
            radiance = shade * visibility * RTXDI_GetDIReservoirInvPdf(reservoir);
            if (any(radiance > 0.0))
            {
                radiance *= glassTransmittance(root, surface, sample);
            }
        }
    }
    if (isInfiniteExact && RAB_IsSurfaceValid(surface) && surface.isLit)
    {
        radiance += shadeInfiniteLightsExactly(root, surface, constants.lightBuffer.infiniteLightBufferRegion);
    }
    // empty without candidates: the next frame with candidates never reads stale reservoirs
    RTXDI_StoreDIReservoir(reservoir, constants.reservoirBuffer, reservoirPosition,
                           constants.bufferIndices.shadingInputBufferIndex);
    storageTex2D<float4, kImageFormatRgba16f>(output)[id.xy] = float4(radiance, 1.0);
}
