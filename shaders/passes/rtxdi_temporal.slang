import types;
import core.binding;
import core.frame;

static const uint kRtxdiTile = 8; // matches src/render/vk_renderer.cpp

// before the RTXDI headers: the filter's groupshared arrays are sized by the group
#define RTXDI_ENABLE_BOILING_FILTER
#define RTXDI_BOILING_FILTER_GROUP_SIZE 8 // kRtxdiTile

#include "lighting/rtxdi_bridge.slang"
// RTXDI 3.2 defaults it only in SpatialResampling.hlsli; undefined, the temporal bias correction compiles out
#define RTXDI_ALLOWED_BIAS_CORRECTION RTXDI_BIAS_CORRECTION_RAY_TRACED
#include "Rtxdi/DI/BoilingFilter.hlsli"
#include "Rtxdi/DI/TemporalResampling.hlsli"
#include "Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli"

// ReSTIR DI temporal resampling (spec/features/rtxdi_3_2_update.md §3.1): this pixel's initial reservoir against last
// frame's final reservoirs at the reprojected pixel and its neighbors, then the boiling filter. The output buffer is
// the initial one, written in place, so without temporal resampling or without history the pass writes nothing

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

[shader("compute")]
[numthreads(kRtxdiTile, kRtxdiTile, 1)]
void rtxdiTemporalMain(uint3 id : SV_DispatchThreadID, uint3 localId : SV_GroupThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    rtxdiBridgeInit(root);
    const GpuRtxdiConstants constants = loadRtxdiConstants(root);
    // uniform: after a reset the previous guides and reservoirs do not apply (undefined after a resize)
    if ((constants.resamplingMode & 1u) == 0 || !hasHistory(root))
    {
        return;
    }
    // no early return before the boiling filter: it synchronizes the whole group. Out of bounds the surface is empty
    const RAB_Surface surface = RAB_GetGBufferSurface(int2(id.xy), false);
    const uint2 reservoirPosition = RTXDI_PixelPosToReservoirPos(id.xy, constants.runtime.activeCheckerboardField);
    RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();
    if (RAB_IsSurfaceValid(surface))
    {
        const RTXDI_DIReservoir initial = RTXDI_LoadDIReservoir(
            constants.reservoirBuffer, reservoirPosition, constants.bufferIndices.initialSamplingOutputBufferIndex);
        RTXDI_RandomSamplerState rng =
            RTXDI_InitRandomSampler(id.xy, constants.runtime.frameIndex, RTXDI_DI_TEMPORAL_RESAMPLING_RANDOM_SEED);
        const float3 motion = tex2D(gRtxdiBridge.current.sampled[kGuideMotion]).Load(int3(id.xy, 0)).xyz;
        int2 temporalPixel;
        // the shade pass evaluates the final reservoir's light again
        RAB_LightSample selectedSample = RAB_EmptyLightSample();
        reservoir = RTXDI_DITemporalResampling(id.xy, surface, initial, rng, constants.runtime, constants.reservoirBuffer,
                                               motion, constants.bufferIndices.temporalResamplingInputBufferIndex,
                                               constants.temporalResampling, temporalPixel, selectedSample);
    }
    if (constants.boilingFilter.enableBoilingFilter != 0)
    {
        RTXDI_BoilingFilter(localId.xy, constants.boilingFilter.boilingFilterStrength, reservoir);
    }
    if (any(id.xy >= gRtxdiBridge.renderSize))
    {
        return;
    }
    RTXDI_StoreDIReservoir(reservoir, constants.reservoirBuffer, reservoirPosition,
                           constants.bufferIndices.temporalResamplingOutputBufferIndex);
}
