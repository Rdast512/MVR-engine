import types;
import core.binding;
import core.counters;
import core.frame;
import core.math;
import geometry.hit;
import material.brdf;
import material.pbr;
import lighting.glass;
import raytracing.trace;

#include "lighting/rtxdi_bridge.slang"
#include "lighting/path_shading.slang"
#include "Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli"

// glass pass (m8 I13 §2, the RTXDI FullSample's GlassPass after its compositing): after combine, before DLSS-RR. Each
// pixel's camera ray walks the glass in front of its vis-buffer surface, front to back, and noisyColor becomes
// sum_i (prod_j<i T_j)(E_i + R_i + D_i) + (prod_i T_i) noisyColor. Glass is not rasterized, so the guides already
// describe the surface behind it (vk_gltf_renderer's primary surface replacement for clear glass)

static const uint kGlassTile = 8; // matches src/render/vk_renderer.cpp kFullscreenTile
// RTXDI's per-pass seeds stop at 31; NRC takes 41 and 42, the GI reference 43
static const uint kGlassLightSeed = 44;

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

// a layer's mirror ray, shaded as X2 of the indirect pass: emission and one sampled light, no indirect light and no
// sky until M7's environment light (C-I13-2). coneDistance: camera to the glass hit
float3 reflectedRadiance(GpuFrameRoot root, GpuRtxdiConstants constants, RayHit hit, GlassLayer layer,
                         float3 direction, float coneDistance, inout RTXDI_RandomSamplerState rng,
                         inout RTXDI_RandomSamplerState coherentRng)
{
    RayDesc mirror;
    mirror.Direction = reflect(direction, layer.normal);
    // normal mapping can tilt the mirror direction below the glass plane
    if (dot(mirror.Direction, layer.geoNormal) <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }
    mirror.Origin = safeSpawnPoint(hit, mirror.Direction);
    mirror.TMin = 0.0;
    mirror.TMax = kMissDistance;
    RayHit reflected;
    if (!traceClosest(root, mirror, reflected))
    {
        return float3(0.0, 0.0, 0.0);
    }
    const PathHit x2 = shadeRayHit(root, reflected, mirror, coneDistance);
    HitOrigin origin;
    origin.hit = reflected;
    return x2.emissive + secondaryDirectLight(root, x2.surface, origin, constants, rng, coherentRng);
}

// the layer's diffuse share lit by one sampled light, Lambert only
float3 diffuseShare(GpuFrameRoot root, GpuRtxdiConstants constants, RayHit hit, GlassLayer layer, float3 direction,
                    float hitDistance, inout RTXDI_RandomSamplerState rng, inout RTXDI_RandomSamplerState coherentRng)
{
    RAB_Surface surface;
    surface.position = layer.position;
    surface.viewDepth = hitDistance;
    surface.normal = layer.normal;
    surface.roughness = 1.0;
    surface.geoNormal = layer.geoNormal;
    surface.viewDir = -direction;
    surface.diffuseAlbedo = layer.diffuseWeight;
    surface.specularF0 = float3(0.0, 0.0, 0.0);
    surface.isLit = true;
    HitOrigin origin;
    origin.hit = hit;
    return lambertLightAt(root, surface, origin, constants, rng, coherentRng);
}

[shader("compute")]
[numthreads(kGlassTile, kGlassTile, 1)]
void glassMain(uint3 id : SV_DispatchThreadID)
{
    const uint2 pixel = id.xy;
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    rtxdiBridgeInit(root);
    if (any(pixel >= gRtxdiBridge.renderSize))
    {
        return;
    }
    const GpuTargets targets = loadTargets(root);
    const GpuCameraData camera = loadCamera(root);
    const GpuRtxdiConstants constants = loadRtxdiConstants(root);
    const uint flags = root.glassFlags;

    // the jittered ray the vis buffer rasterized, ending at its surface
    const PrimaryRay primary = primaryRay(camera, pixel, float2(gRtxdiBridge.renderSize));
    RayDesc ray = primary.ray;
    const RAB_Surface behind = RAB_GetGBufferSurface(int2(pixel), false);
    if (RAB_IsSurfaceValid(behind))
    {
        ray.TMax = min(ray.TMax, dot(behind.position - ray.Origin, ray.Direction));
    }
    const float originDistance = length(ray.Origin - camera.cameraPos);

    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(pixel, constants.runtime.frameIndex, kGlassLightSeed);
    RTXDI_RandomSamplerState coherentRng =
        RTXDI_InitRandomSampler(pixel / RTXDI_TILE_SIZE_IN_PIXELS, constants.runtime.frameIndex, kGlassLightSeed);
    const RayScene scene = loadRayScene(root);
    float3 throughput = float3(1.0, 1.0, 1.0);
    float3 overlay = float3(0.0, 0.0, 0.0);
    float3 reflection = float3(0.0, 0.0, 0.0);
    // I13 §4 variant b: the layers' albedo seen by DLSS-RR, each behind the throughput of the layers before it
    float3 glassDiffuseAlbedo = float3(0.0, 0.0, 0.0);
    float3 glassSpecAlbedo = float3(0.0, 0.0, 0.0);
    float travelled = 0.0;
    uint layers = 0;
    bool isEarlyStop = false;
    // back faces of single-sided glass pass without counting as a layer
    for (uint step = 0; step < 2 * root.glassMaxLayers && layers < root.glassMaxLayers; ++step)
    {
        RayHit hit;
        if (ray.TMax <= 0.0 || !traceClosest(scene, ray, kRayMaskGlass, hit))
        {
            break;
        }
        RayCone cone;
        cone.spread = primary.cone.spread;
        cone.width = primary.cone.width + primary.cone.spread * travelled;
        GlassLayer layer;
        if (evaluateGlassLayer(root, hit, ray.Direction, cone, layer))
        {
            ++layers;
            const float hitDistance = originDistance + travelled + hit.t;
            float3 terms = float3(0.0, 0.0, 0.0);
            if ((flags & kGlassFlagEmission) != 0)
            {
                // a light triangle's emission reaches the frame through light sampling
                terms += hitEmission(root, hit, layer.emission);
            }
            if ((flags & kGlassFlagReflection) != 0 && any(layer.reflectance > 0.0))
            {
                const float3 reflected = layer.reflectance * reflectedRadiance(root, constants, hit, layer,
                                                                               ray.Direction, hitDistance, rng,
                                                                               coherentRng);
                reflection += throughput * reflected;
                terms += reflected;
            }
            if ((flags & kGlassFlagDiffuseShare) != 0 && any(layer.diffuseWeight > 0.0))
            {
                terms += diffuseShare(root, constants, hit, layer, ray.Direction, hitDistance, rng, coherentRng);
            }
            overlay += throughput * terms;
            glassDiffuseAlbedo += throughput * layer.diffuseWeight;
            glassSpecAlbedo += throughput * layer.reflectance;
            throughput *= layer.transmittance;
            if (luminance(throughput) < kGlassMinThroughput)
            {
                isEarlyStop = true;
                break;
            }
        }
        travelled += hit.t;
        advancePastGlass(ray, hit);
    }

    GpuGlassCounters* counters = bdaPtr<GpuGlassCounters>(root.glassCounters);
    waveCount(counters.pixels, layers > 0);
    waveCount(counters.earlyStops, isEarlyStop);
    // the cap was reached with throughput left; glass may or may not follow
    waveCount(counters.capped, layers == root.glassMaxLayers && !isEarlyStop);
    for (uint n = 0; n < kGlassMaxLayersLimit; ++n)
    {
        waveCount(counters.layerPixels[n], layers == n + 1);
    }

    // every pixel: DLSS-RR reads these images in place of the albedo guides (RenderTargets::glassAlbedo)
    if ((flags & kGlassFlagBlendGuides) != 0)
    {
        const float4 diffuseBehind = tex2D(targets.current.sampled[kGuideDiffuseAlbedo]).Load(int3(pixel, 0));
        const float3 specBehind = tex2D(targets.current.sampled[kGuideSpecAlbedo]).Load(int3(pixel, 0)).rgb;
        // formats: RenderTargets kGuideFormats
        storageTex2D<float4, kImageFormatRgba8>(targets.glassDiffuseAlbedoStorage)[pixel] =
            float4(saturate(throughput * diffuseBehind.rgb + glassDiffuseAlbedo), diffuseBehind.a);
        storageTex2D<float4, kImageFormatRgba8>(targets.glassSpecAlbedoStorage)[pixel] =
            float4(saturate(throughput * specBehind + glassSpecAlbedo), 1.0);
    }

    // RenderTargets::kNoisyColorFormat
    const uint color = targets.noisyColorStorage;
    // the Glass debug view shows the pass's own terms in place of the color (DebugView::Glass); alpha: the layer
    // count, for the guide capture's throughput check
    if ((root.debugView & kDebugViewModeMask) == kDebugViewGlass)
    {
        const bool isReflectionShown = (root.debugView & kDebugViewFlagGlassReflection) != 0;
        storageTex2D<float4, kImageFormatRgba16f>(color)[pixel] =
            float4(isReflectionShown ? reflection : throughput, float(layers));
        return;
    }
    if (layers == 0)
    {
        return;
    }
    const float3 behindColor = storageTex2D<float4, kImageFormatRgba16f>(color)[pixel].rgb;
    storageTex2D<float4, kImageFormatRgba16f>(color)[pixel] = float4(overlay + throughput * behindColor, 1.0);
}
