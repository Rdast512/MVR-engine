#include "types.slang"
#include "core/binding.slang"
#include "core/frame.slang"
#include "core/math.slang"
#include "geometry/mesh.slang"
#include "geometry/triangle_cull.slang"

[[vk::push_constant]] ConstantBuffer<MeshPushData> push;

struct MeshPrimitiveOut
{
    bool cull : SV_CullPrimitive;
};

// mesh outputs are write-only, so the triangle tests read the corners from here
groupshared float4 clipCorners[kMaxMeshletVertices];

// one workgroup per visible meshlet; the cull pass already dropped empty and culled ones
[shader("mesh")]
[numthreads(kGroupSize, 1, 1)]
[outputtopology("triangle")]
void meshMain(
    uint gtid : SV_GroupThreadID,
    uint gid  : SV_GroupID,
    out vertices MeshVertexOut verts[kMaxMeshletVertices],
    out indices uint3 tris[kMaxMeshletTriangles],
    out primitives MeshPrimitiveOut prims[kMaxMeshletTriangles])
{
    DrawContext draw = loadDrawContext(push, gid);
    GpuMeshletDesc meshlet = loadMeshlet(draw);
    const uint vertCount = min(uint(meshlet.vertexCount), kMaxMeshletVertices);
    const uint primCount = min(uint(meshlet.triangleCount), kMaxMeshletTriangles);
    SetMeshOutputCounts(vertCount, primCount);

    const GpuCameraData camera = loadCamera(draw.root);
    float4x4 mvp = objectToClip(camera, draw.nodeInstance.worldMatrix);
    const uint material = materialRow(draw.primitive);

    for (uint vi = gtid; vi < vertCount; vi += kGroupSize)
    {
        const GpuVertex v = loadMeshletCorner(draw, meshlet, vi);
        const float4 clip = mul(mvp, float4(v.pos, 1.0));
        verts[vi].pos = clip;
        verts[vi].fragColor = v.color;
        verts[vi].fragTexCoord = v.texCoord;
        verts[vi].materialRow = material;
        clipCorners[vi] = clip;
    }

    // uniform for the whole draw, so the barrier stays in uniform control flow
    const uint triangleCull = draw.root.triangleCull;
    const bool isCulling = (triangleCull & (kTriangleCullBackface | kTriangleCullSmall)) != 0;
    if (isCulling)
    {
        GroupMemoryBarrierWithGroupSync();
    }
    const bool isDoubleSided = draw.isDoubleSided;

    uint backface = 0;
    uint small = 0;
    for (uint ti = gtid; ti < primCount; ti += kGroupSize)
    {
        const uint3 tri = loadFrontFacingTriangle(draw, meshlet, ti);
        tris[ti] = tri;
        const uint reason = isCulling
            ? triangleCullReason(clipCorners[tri.x], clipCorners[tri.y], clipCorners[tri.z], triangleCull,
                                 isDoubleSided, camera.renderTargetSize, draw.root.subpixelSnap)
            : kTriangleKept;
        prims[ti].cull = reason != kTriangleKept;
        backface += reason == kTriangleBackface ? 1 : 0;
        small += reason == kTriangleSmall ? 1 : 0;
    }
    if ((triangleCull & kTriangleCullStats) != 0)
    {
        const uint tested = gtid < primCount ? (primCount - gtid + kGroupSize - 1) / kGroupSize : 0;
        countTriangles(draw, tested, backface, small);
    }
}
