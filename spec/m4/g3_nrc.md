# M4 · G3: Neural Radiance Cache

**Status:** not started; linkage test only (`tests/nrc_library_test.cpp`) · **Milestone:** M4
**Implements:** engine spec §11.4; plan G3 ([pbr_implementation_plan.md](../pbr_implementation_plan.md))
**Depends on:** [G1](./g1_ser_rt_pipeline.md) (update paths are shaded rays), [G2](./g2_indirect_pass.md) (query pass)
**Used by:** [G5](./g5_restir_gi.md)
**Considerations:** C-G3-1 (SHaRC code still in the tree), C-G3-2 (NRC on the 2060 Super)

SDK: NVIDIA RTX Neural Radiance Cache (Vulkan host library + HLSL headers), pinned; prebuilt DLLs (NRC + CUDA runtime) copied next to the executable. Verify every SDK name against the pinned headers.

## Scope (from the plan)

- **Device:** enable `VK_NV_cooperative_vector` and merge NRC's required extensions and features, next to H1's NGX set. They are required: device creation throws when one is missing, with no fallback.
- **Context:** `nrc::vulkan::Context` after device init; `Configure` (render resolution, `kMaxBounces`, training budget) at init and on resize. The SDK allocates its buffers; the engine gives each a heap slot or BDA and never resizes them.
- **Per frame:** `BeginFrame` (scene AABB from plan §9, training on) → `NrcConstants` into the per-slot block → update paths (`NRC_UPDATE`) + G2 as the query pass (`NRC_QUERY`) → `QueryAndTrain` (inference, then one training step) → **custom resolve** writing `throughput × cachedRadiance` into the ReSTIR GI samples (NRC guide step 7), not the SDK's combined-signal resolve → `EndFrame` after submit.
- **Update paths** are shaded rays: they run in the G1 SER pipeline, reordering at every bounce (spec §11.1: the frame's most incoherent rays).
- **Debug view:** the cache's radiance at the primary surface (spec §14, "cache debug resolve").
- **`gi/nrc_bridge.slang`** matches the SDK's HLSL buffer packing with an explicit Slang layout, never by editing vendor headers.
- Constants per frame slot through `GpuFrameRoot.cacheConstants`.
- `nrc_library` becomes the `vk_nrc` module (engine spec AU9); SHaRC code and `GI_CACHE_MODE` are removed (C-G3-1).

## Done when

Both tiers run NRC; training loss falls after load; the cache debug view shows cached radiance; an interior lit only by a sun is not black; the cache row of spec §2.3 is measured on the 2060 Super (spec R4).

## Log

| Date | Change |
|---|---|
| 2026-11-16 | created from plan G3 |
