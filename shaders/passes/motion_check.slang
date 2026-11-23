import types;
import core.binding;
import core.frame;
import core.math;
import core.counters;

// motion vector self-test, ENGINE_MOTION_TEST only (spec/features/done/motion_vectors.md §5): checks every pixel's motion
// against the camera-only motion rebuilt from depth, and counts albedo reprojection errors. Reads the guides of this
// frame and the previous one after the resolve; writes only GpuMotionCheckStats

static const uint kMotionCheckTile = 8; // matches src/render/vk_renderer.cpp
// about 25 rgba8 steps: above the bilinear resampling error of an exactly reprojected texel
static const float kAlbedoErrorThreshold = 0.1;
// rg16f keeps 11 mantissa bits; the resolve interpolates the point, the check rebuilds it from depth
static const float kCameraErrorAbsolute = 0.05;
static const float kCameraErrorRelative = 1e-3;

[[vk::push_constant]] ConstantBuffer<MotionCheckPushData> push;

// the motion the resolve must write if no entity moved: last frame's projection of this pixel's world point
float2 cameraOnlyMotion(GpuCameraData camera, float2 ndc, float depth, float2 targetSize)
{
    const float2 unjitteredNdc = ndc - camera.jitterOffset;
    float4 previousClip;
    if (depth == 0.0)
    {
        // empty pixel: a direction at infinity, rotation only
        const float4 farPoint = mul(camera.invViewProj, float4(unjitteredNdc, 0.0, 1.0));
        previousClip = mul(camera.prevViewProj, float4(farPoint.xyz / farPoint.w - camera.cameraPos, 0.0));
    }
    else
    {
        const float4 world = mul(camera.invViewProj, float4(unjitteredNdc, depth, 1.0));
        previousClip = mul(camera.prevViewProj, float4(world.xyz / world.w, 1.0));
    }
    if (previousClip.w <= 0.0)
    {
        return -2.0 * targetSize;
    }
    return (previousClip.xy / previousClip.w - unjitteredNdc) * 0.5 * targetSize;
}

float maxComponent(float3 value)
{
    return max(value.x, max(value.y, value.z));
}

[shader("compute")]
[numthreads(kMotionCheckTile, kMotionCheckTile, 1)]
void motionCheckMain(uint3 id : SV_DispatchThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    const GpuTargets targets = loadTargets(root);
    if (any(id.xy >= targets.renderSize))
    {
        return;
    }
    GpuMotionCheckStats* stats = bdaPtr<GpuMotionCheckStats>(push.stats);
    if (all(id.xy == 0))
    {
        stats.frameFlags = root.frameFlags;
    }

    const GpuCameraData camera = loadCamera(root);
    const float2 targetSize = float2(targets.renderSize);
    const float2 ndc = pixelToNdc(float2(id.xy) + 0.5, targetSize);
    const float depth = tex2D(targets.depth).Load(int3(id.xy, 0)).r;
    const float2 motion = tex2D(targets.current.sampled[kGuideMotion]).Load(int3(id.xy, 0)).xy;

    waveCount(stats.pixels, true);
    waveCount(stats.emptyPixels, depth == 0.0);
    waveCount(stats.movingPixels, any(motion != 0.0));
    waveMax(stats.maxMotionBits, length(motion));
    // a reset frame has no previous frame to compare against: only its zero motion is checked
    if (!hasHistory(root))
    {
        return;
    }

    const float2 expected = cameraOnlyMotion(camera, ndc, depth, targetSize);
    const float cameraError = length(motion - expected);
    waveCount(stats.cameraMismatches, cameraError > kCameraErrorAbsolute + kCameraErrorRelative * length(expected));
    waveMax(stats.maxCameraErrorBits, cameraError);

    const float2 previousUv = (float2(id.xy) + 0.5 + motion) / targetSize;
    const bool isOffscreen = any(previousUv < 0.0) || any(previousUv > 1.0);
    waveCount(stats.offscreenPixels, isOffscreen);
    const float3 current = tex2D(targets.current.sampled[kGuideDiffuseAlbedo]).Load(int3(id.xy, 0)).rgb;
    const Texture2D previousAlbedo = tex2D(targets.previous.sampled[kGuideDiffuseAlbedo]);
    const SamplerState bilinear = samplerOf(targets.linearSampler);
    const float3 reprojected = previousAlbedo.SampleLevel(bilinear, previousUv, 0.0).rgb;
    const float3 still = previousAlbedo.Load(int3(id.xy, 0)).rgb;
    waveCount(stats.reprojectedErrors, !isOffscreen && maxComponent(abs(current - reprojected)) > kAlbedoErrorThreshold);
    waveCount(stats.stillErrors, !isOffscreen && maxComponent(abs(current - still)) > kAlbedoErrorThreshold);
}
