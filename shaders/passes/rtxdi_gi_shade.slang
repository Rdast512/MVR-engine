import types;
import core.binding;
import core.frame;

static const uint kRtxdiTile = 8; // matches src/render/vk_renderer.cpp

#include "lighting/rtxdi_bridge.slang"
#include "Rtxdi/GI/SpatialResampling.hlsli"
#include "Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli"

// ReSTIR GI spatial resampling and final shading (spec/m4/g5_restir_gi.md), the RTXDI sample's GI/SpatialResampling.hlsl
// and GI/FinalShading.hlsl fused as rtxdi_shade fuses DI's: this pixel's temporal reservoir against this frame's
// neighbors, then indirectRadiance = evalBrdf · Lo(X2) · NdotL · W with a reconnection ray and the glossy MIS against
// the pixel's initial sample. Neighbors are read from the temporal output; the result goes to the spatial output,
// next frame's temporal input. The reference (resampling None) shades the initial sample alone

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

// the RTXDI sample's FinalShading.hlsl
static const float kMisRoughness = 0.3;
static const float kMaxBrdfValue = 1.0e4;

// the initial sample's MIS share at a direction: high where the true lobe is far sharper than one roughened to
// kMisRoughness, where reuse from neighbors and history is least reliable
float glossyMisWeight(RAB_Surface surface, float3 samplePosition)
{
    RAB_Surface roughened = surface;
    roughened.roughness = max(roughened.roughness, kMisRoughness);
    const float3 unit = float3(1.0, 1.0, 1.0);
    const float3 roughBrdf = clamp(shadeGiSample(roughened, samplePosition, unit), 1.0e-4, kMaxBrdfValue);
    const float3 trueBrdf = clamp(shadeGiSample(surface, samplePosition, unit), 0.0, kMaxBrdfValue);
    const float weight = saturate(luminance(trueBrdf) / luminance(trueBrdf + roughBrdf));
    return weight * weight * weight;
}

float3 shadeFinalReservoir(RAB_Surface surface, RTXDI_GIReservoir reservoir, RTXDI_GIReservoir initial,
                           RTXDI_GIFinalShadingParameters params)
{
    if (!RTXDI_IsValidGIReservoir(reservoir))
    {
        return float3(0.0, 0.0, 0.0);
    }
    float3 radiance = shadeGiSample(surface, reservoir.position, reservoir.radiance) * reservoir.weightSum;
    // the sample may be a neighbor's or last frame's: X2 can be hidden from this surface
    if (params.enableFinalVisibility != 0 && any(radiance > 0.0) &&
        !isReconnectionVisible(surface, reservoir.position))
    {
        radiance = float3(0.0, 0.0, 0.0);
    }
    if (params.enableFinalMIS == 0)
    {
        return radiance;
    }
    radiance *= 1.0 - glossyMisWeight(surface, reservoir.position);
    // traced from this surface this frame: visible by construction
    if (RTXDI_IsValidGIReservoir(initial))
    {
        radiance += shadeGiSample(surface, initial.position, initial.radiance) * initial.weightSum *
                    glossyMisWeight(surface, initial.position);
    }
    return radiance;
}

[shader("compute")]
[numthreads(kRtxdiTile, kRtxdiTile, 1)]
void rtxdiGiShadeMain(uint3 id : SV_DispatchThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    rtxdiBridgeInit(root);
    if (any(id.xy >= gRtxdiBridge.renderSize))
    {
        return;
    }
    // RenderTargets::kIndirectRadianceFormat
    const uint output = bdaPtr<GpuTargets>(root.targets)[0].indirectRadianceStorage;
    const GpuRtxdiConstants constants = loadRtxdiConstants(root);
    const RAB_Surface surface = RAB_GetGBufferSurface(int2(id.xy), false);
    const uint2 reservoirPosition = RTXDI_PixelPosToReservoirPos(id.xy, constants.runtime.activeCheckerboardField);
    const RTXDI_GIReservoir initial =
        RTXDI_LoadGIReservoir(constants.giReservoirBuffer, reservoirPosition, kGiInitialReservoirArray);

    float3 radiance = float3(0.0, 0.0, 0.0);
    if (constants.giResamplingMode == 0)
    {
        // the unbiased 1-sample estimate the reuse converges to; the MIS of a sample with itself is the identity
        if (RTXDI_IsValidGIReservoir(initial))
        {
            radiance = shadeGiSample(surface, initial.position, initial.radiance) * initial.weightSum;
        }
    }
    else
    {
        const uint inputArray = constants.giBufferIndices.spatialResamplingInputBufferIndex;
        RTXDI_GIReservoir reservoir =
            RTXDI_LoadGIReservoir(constants.giReservoirBuffer, reservoirPosition, inputArray);
        if ((constants.giResamplingMode & 2u) != 0 && RAB_IsSurfaceValid(surface))
        {
            RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(id.xy, constants.runtime.frameIndex,
                                                                   RTXDI_GI_SPATIAL_RESAMPLING_RANDOM_SEED);
            reservoir = RTXDI_GISpatialResampling(id.xy, surface, inputArray, reservoir, rng, constants.runtime,
                                                  constants.giReservoirBuffer, constants.giSpatialResampling);
        }
        RTXDI_StoreGIReservoir(reservoir, constants.giReservoirBuffer, reservoirPosition,
                               constants.giBufferIndices.spatialResamplingOutputBufferIndex);
        radiance = shadeFinalReservoir(surface, reservoir, initial, constants.giFinalShading);
    }
    storageTex2D<float4, kImageFormatRgba16f>(output)[id.xy] = float4(radiance, 1.0);
}
