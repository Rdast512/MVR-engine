import types;
import core.binding;
import core.frame;

#include "lighting/rtxdi_bridge.slang"
#include "Rtxdi/GI/Reservoir.hlsli"
#include "gi/nrc_bridge.slang"

// NRC custom resolve (NRC guide step 7, spec/m4/g3_nrc.md): after QueryAndTrain, the cache's answer for each pixel's
// query at X2 joins the radiance of the pixel's initial ReSTIR GI reservoir. The reservoir weight is 1/pdf and does not
// depend on radiance, so the add keeps it valid (G2 §1). Material AO of the primary applies to this cache term only
// (engine spec §11.6)

static const uint kResolveTile = 8; // matches src/render/vk_renderer.cpp kFullscreenTile

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

[shader("compute")]
[numthreads(kResolveTile, kResolveTile, 1)]
void nrcResolveMain(uint3 id : SV_DispatchThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    rtxdiBridgeInit(root);
    if (any(id.xy >= gRtxdiBridge.renderSize))
    {
        return;
    }
    const float3 cached = nrcQueryResult(loadNrcConstants(root), id.xy);
    if (all(cached <= 0.0))
    {
        return;
    }
    const float ao = tex2D(gRtxdiBridge.current.sampled[kGuideDiffuseAlbedo]).Load(int3(int2(id.xy), 0)).a;
    const GpuRtxdiConstants constants = loadRtxdiConstants(root);
    const uint2 reservoirPos = RTXDI_PixelPosToReservoirPos(id.xy, constants.runtime.activeCheckerboardField);
    const uint arrayIndex = constants.giBufferIndices.secondarySurfaceReSTIRDIOutputBufferIndex;
    RTXDI_GIReservoir reservoir = RTXDI_LoadGIReservoir(constants.giReservoirBuffer, reservoirPos, arrayIndex);
    if (!RTXDI_IsValidGIReservoir(reservoir))
    {
        return;
    }
    reservoir.radiance += cached * ao;
    RTXDI_StoreGIReservoir(reservoir, constants.giReservoirBuffer, reservoirPos, arrayIndex);
}
