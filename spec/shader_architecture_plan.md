# Shader architecture plan

**Date:** 2026-10-01, updated 2026-11-20
**Status:** plan, aligned with [engine_spec.md](../docs/engine_spec.md) (normative; its §4 rules S1–S11 and §5 frame order win over this doc). M1–M4 are built on the RTX 5070: culling, Hi-Z, vis-buffer raster, resolve, cluster RT + PTLAS, ReSTIR DI 3.2, compute indirect bounce, NRC, ReSTIR GI, combine, indirect modes, DLSS-RR and post exist; SER RT pipelines were shelved to post-1.0 V1. This doc maps the spec onto shader files and passes. Append changes to [Changelog](#changelog).
**Related:** [resource_architecture.md](./resource_architecture.md) (buffers, model slices, frame root, handle vs BDA), [pbr_implementation_plan.md](./pbr_implementation_plan.md) (shading tasks A–H; the spec's §17 lists which of its decisions are superseded), [culling_comparison.md](../reference/culling_comparison.md) (culling library), [vulkan_extensions_reference.md](../docs/vulkan_extensions_reference.md), [shaders/README.md](../shaders/README.md)

What every shader file is for, which pass runs it, and what it reads and writes. The frame is built around:
- **Visibility buffer** for primary visibility: GPU culling, mesh shaders through DGC, two-phase Hi-Z occlusion (spec §8)
- **Compute material resolve** with analytic derivatives, binned by material shader through a DGC compute execution set (spec §9)
- **Cluster ray tracing** with RTX Mega Geometry: CLAS per cluster, cluster BLAS, PTLAS from GPU-written ops (spec §10)
- **Compute ray queries** for all rays in 1.0; **RT pipelines built around SER** shelved to post-1.0 V1 (spec §11.1, §16.5)
- **RTXDI ReSTIR DI** and **ReSTIR GI** (1.0 default), plus **NRC** radiance cache as a runtime comparison switch (spec §11.7)
- **Opacity micromaps (OMM)** for alpha-tested rays, no any-hit (spec §10.5)
- **DLSS Ray Reconstruction** for denoising and upscaling in one step (spec §12)

Target hardware is **NVIDIA RTX 20 and newer**, one code path (spec §1.2, §2).

Legend: **[exists]** has real code today · **[stub]** placeholder file · **[new]** to be created · **[delete]** to be removed. Milestones (M0–M10, L1–L3) are the spec's §16.

---

## 1. Ground rules

The spec's S1–S11 apply; shader-side consequences:

- **One path, no fallbacks.** No vendor checks, raster-only mode or old-technique toggles. Tiers change resolution, ray counts, cache choice and LOD error only (spec P1).
- **Slang modules.** Library code is `module x;` / `import a.b;` (S2). Vendor HLSL (RTXDI, NRC) is `#include`d only from bridge and pass files.
- **Bindless only.** Buffers through typed BDA pointers (`bdaPtr<T>`), read once into locals; images and samplers through heap handles (S3, S4). No descriptor-set bindings, no `StructuredBuffer` for engine data.
- **One root pointer.** Every pass reaches scene data from `GpuFrameRoot`; push data carries the root address plus pass-local values, within `maxPushDataSize` (S7). Render-target heap indices live in the per-frame `GpuTargets` block reached from the root (M0).
- **Rays: compute ray queries in 1.0, SER RT pipelines post-1.0 V1.** In 1.0 every pass shades hits from compute ray queries (`traceClosest` → `decodeHit` → `sampleMaterial`, shared with the vis-buffer resolve). The SER RT pipeline shape (`HitObject::TraceRay` → `ReorderThread(hit, materialBin)` → minimal closest-hit → shade in raygen; one raygen, one miss, one closest-hit per SBT) was shelved on 2026-11-17 (considerations C-G1-3) and returns as a toggle in post-1.0 V1 (spec §11.1, §16.5). No any-hit anywhere.
- **Same ids everywhere.** A vis-buffer pixel and a ray hit both resolve to `(nodeInstance, cluster, triangle)`; the cluster id is the model-slice `meshletRow` (spec §8.3, §10.1).
- **Texture LOD.** No implicit-LOD `Sample` outside fragment shaders: compute resolve uses analytic gradients (`SampleGrad`), ray hits use ray cones (S10). LOD bias `log2(render / display)`.
- **Two resolutions.** Everything up to combine runs at render resolution (indirect at half resolution, checkerboard, on the floor tier); DLSS-RR outputs display resolution; post runs there.
- **Jitter and motion.** The render projection is jittered (Halton 2,3). Culling, Hi-Z and LOD use the unjittered matrix; exact pixel tests use the jittered one or widen by the jitter (spec §12.2). Motion vectors exclude jitter, in render-resolution pixels.
- **HDR until tonemap.** Tonemap exactly once, in post. Guides are linear, never sRGB-encoded.
- **Specialization by generics and link-time constants** (S9), not preprocessor variants, except vendor-mandated defines (`NRC_UPDATE`, `NRC_QUERY`). Device-derived sizes (cluster limits, list capacities) are link-time constants (S11).
- **Handle vs address.** `kNoneIndex` never becomes a handle; non-uniform heap indices go through `sampleSlot` (S6, PBR plan §4.4).
- **Every mirrored struct is checked on both sides** (S5): C++ `static_assert`s on size and offsets, Slang mirrors in `types.slang`, scalar layout, column-major.

### 1.1 Binding model

Implemented; the full description is [resource_architecture.md](./resource_architecture.md) §6–7. Summary:

Every loaded model owns one slice of each model-sliced asset buffer. Unloading compacts the buffers, so **later slices, their addresses and entity ids shift on every unload**. Data inside a slice is relative to its model; tables the CPU rewrites are absolute.

```
GpuFrameRoot (host-visible, one per frame slot, rewritten every frame)
├─ camera          : GpuCameraData*
├─ instances       : GpuObjectUB[entity]           lights place themselves with it
├─ models          : GpuModel[model]               slice addresses incl. meshletBounds, meshletGroups, meshletPrimitives, primitives
├─ primitives      : GpuPrimitive[]                absolute rows, model-relative values
├─ materials, pbrExtensions, lights (+ lightCount, localLightCount): model space, point and spot first
├─ worldLights     : GpuLight[lightCount]          world space, written by light_world (async compute)
├─ prevWorldLights : GpuLight[lightCount]          last frame's worldLights; 0 on a history reset (F2)
├─ nodeInstances   : GpuNodeInstance[entity × node] world + previous world matrix
├─ drawRecords     : GpuDrawRecord[]               cull-internal work items
├─ visibleMeshlets : GpuVisibleMeshlet[]           {nodeInstanceIndex, meshletRow}; lateVisibleBase
├─ cullCounters, triangleCull, subpixelSnap, debugView, frameFlags
├─ targets         : GpuTargets*                    heap indices of every render target (208 B)
├─ frameIndex, exposure
├─ tlas            : uint64                         PTLAS address (M2 C4)
├─ rayInstances    : GpuRayInstance[]               (M2 C2)
├─ rtxdiConstants  : GpuRtxdiConstants*            DI + GI blocks, reservoirs, neighbor offsets (496 B, F1/G5); read through lighting/rtxdi_bridge
├─ cacheConstants  : GpuNrcConstants*              NRC constants and buffers (168 B, M4 G3; zeroed when NRC is paused)
│  planned (spec §6.2):
└─ emissive light lists, material bins (M7)
```

Lookup chain of every raster shader (`loadDrawContext`): push `{frameRoot, firstVisible, meshletCount}` → visible entry → node instance → model → `meshletPrimitives[meshletRow]` → primitive → material.

---

## 2. Frame → passes

The spec's §5 frame, stage by stage, with the shader that runs it.

| # | Stage | Pass / entry | Queue | Status | Milestone |
|---|---|---|---|---|---|
| 1 | Scene update: transforms → `GpuNodeInstance`, previous kept | `scene_update` | graphics | [new]; CPU writes node instances today | §8 open decision 1 |
| 2 | Phase 1 cull: instances → groups → clusters (frustum, cone, sub-pixel, previous Hi-Z) | `meshlet_cull`: `cullInstances`, `cullMeshlets`, `buildMeshSequences` | graphics | [exists] | — |
| 3 | Phase 1 raster: vis buffer + depth | `mesh_raster` + `mesh_fragment` via DGC | graphics | [exists] | M1 |
| 4 | Hi-Z build | `hiz_build` | graphics | [exists], `nvhiz`, 3 levels per dispatch | M1 |
| 5 | Phase 2 cull: candidates and reject masks vs fresh Hi-Z | `cullInstances` (late), `cullRejectedMeshlets`, `buildMeshSequences` | graphics | [exists] | — |
| 6 | Phase 2 raster | as 3 | graphics | [exists] | M1 |
| 7 | Hi-Z build for the next frame | as 4 | graphics | [exists] | — |
| 6' | World lights; RT instance writes → PTLAS update in place (PBR C4) | `light_world`, `rt_instances` | async compute | [exists] | M2, M3 (E2) |
| 8 | Classify: vis buffer → material tiles, bin-id image | `classify` | graphics | [new] | M7 |
| 9 | Resolve + shade → shading surface + DLSS guides | `resolve` (one tiled dispatch until M7, then one shader per bin through a DGC execution set) | graphics | [exists], one tiled dispatch | M1, M7 |
| 10 | ReSTIR DI: initial → temporal → spatial → visibility | `rtxdi_presample` (M7), `rtxdi_initial`, `rtxdi_temporal`, `rtxdi_shade` (spatial resampling + shading) | graphics | [exists] except `rtxdi_presample` | M3; split for RTXDI 3.2 |
| 11 | NRC + indirect | `nrc_update` (`NRC_UPDATE=1`), `indirect` (`NRC_QUERY=1`), `QueryAndTrain` (host), `nrc_resolve` (compute + ray query; NRC passes skipped in the 1.0 ReSTIR GI default, spec §11.7) | graphics | [exists] | M4 |
| 12 | ReSTIR GI: temporal + spatial + final shade | `rtxdi_gi_temporal`, `rtxdi_gi_shade` | graphics | [exists] | M4 |
| 13 | Combine: direct + indirect + emissive | `combine` | graphics | [exists] | M4 (H1 direct + emissive; G6 adds indirect) |
| 14 | DLSS-RR | host NGX | graphics | — | M3 |
| 15 | Post: auto exposure → tonemap → post target; `present` → swapchain, ImGui | `exposure` (histogram, adapt), `post` | graphics | [exists] (H2) | M3 |

Debug passes: `debug_view` [exists] (geometry and culling views from the vis buffer, every guide, `Unshadowed lights`, `Shadowed lights`, `Direct lighting`, `DLSS input`, `RTXDI reservoirs`, `Indirect samples`, `NRC cache`, `Indirect radiance`), `hiz_view` [exists], ray views in `debug_view` [exists, M2] (`Ray meshlets`, `Ray node instances`, `Ray shading normal`; they replace the planned `rt_debug`; primary self-hit distance after L2). `rt_check` [exists] compares primary rays with the vis buffer after the debug view, only under `ENGINE_RT_CHECK`. The ray passes run in a second graphics command buffer that waits for the compute timeline. `mesh_debug` was deleted in M1. `motion_check` [exists] counts motion-vector errors after the debug view, only under `ENGINE_MOTION_TEST` ([motion_vectors.md](./features/motion_vectors.md) §5.1). Output: `present` [exists] samples the post target (Lit) or display image (debug views) into the swapchain, then ImGui.

---

## 3. Directory layout

Library files become Slang modules in M0 (PBR A2). Vendor headers are textual includes, so bridge files stay `#include`d. Entry points live in `passes/`, one compile unit per pass (`engine_add_slang_pass` in `CMakeLists.txt`).

| Path | Status | Purpose |
|---|---|---|
| `types.slang` | [exists] | POD mirrors of `src/core/types.hpp` and `push_data.hpp`, checked field by field by `tests/layout_test.cpp`; includes `GpuTargets`, `GpuRayInstance` and the M0 frame root fields. Morph types removed (2026-11-08) |
| **core/** | | |
| `core/binding.slang` | [exists] | `tex2D`, `uintTex2D`, `storageTex2D<T, format>` (typed storage images), `samplerOf`, `bdaPtr`, `CheckedBuffer<T>` (bounds-checked BDA reads). Non-uniform material fetches go through `sampleSlot` in `material/pbr.slang` (S6) |
| `core/frame.slang` | [exists] | `loadDrawContext`, `loadCamera`, `loadMaterial`, `loadMaterialExtension`, `toWorldLight`, `loadWorldLight` (this frame's world rows) |
| `core/culling.slang` | [exists] | modified port of `vk_lod_clusters` `culling.glsl` (Apache-2.0 header kept): `intersectFrustum`, `isHiZOccluded`, `isSubPixel`, `isFrontFacingHW`, `testTrianglePixel`; reverse Z only |
| `core/math.slang` | [exists] | `objectToClip`, `jitterClip`, `pixelToNdc`, `normalMatrix`, ONB, octahedral encode / decode, `luminance`. To do: PCG RNG, Halton |
| `core/counters.slang` | [exists] | `waveCount`, `waveMax`: one atomic per wave into host-read check stats (`motion_check`, `rt_check`) |
| **geometry/** | | |
| `geometry/mesh.slang` | [exists] | meshlet, vertex and triangle fetch through model slices; `loadFrontFacingTriangle` (winding flip) |
| `geometry/triangle_cull.slang` | [exists] | homogeneous backface + small-primitive test (`SV_CullPrimitive`) |
| `geometry/visbuffer.slang` | [exists] | pack/unpack the 32-bit vis-buffer payload (`visibleRow << kVisTriangleBits \| triangle`); analytic barycentrics and screen derivatives (M1, [visibility_buffer.md](./features/visibility_buffer.md)) |
| `geometry/surface.slang` | [exists] | triangle fetch and attribute interpolation from `(nodeInstance, meshletRow, triangle)`, shared by the resolve (M1) and `decodeHit` (M2) |
| `geometry/hit.slang` | [exists] | `primaryRay`, `traceClosest` (ray query), `decodeHit`: `(instance, cluster, triangle)` → `DrawContext` + `Surface` through `geometry/surface`; positions by position fetch; ray-cone gradients; `safeSpawnPoint` (M2) |
| **material/** | | |
| `material/pbr.slang` | [exists] | `SampledMaterial` + `sampleMaterial` with explicit gradients, used by the resolve and later by ray hits (B1) |
| `material/brdf.slang` | [exists] | glTF GGX `evalBrdf`, `sampleBrdf` (VNDF), `pdfBrdf`, `envBrdfApprox` (B2; `tests/brdf_test.cpp`) |
| **lighting/** | | |
| `lighting/lights.slang` | [exists] | `LightSample`, `evalLight` for point, spot, directional (B3; `tests/lights_test.cpp`). Emissive triangles (M7) go through RTXDI's triangle-light path, not `evalLight` |
| `lighting/path_shading.slang` | [exists] | secondary-hit decode and direct-light shading shared by `indirect` and `nrc_update`; textual include (M4 G2) |
| `lighting/rtxdi_bridge.slang` | [exists] | RTXDI resource macros + `RAB_*` over the guide sets and world lights, `shadeLightSample`, `rasterRayOrigin`, ReSTIR GI reservoir accessors; bounds-checked reservoir and light reads, which RTXDI expects from robust structured buffers (F3, G5); textual include, units compile with `RTXDI_SLANG_FLAGS` (F2) |
| `lighting/rtxdi_constants.slang` | [exists] | `GpuRtxdiConstants` (496 B) mirror next to RTXDI's C parameter structs (DI + GI); textual (F2, G5) |
| **gi/** | | |
| `gi/nrc_bridge.slang` | [exists] | NRC buffer accessors + `NrcConstants` bridge over BDA; textual include, called by `indirect`, `nrc_update` and `nrc_resolve` (M4 G3) |
| `gi/nrc_constants.slang` | [exists] | `GpuNrcConstants` (168 B) mirror matching `src/render/vk_nrc.hpp`; textual (M4 G3) |
| **raytracing/** | | |
| `raytracing/ser.slang` | [shelved, C-G1-3] | `traceShaded` (`HitObject::TraceRay` + `ReorderThread`) and minimal closest-hit reading `ClusterIDNV`; kept in tree, not built (returns post-1.0 V1) |
| `raytracing/trace.slang` | [exists] | ray-query `traceVisibility` and `requireOmm2StateRayFlag()` (`RAY_FLAG_FORCE_OMM_2_STATE`, C-C4-3) |
| **passes/** | | see §5 |

---

## 4. Render targets (render resolution unless noted)

Normative in spec §8.3 and §9.3. Two sets of the surface and guides ping-pong by frame parity for temporal resampling.

| Resource | Format | Written by | Read by |
|---|---|---|---|
| Depth | D32F, reverse Z from M1 (clear 0, `GREATER`) | raster | Hi-Z, RTXDI, ReSTIR GI, DLSS (hardware depth) |
| Hi-Z pyramid | R32F, full mip chain, min reduction from M1 | `hiz_build` | cull passes |
| Visibility buffer | `R32_UINT`: visible-list row, triangle; no clear, empty pixels decided by depth | raster | `classify`, `resolve`, `debug_view` |
| Bin-id image | R8 or R16 | `classify` | per-bin `resolve` overlap guard |
| `normalRoughness` | RGBA16F: world N, perceptual roughness | `resolve` | RTXDI, paths, DLSS |
| `geoNormal` | RG16 snorm, octahedral | `resolve` | ray origin offset |
| `diffuseAlbedo`, `specularF0` | RGBA8; a = ao, specular weight (0: unlit) | `resolve` | RTXDI, paths, DLSS diffuse albedo |
| `specAlbedo` | RGBA8, env-BRDF approximation | `resolve` | DLSS specular albedo |
| `emissive` | R11G11B10F | `resolve` | `combine` |
| `motion` | RGBA16F, render-res pixels, no jitter; z = view depth change | `resolve` | DLSS, ReSTIR temporal |
| `position` | RGBA32F: world position, w = view depth | `resolve` | RTXDI surfaces and ray origins (both sets) |
| `specHitDistance` | R16F, optional, full render resolution after ReSTIR GI | `rtxdi_gi_shade` | DLSS (only if measured useful) |
| `directRadiance` | RGBA16F | `rtxdi_shade` (zeros with no lights) | `combine`; `debug_view` (`Direct lighting`) |
| `indirectRadiance` | RGBA16F | `rtxdi_gi_shade` | `combine`; `debug_view` (`Indirect radiance`) |
| `noisyColor` | RGBA16F HDR | `combine` | DLSS-RR |
| Output color | RGBA16F, display resolution | DLSS-RR | `exposure`, `post` |
| Exposure | R32F, 1×1 | `exposureAdaptMain` | `post`; next frame's DLSS-RR |
| Post color | RGBA16F, display resolution, tonemapped | `post` | `present` (Lit) |

Buffers (BDA, [resource_architecture.md](./resource_architecture.md) §12):
- **Scene:** asset slices through `GpuModel`; materials, lights, primitives, node instances through the frame root.
- **Culling:** cull views, counters, draw records, candidates, reject masks, visible lists, DGC sequences.
- **Ray tracing:** CLAS, cluster BLAS, PTLAS and its per-instance state rows, per-slot `VkPartitionedAccelerationStructureWriteInstanceDataNV[]` and op entry, `GpuRayInstance[]` + `GpuRayInstanceBuild[]`, OMM arrays and per-cluster OMM index buffers.
- **Lighting:** RTXDI reservoirs and light buffer, emissive-triangle list, NRC SDK buffers, ReSTIR GI samples, per-SDK constants.
- **Binning:** per-bin tile lists and counts, DGC compute sequences.

---

## 5. Passes

| Pass (`passes/`) | Status | Stage | Reads | Writes | Milestone |
|---|---|---|---|---|---|
| `meshlet_cull` | [exists] | compute | frame root, cull view, model slices, Hi-Z | draw records, candidates, reject masks, visible lists, DGC sequences, counters | A10 in M0; bins (A6) in M1 |
| `hiz_build` | [exists] | compute | depth, previous mip | up to 3 pyramid mips per dispatch (`nvhiz`), min reduction | M1 |
| `hiz_view` | [exists] | mesh + fragment | pyramid | display image | — |
| `mesh_raster` + `mesh_fragment` | [exists] | mesh + fragment, DGC | visible list, model slices, material flag | vis-buffer ids only, opaque bin, two-sided by winding flip. M6: alpha-tested fragment with `discard` | M1, M6 |
| `present` | [exists] | fullscreen mesh + fragment | `displayColor` | swapchain, then ImGui | M1; `composite` output from M3 |
| `scene_update` | [new] | compute | transform deltas | `GpuNodeInstance`, previous matrices | §8 open decision 1 |
| `resolve` | [exists] | compute | vis buffer, model slices, materials, camera (current + previous) | §4 surface + guides | M1; per-bin variants M7 |
| `classify` | [new] | compute | vis buffer | bin tile lists, counts, bin-id image | M7 |
| `light_world` | [exists] | compute, async | model-space lights, instance rows | `worldLights` | M3 (E2) |
| `rt_instances` | [exists] (C4) | compute, async | node instances, `GpuRayInstance`, `GpuRayInstanceBuild`, PTLAS state rows | PTLAS `WRITE_INSTANCE` arguments for changed rows (all rows on a full build), their count in the op entry, updated state rows | M2 |
| `rt_check` | [exists] | compute + ray query | TLAS, vis buffer, `decodeHit` | `GpuRayCheckStats` (host-visible) | M2, `ENGINE_RT_CHECK` only |
| `rt_ser_check` | [shelved, C-G1-3] | RT pipeline + SER | TLAS, vis buffer, `raytracing/ser` | `GpuSerCheckStats` (kept in tree, not compiled) | M4 G1 / post-1.0 V1 |
| `rtxdi_presample` | [new] | compute | light list | PDF texture, RIS buffers | M7 (`Power_RIS`) |
| `rtxdi_initial`, `rtxdi_temporal`, `rtxdi_shade` | [exists] (F3; `rtxdi_temporal` since RTXDI 3.2, [rtxdi_3_2_update.md](./features/rtxdi_3_2_update.md); `rtxdi_shade` runs the spatial step) | compute + ray query | guides (both sets), motion, lights, TLAS | reservoirs, `directRadiance` | M3 |
| `nrc_update` | [exists] (`NRC_UPDATE=1`) | compute + ray query | guides, TLAS, slices, materials, lights, NRC constants | NRC training paths (NRC comparison modes only) | M4 (G3) |
| `indirect` | [exists] (`NRC_QUERY=1`) | compute + ray query | same | ReSTIR GI initial samples; NRC query records (NRC modes only); multi-bounce ground truth when `giReferenceBounces > 0` | M4 (G2, G3, G5) |
| NRC `QueryAndTrain` | host SDK | — | NRC buffers | inference + one training step (NRC modes only) | M4 (G3) |
| `nrc_resolve` | [exists] | compute | NRC query results, primary material AO | `ao · throughput · cached radiance` into the ReSTIR GI initial samples (NRC modes only) | M4 (G3) |
| `rtxdi_gi_temporal`, `rtxdi_gi_shade` | [exists] | compute + ray query | GI samples, guides (both sets), motion, TLAS | `indirectRadiance`, optional `specHitDistance` | M4 (G5) |
| `combine` | [exists] (H1 direct + emissive; G6 adds indirect) | compute | `directRadiance`, `indirectRadiance`, `emissive` | `noisyColor` | M3/M4 (H1, G6) |
| DLSS-RR | host NGX | — | `noisyColor` + guides, exposure image | output color | M3 (H1) |
| `exposure` | [exists] (H2) | compute: `exposureHistogramMain`, `exposureAdaptMain` | output color | luminance histogram, exposure image, readback | M3 (H2) |
| `post` | [exists] (H2; planned as `composite`) | compute | output color, exposure | post target (ACES fitted, tonemap once); `present` draws it into the swapchain | M3 (H2) |
| `debug_view` | [exists] | compute (+ ray query for the ray views) | vis buffer, any §4 target, TLAS | `displayColor` | M1 onward |

Secondary hit decoding and direct-light shading share one helper (`lighting/path_shading.slang`), included by both `indirect` and `nrc_update`. Direct light at secondary hits calls RTXDI sampling without reuse plus one visibility ray; there is no second light loop. Both passes call the NRC bridge directly (skipped when `cacheConstants` is zeroed in ReSTIR GI mode).

---

## 6. Milestones (shader work)

The spec's §16. Each step renders something testable and is measured against spec §2.3.

| # | Shader work | Visible result |
|---|---|---|
| M0 | **Built.** PBR A1, A2 (modules), A4; `GpuTargets`; morph code removed; `base*`, `dlss` deleted (AU7); capacity checks in `meshlet_cull` | same image; structs asserted |
| M1 | **Built.** Per [visibility_buffer.md](./features/visibility_buffer.md): reverse Z + `nvhiz`; `visbuffer`, `surface`, vis-buffer `mesh_fragment`, opaque bin with winding flip; `resolve` as one tiled dispatch with `sampleMaterial` (B1); `gbuffer` layout; `debug_view`, `present`; `mesh_debug` removed | guide debug views; same geometry as today; culling lossless under reverse Z (`ENGINE_CULL_CHECK`, done 2026-11-13) |
| M2 | **Built** (C4 2026-11-14). `geometry/hit`, `rt_instances` (PTLAS writes), ray views in `debug_view`, `rt_check`; the OMM 2-state ray flag in `raytracing.trace` and `geometry.hit` | debug colors by cluster; unload keeps other models traceable (`ENGINE_RT_CHECK`, 2026-11-13) |
| M3 | **Built** (2026-11-14; RTXDI 3.2 on 2026-11-15). `sampleMaterial`, BRDF, `evalLight` (PBR B); `light_world`, `rtxdi_initial`, `rtxdi_temporal`, `rtxdi_shade`, `combine` (direct + emissive), `exposure`, `post`; culling jitter rule | stable direct light |
| M4 | **Built** on RTX 5070 (2026-11-19). `lighting/path_shading`, `indirect`, `nrc_update`, `gi/nrc_bridge`, `gi/nrc_constants`, `nrc_resolve`, `rtxdi_gi_temporal`, `rtxdi_gi_shade`, `combine` (+ indirect), indirect mode toggle (ReSTIR GI default since 2026-11-20); `raytracing/ser` and `rt_ser_check` built in G1 then shelved 2026-11-17 (C-G1-3) | bounce light under Sponza arches, GI check converges; NRC cache converges (C-G3-4); all three indirect modes switch cleanly |
| M6 | alpha-tested bin fragment; OMM flags in `trace` | cut-outs in raster and shadows, zero ray candidates |
| M7 | `classify`, per-bin `resolve` execution set with overlap guard; emissive-triangle lights; `rtxdi_presample` | per-bin timings; Bistro emissives lit |
| M8 | per-pass tuning from the frame profile; packed RTXDI surface guide (O4); NRC query heuristic if I7 confirms it | per-scope table before and after; GI check gated ([spec/m8](./m8/README.md)) |
| M9 | none (asset pipeline) | — |
| M10 (1.0) | per-stage tuning against spec §2.3 | Bistro at target |
| L1–L3 (post-1.0) | LOD traversal kernels above `meshlet_cull` (node tree, `testForTraversal`, persistent traversal, reject nodes and groups); software raster into the vis buffer; RT LOD traversal and cluster BLAS builds | Bistro fully on cluster LOD (L3) |

---

## 7. Shader rules for culling

From [culling_comparison.md](../reference/culling_comparison.md); detail in spec §8.1.

- `core/culling.slang` is the only place frustum, Hi-Z, sub-pixel and facing tests live. Pass files call it; they do not re-derive clip bounds.
- Boxes at every level (instance, group, cluster); the cluster sphere is only for the cone test.
- `cullMeshlets` runs one 32-lane workgroup per draw record (one group): group box first, verdict shared by its clusters, cone per cluster. The reject mask is OR-ed in groupshared memory, no wave-size assumption.
- Appends: one atomic per wave (`WaveActiveCountBits` / prefix), then the capacity check (A10).
- Ported files keep the NVIDIA Apache-2.0 header and a "modified" note.

---

## 8. Open decisions

| # | Item | Resolution |
|---|---|---|
| 1 | `scene_update`: the CPU writes `GpuNodeInstance` for every entity × node each frame, which spec P2 forbids | owned by spec AU4, optional |
| 2 | Vendor HLSL headers through Slang (spec Q6) | verify per SDK at integration; thin HLSL shim per SDK if needed |
| 4 | Vis buffer 32 vs 64 bit (spec Q3) | 32-bit chosen; compare on real runtime after M1 (feature step V8) |
| 5 | `NonUniform` dropped on heap access (spec Q5) | `sampleSlot` (`material/pbr.slang`); re-probe on every Slang upgrade |

Closed by the spec (2026-10-31 to 2026-11-20): `ClusterID` and attributes on `HitObject` (spec Q8, closed 2026-11-16 on Slang 2026.22: minimal closest-hit reads `ClusterIDNV`); SER shelved for 1.0 on 2026-11-17 (C-G1-3; 1.0 uses compute ray queries, SER returns in post-1.0 V1); visibility buffer over raster G-buffer; Mega Geometry clusters and PTLAS for all RT geometry; SHaRC dropped 2026-11-14 (NRC only, paused as a comparison switch in 1.0 since 2026-11-20); no any-hit; render targets through `GpuTargets`; culling shaped like `vk_lod_clusters`' leaf level.

---

## Changelog

| Date | Change |
|---|---|
| 2026-11-20 | Brought up to M3/M4 as built: `indirect`, `nrc_update`, `nrc_resolve`, `rtxdi_gi_temporal`, `rtxdi_gi_shade`, `lighting/path_shading`, `gi/nrc_bridge`, `gi/nrc_constants` exist; `gi/sharc.slang` removed (C-G3-1); `raytracing/ser` and `rt_ser_check` shelved (C-G1-3); 1.0 uses compute ray queries and defaults to ReSTIR GI mode (NRC paused); `GpuFrameRoot` fields and debug views updated |
| 2026-11-14 | PBR C4 built: `rt_instances` writes PTLAS instances for changed rows; `traceVisibility` and `traceClosest` set `RAY_FLAG_FORCE_OMM_2_STATE` through `requireOmm2StateRayFlag()` |
| 2026-11-14 | PTLAS required (engine spec §10.3): `rt_instances` writes PTLAS ops from PBR C4 |
| 2026-11-14 | NVIDIA only, NRC only (engine spec §1.2, §11.4): SHaRC and `IRadianceCache` dropped; M5 merged into M4 |
| 2026-11-14 | PBR F3: `rtxdi_initial` and `rtxdi_shade` exist and write `directRadiance`; `debug_view` implements `Lighting only`; the bridge bounds-checks reservoir and light reads |
| 2026-11-14 | PBR F2: `lighting/rtxdi_bridge`, `lighting/rtxdi_constants` and `raytracing/trace` (`traceVisibility`) exist; the resolve writes `position` and `motion.z`; `debug_view` gains `Shadowed lights` |
| 2026-11-14 | `light_world` exists (PBR E2): world light rows per frame on async compute; `debug_view` gains `Unshadowed lights` |
| 2026-11-13 | M2 as built (PBR C2, C3): `rt_instances`, `geometry/hit`, `core/counters` and `rt_check` exist. The planned `rt_debug` pass became ray views in `debug_view` |
| 2026-11-13 | `lighting/lights.slang` exists (PBR B3); emissive triangles removed from its row, they are RTXDI triangle lights |
| 2026-11-13 | M1 as built (engine spec §20.4): `resolve`, `present`, `debug_view`, `visbuffer`, `surface`, `brdf` exist; `hiz_build` is 3 levels per dispatch; `mesh_debug` gone; frame root fields of M0; typed `storageTex2D`; Q8 moved to closed |
| 2026-11-12 | Stub files deleted (engine spec AU7, [dead_code_removal.md](./features/dead_code_removal.md)): `base*`, `raytracing/*`, `postprocessing/dlss`, `gi/rtxgi`, `pathtrace*`, `sharc_resolve`, `composite`; their rows are [new] at their milestone |
| 2026-11-08 | `postprocessing/gbuffer.slang` deleted: the guide layout is `GuideImage` in `types.slang` (PBR task A3) |
| 2026-11-08 | Library files converted to Slang modules (PBR task A2); `gi/sharc.slang` stays textual until its bridge |
| 2026-11-07 | Visibility buffer plan applied: 32-bit payload, `geometry/surface`, `present`; `mesh_debug` removed in M1; open decision 4 answered |
| 2026-11-07 | Moved to `spec/` and realigned with [engine_spec.md](../docs/engine_spec.md): vis buffer + compute resolve replace the raster G-buffer; RT pipelines with SER, cluster AS, SHaRC + NRC behind `IRadianceCache`, ReSTIR GI; `raytracing/*`, `gi/sharc`, `pathtrace*`, `sharc_resolve` back in scope; culling, Hi-Z and triangle-cull files added; §2 maps spec §5 stages to passes; §6 follows spec milestones M0–M9, L1–L3; open decisions from spec §18 |
| 2026-10-23 | Aligned with the PBR plan: NRC replaces SHaRC; ray queries in compute, no RT pipeline, SER or mega geometry (reversed 2026-11-07) |
| 2026-10-06 | §1.1 binding model (model-relative slices, `GpuModel` table, new `FrameRoot` structure); handle-vs-BDA and stride-check ground rules |
| 2026-10-01 | Initial plan: GPU-driven raster, RTXDI, path tracing + SHaRC/NRC, OMM, DLSS-RR/SR |
