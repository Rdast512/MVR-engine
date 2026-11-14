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

// render-resolution pixels from this frame's unjittered position to last frame's; z is the view depth there minus
// here (clip w), RTXDI's expected previous linear depth offset
float3 motionPixels(GpuFrameRoot root, float4 currentClip, float4 previousClip, float2 targetSize)
{
    // a point behind either camera has no screen position
    if (!hasHistory(root) || previousClip.w <= 0.0 || currentClip.w <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }
    return float3((previousClip.xy / previousClip.w - currentClip.xy / currentClip.w) * 0.5 * targetSize,
                  previousClip.w - currentClip.w);
}

// diffuseAlbedo.a = ao; specularF0.a = specular weight (0: unlit, no specular lobe at all)
void writeGuides(GpuGuideSet set, uint2 pixel, float4 normalRoughness, float2 geoNormal, float4 diffuseAlbedo,
                 float4 specularF0, float3 specAlbedo, float3 emissive, float3 motion, float4 position)
{
    // formats: RenderTargets kGuideFormats
    storageTex2D<float4, kImageFormatRgba16f>(set.storage[kGuideNormalRoughness])[pixel] = normalRoughness;
    storageTex2D<float2, kImageFormatRg16Snorm>(set.storage[kGuideGeoNormal])[pixel] = geoNormal;
    storageTex2D<float4, kImageFormatRgba8>(set.storage[kGuideDiffuseAlbedo])[pixel] = diffuseAlbedo;
    storageTex2D<float4, kImageFormatRgba8>(set.storage[kGuideSpecularF0])[pixel] = specularF0;
    storageTex2D<float4, kImageFormatRgba8>(set.storage[kGuideSpecAlbedo])[pixel] = float4(specAlbedo, 1.0);
    storageTex2D<float3, kImageFormatR11g11b10f>(set.storage[kGuideEmissive])[pixel] = emissive;
    storageTex2D<float4, kImageFormatRgba16f>(set.storage[kGuideMotion])[pixel] = float4(motion, 0.0);
    storageTex2D<float4, kImageFormatRgba32f>(set.storage[kGuidePosition])[pixel] = position;
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
        const float4 none4 = float4(0.0, 0.0, 0.0, 0.0);
        // a direction has no depth: only the screen motion is meaningful
        const float3 motion = float3(motionPixels(root, currentClip, previousClip, targetSize).xy, 0.0);
        writeGuides(targets.current, id.xy, none4, float2(0.0, 0.0), none4, none4, none, none, motion, none4);
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
    // unlit: f0 and the DLSS specular albedo go to 0 with the weight
    const float3 f0 = brdfInputs.f0 * material.specularWeight;
    const float3 specAlbedo = envBrdfApprox(brdfInputs.f0, saturate(dot(material.n, view)), material.roughness) *
                              material.specularWeight;
    const float4 currentClip = mul(camera.viewProj, float4(surface.position, 1.0));
    const float4 previousClip = mul(camera.prevViewProj, float4(surface.prevPosition, 1.0));
    writeGuides(targets.current, id.xy, float4(material.n, material.roughness), octEncode(surface.geoNormal),
                float4(brdfInputs.cDiff, material.ao), float4(f0, material.specularWeight), specAlbedo,
                material.emissive, motionPixels(root, currentClip, previousClip, targetSize),
                float4(surface.position, currentClip.w));
}
