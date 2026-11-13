# Feature: visibility buffer

**Date:** 2026-11-07 · **Status:** V1–V7 implemented (§10), M1 done 2026-11-13; RTX 5070 timings in engine spec §2.3; V8 deferred; motion vectors continue in [motion_vectors.md](./motion_vectors.md) · **Milestone:** M1 ([engine_spec.md](../../docs/engine_spec.md) §16.2)
**Implements:** engine spec §8.1 (A6, A9), §8.2, §8.3, §9.2 (single-dispatch form), §9.3, §12.2 (partial)
**Depends on:** M0: `GpuTargets` block, shading attributes (PBR A4: normals and tangents for every primitive), animation removal (§7.3)
**Related:** [resource_architecture.md](../resource_architecture.md), [shader_architecture_plan.md](../shader_architecture_plan.md), [culling_comparison.md](../../reference/culling_comparison.md), [pbr_implementation_plan.md](../pbr_implementation_plan.md) B1

Replace the forward raster pass with a visibility buffer: raster writes only depth and a 32-bit triangle id, and one compute pass rebuilds each pixel's surface to write the shading surface and DLSS guides. Every later lighting pass reads those guides.

---

## 1. Goals and non-goals

| Goals | Non-goals (milestone that owns them) |
|---|---|
| Reverse-Z depth, Hi-Z rebuilt for it with the `nvhiz` port (A9) | Lighting, BRDF, lights (M3) |
| Bins by pipeline: one opaque bin, two-sided by winding flip (A6) | Alpha-tested bin and `discard` (M6) |
| R32_UINT visibility buffer, both cull phases | Material classification and per-bin shaders (M7) |
| Compute resolve: analytic barycentrics and derivatives, `sampleMaterial` with `SampleGrad`, motion vectors | Jitter and DLSS (M3); this feature keeps the jitter hooks |
| The §9.3 surface and guide images, two sets by frame parity | Software raster (L1); this feature fixes the format it will share |
| Geometry and culling debug views from the vis buffer, plus a view per guide | Overdraw view (not planned, §9 D2) |
| Render resolution separate from display resolution (equal until M3) | |

---

## 2. Current state (`7dc04f965`)

- `mesh_raster` + `mesh_fragment` draw straight into the swapchain image (`B8G8R8A8_SRGB`), with interpolated color, UV and material row; `sampleBaseColor` is the whole material. Early phase clears, late phase loads ([vk_renderer.cpp](../../src/render/vk_renderer.cpp) `recordPhase`).
- Depth `D32_SFLOAT`, standard Z: clear 1, `LESS`. The Hi-Z build runs after each phase, one mip per dispatch, max reduction ([hiz_build.slang](../../shaders/passes/hiz_build.slang), [vk_hiz.cpp](../../src/render/vk_hiz.cpp)).
- Two state groups (`SingleSided`, `DoubleSided` with cull mode none and no cone test), so 2 DGC executes per phase.
- Geometry debug views are a second raster variant (`mesh_debug`, `RasterVariant::GeometryDebug`), with its own DGC shader info and state command buffers.
- Render extent = swapchain extent everywhere (viewport, Hi-Z, `GpuCameraData::renderTargetSize`).

---

## 3. Design

### 3.1 Visibility buffer format

**`R32_UINT`, payload = `(visibleRow << kVisTriangleBits) | triangle`.**

| Field | Bits | Meaning |
|---|---|---|
| `triangle` | `kVisTriangleBits = ceil(log2(maxTrianglesPerCluster))`, 7 today | meshlet-local triangle index, as `ti` in the mesh shader |
| `visibleRow` | `32 − kVisTriangleBits`, 25 today (33.5 M rows) | row of `GpuFrameRoot::visibleMeshlets` of this frame slot; late-phase rows are `≥ lateVisibleBase` |

- `kVisTriangleBits` is a link-time constant from the device-derived cluster size (S11). Startup asserts that the visible-list capacity of both phases fits the row field.
- **No flags.** Facing is recomputed in the resolve from the projected winding and `NodeFlag::FlipWinding`. The cull phase comes from `visibleRow ≥ lateVisibleBase`.
- **Empty pixels are decided by depth** (`depth == 0`, the reverse-Z clear), never by the payload. The vis buffer needs no clear: early load op `DONT_CARE`, late `LOAD`.
- The resolve turns a pixel into `(nodeInstance, meshletRow, triangle)` through one load: `visibleMeshlets[visibleRow]`. That triple is what a ray hit also decodes to (spec §10.1), so the shared surface code (§3.5) takes the triple, not the row.

**Why 32-bit, against the spec's 64-bit default (spec §8.3, Q3).** `vk_lod_clusters`' software raster, planned for L1, writes `packUint2x32(value32, depthBits)` into an `R64_UINT` image with `imageAtomicMax`, from both the compute and the hardware raster path ([render_raster_clusters_sw.comp.glsl:325](../../reference/vk_lod_clusters/shaders/render_raster_clusters_sw.comp.glsl), [render_raster.frag.glsl:364](../../reference/vk_lod_clusters/shaders/render_raster.frag.glsl)). The sample stores a shaded color in the 32-bit slot; the engine stores the vis payload there. A 64-bit payload cannot share that atomic with depth. With a 32-bit payload, L1 changes only the image format and the write path, not the encoding or any reader. It also halves vis-buffer bandwidth. Decided 2026-11-07 (§9 D1); the real-runtime comparison with 64-bit is engine spec Q3.

### 3.2 Reverse Z (A9)

| Item | Change |
|---|---|
| Projection | `glm::perspective(fov, aspect, far, near)` (zero-to-one is already forced in `pch.h`), as the reference's `perspectiveRH_ZO(fov, aspect, far, near)`. `nearZ` / `farZ` in `GpuCameraData` keep their meaning; finite far plane |
| Depth | clear 0, compare `GREATER` (both phases) |
| Hi-Z | the pyramid keeps the farthest depth, which is now the minimum: min reduction, `SamplerReductionMode::eMin` |
| Culling | reverse Z in [culling.slang](../../shaders/core/culling.slang): nearest depth, occlusion compare and nudge sign, near-plane clip bit. Already written, untested |
| `hiz_view` | linearization formula for reverse Z |
| Tests | `hiz_footprint_test` runs every case in both depth conventions |

### 3.3 Hi-Z build (`nvhiz` port)

Port [nvhiz-update.comp.glsl](../../reference/vk_lod_clusters/shaders/nvhiz-update.comp.glsl) to Slang: 3 levels per dispatch (`NV_HIZ_LEVELS`), `WaveReadLaneAt` for `subgroupShuffle`, `NV_HIZ_REVERSED_Z 1`. Keep the Apache-2.0 header with a "modified" note. The far pyramid is the only output (`NV_HIZ_OUTPUT_NEAR 0`). Pyramid sizing (today the previous power of two of the depth size) follows what the port requires. The C1 footprint rule in `isHiZOccluded` must keep holding: re-run `hiz_footprint_test` against the new pyramid.

### 3.4 Raster

**Bins (A6).** `MeshStateGroup { SingleSided, DoubleSided }` becomes `RasterBin { Opaque, AlphaTested }`; only `Opaque` is populated in M1 (alpha-masked materials stay opaque, as today). One raster state for all bins: back-face culling on.
- Two-sided triangles: the mesh shader swaps two corners when `!isFrontFacingHW(a, b, c)` for a double-sided material, the same branch as `FlipWinding` (`vk_lod_clusters` [render_raster_clusters.mesh.glsl:320](../../reference/vk_lod_clusters/shaders/render_raster_clusters.mesh.glsl)). The triangle index is unchanged.
- Cull: the cone test is skipped by a `TwoSided` bit in `GpuDrawRecord::flags` (set by the CPU template builder from the material), replacing the state-group bits. Groups never span primitives, so `*_MIXED` bits are not needed.
- DGC: one execute per (phase, bin) → 2 per frame in M1, 4 from M6.

**Mesh shader** (`mesh_raster`): position only per vertex; per primitive `SV_PrimitiveID = payload` and `SV_CullPrimitive`. Triangle culling (`triangle_cull.slang`) stays as is. `MeshVertexOut` loses color, UV and material row.

**Fragment shader** (`mesh_fragment`, opaque bin): `return primitiveId;` into the `R32_UINT` attachment. No material access. `[earlydepthstencil]` is implied (no discard, no depth write).

**Attachments:** vis buffer (`R32_UINT`, `COLOR_ATTACHMENT | SAMPLED`), depth (`D32_SFLOAT`, `DEPTH_STENCIL_ATTACHMENT | SAMPLED`), both at render resolution. The swapchain is no longer a raster target.

### 3.5 Resolve

One compute pass, `[numthreads(8, 8, 1)]`, one thread per render pixel, one shader (the M7 per-bin form reuses the same code, §7). Per pixel:

1. Depth `== 0` → write background guides: zero normal and albedo, motion from reprojecting the view direction at infinity with `prevViewProj`; return.
2. Payload → `visibleMeshlets[visibleRow]` → `(nodeInstance, meshletRow, triangle)`.
3. **Shared surface code** (new `geometry/surface.slang`, also used by M2's `decodeHit`): fetch the three corners (`meshletTriangles`, `meshletVertices`, positions, normals, tangents, UVs, vertex color), transform with `worldMatrix` and `prevWorldMatrix`.
4. **Analytic barycentrics** (`geometry/visbuffer.slang`): perspective-correct barycentrics from the three clip positions under the **jittered** raster matrix, evaluated at the pixel center and at `(+1, 0)` and `(0, +1)`. The differences are the exact screen derivatives on the triangle's plane: no helper lanes, no quad overdraw.
5. UV and its gradients → `sampleMaterial` (PBR B1 brought forward, base color / metal-rough / normal / occlusion / emissive) with `SampleGrad` and LOD bias `log2(render / display)` (0 in M1).
6. Facing from the clip-space winding and `FlipWinding`; double-sided back faces flip the shading and geometric normal.
7. Motion: barycentric-interpolated position under `prevWorldMatrix` and unjittered `prevViewProj`, minus the current unjittered projection, in render-resolution pixels. Sign and scale per NGX, set when DLSS lands (H1).
8. Write the guides (§3.6) of set `frameIndex & 1`.

The jitter hook: raster and step 4 use `viewProj` plus the jitter as a clip-space offset (`clip.xy += jitter · clip.w`); culling and motion use `viewProj` without it. The jitter is zero until M3, but the code paths are separate from day one (spec §12.2).

### 3.6 Outputs

| Image | Format | Notes |
|---|---|---|
| Vis buffer | `R32_UINT` | one, read only this frame |
| Depth | `D32_SFLOAT`, reverse Z | hardware depth for DLSS (`InUseHWDepth`) |
| `normalRoughness` | `RGBA16F` | world shading normal, perceptual roughness |
| `geoNormal` | `RG16_SNORM`, octahedral | facing-corrected |
| `diffuseAlbedo` | `RGBA8` | `baseColor · (1 − metallic)` |
| `specularF0` | `RGBA8` | `lerp(0.04, baseColor, metallic)` |
| `specAlbedo` | `RGBA8` | env-BRDF approximation (shared with PBR B2) |
| `emissive` | `R11G11B10F` | emissive factor × texture × strength |
| `motion` | `RG16F` | render-resolution pixels, no jitter |

- Two sets (all rows from `normalRoughness` down), ping-ponged by frame parity; allocated now so RTXDI and ReSTIR GI temporal passes (M3, M4) change no layout.
- Storage writes of `RG16_SNORM`, `RG16F` and `R11G11B10F` use the SPIR-V `StorageImageExtendedFormats` capability (core since 1.0, no device feature); the format features are checked at startup. Storage images are typed through `storageTex2D<T, format>` (`core/binding.slang`), so no write-without-format is needed.
- Heap indices go into `GpuTargets` (M0), reached from `GpuFrameRoot::targets`.
- Memory at the reference tier's render size (1707×960): about 105 MB for both sets plus vis buffer and depth; about 66 MB at 1280×720 on the floor tier.

### 3.7 Display path until M3

- `debug_view` (compute, [new]) writes `displayColor` (`RGBA16F`, render resolution): the albedo guide for the default view, or the selected debug view.
- `present` (fullscreen mesh + fragment, [new]) samples `displayColor` into the swapchain, then ImGui draws in the same rendering. It scales if render ≠ display.
- M3's `composite` replaces `present`'s source with tonemapped DLSS output. It stays a fragment pass, because the sRGB swapchain format has no storage support.

### 3.8 Debug views

All from the vis buffer, in `debug_view`:

| View | Source |
|---|---|
| Meshlets, triangles, primitives, node instances | payload → visible entry → ids (today's `debugId` logic moves here) |
| Bins (replaces "State groups") | material flags of the primitive |
| Cull phase | `visibleRow ≥ lateVisibleBase` |
| Edges overlay | analytic barycentrics (replaces `SV_Barycentrics`) |
| One per §3.6 guide, depth | the image |
| Hi-Z pyramid | stays the existing `hiz_view` raster pass |

This removes `mesh_debug`, `RasterVariant`, and the per-variant DGC shader infos and state command buffers. The `fragmentShaderBarycentric` feature becomes unused.

---

## 4. Frame order and sync

```
early cull → preprocess → early raster (vis + depth) → Hi-Z
late  cull → preprocess → late  raster (vis + depth) → Hi-Z
resolve (compute)  → debug_view (compute) → present + ImGui (swapchain)
```

| Edge | Barrier |
|---|---|
| late raster → resolve | vis buffer: color attachment write → compute sampled read, layout `COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL`. Depth: already `SHADER_READ_ONLY_OPTIMAL` after the late Hi-Z build |
| resolve → debug_view | guide storage writes → compute reads (one memory barrier) |
| debug_view → present | `displayColor` storage write → fragment sampled read |
| previous frame's resolve → this frame's early raster | vis buffer `SHADER_READ_ONLY → COLOR_ATTACHMENT` from `UNDEFINED` (content discarded) |

Hi-Z placement is unchanged. The resolve can overlap the late Hi-Z build (both read depth); no ordering is needed between them.

---

## 5. Code changes

| Area | Change |
|---|---|
| `ResourceManager` | `renderExtent` separate from `swapChainExtent`; vis buffer, guide sets and `displayColor` created at render extent on resize, heap slots in `GpuTargets` |
| `Camera` | reverse-Z projection; unjittered `viewProj` / `prevViewProj`, jitter kept apart |
| `HiZPyramid`, `hiz_build.slang` | `nvhiz` port, min reduction, sized from `renderExtent` |
| `MeshletCulling`, `types.hpp` / `types.slang` | `RasterBin` replaces `MeshStateGroup`; `DrawRecordFlag::TwoSided` |
| `DeviceGeneratedCommands` | one shader info, one state command buffer per bin |
| `ShaderObjects` | vis-buffer mesh + fragment pair, `resolve`, `debug_view`, `present`; `mesh_debug` removed |
| `Renderer` | attachments per §3.4, passes and barriers per §4, viewport and scissor from `renderExtent` |
| `debug_view.hpp` | views per §3.8 |
| Shaders | `mesh_raster`, `mesh_fragment` rewritten; new `geometry/visbuffer`, `geometry/surface`, `passes/resolve`, `passes/debug_view`, `passes/present`; `material/pbr` gains `sampleMaterial`; `core/culling` switch on |
| `GpuTimer` | scopes `Resolve`, `DebugView`, `Present` |

---

## 6. Steps

Each step keeps a correct image and is measured on both GPUs (Sponza static and fast turn, a many-instance scene) before the next.

| # | Step | Done when |
|---|---|---|
| V1 | `renderExtent` split, guide and vis images allocated, `GpuTargets` entries | resize works; images visible in the debug views as cleared |
| V2 | Reverse Z (§3.2) | culling counters identical to standard Z on the same frozen view; `hiz_footprint_test` passes in both conventions |
| V3 | `nvhiz` port (§3.3) | same occlusion counts as V2; Hi-Z time recorded |
| V4 | Bins by pipeline, winding-flip two-sided (§3.4) | same visible counts; double-sided foliage and mirrored instances correct; one execute fewer per phase |
| V5 | Vis-buffer raster + `debug_view` geometry views + `present` | every geometry and culling view matches the old `mesh_debug` output |
| V6 | Resolve, `sampleMaterial`, guides, motion | a guide view for every §3.6 image; albedo view matches the old lit output except for texture LOD differences; motion near zero for a static camera and correct under rotation |
| V7 | Cleanup (`mesh_debug`, `RasterVariant`, `MeshVertexOut` varyings) and timings | spec §2.3 rows "cull + Hi-Z", "vis-buffer raster" and "material resolve" recorded on both tiers |
| V8 | 32 vs 64-bit comparison (engine spec Q3): a temporary `R32G32_UINT` build with the same resolve | raster and resolve timings and bandwidth of both on both tiers; result recorded in Q3 |

---

## 7. Future compatibility

| Later work | What this feature fixes now |
|---|---|
| M2 cluster RT | `geometry/surface.slang` takes `(nodeInstance, meshletRow, triangle)`; `decodeHit` feeds it ray barycentrics and ray-cone gradients. CLAS must be built from each meshlet's final triangle order, so `PrimitiveIndex()` equals the vis-buffer `triangle` |
| M3 DLSS-RR | depth is hardware reverse-Z depth; guides already in NGX's set; the jitter hook of §3.5; `present` becomes `composite`'s output stage; render ≠ display already supported |
| M3–M4 temporal passes | two guide sets by parity; motion without jitter |
| M6 alpha | `RasterBin::AlphaTested`: a second fragment shader with UV interpolation and `discard`, writing the same payload. The resolve does not change |
| M7 binning | `classify` reads the same payload. The pixel → bin chain is `visibleRow → meshletRow → primitive → material → bin`; if classify time shows that chain, the cull pass can write a bin id per visible row so classify needs one load. The resolve body becomes the per-bin shader with the overlap guard |
| L1 software raster | the 32-bit payload fits `packUint2x32(payload, depthBits)` in an `R64_UINT` image; reverse Z makes `imageAtomicMax` keep the nearest. Needs `shaderImageInt64Atomics` (`VK_EXT_shader_image_atomic_int64`, not enabled yet; check the 2060 Super). Hardware depth for DLSS then comes from a fullscreen pass over the atomic image (the reference's `fullscreen_write_depth.frag.glsl`) |
| L1 cluster LOD | `meshletRow` grows into LOD levels; `kVisTriangleBits` follows the cluster preset (8 bits at 256 triangles leaves 24 row bits, 16.7 M rows) |

---

## 8. Risks

| Risk | Mitigation |
|---|---|
| Resolve cost: about 10 dependent loads per pixel before shading (visible entry, node instance, model, primitive, meshlet, corners, vertices) | coherent within a triangle, so mostly cache hits; measure in V6 against the 1.2 ms row; Nsight long-scoreboard stalls decide whether to store more in the visible entry |
| Derivative quality at triangle edges and silhouettes differs from quad derivatives | analytic derivatives are exact on the plane; compare texture sharpness in V6 |
| Reverse-Z regressions in culling (near-plane bit, nudge sign) | V2 compares counts on a frozen view before anything else changes |
| Storage format support for `R11G11B10F` / `RG16_SNORM` | startup check; fall back is a spec change, not a code path |

---

## 9. Open decisions

All decided 2026-11-07.

| # | Decision | Outcome |
|---|---|---|
| D1 | Vis-buffer payload: 32 vs 64 bit | **32-bit** for performance and the L1 software-raster atomic (§3.1); engine spec §8.3 updated. After V7, compare against 64-bit on real runtime on both tiers (engine spec Q3) |
| D2 | Overdraw debug view | **not needed**; dropped from engine spec §14 |
| D3 | Far plane under reverse Z | **finite**, as in §3.2 |

---

## 10. As built (2026-11-07)

| Step | State | Deviation from this spec |
|---|---|---|
| V1 | done: `ResourceManager::renderExtent`, `RenderTargets` ([vk_render_targets.cpp](../../src/render/vk_render_targets.cpp)), `GpuTargets` behind `GpuFrameRoot::targets` (one host block per frame slot) | M0 was not done first: no shading-attribute generation (A4), so the resolve falls back to the geometric normal where a primitive has no normals and skips normal mapping without tangents; morph targets still exist and the resolve applies the same default-weight positions as the raster |
| V2 | done; `hiz_footprint_test` runs the reverse-Z convention (the forward-Z branch was removed, AU5) | not compared against standard Z: reverse Z is the reference. Instead, `ENGINE_CULL_CHECK=1` shows culling is lossless, with images identical with every cull test on and off on 16 Sponza poses (2026-11-13) |
| V3 | done: 3 levels per dispatch ([hiz_build.slang](../../shaders/passes/hiz_build.slang)) | groupshared reductions instead of `subgroupShuffle` (no wave-size assumption); pyramid layout unchanged (previous power of two, level 0 reduces the 1–3 texel footprint), so `hizMip` and its test are untouched. Hi-Z time not recorded |
| V4 | done: `RasterBin`, `DrawRecordFlag::TwoSided`, winding flip in `mesh_raster` | one DGC state command buffer for both bins until M6 gives `AlphaTested` its own fragment shader |
| V5 | done: vis-buffer raster, `debug_view`, `present`; `mesh_debug`, `RasterVariant` and `fragmentShaderBarycentric` removed | barycentrics from Olano–Greer homogeneous edge functions, valid for triangles crossing the eye plane. The Hi-Z view now draws in the present pass, so it shows the final pyramid (the next frame's early test) |
| V6 | done: resolve, `sampleMaterial`, all §3.6 guides, motion | guide storage writes go through typed images (`RWTexture2D<T, 0, format>`, AU5) with a startup format-feature check; sampled views serve the reads. Checked on Sponza: unit normals, shading and geometric normals in one hemisphere, geometric normal facing the viewer, zero motion for a static camera. Motion under camera and entity motion checked by the scripted motion test ([motion_vectors.md](./motion_vectors.md) §5.1, 2026-11-13). Guides of `MetalRoughSpheres`, `TwoSidedPlane` and `UnlitTest` match the Khronos screenshots in kind (`ENGINE_GUIDE_CAPTURE`, PBR plan E1, 2026-11-13) |
| V7 | cleanup done | spec §2.3 timings not recorded on either tier |
| V8 | deferred (2026-11-08): 32-bit payload kept | |

Validation: no errors after startup, during frames, or on resize. The remaining best-practice warnings come from pre-existing code or the ImGui backend, apart from `Depth32Format`, which reverse Z justifies (commented at `findDepthFormat`).

Present on its own submit (done 2026-11-08): the frame work (cull, raster, Hi-Z, resolve, debug view) is submitted before the acquire with no wait; only the present + UI submit waits on the acquire semaphore and signals the fence. An out-of-date acquire signals the fence with an empty submit. The `Acquire wait` timer scope shows the GPU idle time until the image arrives and is excluded from the total. Checked with core + sync validation through repeated resizes: clean.

## References

- Engine: [engine_spec.md](../../docs/engine_spec.md) §8–§9, §12.2, §16; [culling_comparison.md](../../reference/culling_comparison.md) A6, A8, A9
- vk_lod_clusters: [render_raster.frag.glsl](../../reference/vk_lod_clusters/shaders/render_raster.frag.glsl) (`clusterID << 8 | primitive` id for picking, 64-bit `{color, depth}` atomic path), [render_raster_clusters_sw.comp.glsl](../../reference/vk_lod_clusters/shaders/render_raster_clusters_sw.comp.glsl), [render_raster_clusters.mesh.glsl](../../reference/vk_lod_clusters/shaders/render_raster_clusters.mesh.glsl) (two-sided winding flip), [nvhiz-update.comp.glsl](../../reference/vk_lod_clusters/shaders/nvhiz-update.comp.glsl)
- Current code: [mesh_raster.slang](../../shaders/passes/mesh_raster.slang), [mesh_fragment.slang](../../shaders/passes/mesh_fragment.slang), [resolve.slang](../../shaders/passes/resolve.slang), [debug_view.slang](../../shaders/passes/debug_view.slang), [present.slang](../../shaders/passes/present.slang), [hiz_build.slang](../../shaders/passes/hiz_build.slang), [vk_renderer.cpp](../../src/render/vk_renderer.cpp), [vk_hiz.cpp](../../src/render/vk_hiz.cpp), [debug_view.hpp](../../src/render/debug_view.hpp)
