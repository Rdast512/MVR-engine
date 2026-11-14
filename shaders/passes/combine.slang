import types;
import core.binding;
import core.frame;

// noisyColor = direct + emissive (PBR plan G6), the DLSS-RR color input. The indirect term joins in M4; this is the
// only place the lighting terms meet

static const uint kCombineTile = 8; // matches src/render/vk_renderer.cpp kFullscreenTile

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

[shader("compute")]
[numthreads(kCombineTile, kCombineTile, 1)]
void combineMain(uint3 id : SV_DispatchThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    const GpuTargets targets = loadTargets(root);
    if (any(id.xy >= targets.renderSize))
    {
        return;
    }
    const float3 direct = tex2D(targets.directRadianceSampled).Load(int3(id.xy, 0)).rgb;
    const float3 emissive = tex2D(targets.current.sampled[kGuideEmissive]).Load(int3(id.xy, 0)).rgb;
    storageTex2D<float4, kImageFormatRgba16f>(targets.noisyColorStorage)[id.xy] = float4(direct + emissive, 1.0);
}
