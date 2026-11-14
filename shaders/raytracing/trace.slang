module trace;

// visibility rays (PBR plan D4, spec/features/ray_tracing_mega_geometry.md §6.2): first hit ends the search, one
// Proceed() and no candidate loop. No FORCE_OPAQUE ray flag, which would override the instances' OMM state; the
// instance flags already make non-alpha geometry opaque. No facing cull: a back face blocks light

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

// true when nothing in [tMin, tMax] blocks the segment
public bool traceVisibility(uint64_t tlas, float3 origin, float3 direction, float tMin, float tMax)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = tMin;
    ray.TMax = tMax;
    requireOmm2StateRayFlag();
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
             RAY_FLAG_FORCE_OMM_2_STATE> query;
    query.TraceRayInline(RaytracingAccelerationStructure(tlas), RAY_FLAG_NONE, 0xFF, ray);
    query.Proceed();
    return query.CommittedStatus() == COMMITTED_NOTHING;
}
