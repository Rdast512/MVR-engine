import types;
import core.binding;
import core.frame;

static const uint kRtxdiTile = 8; // matches src/render/vk_renderer.cpp

#include "lighting/rtxdi_bridge.slang"
#include "Rtxdi/DI/SpatialResampling.hlsli"
#include "Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli"

// ReSTIR DI spatial resampling and shading (spec/features/rtxdi_3_2_update.md §3.1): this pixel's temporal reservoir
// against this frame's neighbors, one final visibility ray, then directRadiance = evalBrdf · L_i · NdotL · W. Without
// spatial resampling (the reference) it shades the reservoir it loads. Neighbors are read from the temporal output
// and the result is written to the shading buffer, a different one, which is next frame's temporal input

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

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
    RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();
    if (RAB_IsSurfaceValid(surface))
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
        const RAB_LightSample sample =
            RAB_SamplePolymorphicLight(RAB_LoadLightInfo(RTXDI_GetDIReservoirLightIndex(reservoir), false), surface,
                                       RTXDI_GetDIReservoirSampleUV(reservoir));
        const float3 shade = shadeLightSample(surface, sample);
        if (any(shade > 0.0) && RAB_GetConservativeVisibility(surface, sample))
        {
            radiance = shade * RTXDI_GetDIReservoirInvPdf(reservoir);
        }
    }
    RTXDI_StoreDIReservoir(reservoir, constants.reservoirBuffer, reservoirPosition,
                           constants.bufferIndices.shadingInputBufferIndex);
    storageTex2D<float4, kImageFormatRgba16f>(output)[id.xy] = float4(radiance, 1.0);
}
