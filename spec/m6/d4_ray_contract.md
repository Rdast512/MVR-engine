# M6 · D4: ray contract

**Status:** built 2026-11-20; `pointOffset` and `minT` not needed (terminator shot, 0 self-hits); M6 complete · **Milestone:** M6 (closes it)
**Implements:** engine spec §10.5, §11.1; feature spec [ray_tracing_mega_geometry.md](../features/planned/ray_tracing_mega_geometry.md) §6; plan D4 ([pbr_implementation_plan.md](../pbr_implementation_plan.md))
**Depends on:** [D2](./d2_micromap_clas.md) (OMMs on rays), [D3](./d3_alpha_raster_bin.md) (cut-outs in raster), F2 (`traceVisibility`), C3 (`traceClosest`), G1 (SER trace)
**Considerations:** C-D4-1 (closed here), C-D-1 (closed here), C-C4-3 (stays)
**Reference:** nvpro `vk_raytrace` `render_shading.glsl` (`pointOffset` :510, `minT` :570); RTXMG `self_intersection_avoidance.hlsli` (`SafeSpawnPoint`, already ported as `safeSpawnPoint`)

Finish the ray rules that alpha geometry tests: no ray ever sees a candidate, secondary origins do not self-shadow, and every self-test runner covers an alpha scene.

## Scope

| Rule | Today | After D4 |
|---|---|---|
| Candidates | none possible: every instance is `FORCE_OPAQUE` | none possible: alpha instances resolve through 2-state OMMs (opaque or ignored), others stay `FORCE_OPAQUE`. Debug builds count candidates and assert zero |
| Ray flags | `traceVisibility`, `traceClosest`, the SER trace set `RAY_FLAG_FORCE_OMM_2_STATE` via `requireOmm2StateRayFlag()` | unchanged; any new trace function must call it (C-C4-3) |
| Shadow origins | `rasterRayOrigin` (`offsetRay` rule) on raster surfaces, `safeSpawnPoint` on ray hits | the same, plus `pointOffset` if §1.2 shows terminator artifacts, and `minT` if §1.3 shows self-hits |
| Self tests | no alpha scene | an alpha shot or phase in the guide capture and the RT check |

## Decisions

| Decision | Rule | Why |
|---|---|---|
| Candidate counter | debug builds (`ENGINE_DEBUG_RAYS` link-time constant, off in release): after `Proceed()`, `CandidateType()` of a still-running query adds 1 to a counter in the frame's debug buffer; the host logs a nonzero count once per frame as an error | proves the "no any-hit, no candidate loop" contract holds with OMMs; release shaders keep one `Proceed()` and no branch |
| One helper per ray kind | `traceVisibility` for visibility, `traceClosest` / the SER trace for shaded rays; no other `TraceRayInline` call | one place carries the flags and the counter |
| `pointOffset` | built only if the terminator shot (§1.2) shows shadow-terminator banding on Sponza; otherwise not built, with the shot recorded in the feature spec §6.1 | the plan lists it; the engine's raster origins have no triangle, so it costs a payload decode and three vertex fetches per ray. Build it when an image needs it |
| `minT` | built only if the self-hit count of §1.3 is nonzero after OMMs; otherwise `tMin = 0` stays | F2 and RT4 measured 0 self-hits over ~600 M rays with the current offsets |
| Alpha origins | the same offsets on alpha surfaces; the 2-state OMM decides a leaf's own triangle like any other | a ray leaving a cut-out micro-triangle of its own triangle is offset off the plane, so it cannot re-hit it |
| RT check classes | primary rays vs vis buffer on alpha primitives: a mismatch within one micro-triangle edge of a cut-out is the class `alpha-edge`, logged and not a failure; any other alpha mismatch fails | the OMM resolves alpha per micro-triangle (`dynamicSubdivisionScale` = 2 texels), the raster per texel and mip; they differ only along edges |

## 1. Tasks

1. **Candidate counter** in `raytracing/trace.slang` for both helpers and the SER trace; debug-buffer slot and host log in the renderer. Remove the "until plan D" comments (`geometry/hit.slang:76`, `rt_instances.slang`).
2. **Terminator shot:** the guide capture gets a Sponza pose with the sun at grazing angle on the columns (`Direct lighting` view). Inspect for faceted shadow terminators; decide `pointOffset` per the rule above and record it.
3. **Self-hit count:** rerun the RT check's hemisphere-ray self-hit count with OMMs on Sponza and `AlphaBlendModeTest`; decide `minT` per the rule above.
4. **Alpha coverage:**
   - guide capture: an `AlphaBlendModeTest` shot (`Base color`, `Direct lighting`, `Depth`) and the Sponza plants shot under the sun;
   - RT check: an `AlphaBlendModeTest` pose with the `alpha-edge` class;
   - GI check: unchanged poses; its numbers move because plant shadows change. Record the new numbers; the gates must still pass.
5. **Docs:** feature spec §6.1–6.2 "as built", engine spec §10.5 (built), `docs/settings.md` rows for the new captures, considerations closures.

## 2. Verification

| Check | Expected |
|---|---|
| Debug build, every runner | candidate count 0 every frame |
| `ENGINE_RT_CHECK` | passes; `alpha-edge` count logged, other alpha mismatches 0 |
| `ENGINE_GUIDE_CAPTURE` | all views written; alpha shots show cut-outs in raster and in shadows, matching each other |
| `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST` | unchanged results |
| `ENGINE_GI_CHECK` | 8 of 8 |
| Validation, GPU-AV | clean |
| Timing (release, no layers, 1280×720, 5070) | ray passes before and after OMMs on Sponza; recorded with the bake time and memory of D1 / D2 |

## Done when

- the candidate counter reads 0 in every runner, and the self-test runners pass with the alpha shots;
- `pointOffset` and `minT` are each built or recorded as not needed with the evidence;
- C-D4-1 and C-D-1 are closed, and M6's README acceptance list holds.

## Must not

- write a candidate loop, an any-hit shader, or `RAY_FLAG_FORCE_OPAQUE` on any ray;
- trace without `requireOmm2StateRayFlag()`.

## Log

| Date | Change |
|---|---|
| 2026-11-20 | Spec written; `pointOffset` and `minT` gated on measured need |
| 2026-11-20 | Built. **Counter:** `raytracing/trace.slang` has `kDebugRays` (`[vk::constant_id(1)]`, set to true through `pSpecializationInfo` in `createComputeShader` when `!NDEBUG`) and `countCandidate(rayDebug, query.Proceed())`; `traceVisibility` takes a `RayScene` (`tlas`, `rayDebug`, from `loadRayScene(root)`), and `traceClosest` drops its `while (Proceed())` loop for the same single call. `GpuRayDebugCounters` (4 B) sits in a per-slot host buffer `RayDebugCounters_<slot>` reached through `GpuFrameRoot.rayDebug` (0 in Release); the renderer reads and zeroes it after the slot's fence, logs a nonzero frame as an error (`[RayContract]`) and the total at shutdown; a compute → host barrier ends the ray commands. **Alpha coverage:** guide shots `AlphaBlendModeTest_sun` (sun 60° / 35°), `Sponza_plants` (vase plant under the sun, 20° / 65°) and `Sponza_terminator` (upper colonnade, sun 20° / 40°); `GuideCapture::Shot` gains an optional `CameraPose` and `SunAngles`, and `applyGuideCaptureRequest` enables the sun only for shots that set it. **RT check:** `alpha-edge` class (`GpuRayCheckStats::alphaEdges`) for misses, id mismatches outside snap edges and depth ties, and empty hits, when `isNearAlphaCut` finds mip-0 alpha on both sides of the threshold within `microEdge + footprint + 1` texels of the point. `isAlphaTested` and `baseColorGradients` are added to `material/pbr.slang`. The "until plan D" comments are gone |
| 2026-11-20 | Deviations: (1) the counter switch is a SPIR-V specialization constant set by the host build type, not a link-time constant `ENGINE_DEBUG_RAYS`: one `.spv` serves both builds, and Release specializes the branch away. (2) The counter counts queries whose one `Proceed()` returns true, which is the "still-running query" of the spec without a `CandidateType()` read. (3) `traceVisibility` takes a `RayScene` instead of a bare TLAS address, so the counter address travels with it. (4) The alpha-edge radius is recomputed from the OMM SDK's dynamic subdivision (`GetDynamicSubdivisionLevel`, scale 2, level ≤ 5): the micro-edge is the triangle's longest uv edge in texels over `2^N`. A first version with two rings of 8 samples was not monotonic in the radius (doubling it raised the leftover mismatches 1 → 5); a 1-texel grid over the disk is (≤ 64 steps per axis). (5) No `Depth` capture for the alpha shots: the guide views have none; `Base color`, `Shadowed lights` and the other views are written |
| 2026-11-20 | Decisions. **`pointOffset` not built:** `Sponza_terminator` `Shadowed lights` shows smooth shading across the colonnade shafts and arches at a 40° sun with no faceted or banded terminator. **`minT` not built:** the RT check reads 0 self-hits over the full guide capture with OMMs (24.4 M interior pixels, 438 frames), so `tMin = 0` stays. Recorded in the feature spec §6.1 |
| 2026-11-20 | Verification (RTX 5070). Debug, every runner: `Ray contract: 0 ray-query candidates` (RT check 32 of 32, 32 frames; cull check 16 of 16, 454 frames; motion test 13 of 13, 318 frames; guide capture + RT check 315 PNGs, normal checks 2 of 2, convergence passed, RT check 438 of 438, 438 frames). Negative test: alpha instances forced `FORCE_NO_OPAQUE` (0x8) log ~1.78 M candidates per frame; reverted. Release: RT check on Sponza 32 of 32 with ~106 alpha edges per frame; guide capture + RT check 438 of 438, 34,792 alpha edges, 0 self-hits. Negative test: with the micromaps detached, 464 id mismatches stay outside the class against 696 alpha edges and 0 of 32 frames pass. `AlphaBlendModeTest_sun` `Shadowed lights`: each panel's shadow is cut at its marker height like its raster image; `Sponza_plants`: leaf-shaped shadows on the vase and floor |
| 2026-11-20 | Timing, Sponza bench pose (release, no layers, 1280×720, 600 frames, two runs each), ms. With OMMs / micromaps detached: ReSTIR DI initial 0.176–0.178 / 0.163–0.168, DI temporal 0.075–0.076 / 0.072–0.073, DI shade 0.099 / 0.089–0.091, indirect 0.588–0.594 / 0.543–0.556, GI temporal 0.124–0.126 / 0.115–0.118, GI shade 0.452–0.468 / 0.436–0.443, GPU total 3.031–3.035 / 2.853–2.918: the ray passes cost 4–8% more with micromaps. The single `Proceed()` against D3's `while (Proceed()) {}` loop: GPU total 2.971–2.974 ms, within the ±5% noise. Load cost (D1 bake, D2 micromap build 30–120 ms, C-D2-1) accepted |
| 2026-11-20 | `ENGINE_GI_CHECK` (release, no layers): 8 of 8. Gated phases, gallery / courtyard: noise floor +0.10% / +0.01%, ReSTIR GI on traced paths +0.38% / +1.42% (limit 1.5%), one-bounce noise floor −0.10% / −0.02%, ReSTIR GI mode against one bounce +0.11% / +0.98%. Measured: NRC + ReSTIR GI converged +8.26% / +10.53%, NRC mode +9.18% / +8.76%. The courtyard traced-path gate sits near its limit, as before D4 (C-G5-1) |
| 2026-11-20 | Validation (release, core + sync + best practices + NVIDIA + GPU-AV) over the guide capture with the RT check: 315 PNGs, 438 of 438, only the known messages (`BestPractices-SpirvDeprecated_WorkgroupSize` ×3, NGX's leaked fences at `VUID-vkDestroyDevice-device-05137`, GPU-AV's settings notices). Considerations: C-D4-1 and C-D-1 closed, C-D2-1 accepted, C-D3-1 narrowed to the bin choice reading D1's result |
