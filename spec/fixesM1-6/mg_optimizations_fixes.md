# Feature: RTX Mega Geometry — bugs, spec issues, and performance improvements

**Date:** 2026-11-21 · **Status:** review; none implemented yet · **Milestone:** [M8](./README.md) (follows [M2](../m2/) C1–C4, [M6](../m6/) D1–D4, and [mega_geometry_comparison.md](../../reference/mega_geometry_comparison.md))
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
| MG-B1 | High (VUID) | `VkMicromapCreateInfoEXT::offset` can violate 256-byte alignment (`VUID-VkMicromapCreateInfoEXT-offset-07438`) | `vk_mega_geometry.cpp:164, 373-385`, `vk_rt_scene.cpp:385` | Pass `minAlignment = alignment` to `createBuffer` (`vmaCreateBufferWithAlignment`), drop `+ alignment - 1` slack, assert `base % alignment == 0` |
| MG-B2 | Medium (perf bug) | CPU readbacks read from uncached write-combined ReBAR VRAM (`HOST_ACCESS_SEQUENTIAL_WRITE_BIT`) | `vk_mega_geometry.cpp:161-166, 469-473, 533-536`, `vk_rt_scene.cpp:353-355, 483` | Use `eHostCached` (`HOST_ACCESS_RANDOM_BIT`) for GPU→CPU readbacks; return `blasAddresses` from CPU without reading mapped VRAM |
| MG-B3 | Low (edge case) | PTLAS growth can exceed `maxInstanceCount`; log narrows `vk::DeviceSize` to `uint32_t` | `vk_rt_scene.cpp:298-302, 316-317` | Clamp `sized.instances` to `maxInstanceCount`; divide 64-bit `vk::DeviceSize` directly |
| MG-B4 | Medium (correctness on motion) | `templates.sceneBounds` (NRC bounds) and PTLAS partition assignments are frozen at layout time | `vk_rt_scene.cpp:129, 185-195, 230-236`, `vk_renderer.cpp:217` | Update/expand bounds when entities move beyond layout bounds (and reassign partitions when moving scenes need it, `C-C4-5`) |

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

| # | Lever | Expected gain | Reference | Location |
|---|---|---|---|---|
| MG-P1 | Split opaque vs alpha-tested CLAS builds (`eAllowClusterOpacityMicromapsNV` on OMM primitives only) | Reclaims CLAS VRAM (+13.6% / +0.64 MiB on Sponza, −51% host estimate, `C-D2-1`) and removes OMM layout overhead on opaque clusters | `VUID-...-opacityMicromapArray-10881` | `vk_mega_geometry.cpp:309-311, 508-518` |
| MG-P2 | 7-bit CLAS position truncation (`minPositionTruncateBitCount = 7`) + matching quantized raster positions | ~22% smaller CLAS (`mega_geometry_comparison.md` §2.2), lower traversal bandwidth, keeps raster/RT bit-identical | `RTXMG` `baker.cpp:49-74`, `preloaded.cpp:505`; `vk_lod_clusters` `compressionPosDropBits = 7` | `vk_mega_geometry.cpp:217`, `geometry_store.cpp` |
| MG-P3 | Spatial meshlet builder (`meshopt_buildMeshletsSpatial`, `fill_weight = 0.5`) | Tighter cluster AABBs → fewer BVH node overlaps and faster ray traversal; A/B against early/late raster cost | `RTXMG`, `vk_lod_clusters` (`clodDefaultConfigRT`) | `geometry_store.cpp:314` |
| MG-P4 | Eliminate redundant vertex loads and matrix ALU on every ray hit | Removes 36 B dead position loads (`loadCornerVertices` overwritten by `hit.objectPos`), removes `normalMatrix(worldMatrix)` cofactor ALU and `worldMatrix` load by reusing `hit.worldToObject` and `CommittedTriangleFrontFace()` | `RTXMG` `streaming.cpp:117-123` (`stripResidentPositions`), `hit.slang:159` | `hit.slang:89-131`, `surface.slang:28-46, 108-156` |
| MG-P5 | PTLAS partition layout (drop empty cells, Z-order numbering, refinement) + explicit instance AABBs | Smaller PTLAS, better partition locality, ~50 µs combined build + trace win on Caldera | `vk_lod_clusters` `scene_partitions.cpp:46-131`, `docs/partitions_and_ptlas.md` | `vk_rt_scene.cpp:216-242`, `rt_instances.slang:88` |
| MG-P6 | Trim `MegaGeometry::buildModel` load stalls and scratch over-sizing | Drops `eImplicitDestinations` from `scratchBytes`; M9 GPU-only `eImplicitDestinations` → `eMoveObjects` (`NoOverlap`) removes 3 `queue.waitIdle()` stalls | `RTXMG` `blas_pass.cpp:153-160`, `streaming.cpp:51-56` | `vk_mega_geometry.cpp:322-337, 503-531` |
| MG-P7 | Overlap `rt_instances` with async light preparation | Removes false serialization of `rt_instances` behind `rtxdi_presample_regir` caused by `reuseBarrier` | — | `vk_renderer.cpp:613-614`, `vk_rt_scene.cpp:412-422` |

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

## 4. Post-1.0 RTXMG features (when LOD / deformation land)

Recorded from `reference/RTXMG-main` for engine spec §16.3 / §16.5 (not part of M8):

- **BLAS sharing and caching (`RTXMG` `blas_pass.cpp:134-161`):** Deduplicate per-instance Cluster BLASes sharing an LOD cut and cache persistent coarse BLASes into a pool via `MOVE_OBJECTS` (`NoOverlap`).
- **Position stripping (`RTXMG` `streaming.cpp:117-123`):** Drop `float3` positions from the resident RT attribute pool (~12 B/vertex saved) and rely solely on CLAS position fetch.
- **Cluster templates (`RTXMG` `cluster_tessellator.cpp`, `vk_animated_clusters`):** When skinning/deformation returns, build one template per cluster at load and instantiate (`INSTANTIATE_TRIANGLE_CLUSTER`) per frame instead of rebuilding CLAS from scratch.

---

## 5. Verification rules

Every bug fix or perf lever lands with:

- Vulkan core + sync + best practices validation clean (plus GPU-AV for shader changes);
- `ctest`, `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST`, `ENGINE_GUIDE_CAPTURE`, and `ENGINE_GI_CHECK` green;
- ABBA GPU timings (release, vkconfig and all validation layers off, ±5% noise) on Sponza and the moving instance field before keeping any MG-P* lever.
