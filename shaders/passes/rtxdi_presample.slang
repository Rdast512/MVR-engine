import types;
import core.binding;
import core.frame;

#include "lighting/rtxdi_bridge.slang"
#include "Rtxdi/LightSampling/PresamplingFunctions.hlsli"

// local light presampling (spec/m7/f1_light_presampling.md), async compute after the PDF mips: the Power_RIS tiles,
// then ReGIR's cells from them. Ports PresampleLights.hlsl and PresampleReGIR.hlsl of the RTXDI sample
// (reference/RTXDI/Samples/FullSample/Shaders/LightingPasses/Presampling; NVIDIA RTX SDKs license), seeds included

// match src/render/vk_rtxdi.cpp: RTXDI_PRESAMPLING_GROUP_SIZE and the ReGIR build group of the sample
static const uint kPresampleGroupSize = 256;
static const uint kRegirBuildGroupSize = 256;

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

// one thread per tile entry: x the entry, y the tile
[shader("compute")]
[numthreads(kPresampleGroupSize, 1, 1)]
void rtxdiPresampleMain(uint3 id : SV_DispatchThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    rtxdiBridgeInit(root);
    GpuRtxdiConstants* constants = bdaPtr<GpuRtxdiConstants>(root.rtxdiConstants);
    const RTXDI_RISBufferSegmentParameters tiles = constants[0].localLightRisBuffer;
    if (id.x >= tiles.tileSize)
    {
        return;
    }
    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(id.xy, constants[0].runtime.frameIndex, 0);
    RTXDI_PresampleLocalLights(rng, lightPdfTexture(), uint2(gRtxdiBridge.lightPdfSide, gRtxdiBridge.lightPdfSide),
                               id.y, id.x, constants[0].lightBuffer.localLightBufferRegion, tiles);
}

// one thread per light slot of every cell
[shader("compute")]
[numthreads(kRegirBuildGroupSize, 1, 1)]
void rtxdiPresampleRegirMain(uint3 id : SV_DispatchThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    rtxdiBridgeInit(root);
    GpuRtxdiConstants* constants = bdaPtr<GpuRtxdiConstants>(root.rtxdiConstants);
    const uint frameIndex = constants[0].runtime.frameIndex;
    const uint slot = id.x;
    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(uint2(slot & 0xfff, slot >> 12), frameIndex, 1);
    // each 256 slots share one Power_RIS tile
    RTXDI_RandomSamplerState coherentRng = RTXDI_InitRandomSampler(uint2(slot >> 8, 0), frameIndex, 1);
    RTXDI_PresampleLocalLightsForReGIR(rng, coherentRng, slot, constants[0].lightBuffer.localLightBufferRegion,
                                       constants[0].localLightRisBuffer, regirParameters());
}
