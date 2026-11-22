import types;
import core.binding;
import core.frame;
import core.math;
import geometry.surface;
import geometry.visbuffer;
import material.brdf;
import material.pbr;

// vis buffer -> shading surface -> guides of this frame's set (spec/features/visibility_buffer.md §3.5). One entry per
// material bin, launched by the resolve's DGC execute with one group per tile of the bin's list
// (spec/m7/e1_material_binning.md)

[[vk::push_constant]] ConstantBuffer<ResolvePushData> push;

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

// diffuseAlbedo.a = ao; surfaceGuide packs viewDepth, shading/geo normals, specularF0, roughness and isLit (m8 O4)
void writeGuides(GpuGuideSet set, uint2 pixel, float4 normalRoughness, float4 diffuseAlbedo, float3 specAlbedo,
                 float3 emissive, float3 motion, uint4 surfaceGuide)
{
    // formats: RenderTargets kGuideFormats
    storageTex2D<float4, kImageFormatRgba16f>(set.storage[kGuideNormalRoughness])[pixel] = normalRoughness;
    storageTex2D<float4, kImageFormatRgba8>(set.storage[kGuideDiffuseAlbedo])[pixel] = diffuseAlbedo;
    storageTex2D<float4, kImageFormatRgba8>(set.storage[kGuideSpecAlbedo])[pixel] = float4(specAlbedo, 1.0);
    storageTex2D<float3, kImageFormatR11g11b10f>(set.storage[kGuideEmissive])[pixel] = emissive;
    storageTex2D<float4, kImageFormatRgba16f>(set.storage[kGuideMotion])[pixel] = float4(motion, 0.0);
    storageTex2D<uint4, kImageFormatRgba32ui>(set.storage[kGuideSurface])[pixel] = surfaceGuide;
}

// empty pixel: zero guides, and the motion of a direction at infinity
void resolveBackground(GpuFrameRoot root, GpuTargets targets, uint2 pixel)
{
    const GpuCameraData camera = loadCamera(root);
    const float2 targetSize = float2(targets.renderSize);
    const float2 ndc = pixelToNdc(float2(pixel) + 0.5, targetSize);
    // a direction at infinity moves with the camera rotation only
    const float4 farPoint = mul(camera.invViewProj, float4(ndc, 0.0, 1.0));
    const float3 direction = farPoint.xyz / farPoint.w - camera.cameraPos;
    const float4 previousClip = mul(camera.prevViewProj, float4(direction, 0.0));
    const float4 currentClip = mul(camera.viewProj, float4(direction, 0.0));
    const float3 none = float3(0.0, 0.0, 0.0);
    const float4 none4 = float4(0.0, 0.0, 0.0, 0.0);
    // a direction has no depth: only the screen motion is meaningful
    const float3 motion = float3(motionPixels(root, currentClip, previousClip, targetSize).xy, 0.0);
    writeGuides(targets.current, pixel, none4, none4, none, none, motion, uint4(0u, 0u, 0u, 0u));
}

// covered pixel; isUnlit is a constant per entry
void resolveSurface(GpuFrameRoot root, GpuTargets targets, uint2 pixel, bool isUnlit)
{
    const GpuCameraData camera = loadCamera(root);
    const float2 targetSize = float2(targets.renderSize);
    const float2 ndc = pixelToNdc(float2(pixel) + 0.5, targetSize);

    const uint payload = uintTex2D(targets.visBuffer).Load(int3(pixel, 0));
    const DrawContext draw = loadDrawContext(root, visPayloadRow(payload));
    const TriangleCorners corners = loadTriangleCorners(draw, camera, visPayloadTriangle(payload));
    const Barycentrics bary =
        computeBarycentrics(corners.clip[0], corners.clip[1], corners.clip[2], ndc, 2.0 / targetSize);
    const Surface surface = buildSurface(draw, corners, bary);

    // log2(render / display): DLSS-RR reconstructs display-resolution texture detail
    const SampledMaterial material =
        sampleMaterial(loadMaterial(root, materialRow(draw.primitive)), surface, camera.lodBias, isUnlit);

    const float3 view = normalize(camera.cameraPos - surface.position);
    const BrdfInputs brdfInputs = fromMaterial(material);
    // unlit: f0 and the DLSS specular albedo go to 0 with the weight
    const float3 f0 = brdfInputs.f0 * material.specularWeight;
    const float3 specAlbedo = envBrdfApprox(brdfInputs.f0, saturate(dot(material.n, view)), material.roughness) *
                              material.specularWeight;
    const float4 currentClip = mul(camera.viewProj, float4(surface.position, 1.0));
    const float4 previousClip = mul(camera.prevViewProj, float4(surface.prevPosition, 1.0));
    const uint4 surfaceGuide = uint4(asuint(currentClip.w), packOct16(material.n), packOct16(surface.geoNormal),
                                     packSurfaceMaterial(f0, material.roughness, material.specularWeight > 0.0));
    writeGuides(targets.current, pixel, float4(material.n, material.roughness), float4(brdfInputs.cDiff, material.ao),
                specAlbedo, material.emissive, motionPixels(root, currentClip, previousClip, targetSize),
                surfaceGuide);
}

// the pixel of this thread in its group's tile; false outside the target or on another bin's pixel, so every pixel is
// written by exactly one bin
bool binPixel(GpuTargets targets, uint bin, uint3 groupId, uint3 threadId, out uint2 pixel)
{
    const uint tile = bdaPtr<uint>(push.tileList)[groupId.x];
    pixel = uint2(tile & 0xffffu, tile >> 16) * kMaterialTileSize + threadId.xy;
    return all(pixel < targets.renderSize) &&
           storageTex2D<uint, kImageFormatR8ui>(targets.binIdsStorage)[pixel] == bin;
}

[shader("compute")]
[numthreads(kMaterialTileSize, kMaterialTileSize, 1)]
void resolveBackgroundMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    const GpuTargets targets = loadTargets(root);
    uint2 pixel;
    if (binPixel(targets, kMaterialBinBackground, groupId, threadId, pixel))
    {
        resolveBackground(root, targets, pixel);
    }
}

[shader("compute")]
[numthreads(kMaterialTileSize, kMaterialTileSize, 1)]
void resolveStandardMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    const GpuTargets targets = loadTargets(root);
    uint2 pixel;
    if (binPixel(targets, kMaterialBinStandard, groupId, threadId, pixel))
    {
        resolveSurface(root, targets, pixel, false);
    }
}

[shader("compute")]
[numthreads(kMaterialTileSize, kMaterialTileSize, 1)]
void resolveUnlitMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    const GpuTargets targets = loadTargets(root);
    uint2 pixel;
    if (binPixel(targets, kMaterialBinUnlit, groupId, threadId, pixel))
    {
        resolveSurface(root, targets, pixel, true);
    }
}
