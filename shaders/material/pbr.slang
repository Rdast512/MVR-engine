#ifndef ENGINE_MATERIAL_PBR_SLANG
#define ENGINE_MATERIAL_PBR_SLANG

#include "types.slang"
#include "core/binding.slang"
#include "core/frame.slang"
#include "geometry/surface.slang"

// glTF metallic-roughness material at a surface (spec/pbr_implementation_plan.md B1); no KHR_texture_transform yet

static const uint kMaterialFlagUnlit = 1u << 3;

struct SampledMaterial
{
    float3 baseColor; // linear, x COLOR_0
    float opacity;
    float metallic;
    float roughness; // perceptual
    float3 emissive;
    float ao;
    float3 n; // world, normal-mapped, facing-corrected
};

struct TextureGradients
{
    float2 uv;
    float2 ddx;
    float2 ddy;
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

SampledMaterial sampleMaterial(GpuMaterial material, Surface surface, float lodBias)
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

    // tangent-less meshes keep the vertex normal (spec B1 guard)
    if (material.normalTex != kNoneIndex && material.normalSamp != kNoneIndex && surface.tangent.w != 0.0)
    {
        float3 tangentNormal =
            sampleSlot(material.normalTex, material.normalSamp, material.normalUv, surface, lodBias, one).xyz * 2.0 -
            1.0;
        tangentNormal.xy *= material.normalScale;
        const float3 t = normalize(surface.tangent.xyz - surface.normal * dot(surface.normal, surface.tangent.xyz));
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

#endif
