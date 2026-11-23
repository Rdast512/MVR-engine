module glass;

import types;
import core.binding;
import core.frame;
import core.math;
import geometry.hit;
import geometry.surface;
import material.pbr;
import raytracing.trace;

// thin-walled glass, stage 1 of transmission (m8 I13, spec/fixesM1-6/i13_glass_transmission.md §2-3): the terms of one
// crossing and the closest-hit walk over kRayMaskGlass that the glass pass and the shadow transmittance share. No
// any-hit: glass instances keep FORCE_OPAQUE, each crossing is one closest hit and the next search starts past it.
// Port of the RTXDI FullSample GlassPass.hlsl layer loop, with base color applied once and glTF's F0 and diffuse share

// I13 §2: a walk stops once its throughput luminance drops below this (GlassPass.hlsl)
public static const float kGlassMinThroughput = 0.01;
// crossings a walk tests per counted layer: back faces of single-sided glass pass untouched
static const uint kGlassHitsPerLayer = 2;

// one crossing, coverage (alphaMode BLEND) applied
public struct GlassLayer
{
    public float3 transmittance; // (1 - m) t (1 - F) baseColor, blended with pass-through by the coverage
    public float3 reflectance;   // lerp(F, F_m, m): the mirror ray's weight
    public float3 diffuseWeight; // (1 - m)(1 - t)(1 - F) baseColor: the Lambert weight of the direct light
    public float3 emission;
    public float3 normal;        // shading normal, toward the incoming ray
    public float3 geoNormal;     // toward the incoming ray
    public float3 position;
};

// KHR_materials_ior and KHR_materials_specular, factors only: ((ior - 1) / (ior + 1))^2 specular specularColor
float3 dielectricF0(GpuMaterialExtension ext)
{
    const float r = (ext.ior - 1.0) / (ext.ior + 1.0);
    return min(r * r * ext.specularFactor * ext.specularColorFactor, float3(1.0, 1.0, 1.0));
}

// Schlick from f0 at cosine mu
float3 schlick(float3 f0, float mu)
{
    const float grazing = pow(1.0 - saturate(mu), 5.0);
    return f0 + (1.0 - f0) * grazing;
}

// false: a back face of single-sided glass, which a walk passes untouched. cone: the footprint at the ray origin
public bool evaluateGlassLayer(GpuFrameRoot root, RayHit hit, float3 direction, RayCone cone, out GlassLayer layer)
{
    layer = {};
    const DecodedHit decoded = decodeHit(root, hit, direction, cone);
    if (!decoded.surface.isFrontFacing && !decoded.draw.isDoubleSided)
    {
        return false;
    }
    const uint row = materialRow(decoded.draw.primitive);
    const GpuMaterial material = loadMaterial(root, row);
    const GpuMaterialExtension extension = loadMaterialExtension(root, row);
    // no DLSS LOD bias: glass layers are not reconstructed at display resolution beyond their color
    const SampledMaterial sampled = sampleMaterial(material, decoded.surface, 0.0);
    const float4 baseColor = sampleBaseColor(material, decoded.surface, 0.0);
    const float4 one = float4(1.0, 1.0, 1.0, 1.0);
    const float transmission =
        extension.transmissionFactor * sampleExtensionTexture(extension.transmission, decoded.surface, 0.0, one).r;
    // classifyTransmission keeps every glass row free of a metal-rough texture
    const float metallic = saturate(material.metallicFactor);

    // sampleMaterial keeps n in the geometric hemisphere, and buildRaySurface turns that toward the ray
    layer.normal = sampled.n;
    layer.geoNormal = decoded.surface.geoNormal;
    layer.position = decoded.surface.position;
    const float mu = abs(dot(layer.normal, direction));
    const float3 dielectric = schlick(dielectricF0(extension), mu);
    const float3 conductor = schlick(baseColor.rgb, mu);
    const float3 dielectricShare = (1.0 - metallic) * (1.0 - dielectric) * baseColor.rgb;
    layer.reflectance = lerp(dielectric, conductor, metallic);
    layer.transmittance = transmission * dielectricShare;
    layer.diffuseWeight = (1.0 - transmission) * dielectricShare;
    layer.emission = sampled.emissive;

    // MASK is cut by the micromap; BLEND covers alpha of the pixel and passes the rest
    if ((material.flags & kMaterialAlphaModeMask) == uint(AlphaMode.Blend))
    {
        const float alpha = saturate(baseColor.a);
        layer.reflectance *= alpha;
        layer.diffuseWeight *= alpha;
        layer.emission *= alpha;
        layer.transmittance = alpha * layer.transmittance + (1.0 - alpha);
    }
    return true;
}

// the next segment of a walk, past the crossing on the ray's far side
public void advancePastGlass(inout RayDesc ray, RayHit hit)
{
    ray.Origin = safeSpawnPoint(hit, ray.Direction);
    ray.TMin = 0.0;
    ray.TMax = max(ray.TMax - hit.t, 0.0);
}

// I13 §3: the product of the transmittances of the glass on [origin, origin + direction tMax], each at its own
// angle, up to glassMaxLayers crossings. Reflection, the diffuse share and emission do not reach a shadow ray
public float3 glassTransmittance(GpuFrameRoot root, float3 origin, float3 direction, float tMax)
{
    float3 throughput = float3(1.0, 1.0, 1.0);
    const RayScene scene = loadRayScene(root);
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = tMax;
    // texture detail of a shadow crossing does not matter: the finest mip
    RayCone cone;
    cone.width = 0.0;
    cone.spread = 0.0;
    uint layers = 0;
    for (uint step = 0; step < kGlassHitsPerLayer * root.glassMaxLayers && layers < root.glassMaxLayers; ++step)
    {
        RayHit hit;
        if (ray.TMax <= 0.0 || !traceClosest(scene, ray, kRayMaskGlass, hit))
        {
            break;
        }
        GlassLayer layer;
        if (evaluateGlassLayer(root, hit, direction, cone, layer))
        {
            ++layers;
            throughput *= layer.transmittance;
            if (luminance(throughput) < kGlassMinThroughput)
            {
                break;
            }
        }
        advancePastGlass(ray, hit);
    }
    return throughput;
}

// the shadow transmittance switch: kGlassFlagShadowTransmittance is uniform per frame, so scenes without glass skip
// the walk entirely
public float3 shadowTransmittance(GpuFrameRoot root, float3 origin, float3 direction, float tMax)
{
    if ((root.glassFlags & kGlassFlagShadowTransmittance) == 0)
    {
        return float3(1.0, 1.0, 1.0);
    }
    return glassTransmittance(root, origin, direction, tMax);
}
