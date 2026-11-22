# M8 · I10: Mega Geometry perf

**Status:** planned 2026-11-22 · **Milestone:** M8 ([README](./README.md))
**Implements:** MG-S1–MG-S3 (skipped before I10, found in the 2026-11-22 re-check against NVIDIA's samples), MG-P1–MG-P7 ([mg_optimizations_fixes.md](./mg_optimizations_fixes.md) §3), I4's `rayTimeline` latency finding ([i4_frame_profile.md](./i4_frame_profile.md) §3)
**Depends on:** I3 ([i3_mega_geometry_fixes.md](./i3_mega_geometry_fixes.md)), I4 ([i4_frame_profile.md](./i4_frame_profile.md))
**References:**
- `reference/RTXMG-main`: `rtxmg/cluster_lod/{preloaded.cpp,blas_pass.cpp,streaming.cpp,baking/baker.cpp}`, `demo/rtxmg_renderer.cpp` (TLAS)
- `reference/vk_lod_clusters`: `src/scene_preloaded.cpp`, `src/scene_partitions.cpp`, `src/renderer_raytrace_clusters_lod.cpp`, `docs/partitions_and_ptlas.md`
- `reference/vk_partitioned_tlas-main`: `src/partitioned_tlas_acceleration_structure.cpp`
- `vk_animated_clusters` README (triangle BLAS against CLAS on static content), via [reference/mega_geometry_comparison.md](../../reference/mega_geometry_comparison.md) §3.3
- [reference/mega_geometry_comparison.md](../../reference/mega_geometry_comparison.md) (F1–F8, E1–E5), `spec/considerations.md` (`C-D2-1`, `C-E1-3`)

## Scope

The I3 bugs are fixed and still in the code: MG-B1's aligned allocations at `vk_mega_geometry.cpp:175` and `vk_rt_scene.cpp:541-554`; MG-B2's host-cached readbacks; MG-B3's clamped growth; MG-B4's bounds growth and partition reassignment.

A second pass over NVIDIA's CLAS → BLAS → TLAS pipelines found no build-path differences outside the list below:

| Stage | What NVIDIA's samples do | Engine |
|---|---|---|
| CLAS build | sizes pass, then explicit destinations; `FAST_TRACE`; limits taken from the data | same |
| CLAS geometry flags | per-triangle flags for mixed clusters | per-primitive BLAS plus instance flags; F7: no change |
| BLAS | `PREFER_FAST_TRACE` | same |
| TLAS / PTLAS | `PREFER_FAST_TRACE`; global partition unused on static scenes | same |
| `WRITE_INSTANCE` | used for transform changes | same |

What was never done falls into two groups:

- three items that I3 left out (its scope was §2 bugs only) and that never got an MG-P number (MG-S1–MG-S3);
- the seven MG-P levers, plus one I4 latency finding.

The skipped items come first because two of them can change how the MG-P levers are measured.

## Skipped before I10 (on top)

### MG-S1 · Triangle BLAS against CLAS on static content (F2 / E4)

- **What NVIDIA says:** the `vk_animated_clusters` README says triangle BLAS "can provide better ray intersection performance" than CLAS, especially for static content. It measured 1.45 → 1.52 ms render with cluster BLAS.
- **Why it matters here:** every engine model is static (no LOD, nothing deforms). The comparison doc (§3.3) planned this measurement "before 1.0". Its §8 then filed it as E4, a post-1.0 toggle (engine spec §16.5 V2), and the MG findings never listed it. So the one NVIDIA recommendation that targets this exact case was never measured.
- **Experiment:** a temporary measurement build, not landed.
  - `pbr_implementation_plan.md:962` (the "Never" table) forbids KHR triangle BLAS in 1.0, and LOD (§16.3) needs CLAS.
  - For Sponza and Bistro, build compacted `PREFER_FAST_TRACE | ALLOW_DATA_ACCESS` triangle BLAS, one per primitive, from the meshlet-ordered index list (`meshletTriangles` widened to 32-bit indices over `meshletVertices`). This keeps `PrimitiveIndex` in raster order.
  - Map each hit back with a per-primitive table, one 16-bit meshlet row per triangle, in place of `CommittedClusterID()`. That is one extra load per hit, and it is counted in the result.
  - Keep OMM bakes per primitive, with the same indices.
  - Check `ENGINE_RT_CHECK` bit-identical before timing.
- **Measure:** ABBA, `ReSTIR DI initial`, `Indirect`, `ReSTIR GI shade`, BLAS + CLAS bytes, on Sponza gallery, Sponza courtyard and Bistro.
- **Exit:**
  - If triangle BLAS traces > 5 % faster on two of the three scenes, record the gain. The user then decides whether V2 moves into 1.0, since it changes a 1.0 rule.
  - Otherwise, close F2 with the numbers.

### MG-S2 · Per-primitive BLAS recheck at Bistro (comparison §3.2)

- **The deferred recheck:** comparison §3.2 kept one BLAS per mesh primitive, with "recheck at Bistro (M9)". Bistro now loads with 2,909 instances (I4 F8), but no recheck was run. I3's benches were Sponza (103 instances) plus the bunny field.
- **Measure first, no code:** PTLAS build, `RT instances` and the three trace scopes on Bistro (`ENGINE_BENCH_MODEL`). Also log the instances-per-mesh histogram.
- **Only if the PTLAS build exceeds 0.1 ms still, or the trace per instance is worse than on Sponza:** try one BLAS per mesh.
  - All of the mesh's CLAS go into one cluster BLAS.
  - The material comes from a per-cluster primitive row instead of the instance row, so the hit decode changes in `hit.slang`.
- **Exit:** the numbers are recorded, and the merge is either kept with an ABBA gain or closed.

### MG-S3 · I3's verification set misses Bistro and the café runners

- **The gap:** I3 measured and checked Sponza and the moving field only. The Bistro café runners (`ENGINE_CHECK_SCENE=bistro_cafe`, F22 / F23) did not exist yet.
- **For I10:**
  - every MG-P lever gets ABBA benches on Bistro as well as on Sponza and field 64 (`ENGINE_BENCH_FIELD=64 ENGINE_BENCH_FIELD_MOVE=1`);
  - the café temporal runners join the regression set for the levers that change positions or traversal (MG-P2, MG-P3, MG-S1).
- **One more check:** partition reassignment (I3) is zero-cost on a static scene through transform equality. Confirm it on Bistro: no PTLAS writes on a still frame (`PtlasArgCountReadback` stays 0).

## MG-P levers (I4's ranked order)

Line numbers are refreshed to the current tree.

| Order | Lever | Location now | Note from the re-check |
|---|---|---|---|
| 1 | **MG-P2** 7-bit position truncation in CLAS and raster | `vk_mega_geometry.cpp:222-229` (`minPositionTruncateBitCount = 0`), `geometry_store.cpp` | RTXMG `baker.cpp` `QuantizeFloat`, vk_lod_clusters `compressionPosDropBits = 7`. The self-hit test at 7 bits was never run (comparison §2.2), so it is part of this lever |
| 2 | **MG-P4** ray-hit decode without dead loads | `hit.slang:120-125`, `surface.slang:28-50, 129` | unchanged |
| 3 | **MG-P3** spatial meshlets | `geometry_store.cpp:314` (`meshopt_buildMeshletsFlex`) | vk_lod_clusters defaults to RT clustering (`clodDefaultConfigRT`). A/B the raster against the trace |
| 4 | **MG-P1** opaque / OMM CLAS split | `vk_mega_geometry.cpp:314-322` | instance flags are already per primitive (`rt_instances.slang:94`), so only the CLAS flag spreads across a model |
| 5 | **MG-P5** PTLAS layout (empty cells dropped, Z-order, refinement) and explicit AABB | `vk_rt_scene.cpp:270-289`, `rt_instances.slang:88` | the refined grid changes `templates.gridAxis`, which reassignment (I3) reads: re-run I3's field ABBA with both together |
| 6 | **MG-P7** `rt_instances` overlapping light preparation, plus I4's `rayTimeline` signal | `vk_renderer.cpp:669-670`, `vk_rt_scene.cpp:579-589` (`reuseBarrier`) | I4 §3 adds two items. (1) Signal `rayTimeline` before `DLSS-RR` (1.21 ms) when NRC is paused (I4 §3 row 3; today it is signalled at the end of the ray submit, `vk_renderer.cpp:375-377`). (2) Submit the ray work before `AcquireImage` / present recording |
| 7 | **MG-P6** load-time scratch sizing | `vk_mega_geometry.cpp:334-344` | the loop still folds `eImplicitDestinations` into `scratchBytes`; the GPU-only pipeline stays in M9 |

Details per lever: [mg_optimizations_fixes.md](./mg_optimizations_fixes.md) §3.

## Open decision (user)

The GPU-AV gap: MG findings §5 asks for GPU-AV on shader changes. MG-P2, MG-P4, MG-P5 and MG-S1 change shaders. GPU-AV cannot run since M7 E1 (`C-E1-3`). I3 had no shader changes, so this did not bite there.

Routes:

- **(a)** the planned fix in [features/gpuav_execution_sets.md](../features/gpuav_execution_sets.md): a patched local layer that keeps the shipped execution set. Run it before these levers;
- **(b)** fall back to the GPU-AV build switch from `C-E1-3`'s exit (per-bin `vkCmdDispatchIndirect`, never the shipped path);
- **(c)** accept core + sync + best-practices validation, plus the robust-read rule of the RTXDI bridges, for these levers.

## Verification

Per lever, as MG findings §5 sets out, with I10's additions:

- validation: core, sync, best practices and NVIDIA best practices clean. GPU-AV follows the open decision above;
- `ctest`, `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST`, `ENGINE_GUIDE_CAPTURE`, `ENGINE_GI_CHECK`;
- the café runners for MG-P2, MG-P3 and MG-S1 (MG-S3);
- ABBA GPU timings on a release build with every layer off (`VK_LOADER_LAYERS_DISABLE='*validation*' VK_KHRONOS_VALIDATION_GPUAV_ENABLE=false`), ±5 % noise, on Sponza, field 64 moving, and Bistro;
- run in a separate build tree when another session runs checks.

## Done when

- [ ] MG-S1 measured: triangle BLAS against CLAS trace and memory on three scenes; F2 closed with numbers or sent to the user as a 1.0 change
- [ ] MG-S2 Bistro numbers recorded; per-mesh BLAS merged with an ABBA gain or closed
- [ ] MG-S3 Bistro and café runners in the verification set; still-frame PTLAS writes confirmed 0 on Bistro
- [ ] MG-P2, MG-P4, MG-P3, MG-P1, MG-P5, MG-P7 (with I4's two latency items), MG-P6 each landed with an ABBA gain, or closed with numbers under MG findings §5
- [ ] the GPU-AV decision recorded
