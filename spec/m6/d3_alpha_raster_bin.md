# M6 · D3: alpha-tested raster bin

**Status:** planned 2026-11-20 · **Milestone:** M6
**Implements:** engine spec §8.1 (A6 bins), §8.2; plan D3 ([pbr_implementation_plan.md](../pbr_implementation_plan.md))
**Depends on:** [D1](./d1_omm_bake.md) (the qualification rule), M1 vis buffer, M0 two-phase culling and DGC bins
**Considerations:** C-D-1 (the raster half of it: today alpha-masked materials raster opaque)
**Reference:** `vk_lod_clusters` / niagara raster bins; the engine's own `RasterBin` and DGC per-bin executes (`src/render/vk_dgc.cpp`)

Route alpha-tested primitives to `RasterBin::AlphaTested` and give that bin a mesh and fragment shader pair that tests base-color alpha and discards. The opaque bin is unchanged and still fetches no material.

## Scope

| Item | Today | After D3 |
|---|---|---|
| Draw-record bin | always `Opaque` (`vk_meshlet_culling.cpp`, "alpha-masked materials stay opaque until the M6 alpha-tested bin") | `AlphaTested` when D1's rule qualifies the primitive's material, else `Opaque` |
| Shaders | one pair: `mesh_raster.slang` + `mesh_fragment.slang` (payload only) | opaque pair unchanged; alpha pair: mesh variant with UV + material outputs, `mesh_alpha_fragment.slang` |
| DGC | per-bin executes, one shared state command buffer | one state command buffer per bin, each built from exactly the shaders and state its execute binds |
| Vis buffer, Hi-Z, resolve | — | unchanged: the payload written by a surviving fragment is the same |

## Decisions

| Decision | Rule | Why |
|---|---|---|
| Qualification | one function shared by D1 and the bin choice: `isAlphaTested(material)` = Mask, or Blend, with a base-color texture | raster and rays must agree on which surfaces are cut |
| Bin granularity | per primitive; a cluster group never spans primitives | the spec's `ALPHAMASKED` / `*_MIXED` state bits (§8.1 A6) are not needed; recorded as a spec note, no code |
| Mesh variant | one `meshMain` with a link-time constant `kIsAlphaTested`: per-vertex `float2 uv` (base-color UV set, `uvMatrices[baseColor]` applied per vertex), per-primitive `nointerpolation uint materialRow` | Slang specialization, no preprocessor permutation (S9); the affine transform commutes with interpolation |
| Fragment | `alpha = baseColorFactor.a × Sample(baseColor, uv, bias = lodBias).a`; `discard` when `alpha < cutoff` (Mask: `alphaCutoff`, Blend: 0.5); then the payload write | implicit-LOD `Sample` is allowed in a fragment shader; `lodBias` (H1) keeps the mip the resolve picks, so the cut edge matches the shaded alpha |
| Sampler | the material's base-color sampler from the sampler heap, as `sampleMaterial` | same texel lookup as shading |
| Cutoff compare | `alpha < cutoff` discards | glTF: "rendered fully opaque if alpha ≥ cutoff"; matches D1's `LessEqual` choice within one ULP |
| Two-sided | the existing winding flip (`DrawRecordFlag::TwoSided`) | one cull mode for both bins |
| Early-Z | lost for the alpha bin (`discard`) | expected; the bin is measured, not avoided |
| Order | opaque execute before alpha execute in each phase (already the `kRasterBins` order) | opaque depth is written first, so occluded alpha fragments fail the depth test and cost less |
| Blend | not blended: Blend materials are cut at 0.5 | plan D; transparency is post-1.0 |

## 1. Tasks

1. `isAlphaTested` in the material code (C++), used by `vk_meshlet_culling.cpp` and D1.
2. `mesh_raster.slang`: `kIsAlphaTested` link-time constant; when set, write `uv` and `materialRow`. `MeshVertexOut` stays position-only for the opaque pair; the alpha pair uses `MeshAlphaVertexOut` (types.slang) so the opaque fragment interface is unchanged.
3. `shaders/passes/mesh_alpha_fragment.slang` (new).
4. `vk_shader_objects.{hpp,cpp}`: a second `MeshShaderPair`; `bindMeshState(cmd, RasterBin)`.
5. `vk_dgc.cpp`: a state command buffer per bin; preprocess and execute bind the bin's pair. The DGC preprocess / execute state must match exactly (validation catches mismatches).
6. UI: the existing per-bin rows show alpha-tested sequence counts.

## 2. Verification

- `AlphaBlendModeTest`: the Mask and Blend panels cut out as the Khronos screenshot shows (Blend cut at 0.5, so it differs from the blended screenshot by design).
- Guide capture: every view on Sponza; the plants' cut-outs in `Base color` and `Depth`.
- `ENGINE_CULL_CHECK`: still bit-identical on vs off (16 poses).
- `ENGINE_MOTION_TEST`: passes; alpha pixels have the same motion as before.
- Timing (release, no layers, 1280×720, 5070): raster per bin and phase before and after; the opaque bin must not get slower.

## Done when

- alpha-tested primitives raster through the alpha bin with `discard`, and `AlphaBlendModeTest` cut-outs are correct;
- the opaque bin's fragment shader still fetches no material;
- cull check, motion test and validation clean; timings recorded.

## Must not

- test alpha in the resolve, or premultiply alpha;
- add material reads to the opaque bin;
- use one DGC state command buffer for both bins.

## Log

| Date | Change |
|---|---|
| 2026-11-20 | Spec written; `*_MIXED` bits dropped (groups never span primitives) |
