# M4 · G7: indirect mode toggle

**Status:** not started · **Milestone:** M4
**Implements:** engine spec §11.7; plan G7 ([pbr_implementation_plan.md](../pbr_implementation_plan.md))
**Depends on:** [G3](./g3_nrc.md) (NRC), [G5](./g5_restir_gi.md) (ReSTIR GI), [G6](./g6_combine.md) (combine)
**Considerations:** C-G3-3 (each `Configure` leaks SDK objects), C-G3-5 (`requestReset` ineffective), C-G3-6 (no queue work or SDK calls during NRC / DLSS-RR work)
**Reference:** NVIDIA RTXGI `Samples/IndirectDiffuse` (`reference/RTXGI`): `IndirectDiffuseMode` (`UI.h`) picked by radio buttons in `UI.cpp`, and every mode or resolve change sets `resetRenderHistory`. RTXDI sample: ReSTIR GI without a radiance cache.

A runtime toggle between three ways of producing `indirectRadiance`. NRC stays built and initialized in every mode; the toggle only decides which passes run. It lets each scene and tier be compared on the same frame, as the post-1.0 ray-path toggles do (engine spec §16.5).

## Scope

| Mode | Sample radiance at X₂ | NRC | ReSTIR GI | `indirectRadiance` |
|---|---|---|---|---|
| **NRC → ReSTIR GI** (default) | emission + direct light at X₂ + `prefixThroughput × cachedRadiance` (spec §11.3–11.4) | update paths, `QueryAndTrain`, custom resolve, `EndFrame` | temporal + spatial | ReSTIR GI output |
| **NRC** | the same | the same | skipped | the resolved sample shaded at the primary surface: one sample per pixel, no reuse (RTXGI `IndirectDiffuseMode::Nrc` with RR) |
| **ReSTIR GI** | emission + direct light at X₂ only: one bounce, as the RTXDI sample | paused: no update paths, no `QueryAndTrain`, no resolve, no `EndFrame` | temporal + spatial | ReSTIR GI output |

## Decisions

| Decision | Rule | Why |
|---|---|---|
| Switch point | at a frame boundary, without a reload, a device idle or a reconfigure | as §16.5: a toggle must be cheap enough to flip during a capture |
| History | a mode change resets ReSTIR GI history and DLSS-RR history for one frame | the samples change meaning (one bounce vs cache-terminated); RTXGI resets render history on every mode change. G5's reference toggles (`isGiReference`, `giReferenceBounces`) reset nothing yet: reservoirs age out (G5 §1) |
| Ground truth | the multi-bounce one is G5's (`giReferenceBounces` = 8 with the reference); the one-bounce one is the reference with NRC paused and `giReferenceBounces` = 0, which needs the NRC pause to follow the mode instead of `giReferenceBounces` | one indirect pass and path loop (spec §14) |
| NRC while paused | context, SDK buffers and network weights stay; leaving ReSTIR GI mode continues training from the paused weights. A bounds change while paused is applied on resume (one `Configure`) | destroying or recreating the context leaks SDK objects (C-G3-3) and `requestReset` does not reset (C-G3-5); a paused cache must not configure for nothing |
| C-G3-6 wait | follows `Renderer::isNrcRecorded`: a ReSTIR GI frame records no NRC work, so the host wait and the deferred-present flush wait for nothing | the device-loss window exists only while NRC kernels and the DLSS-RR after them run |
| Shader | one indirect pass and one ReSTIR GI pass set; the mode is a frame-uniform value in the NRC constants (query on / off). If the uniform branch costs over 1% of the pass, build two link-time specializations at startup instead (spec S9) and pick one per frame | no preprocessor permutations (S9); both variants stay compiled |
| Combine | unchanged: it always reads `indirectRadiance` | the three terms still meet only in combine (spec §11.6) |
| UI | `Indirect` panel: a combo with the three modes; the `NRC cache` debug view is disabled in ReSTIR GI mode | a debug view must not show a stale cache as if it were live |
| Tiers | both tiers default to NRC → ReSTIR GI (spec P1). The toggle is for A/B measurement and debugging; a tier default changes only by an engine-spec decision backed by this step's numbers (for example the floor tier, if NRC fails C-G3-2 / R4) | one shipped path per tier; the others stay working so the choice can be remeasured |

## Done when

- `Sponza` with the sun renders in all three modes, and a scripted run that switches mode every 64 frames for 3000 uncapped frames at a 2560×1369 window loses no device (C-G3-6) and leaks no more SDK objects than one configure.
- Convergence on a static frame: ReSTIR GI mode against the one-bounce ground truth (spec §14 reference with one bounce); NRC and NRC → ReSTIR GI against the multi-bounce ground truth; the same tolerances as G5.
- The self-test runners cover each mode: the guide capture writes the lighting views per mode, and the ray check passes in each.
- Validation clean, GPU-AV included, in each mode.
- Per-mode timings (indirect pass, NRC, ReSTIR GI, GPU total, frame time) on both tiers recorded here.

## Must not

Destroy or recreate the NRC context on a switch, add a second indirect pass or path loop, or let a tier pick a mode without a spec decision.

## Log

| Date | Change |
|---|---|
| 2026-11-18 | created: indirect mode toggle before 1.0 (user request: switch NRC and ReSTIR GI as a toggle, NRC kept) |
