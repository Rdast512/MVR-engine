# Meshlet Culling: Options and Trade-offs

## Scope

Decision record for GPU culling of the raster path. Covers where culling runs, which tests run, the occlusion scheme, the Hi-Z source, the raster target, DGC scope and LOD. The last sections review an external proposal ("4-tier hierarchical culling + visibility buffer") and give a recommended path.

Decided so far:

| Question | Decision |
|---|---|
| ImGui | Full integration: toggles, culling counters, per-pass GPU timings |
| DGC | Kept as the submission layer for the whole renderer |
| Culling placement | 1C: compute cull + DGC with GPU-written sequences |
| MSAA | Dropped (4C) |
| Occlusion scheme | 3C: two-phase with candidate lists against the previous Hi-Z |
| Raster target | 5A: forward shading stays |
| Per-triangle culling | Behind a toggle (2D) |

## Current State

- Raster: `passes/mesh_raster.slang` (lit) and `passes/mesh_debug.slang` (geometry views). Mesh-only, one mesh workgroup per meshlet, no task stage.
- Data: `MeshPushData{frameRoot, firstVisible, meshletCount}` → `GpuFrameRoot::visibleMeshlets` row → `GpuDrawRecord` → model slices, all through BDA.
- Submission (stage 1): [vk_dgc.cpp](../src/render/vk_dgc.cpp). The cull pass writes the DGC sequences and one sequence count per state group; execute reads the count through `sequenceCountAddress`. No CPU sequence fill and no 95-sequence chunking.
- Bounds: `GpuMeshletBounds` (mesh-space box plus shortest and longest edge, parallel to `GpuMeshletDesc`) for frustum, sub-pixel and Hi-Z; `GpuNode::bboxMin/bboxMax` (the mesh box) for instances (A2 in [culling_comparison.md](../reference/culling_comparison.md)). `GpuMeshletDesc.boundingSphere` and the quantized cone (`coneAxis` int8 × 3, `coneCutoff` int8) from `meshopt_computeMeshletBounds` feed the cone test. Since A3 the builder is clusterlod's level 0 (`meshopt_buildMeshletsFlex`, `kMeshletConeWeight = 0`), and each primitive's meshlets are partitioned into `GpuMeshletGroup`s of ≤ 32 with a box; a draw record is one group.
- Attachments: one render pass, single sample (3.1), color straight into the swapchain, depth `D32_SFLOAT` sampled and stored, standard Z (clear 1.0, `eLess`).
- Hi-Z (3.2): [vk_hiz.cpp](../src/render/vk_hiz.cpp) builds an `R32_SFLOAT` max pyramid (`passes/hiz_build.slang`); nothing reads it yet.
- Two-phase skeleton (3.3): `CullPhase::Early` / `Late`, each with its own `GpuCullView`, `GpuCullCounters`, visible-list region and DGC sequence + preprocess regions per state group. Frame: early cull → preprocess → raster (clear) → pyramid → late cull → preprocess → raster (load) + ImGui → pyramid. The late cull only runs `buildMeshSequences`, so its counts stay 0.
- Occlusion helper (3.4): [culling.slang](../shaders/core/culling.slang), the port of vk_lod_clusters `culling.glsl` (A1 in [culling_comparison.md](../reference/culling_comparison.md)). `intersectFrustum` projects the 8 corners of an object-space box under a world matrix into `ClipBounds` (frustum clip bits, conservative rect and nearest depth; any corner in front of the near plane → not valid), and `hizMip` picks the mip where the rect spans ≤ 1 texel, or one finer when it fits a 2×2 block there (niagara); `isHiZOccluded` takes one max-sampler fetch at the rect center (its 2×2 footprint covers the rect) with a 2-ulp depth nudge. `isSubPixel` culls instances and meshlets whose rect holds no pixel center (C1/C2 in [culling_comparison.md](../reference/culling_comparison.md)). `GpuCullView` carries `occlusionViewProj` (early: `prevViewProj`, late: `viewProj`) and the pyramid indices. `tests/hiz_footprint_test.cpp` checks the bounds on known spheres through both the sphere and the box path (`intersectFrustum`).
- Instance tier two-phase (3.5): early `cullInstances` tests the node box under `GpuNodeInstance::prevWorldMatrix` against the previous pyramid with `prevViewProj`; occluded items go to `instanceCandidates` (capacity = instance count), counted in the late counters' `instanceDispatch`. The late `cullInstances` is dispatched indirectly from that count and re-tests with current bounds against the fresh pyramid. Each phase appends draw records to its own region (`GpuCullView::drawRecordBase`). Morphed instances (`SkipCulling`) are never occlusion-tested.
- Meshlet tier two-phase (3.6): early `cullMeshlets` adds the occlusion test (`prevWorldMatrix`, previous pyramid) after frustum + cone. Since A4 ([culling_comparison.md](../reference/culling_comparison.md)), occluded meshlets are bits in their record's `rejectMasks` word (one `uint` per draw record, no overflow possible). Late: `cullMeshlets` tests recovered instances' records (frustum + cone + fresh pyramid), and `cullRejectedMeshlets` (one workgroup per early record, dispatched with the early `meshletDispatch`) re-tests only occlusion of the set bits with current transforms. Before A4 this used a `meshletCandidates` list. Static camera in Sponza: 3464 meshlets → 2507 frustum, 78 cone, 450 occluded, 429 drawn.
- Invalidation fallback (3.7): `MeshletCulling::occlusionHistory` returns `NoPyramid` (pyramid recreated and not built yet: first frame, resize), `CameraCut` (`Camera::isCut`: first frame, `focusOn`, any projection change), `SceneChanged` (`nodeInstanceLayoutGeneration`), or `Valid`; a frozen culling camera keeps its pyramid and stays `Valid` (A7). Anything but `Valid` drops `CullFlag::Occlusion` from the early view, so the early phase draws every frustum + cone survivor and the late phase gets no candidates. The reason and a fallback-frame count are in the culling panel and the `Culling/HistoryValid` Tracy plot.
- ImGui and timings (stage 4): culling panel tables of instances and meshlets per test × phase (early / late / frame), occlusion history, toggles for instance frustum, frustum, cone, occlusion, counters and the frozen cull camera. Debug views "Cull phase" (early blue, late orange; `GpuFrameRoot::lateVisibleBase`) and "Hi-Z pyramid" (fullscreen log-depth of a chosen mip of the early-built pyramid, `passes/hiz_view.slang`). Nine GPU timestamp scopes with 120-frame rolling averages and a GPU total; Tracy plots per scope and per-phase culling counters.
- Per-triangle culling (stage 5): `geometry/triangle_cull.slang`, used by both mesh shaders. Corners go to groupshared clip positions, then each triangle is flagged with `SV_CullPrimitive`: backface (`isFrontFacingHW`, homogeneous determinant, matching `frontFace = CCW`; skipped for the double-sided group) and small-primitive (`testTrianglePixel`, bounds widened by the subpixel snap from `subPixelPrecisionBits` miss every pixel center). A corner with `w <= 0` keeps the triangle. Toggles "Triangle backface" and "Small triangles", both off by default; with counters on, a triangle table per phase and `Culling/Triangles*` Tracy plots.
- Double-sided materials (stage 1): their own state group and execute with `cullMode = eNone`; the cone test skips them. This fixed the old bug where a global `setCullMode(eBack)` ignored `GpuMaterialFlag::DoubleSided`.

## Hardware and Spec Facts

Measured with `vulkaninfo` on RTX 5070, driver 621.01:

| Property | Value |
|---|---|
| `supportedIndirectCommandsShaderStages` | vertex, tess, geometry, fragment, compute, task, mesh. **No ray tracing stages** |
| `maxIndirectShaderObjectCount` / `maxIndirectPipelineCount` | 16384 / 16384 |
| `maxIndirectSequenceCount` | 4194303 |
| `deviceGeneratedCommandsMultiDrawIndirectCount` | true |
| `maxMeshWorkGroupTotalCount` | 4194304 |

From `validusage.json` (Vulkan SDK 1.4.363):

- `vkCmdPreprocessGeneratedCommandsEXT` must be called outside a render pass instance.
- A layout with graphics stages must not contain `DISPATCH` or `TRACE_RAYS2` tokens. A ray tracing layout may only contain `TRACE_RAYS2`, `EXECUTION_SET` and push tokens, and needs ray tracing stages in the supported set.
- Execution-set shaders need `VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT`. Linked shaders from one `vkCreateShadersEXT` call must all be bound together.
- The value at `sequenceCountAddress` must not exceed the `maxSequenceCount` used to size preprocess memory. Its buffer needs `INDIRECT` usage.
- Raster state (cull mode, depth, blend) is not a token. Different state means a different execute.

## 1. Where Meshlet Culling Runs

### 1A. Task shader (amplification)

A 32-lane task group tests 32 meshlets and dispatches the survivors from a payload.

Pros:
- Smallest change: the DGC layout stays push + draw mesh tasks.
- No intermediate buffers; culled meshlets never launch mesh work.
- Wins on large objects with many meshlets per draw (terrain-like content).

Cons:
- Lanes idle when a primitive has few meshlets (Sponza-like content). One benchmark: 30k small objects, 0.954 ms task vs 0.591 ms compute + mesh-only.
- About 10% overhead even in the best case on NVIDIA and AMD (Granite). AMD emulates task shaders with many small indirect dispatches.
- Hierarchical culling in a task shader was about 10x slower in Granite (large payloads). NVIDIA advises payloads under 108 B; 32 x 4 B indices is already 128 B.
- Cannot switch shaders; culling state lives inside the draw, so stats and two-phase bookkeeping are scattered.
- Raster-only; nothing reusable for Mega Geometry or RT.

### 1B. Compute cull + plain indirect mesh draw

Compute writes a compacted `{drawRecord, meshlet}` list and the group count; one `vkCmdDrawMeshTasksIndirectEXT` per phase.

Pros:
- One thread per meshlet; no task stage overhead. Granite's final design.
- Exact counters for free; two-phase bookkeeping in one place.
- Same compute infrastructure as `mega_select` and cluster LOD selection.

Cons:
- Extra buffers (visible list, args) and a compute → indirect barrier.
- No shader switching; material variants need separate draws.
- Fragment shader needs the draw record per primitive (per-primitive output).

### 1C. Compute cull + DGC with GPU-written sequences (chosen direction)

Same cull as 1B, plus the cull pass writes one DGC sequence per non-empty bin and the sequence count.

Pros:
- Everything from 1B.
- Per-sequence shader switching through execution sets when materials need it.
- Sequences drop from thousands of primitive batches to a few bins, so the 95-sequence NVIDIA bug no longer matters.
- No per-frame CPU sequence fill.

Cons:
- Preprocess per execute, outside the render pass; one extra barrier per phase.
- With one bin it costs slightly more than 1B for no gain; the gain arrives with shader binning.
- State changes still need separate executes (state groups).

### 1D. Task shader inside DGC (CPU sequences)

Current CPU sequences, with a task stage added.

Pros:
- Least code.

Cons:
- Combines the task-shader costs with per-frame CPU sequence fill and the 95-sequence chunking.
- Does not use DGC for GPU decisions at all.

## 2. Cull Tests

Order: cheapest and most rejecting first, texture fetches last.

| Test | Granularity | Cost | Typical gain | Notes |
|---|---|---|---|---|
| Frustum (sphere vs 6 planes) | instance, meshlet | 6 dot products | Large when looking away | Planes extracted on the CPU from `viewProj`; robust to DLSS jitter |
| Normal cone (backface) | meshlet | 1 dot + sqrt | 5–25% of frustum survivors; low on flat architecture | Needs `cone_weight ≈ 0.25` at build; skip double-sided, mirrored, non-uniform scale |
| Hi-Z occlusion (sphere → screen AABB) | instance, meshlet | 1–4 texel fetches | Large in indoor/occluded scenes | Needs pyramid and an occlusion scheme (section 3) |
| Per-triangle backface | triangle, mesh shader | per-triangle math | Granite 3.9 → 3.3 ms | Hardware already culls; mixed on NVIDIA (zeux) |
| Small-primitive / sub-pixel | triangle, mesh shader | bbox vs sample centers | Granite 3.3 → 1.9 ms | Must use current jitter; MSAA changes sample positions |

### 2A. Frustum only

Pros: simplest; no extra data.
Cons: does nothing for back-facing or hidden geometry.

### 2B. Frustum + cone

Pros: 4 B per meshlet (quantized `cone_axis_s8` + `cone_cutoff_s8`); `GpuMeshletDesc` stays 32 B by packing vertex and triangle counts into one `uint`.
Cons: small gain on flat scenes; needs exclusions (double-sided, mirrored, non-uniform scale); slightly worse meshlet quality with cone weight.

Test without apex (correct for perspective, conservative):
`dot(center - cameraPos, axis) >= cutoff * length(center - cameraPos) + radius` → cull.

### 2C. Frustum + cone + Hi-Z

Pros: removes hidden geometry, which dominates indoor scenes.
Cons: needs the depth pyramid, compute infrastructure and an occlusion scheme.

### 2D. Add per-triangle culling in the mesh shader

Pros: helps when raster or primitive throughput is the bottleneck, mostly with dense, small triangles.
Cons:
- Mesh shader must transform all vertices first (shared memory) before testing triangles.
- Triangles with any vertex `w <= 0` must not be culled.
- Mirrored instances and double-sided materials need handling.
- NVIDIA advises not emulating fixed-function work; gains vary.
- Use `SV_CullPrimitive` (no compaction) and gate it behind a toggle; measure first.

### 2E. Add instance-level culling before meshlets

Pros: rejects whole node instances with one test; shrinks the meshlet cull dispatch. Bounds already exist (`GpuNodeInstance.boundingSphere`).
Cons: one more compute pass and an indirect dispatch for the meshlet stage.

## 3. Occlusion Scheme

### 3A. Last frame's Hi-Z only (single pass)

Test current bounds against the previous frame's pyramid.

Pros: one raster pass, one pyramid build.
Cons:
- Not conservative: disoccluded geometry is missing for a frame (popping, holes on fast turns).
- Depth reprojection, if used, leaves gaps and needs a scatter pass.

### 3B. Two-phase with per-meshlet visibility bits (niagara, Granite)

Phase 1 draws what was visible last frame. Build Hi-Z. Phase 2 tests everything against the fresh pyramid, draws the newly visible and writes the bits.

Pros:
- Conservative: no popping.
- One pyramid build per frame.
- Stale bits only cost performance, never correctness. A zeroed buffer means everything draws in phase 2.

Cons:
- Phase 2 re-tests every meshlet.
- Bits are keyed by meshlet/chunk; with LOD switching the keys change and phase 1 loses efficiency.
- Persistent buffer that must be sized and reset on scene changes.

### 3C. Two-phase with a candidate list against the previous Hi-Z (Nanite-style)

Phase 1 tests all instances and meshlets against the previous frame's pyramid, using previous-frame transforms (`prevViewProj`, `prevWorldMatrix`). Visible ones draw; occluded ones go to a candidate buffer. Build Hi-Z from phase 1 depth. Phase 2 re-tests only the candidates.

Pros:
- Conservative: candidates are always re-tested.
- Phase 2 touches only candidates, not the whole scene.
- No persistent per-meshlet state; works with LOD and cluster hierarchy changes. Fits Mega Geometry cluster LOD.
- Hierarchical: occluded instances become instance candidates, so their meshlets are never expanded in phase 1.

Cons:
- Previous pyramid must be kept: build after phase 1 (for phase 2) and after phase 2 (for the next frame).
- Candidate buffer needs a capacity and an overflow policy (overflow → treat as visible).
- Camera cuts and resize invalidate the previous pyramid; phase 1 must skip the occlusion test then.
- Previous-frame transforms are required for every tested object (already in `GpuNodeInstance`).

No depth reprojection is needed: test with previous-frame matrices against the previous pyramid instead.

## 4. Hi-Z Source

### 4A. Depth resolve (`eMax`) into a single-sample image, then pyramid

Pros: keeps MSAA; pyramid build uses a max-reduction sampler.
Cons: extra image and resolve; depth must be stored between phases; `eMax` support must be checked.

### 4B. Read MSAA depth directly in the first pyramid pass

Pros: no extra image.
Cons: N samples per texel in the first pass; cannot use the reduction sampler there.

### 4C. Drop MSAA

Pros:
- DLSS takes aliased, jittered single-sample input, so MSAA goes away with DLSS anyway.
- Hi-Z reads depth directly; no resolve; transient color issue disappears.
- Required for a practical visibility buffer.

Cons: no anti-aliasing until DLSS (or a TAA fallback) lands.

Pyramid build: one dispatch per mip is simple and enough to start; a single-pass downsampler (FidelityFX SPD style) saves barriers later. Size: previous power of two of the depth size, `R32_SFLOAT`, max reduction (standard Z).

## 5. Raster Target

### 5A. Forward shading in the mesh pass (current)

Pros: no new passes.
Cons: full material cost in the raster; overdraw shades twice; doubling raster work for two phases is expensive.

### 5B. G-buffer

Pros: well understood; feeds DLSS-RR guides directly.
Cons: fat render targets; mesh/fragment shaders still load material data; bandwidth heavy at high resolution.

### 5C. Visibility buffer

Raster writes only ids; a compute (or fullscreen) resolve reconstructs attributes and shades.

| Encoding | Bits | Pros | Cons |
|---|---|---|---|
| `R32_UINT`: visible-cluster id (25 b) + triangle (7 b) | 32 | Half the bandwidth; triangle fits since meshlets hold ≤ 126 | Needs a per-frame visible-cluster table to map back to instance + meshlet |
| `R32G32_UINT`: instance \| meshlet \| triangle | 64 | Direct lookup, no table | Twice the bandwidth |

Pros:
- Cheap raster, so two phases cost little more than one.
- Material shading cost is per pixel, not per overdraw.
- Shading becomes compute; material binning moves to tile classification. Raster shader switching is then needed only for alpha-tested and displaced geometry.
- Primary hits for path tracing and DLSS-RR guides can come from one resolve.

Cons:
- Large rewrite: attribute reconstruction, analytic UV derivatives (no `ddx` in compute), motion vectors from `prevWorldMatrix`.
- MSAA is impractical; requires 4C.
- Alpha-tested geometry still needs texture fetches in the raster.

## 6. DGC Scope

| Work | Use | Reason |
|---|---|---|
| Raster draws | DGC, GPU-written sequences, one execute per state group | Shader switching per bin; GPU count |
| Fixed-size compute (cull chunks, pyramid mips) | Plain `vkCmdDispatch` | CPU knows the size; DGC adds preprocess cost |
| GPU-sized compute (binned shading, wavefront PT) | DGC compute layout | GPU picks size and shader |
| Ray tracing | `vkCmdTraceRaysIndirect2KHR` now | Driver 621.01 reports no RT stages for DGC; `TRACE_RAYS2` unusable |
| Mega Geometry CLAS/BLAS builds | Cluster AS indirect commands | Separate indirect mechanism, not DGC |

State groups (separate executes): opaque single-sided, opaque double-sided (`cullMode = eNone`), alpha-tested later.

## 7. LOD

### 7A. None (current)

Pros: nothing to build.
Cons: dense meshes cost full price at any distance.

### 7B. Discrete per-instance LOD

Pros: simple; chosen in the instance cull pass.
Cons: popping; whole-object granularity.

### 7C. Cluster LOD hierarchy (DAG)

Pros:
- Continuous detail; per-cluster selection in the meshlet cull.
- Same hierarchy can feed Mega Geometry CLAS builds, so raster and RT share data.
- meshoptimizer can build it (simplification + cluster grouping).

Cons:
- Large build-time and runtime work; visibility bits (3B) stop being stable, so 3C fits better.

DLSS: measure screen-space error against the display resolution, not the render resolution. A `log2(render / display)` LOD bias only matches this when each LOD level doubles the error.

## 8. Review: Proposed 4-Tier Pipeline + Visibility Buffer

Overall: the structure is sound and matches modern GPU-driven renderers (Nanite-style). Several claims are overstated or inconsistent with each other or with this engine.

### Visibility buffer

- Agree on direction (5C).
- "Register pressure near zero" and "DGC twice per frame virtually free" are overstated. Raster gets cheaper; vertex transform, preprocess and the second cull still cost.
- Not stated: MSAA must go (4C). This engine runs max MSAA today.
- Not stated: the whole shading path becomes compute with analytic derivatives and reconstructed motion vectors. This is the biggest cost of the proposal and is independent of culling.
- 32-bit ids are enough with a visible-cluster table; 64-bit is simpler but doubles bandwidth.

### Tier 1: instance culling + continuous LOD

- Instance frustum + Hi-Z: agree (2E).
- "Continuous LOD" at instance level is a contradiction. Per-instance selection is discrete (7B); continuous LOD needs a cluster hierarchy selected per cluster (7C), which belongs in tier 2.
- DLSS LOD bias: the intent is right (keep display-resolution detail), but the formula only works for LOD chains where each level doubles error. Measure error against display pixels instead.
- Tension with tier 3: keeping display-resolution detail creates sub-pixel triangles at the render resolution, which tier 3 then culls. Detail kept for DLSS accumulation still costs vertex work every frame.

### Tier 2: meshlet culling

- Frustum + Hi-Z sphere projection: agree.
- Meshlet size: 64/124 works; this engine uses 64/126 (NVIDIA's figure). 124 helps packed 4-index writes; not worth a rebuild alone.
- Cone test as written ("view vector dots negatively") is incomplete: a correct perspective test needs the cutoff and the sphere radius (2B formula).
- Cone data is `int8[4]`: agree; meshopt already provides it.
- DGC role: agree that task shaders cannot switch shaders. Correction: DGC switches shaders only (execution sets), not pipeline state. Opaque vs alpha-tested works through execution sets; double-sided needs a separate execute.
- With a visibility buffer most materials share one raster shader, so DGC bins are few: opaque, alpha-tested, double-sided. The DGC case is weaker than the proposal suggests, but still useful.

### Tier 3: per-triangle culling

- Valid technique, but evidence is mixed on NVIDIA (2D). Make it optional and measure.
- `SV_CullPrimitive` does not require testing before `SetMeshOutputCounts`; output counts are set first, culled triangles are flagged.
- Must not cull triangles with any `w <= 0`; must handle mirrored instances and double-sided materials.
- Including the current jitter is correct, and only valid without MSAA.

### Temporal loop

- The described loop is 3C (Nanite-style): agree, it is the better fit once LOD exists.
- "Reproject last frame's depth into a Prev-Hi-Z" is unnecessary: test with previous-frame matrices against the previous pyramid. Depth reprojection leaves holes.
- Missing: the pyramid must also be built after phase 2 for the next frame; camera cuts and resize need a fallback; the candidate buffer needs an overflow policy.

### Fit with this engine

| Proposal element | Verdict | Prerequisite |
|---|---|---|
| Compute cull + DGC | Adopt now | Compute shader objects, GPU-written sequences |
| Frustum + cone | Adopt now | Cone data in `GpuMeshletDesc` |
| Hi-Z + 2POC (3C) | Adopt | Pyramid, previous pyramid, candidate buffer, 4C or 4A |
| Instance tier | Adopt | Instance cull pass |
| Visibility buffer | Adopt later, separate project | Drop MSAA, compute shading rewrite |
| Per-triangle culling | Optional, measure | Mesh shader rework |
| Continuous LOD + DLSS bias | Later | Cluster LOD build (shared with Mega Geometry) |

## 9. Recommended Path

Each stage ships and is measured on its own.

1. **Compute cull + DGC, frustum + cone.** Compute shader objects; cull pass writes the binned visible list, DGC sequences and count; state groups for single- and double-sided (fixes the `DoubleSided` bug); cone data and `cone_weight = 0.25`; morphed primitives skip culling. Retest the 95-sequence bug on driver 621.01.
2. **Instance tier.** Instance frustum cull feeding the meshlet cull through an indirect dispatch.
3. **Hi-Z + two-phase (3C).** Split into sub-steps; each one keeps the image correct and is measured on its own.
   1. **Drop MSAA (4C).** `msaaSamples = e1` for the mesh shader objects, the render pass and ImGui; render straight into the swapchain image (no transient color, no resolve). Depth becomes `D32_SFLOAT` (stencil is unused) with `eSampled` usage and `storeOp = eStore`. Done when: same image minus AA; pass timings recorded as the baseline for the rest of stage 3.
   2. **Hi-Z pyramid build.** Device features `samplerFilterMinmax`; a max-reduction sampler; storage-image heap writes. Pyramid: one `R32_SFLOAT` image, previous power of two of the depth size, full mip chain; recreated on resize. Build pass: mip 0 from depth, then one dispatch per mip with a barrier between mips. For now it runs once, after the single raster pass. Done when: the mips check out in a capture (RenderDoc/Nsight), every texel holds the max of its footprint and odd sizes are covered; build time measured.
   3. **Two-phase frame skeleton, no occlusion yet.** Order: cull 1 → preprocess 1 → raster 1 (clear) → pyramid → cull 2 → preprocess 2 → raster 2 (load) → pyramid. DGC sequences, counts and preprocess regions per (phase, state group); counters per phase. Phase 2 has no work yet: its sequence counts stay 0. One pyramid image is enough because a single queue runs frames in order: the next frame's phase 1 reads what this frame's phase 2 built. Done when: same image as 3.1; the overhead of the empty phase 2 is measured.
   4. **Occlusion test function.** Shared Slang helper: sphere → screen AABB with a given `viewProj`, mip chosen so the AABB covers ≤ 2×2 texels, 4 fetches with the max sampler, compare against the sphere's nearest depth. A sphere crossing the near plane counts as visible. Used with `prevViewProj` + `prevWorldMatrix` in phase 1 and the current matrices in phase 2. Add `MeshletCullSettings::isOcclusionEnabled`. Done when: the helper is unit-checked against a few known boxes or a debug readback; nothing culls with it yet.
   5. **Instance tier two-phase.** Phase 1: instances occluded against the previous pyramid go to an instance candidate buffer (capacity = instance count, so it cannot overflow); visible ones expand as today. Phase 2: re-test the candidates against the fresh pyramid with current matrices; survivors expand into phase 2 draw records → meshlet cull (frustum + cone) → phase 2 sequences. Indirect dispatches read their sizes from the phase counters. Done when: no holes when turning fast or stepping out from behind occluders; instance counters per phase (phase 1 drawn, candidates, recovered in phase 2) are visible.
   6. **Meshlet tier two-phase.** Phase 1 meshlet cull adds the occlusion test against the previous pyramid; occluded meshlets of visible instances go to a meshlet candidate buffer. The phase 2 meshlet cull reads two sources: meshlets of instances recovered in phase 2 and meshlet candidates. Capacity is the scene's meshlet total; overflow sends the meshlet to phase 1 as visible. Done when: meshlet counters per phase; no holes; GPU time compared with 3.5 in an occluded scene (Sponza interior) and an open one.
   7. **Invalidation fallback.** `isHistoryValid` is false on the first frame, after resize (pyramid recreated), on a camera cut (teleport, projection or FOV change, flagged by `Camera`), on a scene layout change (`nodeInstanceLayoutGeneration`), and while the culling camera is frozen (the pyramid then comes from the render camera, not the frozen one). When false, phase 1 skips the occlusion test and phase 2 has no candidates. Done when: resize, teleport and freeze/unfreeze show no missing geometry for a frame; the fallback frame is visible in the counters.
4. **ImGui and timings.** Toggles (frustum, cone, occlusion, per-triangle, freeze culling camera), counters per phase and per test, pyramid mip view, phase-colored debug view, timestamp queries per pass with rolling averages, Tracy plots.
5. **Per-triangle culling (2D)** in the mesh shader behind the toggle; measured with stage 4 timings.
6. **Later, separate projects:** visibility buffer + compute shading (with DLSS); cluster LOD shared with Mega Geometry.

## Decisions

| Decision | Options | Chosen | Reason |
|---|---|---|---|
| Hi-Z source | 4A depth resolve / 4C drop MSAA | **4C drop MSAA** | DLSS needs single-sample input anyway; Hi-Z reads depth directly |
| Occlusion scheme | 3B visibility bits / 3C candidates | **3C candidates** | Phase 2 re-tests only candidates; no persistent per-meshlet state. No LOD planned for now |
| Raster target | 5A forward / 5B G-buffer / 5C visibility buffer | **5A forward** | Scope is geometry culling; raster target is a separate project |
| Per-triangle culling | off / toggle | **Toggle** | Mixed results on NVIDIA; measure before enabling by default |

## Sources

- [Granite: Modernizing mesh rendering](https://themaister.net/blog/2024/01/17/modernizing-granites-mesh-rendering/) — task vs compute numbers, two-phase culling, per-primitive culling gains
- [NVIDIA: Advanced API Performance — Mesh Shaders](https://developer.nvidia.com/blog/advanced-api-performance-mesh-shaders/) — payload size, 64/126 meshlets
- [true-sim/engine issue #20](https://github.com/true-sim/engine/issues/20) — task vs compute culling on small objects
- [zeux: Fine-grained backface culling](https://zeux.io/2023/04/28/triangle-backface-culling/) — per-meshlet and per-triangle backface results
- [nvpro-samples: gl_vk_meshlet_cadscene](https://github.com/nvpro-samples/gl_vk_meshlet_cadscene/blob/master/README.md) — task shader culling and when to move to compute
- [Vulkan Samples: Mesh Shader Culling](https://docs.vulkan.org/samples/latest/samples/extensions/mesh_shader_culling/README.html)
- [Xbox: Meshlet culling](https://devdocs.xbox.com/build/core-features/graphics/newfeatures/ms/ms-meshlet-culling)
- [VK_EXT_device_generated_commands proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_device_generated_commands.html)
- meshoptimizer `meshoptimizer.h` (`meshopt_Bounds`, cone test formulas), local copy in `ThirdParty/_deps/src/meshoptimizer`
- Vulkan SDK 1.4.363 `share/vulkan/registry/validusage.json` — DGC valid-usage rules

## Verification Checklist

One section per step of the [recommended path](#9-recommended-path). Status on 2026-11-04 (commit `d4de24bdb`):

- **Implemented** items are checked against the code. `[x]` means the code does it, or a test ran and passed on that date.
- **Runtime** items need the running app, a capture (RenderDoc/Nsight) or a timing run. A step is closed when all its items pass.
- Stages 1–5 are implemented.
- The ImGui checks need `ENGINE_ENABLE_IMGUI 1`. The working copy of [Constants.h](../src/Constants.h) has it set to 0.

### 1. Compute cull + DGC, frustum + cone

Implemented:

- [x] Culling runs as compute shader objects in [meshlet_cull.slang](../shaders/passes/meshlet_cull.slang): `cullInstances`, `cullMeshlets`, `cullRejectedMeshlets` (was `cullMeshletCandidates` until A4), `buildMeshSequences`.
- [x] The cull pass writes the visible list per state group, the DGC sequences and one sequence count per group. Execute reads the count through `sequenceCountAddress`. Since A5 an entry is `GpuVisibleMeshlet {nodeInstanceIndex, meshletRow}`; the mesh shaders resolve primitive and material from the meshlet, not from the draw record.
- [x] No per-frame CPU sequence fill and no 95-sequence chunking.
- [x] Meshlets per sequence come from `maxMeshWorkGroupCount` / `maxMeshWorkGroupTotalCount`, raised if needed so a group stays within `maxIndirectSequenceCount`.
- [x] State groups single-sided and double-sided, one execute each. Double-sided binds `cullMode = eNone` and skips the cone test, which fixes the `DoubleSided` bug.
- [x] Cone data in `GpuMeshletDesc` (`coneAxis` int8 × 3 + `coneCutoff` int8; still 32 B, `static_assert`); `kMeshletConeWeight = 0.25` (0 since A3, to match clusterlod's level 0).
- [x] Cone test: the meshoptimizer sphere form, run in object space, so it stays exact under non-uniform scale and mirroring. Mirrored instances flip winding (`NodeFlag::FlipWinding`).

Runtime:

- [ ] Same image with every cull toggle off and with all on, apart from the geometry they remove.
- [ ] A double-sided material stays visible from behind; a mirrored instance is neither culled nor drawn inside out.
- [ ] Retest the 95-sequence bug on driver 621.01: lower "Meshlets / sequence" until one group runs more than 96 sequences in one execute; nothing goes missing.

### 2. Instance tier

Implemented:

- [x] `cullInstances` runs one workgroup per instance item and tests the node box under `GpuNodeInstance::worldMatrix` against the frustum (a world sphere before A2). Each survivor copies its draw-record templates.
- [x] The draw-record append counter doubles as the x size of the meshlet cull's indirect dispatch (`meshletDispatch`).
- [x] Templates are rebuilt only when `nodeInstanceLayoutGeneration` changes.
- [x] Instance and draw-record counts are checked against `maxComputeWorkGroupCount`.

Runtime:

- [ ] In the many-instance test scene ([Constants.h](../src/Constants.h)), look away: instances "Frustum culled" rises and "Draw records" drops.
- [ ] "Instance frustum" off gives the same image, with more meshlets tested.

### 3.1 Drop MSAA

Implemented:

- [x] Single sample everywhere: attachments and `setRasterizationSamplesEXT` use `e1`, and color goes straight into the swapchain image with no transient image or resolve.
- [x] Depth is `D32_SFLOAT` with `storeOp = eStore` and is sampled by the Hi-Z build.

Runtime:

- [ ] Image matches the MSAA build minus anti-aliasing.
- [ ] Record the Cull / Preprocess / Draw averages from the ImGui GPU timings as the stage 3 baseline.

### 3.2 Hi-Z pyramid build

Implemented:

- [x] `samplerFilterMinmax` is enabled. The format check requires storage and min/max filtering for `R32_SFLOAT`, and a max-reduction sampler is created.
- [x] The pyramid is the previous power of two of the depth size, with the full mip chain. It is recreated in `rebuildSwapchainResources`.
- [x] Mip 0 reads the full 1–3 depth texels per axis; each later mip is one max-sampler fetch, with a barrier between mips.

Runtime:

- [ ] In RenderDoc or Nsight, every pyramid texel holds the max of its footprint: mip 0 against depth, each later mip against its parent.
- [ ] Repeat at an odd window size, after a resize, and on the last 1×1 mips.
- [ ] Record the `Hi-Z build` average.

### 3.3 Two-phase frame skeleton

Implemented:

- [x] Frame order: early cull → preprocess → raster (clear) → Hi-Z → late cull → preprocess → raster (load) + ImGui → Hi-Z.
- [x] DGC sequences, counts and preprocess regions per (phase, state group); counters per phase.

Runtime:

- [ ] Image matches 3.1; with occlusion off, "Visible meshlets: … late" stays 0.
- [ ] Record the empty late phase's overhead: late cull + late DGC preprocess + late mesh draw + late Hi-Z build.
- [ ] Compare the early Hi-Z build with the 3.2 number to get the cost of the second pyramid build.

### 3.4 Occlusion test function

Implemented:

- [x] [culling.slang](../shaders/core/culling.slang) (formerly `hiz.slang`): `intersectFrustum`, `hizMip` and `isHiZOccluded`, used by both the instance tier and the meshlet tier.
- [x] `hiz_footprint_test` passes: 7 cases, 0 failures (rerun 2026-11-05, after the 1-fetch test from [culling_comparison.md](../reference/culling_comparison.md) C1). The cases cover the screen rect, nearest depth and mip, that one fetch at the rect center covers the rect while one mip finer does not, plus near-plane and behind-camera spheres.

Runtime:

- [ ] Check the `isHiZOccluded` fetches in a capture (covered by the 3.5/3.6 captures).

### 3.5 Instance tier two-phase

Implemented:

- [x] The early phase tests the node box under `prevWorldMatrix` with `prevViewProj` against the previous pyramid. Occluded instances go to `instanceCandidates`, whose capacity is the instance count.
- [x] The late `cullInstances` is dispatched indirectly from `instanceDispatch` and re-tests the current bounds against the fresh pyramid.
- [x] Each phase appends draw records to its own region (`drawRecordBase`).
- [x] Morphed instances are never occlusion-tested.

Runtime:

- [ ] With occlusion on, turn fast and step out from behind occluders: no instance missing for a frame.
- [ ] Instances "Occluded" (early) rises behind occluders; instances "Drawn" (late) is non-zero for a frame after fast turns.
- [ ] Occlusion off gives the same image with zero candidates.

### 3.6 Meshlet tier two-phase

Implemented:

- [x] The early `cullMeshlets` runs the occlusion test after frustum + cone, using `prevWorldMatrix`. Occluded meshlets set a bit in their record's reject mask (A4; a `meshletCandidates` list before).
- [x] ~~Every 64th candidate opens a late workgroup.~~ Replaced in A4: the late re-test runs one workgroup per early record, from the early `meshletDispatch`.
- [x] Late phase: `cullMeshlets` covers recovered instances (frustum + cone + fresh pyramid); `cullRejectedMeshlets` redoes only the occlusion test of the set bits, with current transforms.

Runtime:

- [ ] Turn fast and walk around pillars in Sponza: nothing missing for a frame; "late visible" spikes briefly.
- [ ] Compare GPU time with 3.5 (occlusion off) in an occluded Sponza interior and in an open view. The early draw should drop; the cost is the early occlusion fetches plus the late candidate pass.

### 3.7 Invalidation fallback

Implemented:

- [x] `occlusionHistory` returns `NoPyramid`, `CameraCut`, `SceneChanged` or `Valid` (`CameraFrozen` until A7, see [culling_comparison.md](../reference/culling_comparison.md)). Anything but `Valid` drops `CullFlag::Occlusion` from the early view.
- [x] The fallback count is in the culling panel and in the `Culling/HistoryValid` Tracy plot.

Runtime:

- [x] First frame falls back ("no pyramid"); the early phase draws every frustum + cone survivor and the late phase has no candidates.
- [ ] Resize: fallback count +1, nothing missing.
- [ ] `focusOn` or FOV scroll: fallback count +1 per change, nothing missing.
- [ ] Freeze the culling camera: history stays "valid (pyramid frozen)", the pyramid is not rebuilt, and occlusion keeps culling against the freeze frame (A7; checked in a temporary auto-freeze build, still to check by flying out).
- [ ] Load a model: one "scene changed" frame.

### 4. ImGui and timings

Implemented:

- [x] Toggles: instance frustum, frustum, normal cone, occlusion, counters, freeze culling camera, and "Meshlets / sequence". The per-triangle toggles are listed under stage 5.
- [x] Instance and meshlet tables per test × phase (early / late / frame), the occlusion-history line and the CPU prepare time.
- [x] Debug views "Cull phase" and "Hi-Z pyramid" (with a mip slider).
- [x] Nine GPU timestamp scopes with 120-frame rolling averages and a GPU total.
- [x] Tracy plots per scope and the `Culling/*` plots.

Runtime:

- [ ] "Cull phase" view: all blue with a static camera; orange flashes on fast turns and when stepping out from behind occluders.
- [ ] "Hi-Z pyramid" view: mip 0 matches the scene silhouette; each mip slider step halves the resolution and only ever darkens toward far (max reduction).
- [ ] Table sums with counters on, early column: instances Tested = Frustum culled + Sub-pixel culled + Occluded + Drawn; meshlets Tested + Not tested = Frustum + Cone + Sub-pixel + Occluded + Visible. Late column: instances Tested = Occluded + Drawn.
- [ ] Tracy shows the nine scope plots and the `Culling/*` plots.

### 5. Per-triangle culling (2D)

Implemented:

- [x] Both mesh shaders store the clip positions in groupshared memory, then flag culled triangles with `SV_CullPrimitive` (output counts stay as set, with no compaction). Shared helper: [triangle_cull.slang](../shaders/geometry/triangle_cull.slang). The SPIR-V of both carries `CullPrimitiveEXT`.
- [x] The backface test runs on the triangle after `loadFrontFacingTriangle`, so it honors `FlipWinding`, and the double-sided group skips it.
- [x] Small-primitive test: the bbox, widened by the subpixel snap (half a step of `subPixelPrecisionBits`), misses every pixel center. It reads the clip positions the rasterizer gets, so the DLSS jitter is included once it is in `proj` (there is no jitter yet).
- [x] A triangle with any corner `w <= 0` is never culled.
- [x] Toggles "Triangle backface" and "Small triangles" in the culling panel, both off by default.
- [x] With counters on: tested / backface / small per phase in a triangle table, `Culling/TrianglesBackface` and `Culling/TrianglesSmall` Tracy plots.
- [x] The counters get a host-read barrier after the late raster; the readback relied on the fence alone before.

Runtime:

- [ ] Same image with each toggle on and off, including mirrored instances and double-sided materials; no holes at silhouettes or on thin geometry.
- [ ] Triangle table: Backface + Small ≤ Tested; with only double-sided geometry in view, Backface stays 0.
- [ ] Compare the Draw averages with each toggle on and off in Sponza and in the many-instance scene. Enable a test by default only if it wins on both GPUs.

### 6. Later, separate projects

- [ ] Visibility buffer + compute shading with DLSS: planned in [engine_spec.md](./engine_spec.md) §8.3 and §9. It supersedes the 5A forward decision above.
- [ ] Cluster LOD shared with Mega Geometry: after the final goal ([engine_spec.md](./engine_spec.md) §16.3).
