import types;
import core.binding;

// the RTXDI sample's Accumulation AA mode (AccumulationPass.hlsl; spec/features/planned/nvidia_settings_layout.md
// §4.4): a running mean of noisyColor over still frames, linear and before exposure. Input and output share the
// display extent while the denoiser is off, so no resampling

static const uint kAccumulateTile = 8;

[[vk::push_constant]] ConstantBuffer<AccumulatePushData> push;

[shader("compute")]
[numthreads(kAccumulateTile, kAccumulateTile, 1)]
void accumulateMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= push.size))
    {
        return;
    }
    const float4 color = tex2D(push.input).Load(int3(id.xy, 0));
    let accumulated = storageTex2D<float4, kImageFormatRgba32f>(push.accumulated);
    // weight 1: the first frame, whose target content is undefined
    accumulated[id.xy] = push.weight < 1.0 ? lerp(accumulated[id.xy], color, push.weight) : color;
}
