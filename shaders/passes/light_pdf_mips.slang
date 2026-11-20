import types;
import core.binding;

// local light PDF mips (spec/m7/f1_light_presampling.md), one dispatch per level after light_world: each texel is the
// average of its 2x2 footprint, as GenerateMipsPass in the RTXDI sample. The texture is square, so every footprint
// lies inside the level above and the top mip is the mean power RAB_EvaluateLocalLightSourcePdf divides by

static const uint kLightPdfMipsTile = 8; // matches src/render/vk_rtxdi.cpp

[[vk::push_constant]] ConstantBuffer<LightPdfMipsPushData> push;

[shader("compute")]
[numthreads(kLightPdfMipsTile, kLightPdfMipsTile, 1)]
void lightPdfMipsMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= push.dstSize))
    {
        return;
    }
    // RtxdiContext's PDF format
    const RWTexture2D<float, 0, kImageFormatR32f> src = storageTex2D<float, kImageFormatR32f>(push.srcImage);
    const uint2 base = id.xy * 2;
    const float sum = src[base] + src[base + uint2(1, 0)] + src[base + uint2(0, 1)] + src[base + uint2(1, 1)];
    storageTex2D<float, kImageFormatR32f>(push.dstImage)[id.xy] = sum * 0.25;
}
