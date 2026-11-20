# M6 · D4: ray contract

**Status:** planned 2026-11-20 · **Milestone:** M6 (closes it)
**Implements:** engine spec §10.5, §11.1; feature spec [ray_tracing_mega_geometry.md](../features/ray_tracing_mega_geometry.md) §6; plan D4 ([pbr_implementation_plan.md](../pbr_implementation_plan.md))
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
