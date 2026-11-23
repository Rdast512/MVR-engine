module brdf;

import core.math;
import material.pbr;

// glTF appendix B metallic-roughness brdf with vndf sampling; pixels, hits and sdk bridges all call these

// at a2 = 1e-8 float rounds a2 - 1 to -1 and D(nDotH = 1) is inf
static const float kMinAlpha = 1e-3;
static const float kMinNDotV = 1e-4;
static const float kMinLobeProbability = 0.1;

public struct BrdfInputs
{
    public float3 cDiff;
    public float3 f0;
};

public BrdfInputs fromMaterial(SampledMaterial material)
{
    BrdfInputs inputs;
    inputs.cDiff = material.cDiff;
    inputs.f0 = material.f0;
    return inputs;
}

public float3 fresnelSchlick(float3 f0, float vDotH)
{
    // a dot of unit vectors can round above 1; pow of a negative base is undefined
    return f0 + (1.0 - f0) * pow(saturate(1.0 - vDotH), 5.0);
}

// cross(n, h) keeps 1 - (N.H)^2 accurate near N = H for low roughness (Filament / Lagarde)
public float ggxD(float3 n, float3 h, float nDotH, float a2)
{
    const float3 nCrossH = cross(n, h);
    const float d = nDotH * nDotH * a2 + dot(nCrossH, nCrossH);
    return a2 / (kPi * d * d);
}

// height-correlated smith; already contains 1 / (4 NdotL NdotV)
public float smithVisibility(float nDotL, float nDotV, float a2)
{
    const float ggxV = nDotL * sqrt(nDotV * nDotV * (1.0 - a2) + a2);
    const float ggxL = nDotV * sqrt(nDotL * nDotL * (1.0 - a2) + a2);
    return 0.5 / max(ggxV + ggxL, 1e-7);
}

float ggxAlpha(float roughness)
{
    return max(roughness * roughness, kMinAlpha);
}

// normal mapping can face n away from the viewer; vndf and half-vector evaluation need v above the surface
float3 viewAboveSurface(float3 n, float3 v)
{
    const float nDotV = dot(n, v);
    return nDotV >= kMinNDotV ? v : normalize(v + n * (kMinNDotV - nDotV));
}

// caller multiplies by radiance * NdotL once
public float3 evalBrdf(float3 n, float3 cDiff, float3 f0, float roughness, float3 viewDir, float3 l)
{
    const float nDotL = dot(n, l);
    if (nDotL <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float3 v = viewAboveSurface(n, viewDir);
    const float3 h = normalize(l + v);
    const float nDotV = max(dot(n, v), kMinNDotV);
    const float nDotH = saturate(dot(n, h));
    const float vDotH = saturate(dot(v, h));
    const float alpha = ggxAlpha(roughness);
    const float a2 = alpha * alpha;

    const float3 f = fresnelSchlick(f0, vDotH);
    return (1.0 - f) * cDiff / kPi + f * ggxD(n, h, nDotH, a2) * smithVisibility(nDotL, nDotV, a2);
}

// independent of l, so sampling and pdf agree
float specularProbability(float3 cDiff, float3 f0, float nDotV)
{
    if (luminance(cDiff) <= 0.0)
    {
        return 1.0;
    }
    const float3 f = fresnelSchlick(f0, nDotV);
    const float specular = luminance(f);
    const float total = specular + luminance((1.0 - f) * cDiff);
    // fresnel at nDotV starves diffuse at grazing views: rare huge weights
    return clamp(specular / total, kMinLobeProbability, 1.0 - kMinLobeProbability);
}

// G1(v) * D / (4 NdotV) with isotropic G1 expanded
float vndfPdf(float3 n, float3 h, float nDotV, float nDotH, float a2)
{
    return ggxD(n, h, nDotH, a2) / (2.0 * (nDotV + sqrt(a2 + (1.0 - a2) * nDotV * nDotV)));
}

// dupuy and benyoub 2023 spherical cap; v is local with z along n
float3 sampleVndf(float3 v, float alpha, float2 u)
{
    const float3 stretchedView = normalize(float3(alpha * v.xy, v.z));
    const float phi = 2.0 * kPi * u.x;
    const float z = (1.0 - u.y) * (1.0 + stretchedView.z) - stretchedView.z;
    const float sinTheta = sqrt(saturate(1.0 - z * z));
    const float3 halfway = float3(sinTheta * cos(phi), sinTheta * sin(phi), z) + stretchedView;
    return normalize(float3(alpha * halfway.xy, halfway.z));
}

float3 sampleCosineHemisphere(float2 u)
{
    const float phi = 2.0 * kPi * u.x;
    const float radius = sqrt(u.y);
    return float3(radius * cos(phi), radius * sin(phi), sqrt(saturate(1.0 - u.y)));
}

// solid-angle pdf of sampleBrdf; 0 below the surface
public float pdfBrdf(float3 n, float3 cDiff, float3 f0, float roughness, float3 viewDir, float3 l)
{
    const float nDotL = dot(n, l);
    if (nDotL <= 0.0)
    {
        return 0.0;
    }
    const float3 v = viewAboveSurface(n, viewDir);
    const float3 h = normalize(l + v);
    const float nDotV = dot(n, v);
    const float alpha = ggxAlpha(roughness);
    const float specularPdf = vndfPdf(n, h, nDotV, saturate(dot(n, h)), alpha * alpha);
    return lerp(nDotL / kPi, specularPdf, specularProbability(cDiff, f0, nDotV));
}

public struct BrdfSample
{
    public float3 l;      // world, unit
    public float3 weight; // f * NdotL / pdf
    public float pdf;     // solid angle; 0 means no valid direction
};

// u.x picks the lobe, u.yz the direction
public BrdfSample sampleBrdf(float3 n, float3 cDiff, float3 f0, float roughness, float3 viewDir, float3 u)
{
    const float3 v = viewAboveSurface(n, viewDir);
    float3 t;
    float3 b;
    buildOrthonormalBasis(n, t, b);

    float3 l;
    if (u.x < specularProbability(cDiff, f0, dot(n, v)))
    {
        const float3 viewLocal = float3(dot(t, v), dot(b, v), dot(n, v));
        const float3 h = sampleVndf(viewLocal, ggxAlpha(roughness), u.yz);
        l = reflect(-v, t * h.x + b * h.y + n * h.z);
    }
    else
    {
        const float3 local = sampleCosineHemisphere(u.yz);
        l = t * local.x + b * local.y + n * local.z;
    }

    BrdfSample result;
    result.l = l;
    result.weight = float3(0.0, 0.0, 0.0);
    result.pdf = pdfBrdf(n, cDiff, f0, roughness, v, l);
    // vndf reflections can go below the horizon; written negated so nan is rejected too
    if (!(result.pdf > 0.0))
    {
        result.pdf = 0.0;
        return result;
    }
    result.weight = evalBrdf(n, cDiff, f0, roughness, v, l) * dot(n, l) / result.pdf;
    return result;
}

// NVIDIA EnvBRDFApprox2 (Ray Tracing Gems ch. 32, reference/vk_gltf_renderer/shaders/dlss_util.h): scales additive
// bias by saturate(max(f0) * 50) so f0 == 0 materials (e.g. Bistro spec-gloss) do not leak grazing SpecAlbedo (m8 I14)
public float3 envBrdfApprox(float3 f0, float nDotV, float roughness)
{
    const float alpha = roughness * roughness;
    const float noV = abs(nDotV);
    const float4 x = float4(1.0, noV, noV * noV, noV * noV * noV);
    const float4 y = float4(1.0, alpha, alpha * alpha, alpha * alpha * alpha);
    const float2x2 m1 = float2x2(0.99044, -1.28514, 1.29678, -0.755907);
    const float3x3 m2 = float3x3(1.0, 2.92338, 59.4188, 20.3225, -27.0302, 222.592, 121.563, 626.13, 316.627);
    const float2x2 m3 = float2x2(0.0365463, 3.32707, 9.0632, -9.04756);
    const float3x3 m4 = float3x3(1.0, 3.59685, -1.36772, 9.04401, -16.3174, 9.22949, 5.56589, 19.7886, -20.2123);
    float bias = dot(mul(m1, x.xy), y.xy) * rcp(dot(mul(m2, x.xyw), y.xyw));
    const float scale = dot(mul(m3, x.xy), y.xy) * rcp(dot(mul(m4, x.xzw), y.xyw));
    bias *= saturate(max(f0.r, max(f0.g, f0.b)) * 50.0);
    return mad(f0, max(0.0, scale), max(0.0, bias));
}
