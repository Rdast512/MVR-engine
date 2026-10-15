#ifndef ENGINE_MATERIAL_PBR_SLANG
#define ENGINE_MATERIAL_PBR_SLANG

#include "types.slang"
#include "core/binding.slang"
#include "core/frame.slang"

// glTF base color: factor * texture * vertex color
float4 sampleBaseColor(GpuMaterial material, float2 uv, float3 vertexColor)
{
    float4 color = material.baseColorFactor * float4(vertexColor, 1.0);
    if (material.baseColorTex != kNoneIndex && material.baseColorSamp != kNoneIndex)
    {
        color *= tex2D(material.baseColorTex).Sample(samplerOf(material.baseColorSamp), uv);
    }
    return color;
}

#endif
