#include "types.slang"
#include "core/binding.slang"
#include "core/math.slang"
#include "geometry/mesh.slang"

[[vk::push_constant]] ConstantBuffer<MeshPushData> push;

[shader("mesh")]
[numthreads(kGroupSize, 1, 1)]
[outputtopology("triangle")]
void meshMain(
    uint gtid : SV_GroupThreadID,
    uint gid  : SV_GroupID,
    out vertices MeshVertexOut verts[kMaxMeshletVertices],
    out indices uint3 tris[kMaxMeshletTriangles])
{
    if (gid >= push.meshletCount)
    {
        SetMeshOutputCounts(0, 0);
        return;
    }

    GpuMeshletDesc meshlet = loadMeshlet(push, gid);
    const uint vertCount = min(meshlet.vertexCount, kMaxMeshletVertices);
    const uint primCount = min(meshlet.triangleCount, kMaxMeshletTriangles);
    SetMeshOutputCounts(vertCount, primCount);

    if (vertCount == 0)
    {
        return;
    }

    GpuObjectUB object = bdaPtr<GpuObjectUB>(push.objectUbAddress)[0];
    GpuCameraData camera = bdaPtr<GpuCameraData>(push.cameraAddress)[0];
    float4x4 mvp = objectToClip(camera, object);

    for (uint vi = gtid; vi < vertCount; vi += kGroupSize)
    {
        GpuVertex v = loadMeshletVertex(push, meshlet, vi);
        verts[vi].pos = mul(mvp, float4(v.pos, 1.0));
        verts[vi].fragColor = v.color;
        verts[vi].fragTexCoord = v.texCoord;
    }

    for (uint ti = gtid; ti < primCount; ti += kGroupSize)
    {
        tris[ti] = loadMeshletTriangle(push, meshlet, ti);
    }
}
