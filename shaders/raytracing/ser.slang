// SHELVED with SER (considerations C-G1-3): kept in the tree, not built; the frame traces shaded rays with traceClosest
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

// closest hit through the SER pipeline: the same hit traceClosest reports. Takes the TLAS address, not the frame root:
// whatever the caller loaded before and uses after moves with the thread across the reorder, so callers reload frame
// data after it. An inactive lane (no surface to shade from) joins the reorder with a NOP hit object, so the reorder
// never sits in divergent control flow, and reports a miss
public bool traceShaded(uint64_t tlas, RayDesc ray, out RayHit hit, bool isActive = true)
{
    hit = {};
    ClusterPayload payload = {};
    requireOmm2StateRayFlag();
    HitObject hitObject = HitObject::MakeNop();
    if (isActive)
    {
        // same flags as traceClosest: no facing cull, OMM 2-state from the ray (PTLAS has no instance flag for it)
        hitObject = HitObject::TraceRay(RaytracingAccelerationStructure(tlas),
                                        RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OMM_2_STATE, 0xFF, 0, 0, 0,
                                        ray, payload);
    }
    const bool isHit = hitObject.IsHit();
    // no hint before M7 material bins: the sort already separates hit, miss and NOP by shader (NVIDIA
    // vk_gltf_renderer, vk_mini_samples ser_pathtrace)
    if (kIsSerReorderEnabled)
    {
        ReorderThread(hitObject);
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
