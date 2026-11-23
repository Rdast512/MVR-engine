import types;
import core.binding;
import core.frame;

// noisyColor = direct + indirect + emissive (PBR plan G6, engine spec §11.6), the DLSS-RR color input; the only place
// the lighting terms meet. No AO here: direct never takes it, indirect applied it to the cache term

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
    const float3 emissive = tex2D(targets.current.sampled[kGuideEmissive]).Load(int3(id.xy, 0)).rgb;
    float3 color;
    if ((root.frameFlags & kFrameFlagCleanDlssInput) != 0)
    {
        // ENGINE_DLSS_CHECK (m8 I14 F25): noise-free analytical radiance identical across guide-perturbation controls
        float3 normal = tex2D(targets.current.sampled[kGuideNormalRoughness]).Load(int3(id.xy, 0)).xyz;
        if ((root.frameFlags & kFrameFlagInvertDlssNormal) != 0)
        {
            normal = -normal;
        }
        const float3 albedo = tex2D(targets.current.sampled[kGuideDiffuseAlbedo]).Load(int3(id.xy, 0)).rgb;
        const float3 lightDir = normalize(float3(0.4, 0.8, 0.3));
        const bool hasSurface = dot(normal, normal) > 0.0;
        color = hasSurface ? albedo * (0.25 + 0.75 * saturate(dot(normal, lightDir))) + emissive : float3(0.0, 0.0, 0.0);
    }
    else
    {
        const float3 direct = tex2D(targets.directRadianceSampled).Load(int3(id.xy, 0)).rgb;
        const float3 indirect = tex2D(targets.indirectRadianceSampled).Load(int3(id.xy, 0)).rgb;
        color = direct + indirect + emissive;
    }
    storageTex2D<float4, kImageFormatRgba16f>(targets.noisyColorStorage)[id.xy] = float4(color, 1.0);
}
