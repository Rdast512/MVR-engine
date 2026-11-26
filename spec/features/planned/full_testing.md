# Feature: Full settings testing (settings lab)

**Date:** 2026-11-22 · **Status:** T1–T5, T7, T9 built, not run yet (§9); T12 done (2026-11-26); T6, T8, T10, T11 open · **Milestone:** M8 tooling, ahead of I12 ([README](../../fixesM1-6/README.md)); used by I7, I12, I13 and every later tuning step
**Implements:** a settings lab, separate from the engine checks: a switch catalog (the logic of every lighting setting), a case catalog (scenes and poses that each stress one failure), and a matrix runner that tries settings and finds the switch behind a measured error by one-factor runs and bisection
**Touches:** findings F22, F23 ([m8_optimization_and_fixes.md](../../fixesM1-6/m8_optimization_and_fixes.md)); considerations C-I12-1, C-I12-2 ([considerations.md](../../considerations.md)); stands beside M8 I1's engine checks ([i1_check_runners.md](../../fixesM1-6/i1_check_runners.md)) without changing them (§0)
**Reference:** RTXDI `Doc/NoiseAndBias.md`, `Doc/RestirPT.md` (DLSS-RR decorrelation, duplication history reduction), `Doc/Confidence.md`, `Doc/RestirGI.md`; FullSample `UserInterface.cpp` (`ApplyPreset`, `ApplyReSTIRPTPreset`, `ApplyDenoiserCompatDefaults`); library defaults in `Source/ReSTIRDI.cpp`, `ReSTIRGI.cpp`, `ReSTIRPT.cpp`; `reference/NrcGuide.md` (resolve modes, §Debugging); `reference/vk_denoise_dlssrr/README.md` (guide buffers). NVIDIA's samples ship presets and on-screen toggles, no automated A/B runner.

The café findings took a day of hand-written variants to pi
n down, and both were explained by NVIDIA's own documents once the right switch was in view:

- **F22** (shipped DI confetti under DLSS-RR) is what `NoiseAndBias.md` §Denoiser friendliness describes: a single temporal pass is "too stable", the denoiser "will decide that it doesn't need to be blurred, so the result will be blotchy"; "one spatial sample helps break up the stable patterns". Every FullSample DI preset, `Fast` included, keeps at least one spatial sample. I6 shipped zero.
- **F23** (shipped GI brightens dim places) matches `RestirPT.md` §Decorrelation: "firefly replacement on its own brightens the image"; `fireflyReplacementBiasReduction` exists to bound it, and NVIDIA's DLSS-RR profile keeps it on (`MultiplyBound 15`). I5 turned the bound off for a Sponza darkening it caused.

This feature makes both routes routine: the **logic** route (§3, §4: what each switch does, NVIDIA's value, the signature it leaves) proposes candidates in minutes, and the **brute-force** route (§6: one-factor runs, bisection) confirms or finds them without a hypothesis. Nothing here changes a shipped setting.

---

## 0. Two kinds of testing

The engine has two testing tools with different jobs. This spec builds the second one and leaves the first as it is.

| | Engine checks (exist) | Settings lab (this feature) |
|---|---|---|
| Question | is the engine as it ships right, measured against how it should look in a perfect case? | which settings would be better, and which setting causes an error? |
| What runs | the shipped settings, plus the fixed comparison sets each check already has (NVIDIA's, converged, I1's GI sets, the café's DI sets) | any config: sets, one-factor flips, sweeps, pairs, bisection |
| Against | references: ground truth paths, RTXDI's reference, the noise floors; gates where limits exist | the plan's base config, in noise-floor units; references only where a kind needs them |
| Runs | `ENGINE_GI_CHECK`, `ENGINE_STABILITY_CHECK`, `ENGINE_LIGHT_CHANGE_CHECK`, `ENGINE_SCREEN_EDGE_CHECK`, `ENGINE_GUIDE_CAPTURE`, `ENGINE_MOTION_TEST`, `ENGINE_CULL_CHECK`, `ENGINE_RT_CHECK` (`ENGINE_CHECK_SCENE` for the café) | `ENGINE_LAB=<plan>` |
| Code | `src/checks/engine_*_check.*`, `engine_guide_capture.*`, `check_poses.hpp`, `gi_settings_sets.hpp` | `src/checks/lab/` |
| Output | `[GiCheck]`, `[TemporalCheck]`, … lines in `logs/engine.log`; `logs/checks/` PNGs | `[Lab]` lines; `logs/lab/<plan>.csv`; `logs/lab/` PNGs |
| A result means | the shipped engine moved: a regression, or a fix confirmed | a candidate for the step that owns the setting |

**Rules of the split:**

- **The lab does not change the engine checks.** It changes no step list, enum, pose, seed, limit or output line of theirs. Their numbers stay comparable with every baseline recorded so far.
- **The engine checks ignore the lab.** They do not read `ENGINE_CONFIG` or the lab's registry. Started with `ENGINE_CONFIG` set, a check logs that it ignores it and measures what ships.
- **Shared code is limited to pure functions.** The lab calls `convergence_metrics.hpp` (tile comparison, dark leak, dim bias), `check_images.hpp` (PNGs) and reads the pose coordinates in `check_poses.hpp`. It does not refactor the checks' runners to share their loops: where it needs logic that is private to a check (I1's settling, the stability accumulators), it keeps its own copy with the same definition.
- **The copies are cross-checked.** On the shipped config, a lab measurement must equal the engine check's line for the same case and view within the case's repeatability (T3). That is the lab's own correctness test.
- **The flow is one way.** The lab finds a candidate. The step that owns the setting (I12 for F22 / F23) changes the shipped default. The engine checks then confirm the new shipped state against the references, and their baselines are updated in that step.

---

## 1. What exists and what is missing

| Piece | Now | After this feature |
|---|---|---|
| Settings sets | `GiSettingsSet` (11 GI sets), `DiSet` (8 DI sets, café only), `RtxdiContext::{default,nvidia,converged}{,Gi}Settings` | the lab's registry of named switches (§3); any set is a base plus overrides, written as a string (§2.1). The engine checks keep their sets as they are |
| Overriding a setting for a run | new enum value + rebuild, or `ENGINE_DI_SAMPLING` (one field) | `ENGINE_CONFIG="di=shipped gi=shipped di.spatialSamples=2"` for the interactive engine and lab runs; the engine checks ignore it |
| Scenes | Sponza gallery / courtyard; café tables / dining (`ENGINE_CHECK_SCENE`); the guide capture's 3 DI convergence scenes, separate | the lab's case catalog (§5), with the failure each case stresses; it reads the engine checks' pose coordinates and adds its own cases |
| Bias measurement | GI check (one-bounce and 8-bounce truth), café DI against RTXDI's reference; bias, tile p90, dark leak, dim bias | the same on every case and config, plus hot-tile bias and the noise floor of each reference (§4.2) |
| Stability | temporal std, flicker share, f2f, 8-frame swing | plus lag-1 temporal correlation of the raw signal, the direct measure of "too stable for the denoiser" (§4.2) |
| Finding the switch | by hand | `ENGINE_LAB`: sets, one-factor-at-a-time in both directions, sweeps, pairs, bisection; a CSV and a ranking of switches by effect in noise-floor units (§6) |

---

## 2. Decisions

| Decision | Rule | Why |
|---|---|---|
| A run | case × config × view × measurement kind; seeded from a hash of all four (I1's content seeding) | a run's numbers do not depend on what ran before it, so plans can grow without moving old lines |
| One registry | every switch has a name, a domain, an accessor on `RtxdiContext::Settings` / `GiSettings` / renderer state, a value list, its shipped, NVIDIA and converged values, NVIDIA's source line, a mechanism and a signature (§3) | the catalog is both the documentation and the runner's input; a switch missing from it cannot be tested |
| Effects in noise units | every measured difference is reported as Δ and Δ / noise floor; below 2 noise floors is "no effect" | the café's references are noisy (one-bounce truth reads ±5% against itself, F23); a 3% "effect" there is nothing |
| Noise floor per case and kind | the base config run on 3 seeds; the floor is the largest spread of the metric across them, with the reference-against-itself line where a reference exists | one number per metric, measured where it is used |
| Both directions | one-factor runs flip each switch from the base toward the target **and** from the target toward the base | a switch that only matters together with another shows up in one direction only; that asymmetry flags an interaction (§6.3) |
| Measurement without layers | `VK_LOADER_LAYERS_DISABLE='*validation*'`; cost runs are bench-mode runs on their own (I4's ABBA), never mixed with image runs | layers distort timing; captures distort timing |
| NVIDIA sets stay callable | `di=nvidia`, `gi=nvidia` are registry sets built from the library defaults and the FullSample preset, as today | memory rule: NVIDIA's set is the comparison baseline |
| Engine checks untouched | §0: no change to the GI check, the temporal runners or the guide capture; their lines stay byte for byte on Sponza | the checks measure the shipped state; their baselines stay comparable |
| Results | one `[Lab]` `MEASURED` line per run in `logs/engine.log`, one CSV row per run in `logs/lab/<plan>.csv`, PNGs in `logs/lab/` with `ENGINE_LAB_PNG=1` | the log for reading, the CSV for diffing two runs of a plan; nothing mixes with the checks' outputs |

### 2.1 Config strings

```
config   := item (' ' item)*
item     := set | override
set      := domain '=' setName              # di=shipped | di=nvidia | di=converged | di=reference | gi=… | frame=shipped
override := domain '.' switch '=' value     # di.spatialSamples=2 gi.fireflyBound=on frame.indirectMode=nrc
```

Sets apply first, then overrides, left to right. `ENGINE_CONFIG` applies one config at startup to the interactive engine and bench runs, over the state the other variables set up (`ENGINE_INDIRECT_MODE`, `ENGINE_DI_SAMPLING`), so `ENGINE_CONFIG="di.spatialSamples=2"` changes one switch and `di=nvidia` replaces the DI set (lab plans set their own configs per run; the engine checks ignore it, §0); the UI shows the current config string with a copy button, so an A/B found by hand becomes a runner line. Unknown names and out-of-range values are logged and the run fails (a typo must not silently test the base).

---

## 3. Switch catalog (the logic)

Values: shipped = `RtxdiContext::defaultSettings()` / `defaultGiSettings()`; NVIDIA = library defaults plus FullSample's `Medium` preset (DI) or `ApplyDenoiserCompatDefaults(true)` (PT profile). "If wrong" is the artifact the switch produces when it is the culprit; "Measured by" names the metric that sees it (§4.2).

### 3.1 ReSTIR DI

| Switch | Shipped | NVIDIA | What it does | If wrong | Measured by |
|---|---|---|---|---|---|
| `di.sampling` | `Power_RIS` | `ReGIR_RIS` (Medium) | initial candidates by light power, or by ReGIR world cells | ReGIR: power-PDF MIS with BRDF candidates mismatches its cell selection (C-F1-4); boiling across rooms without it (`NoiseAndBias.md`) | DI bias, dim bias, cost (`presample`) |
| `di.localSamples` | 32 (I16 §7.9) | 8 | initial candidates per pixel | too few: sparse signal that reuse turns into boiling | DI raw std, `Lit` flicker |
| `di.perPixelLightTiles` | on (I16 §7.9) | off | every pixel picks its own RIS tile / ReGIR cell jitter instead of its 16×16 screen tile's | off: a tile's lucky draw lights the whole tile (pops) | `Lit` rises, cost |
| `di.brdfSamples` | 1 | 1 | BRDF-direction candidate (skipped without emissives / env, O8) | MIS mismatch with the presampled PDF | DI bias on `EmissiveStrengthTest` |
| `di.initialVisibility` | on | on | shadow ray on the initial winner | off: occluded candidates survive into reuse | cost; shadow-edge noise |
| `di.history` | 20 | 20 | temporal `M` cap | long: lag after light changes; short: noise | light change settle, `Lit` std |
| `di.temporalCorrection` | `Raytraced` | `Raytraced` | MIS of the temporal neighbor, with a visibility ray | `Basic`: darkening under moving shadows (`NoiseAndBias.md` §Bias, `Confidence.md`) | DI bias, light change |
| `di.visibilityShortcut` | on | on | skips the correction ray when the sample came from last frame | none if final visibility discards invisible samples (`Confidence.md`) | cost |
| `di.fallback` | on | on | zero-motion neighbor when the surface match fails | off: disocclusions start empty | screen-edge settle |
| `di.permutation` | off | on (library) | temporal neighbor taken from a permuted pixel | **off with no spatial reuse: per-pixel samples persist → DLSS-RR keeps the noise (F22)**; on: slight bias (café −3.8% with spatial) | lag-1 correlation, `Lit` flicker, DI bias |
| `di.spatialSamples` | **0** | 2 (Medium 1, normalized to even) | spatial neighbors per frame | **0: "too stable" signal, blotches and confetti under the denoiser (`NoiseAndBias.md`, F22)**; many: more bias with `Basic` | lag-1 correlation, `Lit` flicker, DI bias |
| `di.boostSamples` / `di.boostHistory` | 8 / 4 | 8 / 0 | spatial samples while `M` is below the history | off: disocclusions converge slowly | screen-edge settle |
| `di.spatialCorrection` | `Raytraced` | `Basic` | MIS of spatial neighbors, with visibility rays | `Basic`: darkening at geometric discontinuities and occluders (NVIDIA DI −16.7% in the café, −38% `Sponza_terminator`); `Pairwise` + shortcut: catastrophic darkening (I6) | DI bias, dim bias (negative) |
| `di.radius` | 32 px | 32 px | spatial neighbor search radius | wide: neighbors from other surfaces, correlated patches the denoiser cannot break; tight: neighbors too alike to decorrelate | `Lit` flicker, lag-1 correlation, DI bias |
| `di.materialTest`, `di.depthThreshold`, `di.normalThreshold` | on, 0.1, 0.5 | same | neighbor rejection | loose: light bleeds across edges | dim bias near edges, tile p90 |
| `di.boilingFilter`, `di.boilingStrength` | on, 0.2 | on, 0.2 | drops reservoirs far above the tile mean | off: persistent fireflies; strong: darkening of real highlights | hot-tile bias, swing |
| `di.finalVisibilityReuse` | off | on | reuses stored visibility within age 4 / 16 m | **on: light through shadow edges (O10, "visibility reuse bias" in `NoiseAndBias.md`)** | DI bias at shadow edges, `Sponza_terminator`, dim bias (positive) |
| `di.reference` | off | — | RTXDI without reuse: the DI reference | — | is the reference |

### 3.2 ReSTIR GI and the DLSS-RR profile

| Switch | Shipped | NVIDIA | What it does | If wrong | Measured by |
|---|---|---|---|---|---|
| `gi.history` | 20 (PT DLSS-RR) | 8 | temporal `M` cap | long + correlated: stale blotches; short: noise | `Lit` std, light change |
| `gi.age` | unlimited | 30 | drops samples older than this | limited: brightening, the young samples a bright candidate won (C-G5-1) | GI bias |
| `gi.temporalCorrection` | `Basic` | `Basic` | temporal MIS | — (ray-traced costs 0.06 ms for nothing, C-G5-2) | GI bias |
| `gi.fallback`, `gi.permutation` | on, off | on, off | as DI | — | screen-edge, lag-1 correlation |
| `gi.spatialSamples` | 1 | 2 | spatial neighbors | 0: blotches; more: cost | `Lit` std, cost |
| `gi.radius` | 32 px | 32 px (also ReSTIR PT's DLSS-RR profile) | spatial neighbor search radius | as `di.radius` | `Lit` flicker, lag-1 correlation, GI bias |
| `gi.spatialCorrection` | `Raytraced` | `Basic` | spatial MIS with visibility | `Basic`: −19% / −42% in the café, −27% gallery (C-G5-2) | GI bias, dim bias (negative) |
| `gi.boilingFilter`, `gi.boilingStrength` | off (PT DLSS-RR) | on, 0.2 | as DI | — | hot-tile bias |
| `gi.fullTileBoiling` | on | — (engine, F15) | boiling filter over the whole 8×8 tile in dark tiles | — | dark-region fireflies |
| `gi.decorrelation` | `Stagnancy`, 0.4 / 0.5 / 0.2 | `None` for ReSTIR GI; `Stagnancy` for PT under DLSS-RR | shades the initial sample instead of the resampled one with a probability that grows with stagnancy (`RestirPT.md`) | off: blotches under DLSS-RR; on: more raw noise (confetti if DI is also correlated) | lag-1 correlation, `Lit` flicker |
| `gi.fireflyReplacement`, `gi.fireflyStrength` | on, 0.7 | off for GI; on for PT under DLSS-RR | forces decorrelation on boiling-filter outliers | off: fireflies smeared by DLSS-RR's accumulation | hot-tile bias, swing |
| `gi.fireflyBound` | **off** | **on, ×15** (PT DLSS-RR) | clamps a fresh replacement's UCW to 15× the resampled weight | **off: "firefly replacement on its own brightens the image" (`RestirPT.md`); F23 candidate: café dim tiles +18–23%**; on: darkened Sponza −7.1% / −3.9% (I5) | dim bias, GI bias |
| `gi.duplicationReduction` | off | on, 0.5 (PT DLSS-RR) | lowers the `M` cap where samples repeat | on: −5.6% / −7.6% darkening (I5, MIS correlation) | GI bias, lag-1 correlation |
| `gi.finalVisibility`, `gi.finalMis` | on, on | on, on | final shadow ray; MIS with the raw sample on low roughness (`RestirGI.md`) | off: light through occluders; glossy blotches | dark leak, glossy cases |
| `gi.secondaryResampling` | off | on in FullSample `Medium`; off in `nvidiaGiSettings()` | ReSTIR DI on secondary hits | on: café dining +19% (one-bounce check) | GI bias |
| `gi.fireflyClamp` | off | — (engine) | clamps secondary radiance | on: energy loss | GI bias |
| `gi.roughnessFloor` | on | on (sample default) | clamps secondary roughness (`RestirGI.md`: avoid caustics) | off: caustic fireflies (café +12.7%) | hot-tile bias |

### 3.3 Indirect mode, NRC, frame

| Switch | Shipped | NVIDIA / reference | What it does | If wrong | Measured by |
|---|---|---|---|---|---|
| `frame.indirectMode` | `nrcRestirGi` since M8 I15 (`restirGi`, NRC paused, 2026-11-20 – 11-24) | — | `nrcRestirGi`, `nrc`, `restirGi` | one bounce: ~30% of the 8-bounce energy missing in the café (F23) | GI bias against the 8-bounce truth |
| `nrc.training`, `nrc.iterations`, `nrc.resolveMode` | on, 1, `AddQueryResultToOutput` | `NrcGuide.md` §Debugging | NRC training and resolve; resolve modes are the NRC guide's diagnostics (`DirectCacheView`, `PrimaryVertexTrainingRadiance`, `TrainingBounceHeatMap`) | cache bias (I7) | NRC bias, PNGs of each resolve mode |
| `frame.jitter` | on | on (DLSS-RR needs it) | sub-pixel jitter | off under DLSS-RR: aliasing; on below DLAA without DLSS-RR: boiling (C-H1-7) | `Lit` std |
| `frame.dlssPreset` | `Default` (the driver picks) | DLSS 4 transformer models: `D`, `E` | DLSS-RR model | CNN or an older model: low-light boiling, ghosting | `Lit` std, ghosting on screen edges |
| `frame.dlssQuality` | `Quality` | — | render resolution against display | fewer input pixels per output pixel: more noise to reconstruct | `Lit` std, cost |
| `frame.specularGuides` | none (T10) | DLSS-RR guide: `SpecularHitDistance` or specular motion vectors (NGX `DLSSD` parameters; `vk_denoise_dlssrr` README) | specular hit distance from the primary surface to its reflected hit | missing: DLSS-RR moves reflections with the surface's motion, so glossy and metal surfaces swim | `Lit` std on `glossy.*`, screen-edge settle |
| `gi.targetCompression` | off (T11) | — (RIS stays unbiased for any target PDF that is nonzero where the integrand is, `NoiseAndBias.md` §Target PDF) | resamples by a compressed target (`p̂ / (1 + p̂ / k)`) instead of `p̂`, so one bright candidate wins less often | too strong: more noise in bright regions | hot bias, `Lit` flicker, GI bias (must stay within the floor) |
| `frame.preDenoiserClamp`, `frame.clampNeighbors` | off (T11, built 2026-11-24; rejected for the café, I16 §7.9) | — (RTXPT's firefly filter is the model; RTXPT is not in `reference/`) | clamps the lighting's luminance in DLSS-RR's noisy input to a multiple of DLSS-RR's last output around the reprojected pixel (3×3 max), or with `clampNeighbors` of the input's 3×3 neighbors | biased: real small highlights lose energy; the history reference feeds DLSS-RR's dark output back | hot bias, frame bias, `Lit` swing |
| `frame.exposure` | auto, fixed clock in runners | — | auto exposure (F10) | feedback into DLSS-RR, dark boiling | `Lit` swing |
| `scene.sun` | default / moved / off | — | runner sun state | — | light change |
| `scene.emissiveScale.<material>` | 1 | — | scales one material's emission (the `cafe.toggle` case) | — | light change |
| `frame.historyConfidence` | off (T8) | RTXDI FullSample gradients and confidence (`Confidence.md`) | lowers ReSTIR DI's and GI's per-pixel history cap where the temporal gradient reports a lighting change | off with long history: lag after light changes | light change settle, `Lit` std |

Every value above is read from the code by the registry at build time; the table is checked against it (§7 T1), so the spec cannot drift from what ships.

---

## 4. Signatures (from symptom to candidates)

### 4.1 Symptom table

Read a symptom off the metrics first; the candidates are the switches whose "If wrong" column (§3) predicts that pattern. The signature names what to run to tell the candidates apart.

| Symptom on screen | Metric pattern | Candidates (logic) | Tell them apart by |
|---|---|---|---|
| Confetti or blotches under DLSS-RR, raw signal calm | `Lit` flicker high; raw `Direct lighting` / `Indirect radiance` f2f low; **lag-1 correlation high**; bias ≈ 0 | `di.spatialSamples=0`, `di.permutation=off`, `gi.decorrelation=none`, long `*.history` | one-factor runs on `Lit` flicker; lag-1 correlation of the raw view per switch |
| Boiling in the raw signal too | raw std high, lag-1 correlation low | too few initial samples, sparse lights (emissive-only rooms), `di.sampling` | `di.localSamples` sweep; DI reference's own noise |
| Darkening near edges, occluders or in corners | bias < 0, dim bias < 0, tile p90 up near geometry | `*.spatialCorrection=basic`, `Pairwise` + shortcut, `gi.duplicationReduction`, `gi.age` | each correction mode against the reference |
| Light in dim places, no light in black places | dim bias > 0, dark leak 0 | `gi.fireflyBound=off`, `di.finalVisibilityReuse=on`, `gi.secondaryResampling`, loose neighbor tests | one-factor runs on dim bias; error PNG shows where |
| Light in black places | dark leak > 0 | visibility: ray offsets, OMM, glass masks (F21 / I13), `gi.finalVisibility=off`; not reuse | `di=reference` / `gi` truth also leak → geometry or ray bug, not a setting |
| Persistent bright pixels | hot-tile bias > 0, 8-frame swing | `*.boilingFilter=off`, `gi.fireflyReplacement=off`, `gi.roughnessFloor=off` | sweep of boiling strength |
| Lag after a light change | light-change settle long | `*.history`, `gi.age`, DLSS-RR history | lab `lightChange` runs per switch |
| Dark or noisy band after a camera move | screen-edge settle long | `*.fallback`, `di.boost*`, DLSS-RR | lab `screenEdge` runs per switch |
| Everything consistently too dark or too bright | frame bias, uniform error PNG | indirect mode (missing bounces), NRC bias, exposure | 8-bounce truth vs one-bounce truth; NRC resolve modes |

### 4.2 Measurements

| Kind | Reference | Fields | Notes |
|---|---|---|---|
| `diBias` | `di=reference`, 1024 frames | bias, tile p50 / p90, dark leak, **dim bias**, **hot bias**, noise floor | the café's references need longer runs or bigger tiles: dining's DI reference reads 486% temporal std (§8) |
| `giBias` | GI check truths (one bounce, 8 bounces) | as `diBias`, plus RGB bias | runs only GI-check configs (jitter off, as today) |
| `stability` | none | mean, temporal std, flicker share, lit px, f2f, 8-frame swing, **lag-1 correlation**, block events (`rises`, `dips`, `riseSize`, `riseFrames`, `riseSpread`: 16 px blocks against their temporal median, m8 I16 §7.8) | `Lit` through DLSS-RR, and the raw views (`Direct lighting`, `Indirect radiance`) |
| `lightChange`, `screenEdge` | as I1's | settle frames per region | I1's definitions, in the lab's own copy (§0) |
| `cost` | none | per-pass GPU ms (I4's scopes) | bench mode, separate process, no captures |

New fields:

- **Dim bias** (built 2026-11-22, `convergence_metrics.cpp`): signed bias over lit tiles below 10% of the average lit tile.
- **Hot bias:** the same over tiles above 10× the average lit tile, where fireflies and boiling-filter clipping live.
- **Lag-1 correlation:** per pixel, the correlation of consecutive deviations from the pixel's own mean, `Σ (x_t − μ)(x_{t+1} − μ) / Σ (x_t − μ)²`, averaged over lit pixels. 0 is noise that is fresh every frame, what DLSS-RR expects (`RestirPT.md`: "temporally uncorrelated input noise"); near 1 is the "too stable" signal of `NoiseAndBias.md`. It is computed from the same captures as temporal std and costs one extra buffer.

---

## 5. Case catalog

`src/checks/lab/lab_cases.hpp`. A case is a scene, a pose (or the startup camera), the measurement kinds that apply, and what it stresses. Cases the engine checks also use take their coordinates from `check_poses.hpp`, which stays as it is; the guide scenes load the same models and cameras as the guide capture without going through it. Plans name cases (`cases cafe.tables sponza.courtyard`); `ENGINE_LAB_CASES=…` narrows a plan from the command line.

| Case | Scene / pose | Stresses | Kinds |
|---|---|---|---|
| `sponza.gallery` | startup camera | sun bounce, indirect-dominated | all |
| `sponza.courtyard` | `kCourtyardPose` | direct sun, hard shadows | all |
| `bistro.street` | Bistro startup | 76K emissive triangles plus sun, many lights | stability, `diBias`, cost |
| `cafe.tables` | (−0.5, 1.7, 9.0) → (−10.5, 1.2, 8.0) | emissive-only interior, bar, mixed depth | stability, `diBias`, `giBias` |
| `cafe.dining` | (−5.5, 1.7, 8.0) → (−7.0, 1.0, 15.5) | emissive-only interior, small wall lights, frosted glass | stability, `diBias`, `giBias` |
| `cafe.door` | to choose in T2: the open door (seen in the dining hallway) with sunlit street outside | a bright exterior next to a dark interior: the leak case for both DI and GI | `diBias`, `giBias`, dim bias |
| `guide.lamp` | `LightsPunctualLamp_front` | punctual light, BRDF candidate | `diBias` (gate: I6's) |
| `guide.terminator` | `Sponza_terminator` | shadow terminator, visibility reuse | `diBias` (gate) |
| `guide.emissive` | `EmissiveStrengthTest_front` | triangle lights only | `diBias` (gate) |
| `glossy.*` | for I12: a smooth floor and a metal surface | glossy reuse, final MIS, roughness floor | stability, `giBias` |
| `glass.*` | for I13, once glass transmits | transmission, shadow transmittance | `diBias`, `giBias` |
| `cafe.toggle` | `cafe.dining` with one wall light's emissive switched off, then on (needs a per-material emissive scale the runner can set) | lag of ReSTIR history indoors, where the sun toggle changes nothing (the windows are opaque, F21) | `lightChange` (in `Direct lighting` and `Indirect radiance`) |

Case poses are checked by eye once (the frame PNG must show what the "Stresses" column says) and their PNGs are kept in the spec's results section. A pose that faces a wall is a wasted case; two of the café's first candidate poses did.

---

## 6. Matrix runner

`ENGINE_LAB=<plan>` runs a plan and quits. A plan is a text file in `tests/lab/` (one directive per line) or a built-in name. The lab has its own run loop, accumulators and settling code (§0); it calls the shared pure functions for tile comparison, leak metrics and PNGs.

### 6.1 Directives

```
cases   cafe.tables cafe.dining
views   Lit "Direct lighting"
kinds   stability diBias
base    di=shipped gi=shipped
target  di=nvidia  gi=nvidia
seeds   3                          # noise floor: the base on 3 seeds
sets    di=converged               # extra whole configs, run as given
ofat                               # every switch where base and target differ, flipped alone, both directions
sweep   di.spatialSamples 0 1 2 4  # one switch across values, on the base
pairs   di.spatialSamples di.permutation   # the 2x2 of two switches (values from the target, or name=value)
bisect  Lit:flicker                # §6.4, on one metric
fix     di.spatialSamples=2 di.permutation=on   # §6.6: the config levers run over
lever   L1 spatial: di.spatialSamples=2         # over the fix and on the base; the group is the first word
sweep   di.radius 4 8 16 32 over fix lever L1   # a sweep from the fix (plus overrides after `fix`), in a lever group
stack   Lit:std                    # the fix plus each lever group's best on the metric, after every other run
```

A plan is read top to bottom: each run directive (`sets`, `ofat`, `sweep`, `pairs`, `lever`, `bisect`, `stack`) runs on the `cases`, `views` and `kinds` lines above it, so one plan can give its levers different scopes. Kinds a case does not list (§5) are left out for that case; `diBias` always reads `Direct lighting` and `giBias` `Indirect radiance`, whatever `views` says. A metric is named `view:metric` with the names of `src/checks/lab/lab_plan.hpp` `kLabMetrics` (`std`, `flicker`, `lag1`, `bias`, `dimBias`, `hotBias`, `1b.dimBias`, `8b.bias`, `move.settle`, `pan.band`, …). A switch or case whose feature is not built (`notBuilt` in the registry, §3; a case's `notBuilt` in §5) skips its directive with a logged line; any other unknown name stops the plan before it runs. Plan files are `tests/lab/<name>` or `tests/lab/<name>.lab`.

Built-in plans: `sets` (shipped / NVIDIA / converged on every case), `di-ofat`, `gi-ofat`, `cafe` (what the café work ran by hand), `leak` (dim and dark leak on `cafe.door`, `guide.terminator`), `stability-levers` (§6.6).

### 6.2 Output

- `[Lab] MEASURED <plan>, <case>, <view>, <config>: <fields>` per run.
- `logs/lab/<plan>.csv`: one row per run, every field, the config string, the seed and the noise floor of the row's metric.
- A ranking at the end, per case and metric: `largest effects on cafe.tables Lit flicker: di.spatialSamples 0→2 −51.0 pts (17 floors), di.permutation off→on −40.7 (14), …`. Switches below 2 floors are listed as "no effect".

### 6.3 Interactions

For two switches A and B flipped alone and together, non-additivity is `|Δ(A+B) − Δ(A) − Δ(B)|`. Above 2 noise floors, the pair is reported as interacting and gets a `pairs` run in the next plan. The two-direction one-factor runs flag candidates for free: a switch with an effect from the base but none from the target (or the reverse) needs another switch to matter. F22 is that case: permutation and spatial reuse each fix most of it alone, together only a little more.

### 6.4 Bisection

When one-factor runs do not explain a difference between two configs (the effects do not add up to it within the noise), bisection finds the smallest set of switches that does. It uses delta debugging (ddmin) over the switches that differ:

1. Split the differing switches into two halves. Apply each half to the base alone.
2. If one half reproduces at least 80% of the target's effect on the metric, recurse into it.
3. If neither does, split finer (quarters, then complements), as ddmin does.
4. Stop at a set where removing any one switch loses the effect; report it and its runs.

Shipped and NVIDIA differ in 6 DI switches (sampling, permutation, spatial samples, boost history, spatial correction, final visibility reuse) and 9 GI switches (history, age, spatial samples, spatial correction, boiling filter, full-tile boiling, decorrelation, firefly replacement, firefly bound), so a bisection costs at most a few dozen runs. That is about 10–15 minutes per case on the café's step length.

### 6.5 Run time

One stability run is 780 frames (~9 s on the café on the RTX 5070), one `diBias` run adds a 1024-frame reference per case and seed, one `giBias` run is a GI-check phase (~20 s). A two-direction DI one-factor run on two café cases, `Lit` only, is `2 × 2 × 6` runs plus 3 seeds: under 5 minutes.

### 6.6 Built-in plan: `stability-levers` (F22 candidates)

The levers that could make the café stable enough that a viewer does not notice the noise, as games using RTXDI are. Each lever is tested alone over the F22 fix and in one stack, against its cost in bias, lag and GPU time. The plan only measures; I12 picks what ships.

**Base and fix.** `base di=shipped gi=shipped` (`di=tuned gi=tuned frame.indirectMode=nrcRestirGi` since the shipped sets became NVIDIA's, 2026-11-25, C-NS-6); the F22 fix is `di.spatialSamples=2 di.permutation=on` (measured by hand 2026-11-22: café `Lit` std 26.85% → 8.10% tables, 16.73% → 6.31% dining; flickering px 87% → 30%; DI bias −0.57% / −3.82%). Every lever below runs on top of the fix, so DI confetti does not mask it, and once on the shipped base, to see whether it helps without the fix. Since M8 I15 (spatial) and I16 (permutation) the fix ships, so `fix` equals the base and a lever's two readings coincide (one run); the levers that set the fix's own switches or the shipped mode are mirrored around the shipped base (2026-11-25): L1 turns spatial reuse and permutation off, L2 adds ReGIR without permutation, L5 runs `restirGi`, and L3's DI boiling sweep turns the filter on (off since I16). The `cafe` plan is mirrored the same way (`di.spatialSamples=0`, `di.permutation=off`, `gi.age=1024`, `frame.indirectMode=restirGi`), and `gi-ofat` bisects `8b.dimBias`: the shipped mode is multi-bounce.

| Lever | Configs (over the fix) | Why (source) | Watch for |
|---|---|---|---|
| L1 · DI spatial reuse and permutation | `di.spatialSamples=0`; `di.permutation=off`; both (mirrored, was `=2` / `=on` before I15 / I16); sweep `di.spatialSamples 0 2 4`; sweep `di.radius 4 8 16 32` and `gi.radius 4 8 16 32` | a single temporal pass is "too stable" for the denoiser; one spatial sample breaks the patterns (`NoiseAndBias.md` §Denoiser friendliness); every FullSample DI preset keeps ≥ 1. The radius sweep tests the claim that a tight radius (< 8 px) suits DLSS-RR better: NVIDIA's DLSS-RR profile keeps 32 px (`GetDefaultReSTIRPTSpatialResamplingParams(true)`) | DI bias with permutation (café dining −3.8%); I6's `LightsPunctualLamp` gate (`p0_r0_RT` failed it: +0.46%, p90 23.9%); +~0.2 ms (I6 courtyard) |
| L2 · ReGIR with spatial reuse | `di.sampling=regir` with spatial on, with and without permutation | ReGIR "can help with the boiling situation in large scenes with multiple rooms or similarly separated locations" (`NoiseAndBias.md` §Stages); the hand test (`shipped + ReGIR`) ran with spatial reuse off and only showed that ReGIR does not fix F22 alone | ReGIR's power-PDF MIS bias with BRDF candidates (C-F1-4: café +0.8% / +3.3% without spatial); `presample` and ray-start cost on Bistro (F12, I6: the reason `Power_RIS` ships) |
| L3 · Stronger boiling filters and firefly clamps | sweep `di.boilingStrength 0.1 0.2 0.35 0.5` with `di.boilingFilter=on`; `gi.boilingFilter=on` with sweep `gi.boilingStrength 0.1 0.2 0.5`; sweep `gi.fireflyStrength 0.7 0.9`; `gi.fireflyBound=on`; `gi.fireflyClamp=on` | RTXDI's boiling filter drops reservoirs above `10 / strength − 9` times their tile's mean (`Utils/BoilingFilter.hlsli`): 91× at 0.1, 41× at 0.2, 11× at 0.5, 2.1× at 0.9 (0.1 is weaker than the shipped 0.2, not stronger); the firefly replacement's bound is on in NVIDIA's DLSS-RR profile and "firefly replacement on its own brightens the image" (`RestirPT.md`) | darkening of real highlights and small bright lights (hot bias < 0, frame bias < 0); the bound darkened Sponza −7.1% / −3.9% (I5); the clamp is biased by design |
| L4 · History length, both ways | sweep `di.history 8 12 20 40 60`, `gi.history 4 8 12 20 40 60` | longer: more temporal samples per pixel, lower raw variance. Shorter: samples stick for fewer frames and DLSS-RR's own history does the long-term averaging. NVIDIA's DLSS-RR profile goes the other way: GI history 8 → 20 with decorrelation (`GetDefaultReSTIRPTTemporalResamplingParams(true)`) | lag: light change settle (Sponza sun move / toggle; café `cafe.toggle`); more temporal correlation (lag-1 correlation up) can cost DLSS-RR stability back, the F22 mechanism |
| L4c · Longer history with confidence | the L4 sweep with `frame.historyConfidence=on` (needs T8) | RTXDI computes temporal gradients and a filtered confidence (`Confidence.md`, FullSample `ComputeGradients.hlsl`, `FilterGradientsPass.hlsl`, `ConfidencePass.hlsl`) so history can be long where lighting is steady and reset where it changed | DLSS-RR has no confidence input (the NGX DLSSD parameters have hit distances, no confidence), so the confidence drives ReSTIR's own per-pixel history cap instead of the denoiser's: what is measured is L4's lag with L4's stability |
| L5 · World cache for the GI tail | `frame.indirectMode` `restirGi`, `nrcRestirGi`, `nrc` | ending the path in a radiance cache after the first bounce replaces the noisy tail with a smoothed estimate (`NrcGuide.md` §Algorithm overview). Measured by hand with the fix: café `Lit` std 8.10% → 7.93% tables, 6.31% → 6.06% dining, and the ~30% of 8-bounce energy one bounce misses comes back (F23). The light sampled at the first bounce stays noisy, so the cache cannot remove most of the boiling alone | NRC bias against the 8-bounce truth (I7's subject); NRC cost (I8) |
| L6 · DLSS-RR model, resolution and guides | `frame.dlssPreset` `Default`, `D`, `E`; `frame.dlssQuality` `DLAA`, `Quality`; `frame.specularGuides=on` (needs T10) | NVIDIA presents the DLSS 4 transformer RR models as more stable than the CNN model (release material, not in `reference/`; this lever measures it); DLAA gives the denoiser more input pixels per output pixel; the guide asks for specular hit distance or specular motion vectors, which the engine does not pass (`vk_dlss.hpp` `Frame`) | a model hint can be overridden by the NVIDIA app (NGX logs a DRS override; record it); DLAA's cost; specular guides matter on glossy cases more than on the café's matte walls |
| L7 · Shaping before the denoiser | `gi.targetCompression=on` with sweep `k`; `frame.preDenoiserClamp=on` with sweep of the multiple; `gi.fireflyClamp=on` (L3) for comparison | a compressed target PDF makes rare bright candidates win less often without bias; a clamp on DLSS-RR's input stops single-pixel spikes from entering its history | target compression must keep GI bias within the floor (it is unbiased only while nonzero where `p̂` is); the clamp is biased: hot bias and frame bias |
| Stack | the fix + the best value of each lever from the runs above | what the levers give together; interactions per §6.3 | everything above at once |

**Cases and kinds.**
- `cafe.tables`, `cafe.dining`: `stability` (`Lit`, raw `Direct lighting` and `Indirect radiance`), `diBias`, `giBias`.
- `sponza.gallery`, `sponza.courtyard`, `bistro.street`: `stability`, `giBias`. Every lever must not regress the scenes that are already stable.
- `guide.lamp`, `guide.terminator`, `guide.emissive`: `diBias` for every DI lever. These are I6's gates.
- `lightChange` on Sponza and `cafe.toggle`: for L4 and L4c.
- `glossy.*` (I12): for L6's specular guides and L7.
- `giBias` against the 8-bounce truth: for L5.
- `cost`: bench runs for the fix, L2, L5, L6 (`DLAA`) and the stack.

**Decision table the plan prints** per lever and case: Δ `Lit` std, Δ flicker share, Δ lag-1 correlation, Δ bias, Δ dim / hot bias, Δ settle frames and Δ GPU ms, each in noise floors. A lever is worth shipping when it cuts `Lit` std or flicker by more than 2 floors on the café, its bias and lag stay within 2 floors on every case, and its cost is measured.

**Target.** The plan reports the café against the courtyard's `Lit` std (1.3%) and the gallery's (5.8%). The bar is set in I12 from these numbers. None is fixed here.

---

## 7. Steps

| Step | Work | Done when |
|---|---|---|
| T1 · Registry | `src/render/settings_registry.{hpp,cpp}`: §3's switches with accessors, value lists, the shipped / NVIDIA / converged values read from `RtxdiContext`, the NVIDIA source string; `ENGINE_CONFIG` for the interactive engine; the UI's config string with copy; the engine checks log and ignore `ENGINE_CONFIG` | every field of `RtxdiContext::Settings` and `GiSettings` is a switch or listed as excluded with a reason; a config string round-trips; every engine check's lines byte-identical |
| T2 · Cases | `src/checks/lab/lab_cases.hpp`; the 3 guide scenes as lab cases (the guide capture unchanged); `cafe.door` chosen by PNG; lab runs on Bistro wait for async texture loads before their first run | lab café runs repeat byte for byte (the engine checks' café runs read ~0.2% today, F23 note); each case has a checked frame PNG |
| T3 · Measurements | the lab's run loop and accumulators; hot bias, lag-1 correlation; noise floor runs | on the shipped config, the lab's stability and bias numbers agree with the engine checks' lines for the same case and view within the lab's noise floor (§0 cross-check; the lab seeds its runs from their content, the checks from theirs, so equal numbers are not expected); lag-1 correlation of café shipped DI raw view is high and of DI + spatial reuse is low (the F22 mechanism, measured) |
| T4 · Matrix runner | `ENGINE_LAB`, plans, `sets` / `ofat` / `sweep` / `pairs`, CSV, ranking | the `cafe` plan reproduces the F22 table within the café's repeatability |
| T5 · Bisection | `bisect` directive | §T6's GI search ends on a switch set |
| T6 · Validation | run blind: `di-ofat` on the café (`Lit` flicker) and `gi-ofat` + `bisect` on `cafe.dining` GI dim bias | the DI ranking puts `di.spatialSamples` and `di.permutation` on top; the GI search names the switch behind F23 (expected `gi.fireflyBound`, §3.2) or names what it actually is. Validation passes when the runner finds the known culprit without being told it, not only when it agrees with the expected value |
| T7 · Guide | `docs/settings.md`: the env vars, the plan format, a "finding a culprit" checklist (§4.1 then §6) | a reader can go from a symptom to a plan without this spec |
| T8 · Confidence-driven history | a gradient pass after final shading and a spatial and temporal filter, after RTXDI's FullSample (`Confidence.md`). Its confidence lowers ReSTIR DI's and GI's per-pixel history cap where lighting changed, as duplication history reduction does for GI (`rtxdi_gi_temporal.slang`). The switch is `frame.historyConfidence`; `cafe.toggle` needs a per-material emissive scale | with `frame.historyConfidence=on`, L4's longest history settles a Sponza sun toggle and `cafe.toggle` within 2 noise floors of history 20's settle time, without losing L4's `Lit` stability; off, frames are byte-identical to today |
| T9 · `stability-levers` | the §6.6 plan as a built-in | it prints the decision table for every lever on every case listed there |
| T10 · Specular guides | DLSS-RR's `SpecularHitDistance` from the primary surface's specular ray (or specular motion vectors), behind `frame.specularGuides` | the guide is visible as a debug view, and it matches the RT check's primary-to-reflection distance; off, frames are byte-identical |
| T11 · Shaping switches | `gi.targetCompression` in the GI target PDF (`RAB_GetGISampleTargetPdfForSurface`), `frame.preDenoiserClamp` as a pass before DLSS-RR | each is off by default with byte-identical frames; target compression on keeps the café GI bias within the floor |
| T12 · Trim the café engine check | after T4 and T6: remove the café stability steps that try settings rather than measure what ships: the DI variants (`shipped + spatial reuse`, `+ permutation`, `+ ReGIR`, `+ spatial reuse + permutation`) and the GI and NRC steps run over `DI shipped + spatial reuse + permutation` (`EngineTemporalCheck::buildSteps`, `DiSet`). The café check keeps RTXDI's reference, the shipped / NVIDIA / converged DI sets, the GI sets and the NRC modes | the lab's `cafe` plan reproduces every removed step's numbers within the café's repeatability before the steps go; the remaining café lines and every Sponza line are byte-identical before and after |

Order: T1 → T2 → T3 → T4 → T5 → T6 → T7. T9 needs T4 (L1–L5, L6 without guides and the stack run without T5); L4c needs T8, L6's guides T10 and L7 T11; T8, T10 and T11 can run alongside T2–T5. T12 comes last, once the lab reproduces what it removes.

T8, T10 and T11 are engine features the lab needs, not lab code: each is off by default, so frames and the engine checks' lines stay byte-identical until a step decides to ship it. The café's settings experiments inside the engine check stay until T12 moves them to the lab; the comparison sets an engine check always had (shipped, NVIDIA, converged, I1's GI sets) stay for good.

---

## 8. Rules and known limits

- **No shipped setting changes here.** The runner finds culprits; the step that owns the setting (I12 for F22 / F23) changes it, with I6's and I5's gates re-run.
- **Reference first** still applies to fixes: a culprit is confirmed only when its mechanism (§3 "What it does") explains the signature, ideally in NVIDIA's own words.
- **Reference noise.** In the café, RTXDI's DI reference at 1024 frames still reads tile p90 10–15% against itself, and the one-bounce GI truth ±5% bias. Tile metrics there compare at 2 noise floors, not at Sponza's limits.
- **Parallel sessions.** Two sessions sharing `build/windows-clang-release` overwrite each other's `logs/engine.log` and lock DLLs. A lab run uses its own build tree (`cmake --preset windows-clang-release -B build/<name> -DFMT_MODULE=OFF`).
- **One tool per process.** `ENGINE_LAB` together with any engine check's variable is refused at startup: a process either measures the shipped state or tries settings, never both.

---

## 9. Build status (2026-11-22)

Built and compiled in `build/cafe-release`; no lab plan has run yet, so nothing in §7's "Done when" column is checked except what `tests/lab_registry_test.cpp` covers (CPU only: every switch round-trips through a config string, sets apply before overrides, typos are refused, every built-in plan parses and expands).

| Step | State | Where |
|---|---|---|
| T1 · Registry | built: 57 switches (and 5 planned ones) over `RtxdiContext::Settings` / `GiSettings`, `Dlss::Settings`, `NrcCache::Settings`, indirect mode, jitter; `static_assert` on each struct's size so a new field breaks the build until it gets a switch; `ENGINE_CONFIG`; the UI's config string and *Copy config* button (Lighting panel, under *Defaults*); the checks and the lab log `ENGINE_CONFIG ignored` | `src/render/settings_registry.{hpp,cpp}`, `src/checks/check_harness.cpp` `applyEngineConfig`, `src/ui/engine_ui.cpp` |
| T2 · Cases | built: 9 runnable cases; `cafe.door` (pose not chosen), `glossy.*` (I12), `glass.window` (I13), `cafe.toggle` (T8) are listed with `notBuilt` and skipped. Not checked: frame PNGs per case, byte-repeatability on Bistro | `src/checks/lab/lab_cases.hpp` |
| T3 · Measurements | built: stability (plus lag-1 correlation), `diBias` (1024-frame RTXDI reference per case, DLSS mode, jitter and seed), `giBias` (one-bounce and 8-bounce truths, 2048 frames, jitter off), `lightChange`, `screenEdge` (I1's definitions, the lab's copy), hot bias (`convergence_metrics` `TileErrors::hotBias`, tiles above 10× the average lit tile; the checks do not print it), noise floors (the base on `seeds`, plus the reference read against itself for signed bias metrics). `cost` is not measured in the lab's process: the plan writes `logs/lab/<plan>_cost.ps1` with ABBA bench runs per config (bench mode knows the startup camera and the courtyard pose only, so café cases have no cost run) | `src/checks/lab/lab_metrics.{hpp,cpp}`, `src/checks/lab/engine_lab.cpp` |
| T4 · Runner | built: `ENGINE_LAB`, plans, every directive of §6.1, `[Lab] MEASURED` lines, `logs/lab/<plan>.csv` (one row per run, every metric and its floor), the ranking per case, view and metric, the interaction flags of §6.3 (one-direction OFAT effects, pair non-additivity) | `src/checks/lab/lab_plan.{hpp,cpp}`, `src/checks/lab/engine_lab.{hpp,cpp}` |
| T5 · Bisection | built: ddmin over the switches base and target differ in, 80% reproduce share, a target effect below 2 floors ends it as noise | `EngineLab::advanceBisection` |
| T6 · Validation | open: needs runs | — |
| T7 · Guide | built | `docs/settings.md` §9 |
| T8, T10, T11 | open: engine features; their switches exist as planned (`frame.historyConfidence`, `frame.specularGuides`, `gi.targetCompression`, `frame.preDenoiserClamp`, `scene.emissiveScale`) and are refused by `ENGINE_CONFIG` and skipped by plans | — |
| T9 · `stability-levers` | built as a plan: 1306 runs before the stack (L4c, L6's guides and L7's target compression and clamp skipped until T8 / T10 / T11), a decision line per lever and case, a verdict per lever, the target line, `logs/lab/stability-levers_decisions.csv`. At ~10 s a run this is a multi-hour plan; `ENGINE_LAB_CASES` narrows it | `src/checks/lab/lab_plan.cpp` `kStabilityLeversPlan` |
| T12 · Trim the café check | done 2026-11-26, ahead of T6 (the user's call). The removed steps had become exact duplicates of kept ones: with NVIDIA's `Medium` shipping (C-NS-6), `shipped + spatial reuse / permutation / ReGIR` all equal NVIDIA's DI or sit inside the tuned set the lab plans now name. Before / after on the release tree, validation off: Sponza's 28 lines byte-identical; café 44 steps, 0 failed, kept non-NRC lines within the café's run-to-run spread (means ≤ 0.09%, std ≤ 1.2%, flicker ≤ 1.8%, DI bias ≤ 0.02 pp; steps ahead of the removed block drift the same), NRC lines vary more (C-T-5). The lab's `cafe` plan ran in place of the removed steps: 116 runs, 0 failed, ~34 min; tuned base `Lit` std 2.67% tables / 2.84% dining (I16: 2.76% / 2.95%); spatial reuse and permutation off 6.5% / 4.4%, permutation off alone 3.3% / 3.7%, `gi.decorrelation=none` 4.8% / 4.9%; NVIDIA's shipped sets 4.9% / 6.4%, and NVIDIA's `Medium` DI over the tuned GI reads DI bias −2.7% tables / −11.1% dining against RTXDI's reference (tuned −0.03% / −0.22%) | `src/checks/engine_temporal_check.{hpp,cpp}` (`DiSet`), `src/checks/lab/lab_plan.cpp` `kCafePlan`; logs `build/cafe-release/logs/t12_*.log`, `logs/lab/cafe.csv` |

Runs per built-in plan before bisection and stacks: `sets` 147, `di-ofat` 98, `gi-ofat` 201 (plus its `cafe.dining` bisection), `cafe` 116, `leak` 36, `stability-levers` 1306.
