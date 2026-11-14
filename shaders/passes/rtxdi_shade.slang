import types;
import core.binding;
import core.frame;

static const uint kRtxdiTile = 8; // matches src/render/vk_renderer.cpp

// before the RTXDI headers: the filter's groupshared arrays are sized by the group
#define RTXDI_ENABLE_BOILING_FILTER
#define RTXDI_BOILING_FILTER_GROUP_SIZE 8 // kRtxdiTile

#include "lighting/rtxdi_bridge.slang"
#include "Rtxdi/DI/BoilingFilter.hlsli"
#include "Rtxdi/DI/SpatioTemporalResampling.hlsli"
#include "Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli"

// ReSTIR DI fused spatiotemporal resampling and shading (PBR plan F3): this pixel's initial reservoir against last
// frame's reservoirs at the reprojected pixel and its neighbors, the boiling filter, one final visibility ray, then
// directRadiance = evalBrdf · L_i · NdotL · W. The reference configuration (isFusedResampling 0) and a history reset
// shade the initial reservoir instead. The result is stored for next frame's reuse

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

[shader("compute")]
[numthreads(kRtxdiTile, kRtxdiTile, 1)]
void rtxdiShadeMain(uint3 id : SV_DispatchThreadID, uint3 localId : SV_GroupThreadID)
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
    const GpuRtxdiConstants constants = loadRtxdiConstants(root);
    // no early return before the boiling filter: it synchronizes the whole group. Out of bounds the surface is empty
    const RAB_Surface surface = RAB_GetGBufferSurface(int2(id.xy), false);
    const uint2 reservoirPosition = RTXDI_PixelPosToReservoirPos(id.xy, constants.runtime.activeCheckerboardField);
    RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();
    if (RAB_IsSurfaceValid(surface))
    {
        reservoir = RTXDI_LoadDIReservoir(constants.reservoirBuffer, reservoirPosition,
                                          constants.bufferIndices.initialSamplingOutputBufferIndex);
    }

    // uniform: after a reset the previous guides and reservoirs do not apply (undefined after a resize)
    if (constants.isFusedResampling != 0 && hasHistory(root))
    {
        if (RAB_IsSurfaceValid(surface))
        {
            RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(id.xy, constants.runtime.frameIndex,
                                                                   RTXDI_DI_TEMPORAL_RESAMPLING_RANDOM_SEED);
            const float3 motion = tex2D(gRtxdiBridge.current.sampled[kGuideMotion]).Load(int3(id.xy, 0)).xyz;
            int2 temporalPixel;
            // re-evaluated below from the final reservoir
            RAB_LightSample selectedSample = RAB_EmptyLightSample();
            reservoir = RTXDI_DISpatioTemporalResampling(
                id.xy, surface, reservoir, rng, motion, constants.bufferIndices.temporalResamplingInputBufferIndex,
                constants.runtime, constants.reservoirBuffer, constants.spatioTemporalResampling, temporalPixel,
                selectedSample);
        }
        if (constants.boilingFilter.enableBoilingFilter != 0)
        {
            RTXDI_BoilingFilter(localId.xy, constants.boilingFilter.boilingFilterStrength, reservoir);
        }
    }
    if (any(id.xy >= gRtxdiBridge.renderSize))
    {
        return;
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
