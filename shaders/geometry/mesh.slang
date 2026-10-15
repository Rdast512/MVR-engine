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

GpuVertex loadMeshletVertex(DrawContext draw, GpuMeshletDesc meshlet, uint vi)
{
    uint vertexIndex = bdaPtr<uint>(draw.model.meshletVertices)[meshlet.vertexOffset + vi];
    return bdaPtr<GpuVertex>(draw.model.vertices)[vertexIndex];
}

uint3 loadMeshletTriangle(DrawContext draw, GpuMeshletDesc meshlet, uint ti)
{
    uint8_t* corners = bdaPtr<uint8_t>(draw.model.meshletTriangles);
    uint base = meshlet.triangleOffset + ti * 3;
    return uint3(uint(corners[base + 0]), uint(corners[base + 1]), uint(corners[base + 2]));
}

#endif
