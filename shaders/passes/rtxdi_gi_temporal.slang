import types;
import core.binding;
import core.frame;

static const uint kRtxdiTile = 8; // matches src/render/vk_renderer.cpp

// before the RTXDI headers: the filter's groupshared arrays are sized by the group
#define RTXDI_ENABLE_BOILING_FILTER
#define RTXDI_BOILING_FILTER_GROUP_SIZE 8 // kRtxdiTile

#include "lighting/rtxdi_bridge.slang"
#include "Rtxdi/GI/BoilingFilter.hlsli"
#include "Rtxdi/GI/TemporalResampling.hlsli"
#include "Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli"

// ReSTIR GI temporal resampling (spec/m4/g5_restir_gi.md), as the RTXDI sample's GI/TemporalResampling.hlsl: this
// pixel's initial sample against last frame's final reservoirs at the reprojected pixel, then the boiling filter.
// Reads the initial array, which the final MIS still needs, and writes temporalResamplingOutputBufferIndex. Without
// history it copies the initial sample, so the spatial pass always reads this frame's reservoirs. Only dispatched in
// TemporalAndSpatial mode

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

[shader("compute")]
[numthreads(kRtxdiTile, kRtxdiTile, 1)]
void rtxdiGiTemporalMain(uint3 id : SV_DispatchThreadID, uint3 localId : SV_GroupThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    rtxdiBridgeInit(root);
    const GpuRtxdiConstants constants = loadRtxdiConstants(root);
    // uniform: after a reset the previous guides and reservoirs do not apply (undefined after a resize), after an
    // indirect mode change the reservoirs hold other samples
    const bool isHistoryValid = hasIndirectHistory(root);
    // no early return before the boiling filter: it synchronizes the whole group. Out of bounds the reservoir stays
    // empty so padding lanes never read unwritten VRAM into the group threshold
    const bool isInBounds = all(id.xy < gRtxdiBridge.renderSize);
    const RAB_Surface surface = RAB_GetGBufferSurface(int2(id.xy), false);
    const uint2 reservoirPosition = RTXDI_PixelPosToReservoirPos(id.xy, constants.runtime.activeCheckerboardField);
    RTXDI_GIReservoir reservoir = RTXDI_EmptyGIReservoir();
    if (isInBounds)
    {
        reservoir = RTXDI_LoadGIReservoir(constants.giReservoirBuffer, reservoirPosition, kGiInitialReservoirArray);
    }
    if (isHistoryValid && RAB_IsSurfaceValid(surface))
    {
        RTXDI_RandomSamplerState rng =
            RTXDI_InitRandomSampler(id.xy, constants.runtime.frameIndex, RTXDI_GI_TEMPORAL_RESAMPLING_RANDOM_SEED);
        const float3 motion = tex2D(gRtxdiBridge.current.sampled[kGuideMotion]).Load(int3(id.xy, 0)).xyz;
        RTXDI_GITemporalResamplingParameters temporal = constants.giTemporalResampling;
        // varied per pixel, as the sample: a disocclusion's reservoirs do not all expire on one frame
        temporal.maxReservoirAge = uint(float(temporal.maxReservoirAge) * (0.5 + RTXDI_GetNextRandom(rng) * 0.5));
        reservoir = RTXDI_GITemporalResampling(id.xy, surface, motion,
                                               constants.giBufferIndices.temporalResamplingInputBufferIndex, reservoir,
                                               rng, constants.runtime, constants.giReservoirBuffer, temporal);
    }
    if (isHistoryValid && constants.giBoilingFilter.enableBoilingFilter != 0)
    {
        RTXDI_GIBoilingFilter(localId.xy, constants.giBoilingFilter.boilingFilterStrength, reservoir);
    }
    if (!isInBounds)
    {
        return;
    }
    RTXDI_StoreGIReservoir(reservoir, constants.giReservoirBuffer, reservoirPosition,
                           constants.giBufferIndices.temporalResamplingOutputBufferIndex);
}
