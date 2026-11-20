# Feature: ray tracing with RTX Mega Geometry (patterns from `vk_lod_clusters`)

**Date:** 2026-11-13 · **Status:** C1 built (CLAS + cluster BLAS per primitive), RT1–RT3 done (C2 built), RT4–RT5 done (C3 built); PTLAS required since 2026-11-14 and built (C4, RT9 done); the RT pipeline not started · **Milestone:** M2 (C2, C3, C4), M4 (RT pipeline, SER), M6 (OMM), post-1.0 (LOD, §16.3)
**Implements:** engine spec §10 (RT scene), §11.1 (ray model); plan C2, C3, D4, G1 ([pbr_implementation_plan.md](../pbr_implementation_plan.md))
**Re-check 2026-11-17:** [reference/mega_geometry_comparison.md](../../reference/mega_geometry_comparison.md) (updated samples: ray queries vs pipeline, static CLAS vs triangle BLAS, RT clustering, truncation in raster, PTLAS layout)
**Related:** [g1_ser_rt_pipeline.md](../m4/g1_ser_rt_pipeline.md), [visibility_buffer.md](./visibility_buffer.md), sample [README](../../reference/vk_lod_clusters/README.md) and [docs/](../../reference/vk_lod_clusters/docs/); secondary references in §1.1

This document maps the NVIDIA sample `reference/vk_lod_clusters` onto the engine: what it does for ray tracing, which parts the engine takes as they are, which parts it changes, and in what order to build them. Sample paths are relative to `reference/vk_lod_clusters/` unless a section names another reference. Line numbers are from the vendored copy and are given for orientation only.

---

## 1. The sample at a glance

| Piece | Sample file | Engine status |
|---|---|---|
| CLAS per cluster, built once (preloaded mode) | `src/scene_preloaded.cpp` (`initClas`) | **built** (C1), [vk_mega_geometry.cpp](../../src/core/vk_mega_geometry.cpp) |
| Prebuilt low-detail BLAS per geometry | `src/scene_preloaded.cpp`, `traversal_init.comp.glsl:303` | not needed until LOD (§9) |
| Per-frame cluster BLAS builds (implicit, GPU count) | `src/renderer_raytrace_clusters_lod.cpp` ~950–1075, `blas_setup_insertion.comp.glsl`, `blas_clusters_insert.comp.glsl` | not needed until LOD (§9) |
| TLAS instance patch (BLAS address per instance) | `instance_assign_blas.comp.glsl` | C2 (§3) |
| TLAS build / update | `renderer_raytrace_clusters_lod.cpp:1379` (`initRayTracingTlas`), `:1448` (`updateRayTracingTlas`) | C2 (§3) |
| RT pipeline with cluster AS | `renderer_raytrace_clusters_lod.cpp:1237` (`initRayTracingPipeline`) | G1 (§4) |
| Minimal closest-hit + shading in raygen (path tracer) | `render_pathtrace_clusters.rchit.glsl`, `render_pathtrace_hit.glsl`, `render_pathtrace.rgen.glsl` | C3 + G1 (§5) |
| Ray origin offset, terminator fix, shadow/AO rays | `render_shading.glsl:490` (`offsetRay`), `:510` (`pointOffset`), `:528`, `:570` | C3, D4 (§6) |
| Ray-cone texture LOD | `texturing.glsl:53–75`, `render_pathtrace.rgen.glsl:291, 428, 618` | C3 (§7) |
| Alpha via any-hit | `render_pathtrace_clusters.rahit.glsl` | **not taken**: OMM (§8) |
| LOD traversal, RT cut, BLAS sharing / merging / caching | `traversal*.comp.glsl`, `geometry_blas_sharing.comp.glsl`, `docs/blas_*.md` | post-1.0 (§9) |
| CLAS streaming + GPU allocator | `scene_streaming*.cpp`, `stream_*.comp.glsl`, `docs/clas_allocation.md` | out of scope (spec §10.2: everything resident) |

The sample's two ray-tracing renderers differ in where shading happens. `render_raytrace_clusters.rchit.glsl` shades inside the closest-hit (recursion depth 2 for shadow rays). The path tracer (`usePathtrace`) has a closest-hit that only reports ids and shades in raygen (recursion depth 1). **The engine follows the path tracer**: it is the shape SER needs (spec §11.1).

### 1.1 Secondary references (reviewed 2026-11-13)

| Reference | What it is | What the engine takes |
|---|---|---|
| `reference/RTXMG-main` (RTX Mega Geometry) | D3D12 + Vulkan sample (NVRHI): a port of `vk_lod_clusters` for ray tracing only (`docs/ClusterLOD.md`), plus GPU tessellation of subdivision surfaces into clusters (`docs/ClusterTess.md`). It does not use PTLAS | `SafeSpawnPoint` origin offsets (§6.1); bake and selection defaults for §16.3 (§9.5); position truncation data point (§9.4); per-hit tangent frames as a memory fallback (§5.2) |
| `reference/vk_tessellated_clusters-main` | Vulkan sample: per-frame tessellation and displacement into clusters, CLAS built from **cluster templates** | nothing now; templates are the path for deformed geometry (§9.6) |
| `reference/vk_partitioned_tlas-main` | Vulkan sample for `VK_NV_partitioned_acceleration_structure`: 1.2 M static + 170 k dynamic instances, partial TLAS updates | the PTLAS design (§3.6), required since 2026-11-14 |

---

## 2. Data model

### 2.1 Load time (C1, built)

| Item | Sample | Engine |
|---|---|---|
| CLAS input | per cluster: positions, 8-bit local indices, `clusterID` | same; `clusterID = meshletRow` (model-relative), `indexType = e8`, triangle order = raster order |
| CLAS sizing | `COMPUTE_SIZES` → readback → exact allocation → `EXPLICIT_DESTINATIONS` | same; Sponza 3500 CLAS in 4.72 MiB against a 15.81 MiB host estimate |
| BLAS unit | one BLAS per **geometry** (all its clusters); the material is resolved per cluster/triangle (`texturing.glsl:128` `resolveMaterialID`) | one BLAS per **mesh primitive**; the material is per primitive (`GpuRayInstance.primitiveRow`) |
| BLAS sizing | per-frame implicit builds into one buffer (LOD); low-detail BLAS implicit at load | `COMPUTE_SIZES` + `EXPLICIT_DESTINATIONS` after the CLAS build, since the BLAS refers to CLAS by address |
| Lifetime | scene lifetime | per model, freed when its `refCount` reaches 0; other models' AS are untouched |
| Build flags | `PREFER_FAST_TRACE` on CLAS (`scene_preloaded.hpp:20`), BLAS flags configurable (default 0) | `PREFER_FAST_TRACE \| ALLOW_DATA_ACCESS` on both (§2.3) |

**BLAS per primitive vs per mesh.** The sample's per-geometry BLAS gives one TLAS instance per object, at the cost of a per-cluster material lookup at every hit. The engine's per-primitive BLAS gives one TLAS instance per (node instance, primitive), and the material comes from the instance row with no cluster-level lookup. Keep per-primitive unless the TLAS build measures over budget (§3.4); the fallback is the sample's layout, with the primitive row stored per cluster.

### 2.2 Ids at a hit

| Builtin | Value | Resolves to |
|---|---|---|
| `InstanceIndex()` | row in the TLAS instance array | `GpuRayInstance[row]` → `{nodeInstanceIndex, primitiveRow}` |
| `InstanceID()` (custom index, 24 bit) | free; see §3.2 | candidate: `primitiveRow`, to save one load for the SER hint |
| `ClusterID` (NV builtin) | the CLAS `clusterID` = `meshletRow` | cluster in the model slice |
| `PrimitiveIndex()` | triangle in the cluster | the vis-buffer triangle, since CLAS keeps raster order |
| `HitTriangleVertexPosition` | the 3 CLAS positions (object space) | geometric normal, next origin (spec §10.2) |

### 2.3 Position fetch build flag (RT1, done)

`VUID-HitTriangleVertexPositionsKHR-None-08751` (hit shaders) and `VUID-RuntimeSpirv-flags-08761` (ray queries) require the AS to be built with `ALLOW_DATA_ACCESS`. The sample reads `gl_HitTriangleVertexPositionsEXT` from CLAS built with `PREFER_FAST_TRACE` only, which works on NVIDIA but is not backed by those VUIDs. The `VK_NV_cluster_acceleration_structure` reference page (checked 2026-11-13) says nothing about data access or position fetch; its only new build flag is `ALLOW_CLUSTER_OPACITY_MICROMAPS_BIT_NV`. So no exemption is documented.

- **As built:** `MegaGeometry::buildModel` passes `PREFER_FAST_TRACE | ALLOW_DATA_ACCESS` for both the CLAS and the BLAS inputs, and the compute-sizes queries see the same flags, so the exact sizes include any cost.
- **Measured (RTX 5070, driver 621.1):** no size change at log precision. Sponza: 3500 CLAS in 4.72 MiB and 103 BLAS in 0.24 MiB, the same as without the flag. Validation clean (core, sync, best practices + NVIDIA) on the motion test (13 of 13) and the guide capture (model 0 erased with model 1 resident, normal checks 2 of 2).
- If a later NV spec revision exempts cluster AS, record it here and drop the flag.

---

## 3. Per frame: instances and TLAS (C2)

### 3.1 Sequence

```
graphics (frame N)                            async compute (frame N)
  scene update (CPU deltas, ReBAR)  ──────►   rt_instances.slang: GpuNodeInstance × primitives → VkAccelerationStructureInstanceKHR + GpuRayInstance
                                              barrier  SHADER_WRITE → AS_BUILD_INPUT_READ (stage COMPUTE → AS_BUILD)
  phase 1 / phase 2 raster, resolve           vkCmdBuildAccelerationStructuresKHR (TLAS, full build)
                                              signal timeline = frame value
  wait timeline before the first ray pass ◄── 
  ray passes (ray queries, SER pipeline)
```

The sample runs the same chain on one queue: BLAS builds → barrier → `instance_assign_blas` → barrier → TLAS build → barrier → trace (`renderer_raytrace_clusters_lod.cpp` ~950–1075). With one level of detail the engine has no per-frame BLAS builds, so the chain starts at the instance write.

### 3.2 Instance write pass (`shaders/passes/rt_instances.slang`)

One thread per ray instance. The sample patches only `blasReference` into instances uploaded once from the host (`initRayTracingTlas`), because its instances never move. Engine entities move, so the pass writes whole instances.

| Field | Value | Source / note |
|---|---|---|
| `transform` | 3×4 of the node instance's current world matrix | `GpuNodeInstance` |
| `instanceCustomIndex` | `primitiveRow` (measure first; else 0) | lets the SER hint skip the `GpuRayInstance` load (§4.3) |
| `mask` | `0xFF` | as the sample; reserve bits only when a pass needs to exclude a class |
| `instanceShaderBindingTableRecordOffset` | 0 | one hit group (§4.1) |
| `flags` | `FORCE_OPAQUE`; under PTLAS (C4) alpha-tested primitives carry no flag and rays force 2-state (§8) | spec §10.5. **No** facing flags: RT handles negative determinants itself (`renderer_raytrace_clusters_lod.cpp:1391`), and two-sided needs no flag when rays do not cull back faces |
| `accelerationStructureReference` | the primitive's BLAS address from the per-model table; **0** for a primitive without meshlets | an instance with reference 0 is inactive (Vulkan spec), so it costs nothing at trace time |

Rows map 1:1 to TLAS instances, so `InstanceIndex()` indexes `GpuRayInstance` directly.

**Row assignment (as built).** `GpuRayInstance[row] = {nodeInstanceIndex, primitiveRow}` and the row's BLAS address only change with the scene layout, not per frame. `RayTracingScene` writes both tables per frame slot when `FrameData::layoutGeneration` moves, with the cull templates' entity filter; each thread reads its row and needs no search. Per frame the CPU writes nothing per instance, and the GPU reads one `GpuRayInstance`, one world matrix and one address per thread.

### 3.3 Instance count without a GPU readback

The TLAS build range (`primitiveCount`) is a host value. A GPU-written count needs `vkCmdBuildAccelerationStructuresIndirectKHR`, which requires `accelerationStructureIndirectBuild`, a feature the RTX 5070 on driver 621.1 does not expose (logged at startup: `AS: indirectBuild=false`).

- The CPU knows the exact ray-instance count: the template row count, rebuilt with the layout. Pass it as `primitiveCount`.
- Size the instance buffer, TLAS and scratch for a capacity (`vkGetAccelerationStructureBuildSizesKHR` with the capacity count, bounded by `maxInstanceCount`), with a counter and an overflow counter in the debug UI (spec S11). Grow when the count exceeds it, outside frames in flight.
- Slots that should not be traced (hidden entity, no meshlets) get reference 0 instead of compaction. The sample does the same for culled instances after frame 0.
- The PTLAS build is indirect without any feature bit: `VkBuildPartitionedAccelerationStructureInfoNV::srcInfos` and `srcInfosCount` are device addresses. PTLAS (§3.6) removes this limit: its op count is written on the GPU.

### 3.4 Build or update

| | Sample | Engine |
|---|---|---|
| Flags | `ALLOW_UPDATE` | `PREFER_FAST_TRACE` |
| Mode | full build in frame 0, then `MODE_UPDATE` with the previous TLAS as source (`:1448`) | full build every frame (spec §10.3) |
| Why | static instances; only BLAS references change | entities move; a refit tree degrades with motion and every count change needs a rebuild anyway |

**Measured full build (2026-11-13, RTX 5070, async compute, bench without layers):** 0.101 ms for 103 instances, 0.104 ms for 167, 0.119 ms for 359, 0.150 ms for 4199; the instance write takes 0.0008 ms. Still to measure: update mode, and the 2060 Super. These numbers are the baseline for PTLAS, which replaced the 0.3 ms threshold rule on 2026-11-14 (§3.6).

**Revisited against the secondary references (2026-11-13):** no C2 change.
- RTXMG builds its TLAS per frame, as the engine does, and does not use PTLAS.
- `vk_tessellated_clusters` builds with `ALLOW_UPDATE` only, because its instances are static.
- The per-slot full build is also the layout that needs no change-tracking. Skipping the build for a still scene would need per-frame transform-change detection, which the CPU does not have, and it would save at most 0.15 ms of overlapped async compute.
- PTLAS was then on an M9 trigger; since 2026-11-14 it is required (§3.6).

### 3.5 Resources and sync

- ~~Per frame slot: instance buffer, `GpuRayInstance[]`, TLAS storage, scratch.~~ Since C4: per frame slot `GpuRayInstance[]`, `GpuRayInstanceBuild[]`, the write-instance list and the op entry; one PTLAS with its scratch and state rows for all slots (§3.6). `GpuFrameRoot.tlas` is the PTLAS address, `.rayInstances` the slot's rows.
- Queue: `Device::computeQueue` with its own command pool (a command buffer goes only to its pool's family). Graphics waits on the compute timeline value before the first stage that traces, with `dstStageMask` = that stage (compute for ray queries, `RAY_TRACING_SHADER` for the SER pipeline).
- Queue-family ownership: instance and TLAS buffers written on compute and read on graphics are either `EXCLUSIVE` with release/acquire barriers, or `CONCURRENT` when the two families differ (`VUID-VkBufferCreateInfo-sharingMode-00914` when they do not). **As built:** `createBuffer` makes every buffer `CONCURRENT` over the device's distinct families, so no transfers are recorded.
- **As built:** each compute submit signals the frame number on `FrameSync::computeTimeline`. Before rewriting a slot (node instances, ray-instance tables, TLAS buffers) the CPU waits for that slot's last value, which the frame fence alone does not cover while no graphics work waits on compute. Each build starts with an execution + AS-write barrier against the slot's previous build on the same queue.
- **As built (C3):** each frame slot records two graphics command buffers: the frame (cull, raster, Hi-Z, resolve) and a ray buffer (debug view, motion check, ray check, display capture). One `vkQueueSubmit2` carries both, and only the second waits for the slot's `computeTimeline` value at `COMPUTE_SHADER`, so raster and resolve never wait for the TLAS build. The compute submit is queued first, so the wait never comes before its signal. The CPU wait before a slot is rewritten stays, and now returns at once, since the fenced work already waited.
- BLAS are read-only after load and outlive every frame that references them: model unload waits for the frames in flight that traced it (the same rule as the other model slices).

### 3.6 PTLAS (`vk_partitioned_tlas`), required (plan C4)

Required since 2026-11-14 (engine spec §10.3); built 2026-11-14 (plan C4, step RT9; as built below). It replaces the build step of §3.1 and the per-slot TLAS of §3.5. The sample's helper is `src/partitioned_acceleration_structures.hpp`; the GPU writes happen in `shaders/animation_update_instances.comp.glsl`.

**API in short:**

- **Sizes.** `vkGetPartitionedAccelerationStructuresBuildSizesNV` takes `instanceCount`, `partitionCount`, `maxInstancePerPartitionCount` and `maxInstanceInGlobalPartitionCount`, bounded by `VkPhysicalDevicePartitionedAccelerationStructurePropertiesNV::maxPartitionCount`. The engine already queries these properties (`HardwareCapabilities::partitionedAccelerationStructure`).
- **Build.** `vkCmdBuildPartitionedAccelerationStructuresNV` runs a GPU list of `VkBuildPartitionedAccelerationStructureIndirectCommandNV {opType, argCount, argData}`. The op list and its count are device addresses, so nothing is read back.
- **Ops:**
  - `WRITE_INSTANCE`: a `VkPartitionedAccelerationStructureWriteInstanceDataNV`, i.e. the KHR instance fields plus `instanceIndex` (a stable slot, which must be unique within one op list), `partitionIndex` and `explicitAABB`.
  - `UPDATE_INSTANCE`: `{instanceIndex, instanceContributionToHitGroupIndex, accelerationStructure}`, a BLAS swap without rewriting the transform. It needs `allowInstanceUpdate`.
  - `WRITE_PARTITION_TRANSLATION`: an offset per partition (`enablePartitionTranslation`).
- **Global partition.** `VK_PARTITIONED_ACCELERATION_STRUCTURE_PARTITION_INDEX_GLOBAL_NV` (`~0u`) is cheap to update but costs trace time. The sample has three modes:
  - always rewrite the instance in its own partition;
  - move moving instances to the global partition and back once they stop;
  - decide per partition by its distance to the camera (near partitions stay, far ones go global).
- **Cost.** An update scales with the instances and partitions that changed; a full TLAS build scales with all instances.
- **Binding.** The sample traces with `accelerationStructureEXT(pc.tlas)` and passes the PTLAS address in push constants (`partitioned_tlas.cpp:497`). So `GpuFrameRoot.tlas` keeps working unchanged. `VK_DESCRIPTOR_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_NV` is only needed for descriptor sets, which the engine does not use for AS.
- **Instance flags.** `VkPartitionedAccelerationStructureInstanceFlagBitsNV` has facing-cull disable, flip facing, `FORCE_OPAQUE`, `FORCE_NO_OPAQUE` and `ENABLE_EXPLICIT_BOUNDING_BOX`. It has **no OMM 2-state flag** (§8).

**Engine mapping:**

| Item | Rule |
|---|---|
| `instanceIndex` | the ray-instance row (§3.2). Rows are stable within a layout; a layout change rewrites every instance, which is today's full build |
| Partitions | a uniform XZ grid over the instances' world-space centers at layout time, about 64 instances per cell. **No global partition:** parking moving instances there measured slower on the 5070 (below) |
| Changed set | the instance pass compares the node instance's world matrix with the one the PTLAS holds (a 48-byte state row per instance, rewritten with each write), appends a `WRITE_INSTANCE` with one atomic per wave, and the build reads the count from the op entry. Static frames write zero instances. A stored row cannot miss a move the way a current/previous pair can when a history reset rewrites the previous matrix |
| OMM | alpha-tested instances drop `FORCE_OPAQUE`, and the ray flag forces 2-state (§8) |
| Frames in flight | the per-slot TLAS of §3.5 does not fit: an update applies to its source, and slot N last saw frame N − `MAX_FRAMES_IN_FLIGHT`. **Decided (R5):** one PTLAS updated in place. VUID-10549 forbids src/dst overlap only when they differ, and the sample updates in place (`partitioned_acceleration_structures.hpp:222–228`). Frame N's update waits for frame N−1's last ray pass: the ray command buffer signals a graphics timeline value that the compute submit waits on. One scratch buffer |

**As built (C4, 2026-11-14).** `RayTracingScene` and `rt_instances.slang`; the details are in plan C4.
- One PTLAS, sized with headroom (instances doubled on growth, partitions and per-partition counts monotonic), built every frame with that sized input, `PREFER_FAST_TRACE`. A layout change or a new PTLAS is a full build (source 0); otherwise the update is in place.
- Per slot, a host-visible op entry `{WRITE_INSTANCE, argCount, writeInfos, 104}` and its op count of 1. The CPU resets `argCount` after the slot's compute wait, and the pass appends to it. The build reads both by address.
- Sync: the ray command buffer signals `FrameSync::rayTimeline`; the compute submit of frame N waits for N−1 at `ACCELERATION_STRUCTURE_BUILD`. A growth waits on the CPU for both timelines.
- `instanceIndex` = ray-instance row; `InstanceIndex()` returns it in ray queries (`ENGINE_RT_CHECK`: no id mismatches). `instanceID` = primitive row.

**Measured (2026-11-14, RTX 5070, bench without layers).** Still frames: 0.015–0.019 ms for 103 to 4199 instances, against 0.10–0.15 ms for C2's full build. Moving: 0.12 ms for Sponza with all 103 instances written, 0.12 ms for the side-64 field with 359 written, 0.18 ms with all 4199 written (C2: 0.101 and 0.150 ms). Trace passes do not change between still and moving frames. Any non-empty update costs at least ~0.11 ms, so an all-moving scene updates slower than a full build; mostly static scenes are the PTLAS case.
- Global partition (moving instances parked, returned after 16 still frames): 0.19–0.21 ms in the same moving cases, so it was dropped.
- `PREFER_FAST_BUILD` or no flags: no gain on moving frames; `PREFER_FAST_BUILD` raises still frames to 0.09–0.14 ms.
- Grid density (~16, ~64, ~256 per cell): no consistent winner; ~64 kept.
- Open: Bistro (M10; engine spec Q10), the 2060 Super, and the sample's near/far mode, which could bring the global partition back for far partitions.

---

## 4. RT pipeline (G1)

### 4.1 Creation (sample `initRayTracingPipeline`, `:1237`)

| Item | Sample | Engine |
|---|---|---|
| Stages | raygen, miss, AO miss, closest-hit, optional any-hit | raygen, miss, closest-hit; **no any-hit** (§8) |
| Hit groups | one triangle group | one; `instanceShaderBindingTableRecordOffset = 0`, `sbtRecordOffset = 0` |
| Recursion | 1 for the path tracer, 2 when the closest-hit traces shadow rays (`:1331`) | **1**: every ray starts in raygen; visibility uses ray queries |
| Cluster AS | `VkRayTracingPipelineClusterAccelerationStructureCreateInfoNV.allowClusterAccelerationStructure = true` in `pNext` (`:1338`) | same; required, or cluster BLAS hits are undefined |
| Binding | descriptor sets | descriptor-heap pipeline flag, as the shader objects |
| SBT | `nvvk::SBTGenerator`, buffer usage `SHADER_BINDING_TABLE` | hand-written: handle size / alignment / base alignment from `VkPhysicalDeviceRayTracingPipelinePropertiesKHR`; one buffer, three regions |
| Dispatch | `vkCmdTraceRaysKHR` | `vkCmdTraceRaysIndirect2KHR` (spec Q4) |

### 4.2 Closest-hit payload

The sample's path-trace closest-hit (`render_pathtrace_clusters.rchit.glsl`) writes `{hitT, instanceID, clusterID, triangleID, barycentrics}` plus either the 3 fetched positions or the world geometric normal (`PATHTRACE_HIT_POSITIONS`). The engine's SER design ([g1_ser_rt_pipeline.md](../m4/g1_ser_rt_pipeline.md) §3) writes `{clusterId, triangle, barycentrics, hitT}` (24 B) and takes the positions from `HitObject::GetTriangleVertexPositions` in raygen, which keeps the payload small across `ReorderThread`.

**Reading `ClusterID` in Slang.** The sample declares it in GLSL with `spirv_decorate(extensions = ["SPV_NV_cluster_acceleration_structure"], capabilities = [5437], 11, 5436) in int gl_ClusterIDNV_` (BuiltIn `ClusterIDNV` = 5436, capability `RayTracingClusterAccelerationStructureNV` = 5437). If the pinned Slang has no closest-hit intrinsic for it, use the same SPIR-V through `spirv_asm` in one helper in `raytracing.hit`. The SER4 compile probe answers which.

### 4.3 SER hint

The sample does not use SER. The engine's hint (SER spec §3) needs `materialBin(primitives[rayInstances[InstanceIndex()].primitiveRow].materialId)`: three dependent loads before the reorder. Storing `primitiveRow` in `instanceCustomIndex` (§3.2) removes one. Measure the reorder cost with and without it.

---

## 5. Hit decode (C3)

### 5.1 Chain

The sample's raygen-side decode (`render_pathtrace_hit.glsl`, `getHitAttributes`):

```
instance = instances[instanceID]; geometry = geometries[instance.geometryID]
cluster  = geometry.preloadedClusters[clusterID]           // cluster header by address
idx      = cluster.localIndices[triangleID*3 + 0..2]        // 8-bit cluster-local vertex ids
normals / tangents / uvs by idx from the cluster's attribute arrays
```

The engine's `decodeHit` (plan C3) is the same chain over the model slices: `GpuRayInstance` → `GpuNodeInstance` + `GpuPrimitive` → `GpuModel` → meshlet `meshletRow` → `meshletTriangles` → `meshletVertices` → attribute streams. It must share the attribute fetch and `buildSurface` with the vis-buffer resolve, so the C3 "same shading normal" check compares one code path with itself.

**As built (RT4, `shaders/geometry/hit.slang`):** `traceClosest` returns a `RayHit` (instance index, `CommittedClusterID()`, primitive index, barycentrics, `t`, the 3 fetched positions, both instance matrices). `decodeHit` turns `GpuRayInstance.nodeInstanceIndex` and the cluster id into a `GpuVisibleMeshlet` and runs the resolve's own functions on it: `loadDrawContext`, then `loadCornerVertices`, then `buildRaySurface`. The resolve's functions are split along the same seam: `loadTriangleCorners` is `loadCornerVertices` plus jittered clip positions and facing, and `buildSurface` is `buildSurfaceAt` plus analytic gradients.

### 5.2 Geometry at the hit

Taken from `getHitAttributes` as is:

1. **World position** = `origin + dir · hitT`. It matches the depth the ray reports. Do not interpolate the fetched positions for it.
2. **Geometric normal** from the fetched positions: `oGeo = cross(p1 − p0, p2 − p0)`, world `normalize(oGeo · worldMatrixI)` (inverse transpose). The fetched positions are the CLAS ones (truncated when `positionTruncateBitCount > 0`), so the normal matches the surface the ray hit.
3. **Facing**: `backFacing = dot(oGeo, objectRayDir) > 0`. On a back-face hit, negate the shading normal **and** the geometric normal together.
4. **Mirrored instances**: the inverse-transpose normal and the cross-product normal disagree in sign under a negative determinant. Snap the interpolated shading normal into the geometric hemisphere (`dot(wNormal, wGeo) < 0 → −wNormal`) before the facing flip.
5. **Tangent**: interpolate, transform by `worldMatrixI`, normalize; the sign comes from vertex 0. The engine's A4 tangents slot in here. RTXMG stores no tangents at all (simplifier tangent weight 0) and builds the frame per hit from the triangle's position and UV deltas (`ClusterLodPerturbNormal`, `demo/shaders/rtxmg_demo_path_tracer.hlsl`). It is the fallback if tangent memory matters after §16.3, but raster and rays must then use the same derivation.
6. **Shading-normal sanity** (sample's closest-hit variant): when `reflect(dir, N)` points below the geometric surface, bend `N` toward the geometric normal. Apply it in `buildSurface` only for rays, not the resolve, or the C3 resolve comparison fails by construction.

**As built (RT4):**

- **Position:** interpolated from the fetched positions at the hit barycentrics, then transformed by the world matrix, as the resolve does, instead of `origin + dir · hitT` (item 1). One surface builder serves both, and with untruncated CLAS (§9.4) the two points land at most 0.056 px apart on screen, from the precision of the hardware hit barycentrics (`ENGINE_RT_CHECK` gates at 0.1 px). `safeSpawnPoint` rebuilds its own `precise` point (§6.1).
- **Geometric normal and facing:** from the fetched positions; front-facing when `dot(normalMatrix(world) · cross(p1 − p0, p2 − p0), dir) < 0`. `normalMatrix` keeps normals outward under a negative determinant, which gives the facing the rasterizer derives with `FlipWinding`, so the mirror snap (item 4) is not needed: the mirrored motion-test scenarios and `TwoSidedPlane` match.
- Item 6 is not built; it comes with secondary shading (G1).

### 5.3 Texture LOD

The hit computes `texelDensity = sqrt(uvArea / worldArea)` per triangle (`computeTexelDensity`, `texturing.glsl:159`, from the fetched positions and the instance matrix) for the ray cone (§7).

---

## 6. Ray origins and visibility rays (C3, D4)

### 6.1 Offsets

| Function | Rule | Engine use |
|---|---|---|
| `offsetRay(p, dir, geoN)` (`render_shading.glsl:490`) | `n = sign(dot(dir, geoN)) · geoN`; `p + n · (1/65536) · length(p)` | superseded by `SafeSpawnPoint` below; kept as the reference behavior |
| `SafeSpawnPoint` (RTXMG `demo/shaders/self_intersection_avoidance.hlsli`, BSD-3) | rebuilds the hit point as `v0 + b1·e1 + b2·e2` with `precise` math, then offsets along the world geometric normal by a float error bound. The bound covers object-space reconstruction (`c0·abs(v0) + c1·extent`), object → world (`c1·abs(M3×3)·abs(pObj) + c2·abs(t)`) and world → object, with `c0 = 2^−24`, `c1 ≈ 3·2^−24`, `c2 ≈ 2^−23` | **every secondary origin.** Unlike `offsetRay`, the offset follows the actual float error of the triangle and transform: it is small near the origin and does not over-offset small, thin geometry. Inputs: the 3 fetched object positions, barycentrics, and the instance's object → world and world → object matrices. Port it to Slang with `precise`. On a raster surface, add the truncation bound of spec §10.2 (`2^(t−23) · max(abs(p))`) when CLAS positions are truncated |
| `pointOffset` (Hanika 2021, `:510`) | pushes `p` onto the smooth hull of the 3 vertex normals, in object space, then to world | shadow / visibility origins on smooth low-poly geometry (terminator artifacts). Needs the 3 object-space positions (fetched) and the vertex normals (already fetched for shading). Normals flipped with the facing |
| `minT` (`:570`) | `shadowRayMinT · (1 + dist(p, camera) · bias)` | visibility rays from raster surfaces; tune the two constants on Sponza in the C3 self-hit debug view (spec §10.4) |

The origin offset uses the **geometric** normal, never the shading normal.

**As built (PBR F2), raster surfaces:**
- **Why not `safeSpawnPoint`:** RTXDI's rays start from guide surfaces, which carry no traced triangle.
- **Rule:** `rasterRayOrigin` (`lighting/rtxdi_bridge.slang`) uses the `offsetRay` reference. It offsets along the guide's geometric normal on the ray's side, scaled by `max(|p|, viewDepth) / 65536`, with `p` from the `position` guide.
- **Check:** a temporary `ENGINE_RT_CHECK` run spawned its hemisphere ray this way. It found 0 self-hits over 227.5 M rays (cull check), 212.9 M (motion test) and 13.1 M (guide capture). Without the offset, about 45% of interior pixels hit their own triangle.
- `pointOffset` and `minT` stay with D4.

**As built (RT4):** `safeSpawnPoint(hit, direction)` in `geometry/hit.slang`, offsetting to `direction`'s side of the triangle. `ENGINE_RT_CHECK` spawns one cosine-weighted ray from every matched interior primary hit: 0 self-hits over about 385 million rays (cull check, motion test, guide capture). The truncation bound is not added, since positions are untruncated (§9.4). `pointOffset` and `minT` stay with D4.

### 6.2 Visibility rays

The sample traces shadow and AO rays through the pipeline with `TerminateOnFirstHit | SkipClosestHitShader` (+ `Opaque` when the scene has no alpha test) and a miss shader that clears the payload. The engine uses `RayQuery` (spec §11.1, plan D4):

- `RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES>`, one `Proceed()`, no candidate loop.
- **No `RAY_FLAG_FORCE_OPAQUE`**: it overrides the instance OMM state and removes alpha cut-outs. The instance flags already make non-alpha geometry opaque.
- No facing cull: a back face blocks light (closed meshes would leak otherwise). Same as the sample.
- Debug builds count any candidate returned by `Proceed()` and assert zero (plan D4).

**As built (PBR F2):** `traceVisibility(tlas, origin, direction, tMin, tMax)` in `raytracing/trace.slang`, with the flags above; `true` when nothing blocks. The debug candidate counter is still open (D4).

---

## 7. Ray cones (C3, G2)

From the sample (Improved Ray Cones, JCGT 2021), in `texturing.glsl` and `render_pathtrace.rgen.glsl`:

| Quantity | Formula | Where |
|---|---|---|
| Width at a hit | `w = w0 + spread · hitT` | `rchit:82`, `rgen:428` |
| Footprint in UV | `foot = w · texelDensity · texGradScale / sqrt(max(|dot(dir, geoN)|, 1e-4))` | `coneFootprintUV` (`texturing.glsl:53`); `sqrt` instead of the paper's linear grazing term, which over-blurs |
| Level | `log2(foot) + bias + 0.5 · log2(texW · texH)`, or gradients `(foot, 0)` / `(0, foot)` | `makeConeTexLOD` (`:61`), `sampleBindless` |
| Spread after a bounce | `spread += 0.3 · 2 · acos(1 − (1/pdf) / 2π)`, clamped to 2π | `rgen:618` |
| Camera rays | `w0 = 0`, `spread = view.pixelAngle` | `rgen:291` |

Engine mapping:

- `sampleMaterial` takes a `RayCone` and turns it into `SampleLevel` (one level for all slots, plus each texture's `0.5·log2(w·h)`) or `SampleGrad`. Prefer `SampleGrad` with the isotropic gradients: it reuses the resolve's sampling path and keeps anisotropic filtering consistent between raster and rays.
- Secondary rays start at a raster surface, not the camera: `w0 = pixelAngle · viewDistance`, `spread = pixelAngle` (plus the bounce term from the sampled lobe's pdf). `pixelAngle` is measured at **display** resolution when upscaling, matching the resolve's LOD bias `log2(render / display)`.
- Pass `w`, `spread` through the SER live state (two floats), not the payload.

**As built (RT5):**

- `sampleMaterial` takes no `RayCone`. `buildRaySurface` writes isotropic gradients `(foot, 0)` and `(0, foot)` per UV set into `Surface.uvDdx` / `uvDdy`, with `foot = w / sqrt(max(|dot(dir, geoN)|, 1e-4)) · sqrt(uvArea / worldArea)`, and `SampleGrad` runs the resolve's path unchanged.
- Camera rays start on the near plane: `w0 = spread · |origin − cameraPos|`, with `spread = pixelAngle · cos^1.5 θ`, where `pixelAngle = 2 / (|proj[1][1]| · height)` and `cos θ = near / |origin − cameraPos|`. A pixel at θ off the view axis subtends `pixelAngle² · cos³ θ`. With the sample's constant spread, the corner mips came out 0.58 levels too coarse; with the correction the largest gap from the raster footprint is 0.22 levels (Sponza, all `ENGINE_RT_CHECK` runs).
- The comparison skips triangles whose UV area is degenerate (both footprints below one texel of a 65536² texture). Both areas there are float noise, and both pick mip 0.

---

## 8. Alpha (D1–D4)

The sample's any-hit (`render_pathtrace_clusters.rahit.glsl`) samples the alpha mask with the same ray-cone footprint as shading. The engine replaces it with OMMs (spec §10.5):

- Baked per primitive with the OMM SDK CPU baker over the final cluster triangles; each CLAS gets its subrange as `opacityMicromapIndexBuffer`; the build sets `ALLOW_CLUSTER_OPACITY_MICROMAPS_BIT_NV`. C1 already writes `opacityMicromapIndexType = 0` (no OMM), so D2 only fills those fields.
- Instances: no opacity flag on alpha-tested primitives, `FORCE_OPAQUE` elsewhere, and the 2-state ray flag on every ray (next bullet). No any-hit, no candidate loops, no `RAY_FLAG_FORCE_OPAQUE` (§6.2).
- **Opacity per geometry instead of per instance.** RTXMG sets `VK_CLUSTER_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT_NV` (plus `CULL_DISABLE` for two-sided) in each CLAS's `baseGeometryIndexAndGeometryFlags`, from the cluster's or triangle's material (`rtxmg/include/rtxmg/cluster_lod/baked_geometry.h`), because its per-geometry BLAS mixes materials. The engine's BLAS is per primitive, so one material per instance row: the instance flag carries the same information, and the CPU templates can set it per row. Not taken. `CULL_DISABLE` is not needed either, since engine rays never cull facing (§6.2).
- **PTLAS (§3.6, required) has no 2-state instance flag.** Alpha-tested instances carry no flag. Every ray sets the 2-state ray flag (SPIR-V `RayFlagsForceOpacityMicromap2StateEXT`, `0x400`; Slang `RAY_FLAG_FORCE_OMM_2_STATE`). Opaque instances keep `FORCE_OPAQUE`, so the ray flag only affects OMM geometry. `traceVisibility` and `traceClosest` carry it since C4, and the SER trace has it from the start. Slang 2026.20 emits the flag without its capability; `requireOmm2StateRayFlag()` in `raytracing.trace` declares it (considerations C-C4-3), and the SER trace must call it too.
- What is lost: the sample's any-hit filters alpha at the ray-cone footprint, so distant foliage thins out smoothly. OMM 2-state is a fixed, footprint-independent cut. Accept it; revisit only if distant foliage aliases visibly in shadows.

---

## 9. After §16.3: LOD in the RT scene

None of this is built before the DAG exists. It is listed so the C1/C2 data model does not block it.

### 9.1 RT cut

- **Metric** (`traversal.glsl:217`, `testForTraversal`): `errorDistance = max(near, sphereDist − r · scale)`; a node is coarse enough when `maxQuadricError · scale / errorDistance ≥ threshold · errorScale`.
- **Instances failing visibility** get `errorScale = culledErrorScale` (default 2.0, `traversal_init.comp.glsl:222`). The engine applies it only **outside the current frustum with a margin**, never for Hi-Z occlusion (spec §10.3–10.4: RT runs in parallel with phase 2 and cannot see disocclusions).
- **Low-detail shortcut** (`traversal_init.comp.glsl:232–303`): when an instance's second-coarsest level already satisfies the metric, it is never traversed, and its TLAS instance points at a BLAS prebuilt at load from the coarsest level. Most distant instances cost nothing per frame. Build these with the C1 path (explicit, exact sizes).

### 9.2 Per-frame BLAS builds

- `CLUSTER_OPERATION_MODE_IMPLICIT_DESTINATIONS` into one per-frame BLAS buffer; `srcInfosCount` is a **GPU address** (`blasBuildCounter`), so the build count needs no readback (unlike the TLAS, §3.3); `dstAddressesArray` returns each BLAS address and `instance_assign_blas` writes them into the TLAS instances.
- Inputs: `maxClusterCountPerAccelerationStructure = min(maxPerGeometryClusters, maxRenderClusters)`, `maxTotalClusterCount = maxRenderClusters`; build count capacity = instances, or geometries × 2 with sharing + merging (plus cached builds). Capacities and overflow counters as for every GPU list.
- This changes C2's instance pass: it reads a build index per instance (low detail / own / shared / cached bits) instead of the static per-primitive address.

### 9.3 BLAS reuse ([blas_sharing.md](../../reference/vk_lod_clusters/docs/blas_sharing.md), [blas_merging.md](../../reference/vk_lod_clusters/docs/blas_merging.md), [blas_caching.md](../../reference/vk_lod_clusters/docs/blas_caching.md))

| Technique | Idea | Requirement |
|---|---|---|
| Sharing | classify each instance's LOD range from min/max proxies; a per-geometry histogram and an `atomicMax(lodMin << 27 \| instanceID)` pick one instance whose BLAS others reuse | instances of the same geometry with overlapping ranges |
| Merging | one BLAS per geometry with the highest resident detail among its instances | bounded builds: 2–3 per geometry per frame with sharing |
| Caching | keep a discrete-level BLAS across frames (copied out of the implicit build with `MOVE_OBJECTS`) | **CLAS addresses persistent across frames**: true for the engine's load-time CLAS |

Per-primitive BLAS (§2.1) multiplies build counts by primitives per mesh. Recheck the per-mesh layout when LOD lands.

### 9.4 CLAS memory

Host size estimates are 3–5× the real size (`docs/clas_allocation.md`). The C1 path (compute sizes, exact placement) already handles the resident case. The sample's GPU bit allocator over a sparse buffer is only needed for streaming, which is out of scope.

**Position truncation.** RTXMG bakes with "Pos Drop Bits" 7 and passes it as `positionTruncateBitCount`; `vk_lod_clusters` defaults to 0, and so does the engine (C1). With position fetch, RTXMG keeps no positions outside the CLAS, so for it truncation shrinks the only copy. The engine keeps full positions for raster, so the gain is CLAS size only. At `t = 7` the §10.2 origin bound is `2^−16 · max(abs(p))`, about 0.5 mm at 30 m from the model origin. This is the first lever for spec R7.

**Measured (2026-11-13, RTX 5070, driver 621.1, exact sizes from C1's compute-sizes pass, `ENGINE_GUIDE_CAPTURE` models):**

| Model | CLAS | t = 0 | t = 4 | t = 7 | t = 10 |
|---|---|---|---|---|---|
| Sponza | 3500 | 4.72 MiB | 4.10 MiB | 3.66 MiB (−22%) | 3.26 MiB (−31%) |
| guide model | 5205 | 8.16 MiB | 7.03 MiB | 6.22 MiB (−24%) | 5.56 MiB (−32%) |
| largest guide model | 23 896 | 22.84 MiB | 20.13 MiB | 17.76 MiB (−22%) | 15.36 MiB (−33%) |

BLAS sizes do not change. **Decision: stay at 0.** At 7 bits Sponza saves about 1 MiB. Against that, every primary-surface origin would carry the §10.2 bound, and raster and RT positions would stop matching bit for bit, which RT4's pixel-for-pixel check relies on. Revisit at Bistro scale under R7, after RT4 exists to check self-hits.

**Attribute compression (data point).** RTXMG stores UVs on a per-cluster power-of-two grid: 4 B per vertex plus 16 B per cluster, with a float fallback when the grid cannot hold them. Its normals are 4 B. With positions dropped, UVs dominate its geometry pool. This concerns the asset streams rather than RT, so it is recorded here only as an option for R7.

### 9.5 Bake and selection defaults (RTXMG `docs/ClusterLOD.md`)

| Parameter | RTXMG default | Engine note |
|---|---|---|
| Cluster size | 128 triangles / 128 vertices (RT only) | the engine's clusters also feed mesh shaders, so their size comes from the mesh-shader limits; keep one cluster set for both |
| Group size / node width | 32 clusters / 8 | as the engine spec's group size (≤ 32, `meshopt_partitionClusters`) |
| Simplification attribute weights | normal 0.5, texcoord 0.5, tangent 0, material 32 | start here. A texcoord weight of 0 lets UV-shredded coarse levels pass the error test. Material 32 keeps material borders, which per-primitive BLAS (§2.1) gets for free |
| LOD threshold | `2 · tan(fovY / 2) · lodPixelError / viewportHeight`, `lodPixelError = 1.0` | the same form; `viewportHeight` is the **display** height when upscaling |
| Adaptive error | +2% per frame above 85% pool occupancy, recovers below 70% | streaming only; the engine is resident (spec §10.2) |
| Culling | soft by default: off-screen **and** occluded geometry stays in the TLAS, coarsened by "culled error scale" | the engine keeps frustum only (§10); RTXMG's "hard (invisible)" mode, which drops off-screen instances, confirms that removing them breaks secondary rays |
| Low-detail BLAS | one cluster (`lowDetailClusterID`), prebuilt | as §9.1 |
| BLAS reuse | sharing, caching and merging all on; shared / cached tail levels 8 | as §9.3; the profiler block (builds per frame, instances per strategy) is the UI to copy |

### 9.6 Cluster templates and tessellation (`vk_tessellated_clusters`, RTXMG `docs/ClusterTess.md`)

Not planned: the engine has no subdivision surfaces or displacement. What carries over if they arrive, or for deformed meshes:

- **Templates.** A template is a CLAS treelet built once per topology (index buffer only, no positions). Each frame, `VK_NV_cluster_acceleration_structure` instantiates it with new positions. RTXMG reports a build cost close to a refit, with lower memory and steadier trace performance than refitting. It needs triangles within a cluster to be spatially close, which meshlets are. For skinned or vertex-animated meshes, build one template per meshlet at load and instantiate it with the deformed positions each frame, instead of rebuilding CLAS. Keep ≤ 128 triangles / vertices per template (sample recommendation).
- **BLAS addresses.** Per-frame BLAS builds with implicit destinations write `dstAddresses` straight into the instances' `accelerationStructureReference`. The temporary build inputs overlap the BLAS data memory, since they are dead once the build is done. Use the same trick for §9.2 scratch.
- **Mixed cluster sources.** Generated clusters tag the top 2 bits of `clusterID` to find their source triangles. If the engine ever mixes generated and baked clusters, reserve high bits of `meshletRow` the same way.
- **Shadow pops.** The sample skips tessellation for instances that are not primary-visible, and shadows from them pop. Its listed fix is to fade the rate by distance to the frustum planes, which is the engine's frustum-with-margin rule (§9.1, spec §10.4).

---

## 10. Where the engine deviates from the sample

| Topic | Sample | Engine | Reason |
|---|---|---|---|
| BLAS unit | per geometry | per mesh primitive | material per instance row; no LOD yet |
| Instances | uploaded once, BLAS address patched per frame | fully written on the GPU per frame | moving entities |
| TLAS | `ALLOW_UPDATE`, update after frame 0 | full build, `PREFER_FAST_TRACE` | motion degrades refits; measured switch (§3.4) |
| Queue | one queue | async compute + timeline semaphore | overlap with raster (spec X2) |
| Shading | closest-hit (raytrace) or raygen (pathtrace) | raygen only, after `ReorderThread` | SER (spec §11.1) |
| Visibility | trace with `SkipClosestHit`, miss shader | `RayQuery` | no SBT entry, no payload |
| Alpha | any-hit with cone-filtered alpha | OMM 2-state, no any-hit | spec §10.5 |
| RT LOD coarsening | frustum **or** Hi-Z failure | frustum only, with a margin | hybrid consistency (spec §10.4) |
| Ray origin offset | `offsetRay` (`length(p) / 65536`) | `SafeSpawnPoint` error bound (RTXMG) | offset follows the real float error (§6.1) |
| Hit position | `origin + dir · hitT` | fetched positions interpolated at the hit barycentrics | one surface builder with the resolve (§5.2) |
| Mirrored instances | shading normal snapped into the geometric hemisphere | no snap: `normalMatrix` keeps normals outward | same facing as raster `FlipWinding` (§5.2) |
| Camera-ray cone | `spread = pixelAngle` | `pixelAngle · cos^1.5 θ` | constant spread over-blurs off-axis pixels by about half a mip (§7) |
| CLAS memory | streaming + GPU allocator, or preloaded | preloaded only | spec §10.2 |

---

## 11. Steps

| Step | Plan | Do | Done when |
|---|---|---|---|
| RT1 | C1 follow-up | `ALLOW_DATA_ACCESS` on CLAS/BLAS builds (§2.3); log `accelerationStructureIndirectBuild` and `maxInstanceCount` at startup | **done 2026-11-13:** validation clean; sizes unchanged (§2.3); startup log `AS: indirectBuild=false maxInstanceCount=16777215` on the 5070 |
| RT2 | C2 | `rt_instances.slang` + per-slot instance/TLAS/scratch buffers; CPU-maintained ray-instance count and per-node prefix; capacity and overflow counters in the UI | **done 2026-11-13:** a temporary readback matched a CPU reference with 0 mismatches and transform error 0 (103 to 4199 instances); rows are per layout instead of a per-node prefix (§3.2); count and capacity in the UI, no overflow possible (§3.3) |
| RT3 | C2 | TLAS full build on the compute queue, timeline wait before the first ray stage; ownership barriers | **done 2026-11-13** on the 5070: sync validation clean, GPU-AV without errors; TLAS build 0.10–0.15 ms (§3.4); the graphics wait landed with RT4 (§3.5). Open: the Nsight overlap capture and the 2060 Super numbers |
| RT4 | C3 | `raytracing.hit` `decodeHit` sharing the resolve's attribute fetch; geometric normal, facing, mirror snap (§5.2); `SafeSpawnPoint` + truncation bound (§6.1) | a ray-query debug CS colors by `meshletRow` / `nodeInstanceIndex` and matches the vis-buffer views pixel for pixel away from silhouettes; closes C1's "traceable after unload" check (guide capture with model 0 erased). **Done 2026-11-13** on the 5070: `ENGINE_RT_CHECK` matched every interior pixel in 214 of 214 cull-check frames, 318 of 318 motion-test frames and 70 of 70 guide-capture frames. That excludes coverage within the rasterizer's vertex snap of an edge, and one coplanar depth tie. The decode at the resolve's barycentrics reproduces its normal, geometric normal and material normal exactly, and there are 0 self-hits. Validation clean (core, sync, best practices + NVIDIA) on the guide capture. Plan C3 has the details |
| RT5 | C3 | ray cones in `sampleMaterial` (§7) | a decoded primary hit gives the resolve's shading normal; mip level within ±0.5 of the resolve's on Sponza floor. **Done 2026-11-13:** shading normal exact (0.0000°) at the resolve's barycentrics; mip within 0.22 levels everywhere on Sponza, 0 pixels over 0.5 (§7) |
| RT6 | D4 | `traceVisibility` ray query (§6.2; **built with PBR F2**), `pointOffset` for shadow origins, `minT` bias | no terminator banding on a low-poly smooth-shaded mesh; self-hit debug view clean on Sponza |
| RT7 | G1 | RT pipeline (§4.1), SBT, `ClusterID` access (§4.2), indirect dispatch | a debug raygen reproduces RT4's view; SER on/off timings on the 5070 |
| RT8 | — | measure TLAS build vs update, `instanceCustomIndex = primitiveRow` vs extra load | numbers recorded here; decisions in §3.4 and §4.3 closed |
| RT9 | C4 (M2, required since 2026-11-14) | PTLAS (§3.6): extension required at device creation, one PTLAS updated in place, grid partitions, changed-instance ops from the instance pass, OMM ray flag in `traceVisibility` and `traceClosest`. The global partition was measured and dropped | **done 2026-11-14** on the 5070: `ENGINE_RT_CHECK` 318/294/154 of 318/294/154 frames; a still frame writes 0 instances; still update 0.015–0.019 ms, moving 0.12–0.18 ms against 0.10–0.15 ms full build (§3.6); validation clean, GPU-AV without errors. Open: Bistro, the 2060 Super |

---

## 12. Open questions

| # | Question | Resolved by |
|---|---|---|
| R1 | Does `VK_NV_cluster_acceleration_structure` exempt CLAS from `ALLOW_DATA_ACCESS` for position fetch? | reference page has no exemption (2026-11-13); RT1 sets the flag at no measured cost |
| R2 | Cluster AS and position fetch on the 2060 Super (spec Q7) | `vulkaninfo` on that machine |
| R3 | Slang closest-hit intrinsic for `ClusterID` | ray queries resolved (RT4): `CommittedClusterID()` emits `OpRayQueryGetIntersectionClusterIdNV`. Closest-hit: SER4 compile probe |
| R4 | Per-primitive vs per-mesh BLAS once LOD exists | build counts with sharing/merging on Sponza (§9.3) |
| R5 | PTLAS with frames in flight: is an in-place update (src = dst) allowed, or does it need one PTLAS per slot (§3.6)? | **resolved 2026-11-14:** in place is allowed (VUID-10549 forbids overlap only when src ≠ dst; the sample updates in place). One PTLAS, with a wait on the previous frame's ray passes (§3.6) |
| R6 | `positionTruncateBitCount` 7 (RTXMG default): CLAS bytes saved, and does the §10.2 origin bound hold in the RT4 self-hit view? | **memory measured 2026-11-13:** −22% CLAS at 7 bits (§9.4); kept at 0. At 0 bits, RT4 sees 0 self-hits (§6.1). The self-hit test at 7 bits is not run; it only matters if R7 asks for the saving |
