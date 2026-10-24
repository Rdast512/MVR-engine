#include "types.slang"
#include "core/binding.slang"
#include "core/frame.slang"
#include "core/math.slang"
#include "geometry/mesh.slang"

// geometry debug views: same meshlet path as mesh_raster, colored by id instead of material

[[vk::push_constant]] ConstantBuffer<MeshPushData> push;

// meshlets hold at most kMaxMeshletTriangles (< 128) triangles
static const uint kTriangleIdBits = 7;

struct MeshDebugVertexOut
{
    float4 pos     : SV_Position;
    float3 viewPos : VIEW_POS;
};

struct MeshDebugPrimitiveOut
{
    // (model-relative meshlet << kTriangleIdBits) | triangle
    uint primitiveId : SV_PrimitiveID;
};

[shader("mesh")]
[numthreads(kGroupSize, 1, 1)]
[outputtopology("triangle")]
void meshDebugMain(
    uint gtid : SV_GroupThreadID,
    uint gid  : SV_GroupID,
    out vertices MeshDebugVertexOut verts[kMaxMeshletVertices],
    out indices uint3 tris[kMaxMeshletTriangles],
    out primitives MeshDebugPrimitiveOut prims[kMaxMeshletTriangles])
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

    const GpuCameraData camera = loadCamera(draw.root);
    const float4x4 modelView = mul(camera.view, draw.nodeInstance.worldMatrix);

    for (uint vi = gtid; vi < vertCount; vi += kGroupSize)
    {
        const float4 viewPos = mul(modelView, float4(loadMeshletCorner(draw, meshlet, vi).pos, 1.0));
        verts[vi].pos = mul(camera.proj, viewPos);
        verts[vi].viewPos = viewPos.xyz;
    }

    // model-relative: instances of one model show the same meshlet colors
    const uint meshletId = draw.primitive.firstMeshlet + meshletIndex;
    for (uint ti = gtid; ti < primCount; ti += kGroupSize)
    {
        tris[ti] = loadFrontFacingTriangle(draw, meshlet, ti);
        prims[ti].primitiveId = (meshletId << kTriangleIdBits) | ti;
    }
}

// pcg: neighbouring ids map to unrelated colors
uint hashId(uint value)
{
    const uint state = value * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float3 idColor(uint id)
{
    const uint hash = hashId(id);
    const float hue = float(hash & 0xffffu) / 65535.0;
    const float saturation = 0.55 + 0.45 * float((hash >> 16) & 0xffu) / 255.0;
    const float3 rgb = saturate(abs(frac(hue + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
    return lerp(float3(1.0, 1.0, 1.0), rgb, saturation);
}

uint debugId(uint mode, uint primitiveId)
{
    switch (mode)
    {
    case kDebugViewMeshlets:
        return primitiveId >> kTriangleIdBits;
    case kDebugViewPrimitives:
        return loadDrawContext(push).record.primitiveIndex;
    case kDebugViewNodeInstances:
        return loadDrawContext(push).record.nodeInstanceIndex;
    default:
        return primitiveId;
    }
}

[shader("fragment")]
float4 fragDebugMain(MeshDebugVertexOut vertIn, uint primitiveId : SV_PrimitiveID,
                     float3 barycentrics : SV_Barycentrics) : SV_TARGET
{
    const uint debugView = bdaPtr<GpuFrameRoot>(push.frameRoot)[0].debugView;
    float3 color = idColor(debugId(debugView & kDebugViewModeMask, primitiveId));

    // flat headlight from the face normal keeps the shape readable under flat colors
    const float3 faceNormal = normalize(cross(ddx(vertIn.viewPos), ddy(vertIn.viewPos)));
    const float headlight = abs(dot(faceNormal, normalize(vertIn.viewPos)));
    color *= 0.35 + 0.65 * headlight;

    if ((debugView & kDebugViewFlagEdges) != 0)
    {
        // about one pixel wide at any distance
        const float3 edgeDistance = barycentrics / max(fwidth(barycentrics), 1e-6);
        const float edge = 1.0 - saturate(min(edgeDistance.x, min(edgeDistance.y, edgeDistance.z)));
        color = lerp(color, float3(0.0, 0.0, 0.0), edge);
    }
    return float4(color, 1.0);
}
