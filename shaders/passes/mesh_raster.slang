#include "types.slang"
#include "core/binding.slang"
#include "core/frame.slang"
#include "core/math.slang"
#include "geometry/mesh.slang"
#include "geometry/triangle_cull.slang"
#include "geometry/visbuffer.slang"

// vis-buffer raster: position per vertex, the payload per primitive (passes/mesh_fragment.slang writes it)

[[vk::push_constant]] ConstantBuffer<MeshPushData> push;

struct MeshPrimitiveOut
{
    uint payload : SV_PrimitiveID;
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

    for (uint vi = gtid; vi < vertCount; vi += kGroupSize)
    {
        const float4 clip = jitterClip(camera, mul(mvp, float4(loadMeshletCorner(draw, meshlet, vi).pos, 1.0)));
        verts[vi].pos = clip;
        clipCorners[vi] = clip;
    }

    // uniform for the whole draw, so the barrier stays in uniform control flow
    const uint triangleCull = draw.root.triangleCull;
    const bool isCulling = (triangleCull & (kTriangleCullBackface | kTriangleCullSmall)) != 0;
    const bool isDoubleSided = draw.isDoubleSided;
    if (isCulling || isDoubleSided)
    {
        GroupMemoryBarrierWithGroupSync();
    }

    uint backface = 0;
    uint small = 0;
    for (uint ti = gtid; ti < primCount; ti += kGroupSize)
    {
        uint3 tri = loadFrontFacingTriangle(draw, meshlet, ti);
        // one cull mode for every bin (engine spec A6): a two-sided back face is drawn with its winding flipped.
        // The payload keeps the meshlet-local index; the resolve recomputes facing
        if (isDoubleSided && !isFrontFacingHW(clipCorners[tri.x], clipCorners[tri.y], clipCorners[tri.z]))
        {
            tri = tri.xzy;
        }
        tris[ti] = tri;
        prims[ti].payload = packVisPayload(draw.visibleRow, ti);
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
