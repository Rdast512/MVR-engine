# Shader audit: correctness and performance

**Date:** 2026-11-11 · **Tree:** `feature-pbr` on `bfb8d1587` plus the working tree
**Related:** [engine_spec.md](./engine_spec.md) §20 (architecture audit), §4 (shader rules S1–S11)

**Method:** every compiled shader and every module it imports was read line by line: `passes/` (`meshlet_cull`, `mesh_raster`, `mesh_fragment`, `hiz_build`, `resolve`, `debug_view`, `present`, `hiz_view`), `core/`, `geometry/`, `material/`. Nothing was compiled, run or measured for this audit.

- Correctness findings come from reading the code against the math and the references named in it.
- Performance findings are hypotheses. Per P5 none of them lands without GPU timer numbers on a release build with validation off.
- Not re-read here: `types.slang` (its layouts were compared with C++ in the architecture audit and matched), the stub files (to be deleted, §20.2 AU7), `gi/sharc.slang`.

---

## 1. Summary

| | Count | Most important |
|---|---|---|
| Correctness, likely wrong | 1 | back faces of two-sided normal-mapped surfaces get a wrong tangent sign (C1) |
| Correctness, robustness | 4 | NaN paths in the BRDF and the surface rebuild; raster and resolve compute clip positions differently |
| Correctness, will break later | 2 | normal-map Z when BC5 arrives; reject mask width tied to a constant |
| Performance | 9 | group-uniform tests repeated in every lane of the cull passes; long dependent-load chain in the mesh shader and the resolve; interleaved vertex stream |

No finding affects the culling results: the two-phase logic, list appends and overflow accounting are correct as written (§4).

---

## 2. Correctness

| ID | Finding | Location | Effect | Fix |
|---|---|---|---|---|
| C1 | **Back-face tangent is not negated.** For a back face the normal is flipped and the bitangent follows it (`cross(-n, t) · w`), but the tangent stays as it is. The Khronos sample renderer negates all three (`t`, `b`, `ng`) for back faces, which makes the back-face shading normal the exact negation of the front-face one | [pbr.slang:124-126](../shaders/material/pbr.slang#L124-L126) | on the back side of a two-sided, normal-mapped surface the normal's component along the tangent has the wrong sign: bumps are lit from the wrong side in that direction | negate `t` for back faces as well. Confirm on a two-sided normal-mapped asset viewed from behind (Sponza foliage, `NormalTangentMirrorTest` made two-sided) |
| C2 | **Raster and resolve build clip positions with different expressions.** The mesh shader uses `proj · (view · world)`; the resolve uses `viewProj · world`. The results differ by rounding | [math.slang:23-26](../shaders/core/math.slang#L23-L26), [mesh_raster.slang:40](../shaders/passes/mesh_raster.slang#L40), [surface.slang:28](../shaders/geometry/surface.slang#L28) | `TriangleCorners.clip` is documented as "as the rasterizer saw them" but is not bit-identical, so barycentrics are off by rounding and can leave [0, 1] at triangle edges. Also one extra 4×4 multiply per mesh-shader lane | one helper used by both, built on `camera.viewProj` |
| C3 | **`fresnelSchlick` can receive a cosine above 1.** `specularProbability` passes `dot(n, v)` of two unit vectors, which rounding can push to 1 + ε; `pow` of a negative base is undefined in SPIR-V | [brdf.slang:32](../shaders/material/brdf.slang#L32), [:88](../shaders/material/brdf.slang#L88) | a NaN lobe probability at exactly head-on views; `sampleBrdf` rejects the sample (`!(pdf > 0)`), so the result is a dropped sample, not a crash | `pow(saturate(1.0 - cosine), 5.0)` inside `fresnelSchlick` |
| C4 | **Unguarded normalizations in the surface rebuild.** `geoNormal` normalizes a cross product that underflows for extreme slivers. `edgeWeights` divides by a weight sum that is zero when the neighbouring pixel lies on the triangle plane's horizon line | [surface.slang:95](../shaders/geometry/surface.slang#L95), [visbuffer.slang:39](../shaders/geometry/visbuffer.slang#L39) | rare NaN in the `GeoNormal` guide, or infinite UV gradients passed to `SampleGrad` (undefined mip choice) | fall back to the vertex normal when the cross product's squared length is below a threshold; clamp the gradient magnitude |
| C5 | **Normal-map Z is read from the texture.** `encodedNormal * 2 − 1` uses the blue channel | [pbr.slang:120-122](../shaders/material/pbr.slang#L120-L122) | correct for today's BC7 and PNG normal maps. With the BC5 normal maps of spec §6.4 the blue channel reads 0, so Z becomes −1 | reconstruct `z = sqrt(saturate(1 − x² − y²))` when the BC5 path lands (M8), keyed by a material flag |
| C6 | **Reject mask width is tied to a constant by convention only.** `1u << lane` needs `kMeshletGroupMaxSize ≤ 32` | [meshlet_cull.slang:282](../shaders/passes/meshlet_cull.slang#L282), [:344](../shaders/passes/meshlet_cull.slang#L344) | silent wrap if the group size is ever raised | a static assert next to the constant |
| C7 | **Heap indices lose `NonUniform`.** Known (spec S6, Q5): Slang drops the decoration, NVIDIA tolerates it | [pbr.slang:64-65](../shaders/material/pbr.slang#L64-L65) | undefined by the spec | already tracked; re-probe on each Slang upgrade |

**Applied 2026-11-12** (debug and release build clean, `ctest` 5/5; the image checks are still open):

| ID | Change | Open check |
|---|---|---|
| C1 | `sampleMaterial` negates `t` for back faces, after `b` is built from the flipped normal | two-sided normal-mapped asset viewed from behind |
| C2 | `objectToClip` is `viewProj · world`; raster and `loadTriangleCorners` both call it | — |
| C3 | `fresnelSchlick` saturates `1 − cosine` | — |
| C4 | `buildSurface`: the geometric normal falls back to the vertex normal below a squared length of 1e-30 (to a fixed unit vector when neither exists). `edgeWeights` bounds the weight sum to 1e-6 of the weights' magnitude, so neighbour barycentrics and UV gradients stay finite | — |
| C5 | normal-map Z is always rebuilt from XY, with no material flag: one path for BC7/PNG today and BC5 later. Mip-averaged (shortened) texels now come out slightly flatter than before | normal-mapped asset at distance, before/after |
| C6 | `static_assert(kMeshletGroupMaxSize <= 32)` in `cullMeshlets` and in `types.hpp`. Slang 2026.20 accepts `static_assert` only as a statement, not at module scope; a probe confirmed it fires | — |
| C7 | re-probed: `resolve.spv` from Slang 2026.20 has no `NonUniform` decoration on the heap access chains. Q5 stays open | next Slang upgrade |

---

## 3. Performance

Ordered by expected gain. Each needs a before and after number on both cards.

| ID | Finding | Location | Why it costs | Option |
|---|---|---|---|---|
| P1 | **Instance cull repeats one test in 64 lanes.** A workgroup handles one instance; every lane loads the frame root, the cull view, the node instance, the model and the node, projects the same 8 corners and takes the same Hi-Z sample. The lanes are only needed for the record copy at the end | [meshlet_cull.slang:66-138](../shaders/passes/meshlet_cull.slang#L66-L138) | 64× the arithmetic and texture work per instance. It scales with instance count, so the instance-field scene shows it, Sponza does not | lane 0 tests and publishes the verdict through `groupshared`; the other lanes wait at the existing barrier. Or one lane per instance with the record copy as a second pass |
| P2 | **Meshlet cull repeats the group tests in 32 lanes.** The group box is projected for frustum and again for occlusion in every lane before the per-meshlet tests | [meshlet_cull.slang:202-219](../shaders/passes/meshlet_cull.slang#L202-L219) | roughly doubles the per-lane arithmetic of the pass and adds 31 redundant Hi-Z samples per group | same as P1: lane 0 computes the group verdict; the pass already has group barriers in the early phase |
| P3 | **Late phase projects each box twice with the same matrix.** In the late phase `occlusionViewProj` and `cullViewProj` are the same view and `occlusionWorld` is `world`, yet `isBoxOccluded` re-projects the corners the frustum test just projected | [meshlet_cull.slang:21-26](../shaders/passes/meshlet_cull.slang#L21-L26), [:216-253](../shaders/passes/meshlet_cull.slang#L216-L253) | 8 matrix-vector products and 8 divides per box, per lane | pass the `ClipBounds` of the frustum test to the Hi-Z test when `isLate` |
| P4 | **Seven dependent loads before the mesh shader can start, in all 32 lanes.** root → visible row → node instance → model → `meshletPrimitives` → primitive → material row. The material row (240 B, random access) is read only for the two-sided bit | [frame.slang:21-36](../shaders/core/frame.slang#L21-L36), [mesh_raster.slang:33-40](../shaders/passes/mesh_raster.slang#L33-L40) | each hop is a full memory latency; the last two exist for one bit | copy the two-sided bit into `GpuPrimitive` at load (or a spare bit of the visible row). That removes one hop from raster and one from the resolve's draw context |
| P5 | **Positions are interleaved with shading attributes.** `GpuVertex` is 32 B (position, color, UV0); the mesh shader reads 12 B of it | [mesh.slang:19-22](../shaders/geometry/mesh.slang#L19-L22), [mesh_raster.slang:44](../shaders/passes/mesh_raster.slang#L44) | 2.7× the vertex bandwidth in raster; vertex color is white for almost every asset | a positions-only stream, as spec §6.3 plans. M2 needs it anyway as the CLAS build input |
| P6 | **The resolve loads each vertex twice and the material row twice.** `loadTriangleCorners` loads the three vertices for their positions and `buildSurface` loads them again for color and UV; `loadDrawContext` reads the material flags and the resolve then loads the whole row | [surface.slang:35](../shaders/geometry/surface.slang#L35), [:104](../shaders/geometry/surface.slang#L104), [resolve.slang:76](../shaders/passes/resolve.slang#L76) | repeated loads through BDA pointers are not guaranteed to be merged by the compiler | keep the loaded `GpuVertex` in `TriangleCorners`; with P4 the flag read disappears |
| P7 | **Large structs are copied whole into locals.** `GpuCameraData` (512 B), `GpuCullView` (288 B), `GpuFrameRoot` (176 B), `GpuNodeInstance` (144 B) and `GpuModel` (112 B) are read with `ptr[0]` in every lane, though most passes use a few fields | every pass, for example [mesh_raster.slang:39](../shaders/passes/mesh_raster.slang#L39) | if the compiler emits a whole-struct load, each lane fetches hundreds of unused bytes | inspect the emitted SPIR-V of `meshMain` first. Where a whole-struct `OpLoad` survives, read fields through the pointer |
| P8 | **Hi-Z depth nudge is absolute.** `kHiZDepthNudge` is two ulps at depth 1. Under reverse Z far depths are near 0, where two real surfaces can differ by less than that | [culling.slang:45](../shaders/core/culling.slang#L45), [:220](../shaders/core/culling.slang#L220) | conservative, never wrong: distant occludees behind distant occluders are drawn | a nudge relative to the sampled depth; check the cull counters on a long view before and after |
| P9 | **Hi-Z mip reduction uses four `Load`s per texel** | [hiz_build.slang:48-58](../shaders/passes/hiz_build.slang#L48-L58) | minor: four fetches where one gather or one min-reduction sample would do | only if the Hi-Z rows of the timer show it |

One more behaviour to be aware of, not a cost today: a box that was outside the **previous** frustum has its clip rect clamped to the screen edge, so the early Hi-Z test compares it against an edge texel ([culling.slang:116-122](../shaders/core/culling.slang#L116-L122)). It can be classified as occluded and then drawn in the late phase. The image is correct; geometry entering the screen during a fast turn may shift from the early to the late phase.

---

## 4. Checked and correct

| Area | What was checked |
|---|---|
| Frustum test | clip bits per corner, AND for outside, OR for near and eye-plane crossing; near bit follows reverse Z; boxes crossing the near plane are never occluded or sub-pixel |
| Hi-Z test | mip choice and the one-finer refinement keep the 2×2 footprint of a center sample over the whole rect; nearest box depth against the farthest stored depth with the right sign |
| Sub-pixel and small-triangle tests | pixel-center index range with the subpixel snap; conservative for corners with `w > 0`; corners behind the eye keep the triangle |
| Cone test | object-space camera through the adjugate inverse; valid under non-uniform scale and mirroring; matches meshoptimizer's sphere form |
| Two-phase handover | candidates and reject masks are written for every early record; late re-tests use current matrices; history-invalid frames leave empty masks |
| List appends | one atomic per wave, first-lane base plus prefix count; every append checks its capacity and counts overflow; dispatches that include dropped items return early; DGC sequence count is clamped to the bin capacity |
| Barriers | every `GroupMemoryBarrierWithGroupSync` is in group-uniform control flow; no wave-size assumption |
| Mesh shader | counts clamped to the output limits; winding flip for mirrored instances and for two-sided back faces; per-vertex output is position only |
| Hi-Z build | footprint of 1 to 3 depth texels per axis covers every source texel; edge clamping leaves the reduction unchanged; levels written only inside their size |
| Barycentrics and motion | homogeneous edge functions; derivatives as neighbour differences on the triangle plane; motion from unjittered matrices, zero on a history reset; sky pixels move with rotation only |
| Normals | cofactor normal matrix with the determinant sign; tangent handedness under mirroring; octahedral encode and decode |
| BRDF | glTF appendix B diffuse and specular terms, GGX D, height-correlated visibility, Dupuy–Benyoub VNDF sampling and its pdf, one-sample lobe mixture weight, Karis environment fit |
| Sampling | no implicit-LOD `Sample` in compute or mesh stages; `SampleGrad` with analytic gradients in the resolve |

---

## 5. Suggested order

1. ~~C1–C7~~ applied 2026-11-12 (§2); confirm C1 and C5 on assets.
2. ~~C2 and C3~~ (done).
3. Record the M0/M1 timings the spec still owes (§16.2), then measure P1–P3 on the instance-field scene and P4–P7 on Sponza.
4. P4 and P5 together with M2: the two-sided bit and the position stream both touch the load path that M2 changes for CLAS.
5. ~~C5 with the BC5 path (M8); C4 and C6 whenever those files are next edited~~ (done).
