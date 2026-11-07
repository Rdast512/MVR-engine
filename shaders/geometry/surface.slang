#ifndef ENGINE_GEOMETRY_SURFACE_SLANG
#define ENGINE_GEOMETRY_SURFACE_SLANG

#include "types.slang"
#include "core/binding.slang"
#include "core/culling.slang"
#include "core/frame.slang"
#include "core/math.slang"
#include "geometry/mesh.slang"
#include "geometry/visbuffer.slang"

// one triangle's surface at a pixel, rebuilt from (node instance, meshlet, triangle): the vis-buffer resolve here,
// ray hits (M2 decodeHit) with ray barycentrics and ray-cone gradients

// the triangle's corners as the rasterizer saw them
struct TriangleCorners
{
    uint3 vertexIndex; // model-relative, in meshlet order (not winding-adjusted)
    float3 objectPos[3]; // default morph weights applied, as in mesh_raster
    float4 clip[3];      // jittered
    // front face of the winding the rasterizer received (FlipWinding applied)
    bool isFrontFacing;
};

TriangleCorners loadTriangleCorners(DrawContext draw, GpuCameraData camera, uint triangle)
{
    const GpuMeshletDesc meshlet = loadMeshlet(draw);
    const uint3 local = loadMeshletTriangle(draw, meshlet, triangle);
    const float4x4 worldViewProj = mul(camera.viewProj, draw.nodeInstance.worldMatrix);
    TriangleCorners corners;
    [unroll]
    for (uint i = 0; i < 3; ++i)
    {
        const uint vertexIndex = loadMeshletVertexIndex(draw, meshlet, local[i]);
        corners.vertexIndex[i] = vertexIndex;
        corners.objectPos[i] = applyMorphPosition(draw, vertexIndex, loadVertex(draw, vertexIndex).pos);
        corners.clip[i] = jitterClip(camera, mul(worldViewProj, float4(corners.objectPos[i], 1.0)));
    }
    const bool isFlipped = (draw.nodeInstance.flags & kNodeFlagFlipWinding) != 0;
    corners.isFrontFacing = isFlipped ? isFrontFacingHW(corners.clip[0], corners.clip[2], corners.clip[1])
                                      : isFrontFacingHW(corners.clip[0], corners.clip[1], corners.clip[2]);
    return corners;
}

struct Surface
{
    float3 position;     // world
    float3 prevPosition; // world, previous instance transform
    float3 geoNormal;    // world, toward the viewer's side for visible back faces
    float3 normal;       // world vertex normal, facing-corrected; geoNormal when the mesh has none
    float4 tangent;      // world xyz, w = bitangent sign; w = 0 when the mesh has none
    float3 color;        // COLOR_0
    float2 uv[2];
    float2 uvDdx[2];
    float2 uvDdy[2];
    bool isFrontFacing;
};

// a zero row in an optional stream means the primitive has no such attribute
float3 loadVertexNormal(DrawContext draw, uint vertexIndex)
{
    return (draw.model.attributeMask & kModelAttributeNormals) != 0
        ? bdaPtr<float3>(draw.model.normals)[vertexIndex]
        : float3(0.0, 0.0, 0.0);
}

float4 loadVertexTangent(DrawContext draw, uint vertexIndex)
{
    return (draw.model.attributeMask & kModelAttributeTangents) != 0
        ? bdaPtr<float4>(draw.model.tangents)[vertexIndex]
        : float4(0.0, 0.0, 0.0, 0.0);
}

float2 loadVertexUv1(DrawContext draw, uint vertexIndex)
{
    return (draw.model.attributeMask & kModelAttributeUv1) != 0
        ? bdaPtr<float2>(draw.model.uv1)[vertexIndex]
        : float2(0.0, 0.0);
}

Surface buildSurface(DrawContext draw, TriangleCorners corners, Barycentrics bary)
{
    const float4x4 world = draw.nodeInstance.worldMatrix;
    const float3 objectPos = interpolate(bary, corners.objectPos[0], corners.objectPos[1], corners.objectPos[2]);

    Surface surface;
    surface.isFrontFacing = corners.isFrontFacing;
    surface.position = mul(world, float4(objectPos, 1.0)).xyz;
    surface.prevPosition = mul(draw.nodeInstance.prevWorldMatrix, float4(objectPos, 1.0)).xyz;

    // front faces wind counter-clockwise in object space; the normal matrix keeps it outward under mirroring
    const float3x3 toWorldNormal = normalMatrix(world);
    const float3 objectGeoNormal =
        cross(corners.objectPos[1] - corners.objectPos[0], corners.objectPos[2] - corners.objectPos[0]);
    const float facing = corners.isFrontFacing ? 1.0 : -1.0;
    surface.geoNormal = normalize(mul(toWorldNormal, objectGeoNormal)) * facing;

    GpuVertex vertices[3];
    float3 normals[3];
    float4 tangents[3];
    float2 uv1[3];
    [unroll]
    for (uint i = 0; i < 3; ++i)
    {
        vertices[i] = loadVertex(draw, corners.vertexIndex[i]);
        normals[i] = loadVertexNormal(draw, corners.vertexIndex[i]);
        tangents[i] = loadVertexTangent(draw, corners.vertexIndex[i]);
        uv1[i] = loadVertexUv1(draw, corners.vertexIndex[i]);
    }

    const float3 objectNormal = interpolate(bary, normals[0], normals[1], normals[2]);
    surface.normal = dot(objectNormal, objectNormal) > 0.0
        ? normalize(mul(toWorldNormal, objectNormal)) * facing
        : surface.geoNormal;

    const float4 objectTangent = interpolate(bary, tangents[0], tangents[1], tangents[2]);
    surface.tangent = float4(0.0, 0.0, 0.0, 0.0);
    if (dot(objectTangent.xyz, objectTangent.xyz) > 0.0)
    {
        // mirroring reverses the bitangent's handedness
        const bool isFlipped = (draw.nodeInstance.flags & kNodeFlagFlipWinding) != 0;
        const float sign = (objectTangent.w < 0.0 ? -1.0 : 1.0) * (isFlipped ? -1.0 : 1.0);
        surface.tangent = float4(normalize(mul((float3x3)world, objectTangent.xyz)), sign);
    }

    surface.color = interpolate(bary, vertices[0].color, vertices[1].color, vertices[2].color);
    surface.uv[0] = interpolate(bary, vertices[0].texCoord, vertices[1].texCoord, vertices[2].texCoord);
    surface.uvDdx[0] = interpolateDdx(bary, vertices[0].texCoord, vertices[1].texCoord, vertices[2].texCoord);
    surface.uvDdy[0] = interpolateDdy(bary, vertices[0].texCoord, vertices[1].texCoord, vertices[2].texCoord);
    surface.uv[1] = interpolate(bary, uv1[0], uv1[1], uv1[2]);
    surface.uvDdx[1] = interpolateDdx(bary, uv1[0], uv1[1], uv1[2]);
    surface.uvDdy[1] = interpolateDdy(bary, uv1[0], uv1[1], uv1[2]);
    return surface;
}

#endif
