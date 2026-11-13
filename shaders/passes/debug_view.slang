import types;
import core.binding;
import core.frame;
import core.math;
import geometry.hit;
import geometry.surface;
import geometry.visbuffer;
import material.pbr;

// the display image until lighting (M3): the albedo guide, or the selected debug view (src/render/debug_view.hpp)

static const uint kDebugViewTile = 8; // matches src/render/vk_renderer.cpp

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

// pcg: neighbouring ids map to unrelated colors
uint hashId(uint value)
{
    const uint state = value * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float3 idColor(uint id)
{
    const uint hash = hashId(id);
    const float hue = float(hash & 0xffffu) / 65535.0;
    const float saturation = 0.55 + 0.45 * float((hash >> 16) & 0xffu) / 255.0;
    const float3 rgb = saturate(abs(frac(hue + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
    return lerp(float3(1.0, 1.0, 1.0), rgb, saturation);
}

bool isIdView(uint mode)
{
    return mode >= kDebugViewMeshlets && mode <= kDebugViewCullPhase;
}

bool isRayView(uint mode)
{
    return mode >= kDebugViewRayMeshlets && mode <= kDebugViewRayShadingNormal;
}

// flat headlight from the geometric normal keeps the shape readable under flat colors
float headlight(float3 geoNormal, float3 view)
{
    return 0.35 + 0.65 * abs(dot(geoNormal, view));
}

// the id the geometry views color by, from the decoded payload
float3 idViewColor(uint mode, DrawContext draw, uint triangle)
{
    // model-relative: instances of one model show the same meshlet colors
    const uint meshletId = draw.visible.meshletRow;
    switch (mode)
    {
    case kDebugViewMeshlets:
        return idColor(meshletId);
    case kDebugViewPrimitives:
        // model-relative row, made distinct across models
        return idColor(draw.primitiveRow ^ (draw.nodeInstance.modelIndex * 0x9E3779B1u));
    case kDebugViewNodeInstances:
        return idColor(draw.visible.nodeInstanceIndex);
    case kDebugViewBins:
        // opaque one-sided grey, opaque two-sided green (alpha-tested lands in M6)
        return draw.isDoubleSided ? float3(0.3, 0.85, 0.35) : float3(0.7, 0.7, 0.7);
    case kDebugViewCullPhase:
        // early blue, late orange
        return draw.visibleRow >= draw.root.lateVisibleBase ? float3(1.0, 0.55, 0.1) : float3(0.25, 0.5, 1.0);
    default:
        return idColor((meshletId << kVisTriangleBits) | triangle);
    }
}

// a primary ray through the TLAS, colored like the raster view it must match (plan C3); black on a miss
float3 rayViewColor(uint mode, GpuFrameRoot root, GpuCameraData camera, uint2 pixel, float2 targetSize)
{
    const PrimaryRay primary = primaryRay(camera, pixel, targetSize);
    RayHit hit;
    if (!traceClosest(root, primary.ray, hit))
    {
        return float3(0.0, 0.0, 0.0);
    }
    const DecodedHit decoded = decodeHit(root, hit, primary.ray.Direction, primary.cone);
    if (mode == kDebugViewRayShadingNormal)
    {
        // ray-cone gradients: the normal map's mip comes from the cone, not from screen derivatives
        const GpuMaterial material = loadMaterial(root, materialRow(decoded.draw.primitive));
        return sampleMaterial(material, decoded.surface, 0.0).n * 0.5 + 0.5;
    }
    const uint rasterMode = mode == kDebugViewRayMeshlets ? kDebugViewMeshlets : kDebugViewNodeInstances;
    return idViewColor(rasterMode, decoded.draw, hit.triangle) *
        headlight(decoded.surface.geoNormal, -primary.ray.Direction);
}

float4 loadGuide(GpuGuideSet set, uint guide, uint2 pixel)
{
    return tex2D(set.sampled[guide]).Load(int3(pixel, 0));
}

// log-scaled linear depth, as passes/hiz_view.slang: near white, far black
float depthShade(float depth, GpuCameraData camera)
{
    const float linearZ = camera.nearZ * camera.farZ / (camera.nearZ + depth * (camera.farZ - camera.nearZ));
    return 1.0 - saturate(log(linearZ / camera.nearZ) / log(camera.farZ / camera.nearZ));
}

// this frame's albedo against last frame's at the reprojected position (spec/features/motion_vectors.md §5)
float3 reprojectionError(GpuTargets targets, uint2 pixel)
{
    const float2 motion = loadGuide(targets.current, kGuideMotion, pixel).xy;
    const float2 uv = (float2(pixel) + 0.5 + motion) / float2(targets.renderSize);
    const float3 previous = tex2D(targets.previous.sampled[kGuideDiffuseAlbedo])
                                .SampleLevel(samplerOf(targets.linearSampler), uv, 0.0)
                                .rgb;
    return saturate(abs(loadGuide(targets.current, kGuideDiffuseAlbedo, pixel).rgb - previous) * 4.0);
}

float3 guideViewColor(uint mode, GpuFrameRoot root, GpuTargets targets, GpuCameraData camera, uint2 pixel, float depth)
{
    const GpuGuideSet set = targets.current;
    switch (mode)
    {
    case kDebugViewDepth:
        return depthShade(depth, camera).xxx;
    case kDebugViewShadingNormal:
        return loadGuide(set, kGuideNormalRoughness, pixel).xyz * 0.5 + 0.5;
    case kDebugViewRoughness:
        return loadGuide(set, kGuideNormalRoughness, pixel).www;
    case kDebugViewGeometricNormal:
        return depth == 0.0 ? float3(0.0, 0.0, 0.0)
                            : octDecode(loadGuide(set, kGuideGeoNormal, pixel).xy) * 0.5 + 0.5;
    case kDebugViewSpecularF0:
        return loadGuide(set, kGuideSpecularF0, pixel).rgb;
    case kDebugViewSpecularAlbedo:
        return loadGuide(set, kGuideSpecAlbedo, pixel).rgb;
    case kDebugViewEmissive:
        return loadGuide(set, kGuideEmissive, pixel).rgb;
    case kDebugViewMotionVectors:
    {
        // direction as hue-like red/green, 16 px of motion saturates
        const float2 motion = loadGuide(set, kGuideMotion, pixel).xy;
        return float3(saturate(abs(motion) / 16.0), 0.0) + (all(motion == 0.0) ? 0.0 : 0.1);
    }
    case kDebugViewReprojection:
        // a reset frame has nothing valid to compare against
        return hasHistory(root) ? reprojectionError(targets, pixel) : float3(0.0, 0.0, 0.0);
    default:
        return loadGuide(set, kGuideDiffuseAlbedo, pixel).rgb;
    }
}

[shader("compute")]
[numthreads(kDebugViewTile, kDebugViewTile, 1)]
void debugViewMain(uint3 id : SV_DispatchThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    const GpuTargets targets = loadTargets(root);
    if (any(id.xy >= targets.renderSize))
    {
        return;
    }
    const GpuCameraData camera = loadCamera(root);
    const uint mode = root.debugView & kDebugViewModeMask;
    const float depth = tex2D(targets.depth).Load(int3(id.xy, 0)).r;
    const bool isEmpty = depth == 0.0;
    const bool isEdges = (root.debugView & kDebugViewFlagEdges) != 0;

    float3 color = float3(0.0, 0.0, 0.0);
    if (isRayView(mode))
    {
        color = rayViewColor(mode, root, camera, id.xy, float2(targets.renderSize));
    }
    else if (!isIdView(mode))
    {
        color = guideViewColor(mode, root, targets, camera, id.xy, depth);
    }
    if (!isEmpty && (isIdView(mode) || isEdges))
    {
        const uint payload = uintTex2D(targets.visBuffer).Load(int3(id.xy, 0));
        const DrawContext draw = loadDrawContext(root, visPayloadRow(payload));
        const uint triangle = visPayloadTriangle(payload);
        if (isIdView(mode))
        {
            const float3 geoNormal = octDecode(loadGuide(targets.current, kGuideGeoNormal, id.xy).xy);
            const float2 ndc = pixelToNdc(float2(id.xy) + 0.5, float2(targets.renderSize));
            const float4 farPoint = mul(camera.invViewProj, float4(ndc, 0.0, 1.0));
            const float3 view = normalize(camera.cameraPos - farPoint.xyz / farPoint.w);
            color = idViewColor(mode, draw, triangle) * headlight(geoNormal, view);
        }
        if (isEdges)
        {
            // about one pixel wide at any distance, from the analytic barycentric derivatives
            const TriangleCorners corners = loadTriangleCorners(draw, camera, triangle);
            const Barycentrics bary = computeBarycentrics(corners.clip[0], corners.clip[1], corners.clip[2],
                                                          pixelToNdc(float2(id.xy) + 0.5, float2(targets.renderSize)),
                                                          2.0 / float2(targets.renderSize));
            const float3 edgeDistance = bary.lambda / max(abs(bary.ddx) + abs(bary.ddy), 1e-6);
            const float edge = 1.0 - saturate(min(edgeDistance.x, min(edgeDistance.y, edgeDistance.z)));
            color = lerp(color, float3(0.0, 0.0, 0.0), edge);
        }
    }
    // RenderTargets::kDisplayFormat
    storageTex2D<float4, kImageFormatRgba16f>(targets.displayStorage)[id.xy] = float4(color, 1.0);
}
