# M4 · G7: indirect mode toggle

**Status:** built 2026-11-19; verified on the RTX 5070 (§3). Open: 2060 Super timings · **Milestone:** M4
**Implements:** engine spec §11.7; plan G7 ([pbr_implementation_plan.md](../pbr_implementation_plan.md))
**Depends on:** [G3](./g3_nrc.md) (NRC), [G5](./g5_restir_gi.md) (ReSTIR GI), [G6](./g6_combine.md) (combine)
**Considerations:** C-G3-3 (each `Configure` leaks SDK objects), C-G3-5 (`requestReset` ineffective), C-G3-6 (no queue work or SDK calls during NRC / DLSS-RR work), C-G5-3 (NRC reads ~10% bright)
**Reference:** NVIDIA RTXGI `Samples/IndirectDiffuse` (`reference/RTXGI`): `IndirectDiffuseMode` (`UI.h`) picked by radio buttons in `UI.cpp`, and every mode or resolve change sets `resetRenderHistory`. RTXDI sample: ReSTIR GI without a radiance cache.

A runtime toggle between three ways of producing `indirectRadiance`. NRC stays built and initialized in every mode; the toggle only decides which passes run. It lets each scene and tier be compared on the same frame, as the post-1.0 ray-path toggles do (engine spec §16.5).

## Scope

| Mode | Sample radiance at X₂ | NRC | ReSTIR GI | `indirectRadiance` |
|---|---|---|---|---|
| **NRC → ReSTIR GI** | emission + direct light at X₂ + `prefixThroughput × cachedRadiance` (spec §11.3–11.4) | update paths, `QueryAndTrain`, custom resolve, `EndFrame` | temporal + spatial | ReSTIR GI output |
| **NRC** | the same | the same | skipped | the resolved sample shaded at the primary surface: one sample per pixel, no reuse (RTXGI `IndirectDiffuseMode::Nrc` with RR) |
| **ReSTIR GI** (default since 2026-11-20) | emission + direct light at X₂ only: one bounce, as the RTXDI sample | paused: no update paths, no `QueryAndTrain`, no resolve, no `EndFrame` | temporal + spatial | ReSTIR GI output |

## Decisions

| Decision | Rule | Why |
|---|---|---|
| Switch point | at a frame boundary, without a reload, a device idle or a reconfigure | as §16.5: a toggle must be cheap enough to flip during a capture |
| History | a change of the mode, of `isGiReference` or of ground-truth paths resets ReSTIR GI history and DLSS-RR history for one frame; ReSTIR DI keeps its history | the samples change meaning (one bounce vs cache-terminated); RTXGI resets render history on every mode change |
| Ground truth | the multi-bounce one is G5's (`giReferenceBounces` = 8 with the reference); the one-bounce one is ReSTIR GI mode with the reference (NRC paused, `giReferenceBounces` = 0) | one indirect pass and path loop (spec §14) |
| NRC while paused | context, SDK buffers and network weights stay; leaving ReSTIR GI mode continues training from the paused weights. A bounds change while paused is applied on resume (one `Configure`) | destroying or recreating the context leaks SDK objects (C-G3-3) and `requestReset` does not reset (C-G3-5); a paused cache must not configure for nothing |
| C-G3-6 wait | follows `Renderer::isNrcRecorded`: a ReSTIR GI frame records no NRC work, so the host wait and the deferred-present flush wait for nothing | the device-loss window exists only while NRC kernels and the DLSS-RR after them run |
| Shader | no new shader input: a paused cache has zeroed NRC constants (the query pass makes no queries, as without a scene), and NRC mode is RTXDI's resampling mode `None`, which the G5 shade pass already handles. The 1% uniform-branch rule has nothing to measure | no preprocessor permutations (S9); one indirect pass and one ReSTIR GI pass set |
| Combine | unchanged: it always reads `indirectRadiance` | the three terms still meet only in combine (spec §11.6) |
| UI | `Indirect (M4)` panel: an `Indirect mode` combo; the `NRC cache` debug view is disabled while NRC is paused (ReSTIR GI mode or ground-truth paths), and the panel shows `paused` | a debug view must not show a stale cache as if it were live |
| Tiers | both tiers default to ReSTIR GI since 2026-11-20 (spec P1, §11.7; NRC → ReSTIR GI until then): NRC's ~10% bias (C-G5-3) outweighs its second bounce. The toggle is for A/B measurement and debugging; a tier default changes only by an engine-spec decision backed by this step's numbers (for example the floor tier, if NRC fails C-G3-2 / R4) | one shipped path per tier; the others stay working so the choice can be remeasured |

## 1. As built

- **`IndirectMode`** (`src/render/indirect_mode.hpp`): `NrcRestirGi`, `Nrc`, `RestirGi`, with UI labels and ids (`nrc-restir-gi`, `nrc`, `restir-gi`). `Renderer::indirectMode` is read once per recorded frame.
- **Pause:** `Renderer::isNrcPaused()` is true in ReSTIR GI mode or with ground-truth paths. `NrcCache::prepareFrame(slot, bounds, isPaused)` zeroes the slot's constants and returns before the bounds test, so new bounds configure once on resume. It replaces G5's `NrcCache::pauseFrame`. `isNrcRecorded = nrc.isActive() && !isNrcPaused()`.
- **NRC mode:** `RtxdiContext::prepareFrame(slot, frame, isGiReuseAllowed)` with `isGiReuseAllowed = mode != Nrc`; reuse runs when allowed and `isGiReference` is off (`isGiReuseEnabled()`). Without reuse the temporal pass is skipped and the shade pass shades the initial (resolved) sample, as G5's reference.
- **History:** `Renderer` compares `{mode, isGiReference, ground-truth paths}` with last frame's. A change sets `FrameFlag::IndirectHistoryReset` (`kFrameFlagIndirectHistoryReset`, bit 1), which `rtxdi_gi_temporal` reads through `hasIndirectHistory`, marks DLSS-RR history invalid, and logs `[Renderer] indirect: …`. This also gives G5's reference toggles the reset G5 left open.
- **`ENGINE_INDIRECT_MODE`:** a mode id pins the mode; `cycle` switches every 64 frames (`kIndirectModeCycleFrames`, `Engine::isIndirectModeCycling`). Unknown values are logged and ignored.
- **GI check** (`ENGINE_GI_CHECK=1`): per pose, after G5's phases, `NRC mode` (measured against the multi-bounce ground truth), a one-bounce ground truth (ReSTIR GI mode, reference, 2048 frames), its noise floor (bias gated at 1%) and `ReSTIR GI mode (converged settings)` (gated). Phases carry their mode and a role (ground truth, noise floor, compared). A compared phase's tile limit is the larger of its own and twice the current ground truth's noise-floor tile p90 (`kNoiseFloorTileFactor`): the one-bounce truth's dim gallery tiles get sun fireflies (noise floor p90 13.5–15.3%), and for G5's phases the rule still yields 8% (2 × 3.74%). Summary: `ReSTIR GI convergence: 8 of 8 passed`; ~10 min.
- **Guide capture:** 21 views per shot. `IndirectSamples`, `IndirectRadiance` and `DlssInput` again in NRC and ReSTIR GI mode (files `<shot>_<view>_<mode id>.png`); no NRC cache view in ReSTIR GI mode. The mode switches on the captured frame, so ReSTIR GI shows its first frame without history. 252 PNGs.

## 2. Files

| File | Change |
|---|---|
| `src/render/indirect_mode.hpp` | new: `IndirectMode`, `kIndirectModes` |
| `src/render/vk_renderer.{hpp,cpp}` | `indirectMode`, `isNrcPaused`, `IndirectState` change detection, `FrameFlag::IndirectHistoryReset`, NRC pause and RTXDI reuse inputs |
| `src/render/vk_nrc.{hpp,cpp}` | `prepareFrame(…, isPaused)`; `pauseFrame` removed |
| `src/render/vk_rtxdi.{hpp,cpp}` | `prepareFrame(…, isGiReuseAllowed)`, `isGiReuseEnabled`, `configureGi(isGiReuse)` |
| `src/core/types.hpp`, `shaders/types.slang`, `shaders/core/frame.slang`, `shaders/passes/rtxdi_gi_temporal.slang` | the frame flag and `hasIndirectHistory` |
| `src/core/vk_engine.{hpp,cpp}` | `ENGINE_INDIRECT_MODE`; the guide capture and GI check set the mode |
| `src/core/engine_gi_check.{hpp,cpp}` | mode phases, roles, noise-floor-relative tile limit |
| `src/core/engine_guide_capture.{hpp,cpp}` | `ShotView` (view + mode), 21 views |
| `src/ui/engine_ui.cpp` | mode combo, NRC state, NRC cache view disabled while paused, Defaults resets the mode |

## 3. Verification

Release, RTX 5070, 2026-11-19.

| Check | Result |
|---|---|
| GI check, no layers | 8 of 8. Gallery / courtyard: noise floor +0.19% / −0.01%; ReSTIR GI on traced paths +0.56% / +1.44% (limit 1.5%; earlier runs of the unchanged phase 0.90–1.08% in the courtyard: close to its limit, watch it); one-bounce noise floor +0.04% / +0.17% (tile p90 15.3% / 5.2%); ReSTIR GI mode against one bounce +0.31% / +0.63%, tile p90 14.9% / 9.9% (limits 30.6% / 10.4%) |
| NRC modes against the multi-bounce ground truth (measured) | NRC mode +11.9% / +8.1%, NRC + ReSTIR GI +8.8% / +10.2%: the cache term's bias (C-G5-3) without any reuse. Not gated, against the done-when's "same tolerances as G5" for these two modes |
| Validation: core + sync + best practices, `ENGINE_GUIDE_CAPTURE=1 ENGINE_RT_CHECK=1` (every mode on every shot) | clean apart from the known `SpirvDeprecated_WorkgroupSize` ×3 and `VUID-vkDestroyDevice-device-05137`; ReSTIR DI convergence passed, normals 2 of 2, ray check 366 of 366 frames, 252 PNGs |
| GPU-AV, `ENGINE_INDIRECT_MODE=cycle ENGINE_BENCH_FRAMES=640` on Sponza (14 switches) | clean apart from `WARNING-Setting-Limit-Adjusted` and 05137 |
| SDK leaks, core validation, 640 frames | 51 leaked objects with `cycle` (14 switches) and with a fixed mode; one `Configure` in each |
| C-G3-6, `ENGINE_INDIRECT_MODE=cycle ENGINE_BENCH_FRAMES=3000` at a 2560×1369 window (render 1707×913), uncapped, 2 runs | no device loss, 50 switches each, GPU total 9.83 / 9.85 ms (mode mix) |

Timings, 1280×720 window (render 853×480), startup gallery with the sun, uncapped, two rounds per mode (A B C C B A), ms:

| Mode | NRC update | Indirect | NRC train | NRC resolve | GI temporal | GI shade | DLSS-RR | GPU total | Frame time |
|---|---|---|---|---|---|---|---|---|---|
| NRC → ReSTIR GI | 0.021 | 0.66 | 0.42 | 0.03 | 0.11 | 0.36–0.39 | 1.11 | 3.27–3.30 | 3.86 |
| NRC | 0.021 | 0.66 | 0.41 | 0.03 | — | 0.05 | 1.20 | 2.90 | 3.52 |
| ReSTIR GI | — | 0.56 | — | — | 0.12 | 0.28 | 1.16 | 2.72 | 2.73 |

Frame time is (wall at 6000 − wall at 1500 frames) / 4500. The NRC modes pay the C-G3-6 host wait (0.57–0.62 ms over GPU total); ReSTIR GI mode records no NRC work and pays nothing. 2060 Super: not measured.

## Done when

- `Sponza` with the sun renders in all three modes, and a scripted run that switches mode every 64 frames for 3000 uncapped frames at a 2560×1369 window loses no device (C-G3-6) and leaks no more SDK objects than one configure. **Met.**
- Convergence on a static frame: ReSTIR GI mode against the one-bounce ground truth (spec §14 reference with one bounce); NRC and NRC → ReSTIR GI against the multi-bounce ground truth; the same tolerances as G5. **ReSTIR GI mode met (tile limit relative to the noise floor, §1); the NRC modes measured only, blocked by C-G5-3.**
- The self-test runners cover each mode: the guide capture writes the lighting views per mode, and the ray check passes in each. **Met.**
- Validation clean, GPU-AV included, in each mode. **Met.**
- Per-mode timings (indirect pass, NRC, ReSTIR GI, GPU total, frame time) on both tiers recorded here. **RTX 5070 recorded; 2060 Super open.**

## Must not

Destroy or recreate the NRC context on a switch, add a second indirect pass or path loop, or let a tier pick a mode without a spec decision.

## Log

| Date | Change |
|---|---|
| 2026-11-18 | created: indirect mode toggle before 1.0 (user request: switch NRC and ReSTIR GI as a toggle, NRC kept) |
| 2026-11-19 | built (§1): `IndirectMode`, NRC pause in `prepareFrame`, RTXDI reuse input, `IndirectHistoryReset` (also for G5's reference toggles), `ENGINE_INDIRECT_MODE`, GI check mode phases with a noise-floor-relative tile limit, guide capture per mode, UI combo; verified and timed on the 5070 (§3) |
| 2026-11-20 | default switched to ReSTIR GI on both tiers (user decision: NRC reads ~10% bright, C-G5-3); NRC stays built and paused |
