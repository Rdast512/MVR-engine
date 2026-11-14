module pbr;

import types;
import core.binding;
import geometry.surface;

// glTF metallic-roughness material at a surface (spec/pbr_implementation_plan.md B1)

// index GpuMaterial.uvMatrices; same order as the kMaterialFlagUvTransform* bits
static const uint kUvSlotBaseColor = 0;
static const uint kUvSlotMetalRough = 1;
static const uint kUvSlotNormal = 2;
static const uint kUvSlotOcclusion = 3;
static const uint kUvSlotEmissive = 4;

public struct SampledMaterial
{
    public float3 baseColor; // linear, x COLOR_0
    public float opacity;
    public float metallic;
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

// lodBias: log2(render / display) once upscaling lands; applied by scaling the gradients
TextureGradients textureGradients(GpuMaterial material, uint slot, Surface surface, uint uvSet, float lodBias)
{
    const uint set = min(uvSet, 1u);
    const float scale = exp2(lodBias);
    TextureGradients result;
    result.uv = surface.uv[set];
    result.ddx = surface.uvDdx[set] * scale;
    result.ddy = surface.uvDdy[set] * scale;
    if ((material.flags & (kMaterialFlagUvTransformBaseColor << slot)) != 0)
    {
        const GpuUvMatrix transform = material.uvMatrices[slot];
        const float3 homogeneousUv = float3(result.uv, 1.0);
        result.uv = float2(dot(transform.row0, homogeneousUv), dot(transform.row1, homogeneousUv));
        // gradients are directions: no translation
        result.ddx = float2(dot(transform.row0.xy, result.ddx), dot(transform.row1.xy, result.ddx));
        result.ddy = float2(dot(transform.row0.xy, result.ddy), dot(transform.row1.xy, result.ddy));
    }
    return result;
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

public SampledMaterial sampleMaterial(GpuMaterial material, Surface surface, float lodBias)
{
    const float4 one = float4(1.0, 1.0, 1.0, 1.0);
    // sRGB views decode base color and emissive in hardware
    const float4 baseColor = material.baseColorFactor * float4(surface.color, 1.0) *
        sampleSlot(material, kUvSlotBaseColor, material.baseColorTex, material.baseColorSamp, material.baseColorUv,
                   surface, lodBias, one);
    // B metal, G rough
    const float4 metalRough = sampleSlot(material, kUvSlotMetalRough, material.metalRoughTex, material.metalRoughSamp,
                                         material.metalRoughUv, surface, lodBias, one);
    const float occlusion = sampleSlot(material, kUvSlotOcclusion, material.occlusionTex, material.occlusionSamp,
                                       material.occlusionUv, surface, lodBias, one).r;
    const float3 emissive = material.emissiveFactor *
        sampleSlot(material, kUvSlotEmissive, material.emissiveTex, material.emissiveSamp, material.emissiveUv, surface,
                   lodBias, one).rgb;

    SampledMaterial result;
    result.baseColor = baseColor.rgb;
    result.opacity = baseColor.a;
    result.metallic = saturate(material.metallicFactor * metalRough.b);
    result.roughness = saturate(material.roughnessFactor * metalRough.g);
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

    // unlit: no diffuse or specular response, the color is all emission
    if ((material.flags & kMaterialFlagUnlit) != 0)
    {
        result.emissive = result.baseColor;
        result.baseColor = float3(0.0, 0.0, 0.0);
        result.metallic = 0.0;
        result.specularWeight = 0.0;
    }
    return result;
}
