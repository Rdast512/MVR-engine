module surface;

import types;
import core.binding;
import core.frame;
import core.math;
import core.culling;
import geometry.mesh;
import geometry.visbuffer;

// one triangle's surface at a pixel, rebuilt from (node instance, meshlet, triangle): the vis-buffer resolve here,
// ray hits (M2 decodeHit) with ray barycentrics and ray-cone gradients

// the triangle's corners as the rasterizer saw them
public struct TriangleCorners
{
    public uint3 vertexIndex; // model-relative, in meshlet order (not winding-adjusted)
    public float3 objectPos[3];
    // kept from the position load, so buildSurface does not fetch the vertices again
    public float3 color[3];
    public float2 texCoord[3];
    public float4 clip[3];      // jittered; zero for ray hits
    // front face of the winding the rasterizer received (FlipWinding applied), or the face a ray hit
    public bool isFrontFacing;
};

// ids, positions and the attributes loaded with them; projection and facing are the caller's
public TriangleCorners loadCornerVertices(DrawContext draw, uint triangle)
{
    const GpuMeshletDesc meshlet = loadMeshlet(draw);
    const uint3 local = loadMeshletTriangle(draw, meshlet, triangle);
    TriangleCorners corners;
    [unroll]
    for (uint i = 0; i < 3; ++i)
    {
        const uint vertexIndex = loadMeshletVertexIndex(draw, meshlet, local[i]);
        corners.vertexIndex[i] = vertexIndex;
        const GpuVertex vertex = loadVertex(draw, vertexIndex);
        corners.objectPos[i] = vertex.pos;
        corners.color[i] = vertex.color;
        corners.texCoord[i] = vertex.texCoord;
        corners.clip[i] = float4(0.0, 0.0, 0.0, 0.0);
    }
    corners.isFrontFacing = true;
    return corners;
}

// ray-hit corners (m8 MG-P4): uses the hardware-fetched CLAS positions and skips reading GpuVertex::pos (36 B/hit)
public TriangleCorners loadRayCornerVertices(DrawContext draw, uint triangle, float3 objectPos[3])
{
    const GpuMeshletDesc meshlet = loadMeshlet(draw);
    const uint3 local = loadMeshletTriangle(draw, meshlet, triangle);
    GpuVertex* vertices = bdaPtr<GpuVertex>(draw.model.vertices);
    TriangleCorners corners;
    [unroll]
    for (uint i = 0; i < 3; ++i)
    {
        const uint vertexIndex = loadMeshletVertexIndex(draw, meshlet, local[i]);
        corners.vertexIndex[i] = vertexIndex;
        corners.objectPos[i] = objectPos[i];
        corners.color[i] = vertices[vertexIndex].color;
        corners.texCoord[i] = vertices[vertexIndex].texCoord;
        corners.clip[i] = float4(0.0, 0.0, 0.0, 0.0);
    }
    corners.isFrontFacing = true;
    return corners;
}

public TriangleCorners loadTriangleCorners(DrawContext draw, GpuCameraData camera, uint triangle)
{
    TriangleCorners corners = loadCornerVertices(draw, triangle);
    const float4x4 worldViewProj = objectToClip(camera, draw.nodeInstance.worldMatrix);
    [unroll]
    for (uint i = 0; i < 3; ++i)
    {
        corners.clip[i] = jitterClip(camera, mul(worldViewProj, float4(corners.objectPos[i], 1.0)));
    }
    const bool isFlipped = (draw.nodeInstance.flags & kNodeFlagFlipWinding) != 0;
    corners.isFrontFacing = isFlipped ? isFrontFacingHW(corners.clip[0], corners.clip[2], corners.clip[1])
                                      : isFrontFacingHW(corners.clip[0], corners.clip[1], corners.clip[2]);
    return corners;
}

public struct Surface
{
    public float3 position;     // world
    public float3 prevPosition; // world, previous instance transform
    public float3 geoNormal;    // world, toward the viewer's side for visible back faces
    public float3 normal;       // world vertex normal, facing-corrected; geoNormal when the mesh has none
    public float4 tangent;      // world xyz, w = bitangent sign; w = 0 when the mesh has none
    public float3 color;        // COLOR_0
    public float2 uv[2];
    public float2 uvDdx[2];
    public float2 uvDdy[2];
    public bool isFrontFacing;
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

public float2 loadVertexUv1(DrawContext draw, uint vertexIndex)
{
    return (draw.model.attributeMask & kModelAttributeUv1) != 0
        ? bdaPtr<float2>(draw.model.uv1)[vertexIndex]
        : float2(0.0, 0.0);
}

// below this normalize can return inf or NaN
static const float kMinGeoNormalLengthSq = 1e-30;
// degenerate triangle without vertex normals: any unit vector keeps the guides finite
static const float3 kFallbackNormal = float3(0.0, 0.0, 1.0);
// grazing hits: bounds the 1/sqrt(cos) growth of the ray-cone footprint
static const float kMinConeCosine = 1e-4;

// everything but the uv gradients, which raster and rays derive differently; bary.ddx/ddy are not read.
// uv1 holds the corners' second uv set for those gradients
Surface buildSurfaceAt(DrawContext draw, TriangleCorners corners, Barycentrics bary, out float2 uv1[3])
{
    const float4x4 world = draw.nodeInstance.worldMatrix;
    const float3 objectPos = interpolate(bary, corners.objectPos[0], corners.objectPos[1], corners.objectPos[2]);

    Surface surface;
    surface.isFrontFacing = corners.isFrontFacing;
    surface.position = mul(world, float4(objectPos, 1.0)).xyz;
    surface.prevPosition = mul(draw.nodeInstance.prevWorldMatrix, float4(objectPos, 1.0)).xyz;

    float3 normals[3];
    float4 tangents[3];
    [unroll]
    for (uint i = 0; i < 3; ++i)
    {
        normals[i] = loadVertexNormal(draw, corners.vertexIndex[i]);
        tangents[i] = loadVertexTangent(draw, corners.vertexIndex[i]);
        uv1[i] = loadVertexUv1(draw, corners.vertexIndex[i]);
    }

    // front faces wind counter-clockwise in object space; the normal matrix keeps it outward under mirroring
    const float3x3 toWorldNormal = normalMatrix(world);
    const float facing = corners.isFrontFacing ? 1.0 : -1.0;
    const float3 worldGeoNormal = mul(
        toWorldNormal, cross(corners.objectPos[1] - corners.objectPos[0], corners.objectPos[2] - corners.objectPos[0]));
    const float3 objectNormal = interpolate(bary, normals[0], normals[1], normals[2]);
    const bool hasGeoNormal = dot(worldGeoNormal, worldGeoNormal) > kMinGeoNormalLengthSq;
    const bool hasVertexNormal = dot(objectNormal, objectNormal) > 0.0;
    const float3 vertexNormal =
        hasVertexNormal ? normalize(mul(toWorldNormal, objectNormal)) * facing : kFallbackNormal;
    // slivers underflow the cross product: the vertex normal stands in
    surface.geoNormal = hasGeoNormal ? normalize(worldGeoNormal) * facing : vertexNormal;
    surface.normal = hasVertexNormal ? vertexNormal : surface.geoNormal;

    const float4 objectTangent = interpolate(bary, tangents[0], tangents[1], tangents[2]);
    surface.tangent = float4(0.0, 0.0, 0.0, 0.0);
    if (dot(objectTangent.xyz, objectTangent.xyz) > 0.0)
    {
        // mirroring reverses the bitangent's handedness
        const bool isFlipped = (draw.nodeInstance.flags & kNodeFlagFlipWinding) != 0;
        const float sign = (objectTangent.w < 0.0 ? -1.0 : 1.0) * (isFlipped ? -1.0 : 1.0);
        surface.tangent = float4(normalize(mul((float3x3)world, objectTangent.xyz)), sign);
    }

    surface.color = interpolate(bary, corners.color[0], corners.color[1], corners.color[2]);
    surface.uv[0] = interpolate(bary, corners.texCoord[0], corners.texCoord[1], corners.texCoord[2]);
    surface.uv[1] = interpolate(bary, uv1[0], uv1[1], uv1[2]);
    return surface;
}

// vis-buffer pixel: exact screen gradients from the barycentric derivatives
public Surface buildSurface(DrawContext draw, TriangleCorners corners, Barycentrics bary)
{
    float2 uv1[3];
    Surface surface = buildSurfaceAt(draw, corners, bary, uv1);
    surface.uvDdx[0] = interpolateDdx(bary, corners.texCoord[0], corners.texCoord[1], corners.texCoord[2]);
    surface.uvDdy[0] = interpolateDdy(bary, corners.texCoord[0], corners.texCoord[1], corners.texCoord[2]);
    surface.uvDdx[1] = interpolateDdx(bary, uv1[0], uv1[1], uv1[2]);
    surface.uvDdy[1] = interpolateDdy(bary, uv1[0], uv1[1], uv1[2]);
    return surface;
}

// uv units per world unit of one uv set over the triangle: sqrt(uv area / world area)
float uvDensity(float2 uv[3], float worldArea)
{
    const float2 e1 = uv[1] - uv[0];
    const float2 e2 = uv[2] - uv[0];
    return worldArea > 0.0 ? sqrt(abs(e1.x * e2.y - e1.y * e2.x) / worldArea) : 0.0;
}

// ray hit (spec/features/ray_tracing_mega_geometry.md §7, m8 MG-P4): uses hit objectToWorld / worldToObject directly,
// skips prevPosition and normalMatrix() cofactor ALU, and derives isotropic gradients from the ray-cone width
public Surface buildRaySurface(DrawContext draw, TriangleCorners corners, float2 hitBarycentrics, float coneWidth,
                               float3 rayDirection, float3x4 objectToWorld, float3x4 worldToObject)
{
    Barycentrics bary;
    bary.lambda = float3(1.0 - hitBarycentrics.x - hitBarycentrics.y, hitBarycentrics.x, hitBarycentrics.y);
    bary.ddx = float3(0.0, 0.0, 0.0);
    bary.ddy = float3(0.0, 0.0, 0.0);

    const float3 objectPos = interpolate(bary, corners.objectPos[0], corners.objectPos[1], corners.objectPos[2]);
    const float3x3 toWorld = (float3x3)objectToWorld;
    // inverse-transpose of objectToWorld: normalMatrix()'s direction, mirrored or not
    const float3x3 toWorldNormal = transpose((float3x3)worldToObject);
    const float3 edge1 = corners.objectPos[1] - corners.objectPos[0];
    const float3 edge2 = corners.objectPos[2] - corners.objectPos[0];
    const float3 worldGeoNormal = mul(toWorldNormal, cross(edge1, edge2));
    const bool isFrontFacing = dot(worldGeoNormal, rayDirection) < 0.0;
    const float facing = isFrontFacing ? 1.0 : -1.0;

    float3 normals[3];
    float4 tangents[3];
    float2 uv1[3];
    [unroll]
    for (uint i = 0; i < 3; ++i)
    {
        normals[i] = loadVertexNormal(draw, corners.vertexIndex[i]);
        tangents[i] = loadVertexTangent(draw, corners.vertexIndex[i]);
        uv1[i] = loadVertexUv1(draw, corners.vertexIndex[i]);
    }

    Surface surface;
    surface.isFrontFacing = isFrontFacing;
    surface.position = mul(objectToWorld, float4(objectPos, 1.0));
    surface.prevPosition = float3(0.0, 0.0, 0.0);

    const float3 objectNormal = interpolate(bary, normals[0], normals[1], normals[2]);
    const bool hasGeoNormal = dot(worldGeoNormal, worldGeoNormal) > kMinGeoNormalLengthSq;
    const bool hasVertexNormal = dot(objectNormal, objectNormal) > 0.0;
    const float3 vertexNormal =
        hasVertexNormal ? normalize(mul(toWorldNormal, objectNormal)) * facing : kFallbackNormal;
    surface.geoNormal = hasGeoNormal ? normalize(worldGeoNormal) * facing : vertexNormal;
    surface.normal = hasVertexNormal ? vertexNormal : surface.geoNormal;

    const float4 objectTangent = interpolate(bary, tangents[0], tangents[1], tangents[2]);
    surface.tangent = float4(0.0, 0.0, 0.0, 0.0);
    if (dot(objectTangent.xyz, objectTangent.xyz) > 0.0)
    {
        const bool isFlipped = (draw.nodeInstance.flags & kNodeFlagFlipWinding) != 0;
        const float sign = (objectTangent.w < 0.0 ? -1.0 : 1.0) * (isFlipped ? -1.0 : 1.0);
        surface.tangent = float4(normalize(mul(toWorld, objectTangent.xyz)), sign);
    }

    surface.color = interpolate(bary, corners.color[0], corners.color[1], corners.color[2]);
    surface.uv[0] = interpolate(bary, corners.texCoord[0], corners.texCoord[1], corners.texCoord[2]);
    surface.uv[1] = interpolate(bary, uv1[0], uv1[1], uv1[2]);

    const float worldArea = length(cross(mul(toWorld, edge1), mul(toWorld, edge2)));
    // a tilted surface stretches the footprint along one axis by 1/cos: sqrt keeps the area
    const float cosine = max(abs(dot(rayDirection, surface.geoNormal)), kMinConeCosine);
    const float footprint = coneWidth / sqrt(cosine);
    const float foot0 = footprint * uvDensity(corners.texCoord, worldArea);
    const float foot1 = footprint * uvDensity(uv1, worldArea);
    surface.uvDdx[0] = float2(foot0, 0.0);
    surface.uvDdy[0] = float2(0.0, foot0);
    surface.uvDdx[1] = float2(foot1, 0.0);
    surface.uvDdy[1] = float2(0.0, foot1);
    return surface;
}
