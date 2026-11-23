module trace;

import types;
import core.binding;
import core.counters;

// the ray contract (PBR plan D4, spec/m6/d4_ray_contract.md, spec/features/planned/ray_tracing_mega_geometry.md §6.2): every
// query takes one Proceed() and no candidate loop. Instances are FORCE_OPAQUE or carry 2-state micromaps, so no
// candidate can reach a query; no FORCE_OPAQUE ray flag, which would override the OMM state. No facing cull: a back
// face blocks light. traceVisibility and geometry/hit.slang traceClosest are the only TraceRayInline calls

// set by the host in Debug builds (kDebugRaysConstantId in src/render/vk_shader_objects.cpp): each query that still
// runs after its Proceed() is counted, and the host logs any. Release specializes the branch away
public [vk::constant_id(1)] const bool kDebugRays = false;

// PTLAS instances have no OMM 2-state flag, so every ray carries RAY_FLAG_FORCE_OMM_2_STATE (feature spec §8).
// Slang 2026.20 emits the flag without its capability; spirv-val rejects the module until it is declared
public void requireOmm2StateRayFlag()
{
    spirv_asm
    {
        OpExtension "SPV_EXT_opacity_micromap";
        OpCapability RayTracingOpacityMicromapEXT
    };
}

// the frame's TLAS and the counters its traces report to
public struct RayScene
{
    public uint64_t tlas;
    public uint64_t rayDebug; // GpuRayDebugCounters, 0 unless kDebugRays
};

public RayScene loadRayScene(GpuFrameRoot root)
{
    RayScene scene;
    scene.tlas = root.tlas;
    scene.rayDebug = root.rayDebug;
    return scene;
}

// isProceeding: the query's one Proceed() returned true, so it met a candidate
public void countCandidate(uint64_t rayDebug, bool isProceeding)
{
    if (kDebugRays && rayDebug != 0)
    {
        GpuRayDebugCounters* counters = bdaPtr<GpuRayDebugCounters>(rayDebug);
        waveCount(counters.candidates, isProceeding);
    }
}

// true when nothing opaque in [tMin, tMax] blocks the segment; the first hit ends the search. Glass never blocks
// (m8 I13): final shading multiplies its transmittance instead
public bool traceVisibility(RayScene scene, float3 origin, float3 direction, float tMin, float tMax)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = tMin;
    ray.TMax = tMax;
    requireOmm2StateRayFlag();
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
             RAY_FLAG_FORCE_OMM_2_STATE> query;
    query.TraceRayInline(RaytracingAccelerationStructure(scene.tlas), RAY_FLAG_NONE, kRayMaskOpaque, ray);
    countCandidate(scene.rayDebug, query.Proceed());
    return query.CommittedStatus() == COMMITTED_NOTHING;
}
