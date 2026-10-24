#ifndef ENGINE_GEOMETRY_MESH_SLANG
#define ENGINE_GEOMETRY_MESH_SLANG

#include "types.slang"
#include "core/binding.slang"
#include "core/frame.slang"

// meshlet data is model-relative: every table is indexed from the model's slice
GpuMeshletDesc loadMeshlet(DrawContext draw, uint meshletIndex)
{
    return bdaPtr<GpuMeshletDesc>(draw.model.meshlets)[draw.primitive.firstMeshlet + meshletIndex];
}

// model-relative vertex id of a meshlet corner
uint loadMeshletVertexIndex(DrawContext draw, GpuMeshletDesc meshlet, uint vi)
{
    return bdaPtr<uint>(draw.model.meshletVertices)[meshlet.vertexOffset + vi];
}

GpuVertex loadVertex(DrawContext draw, uint vertexIndex)
{
    return bdaPtr<GpuVertex>(draw.model.vertices)[vertexIndex];
}

// glTF default weights only; animated weights would come from the instance
float3 applyMorphPosition(DrawContext draw, uint vertexIndex, float3 position)
{
    const GpuPrimitive primitive = draw.primitive;
    if (primitive.morphCount == 0 || primitive.morphWeightFirst == kNoneIndex)
    {
        return position;
    }
    // a target holds one delta per primitive vertex
    const uint local = vertexIndex - primitive.firstVertex;
    for (uint t = 0; t < primitive.morphCount; ++t)
    {
        const float weight = bdaPtr<float>(draw.model.morphWeights)[primitive.morphWeightFirst + t];
        const GpuMorphTarget target = bdaPtr<GpuMorphTarget>(draw.model.morphTargets)[primitive.morphFirst + t];
        if (weight != 0.0 && target.posOffset != kNoneIndex)
        {
            position += weight * bdaPtr<float3>(draw.model.morphPos)[target.posOffset + local];
        }
    }
    return position;
}

uint3 loadMeshletTriangle(DrawContext draw, GpuMeshletDesc meshlet, uint ti)
{
    uint8_t* corners = bdaPtr<uint8_t>(draw.model.meshletTriangles);
    uint base = meshlet.triangleOffset + ti * 3;
    return uint3(uint(corners[base + 0]), uint(corners[base + 1]), uint(corners[base + 2]));
}

#endif
