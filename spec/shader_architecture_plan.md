# Shader architecture plan

**Date:** 2026-10-01, updated 2026-11-13
**Status:** plan, aligned with [engine_spec.md](../docs/engine_spec.md) (normative; its §4 rules S1–S11 and §5 frame order win over this doc). M1 is built: culling, Hi-Z, the vis-buffer raster, the resolve, the debug view and the present pass exist; the BRDF exists, no light loop yet. This doc maps the spec onto shader files and passes. Append changes to [Changelog](#changelog).
**Related:** [resource_architecture.md](./resource_architecture.md) (buffers, model slices, frame root, handle vs BDA), [pbr_implementation_plan.md](./pbr_implementation_plan.md) (shading tasks A–H; the spec's §17 lists which of its decisions are superseded), [culling_comparison.md](../reference/culling_comparison.md) (culling library), [vulkan_extensions_reference.md](../docs/vulkan_extensions_reference.md), [shaders/README.md](../shaders/README.md)

What every shader file is for, which pass runs it, and what it reads and writes. The frame is built around:
- **Visibility buffer** for primary visibility: GPU culling, mesh shaders through DGC, two-phase Hi-Z occlusion (spec §8)
- **Compute material resolve** with analytic derivatives, binned by material shader through a DGC compute execution set (spec §9)
- **Cluster ray tracing** with RTX Mega Geometry: CLAS per cluster, cluster BLAS, GPU-written TLAS (spec §10)
- **RT pipelines built around SER** for every pass that shades hits; ray queries for visibility only (spec §11.1)
- **RTXDI ReSTIR DI**, **SHaRC / NRC** radiance cache, **ReSTIR GI** (spec §11)
- **Opacity micromaps (OMM)** for alpha-tested rays, no any-hit (spec §10.5)
- **DLSS Ray Reconstruction** for denoising and upscaling in one step (spec §12)

Target hardware is **NVIDIA RTX 20 and newer**, one code path (spec §1.2, §2).

Legend: **[exists]** has real code today · **[stub]** placeholder file · **[new]** to be created · **[delete]** to be removed. Milestones (M0–M9, L1–L3) are the spec's §16.

---

## 1. Ground rules

The spec's S1–S11 apply; shader-side consequences:

- **One path, no fallbacks.** No vendor checks, raster-only mode or old-technique toggles. Tiers change resolution, ray counts, cache choice and LOD error only (spec P1).
- **Slang modules.** Library code is `module x;` / `import a.b;` (S2). Vendor HLSL (RTXDI, SHaRC, NRC) is `#include`d only from bridge and pass files.
- **Bindless only.** Buffers through typed BDA pointers (`bdaPtr<T>`), read once into locals; images and samplers through heap handles (S3, S4). No descriptor-set bindings, no `StructuredBuffer` for engine data.
- **One root pointer.** Every pass reaches scene data from `GpuFrameRoot`; push data carries the root address plus pass-local values, within `maxPushDataSize` (S7). Render-target heap indices live in the per-frame `GpuTargets` block reached from the root (M0).
- **Shaded rays use RT pipelines with SER.** `HitObject::TraceRay` → `ReorderThread(hit, materialBin)` → minimal closest-hit returning `{instance, cluster, triangle, barycentrics, hitT}` → shade in raygen with the same `decodeHit` and `sampleMaterial` as the vis-buffer resolve (spec §11.1). One raygen, one miss, one closest-hit per SBT; no any-hit, no per-material hit groups. Visibility-only rays are ray queries.
- **Same ids everywhere.** A vis-buffer pixel and a ray hit both resolve to `(nodeInstance, cluster, triangle)`; the cluster id is the model-slice `meshletRow` (spec §8.3, §10.1).
- **Texture LOD.** No implicit-LOD `Sample` outside fragment shaders: compute resolve uses analytic gradients (`SampleGrad`), ray hits use ray cones (S10). LOD bias `log2(render / display)`.
- **Two resolutions.** Everything up to combine runs at render resolution (indirect at half resolution, checkerboard, on the floor tier); DLSS-RR outputs display resolution; post runs there.
- **Jitter and motion.** The render projection is jittered (Halton 2,3). Culling, Hi-Z and LOD use the unjittered matrix; exact pixel tests use the jittered one or widen by the jitter (spec §12.2). Motion vectors exclude jitter, in render-resolution pixels.
- **HDR until tonemap.** Tonemap exactly once, in post. Guides are linear, never sRGB-encoded.
- **Specialization by generics and link-time constants** (S9), not preprocessor variants, except vendor-mandated defines (`NRC_UPDATE`, `NRC_QUERY`, `SHARC_UPDATE`, `SHARC_QUERY`). Device-derived sizes (cluster limits, list capacities) are link-time constants (S11).
- **Handle vs address.** `kNoneIndex` never becomes a handle; non-uniform heap indices go through `sampleOr` (S6, PBR plan §4.4).
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
├─ materials, pbrExtensions, lights (+ lightCount)
├─ nodeInstances   : GpuNodeInstance[entity × node] world + previous world matrix
├─ drawRecords     : GpuDrawRecord[]               cull-internal work items
├─ visibleMeshlets : GpuVisibleMeshlet[]           {nodeInstanceIndex, meshletRow}; lateVisibleBase
├─ cullCounters, triangleCull, subpixelSnap, debugView, frameFlags
├─ targets         : GpuTargets*                    heap indices of every render target
├─ frameIndex, exposure
├─ tlas            : uint64                         0 until M2
├─ rayInstances    : GpuRayInstance[]               0 until M2
├─ rtxdiConstants, cacheConstants                   0 until M3 / M4
│  planned (spec §6.2):
└─ jitter (M3), light lists, material bins
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
| 6' | RT instance write → TLAS build | `rt_instances` | async compute | [new] | M2 |
| 8 | Classify: vis buffer → material tiles, bin-id image | `classify` | graphics | [new] | M7 |
| 9 | Resolve + shade → shading surface + DLSS guides | `resolve` (one tiled dispatch until M7, then one shader per bin through a DGC execution set) | graphics | [exists], one tiled dispatch | M1, M7 |
| 10 | ReSTIR DI: initial → spatiotemporal → visibility | `rtxdi_presample` (M7), `rtxdi_initial`, `rtxdi_resample`, `rtxdi_shade` | graphics | [new] | M3 |
| 11 | GI cache + indirect | SHaRC: `pathtrace_update` → `sharc_resolve` → `pathtrace_query`; NRC: same path sources with `NRC_*` defines → `QueryAndTrain` (host) → `nrc_resolve` | graphics | [new] | M4 (SHaRC), M5 (NRC) |
| 12 | ReSTIR GI: temporal + spatial | `restir_gi` | graphics | [new] | M4 |
| 13 | Combine: direct + indirect + emissive | `combine` | graphics | [new] | M4 |
| 14 | DLSS-RR | host NGX | graphics | — | M3 |
| 15 | Post: auto exposure → tonemap → swapchain, ImGui | `exposure`, `composite` | graphics | [new] | M3 |

Debug passes: `debug_view` [exists] (geometry and culling views from the vis buffer, every guide; later reservoirs, cache debug resolve, DLSS input vs output), `hiz_view` [exists], `rt_debug` [new, M2] (ray-query cluster colors, primary self-hit distance after L2). `mesh_debug` was deleted in M1. Output: `present` [exists] samples the display image into the swapchain, then ImGui.

---

## 3. Directory layout

Library files become Slang modules in M0 (PBR A2). Vendor headers are textual includes, so bridge files stay `#include`d. Entry points live in `passes/`, one compile unit per pass (`engine_add_slang_pass` in `CMakeLists.txt`).

| Path | Status | Purpose |
|---|---|---|
| `types.slang` | [exists] | POD mirrors of `src/core/types.hpp` and `push_data.hpp`, checked field by field by `tests/layout_test.cpp`; includes `GpuTargets`, `GpuRayInstance` and the M0 frame root fields. Morph types removed (2026-11-08) |
| **core/** | | |
| `core/binding.slang` | [exists] | `tex2D`, `uintTex2D`, `storageTex2D<T, format>` (typed storage images), `samplerOf`, `bdaPtr`. To do: `sampleOr` (S6) |
| `core/frame.slang` | [exists] | `loadDrawContext`, `loadCamera`, `loadMaterial`, `loadMaterialExtension`, `loadWorldLight` |
| `core/culling.slang` | [exists] | modified port of `vk_lod_clusters` `culling.glsl` (Apache-2.0 header kept): `intersectFrustum`, `isHiZOccluded`, `isSubPixel`, `isFrontFacingHW`, `testTrianglePixel`; reverse Z only |
| `core/math.slang` | [exists] | `objectToClip`, `jitterClip`, `pixelToNdc`, `normalMatrix`, ONB, octahedral encode / decode, `luminance`. To do: PCG RNG, Halton |
| **geometry/** | | |
| `geometry/mesh.slang` | [exists] | meshlet, vertex and triangle fetch through model slices; `loadFrontFacingTriangle` (winding flip) |
| `geometry/triangle_cull.slang` | [exists] | homogeneous backface + small-primitive test (`SV_CullPrimitive`) |
| `geometry/visbuffer.slang` | [exists] | pack/unpack the 32-bit vis-buffer payload (`visibleRow << kVisTriangleBits \| triangle`); analytic barycentrics and screen derivatives (M1, [visibility_buffer.md](./features/visibility_buffer.md)) |
| `geometry/surface.slang` | [exists] | triangle fetch and attribute interpolation from `(nodeInstance, meshletRow, triangle)`, shared by the resolve (M1) and `decodeHit` (M2) |
| `geometry/hit.slang` | [new] | `decodeHit`: `(instance, cluster, triangle)` → world `SurfaceHit`; positions by position fetch at ray hits; ray-cone LOD (M2) |
| **material/** | | |
| `material/pbr.slang` | [exists] | `SampledMaterial` + `sampleMaterial` with explicit gradients, used by the resolve and later by ray hits (B1) |
| `material/brdf.slang` | [exists] | glTF GGX `evalBrdf`, `sampleBrdf` (VNDF), `pdfBrdf`, `envBrdfApprox` (B2; `tests/brdf_test.cpp`) |
| **lighting/** | | |
| `lighting/lights.slang` | [new] | `LightSample`, `evalLight` for point, spot, directional, emissive triangle (B3, M7) |
| `lighting/rtxdi_bridge.slang` | [new] | RTXDI resource macros + `RAB_*`, textual include (F2) |
| **gi/** | | |
| `gi/radiance_cache.slang` | [new] | `IRadianceCache { update, query }`; the cache is a generic parameter, chosen per tier (spec §11.4) |
| `gi/sharc.slang` | [exists], partial | SHaRC through BDA (`RW_STRUCTURED_BUFFER` → pointer), `SHARC_UPDATE` / `SHARC_QUERY` per compile unit; implements `IRadianceCache` (M4) |
| `gi/nrc_bridge.slang` | [new] | NRC buffers + `NrcConstants`, matching the SDK's HLSL packing; implements `IRadianceCache` (M5) |
| **raytracing/** | | |
| `raytracing/payload.slang` | [new] | minimal hit record `{instance, cluster, triangle, barycentrics, hitT}` |
| `raytracing/hit.slang` | [new] | the one minimal closest-hit (`ClusterID`, needs `allowClusterAccelerationStructure`) and miss; skipped if Slang exposes cluster id on `HitObject` (Q8) |
| `raytracing/trace.slang` | [new] | `HitObject::TraceRay` + `ReorderThread` with the material-bin hint; ray-query `traceVisibility` for visibility rays; `FORCE_OPACITY_MICROMAP_2_STATE` / `FORCE_OPAQUE` flags from the instance |
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
| `diffuseAlbedo`, `specularF0` | RGBA8 | `resolve` | RTXDI, paths, DLSS diffuse albedo |
| `specAlbedo` | RGBA8, env-BRDF approximation | `resolve` | DLSS specular albedo |
| `emissive` | R11G11B10F | `resolve` | `combine` |
| `motion` | RG16F, render-res pixels, no jitter | `resolve` | DLSS, ReSTIR temporal |
| `specHitDistance` | R16F, optional, full render resolution after ReSTIR GI | `restir_gi` | DLSS (only if measured useful) |
| `directRadiance` | RGBA16F | `rtxdi_shade` | `combine` |
| `indirectRadiance` | RGBA16F | `restir_gi` | `combine` |
| `noisyColor` | RGBA16F HDR | `combine` | DLSS-RR |
| Output color | RGBA16F, display resolution | DLSS-RR | `exposure`, `composite` |

Buffers (BDA, [resource_architecture.md](./resource_architecture.md) §12):
- **Scene:** asset slices through `GpuModel`; materials, lights, primitives, node instances through the frame root.
- **Culling:** cull views, counters, draw records, candidates, reject masks, visible lists, DGC sequences.
- **Ray tracing:** CLAS, cluster BLAS, TLAS, `VkAccelerationStructureInstanceKHR[]`, `GpuRayInstance[]`, OMM arrays and per-cluster OMM index buffers.
- **Lighting:** RTXDI reservoirs and light buffer, emissive-triangle list, SHaRC hash/resolved buffers or NRC SDK buffers, ReSTIR GI samples, per-SDK constants.
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
| `rt_instances` | [new] | compute, async | node instances, models | AS instances, `GpuRayInstance`, TLAS build input | M2 |
| `rt_debug` | [new] | compute + ray query | TLAS, `decodeHit` | debug image | M2 |
| `rtxdi_presample` | [new] | compute | light list | PDF texture, RIS buffers | M7 (`Power_RIS`) |
| `rtxdi_initial`, `rtxdi_resample`, `rtxdi_shade` | [new] | compute + ray query | guides (both sets), motion, lights, TLAS | reservoirs, `directRadiance` | M3 |
| `pathtrace_update` | [new] | RT pipeline + SER | guides, TLAS, slices, materials, lights, cache | cache training (SHaRC `SHARC_UPDATE`, NRC `NRC_UPDATE`) | M4, M5 |
| `pathtrace_query` | [new] | RT pipeline + SER | same | ReSTIR GI initial samples, cache query records | M4, M5 |
| `pathtrace` | [new] | — | — | — | shared path code; becomes a module imported by the two above |
| `sharc_resolve` | [new] | compute | SHaRC buffers | resolved cache | M4 |
| NRC `QueryAndTrain` | host SDK | — | NRC buffers | inference + one training step | M5 |
| `nrc_resolve` | [new] | compute | NRC query results | `throughput × cached radiance` into the ReSTIR GI samples | M5 |
| `restir_gi` | [new] | compute + ray query | GI samples, guides, motion | `indirectRadiance`, optional `specHitDistance` | M4 |
| `combine` | [new] | compute | `directRadiance`, `indirectRadiance`, `emissive` | `noisyColor` | M4 |
| DLSS-RR | host NGX | — | `noisyColor` + guides | output color | M3 |
| `exposure` | [new] | compute | output color | luminance histogram, exposure | M3 |
| `composite` | [new] | compute | output color, exposure | swapchain (tonemap once) | M3 |
| `debug_view` | [exists] | compute | vis buffer, any §4 target | `displayColor` | M1 onward |

There is one path loop (`pathtrace`). Direct light at secondary hits calls RTXDI sampling without reuse plus one visibility ray; there is no second light loop. Path code is shared between SHaRC and NRC through `IRadianceCache`.

---

## 6. Milestones (shader work)

The spec's §16. Each step renders something testable and is measured against spec §2.3.

| # | Shader work | Visible result |
|---|---|---|
| M0 | **Built.** PBR A1, A2 (modules), A4; `GpuTargets`; morph code removed; `base*`, `dlss` deleted (AU7); capacity checks in `meshlet_cull` | same image; structs asserted |
| M1 | **Built.** Per [visibility_buffer.md](./features/visibility_buffer.md): reverse Z + `nvhiz`; `visbuffer`, `surface`, vis-buffer `mesh_fragment`, opaque bin with winding flip; `resolve` as one tiled dispatch with `sampleMaterial` (B1); `gbuffer` layout; `debug_view`, `present`; `mesh_debug` removed | guide debug views; same geometry and cull counts as today |
| M2 | `geometry/hit`, `rt_instances`, `rt_debug` | debug colors by cluster; unload keeps other models traceable |
| M3 | `sampleMaterial`, BRDF, `evalLight` (PBR B); RTXDI passes; `exposure`, `composite`; culling jitter rule | stable direct light |
| M4 | `raytracing/*`, `pathtrace_*` with SER, `radiance_cache`, `sharc`, `sharc_resolve`, `restir_gi`, `combine` | bounce light, reorder on/off timings |
| M5 | `nrc_bridge`, NRC path defines, `nrc_resolve` | NRC vs SHaRC A/B |
| M6 | alpha-tested bin fragment; OMM flags in `trace` | cut-outs in raster and shadows, zero ray candidates |
| M7 | `classify`, per-bin `resolve` execution set with overlap guard; emissive-triangle lights; `rtxdi_presample` | per-bin timings; Bistro emissives lit |
| M8 | none (asset pipeline) | — |
| M9 (1.0) | per-stage tuning against spec §2.3 | Bistro at target |
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
| 5 | `NonUniform` dropped on heap access (spec Q5) | `sampleOr`; re-probe on every Slang upgrade |

Closed by the spec (2026-10-31 to 2026-11-08): `ClusterID` and attributes on `HitObject` (spec Q8: in Slang v2026.20 `GetClusterID` is NV-only, so the minimal closest-hit invoke stays; compile probe at M4); visibility buffer over raster G-buffer; RT pipelines with SER for shaded rays; Mega Geometry clusters for all RT geometry; SHaRC and NRC behind one interface; no any-hit; render targets through `GpuTargets`; culling shaped like `vk_lod_clusters`' leaf level.

---

## Changelog

| Date | Change |
|---|---|
| 2026-11-13 | M1 as built (engine spec §20.4): `resolve`, `present`, `debug_view`, `visbuffer`, `surface`, `brdf` exist; `hiz_build` is 3 levels per dispatch; `mesh_debug` gone; frame root fields of M0; typed `storageTex2D`; Q8 moved to closed |
| 2026-11-12 | Stub files deleted (engine spec AU7, [dead_code_removal.md](./features/dead_code_removal.md)): `base*`, `raytracing/*`, `postprocessing/dlss`, `gi/rtxgi`, `pathtrace*`, `sharc_resolve`, `composite`; their rows are [new] at their milestone |
| 2026-11-08 | `postprocessing/gbuffer.slang` deleted: the guide layout is `GuideImage` in `types.slang` (PBR task A3) |
| 2026-11-08 | Library files converted to Slang modules (PBR task A2); `gi/sharc.slang` stays textual until its bridge |
| 2026-11-07 | Visibility buffer plan applied: 32-bit payload, `geometry/surface`, `present`; `mesh_debug` removed in M1; open decision 4 answered |
| 2026-11-07 | Moved to `spec/` and realigned with [engine_spec.md](../docs/engine_spec.md): vis buffer + compute resolve replace the raster G-buffer; RT pipelines with SER, cluster AS, SHaRC + NRC behind `IRadianceCache`, ReSTIR GI; `raytracing/*`, `gi/sharc`, `pathtrace*`, `sharc_resolve` back in scope; culling, Hi-Z and triangle-cull files added; §2 maps spec §5 stages to passes; §6 follows spec milestones M0–M9, L1–L3; open decisions from spec §18 |
| 2026-10-23 | Aligned with the PBR plan: NRC replaces SHaRC; ray queries in compute, no RT pipeline, SER or mega geometry (reversed 2026-11-07) |
| 2026-10-06 | §1.1 binding model (model-relative slices, `GpuModel` table, new `FrameRoot` structure); handle-vs-BDA and stride-check ground rules |
| 2026-10-01 | Initial plan: GPU-driven raster, RTXDI, path tracing + SHaRC/NRC, OMM, DLSS-RR/SR |
