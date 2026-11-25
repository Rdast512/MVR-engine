# M8 · I10: Mega Geometry perf

**Status:** built 2026-11-22 (2026-11-23 user decision: 1.0 stays on CLAS; MG-S1 / V2 and deferred levers tracked for post-1.0 animations, streaming, and large tree worlds) · **Milestone:** M8 ([README](./README.md))
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

- **(a)** ~~a patched local layer ([features/planned/gpuav_execution_sets.md](../features/planned/gpuav_execution_sets.md))~~: dropped 2026-11-22 (user);
- **(b)** **chosen 2026-11-22:** the temporary GPU-AV switch `ENGINE_GPUAV_RESOLVE_PER_BIN=1` (per-bin `vkCmdDispatchIndirect`, never the shipped path; considerations `C-E1-3`). The levers run GPU-AV with it;
- **(c)** accept core + sync + best-practices validation, plus the robust-read rule of the RTXDI bridges, for these levers.

## Verification

Per lever, as MG findings §5 sets out, with I10's additions:

- validation: core, sync, best practices and NVIDIA best practices clean. GPU-AV follows the open decision above;
- `ctest`, `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST`, `ENGINE_GUIDE_CAPTURE`, `ENGINE_GI_CHECK`;
- the café runners for MG-P2, MG-P3 and MG-S1 (MG-S3);
- ABBA GPU timings on a release build with every layer off (`VK_LOADER_LAYERS_DISABLE='*validation*' VK_KHRONOS_VALIDATION_GPUAV_ENABLE=false`), ±5 % noise, on Sponza, field 64 moving, and Bistro;
- run in a separate build tree when another session runs checks.

## Results (2026-11-22, RTX 5070, driver 621.01, 1280×720 / 853×480 render, Release, no layers)

All ABBA runs use interleaved `A1 / B1 / B2 / A2` at 1,200 frames (`Norm%` normalizes each scope against `DLSS-RR` to remove 720p boost-clock drift; MG-P4 shading was also confirmed at 2560×1369).

### Skipped items (MG-S1 – MG-S3)

- **MG-S2 (Per-primitive BLAS recheck at Bistro) — closed (no code change):**
  - `models/bistro/bistro.gltf` has `551` unique meshes, `551` unique primitives, `2,909` mesh nodes, `2,909` primitive instances — histogram `[(1, 2909)]` (`100%` single-primitive meshes; per-mesh BLAS is structurally identical to per-primitive BLAS on Bistro).
  - Baseline Bistro still-frame async compute: `AsyncPtlas = 0.0658 ms` (`< 0.1 ms` gate), `AsyncRtInst = 0.0009 ms`, `RaySum = 0.8648 ms` (faster than Sponza `1.39 / 1.67 ms`).
- **MG-S3 (Bistro & café verification + still-frame PTLAS writes) — done:**
  - Confirmed `Written = 0` (`PtlasArgCountReadback == 0`) on Bistro still frames.
  - Bistro added to every MG-P ABBA; Bistro café temporal runners (`ENGINE_CHECK_SCENE=bistro_cafe`) pass (`ENGINE_STABILITY_CHECK=1` exit 0, `ENGINE_SCREEN_EDGE_CHECK=1` 80 steps / 0 failed).
- **MG-S1 (Compacted KHR Triangle BLAS vs CLAS on static content, F2 / E4) — measured, temporary build reverted, V2 decision open for user:**
  - Temporary build: compacted `ePreferFastTrace | eAllowDataAccess | eAllowCompaction` KHR triangle BLAS per primitive, OMMs via `VkAccelerationStructureTrianglesOpacityMicromapEXT` with exact triangle usage histogram, 32-bit meshlet-ordered indices, 16-bit `triToMeshlet` lookup table. `ENGINE_RT_CHECK=1` passed 32/32 (`0` misses, `0` self-hits, `0.0000 deg` decode error).
  - **Memory:**
    - Sponza: Compacted Triangle BLAS `5.68 MiB` (uncompacted `16.35 MiB`) + `triMap 0.50 MiB` + OMM `1.11 MiB` = **`7.30 MiB`** vs pre-I10 Flex CLAS+CBLAS+OMM `6.72 MiB` and post-I10 spatial CLAS+CBLAS+OMM **`7.61 MiB`** (`−0.31 MiB` smaller than post-I10 CLAS).
    - Bistro: Compacted Triangle BLAS `41.29 MiB` (uncompacted `107.83 MiB`) + `triMap 3.33 MiB` + OMM `1.32 MiB` = **`45.94 MiB`** vs pre-I10 Flex CLAS+CBLAS+OMM `44.75 MiB` and post-I10 spatial CLAS+CBLAS+OMM **`49.98 MiB`** (`−4.04 MiB` smaller than post-I10 CLAS).
  - **ABBA trace (`RaySum = DI initial + Indirect + GI shade`, 1,200f):**
    - Against **pre-I10 Flex CLAS**: `gallery −28.7%` raw (`~−20% norm`), `courtyard −18.7%` (`DLSS-RR +1.2%`, `GPU total −10.6%`), `bistro −13.3%` (`DLSS-RR −1.5%`, `GPU total −4.4%`).
    - Against **post-I10 optimized CLAS** (`snap_final`, even though `snap_mgs1` lacked MG-P4/P5/P7):
      - `gallery`: `RaySum 1.1190 → 0.9749 ms` (**`−12.9%` raw / `−12.4% norm`**), `GPU total 3.0695 → 2.9040 ms` (`−5.4%` raw / `−4.9% norm`, `DLSS-RR −0.6%`).
      - `courtyard`: `RaySum 1.3967 → 1.1842 ms` (**`−15.2%` raw / `−15.1% norm`**), `GPU total 3.4212 → 3.1738 ms` (`−7.2%` raw / `−7.1% norm`, `DLSS-RR −0.1%`).
      - `bistro`: `RaySum 0.7447 → 0.6360 ms` (**`−14.6%` raw / `−5.2% norm`**).
  - **Decision (2026-11-23, user):** Compacted triangle BLAS exceeded the `> 5%` trace gate on all 3 scenes even against post-I10 optimized CLAS (`−12.4% / −15.1% / −5.2% norm`, `−12.9% / −15.2% / −14.6%` raw) and used less VRAM (`−0.31 MiB` Sponza, `−4.04 MiB` Bistro), **but 1.0 stays on CLAS** because static content is temporary — animations, streaming, and a large world of trees land after 1.0 where CLAS, cluster templates, and cluster LOD are required. V2 remains tracked in `docs/engine_spec.md` §16.5 for static-only geometry post-1.0.

### Deferred / reverted levers useful post-1.0 (animations, streaming, large tree worlds)

Full details are tracked in [mg_optimizations_fixes.md](./mg_optimizations_fixes.md) §4 and `docs/engine_spec.md` §16.3 / §16.5:

- **MG-P2 (7-bit CLAS position truncation):** Reverted in 1.0 (trace `+4.3–5.9%` on RTX 5070), verified `0` self-hits and `−18–20%` CLAS VRAM. Useful post-1.0 for **streaming / large tree worlds / 8 GB floor tier** (or coarse/distant LOD levels only) when the CLAS residency pool becomes VRAM-bound.
- **MG-P5 explicit instance AABBs (`ENABLE_EXPLICIT_BOUNDING_BOX_NV`):** Reverted in 1.0 (Arvo boxes loosened rotated Bistro primitives, `RaySum +3.3%` norm). Useful post-1.0 when **thousands of animated/wind-swaying or LOD-swapping tree instances** update per frame, using tighter or selective bounds so PTLAS build skips BLAS bound reads without bloating rotated static geometry.
- **MG-P6 GPU-only build pipeline (`eImplicitDestinations` → GPU allocator → `eMoveObjects` `NO_OVERLAP`):** Deferred from 1.0 (3× synchronous `queue.waitIdle()`). Required for **post-1.0 streaming** to build/compact streamed world chunks asynchronously with zero CPU readbacks and zero frame hitches.
- **MG-S2 per-mesh Cluster BLAS merge (plus BLAS sharing / caching / merging):** Closed in 1.0 because Bistro has 100% single-primitive meshes. Useful post-1.0 for **large tree worlds** with multi-primitive trees (trunk + branches + leaf cards) instanced heavily — cuts PTLAS instance count 2–4× alongside L2 BLAS sharing/caching.
- **Cluster templates (`INSTANTIATE_TRIANGLE_CLUSTER`) & position stripping:** Cluster templates avoid per-frame topology rebuilds for **animations and wind-deformed foliage**; position stripping drops `float3` positions from the resident RT attribute pool (~12 B/vertex saved for **streaming**) once raster/RT streams decouple (MG-P4 already removed all ray-hit vertex position loads).
- **MG-S1 / V2 (compacted KHR triangle BLAS):** Kept post-1.0 (`docs/engine_spec.md` §16.5) as an optional faster path for permanently static, single-level geometry (terrain/architecture) alongside CLAS-based animated/LOD/tree assets.

### Per-lever ABBA summary (MG-P1 – MG-P7)

| Lever | Status | Key measured numbers (1,200f ABBA unless noted) |
|---|---|---|
| **MG-P2** 7-bit position truncation | **reverted** | `ENGINE_RT_CHECK` 32/32 (`0` self-hits across 7.79M interior px, `0.0000 deg` error); CLAS VRAM `−19.6%` Sponza (`5.36 → 4.31 MiB`), `−18.1%` Bistro (`41.71 → 34.18 MiB`); regressed `RaySum` on 3/4 scenes (`+5.9%` gallery, `+4.3%` field 64, `+4.4%` Bistro; `GI shade +7.2% / +8.9% / +14.1%`) |
| **MG-P4** ray-hit decode without dead loads | **kept** | Bit-identical `ENGINE_RT_CHECK` (`0.0000 deg` error); skips 36 B vertex pos load, 136 B `GpuNodeInstance` load, `meshletPrimitives` load, and `normalMatrix` cofactors; 2560×1369 ABBA `Indirect` norm `−1.1%` gallery, `−0.6%` courtyard, `−4.5%` Bistro (`−0.107 ms / −5.8%` raw) |
| **MG-P3** spatial meshlets | **kept (`min=42, fill=0.75`)** | `min=31, fill=0.5` inflated Bistro clusters +53% (`GPU total +5.5%`); tuned `min=42, fill=0.75` cut `RaySum` norm `−10.9%` gallery (`GPU total −3.9%`), `−6.6%` courtyard (`−3.2%`), `−12.8%` Bistro (`−0.4%`) |
| **MG-P1** opaque / OMM CLAS split | **kept** | Sponza CLAS `6.56 → 6.02 MiB` (`−8.2%`, host est. `−28.7%`); Bistro CLAS `50.88 → 46.16 MiB` (`−9.3%`, `−4.72 MiB`); packed in original meshlet row order; neutral GPU total (`0.0%` gallery/courtyard/Bistro, `−1.4%` field 64) |
| **MG-P5** PTLAS layout + explicit AABBs | **layout kept, explicit AABBs reverted** | Derived power-of-two grid (`kDerivedMaxInstances = 1024`), empty cells dropped, 2D Morton Z-order: Field 64 `Partitions 81 → 64`, `PtlasKiB 1,405 → 1,385`, `AsyncPtlas −9.0%`, `Ray start wait −36.3%`, `DI initial −27.9%`, `GPU total −2.3%` raw (`−1.0% norm`), neutral Sponza & Bistro. Explicit Arvo AABBs loosened rotated Bistro primitives (`RaySum +3.3% norm`, `Indirect +5.1%`) and were reverted (`GpuRayInstanceBuild` stays 16 B) |
| **MG-P7** async compute overlap + I4 §3 latency | **kept (all 3 parts)** | (1) `rtScene.recordBuild` before `rtxdi.recordLightPreparation` with `bindHeaps(cmd)` after `buildPartitionedAccelerationStructuresNV`; (2) pre-DLSS pure-Vulkan ray submit before `AcquireImage` (DLSS-RR + post stay with `presentCmd` in one post-acquire submit for C-G3-6; since 2026-11-25 the split is the first SDK segment, so NRC frames submit their pure-Vulkan `DirectLight` segment early too, with the post-acquire part waiting on the TLAS again); (3) pre-DLSS `rayTimeline` signal when NRC is paused and `rayCheck` is off. Field 64 `Ray start wait 0.0355 → 0.0071 ms` (`−80%`), `RasterSum −15.1%`, `GPU total −1.1%` raw, `CpuDrawFrame −3.1%`; Bistro `RasterSum −40.8%`, `Ray start wait −47.8%`, `OverlapOffset +0.08 → −1.16 ms` |
| **MG-P6** load-time scratch sizing | **kept** | `eImplicitDestinations` dropped from `scratchBytes` (queried only for `clasEstimate` log); driver 621.01 reports identical scratch for explicit and implicit (`4.67 MiB` Sponza, `14.04 MiB` Bistro) |

### End-to-end I10 ABBA (`snap_base` vs `snap_final`, 1,200 frames)

| Scene | `Ray start wait` | `RasterSum (4)` | `RaySum (3)` | `GPU total` (raw / norm) | `CpuDrawFrame` |
|---|---|---|---|---|---|
| Sponza gallery | `0.0413 → 0.0092 ms` (`−77.7%`) | `0.0368 → 0.0397 ms` | `1.1671 → 1.1248 ms` (`−3.6%`) | **`3.1149 → 2.9812 ms`** (`−4.3%` / `+0.4%`) | `−5.5%` |
| Sponza courtyard | `0.0441 → 0.0110 ms` (`−75.1%`) | `0.0478 → 0.0456 ms` (`−4.6%`) | `1.5087 → 1.4122 ms` (`−6.4%` / `−9.3%`) | **`3.5974 → 3.5137 ms`** (`−2.3%` / `−5.3%`) | `−3.4%` |
| Field 64 moving | `0.1833 → 0.0078 ms` (`−95.7%`) | `0.0538 → 0.0447 ms` (`−16.8%`) | `1.0671 → 1.1251 ms` | **`3.2712 → 3.0898 ms`** (`−5.5%` / `−0.3%`) | `−4.3%` |
| Bistro | `0.0516 → 0.0102 ms` (`−80.3%`) | `0.0615 → 0.0277 ms` (`−54.9%`) | `0.7764 → 0.7640 ms` (`−1.6%` / `−7.1%`) | **`2.6536 → 2.5866 ms`** (`−2.5%` / `−8.0%`) | `−3.4%` |

### Final verification

- `ctest`: **8/8 passed**
- `ENGINE_RT_CHECK=1`: **32/32 passed** (`0` misses, `0` id mismatches, `0` self-hits, `0.0000 deg` decode error)
- `ENGINE_CULL_CHECK=1`: **16/16 passed** (`0 px` differ)
- `ENGINE_MOTION_TEST=1`: **13/13 passed**
- `ENGINE_GUIDE_CAPTURE=1`: **2/2 normal checks + 3/3 DI convergence checks passed**
- `ENGINE_GI_CHECK=1`: **14/14 gated phases passed**
- Bistro café temporal runners (`ENGINE_CHECK_SCENE=bistro_cafe`): `ENGINE_STABILITY_CHECK=1` exit 0, `ENGINE_SCREEN_EDGE_CHECK=1` 80 steps / 0 failed
- Vulkan validation (core + sync + best practices + NVIDIA best practices, `ENGINE_INDIRECT_MODE=cycle`): **0 engine errors/warnings**
- GPU-AV (`ENGINE_GPUAV_RESOLVE_PER_BIN=1`, `ENGINE_RT_CHECK=1`): **0 GPU-AV errors**, 32/32 matched

## Done when

- [x] MG-S1 measured: triangle BLAS against CLAS trace and memory on three scenes; 2026-11-23 user decision: 1.0 stays on CLAS, V2 stays post-1.0 with I10 numbers (`−12.4% / −15.1% / −5.2%` norm `RaySum`, smaller VRAM)
- [x] MG-S2 Bistro numbers recorded; per-mesh BLAS closed (`100%` single-primitive meshes, `AsyncPtlas = 0.0658 ms < 0.1 ms`)
- [x] MG-S3 Bistro and café runners in the verification set; still-frame PTLAS writes confirmed 0 on Bistro
- [x] MG-P2, MG-P4, MG-P3, MG-P1, MG-P5, MG-P7 (with I4's two latency items), MG-P6 each landed with an ABBA gain, or closed with numbers under MG findings §5
- [x] the GPU-AV decision recorded (`ENGINE_GPUAV_RESOLVE_PER_BIN=1`, route (b), verified clean)
