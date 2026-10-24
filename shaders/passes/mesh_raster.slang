#include "types.slang"
#include "core/binding.slang"
#include "core/frame.slang"
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
    DrawContext draw = loadDrawContext(push);
    const uint meshletIndex = draw.record.meshletOffset + gid;
    if (gid >= push.meshletCount || meshletIndex >= draw.primitive.meshletCount)
    {
        SetMeshOutputCounts(0, 0);
        return;
    }

    GpuMeshletDesc meshlet = loadMeshlet(draw, meshletIndex);
    const uint vertCount = min(meshlet.vertexCount, kMaxMeshletVertices);
    const uint primCount = min(meshlet.triangleCount, kMaxMeshletTriangles);
    SetMeshOutputCounts(vertCount, primCount);

    if (vertCount == 0)
    {
        return;
    }

    float4x4 mvp = objectToClip(loadCamera(draw.root), draw.nodeInstance.worldMatrix);

    for (uint vi = gtid; vi < vertCount; vi += kGroupSize)
    {
        const uint vertexIndex = loadMeshletVertexIndex(draw, meshlet, vi);
        GpuVertex v = loadVertex(draw, vertexIndex);
        verts[vi].pos = mul(mvp, float4(applyMorphPosition(draw, vertexIndex, v.pos), 1.0));
        verts[vi].fragColor = v.color;
        verts[vi].fragTexCoord = v.texCoord;
    }

    // a mirrored placement reverses winding; swapping two corners restores the front face
    const bool isMirrored = (draw.nodeInstance.flags & kNodeFlagFlipWinding) != 0;
    for (uint ti = gtid; ti < primCount; ti += kGroupSize)
    {
        const uint3 tri = loadMeshletTriangle(draw, meshlet, ti);
        tris[ti] = isMirrored ? tri.xzy : tri;
    }
}
