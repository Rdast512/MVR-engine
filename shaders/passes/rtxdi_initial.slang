import types;
import core.binding;
import core.frame;

#include "lighting/rtxdi_bridge.slang"
#include "Rtxdi/DI/InitialSampling.hlsli"
#include "Rtxdi/DI/ReservoirStorage.hlsli"
#include "Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli"

// ReSTIR DI initial sampling (PBR plan F3): RIS over local and infinite light candidates for this frame's surface, one
// visibility ray on the chosen sample, then the reservoir rtxdi_shade reads at the same pixel. BRDF candidates wait for
// emissive triangles (M7): numBrdfSamples is 0 (src/render/vk_rtxdi.cpp)

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
    // shared by a tile: RIS tile selection (Power_RIS, M7)
    RTXDI_RandomSamplerState coherentRng = RTXDI_InitRandomSampler(id.xy / RTXDI_TILE_SIZE_IN_PIXELS, frameIndex,
                                                                   RTXDI_DI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED);

    const RAB_Surface surface = RAB_GetGBufferSurface(int2(id.xy), false);
    RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();
    if (RAB_IsSurfaceValid(surface))
    {
        RAB_LightSample lightSample;
        reservoir = RTXDI_SampleLightsForSurface(rng, coherentRng, surface, constants.initialSampling,
                                                 constants.lightBuffer, lightSample);
        if (constants.initialSampling.enableInitialVisibility != 0 && RTXDI_IsValidDIReservoir(reservoir) &&
            !RAB_GetConservativeVisibility(surface, lightSample))
        {
            // drops the sample, keeps M: the occluded candidates still count
            RTXDI_StoreVisibilityInDIReservoir(reservoir, float3(0.0, 0.0, 0.0), true);
        }
    }
    // empty pixels too: rtxdi_shade loads every pixel's reservoir
    RTXDI_StoreDIReservoir(reservoir, constants.reservoirBuffer,
                           RTXDI_PixelPosToReservoirPos(id.xy, constants.runtime.activeCheckerboardField),
                           constants.bufferIndices.initialSamplingOutputBufferIndex);
}
