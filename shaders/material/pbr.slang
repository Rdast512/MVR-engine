module pbr;

import types;
import core.binding;
import geometry.surface;

// glTF metallic-roughness material at a surface (spec/pbr_implementation_plan.md B1); no KHR_texture_transform yet

public struct SampledMaterial
{
    public float3 baseColor; // linear, x COLOR_0
    public float opacity;
    public float metallic;
    public float roughness; // perceptual
    public float3 emissive;
    public float ao;
    public float3 n; // world, normal-mapped, facing-corrected
};

public struct TextureGradients
{
    public float2 uv;
    public float2 ddx;
    public float2 ddy;
};

// lodBias: log2(render / display) once upscaling lands; applied by scaling the gradients
TextureGradients textureGradients(Surface surface, uint uvSet, float lodBias)
{
    const uint set = min(uvSet, 1u);
    const float scale = exp2(lodBias);
    TextureGradients result;
    result.uv = surface.uv[set];
    result.ddx = surface.uvDdx[set] * scale;
    result.ddy = surface.uvDdy[set] * scale;
    return result;
}

// absent texture: the factor alone (kNoneIndex never becomes a handle)
float4 sampleSlot(uint texture, uint samplerIndex, uint uvSet, Surface surface, float lodBias, float4 fallback)
{
    if (texture == kNoneIndex || samplerIndex == kNoneIndex)
    {
        return fallback;
    }
    const TextureGradients gradients = textureGradients(surface, uvSet, lodBias);
    return tex2D(NonUniformResourceIndex(texture))
        .SampleGrad(samplerOf(NonUniformResourceIndex(samplerIndex)), gradients.uv, gradients.ddx, gradients.ddy);
}

static const float kMinOrthoTangentLengthSq = 1e-8;

public SampledMaterial sampleMaterial(GpuMaterial material, Surface surface, float lodBias)
{
    const float4 one = float4(1.0, 1.0, 1.0, 1.0);
    // sRGB views decode base color and emissive in hardware
    const float4 baseColor = material.baseColorFactor * float4(surface.color, 1.0) *
        sampleSlot(material.baseColorTex, material.baseColorSamp, material.baseColorUv, surface, lodBias, one);
    // B metal, G rough
    const float4 metalRough =
        sampleSlot(material.metalRoughTex, material.metalRoughSamp, material.metalRoughUv, surface, lodBias, one);
    const float occlusion =
        sampleSlot(material.occlusionTex, material.occlusionSamp, material.occlusionUv, surface, lodBias, one).r;
    const float3 emissive = material.emissiveFactor *
        sampleSlot(material.emissiveTex, material.emissiveSamp, material.emissiveUv, surface, lodBias, one).rgb;

    SampledMaterial result;
    result.baseColor = baseColor.rgb;
    result.opacity = baseColor.a;
    result.metallic = saturate(material.metallicFactor * metalRough.b);
    result.roughness = saturate(material.roughnessFactor * metalRough.g);
    result.ao = lerp(1.0, occlusion, material.occlusionStrength);
    result.emissive = emissive;
    result.n = surface.normal;

    // assets can ship tangents parallel to the normal; normalizing the zero residual gives NaN
    const float3 orthoTangent = surface.tangent.xyz - surface.normal * dot(surface.normal, surface.tangent.xyz);
    const float orthoLengthSq = dot(orthoTangent, orthoTangent);

    // tangent-less meshes keep the vertex normal (spec B1 guard)
    if (material.normalTex != kNoneIndex && material.normalSamp != kNoneIndex && surface.tangent.w != 0.0 &&
        orthoLengthSq > kMinOrthoTangentLengthSq)
    {
        float3 tangentNormal =
            sampleSlot(material.normalTex, material.normalSamp, material.normalUv, surface, lodBias, one).xyz * 2.0 -
            1.0;
        tangentNormal.xy *= material.normalScale;
        const float3 t = orthoTangent / sqrt(orthoLengthSq);
        // back faces flipped the normal; the bitangent follows it
        const float3 b = cross(surface.normal, t) * surface.tangent.w;
        result.n = normalize(tangentNormal.x * t + tangentNormal.y * b + tangentNormal.z * surface.normal);
    }

    // unlit: no diffuse or specular response, the color is all emission
    if ((material.flags & kMaterialFlagUnlit) != 0)
    {
        result.emissive = result.baseColor;
        result.baseColor = float3(0.0, 0.0, 0.0);
        result.metallic = 0.0;
    }
    return result;
}
