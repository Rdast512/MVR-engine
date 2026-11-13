# Feature: ray tracing with RTX Mega Geometry (patterns from `vk_lod_clusters`)

**Date:** 2026-11-13 · **Status:** C1 built (CLAS + cluster BLAS per primitive), RT1 done; C2, C3 and the RT pipeline not started · **Milestone:** M2 (C2, C3), M4 (RT pipeline, SER), M6 (OMM), post-1.0 (LOD, §16.3)
**Implements:** engine spec §10 (RT scene), §11.1 (ray model); plan C2, C3, D4, G1 ([pbr_implementation_plan.md](../pbr_implementation_plan.md))
**Related:** [shader_execution_reordering.md](./shader_execution_reordering.md), [visibility_buffer.md](./visibility_buffer.md), sample [README](../../reference/vk_lod_clusters/README.md) and [docs/](../../reference/vk_lod_clusters/docs/)

This document maps the NVIDIA sample `reference/vk_lod_clusters` onto the engine: what it does for ray tracing, which parts the engine takes as they are, which parts it changes, and in what order to build them. Sample paths are relative to `reference/vk_lod_clusters/`. Line numbers are from the vendored copy and are given for orientation only.

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
| `flags` | `FORCE_OPAQUE`, or `FORCE_OPACITY_MICROMAP_2_STATE_EXT` for alpha-tested primitives | spec §10.5. **No** facing flags: RT handles negative determinants itself (`renderer_raytrace_clusters_lod.cpp:1391`), and two-sided needs no flag when rays do not cull back faces |
| `accelerationStructureReference` | the primitive's BLAS address from the per-model table; **0** for a primitive without meshlets | an instance with reference 0 is inactive (Vulkan spec), so it costs nothing at trace time |

The same thread writes `GpuRayInstance[row] = {nodeInstanceIndex, primitiveRow}`. Rows map 1:1 to TLAS instances, so `InstanceIndex()` indexes `GpuRayInstance` directly.

**Row assignment.** The thread needs (node instance, primitive) from its row. Keep a per-node-instance prefix (`firstRayInstance`) maintained by the CPU on scene deltas (O(changed)), and give each thread its row by a binary search over the prefixes, or dispatch one workgroup per node instance with threads over its primitives. The second needs no search and suits the sample's per-instance threading; pick it unless primitive counts per node vary so much that occupancy suffers.

### 3.3 Instance count without a GPU readback

The TLAS build range (`primitiveCount`) is a host value. A GPU-written count needs `vkCmdBuildAccelerationStructuresIndirectKHR`, which requires `accelerationStructureIndirectBuild`, a feature the RTX 5070 on driver 621.1 does not expose (logged at startup: `AS: indirectBuild=false`).

- The CPU knows the exact ray-instance count: Σ primitives over live node instances, updated on scene deltas. Pass it as `primitiveCount`.
- Size the instance buffer, TLAS and scratch for a capacity (`vkGetAccelerationStructureBuildSizesKHR` with the capacity count, bounded by `maxInstanceCount`), with a counter and an overflow counter in the debug UI (spec S11). Grow when the count exceeds it, outside frames in flight.
- Slots that should not be traced (hidden entity, no meshlets) get reference 0 instead of compaction. The sample does the same for culled instances after frame 0.

### 3.4 Build or update

| | Sample | Engine |
|---|---|---|
| Flags | `ALLOW_UPDATE` | `PREFER_FAST_TRACE` |
| Mode | full build in frame 0, then `MODE_UPDATE` with the previous TLAS as source (`:1448`) | full build every frame (spec §10.3) |
| Why | static instances; only BLAS references change | entities move; a refit tree degrades with motion and every count change needs a rebuild anyway |

Measure on the 5070 and the 2060 Super: full build vs update, Sponza ×N. Switch to update only if the build exceeds 0.3 ms and the trace cost of a refit tree stays within noise; above that, the spec's next step is PTLAS (`VK_NV_partitioned_acceleration_structure`, already detected as optional).

### 3.5 Resources and sync

- Per frame slot: instance buffer (`AS_BUILD_INPUT_READ_ONLY | SHADER_DEVICE_ADDRESS | STORAGE`), `GpuRayInstance[]`, TLAS storage, scratch aligned to `minAccelerationStructureScratchOffsetAlignment`. `GpuFrameRoot.tlas` and `.rayInstances` point at the slot's copies.
- Queue: `Device::computeQueue` with its own command pool (a command buffer goes only to its pool's family). Graphics waits on the compute timeline value before the first stage that traces, with `dstStageMask` = that stage (compute for ray queries, `RAY_TRACING_SHADER` for the SER pipeline).
- Queue-family ownership: instance and TLAS buffers written on compute and read on graphics are either `EXCLUSIVE` with release/acquire barriers, or `CONCURRENT` when the two families differ (`VUID-VkBufferCreateInfo-sharingMode-00914` when they do not).
- BLAS are read-only after load and outlive every frame that references them: model unload waits for the frames in flight that traced it (the same rule as the other model slices).

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

The sample's path-trace closest-hit (`render_pathtrace_clusters.rchit.glsl`) writes `{hitT, instanceID, clusterID, triangleID, barycentrics}` plus either the 3 fetched positions or the world geometric normal (`PATHTRACE_HIT_POSITIONS`). The engine's SER design ([shader_execution_reordering.md](./shader_execution_reordering.md) §3) writes `{clusterId, triangle, barycentrics, hitT}` (24 B) and takes the positions from `HitObject::GetTriangleVertexPositions` in raygen, which keeps the payload small across `ReorderThread`.

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

### 5.2 Geometry at the hit

Taken from `getHitAttributes` as is:

1. **World position** = `origin + dir · hitT`. It matches the depth the ray reports. Do not interpolate the fetched positions for it.
2. **Geometric normal** from the fetched positions: `oGeo = cross(p1 − p0, p2 − p0)`, world `normalize(oGeo · worldMatrixI)` (inverse transpose). The fetched positions are the CLAS ones (truncated when `positionTruncateBitCount > 0`), so the normal matches the surface the ray hit.
3. **Facing**: `backFacing = dot(oGeo, objectRayDir) > 0`. On a back-face hit, negate the shading normal **and** the geometric normal together.
4. **Mirrored instances**: the inverse-transpose normal and the cross-product normal disagree in sign under a negative determinant. Snap the interpolated shading normal into the geometric hemisphere (`dot(wNormal, wGeo) < 0 → −wNormal`) before the facing flip.
5. **Tangent**: interpolate, transform by `worldMatrixI`, normalize; the sign comes from vertex 0. The engine's A4 tangents slot in here.
6. **Shading-normal sanity** (sample's closest-hit variant): when `reflect(dir, N)` points below the geometric surface, bend `N` toward the geometric normal. Apply it in `buildSurface` only for rays, not the resolve, or the C3 resolve comparison fails by construction.

### 5.3 Texture LOD

The hit computes `texelDensity = sqrt(uvArea / worldArea)` per triangle (`computeTexelDensity`, `texturing.glsl:159`, from the fetched positions and the instance matrix) for the ray cone (§7).

---

## 6. Ray origins and visibility rays (C3, D4)

### 6.1 Offsets

| Function | Rule | Engine use |
|---|---|---|
| `offsetRay(p, dir, geoN)` (`render_shading.glsl:490`) | `n = sign(dot(dir, geoN)) · geoN`; `p + n · (1/65536) · length(p)` | every secondary origin. Add the truncation bound of spec §10.2 (`2^(t−23) · max(abs(p))`) on top when CLAS positions are truncated and the origin is a raster surface |
| `pointOffset` (Hanika 2021, `:510`) | pushes `p` onto the smooth hull of the 3 vertex normals, in object space, then to world | shadow / visibility origins on smooth low-poly geometry (terminator artifacts). Needs the 3 object-space positions (fetched) and the vertex normals (already fetched for shading). Normals flipped with the facing |
| `minT` (`:570`) | `shadowRayMinT · (1 + dist(p, camera) · bias)` | visibility rays from raster surfaces; tune the two constants on Sponza in the C3 self-hit debug view (spec §10.4) |

The origin offset uses the **geometric** normal, never the shading normal.

### 6.2 Visibility rays

The sample traces shadow and AO rays through the pipeline with `TerminateOnFirstHit | SkipClosestHitShader` (+ `Opaque` when the scene has no alpha test) and a miss shader that clears the payload. The engine uses `RayQuery` (spec §11.1, plan D4):

- `RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES>`, one `Proceed()`, no candidate loop.
- **No `RAY_FLAG_FORCE_OPAQUE`**: it overrides the instance OMM state and removes alpha cut-outs. The instance flags already make non-alpha geometry opaque.
- No facing cull: a back face blocks light (closed meshes would leak otherwise). Same as the sample.
- Debug builds count any candidate returned by `Proceed()` and assert zero (plan D4).

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

---

## 8. Alpha (D1–D4)

The sample's any-hit (`render_pathtrace_clusters.rahit.glsl`) samples the alpha mask with the same ray-cone footprint as shading. The engine replaces it with OMMs (spec §10.5):

- Baked per primitive with the OMM SDK CPU baker over the final cluster triangles; each CLAS gets its subrange as `opacityMicromapIndexBuffer`; the build sets `ALLOW_CLUSTER_OPACITY_MICROMAPS_BIT_NV`. C1 already writes `opacityMicromapIndexType = 0` (no OMM), so D2 only fills those fields.
- Instances: `FORCE_OPACITY_MICROMAP_2_STATE_EXT` on alpha-tested primitives, `FORCE_OPAQUE` elsewhere. No any-hit, no candidate loops, no `RAY_FLAG_FORCE_OPAQUE` (§6.2).
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
| CLAS memory | streaming + GPU allocator, or preloaded | preloaded only | spec §10.2 |

---

## 11. Steps

| Step | Plan | Do | Done when |
|---|---|---|---|
| RT1 | C1 follow-up | `ALLOW_DATA_ACCESS` on CLAS/BLAS builds (§2.3); log `accelerationStructureIndirectBuild` and `maxInstanceCount` at startup | **done 2026-11-13:** validation clean; sizes unchanged (§2.3); startup log `AS: indirectBuild=false maxInstanceCount=16777215` on the 5070 |
| RT2 | C2 | `rt_instances.slang` + per-slot instance/TLAS/scratch buffers; CPU-maintained ray-instance count and per-node prefix; capacity and overflow counters in the UI | instances match a CPU reference dump for Sponza ×2 with one entity yawing; no BLAS rebuilds |
| RT3 | C2 | TLAS full build on the compute queue, timeline wait before the first ray stage; ownership barriers | sync validation clean; Nsight shows the build overlapping raster; build time logged on both GPUs |
| RT4 | C3 | `raytracing.hit` `decodeHit` sharing the resolve's attribute fetch; geometric normal, facing, mirror snap (§5.2); `offsetRay` + truncation bound | a ray-query debug CS colors by `meshletRow` / `nodeInstanceIndex` and matches the vis-buffer views pixel for pixel away from silhouettes; closes C1's "traceable after unload" check (guide capture with model 0 erased) |
| RT5 | C3 | ray cones in `sampleMaterial` (§7) | a decoded primary hit gives the resolve's shading normal; mip level within ±0.5 of the resolve's on Sponza floor |
| RT6 | D4 | `traceVisibility` ray query (§6.2), `pointOffset` for shadow origins, `minT` bias | no terminator banding on a low-poly smooth-shaded mesh; self-hit debug view clean on Sponza |
| RT7 | G1 | RT pipeline (§4.1), SBT, `ClusterID` access (§4.2), indirect dispatch | a debug raygen reproduces RT4's view; SER on/off timings on the 5070 |
| RT8 | — | measure TLAS build vs update, `instanceCustomIndex = primitiveRow` vs extra load | numbers recorded here; decisions in §3.4 and §4.3 closed |

---

## 12. Open questions

| # | Question | Resolved by |
|---|---|---|
| R1 | Does `VK_NV_cluster_acceleration_structure` exempt CLAS from `ALLOW_DATA_ACCESS` for position fetch? | reference page has no exemption (2026-11-13); RT1 sets the flag at no measured cost |
| R2 | Cluster AS and position fetch on the 2060 Super (spec Q7) | `vulkaninfo` on that machine |
| R3 | Slang closest-hit intrinsic for `ClusterID` | SER4 compile probe |
| R4 | Per-primitive vs per-mesh BLAS once LOD exists | build counts with sharing/merging on Sponza (§9.3) |
