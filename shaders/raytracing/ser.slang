module ser;

import types;
import geometry.hit;
import raytracing.trace;

// shaded rays (spec/m4/g1_ser_rt_pipeline.md §3): HitObject trace -> ReorderThread -> minimal closest-hit -> the
// caller shades in raygen. Each pass declares its raygen, closesthit and miss entries and calls this module

// what the EXT hit object cannot report: HitObject::GetClusterID is NV-only (SER4, Slang 2026.22)
public struct ClusterPayload
{
    public uint clusterId;
};

// no material bins before M7 and no alpha-tested primitives before M6: hits and misses are the only coherence keys
public static const uint kSerHintHit = 0;
public static const uint kSerHintMiss = 1;
public static const uint kSerHintBits = 1;

// one pipeline per mode (RtPipeline::Source::isReorderEnabled): a runtime branch around ReorderThread costs 0.18 ms
// with reordering off, against 0.69 ms with it compiled out (G1 check pass, 1280x720, RTX 5070)
public [vk::constant_id(0)] const bool kIsSerReorderEnabled = true;

// closest-hit body. Slang 2026.22 has no ClusterID() intrinsic for SPIR-V hit shaders, so the builtin is read
// directly (considerations C-G1-1)
public void writeClusterPayload(inout ClusterPayload payload)
{
    payload.clusterId = spirv_asm
    {
        OpExtension "SPV_NV_cluster_acceleration_structure";
        OpCapability RayTracingClusterAccelerationStructureNV;
        result:$$uint = OpLoad builtin(ClusterIDNV:uint);
    };
}

// closest hit through the SER pipeline: the same hit traceClosest reports. Live across the reorder: the ray and the
// caller's own state only
public bool traceShaded(GpuFrameRoot root, RayDesc ray, out RayHit hit)
{
    hit = {};
    ClusterPayload payload = {};
    requireOmm2StateRayFlag();
    // same flags as traceClosest: no facing cull, OMM 2-state from the ray (PTLAS has no instance flag for it)
    HitObject hitObject = HitObject::TraceRay(RaytracingAccelerationStructure(root.tlas),
                                              RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OMM_2_STATE, 0xFF,
                                              0, 0, 0, ray, payload);
    const bool isHit = hitObject.IsHit();
    if (kIsSerReorderEnabled)
    {
        ReorderThread(hitObject, isHit ? kSerHintHit : kSerHintMiss, kSerHintBits);
    }
    if (!isHit)
    {
        return false;
    }
    HitObject::Invoke(hitObject, payload);
    hit.rayInstance = hitObject.GetInstanceIndex();
    hit.meshletRow = payload.clusterId;
    hit.triangle = hitObject.GetPrimitiveIndex();
    hit.barycentrics = hitObject.GetAttributes<BuiltInTriangleIntersectionAttributes>().barycentrics;
    // TMax of a hit object is its hit distance (OpHitObjectGetRayTMaxEXT); GetRayTCurrent is HLSL-only
    hit.t = hitObject.GetRayDesc().TMax;
    hit.objectPos = hitObject.GetTriangleVertexPositions();
    // the hit object reports 4x3 like ObjectToWorld4x3(); RayHit holds the 3x4 the ray query reports
    hit.objectToWorld = transpose(hitObject.GetObjectToWorld());
    hit.worldToObject = transpose(hitObject.GetWorldToObject());
    return true;
}
