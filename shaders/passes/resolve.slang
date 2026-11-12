import types;
import core.binding;
import core.frame;
import core.math;
import geometry.surface;
import geometry.visbuffer;
import material.brdf;
import material.pbr;

// vis buffer -> shading surface -> guides of this frame's set (spec/features/visibility_buffer.md §3.5)

static const uint kResolveTile = 8; // matches src/render/vk_renderer.cpp

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

// render-resolution pixels from this frame's unjittered position to last frame's
float2 motionPixels(GpuFrameRoot root, float4 currentClip, float4 previousClip, float2 targetSize)
{
    // a point behind either camera has no screen position
    if (!hasHistory(root) || previousClip.w <= 0.0 || currentClip.w <= 0.0)
    {
        return float2(0.0, 0.0);
    }
    return (previousClip.xy / previousClip.w - currentClip.xy / currentClip.w) * 0.5 * targetSize;
}

void writeGuides(GpuGuideSet set, uint2 pixel, float4 normalRoughness, float2 geoNormal, float3 diffuseAlbedo,
                 float3 specularF0, float3 specAlbedo, float3 emissive, float2 motion)
{
    storageTex2D4(set.storage[kGuideNormalRoughness])[pixel] = normalRoughness;
    storageTex2D4(set.storage[kGuideGeoNormal])[pixel] = float4(geoNormal, 0.0, 0.0);
    storageTex2D4(set.storage[kGuideDiffuseAlbedo])[pixel] = float4(diffuseAlbedo, 1.0);
    storageTex2D4(set.storage[kGuideSpecularF0])[pixel] = float4(specularF0, 1.0);
    storageTex2D4(set.storage[kGuideSpecAlbedo])[pixel] = float4(specAlbedo, 1.0);
    storageTex2D4(set.storage[kGuideEmissive])[pixel] = float4(emissive, 0.0);
    storageTex2D4(set.storage[kGuideMotion])[pixel] = float4(motion, 0.0, 0.0);
}

[shader("compute")]
[numthreads(kResolveTile, kResolveTile, 1)]
void resolveMain(uint3 id : SV_DispatchThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    const GpuTargets targets = loadTargets(root);
    if (any(id.xy >= targets.renderSize))
    {
        return;
    }
    const GpuCameraData camera = loadCamera(root);
    const float2 targetSize = float2(targets.renderSize);
    const float2 ndc = pixelToNdc(float2(id.xy) + 0.5, targetSize);

    // reverse Z: the clear value 0 is empty sky; the payload of an empty pixel is undefined
    const float depth = tex2D(targets.depth).Load(int3(id.xy, 0)).r;
    if (depth == 0.0)
    {
        // a direction at infinity moves with the camera rotation only
        const float4 farPoint = mul(camera.invViewProj, float4(ndc, 0.0, 1.0));
        const float3 direction = farPoint.xyz / farPoint.w - camera.cameraPos;
        const float4 previousClip = mul(camera.prevViewProj, float4(direction, 0.0));
        const float4 currentClip = mul(camera.viewProj, float4(direction, 0.0));
        const float3 none = float3(0.0, 0.0, 0.0);
        writeGuides(targets.current, id.xy, float4(0.0, 0.0, 0.0, 0.0), float2(0.0, 0.0), none, none, none, none,
                    motionPixels(root, currentClip, previousClip, targetSize));
        return;
    }

    const uint payload = uintTex2D(targets.visBuffer).Load(int3(id.xy, 0));
    const DrawContext draw = loadDrawContext(root, visPayloadRow(payload));
    const TriangleCorners corners = loadTriangleCorners(draw, camera, visPayloadTriangle(payload));
    const Barycentrics bary =
        computeBarycentrics(corners.clip[0], corners.clip[1], corners.clip[2], ndc, 2.0 / targetSize);
    const Surface surface = buildSurface(draw, corners, bary);

    // render == display until upscaling (M3): no LOD bias
    const SampledMaterial material = sampleMaterial(loadMaterial(root, materialRow(draw.primitive)), surface, 0.0);

    const float3 view = normalize(camera.cameraPos - surface.position);
    const BrdfInputs brdfInputs = fromMaterial(material);
    const float3 specAlbedo = envBrdfApprox(brdfInputs.f0, saturate(dot(material.n, view)), material.roughness);
    const float4 currentClip = mul(camera.viewProj, float4(surface.position, 1.0));
    const float4 previousClip = mul(camera.prevViewProj, float4(surface.prevPosition, 1.0));
    writeGuides(targets.current, id.xy, float4(material.n, material.roughness), octEncode(surface.geoNormal),
                brdfInputs.cDiff, brdfInputs.f0, specAlbedo, material.emissive,
                motionPixels(root, currentClip, previousClip, targetSize));
}
