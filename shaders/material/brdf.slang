module brdf;

// rt/pt brdf. raster preview uses material/pbr.slang sampleAlbedo.

// dielectric reflectance at normal incidence
public static const float kDielectricF0 = 0.04;

// split-sum environment BRDF, analytic fit (Karis, "Physically Based Shading on Mobile"): f0 * scale + bias
public float3 envBrdfApprox(float3 f0, float nDotV, float roughness)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * nDotV)) * r.x + r.y;
    const float2 scaleBias = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * scaleBias.x + scaleBias.y;
}
