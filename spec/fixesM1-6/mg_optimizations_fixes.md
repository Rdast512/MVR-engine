# Feature: RTX Mega Geometry — bugs, spec issues, and performance improvements

**Date:** 2026-11-21 · **Status:** §2 bugs (MG-B1–MG-B4 + C-C4-5) fixed in M8 I3 ([i3_mega_geometry_fixes.md](./i3_mega_geometry_fixes.md), 2026-11-21); §3 perf levers wait for M8 I10 ([i10_mega_geometry_perf.md](./i10_mega_geometry_perf.md), which also adds MG-S1–MG-S3, the items skipped before it) · **Milestone:** [M8](./README.md) (follows [M2](../m2/) C1–C4, [M6](../m6/) D1–D4, and [mega_geometry_comparison.md](../../reference/mega_geometry_comparison.md))
**Why:** Audit of the engine's RTX Mega Geometry stack (CLAS, Cluster BLAS, OMM integration, PTLAS, and ray-hit decode) against Vulkan 1.4.363 VUIDs and NVIDIA's local `RTXMG` and `vk_lod_clusters` references.

**Sources**

- Engine:
  - `src/core/vk_mega_geometry.{hpp,cpp}` (CLAS, Cluster BLAS, OMM build)
  - `src/render/vk_rt_scene.{hpp,cpp}` and `shaders/passes/rt_instances.slang` (PTLAS templates, GPU instance writes, async compute build)
  - `src/render/vk_renderer.cpp` (async compute submission, `computeTimeline` / `rayTimeline`)
  - `src/util/vk_utils.cpp` (`createBuffer`, `createHostBuffer`)
  - `src/core/geometry_store.cpp` (meshlet builder)
  - `shaders/raytracing/trace.slang`, `shaders/geometry/hit.slang`, `shaders/geometry/surface.slang`, `shaders/core/math.slang`
- NVIDIA references:
  - `reference/RTXMG-main` (`rtxmg/cluster_lod/{preloaded.cpp,blas_pass.cpp,streaming.cpp,baking/baker.cpp}`, `demo/shaders/{rtxmg_demo_path_tracer.hlsl,self_intersection_avoidance.hlsli}`)
  - `reference/vk_lod_clusters` (`src/scene_partitions.cpp`, `docs/partitions_and_ptlas.md`)
  - `reference/vk_partitioned_tlas-main` (`src/partitioned_tlas_acceleration_structure.cpp`)
  - [reference/mega_geometry_comparison.md](../../reference/mega_geometry_comparison.md)
- Vulkan SDK 1.4.363: `share/vulkan/registry/validusage.json` (`VK_NV_cluster_acceleration_structure`, `VK_NV_partitioned_acceleration_structure`, `VK_EXT_opacity_micromap`).

---

## 1. Current implementation

- **Static geometry (`vk_mega_geometry.cpp`):** Per model at load (`queue.waitIdle()`): builds `VkMicromapEXT` (OMM) for alpha-tested primitives → CLAS (`eComputeSizes` → `eExplicitDestinations`, 8-bit raster-order indices, `clusterID` = model-relative meshlet row, `minPositionTruncateBitCount = 0`) → Cluster BLAS per primitive (`eComputeSizes` → `eExplicitDestinations`). Flags: `ePreferFastTrace | eAllowDataAccess`, plus `eAllowClusterOpacityMicromapsNV` on CLAS whenever the model has any OMMs.
- **Per-frame TLAS (`vk_rt_scene.cpp`, `rt_instances.slang`):** Single `VK_NV_partitioned_acceleration_structure` updated in place on async compute (`eWriteInstance` for dirty transforms only, one atomic per wave into `operation.argCount`, fixed XZ grid partitions, no global partition, no explicit AABB). Synced with graphics via `rayTimeline` / `computeTimeline`.
- **Traversal and hit decode (`trace.slang`, `hit.slang`, `surface.slang`):** Single-`Proceed()` `RayQuery` with `RAY_FLAG_FORCE_OMM_2_STATE`, `CommittedClusterID()` + `CommittedPrimitiveIndex()` + hardware position fetch + RTXMG's `SafeSpawnPoint` port.

---

## 2. Bugs and spec issues

| # | Severity | Issue | Location | Fix |
|---|---|---|---|---|
| MG-B1 | High (VUID) | `VkMicromapCreateInfoEXT::offset` can violate 256-byte alignment (`VUID-VkMicromapCreateInfoEXT-offset-07438`) | `vk_mega_geometry.cpp`, `vk_rt_scene.cpp` | **Fixed in I3 (2026-11-21):** `minAlignment = alignment` passed to `createBuffer`, `+ alignment - 1` slack removed, `base % alignment == 0` checked at runtime, `.offset = plan.storageOffset` |
| MG-B2 | Medium (perf bug) | CPU readbacks read from uncached write-combined ReBAR VRAM (`HOST_ACCESS_SEQUENTIAL_WRITE_BIT`) | `vk_mega_geometry.cpp`, `vk_rt_scene.cpp` | **Fixed in I3 (2026-11-21):** `eHostCached` `ClusterAsSizesReadback` and `PtlasArgCountReadback` (via 4 B `copyBuffer2`); `blasAddresses` returned from CPU in `placeStructures()` |
| MG-B3 | Low (edge case) | PTLAS growth can exceed `maxInstanceCount`; log narrows `vk::DeviceSize` to `uint32_t` | `vk_rt_scene.cpp` | **Fixed in I3 (2026-11-21):** `uint64_t` growth clamped to `maxInstanceCount`, `std::unsigned_integral` `divideRoundUp` logs 64-bit `vk::DeviceSize` directly |
| MG-B4 | Medium (correctness on motion) | `templates.sceneBounds` (NRC bounds) and PTLAS partition assignments are frozen at layout time | `vk_rt_scene.{hpp,cpp}`, `object_storage.hpp` | **Fixed in I3 (2026-11-21):** dynamic `sceneBounds` grows monotonically with 25% / 1 m headroom on moved entities; partition reassignment shipped on by default (−26.6% PTLAS build, −0.031 ms GPU total on moving field 64; closes `C-C4-5`) |

### MG-B1 · `VkMicromapCreateInfoEXT::offset` alignment (`VUID-VkMicromapCreateInfoEXT-offset-07438`)

- `VUID-VkMicromapCreateInfoEXT-offset-07438` requires `VkMicromapCreateInfoEXT::offset` itself to be a multiple of 256 bytes, not just `bufferDeviceAddress + offset`.
- `MegaGeometry::createOwnedBuffer` (`vk_mega_geometry.cpp:164`) and `RayTracingScene::createAlignedBuffer` (`vk_rt_scene.cpp:385`) manually over-allocate `bytes + alignment - 1` and align `address` up instead of using `createBuffer`'s `minAlignment` parameter (`vk_utils.cpp:76`, `vmaCreateBufferWithAlignment`).
- At `vk_mega_geometry.cpp:376-385`, `storageStart = model.micromapStorage.address - base` is added to `plan.storageOffset`. If VMA places `micromapStorage` at a non-256-byte offset, `storageStart` is `1..255` and `offset = storageStart + plan.storageOffset` violates the VUID.
- **Fix:** Pass `minAlignment = alignment` to `createBuffer` in both helpers, remove the `+ alignment - 1` slack, and assert `base % alignment == 0` so `storageStart == 0`.

### MG-B2 · Uncached write-combined ReBAR readbacks

- `createBuffer` (`vk_utils.cpp:95-104`) maps `eHostVisible | eHostCoherent` without `eHostCached` to `VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT` (ReBAR write-combined VRAM; `vk_utils.cpp:102` notes ~100× slower CPU reads).
- Three readbacks hit WC memory:
  1. `MegaGeometry::buildModel` `readSizes()` (`vk_mega_geometry.cpp:469-473`) reads `uint32_t[clusterCount]` and `uint32_t[blasCount]` from `inputs.mapped` (`vmaInvalidateAllocation` on coherent WC memory does not make reads cached).
  2. `MegaGeometry::buildModel` (`vk_mega_geometry.cpp:533-536`) reads `blasAddresses` back from `inputs.mapped` even though `placeStructures()` just wrote those same addresses from the CPU at line 483.
  3. `RayTracingScene::collectResults()` (`vk_rt_scene.cpp:483`) reads `buffers.operation.mapped` (`createHostBuffer`, WC) every frame.
- **Fix:** Allocate a small `eHostVisible | eHostCoherent | eHostCached` readback buffer for `sizes` in `MegaGeometry` and for the per-frame `argCount` readback in `RayTracingScene`; keep `blasAddresses` in a CPU `std::vector<vk::DeviceAddress>` returned by `placeStructures()`.

### MG-B3 · PTLAS capacity clamp and 64-bit logging

- `RayTracingScene::buildTemplates()` checks `count > maxInstances` (`vk_rt_scene.cpp:202`), but `createPtlas()` computes `std::max({templates.shape.instances, previous.instances * 2, kMinCapacity})` (`vk_rt_scene.cpp:299`) without clamping to `maxInstanceCount`.
- Lines 316–317 cast `sizes.accelerationStructureSize` and `sizes.buildScratchSize` to `uint32_t` before `divideRoundUp(..., 1024)`.
- **Fix:** Clamp `sized.instances` to `static_cast<uint32_t>(std::min<uint64_t>(maxInstances, UINT32_MAX))` and use a `vk::DeviceSize` overload for `divideRoundUp`.

### MG-B4 · Moving entities outside layout-time `sceneBounds`

- `templates.sceneBounds` is computed once in `buildTemplates()` (`vk_rt_scene.cpp:188-195`) from layout-time transforms and passed to `NrcCache::prepareFrame` (`vk_renderer.cpp:217`).
- `NrcCommon.h:145` requires every ray-hittable point to lie inside `sceneBounds`; an entity moving outside its layout-time AABB clips NRC position encoding (and widens its fixed PTLAS partition bounds, `C-C4-5`).
- **Fix:** Track dynamic entity bounds (or add margin / reconfigure when a moving entity exceeds `sceneBounds`), and revisit partition reassignment when moving-entity scenes are benchmarked.

---

## 3. Performance improvements

| # | Lever | Expected gain | Result (I10, 2026-11-22, RTX 5070) | Location |
|---|---|---|---|---|
| MG-P1 | Split opaque vs alpha-tested CLAS builds (`eAllowClusterOpacityMicromapsNV` on OMM primitives only) | Reclaims CLAS VRAM and removes OMM layout overhead on opaque clusters | **kept:** Sponza CLAS `6.56 → 6.02 MiB` (−8.2%, −0.54 MiB; host est. −28.7%), Bistro CLAS `50.88 → 46.16 MiB` (−9.3%, −4.72 MiB); packed in meshlet row order; neutral GPU total (`0.0%` gallery/courtyard/Bistro, `−1.4%` field 64) | `vk_mega_geometry.cpp` |
| MG-P2 | 7-bit CLAS position truncation (`minPositionTruncateBitCount = 7`) + matching quantized raster positions | ~22% smaller CLAS, keeps raster/RT bit-identical | **measured & reverted:** `ENGINE_RT_CHECK` 32/32 passed with 0 self-hits; saved CLAS VRAM (`−19.6%` Sponza, `−18.1%` Bistro) but regressed trace on 3/4 scenes (`RaySum +5.9%` gallery, `+4.3%` field 64, `+4.4%` Bistro; `GI shade +7.2%/+8.9%/+14.1%`) | `vk_mega_geometry.cpp`, `geometry_store.cpp` |
| MG-P3 | Spatial meshlet builder (`meshopt_buildMeshletsSpatial`) | Tighter cluster AABBs → faster ray traversal; A/B against raster cost | **kept (`min = 42, fill = 0.75`):** `RaySum −10.9%` gallery (`GPU total −3.9%`), `−6.6%` courtyard (`−3.2%`), `−12.8%` Bistro (`−0.4%`); `min = 31, fill = 0.5` inflated Bistro clusters +53% and regressed Bistro `GPU total +5.5%` | `geometry_store.cpp` |
| MG-P4 | Eliminate redundant vertex loads and matrix ALU on every ray hit | Removes 36 B dead position loads, 136 B `GpuNodeInstance` dead load, and `normalMatrix` cofactors via `hit.objectToWorld` / `transpose((float3x3)hit.worldToObject)` | **kept:** bit-identical `ENGINE_RT_CHECK` (`0.0000 deg` decode error); 2560×1369 ABBA `Indirect` clock-normalized `−1.1%` gallery, `−0.6%` courtyard, `−4.5%` Bistro (`−0.107 ms / −5.8%` raw) | `frame.slang`, `surface.slang`, `hit.slang` |
| MG-P5 | PTLAS partition layout (drop empty cells, Z-order numbering, refinement) + explicit instance AABBs | Smaller PTLAS, better partition locality | **layout kept (`kDerivedMaxInstances = 1024`), explicit AABBs reverted:** layout-only cuts Field 64 `Partitions 81 → 64`, `PtlasKiB 1,405 → 1,385`, `AsyncPtlas −9.0%`, `Ray start wait −36.3%`, `DI initial −27.9%`, `GPU total −2.3%` raw (`−1.0% norm`), neutral on Sponza & Bistro; explicit Arvo AABBs loosened Bistro's rotated primitives and regressed Bistro `RaySum +3.3% norm` (`Indirect +5.1%`) | `vk_rt_scene.cpp`, `vk_rt_scene.hpp` |
| MG-P6 | Trim `MegaGeometry::buildModel` load stalls and scratch over-sizing | Drops `eImplicitDestinations` from `scratchBytes` (M9 GPU-only pipeline deferred) | **kept:** `eImplicitDestinations` queried only for `clasEstimate` log; on driver 621.01 `eExplicitDestinations` matches `eImplicitDestinations` (`4.67 MiB` Sponza, `14.04 MiB` Bistro) | `vk_mega_geometry.cpp` |
| MG-P7 | Overlap `rt_instances` with async light preparation + I4 §3 `rayTimeline` / pre-acquire ray submit | Removes false compute serialization, hides `AcquireImage` CPU latency, overlaps async compute with `DLSS-RR` | **kept (all three parts):** `rtScene.recordBuild` before `rtxdi.recordLightPreparation` (with `bindHeaps` after `buildPartitionedAccelerationStructuresNV`), pure-Vulkan pre-DLSS ray submit before `AcquireImage` (DLSS-RR + post stay with `presentCmd` for C-G3-6), and pre-DLSS `rayTimeline` signal when NRC is paused and `rayCheck` is off; Field 64 `Ray start wait −80%` (`0.0355 → 0.0071 ms`, `−95.7%` vs baseline), `GPU total −1.1%` raw, `CpuDrawFrame −3.1%`; Bistro `RasterSum −40.8%`, `Ray start wait −47.8%`, `OverlapOffset +0.08 → −1.16 ms` | `vk_renderer.cpp`, `vk_renderer.hpp`, `vk_rt_scene.cpp`, `vk_rtxdi.cpp`, `engine_bench.cpp` |

### Notes per lever

- **MG-P1 (Split OMM CLAS build):** When a model has any alpha-tested primitive, `vk_mega_geometry.cpp:309-311` currently sets `eAllowClusterOpacityMicromapsNV` on all clusters of the model (89 opaque primitives on Sponza pay for 14 alpha primitives). Build opaque clusters in one indirect call (flags without `eAllowClusterOpacityMicromapsNV`, `opacityMicromapIndexType` unused) and OMM clusters in a second call inside the same submit, writing into disjoint slices of `model.clas`.
- **MG-P2 (Position truncation):** Quantize vertex positions (`QuantizeFloat`, drop 7 mantissa bits) before meshlet/raster upload so raster and CLAS read identical vertices (`ENGINE_RT_CHECK` stays bit-identical). Re-verify `safeSpawnPoint` / self-hit check at 7 bits (origin error grows by `2^(7-23)·max|p|`). Keep `eAllowDataAccess` on CLAS/BLAS for VUID compliance (`VUID-HitTriangleVertexPositionsKHR-None-08751`, measured free in `mega_geometry_comparison.md` §2.1).
- **MG-P3 (Spatial meshlets):** Replace `meshopt_buildMeshletsFlex` with `meshopt_buildMeshletsSpatial` (64 v / 126 t / min 31 / `fill_weight = 0.5`) behind an A/B; ship only if trace (`Indirect`, `rt_check`) gains clearly without regressing early/late mesh raster.
- **MG-P4 (Ray-hit decode):**
  1. `loadCornerVertices()` (`surface.slang:38-41`) fetches `vertex.pos` for all 3 corners even though `hit.slang:122` immediately overwrites `corners.objectPos = hit.objectPos`. Split positions from shading attributes (or skip position reads in the ray-hit path).
  2. `traceClosest()` (`hit.slang:95-96`) already fetches `hit.objectToWorld` and `hit.worldToObject` (96 B of registers), yet `decodeHit()` (`hit.slang:125`) and `buildSurfaceAt()` (`surface.slang:129`) load `nodeInstance.worldMatrix` from BDA and recompute `normalMatrix(world)` via 3×3 cofactors. Use `transpose((float3x3)hit.worldToObject)` directly for normals and `query.CommittedTriangleFrontFace()` (XOR `kNodeFlagFlipWinding`) for facing.
- **MG-P5 (PTLAS layout + explicit AABBs):**
  1. Drop empty XZ cells (`partitionCount` = occupied cells), number occupied cells along a Morton Z-order curve (`part1By1`), and double grid resolution while any cell exceeds a max-instances target (bounded by `maxPartitionCount`).
  2. Pass `VK_PARTITIONED_ACCELERATION_STRUCTURE_INSTANCE_FLAG_ENABLE_EXPLICIT_BOUNDING_BOX_NV` in `rt_instances.slang` with the world-transformed mesh-space `primitiveBounds` (already computed in `vk_rt_scene.cpp:47-56`) instead of zeros, so the driver skips deriving world AABBs from every BLAS.
- **MG-P6 (Load-time build):** Don't fold `eImplicitDestinations` into `scratchBytes` (it is only queried for logging `clasEstimate`). For M9 background loading, move from 3 `queue.waitIdle()` submits to RTXMG's GPU-only pipeline (`eImplicitDestinations` into a reusable transient buffer → GPU prefix sum / allocator → `eMoveObjects` with `NO_OVERLAP` into compact persistent storage).
- **MG-P7 (Async compute overlap):** In `vk_renderer.cpp:613-614`, `rtScene.recordBuild()` runs after `rtxdi.recordLightPreparation()`, and `reuseBarrier` (`vk_rt_scene.cpp:412-422`) stalls `rt_instances` behind `rtxdi_presample_regir` despite disjoint buffers. Reorder or tighten the barrier scope so `rt_instances` overlaps light preparation on the async compute queue.

---

## 4. Post-1.0 features (animations, streaming, cluster LOD, and large tree worlds)

Recorded from `reference/RTXMG-main`, `reference/vk_lod_clusters`, `reference/vk_animated_clusters`, and the unimplemented / reverted I10 findings (`i10_mega_geometry_perf.md`, 2026-11-22/23) for engine spec §16.3 / §16.5:

- **Cluster templates for animation and wind-swaying foliage (`vk_animated_clusters`, `RTXMG` `cluster_tessellator.cpp`):** When skinning/deformation and animated foliage land post-1.0, build one immutable cluster template per cluster topology at load and instantiate (`INSTANTIATE_TRIANGLE_CLUSTER`) per frame from deformed vertex positions instead of rebuilding CLAS topology from scratch.
- **GPU-only streaming CLAS/BLAS build pipeline (deferred half of MG-P6; `RTXMG` `blas_pass.cpp:153-160`, `streaming.cpp:51-56`):** Replace 1.0's synchronous 3× `queue.waitIdle()` model builds with `eImplicitDestinations` into a reusable transient buffer → GPU prefix sum / sub-allocator → `eMoveObjects` (`NO_OVERLAP`) into compact persistent storage so streamed world chunks load with zero CPU readbacks and zero frame hitches.
- **BLAS sharing, caching, and merging (`vk_lod_clusters` L2, `RTXMG` `blas_pass.cpp:134-161`):** For a large world of instanced trees with cluster LOD, deduplicate per-instance Cluster BLASes sharing an LOD cut and cache persistent coarse BLASes into a pool via `MOVE_OBJECTS` (`NoOverlap`) so thousands of tree instances share 2–3 BLAS builds per species per frame.
- **Per-mesh Cluster BLAS merge (revisit MG-S2 for multi-primitive trees):** Closed in 1.0 because Bistro has 100% single-primitive meshes (`551/551`, `2,909` instances). Revisit in a large tree world where multi-primitive trees (trunk + branches + leaf cards) are heavily instanced: merging a mesh's primitives into one Cluster BLAS (with per-cluster primitive/material lookup in `hit.slang`) cuts PTLAS instance counts and per-frame updates by 2–4×.
- **7-bit CLAS position truncation (reverted MG-P2; `minPositionTruncateBitCount = 7`):** Reverted in 1.0 because it regressed RTX 5070 trace time (`+4.3–5.9%` `RaySum`, `GI shade +7–14%`) while VRAM was not tight; however `ENGINE_RT_CHECK` verified `0` self-hits across 7.79M interior pixels and `−18–20%` CLAS VRAM (`−7.53 MiB` on Bistro). Revisit as a VRAM/residency compression lever for streamed worlds, large tree populations, and the 8 GB floor tier (`engine_spec.md` R5/R7), or apply only to coarse/distant LOD levels where rays hit infrequently.
- **PTLAS explicit instance AABBs (reverted half of MG-P5; `ENABLE_EXPLICIT_BOUNDING_BOX_NV`):** Reverted in 1.0 because Arvo-transformed mesh-space AABBs loosened Bistro's rotated primitives (`RaySum +3.3%` norm, `Indirect +5.1%`) while `AsyncPtlas` was already small. Revisit for large dynamic/animated worlds (thousands of wind-animated or LOD-swapping tree instances updating per frame), using tighter per-instance bounds (or enabling only on compact/non-skewed/deforming instances where PTLAS build cost outweighs box looseness).
- **Large-world PTLAS partitioning & translation ops:** MG-P5's derived-grid layout caps `kDerivedMaxPartitions = 4096` and `kMaxGridAxis = 128` over XZ. For kilometer-scale streamed worlds with moving clusters of trees/entities, revisit dynamic grid re-centering, streaming partition allocation, and `WRITE_PARTITION_TRANSLATION_NV` for moving hierarchical groups.
- **Position stripping (`RTXMG` `streaming.cpp:117-123`):** MG-P4 already removed all vertex-position loads from ray-hit decoding (`surface.slang`, `hit.slang`). Once geometry streaming decouples resident RT attributes from raster streams, drop `float3` positions from the resident RT attribute pool (~12 B/vertex saved) and rely solely on CLAS position fetch.
- **Compacted KHR triangle BLAS for static geometry (MG-S1 / `engine_spec.md` §16.5 V2):** User decided 2026-11-23 to stay on a single CLAS path for 1.0 (since animations, streaming, and a large tree world follow 1.0). Keep V2 as a post-1.0 toggle for permanently static, single-level geometry (terrain/architecture) alongside CLAS-based animated/LOD/tree content, backed by I10's measured `−12.4% / −15.1% / −5.2%` norm `RaySum` and `−0.31 / −4.04 MiB` VRAM advantage over post-I10 spatial CLAS.

---

## 5. Verification rules

Every bug fix or perf lever lands with:

- Vulkan core + sync + best practices validation clean (plus GPU-AV for shader changes);
- `ctest`, `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST`, `ENGINE_GUIDE_CAPTURE`, and `ENGINE_GI_CHECK` green;
- ABBA GPU timings (release, vkconfig and all validation layers off, ±5% noise) on Sponza and the moving instance field before keeping any MG-P* lever.
