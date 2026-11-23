module pbr;

import types;
import core.binding;
import geometry.surface;

// glTF metallic-roughness material at a surface (spec/pbr_implementation_plan.md B1), or a
// KHR_materials_pbrSpecularGlossiness one in the same slots

// dielectric reflectance at normal incidence
public static const float kDielectricF0 = 0.04;

// index GpuMaterial.uvMatrices; same order as the kMaterialFlagUvTransform* bits
static const uint kUvSlotBaseColor = 0;
static const uint kUvSlotMetalRough = 1;
static const uint kUvSlotNormal = 2;
static const uint kUvSlotOcclusion = 3;
static const uint kUvSlotEmissive = 4;

public struct SampledMaterial
{
    // glTF appendix B: spec-gloss sets both directly, which metallic and base color cannot express
    public float3 cDiff; // linear, x COLOR_0
    public float3 f0;
    public float opacity;
    public float roughness; // perceptual
    public float3 emissive;
    public float ao;
    public float3 n; // world, normal-mapped, facing-corrected
    // scales the whole specular lobe, fresnel included: 0 for unlit, which reflects nothing
    public float specularWeight;
};

public struct TextureGradients
{
    public float2 uv;
    public float2 ddx;
    public float2 ddy;
};

// matches kBlendAlphaCutoff in src/core/material_store.hpp
static const float kBlendAlphaCutoff = 0.5;

// matches alphaTestCutoff in src/core/material_store.hpp: factor.a x texture.a below it is cut
public float alphaTestCutoff(GpuMaterial material)
{
    return (material.flags & kMaterialAlphaModeMask) == uint(AlphaMode.Mask) ? material.alphaCutoff : kBlendAlphaCutoff;
}

// uv' = M * (uv, 1) where the slot's KHR_texture_transform bit is set
float2 transformUv(GpuMaterial material, uint slot, float2 uv)
{
    if ((material.flags & (kMaterialFlagUvTransformBaseColor << slot)) == 0)
    {
        return uv;
    }
    const GpuUvMatrix transform = material.uvMatrices[slot];
    const float3 homogeneousUv = float3(uv, 1.0);
    return float2(dot(transform.row0, homogeneousUv), dot(transform.row1, homogeneousUv));
}

// base-color uv set 0 or 1 at a vertex, transformed: the transform is affine, so it commutes with interpolation
public float2 baseColorUv(GpuMaterial material, float2 uv0, float2 uv1)
{
    return transformUv(material, kUvSlotBaseColor, material.baseColorUv == 0 ? uv0 : uv1);
}

// lodBias: log2(render / display) under DLSS (GpuCameraData.lodBias); applied by scaling the gradients
TextureGradients textureGradients(GpuMaterial material, uint slot, Surface surface, uint uvSet, float lodBias)
{
    const uint set = min(uvSet, 1u);
    const float scale = exp2(lodBias);
    TextureGradients result;
    result.uv = transformUv(material, slot, surface.uv[set]);
    result.ddx = surface.uvDdx[set] * scale;
    result.ddy = surface.uvDdy[set] * scale;
    if ((material.flags & (kMaterialFlagUvTransformBaseColor << slot)) != 0)
    {
        const GpuUvMatrix transform = material.uvMatrices[slot];
        // gradients are directions: no translation
        result.ddx = float2(dot(transform.row0.xy, result.ddx), dot(transform.row1.xy, result.ddx));
        result.ddy = float2(dot(transform.row0.xy, result.ddy), dot(transform.row1.xy, result.ddy));
    }
    return result;
}

// matches isAlphaTested in src/core/material_store.hpp: the alpha-tested raster bin and the OMM bake take these
public bool isAlphaTested(GpuMaterial material)
{
    if ((material.flags & kMaterialAlphaModeMask) == uint(AlphaMode.Opaque) || material.baseColorTex == kNoneIndex ||
        material.baseColorSamp == kNoneIndex)
    {
        return false;
    }
    const float cutoff = alphaTestCutoff(material);
    return cutoff > 0.0 && material.baseColorFactor.a >= cutoff;
}

// matches materialBin in src/core/material_store.hpp: the resolve bin of a surface pixel, from GpuMaterial.flags so
// classify loads one field of the row
public uint materialBin(uint materialFlags)
{
    return (materialFlags & kMaterialFlagUnlit) != 0 ? kMaterialBinUnlit : kMaterialBinStandard;
}

// the base-color texture's uv and lodBias-scaled gradients at a surface, as sampleMaterial reads them
public TextureGradients baseColorGradients(GpuMaterial material, Surface surface, float lodBias)
{
    return textureGradients(material, kUvSlotBaseColor, surface, material.baseColorUv, lodBias);
}

// absent texture: the factor alone (kNoneIndex never becomes a handle)
float4 sampleSlot(GpuMaterial material, uint slot, uint texture, uint samplerIndex, uint uvSet, Surface surface,
                  float lodBias, float4 fallback)
{
    if (texture == kNoneIndex || samplerIndex == kNoneIndex)
    {
        return fallback;
    }
    const TextureGradients gradients = textureGradients(material, slot, surface, uvSet, lodBias);
    return tex2D(NonUniformResourceIndex(texture))
        .SampleGrad(samplerOf(NonUniformResourceIndex(samplerIndex)), gradients.uv, gradients.ddx, gradients.ddy);
}

static const float kMinOrthoTangentLengthSq = 1e-8;
static const float kMinUvTransformDeterminant = 1e-8;

// normal map axes follow dP/du' and -dP/dv' (+y up, v grows down), from the inverse of the uv matrix's 2x2 part
void transformTangentFrame(GpuUvMatrix transform, inout float3 t, inout float3 b)
{
    const float determinant = transform.row0.x * transform.row1.y - transform.row0.y * transform.row1.x;
    // zero scale has no inverse
    if (abs(determinant) <= kMinUvTransformDeterminant)
    {
        return;
    }
    const float orientation = determinant < 0.0 ? -1.0 : 1.0;
    const float3 tangent = normalize((transform.row1.y * t + transform.row1.x * b) * orientation);
    const float3 bitangent = normalize((transform.row0.y * t + transform.row0.x * b) * orientation);
    t = tangent;
    b = bitangent;
}

// emissiveFactor x the emissive texture; also a triangle light's radiance at its uv centroid (passes/light_world.slang)
public float3 sampleEmissive(GpuMaterial material, Surface surface, float lodBias)
{
    const float4 one = float4(1.0, 1.0, 1.0, 1.0);
    return material.emissiveFactor * sampleSlot(material, kUvSlotEmissive, material.emissiveTex, material.emissiveSamp,
                                                material.emissiveUv, surface, lodBias, one).rgb;
}

// factor x COLOR_0 x texture, alpha included; sRGB views decode the texture in hardware
public float4 sampleBaseColor(GpuMaterial material, Surface surface, float lodBias)
{
    const float4 one = float4(1.0, 1.0, 1.0, 1.0);
    return material.baseColorFactor * float4(surface.color, 1.0) *
        sampleSlot(material, kUvSlotBaseColor, material.baseColorTex, material.baseColorSamp, material.baseColorUv,
                   surface, lodBias, one);
}

// a KHR_materials_* texture (GpuMaterialExtension) with its own KHR_texture_transform, M = T * R * S as
// packUvMatrix in src/core/material_store.hpp; absent: the fallback
public float4 sampleExtensionTexture(GpuTextureRef ref, Surface surface, float lodBias, float4 fallback)
{
    if (ref.tex == kNoneIndex || ref.samp == kNoneIndex)
    {
        return fallback;
    }
    const uint set = min(ref.uv, 1u);
    const float c = cos(ref.uvXform.rotation);
    const float s = sin(ref.uvXform.rotation);
    const float2x2 linear = float2x2(c * ref.uvXform.scale.x, s * ref.uvXform.scale.y,
                                     -s * ref.uvXform.scale.x, c * ref.uvXform.scale.y);
    const float scale = exp2(lodBias);
    const float2 uv = mul(linear, surface.uv[set]) + ref.uvXform.offset;
    const float2 ddx = mul(linear, surface.uvDdx[set]) * scale;
    const float2 ddy = mul(linear, surface.uvDdy[set]) * scale;
    return tex2D(NonUniformResourceIndex(ref.tex))
        .SampleGrad(samplerOf(NonUniformResourceIndex(ref.samp)), uv, ddx, ddy);
}

// rays hit every material: the unlit body is picked per row
public SampledMaterial sampleMaterial(GpuMaterial material, Surface surface, float lodBias)
{
    return sampleMaterial(material, surface, lodBias, materialBin(material.flags) == kMaterialBinUnlit);
}

// isUnlit: a constant in each resolve bin's entry, so the bin compiles only its own body
public SampledMaterial sampleMaterial(GpuMaterial material, Surface surface, float lodBias, bool isUnlit)
{
    const float4 one = float4(1.0, 1.0, 1.0, 1.0);
    const float4 baseColor = sampleBaseColor(material, surface, lodBias);
    // metal-rough: B metal, G rough; spec-gloss: RGB specular (sRGB view), A glossiness
    const float4 parameters = sampleSlot(material, kUvSlotMetalRough, material.metalRoughTex, material.metalRoughSamp,
                                         material.metalRoughUv, surface, lodBias, one);
    const float occlusion = sampleSlot(material, kUvSlotOcclusion, material.occlusionTex, material.occlusionSamp,
                                       material.occlusionUv, surface, lodBias, one).r;
    const float3 emissive = sampleEmissive(material, surface, lodBias);

    SampledMaterial result;
    result.opacity = baseColor.a;
    if ((material.flags & kMaterialFlagSpecularGlossiness) != 0)
    {
        // the extension's model: F0 = specular, cDiff = diffuse (1 - max(specular)), roughness = 1 - glossiness
        result.f0 = saturate(material.specularFactor * parameters.rgb);
        result.cDiff = baseColor.rgb * (1.0 - max(result.f0.r, max(result.f0.g, result.f0.b)));
        result.roughness = saturate(1.0 - material.glossinessFactor * parameters.a);
    }
    else
    {
        const float metallic = saturate(material.metallicFactor * parameters.b);
        result.cDiff = baseColor.rgb * (1.0 - metallic);
        result.f0 = lerp(float3(kDielectricF0, kDielectricF0, kDielectricF0), baseColor.rgb, metallic);
        result.roughness = saturate(material.roughnessFactor * parameters.g);
    }
    result.ao = lerp(1.0, occlusion, material.occlusionStrength);
    result.emissive = emissive;
    result.n = surface.normal;
    result.specularWeight = 1.0;

    // assets can ship tangents parallel to the normal; normalizing the zero residual gives NaN
    const float3 orthoTangent = surface.tangent.xyz - surface.normal * dot(surface.normal, surface.tangent.xyz);
    const float orthoLengthSq = dot(orthoTangent, orthoTangent);

    // tangent-less meshes keep the vertex normal (spec B1 guard)
    if (material.normalTex != kNoneIndex && material.normalSamp != kNoneIndex && surface.tangent.w != 0.0 &&
        orthoLengthSq > kMinOrthoTangentLengthSq)
    {
        const float2 encodedNormal = sampleSlot(material, kUvSlotNormal, material.normalTex, material.normalSamp,
                                                material.normalUv, surface, lodBias, one).xy;
        // z from xy: two-channel (BC5) maps have no blue channel
        float3 tangentNormal;
        tangentNormal.xy = encodedNormal * 2.0 - 1.0;
        tangentNormal.z = sqrt(saturate(1.0 - dot(tangentNormal.xy, tangentNormal.xy)));
        tangentNormal.xy *= material.normalScale;
        float3 t = orthoTangent / sqrt(orthoLengthSq);
        // back faces flipped the normal; the bitangent follows it
        float3 b = cross(surface.normal, t) * surface.tangent.w;
        // negate t too, so the back-face normal is the exact negation of the front-face one (Khronos sample viewer)
        if (!surface.isFrontFacing)
        {
            t = -t;
        }
        if ((material.flags & kMaterialFlagUvTransformNormal) != 0)
        {
            transformTangentFrame(material.uvMatrices[kUvSlotNormal], t, b);
        }
        result.n = normalize(tangentNormal.x * t + tangentNormal.y * b + tangentNormal.z * surface.normal);
    }
    // m8 I2 F19 (ND9): keep the shading normal in the geometric normal's hemisphere (RTXGI Pathtracer.hlsl:421, 469)
    if (dot(surface.geoNormal, result.n) < 0.0)
    {
        result.n = -result.n;
    }

    // unlit: no diffuse or specular response, the color is all emission
    if (isUnlit)
    {
        result.emissive = baseColor.rgb;
        result.cDiff = float3(0.0, 0.0, 0.0);
        result.f0 = float3(kDielectricF0, kDielectricF0, kDielectricF0);
        result.specularWeight = 0.0;
    }
    return result;
}
