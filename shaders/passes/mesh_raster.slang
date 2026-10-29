#include "types.slang"
#include "core/binding.slang"
#include "core/frame.slang"
#include "core/math.slang"
#include "geometry/mesh.slang"

[[vk::push_constant]] ConstantBuffer<MeshPushData> push;

// one workgroup per visible meshlet; the cull pass already dropped empty and culled ones
[shader("mesh")]
[numthreads(kGroupSize, 1, 1)]
[outputtopology("triangle")]
void meshMain(
    uint gtid : SV_GroupThreadID,
    uint gid  : SV_GroupID,
    out vertices MeshVertexOut verts[kMaxMeshletVertices],
    out indices uint3 tris[kMaxMeshletTriangles])
{
    DrawContext draw = loadDrawContext(push, gid);
    GpuMeshletDesc meshlet = loadMeshlet(draw);
    const uint vertCount = min(uint(meshlet.vertexCount), kMaxMeshletVertices);
    const uint primCount = min(uint(meshlet.triangleCount), kMaxMeshletTriangles);
    SetMeshOutputCounts(vertCount, primCount);

    float4x4 mvp = objectToClip(loadCamera(draw.root), draw.nodeInstance.worldMatrix);
    const uint material = materialRow(draw.primitive);

    for (uint vi = gtid; vi < vertCount; vi += kGroupSize)
    {
        const GpuVertex v = loadMeshletCorner(draw, meshlet, vi);
        verts[vi].pos = mul(mvp, float4(v.pos, 1.0));
        verts[vi].fragColor = v.color;
        verts[vi].fragTexCoord = v.texCoord;
        verts[vi].materialRow = material;
    }

    for (uint ti = gtid; ti < primCount; ti += kGroupSize)
    {
        tris[ti] = loadFrontFacingTriangle(draw, meshlet, ti);
    }
}
