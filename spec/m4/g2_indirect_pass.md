# M4 · G2: indirect pass

**Started:** 2026-11-16 · **Status:** built 2026-11-16; compute + `traceClosest` since 2026-11-17 (SER shelved, C-G1-3); 2060 Super numbers open · **Milestone:** M4
**Implements:** engine spec §11.3; plan G2 ([pbr_implementation_plan.md](../pbr_implementation_plan.md))
**Depends on:** C3 (`traceClosest`; [G1](./g1_ser_rt_pipeline.md)'s `traceShaded` while SER was in use), E1 (guides), F1–F2 (RTXDI context, bridge), C3 (`decodeHit`, `safeSpawnPoint`)
**Used by:** [G3](./g3_nrc.md) (query pass, resolve completes the samples), [G5](./g5_restir_gi.md) (initial reservoirs)
**Related:** RTXDI 3.2 `Include/Rtxdi/GI/Reservoir.hlsli`, `ReSTIRGI.h`

One BRDF-sampled ray per pixel from the primary surface, shaded at its hit X₂ with sampled direct light and emission, stored as the pixel's **initial ReSTIR GI reservoir**. G5 resamples those reservoirs; G3 adds the cache term for bounces 2…∞ into them.

---

## 1. Decisions

| Decision | Rule | Why |
|---|---|---|
| Output | RTXDI GI reservoirs (`RTXDI_PackedGIReservoir`), written with `RTXDI_MakeGIReservoir(X₂, n₂, Lo(X₂), pdf)` at `secondarySurfaceReSTIRDIOutputBufferIndex`, for every pixel (empty where nothing was traced) | the RTXDI sample's secondary-surface pass does the same; G5 reads them with no conversion pass |
| GI host state | `RtxdiContext` gains a `ReSTIRGIContext` (checkerboard off), the GI reservoir buffer (2 × render-size reservoirs) and GI fields in `GpuRtxdiConstants`. Resampling mode `None` until G5 | the buffer layout and indices come from the library; one RTXDI host module |
| Cache term | absent until G3. Lo(X₂) = emission + direct light at X₂ | G3's resolve adds `throughput × cachedRadiance` to the stored radiance; the reservoir weight is `1/pdf` and does not depend on radiance, so a later add is valid |
| Direction | `sampleBrdf` at the primary guide surface (VNDF), solid-angle pdf; RNG seed `RTXDI_GI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED` | same sampler RTXDI's BRDF rays use (`RAB_SurfaceImportanceSampleBrdf`) |
| Origin | `rasterRayOrigin` (geometric-normal offset of the bridge) | the primary surface is a raster pixel, no traced triangle |
| Trace | `traceShaded(tlas, ray, hit, isActive)` (G1): hit/miss hint; inactive lanes join with a NOP hit object | the reorder never sits in divergent control flow |
| Live state across the trace | the pixel and `PrimaryBounce` (ray, pdf, isActive, TLAS address) only; the frame root, bridge state and RTXDI constants are reloaded after it | whatever is live moves with the thread at the reorder; the reloads hit cache |
| Direct light at X₂ | `RTXDI_SampleLightsForSurface` with the DI initial-sampling parameters, `numBrdfSamples = 0`, no reuse, seed `RTXDI_SECONDARY_DI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED`; then one `traceVisibility` from `safeSpawnPoint` | spec §11.3. No geometry is a light before emissive triangles (M7), so a BRDF candidate would only spend a ray |
| X₂ surface | `rayHitSurface`: `RAB_Surface` from `decodeHit` + `sampleMaterial` (ray-cone level, cone from the primary pixel footprint at the origin's camera distance, no DLSS bias) | the bridge's light functions take any `RAB_Surface`; indirect texture detail is never reconstructed at display resolution |
| Unlit X₂ | emission only | as the resolve: unlit takes no light |
| Miss | zero radiance at `origin + l·kMissDistance` (10⁵), M = 1 | a miss is a valid sample that contributes nothing; M counts it for G5's normalization. No environment light until M7 |
| No primary surface / unlit primary / pdf 0 | empty reservoir | nothing to resample |
| Subgroup ops | none after the trace | C-G1-2 |
| Reorder | UI checkbox, two pipelines; **off by default** (§4) | G1 §5: a runtime branch costs 0.18 ms; reorder measured slower here |
| Floor tier | full resolution for now; half-res checkerboard deferred (C-G2-1) | no tier switch exists, and the 2060 Super is not in the dev machine |

---

## 2. Files

| File | Change |
|---|---|
| `src/render/vk_rtxdi.{hpp,cpp}` | `ReSTIRGIContext`, GI reservoir buffer, GI fields in `GpuRtxdiConstants` (392 B) |
| `shaders/lighting/rtxdi_constants.slang`, `tests/layout_test.cpp` | mirror of the GI fields, offsets checked |
| `shaders/lighting/rtxdi_bridge.slang` | generic bounds-checked `RtxdiReservoirBuffer<T>` for DI and GI (`RTXDI_GI_RESERVOIR_BUFFER`), `rayHitSurface` (X₂ as `RAB_Surface`) |
| `shaders/raytracing/ser.slang` | `traceShaded` takes the TLAS address instead of the frame root, and `isActive` (NOP hit object) |
| `shaders/passes/indirect.slang` | raygen + closest-hit + miss |
| `src/render/vk_indirect.{hpp,cpp}` | `IndirectPass`: two `RtPipeline`s, settings, record with its barriers |
| `shaders/passes/debug_view.slang`, `debug_view.hpp`, `types.slang` | `Indirect samples` view (27) |
| `src/core/engine_guide_capture.hpp` | the guide capture writes `indirect_samples` for every shot |
| `src/render/vk_renderer.{hpp,cpp}`, `vk_gpu_timer.hpp`, `engine_ui.cpp` | record after ReSTIR DI, `Indirect` timer scope and Tracy zone, "Indirect (M4)" UI section replacing the RTXGI placeholder |

---

## 3. Verification

| Check | Result (RTX 5070, driver 621.01) |
|---|---|
| `Indirect samples` view: `evalBrdf · NdotL · Lo(X₂) / pdf` per pixel, the 1-sample estimate of one-bounce indirect light | startup gallery pose, 64-frame average: floor and pillars bounce-lit from the sunlit walls; no leaks (the stepped edge is the curtain hem) |
| Validation: core + sync + best practices, `ENGINE_RT_CHECK=1 ENGINE_GUIDE_CAPTURE=1` | clean apart from the known NGX `SpirvDeprecated_WorkgroupSize` (H1); ray check 258/258, SER check 258/258 both modes, normal checks 2/2, **ReSTIR DI convergence check passed** |
| GPU-AV | device lost on the first frame with BDA instrumentation on; not an engine access fault: see C-G2-2 for the bisect and the coverage that replaces it |
| Unbiasedness against a many-sample one-bounce reference | deferred to G5's ground-truth check (spec §14), which needs the same path code |

---

## 4. Measurements

Release build, no layers, `Indirect` timer scope, 853×480 render resolution, idle GPU, three interleaved rounds (2026-11-17, after the [G1 follow-ups](./g1_ser_followups.md); the 2026-11-16 numbers ran beside a background GPU load).

| Pose | Reorder off | Reorder on |
|---|---|---|
| startup gallery | 0.42–0.43 ms | 0.46–0.48 ms |
| courtyard, `lookAt({6, 2.5, 1.5}, {-10, 1, -1})` | 0.55 ms | 0.57 ms |

- `ReorderThread` costs ~0.067 ms on this pass by itself (trace-only variant: 0.31 → 0.38 ms, 0.34 → 0.41 ms) and saves ~0.045 ms of hit shading at the courtyard, less at the gallery. Off by default.
- No coherence key helps yet: a material-row hint (4, 8, 16 bits) gained nothing on 2026-11-16, the plain `ReorderThread(hitObject)` equals the hit/miss hint, and the closest-hit `Invoke` costs < 1 % (follow-ups §6).
- **Recheck** when the shading diverges: M7 material bins, G3 multi-bounce paths.

---

## 5. Log

| Date | Change |
|---|---|
| 2026-11-16 | created from plan G2 |
| 2026-11-16 | design (§1): output to RTXDI GI reservoirs, GI context in `RtxdiContext`, no cache term before G3, floor tier deferred |
| 2026-11-17 | §4 re-measured on an idle GPU after the G1 follow-ups; reorder off stays the default |
| 2026-11-16 | built: indirect pass, GI reservoirs, debug view, UI; reorder off by default (§4); GPU-AV device lost bisected to driver + BDA instrumentation after a hit-object trace (C-G2-2) |
| 2026-11-17 | SER shelved (considerations C-G1-3): `indirect.slang` is a compute pass (`indirectMain`, 8×8) tracing with `traceClosest`; `IndirectPass` became `recordIndirectPass`; the reorder setting and UI toggle are gone. GPU-AV runs it clean (C-G2-2 moot). Gallery `Indirect` 0.576 ms (best 0.544) with the G3 query, against 0.528 (0.475) through the SER pipeline |
