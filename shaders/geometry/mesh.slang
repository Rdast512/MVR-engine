#ifndef ENGINE_GEOMETRY_MESH_SLANG
#define ENGINE_GEOMETRY_MESH_SLANG

#include "types.slang"
#include "core/binding.slang"
#include "core/math.slang"

GpuMeshletDesc loadMeshlet(MeshPushData push, uint gid)
{
    GpuMeshletDesc* table = bdaPtr<GpuMeshletDesc>(push.meshlets);
    return table[push.firstMeshlet + gid];
}

GpuVertex loadMeshletVertex(MeshPushData push, GpuMeshletDesc meshlet, uint vi)
{
    uint* meshletVertTable = bdaPtr<uint>(push.meshletVertices);
    GpuVertex* vertexTable = bdaPtr<GpuVertex>(push.vertices);
    uint globalVertexIndex = meshletVertTable[meshlet.vertexOffset + vi];
    return vertexTable[globalVertexIndex];
}

uint3 loadMeshletTriangle(MeshPushData push, GpuMeshletDesc meshlet, uint ti)
{
    uint8_t* meshletTriTable = bdaPtr<uint8_t>(push.meshletTriangles);
    uint base = meshlet.triangleOffset + ti * 3;
    return uint3(
        uint(meshletTriTable[base + 0]),
        uint(meshletTriTable[base + 1]),
        uint(meshletTriTable[base + 2]));
}

#endif
