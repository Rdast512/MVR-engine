import types;
import core.binding;
import core.frame;

// noisyColor = direct + indirect + emissive (PBR plan G6, engine spec §11.6), the DLSS-RR color input; the only place
// the lighting terms meet. No AO here: direct never takes it, indirect applied it to the cache term

static const uint kCombineTile = 8; // matches src/render/vk_renderer.cpp kFullscreenTile

float lum(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// the 3x3 neighbors' mean luminance of this frame's noisy input, the center excluded
float neighborLuminance(GpuTargets targets, int2 pixel, bool isDirectEnabled, bool isIndirectEnabled)
{
    float total = 0.0;
    float count = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            const int2 neighbor = pixel + int2(x, y);
            if ((x == 0 && y == 0) || any(neighbor < 0) || any(neighbor >= int2(targets.renderSize)))
            {
                continue;
            }
            const float3 direct = isDirectEnabled
                ? tex2D(targets.directRadianceSampled).Load(int3(neighbor, 0)).rgb
                : float3(0.0, 0.0, 0.0);
            const float3 indirect = isIndirectEnabled
                ? tex2D(targets.indirectRadianceSampled).Load(int3(neighbor, 0)).rgb
                : float3(0.0, 0.0, 0.0);
            const float3 neighborLighting = direct + indirect;
            if (all(isfinite(neighborLighting)))
            {
                total += lum(neighborLighting);
                count += 1.0;
            }
        }
    }
    return count > 0.0 ? total / count : 0.0;
}

// the largest luminance of DLSS-RR's last output around where this pixel was last frame; < 0 without one
float historyLuminance(GpuFrameRoot root, GpuTargets targets, int2 pixel)
{
    if ((root.frameFlags & kFrameFlagDlssHistoryValid) == 0)
    {
        return -1.0;
    }
    const float2 motion = tex2D(targets.current.sampled[kGuideMotion]).Load(int3(pixel, 0)).xy;
    const float2 previous = (float2(pixel) + 0.5 + motion) / float2(targets.renderSize);
    if (any(previous < 0.0) || any(previous >= 1.0))
    {
        return -1.0;
    }
    const int2 center = int2(previous * float2(targets.displaySize));
    float largest = 0.0;
    bool hasFiniteSample = false;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            const int2 texel = clamp(center + int2(x, y), int2(0, 0), int2(targets.displaySize) - 1);
            const float3 sample = tex2D(targets.upscaledSampled).Load(int3(texel, 0)).rgb;
            if (all(isfinite(sample)))
            {
                largest = max(largest, lum(sample));
                hasFiniteSample = true;
            }
        }
    }
    if (!hasFiniteSample)
    {
        return -1.0;
    }
    // the output carries the input scale
    return root.exposure != 1.0 ? largest / max(root.exposure, 1e-6) : largest;
}

// full_testing T11: the lighting's luminance held to root.preDenoiserClamp times the reference, emission untouched
float3 clampLighting(GpuFrameRoot root, GpuTargets targets, int2 pixel, float3 lighting, bool isDirectEnabled,
                     bool isIndirectEnabled)
{
    const float reference = (root.frameFlags & kFrameFlagPreDenoiserClampNeighbors) != 0
        ? neighborLuminance(targets, pixel, isDirectEnabled, isIndirectEnabled)
        : historyLuminance(root, targets, pixel);
    const float luminance = lum(lighting);
    const float bound = root.preDenoiserClamp * reference;
    return reference > 0.0 && luminance > bound ? lighting * (bound / luminance) : lighting;
}

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
        const bool isDirectEnabled =
            (root.frameFlags & (kFrameFlagDirectDisabled | kFrameFlagDlssInputIndirectOnly)) == 0;
        const bool isIndirectEnabled =
            (root.frameFlags & (kFrameFlagIndirectDisabled | kFrameFlagDlssInputDirectOnly)) == 0;
        const float3 direct = isDirectEnabled
            ? tex2D(targets.directRadianceSampled).Load(int3(id.xy, 0)).rgb
            : float3(0.0, 0.0, 0.0);
        const float3 indirect = isIndirectEnabled
            ? tex2D(targets.indirectRadianceSampled).Load(int3(id.xy, 0)).rgb
            : float3(0.0, 0.0, 0.0);
        float3 lighting = direct + indirect;
        if (root.preDenoiserClamp > 0.0)
        {
            lighting = clampLighting(root, targets, int2(id.xy), lighting, isDirectEnabled, isIndirectEnabled);
        }
        // the exposure after DLSS-RR meters its output, so a scale here changes only what the network sees
        color = lighting + emissive;
        if (root.exposure != 1.0)
        {
            color *= max(root.exposure, 1e-6);
        }
        const bool isAccumulating = (root.frameFlags & kFrameFlagDlssInputAccumulate) != 0;
        const bool isFrozen = (root.frameFlags & kFrameFlagDlssInputFrozen) != 0;
        if (targets.dlssInputAccumulationStorage != kNoneIndex)
        {
            if (isAccumulating || isFrozen)
            {
                let accumulation = storageTex2D<float4, kImageFormatRgba32f>(targets.dlssInputAccumulationStorage);
                const bool isRestarted = isAccumulating && (root.frameFlags & kFrameFlagHistoryReset) != 0;
                const float4 stored = accumulation[id.xy];
                float4 sum = isRestarted || !all(isfinite(stored)) ? float4(0.0, 0.0, 0.0, 0.0) : stored;
                if (isAccumulating && all(isfinite(color)))
                {
                    sum += float4(color, 1.0);
                    accumulation[id.xy] = sum;
                }
                color = sum.w > 0.0 ? sum.rgb / sum.w : color;
            }
            else if (root.inputHistoryWeight > 0.0)
            {
                // a still camera's exponential average; motion or a reset restarts it at this frame's value
                let average = storageTex2D<float4, kImageFormatRgba32f>(targets.dlssInputAccumulationStorage);
                const float2 motion = tex2D(targets.current.sampled[kGuideMotion]).Load(int3(id.xy, 0)).xy;
                // a still camera's motion is float noise of two separately computed matrices, not zero
                const bool isKept = (root.frameFlags & kFrameFlagHistoryReset) == 0 && all(abs(motion) < 0.01);
                const float4 history = average[id.xy];
                color = isKept && history.w > 0.0 && all(isfinite(history))
                    ? lerp(history.rgb, color, saturate(root.inputHistoryWeight))
                    : color;
                if (all(isfinite(color)))
                {
                    average[id.xy] = float4(color, 1.0);
                }
            }
        }
    }
    storageTex2D<float4, kImageFormatRgba16f>(targets.noisyColorStorage)[id.xy] = float4(color, 1.0);
}
