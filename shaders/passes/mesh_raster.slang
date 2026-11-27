import types;
import core.frame;
import core.math;
import core.culling;
import geometry.mesh;
import geometry.surface;
import geometry.triangle_cull;
import geometry.visbuffer;
import material.pbr;

// vis-buffer raster: position per vertex, the payload per primitive (passes/mesh_fragment.slang writes it). The
// alpha-tested bin (meshAlphaMain, passes/mesh_alpha_fragment.slang) adds the base-color uv and the alpha test inputs

[[vk::push_constant]] ConstantBuffer<MeshPushData> push;

struct MeshPrimitiveOut
{
    uint payload : SV_PrimitiveID;
    bool cull : SV_CullPrimitive;
};

// one vertex output per raster bin
interface IRasterVertex
{
    // what every vertex of the workgroup's meshlet shares
    associatedtype Shared;
    static Shared loadShared(DrawContext draw, GpuCameraData camera);
    static This create(Shared shared, DrawContext draw, uint vertexIndex, GpuVertex vertex, float4 clip);
}

struct NoShared
{
};

extension MeshVertexOut : IRasterVertex
{
    typealias Shared = NoShared;

    static NoShared loadShared(DrawContext draw, GpuCameraData camera)
    {
        return {};
    }

    static MeshVertexOut create(NoShared shared, DrawContext draw, uint vertexIndex, GpuVertex vertex, float4 clip)
    {
        MeshVertexOut result;
        result.pos = clip;
        return result;
    }
}

struct AlphaShared
{
    GpuMaterial material;
    uint2 baseColor;
    float2 alphaTest;
};

extension MeshAlphaVertexOut : IRasterVertex
{
    typealias Shared = AlphaShared;

    // the bin holds only isAlphaTested materials: textured, factor alpha >= cutoff > 0
    static AlphaShared loadShared(DrawContext draw, GpuCameraData camera)
    {
        AlphaShared shared;
        shared.material = loadMaterial(draw.root, materialRow(draw.primitive));
        shared.baseColor = uint2(shared.material.baseColorTex, shared.material.baseColorSamp);
        shared.alphaTest = float2(alphaTestCutoff(shared.material) / shared.material.baseColorFactor.a, camera.lodBias);
        return shared;
    }

    static MeshAlphaVertexOut create(AlphaShared shared, DrawContext draw, uint vertexIndex, GpuVertex vertex,
                                     float4 clip)
    {
        float2 uv1 = float2(0.0, 0.0);
        if (shared.material.baseColorUv != 0)
        {
            uv1 = loadVertexUv1(draw, vertexIndex);
        }
        MeshAlphaVertexOut result;
        result.pos = clip;
        result.uv = baseColorUv(shared.material, loadVertexUv0(draw, vertexIndex, vertex.texCoord), uv1);
        result.baseColor = shared.baseColor;
        result.alphaTest = shared.alphaTest;
        return result;
    }
}

// mesh outputs are write-only, so the triangle tests read the corners from here
groupshared float4 clipCorners[kMaxMeshletVertices];

// one workgroup per visible meshlet; the cull pass already dropped empty and culled ones
void rasterMeshlet<V : IRasterVertex>(
    MeshPushData drawPush,
    uint gtid,
    uint gid,
    out vertices V verts[kMaxMeshletVertices],
    out indices uint3 tris[kMaxMeshletTriangles],
    out primitives MeshPrimitiveOut prims[kMaxMeshletTriangles])
{
    DrawContext draw = loadDrawContext(drawPush, gid);
    GpuMeshletDesc meshlet = loadMeshlet(draw);
    const uint vertCount = min(uint(meshlet.vertexCount), kMaxMeshletVertices);
    const uint primCount = min(uint(meshlet.triangleCount), kMaxMeshletTriangles);
    SetMeshOutputCounts(vertCount, primCount);

    const GpuCameraData camera = loadCamera(draw.root);
    float4x4 mvp = objectToClip(camera, draw.nodeInstance.worldMatrix);
    const V.Shared shared = V.loadShared(draw, camera);

    for (uint vi = gtid; vi < vertCount; vi += kGroupSize)
    {
        const uint vertexIndex = loadMeshletVertexIndex(draw, meshlet, vi);
        const GpuVertex vertex = loadVertex(draw, vertexIndex);
        const float4 clip = jitterClip(camera, mul(mvp, float4(vertex.pos, 1.0)));
        verts[vi] = V.create(shared, draw, vertexIndex, vertex, clip);
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
    rasterMeshlet<MeshVertexOut>(push, gtid, gid, verts, tris, prims);
}

[shader("mesh")]
[numthreads(kGroupSize, 1, 1)]
[outputtopology("triangle")]
void meshAlphaMain(
    uint gtid : SV_GroupThreadID,
    uint gid  : SV_GroupID,
    out vertices MeshAlphaVertexOut verts[kMaxMeshletVertices],
    out indices uint3 tris[kMaxMeshletTriangles],
    out primitives MeshPrimitiveOut prims[kMaxMeshletTriangles])
{
    rasterMeshlet<MeshAlphaVertexOut>(push, gtid, gid, verts, tris, prims);
}

// without DGC (default): one vkCmdDrawMeshTasksIndirectCountEXT per (phase, bin) over the DGC sequences. The push
// holds the bin's first row and meshletsPerSequence; draw i is sequence i of buildMeshSequences
MeshPushData indirectDrawPush(uint drawIndex)
{
    MeshPushData drawPush = push;
    drawPush.firstVisible += drawIndex * push.meshletCount;
    return drawPush;
}

[shader("mesh")]
[numthreads(kGroupSize, 1, 1)]
[outputtopology("triangle")]
void meshIndirectMain(
    uint gtid : SV_GroupThreadID,
    uint gid  : SV_GroupID,
    uint drawIndex : SV_DrawIndex,
    out vertices MeshVertexOut verts[kMaxMeshletVertices],
    out indices uint3 tris[kMaxMeshletTriangles],
    out primitives MeshPrimitiveOut prims[kMaxMeshletTriangles])
{
    rasterMeshlet<MeshVertexOut>(indirectDrawPush(drawIndex), gtid, gid, verts, tris, prims);
}

[shader("mesh")]
[numthreads(kGroupSize, 1, 1)]
[outputtopology("triangle")]
void meshAlphaIndirectMain(
    uint gtid : SV_GroupThreadID,
    uint gid  : SV_GroupID,
    uint drawIndex : SV_DrawIndex,
    out vertices MeshAlphaVertexOut verts[kMaxMeshletVertices],
    out indices uint3 tris[kMaxMeshletTriangles],
    out primitives MeshPrimitiveOut prims[kMaxMeshletTriangles])
{
    rasterMeshlet<MeshAlphaVertexOut>(indirectDrawPush(drawIndex), gtid, gid, verts, tris, prims);
}
