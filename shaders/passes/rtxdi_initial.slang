import types;
import core.binding;
import core.frame;

#include "lighting/rtxdi_bridge.slang"
#include "Rtxdi/DI/InitialSampling.hlsli"
#include "Rtxdi/DI/ReservoirStorage.hlsli"
#include "Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli"

// ReSTIR DI initial sampling (PBR plan F3): RIS over local, infinite and BRDF light candidates for this frame's
// surface, one visibility ray on the chosen sample, then the reservoir rtxdi_shade reads at the same pixel. Local
// candidates come from the presampled tiles of the configured mode (spec/m7/f1_light_presampling.md)

static const uint kRtxdiTile = 8; // matches src/render/vk_renderer.cpp

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

[shader("compute")]
[numthreads(kRtxdiTile, kRtxdiTile, 1)]
void rtxdiInitialMain(uint3 id : SV_DispatchThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    rtxdiBridgeInit(root);
    if (any(id.xy >= gRtxdiBridge.renderSize))
    {
        return;
    }
    const GpuRtxdiConstants constants = loadRtxdiConstants(root);
    const uint frameIndex = constants.runtime.frameIndex;
    RTXDI_RandomSamplerState rng =
        RTXDI_InitRandomSampler(id.xy, frameIndex, RTXDI_DI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED);
    // shared by a tile: its RIS tile (Power_RIS) and its ReGIR cell jitter
    RTXDI_RandomSamplerState coherentRng = RTXDI_InitRandomSampler(id.xy / RTXDI_TILE_SIZE_IN_PIXELS, frameIndex,
                                                                   RTXDI_DI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED);

    const RAB_Surface surface = RAB_GetGBufferSurface(int2(id.xy), false);
    RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();
    if (RAB_IsSurfaceValid(surface))
    {
        RTXDI_DIInitialSamplingParameters sampling = constants.initialSampling;
        // m8 I6 O8: without emissive triangles or an environment light no BRDF ray can hit a light
        if (gRtxdiBridge.triangleLights.lightCount == 0 &&
            constants.lightBuffer.environmentLightParams.lightPresent == 0)
        {
            sampling.numBrdfSamples = 0;
        }
        RAB_LightSample lightSample;
        // RTXDI_SampleLightsForSurface already traces initial visibility when enableInitialVisibility is set
        reservoir = RTXDI_SampleLightsForSurface(rng, coherentRng, surface, sampling, constants.lightBuffer,
                                                 constants.localLightRisBuffer, constants.environmentLightRisBuffer,
                                                 regirParameters(), lightSample);
    }
    // empty pixels too: rtxdi_shade loads every pixel's reservoir
    RTXDI_StoreDIReservoir(reservoir, constants.reservoirBuffer,
                           RTXDI_PixelPosToReservoirPos(id.xy, constants.runtime.activeCheckerboardField),
                           constants.bufferIndices.initialSamplingOutputBufferIndex);
}
