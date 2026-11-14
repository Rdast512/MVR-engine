import types;
import core.binding;
import core.frame;

// DLSS-RR output × exposure -> one tonemap curve (ACES, Stephen Hill's fit of the RRT + sRGB ODT) -> the post target,
// linear [0, 1]; present writes it into the sRGB swapchain. The only tonemap in the frame (PBR plan H2)

static const uint kPostTile = 8;

// sRGB -> AP1 with the RRT's saturation, and back (Hill's ACESFitted)
static const float3x3 kAcesInput = float3x3(
    0.59719, 0.35458, 0.04823,
    0.07600, 0.90834, 0.01566,
    0.02840, 0.13383, 0.83777);
static const float3x3 kAcesOutput = float3x3(
    1.60475, -0.53108, -0.07367,
    -0.10208, 1.10813, -0.00605,
    -0.00327, -0.07276, 1.07602);

float3 rrtAndOdtFit(float3 v)
{
    const float3 a = v * (v + 0.0245786) - 0.000090537;
    const float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

float3 acesFitted(float3 color)
{
    return saturate(mul(kAcesOutput, rrtAndOdtFit(mul(kAcesInput, color))));
}

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

[shader("compute")]
[numthreads(kPostTile, kPostTile, 1)]
void postMain(uint3 id : SV_DispatchThreadID)
{
    const GpuTargets targets = loadTargets(bdaPtr<GpuFrameRoot>(push.frameRoot)[0]);
    if (any(id.xy >= targets.displaySize))
    {
        return;
    }
    const float exposure = storageTex2D<float, kImageFormatR32f>(targets.exposureStorage)[uint2(0, 0)];
    const float3 color = tex2D(targets.upscaledSampled).Load(int3(id.xy, 0)).rgb * exposure;
    storageTex2D<float4, kImageFormatRgba16f>(targets.postStorage)[id.xy] = float4(acesFitted(max(color, 0.0)), 1.0);
}
