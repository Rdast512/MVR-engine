module hit;

import types;
import core.binding;
import core.frame;
import core.math;
import geometry.surface;
import raytracing.trace;

// ray hits -> the Surface the vis-buffer resolve builds (plan C3, spec/features/ray_tracing_mega_geometry.md §5-7)

// width at the ray origin and growth per unit distance (improved ray cones, JCGT 2021)
public struct RayCone
{
    public float width;
    public float spread;
};

// a committed triangle hit as a ray query reports it
public struct RayHit
{
    public uint rayInstance; // InstanceIndex(): row of root.rayInstances
    public uint meshletRow;  // ClusterID: the CLAS clusterID is the model-relative meshlet row (C1)
    public uint triangle;    // PrimitiveIndex(): the vis-buffer triangle, since CLAS keep raster order
    public float2 barycentrics;
    public float t;
    public float3 objectPos[3]; // position fetch: the traced CLAS positions (spec §10.2)
    public float3x4 objectToWorld;
    public float3x4 worldToObject;
};

public struct PrimaryRay
{
    public RayDesc ray;
    public RayCone cone;
};

public struct DecodedHit
{
    public DrawContext draw;
    public Surface surface;
};

// angle one pixel subtends at the image center
public float pixelSpreadAngle(GpuCameraData camera, float2 targetSize)
{
    return 2.0 / (abs(camera.proj[1][1]) * targetSize.y);
}

// through the pixel center the jittered raster samples, from the near plane to the far plane, so the first surface is
// the one the vis buffer holds
public PrimaryRay primaryRay(GpuCameraData camera, uint2 pixel, float2 targetSize)
{
    // raster adds the jitter in clip space: the unjittered point under this pixel center is ndc - jitter
    const float2 ndc = pixelToNdc(float2(pixel) + 0.5, targetSize) - camera.jitterOffset;
    // reverse Z: near plane at 1, far plane at 0
    const float4 nearPoint = mul(camera.invViewProj, float4(ndc, 1.0, 1.0));
    const float4 farPoint = mul(camera.invViewProj, float4(ndc, 0.0, 1.0));
    const float3 origin = nearPoint.xyz / nearPoint.w;
    const float3 toFar = farPoint.xyz / farPoint.w - origin;

    PrimaryRay primary;
    primary.ray.Origin = origin;
    primary.ray.Direction = normalize(toFar);
    primary.ray.TMin = 0.0;
    primary.ray.TMax = length(toFar);
    // a pixel at angle theta off the axis subtends pixelAngle^2 * cos^3(theta): the isotropic spread keeps that area,
    // or corner pixels pick a mip about half a level too coarse. The origin lies on the near plane
    const float distance = length(origin - camera.cameraPos);
    const float cosine = min(camera.nearZ / distance, 1.0);
    primary.cone.spread = pixelSpreadAngle(camera, targetSize) * cosine * sqrt(cosine);
    primary.cone.width = distance * primary.cone.spread;
    return primary;
}

// closest hit under the ray contract of raytracing/trace.slang: one Proceed(), no candidates, no facing cull. OMM
// 2-state comes from the ray flag: PTLAS has no instance flag for it. instanceMask: kRayMask* (m8 I13)
public bool traceClosest(RayScene scene, RayDesc ray, uint instanceMask, out RayHit hit)
{
    hit = {};
    requireOmm2StateRayFlag();
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OMM_2_STATE> query;
    query.TraceRayInline(RaytracingAccelerationStructure(scene.tlas), RAY_FLAG_NONE, instanceMask, ray);
    countCandidate(scene.rayDebug, query.Proceed());
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return false;
    }
    hit.rayInstance = query.CommittedInstanceIndex();
    hit.meshletRow = query.CommittedClusterID();
    hit.triangle = query.CommittedPrimitiveIndex();
    hit.barycentrics = query.CommittedTriangleBarycentrics();
    hit.t = query.CommittedRayT();
    hit.objectPos = query.CommittedGetIntersectionTriangleVertexPositions();
    hit.objectToWorld = query.CommittedObjectToWorld3x4();
    hit.worldToObject = query.CommittedWorldToObject3x4();
    return true;
}

// glass is transparent to every ray but the glass pass's and the shadow transmittance's (m8 I13)
public bool traceClosest(RayScene scene, RayDesc ray, out RayHit hit)
{
    return traceClosest(scene, ray, kRayMaskOpaque, hit);
}

public bool traceClosest(GpuFrameRoot root, RayDesc ray, out RayHit hit)
{
    return traceClosest(loadRayScene(root), ray, kRayMaskOpaque, hit);
}

// the vis-buffer decode chain from (node instance, meshlet, triangle); positions and transforms come from the hit
// (m8 MG-P4)
public DecodedHit decodeHit(GpuFrameRoot root, RayHit hit, float3 rayDirection, RayCone cone)
{
    DecodedHit decoded;
    decoded.draw = loadRayDrawContext(root, hit.rayInstance, hit.meshletRow, hit.objectToWorld);
    const TriangleCorners corners = loadRayCornerVertices(decoded.draw, hit.triangle, hit.objectPos);
    decoded.surface = buildRaySurface(decoded.draw, corners, hit.barycentrics, cone.width + cone.spread * hit.t,
                                      rayDirection, hit.objectToWorld, hit.worldToObject);
    return decoded;
}

// float error bounds of the reconstruction and the two transforms (single precision ulp scales)
static const float kSpawnObjectError = 5.9604644775390625E-8;
static const float kSpawnExtentError = 1.788139769587360206060111522674560546875E-7;
static const float kSpawnTransformError = 1.19209317972490680404007434844970703125E-7;

// origin for a ray leaving the hit toward `direction`'s side of the triangle. Port of SafeSpawnPoint from
// RTXMG demo/shaders/self_intersection_avoidance.hlsli (Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES,
// BSD-3-Clause): the hit point rebuilt from the traced triangle, pushed along the world geometric normal by a bound
// on its float error instead of a fixed fraction of its distance (spec/features/ray_tracing_mega_geometry.md §6.1)
public float3 safeSpawnPoint(RayHit hit, float3 direction)
{
    const float3 v0 = hit.objectPos[0];
    precise const float3 edge1 = hit.objectPos[1] - v0;
    precise const float3 edge2 = hit.objectPos[2] - v0;
    // base vertex last: smaller object-space error
    precise const float3 objectPos = v0 + mad(hit.barycentrics.x, edge1, hit.barycentrics.y * edge2);
    const float3 objectNormal = cross(edge1, edge2);

    // translation last: smaller world-space error
    const float3x4 o2w = hit.objectToWorld;
    precise const float worldX = o2w[0][3] + mad(o2w[0][0], objectPos.x, mad(o2w[0][1], objectPos.y, o2w[0][2] * objectPos.z));
    precise const float worldY = o2w[1][3] + mad(o2w[1][0], objectPos.x, mad(o2w[1][1], objectPos.y, o2w[1][2] * objectPos.z));
    precise const float worldZ = o2w[2][3] + mad(o2w[2][0], objectPos.x, mad(o2w[2][1], objectPos.y, o2w[2][2] * objectPos.z));
    precise const float3 worldPos = float3(worldX, worldY, worldZ);

    // inverse transpose: the rows of worldToObject are the columns
    float3 worldNormal = mul(transpose((float3x3)hit.worldToObject), objectNormal);
    const float worldScale = rsqrt(dot(worldNormal, worldNormal));
    worldNormal *= worldScale;

    const float3 extent3 = abs(edge1) + abs(edge2) + abs(edge1 - edge2);
    const float extent = max(max(extent3.x, extent3.y), extent3.z);
    // reconstruction and intersection, object space
    float3 objectError = mad(kSpawnObjectError, abs(v0), kSpawnExtentError * extent);
    // object to world
    const float3 translation = float3(o2w[0][3], o2w[1][3], o2w[2][3]);
    const float3 worldError = mad(kSpawnExtentError, mul(abs((float3x3)o2w), abs(objectPos)),
                                  kSpawnTransformError * abs(translation));
    // world to object
    objectError = mad(kSpawnTransformError, mul(abs(hit.worldToObject), float4(abs(worldPos), 1.0)), objectError);

    const float offset =
        mad(worldScale, dot(objectError, abs(objectNormal)), dot(worldError, abs(worldNormal)));
    const float side = dot(worldNormal, direction) < 0.0 ? -1.0 : 1.0;
    precise const float3 spawn = mad(offset * side, worldNormal, worldPos);
    return spawn;
}
