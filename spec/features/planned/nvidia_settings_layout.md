# Feature: NVIDIA settings layout and denoiser off

**Date:** 2026-11-25 · **Status:** implemented, GPU checks of S3–S5 pending (considerations C-NS-1) · **Milestone:** M8 tooling, beside the settings lab ([full_testing.md](./full_testing.md))
**Implements:** a `Settings` window that copies the RTXDI FullSample's ImGui layout (sections, order, labels, ranges, help text), so an engine config and an NVIDIA sample config can be compared row by row; a denoiser mode with `Off`; NVIDIA's `Accumulation` AA mode for converged images without a denoiser; highlighting of the rows that differ from a chosen comparison config
**Reference:** RTXDI `Samples/FullSample/Source/UserInterface.cpp` (`buildUI` L2198, `SceneSettings` L1876, `GeneralRenderingSettings` L549, `SamplingSettings` L602, `DenoiserSettings` L1658, `PostProcessSettings` L1357, `UIData::ApplyPreset`, `ApplyDenoiserCompatDefaults` L443, header colors L532–533); RTXDI `App/SceneRenderer.cpp` (`ProcessAccumulationModeLogic` L1321, camera-static test L1078, `ResolveAA` L1670), `Shaders/AccumulationPass.hlsl`; RTXGI `Samples/IndirectDiffuse/UI.cpp` (`NRC:` header L294, denoiser radio L237–275)
**Touches:** `src/ui/engine_ui.{hpp,cpp}`, `src/render/vk_renderer.{hpp,cpp}`, `src/render/vk_post.*`, `src/render/vk_rtxdi.{hpp,cpp}`, `src/lab/settings_registry.{hpp,cpp}`, [docs/settings.md](../../../docs/settings.md)

Today the lighting settings sit under one `Ray tracing` header in the engine's own order (Mega Geometry, ReSTIR DI, glass, DLSS-RR, indirect mode, NRC, ReSTIR GI), with labels and ranges of our own, plus separate `Sun` and `Post` headers. The RTXDI sample groups the same settings differently (Scene → General Rendering → Static Context → Direct Lighting → Indirect Lighting → Denoising → Post-Processing → Debug), names them differently ("Discard Invisible Samples" is our "Visibility shortcut") and nests them in tree nodes per resampling stage. Comparing a config against the sample means translating every row by hand.

Both NVIDIA samples also let the denoiser be switched off (`DenoiserMode::NONE` in RTXDI, `DenoiserType::None` in RTXGI), and both can then average still frames (`AntiAliasingMode::Accumulation` in RTXDI, `DenoiserType::Accumulation` in RTXGI) to show what the estimator converges to. The engine always runs DLSS-RR on `Lit`, so the raw ReSTIR output is visible only through the `DLSS input` debug view: no exposure, no tonemap, render resolution. A converged image exists only inside the checks.

This feature changes the UI and adds two renderer modes (denoiser off, accumulation). No shipped default changes.

---

## 1. NVIDIA's layout

`buildUI` opens two windows: `Settings (Tilde key to hide)` at (10, 10), auto-resize, minimum width 375, item width 100; and `Performance` at the top right. Sections are colored tree nodes (`ImGui_ColoredTreeNode`): regular `0xffff8080`, attention `0xff80ffff`, each followed by a separator. Help is a `(?)` marker after the widget (`ShowHelpMarker`). A sampling change sets the preset to `(Custom)` and resets accumulation.

| # | Section (color) | Open | Contents |
|---|---|---|---|
| 1 | Scene (regular) | closed | animations, alpha-tested and transparent geometry, environment map, bias, rotation, global emissive factor, material and light editors, camera |
| 2 | General Rendering (regular) | closed | reload shaders, RayQuery, rasterize G-buffer, resolution scale, FPS limit, pixel jitter |
| 3 | Static ReSTIR Context Settings (regular) | closed | env map importance sampling; `RTXDI Context` (checkerboard), `ReGIR Context` (lights per cell, mode, grid / onion) with `Apply Settings` |
| 4 | Direct Lighting (attention) | closed | mode, `Preset`, `Show Advanced Settings`, `Resampling Mode`; nodes `ReGIR Presampling`, `Initial Sampling` (→ `Local Light Sampling`), `Temporal Resampling`, `Spatial Resampling`, `Final Shading` |
| 5 | Indirect Lighting (attention) | **open** | mode, `Min Secondary Roughness`; nodes `Secondary Surface Light Sampling`, `Reuse Primary Samples`; for ReSTIR GI: `Resampling Mode`, nodes `Temporal Resampling`, `Spatial Resampling`, then `Final visibility`, `Final MIS`; ReSTIR PT nodes (`Decorrelation`, …) |
| 6 | Denoising (attention) | closed | `Denoiser mode` (Off / NRD ReLAX / NRD ReBLUR / DLSS-RR), `Auto-apply denoiser compatibility preset` |
| 7 | Post-Processing (regular) | closed | AA radio (No AA / Accumulation / TAAU / DLSS SR; disabled with "Post processing provided by DLSS-RR"), reference image, tone mapping, exposure bias, bloom |
| 8 | Debug (regular) | closed | shader debug print, path visualization, render output |

`buildUI` calls `DenoiserSettings` only `#ifdef WITH_NRD`. The engine shows section 6 unconditionally; it is our only route to the denoiser.

---

## 2. Target layout

One `Settings` window in NVIDIA's position, size rules, colors, order and open states. It takes over the `Ray tracing`, `Sun` and `Post` headers. The `Engine` window keeps load, models, camera, culling, debug view, instance field and assets. The stats overlay stays; NVIDIA's `Performance` window is not copied.

Rules:
- **Labels, ranges and help text are NVIDIA's**, verbatim, including their order inside a node. Where the engine's range must differ (a hard limit, a value NVIDIA's range excludes), the engine's wins and the help marker says why. The current engine tooltips move into the `(?)` text after NVIDIA's.
- **Rows NVIDIA has and the engine does not** stay in place, disabled, showing the engine's fixed value (e.g. `Secondary Samples 2`), or `(not in engine)` when there is no value. Rows that only make sense in NVIDIA's framework (reload shaders, RayQuery toggle, D3D12 notes, shader debug print) are left out and listed in §2.9.
- **Rows the engine has and NVIDIA does not** go in one `Engine` tree node at the end of the section they belong to, so the NVIDIA part reads 1:1. The ReSTIR PT DLSS-RR profile ported to GI (O11) keeps NVIDIA's ReSTIR PT labels.
- `Show Advanced Settings` hides the same rows NVIDIA hides.
- A change in Direct Lighting sets `Preset` to `(Custom)`, as NVIDIA's `samplingSettingsChanged`. History resets stay what they are today; the engine has no accumulation to reset.
- The window ends with the current `Defaults` and `Copy config` buttons and the config string.

### 2.1 Scene

| NVIDIA | Engine |
|---|---|
| Animations, Animation Speed | (not in engine) |
| Alpha-Tested Geometry | disabled, on (the alpha bin always runs) |
| Transparent Geometry | `GlassSettings::isPassEnabled` |
| Environment, Environment Bias (EV), Environment Rotation | (not in engine) |
| Global Emissive Factor | (not in engine; lab `scene.emissiveScale` is planned, T8) |
| `Engine` node | the `Sun` panel; the rest of `GlassSettings` (max layers, reflection, diffuse share, emission, shadow transmittance, DLSS guide blend) |

### 2.2 General Rendering

| NVIDIA | Engine |
|---|---|
| Resolution Scale (%) | disabled, the render-to-display scale of the DLSS mode (§2.6) |
| Pixel Jitter (checkbox + MSAA / Halton / R2 / White Noise) | checkbox: `frame.jitter`; pattern disabled, Halton |
| `Engine` node | RTX Mega Geometry: PTLAS line, `Instances per partition`, `Force full PTLAS build` |

### 2.3 Static ReSTIR Context Settings

| NVIDIA | Engine |
|---|---|
| Importance Sample Env. Map | (not in engine) |
| RTXDI Context → Checkerboard Rendering | disabled, off |
| ReGIR Context → Lights per Cell, ReGIR Mode, grid / onion, Total ReGIR Cells | disabled, the engine's ReGIR static parameters (read from `RtxdiContext`) |

### 2.4 Direct Lighting

| NVIDIA | Engine |
|---|---|
| Direct Lighting Mode | disabled, `ReSTIR` |
| Preset | §3 |
| Resampling Mode: None / Temporal / Spatial / Temporal + Spatial | None: `RtxdiContext::isReference`; Spatial: `isSpatialOnly`; Temporal + Spatial: both off; Temporal: a new `RtxdiContext` value (RTXDI's `ReSTIRDI_ResamplingMode::Temporal`), S2 |
| ReGIR Presampling (mode, cell size, build samples, jitter, freeze, visualize) | disabled, the engine's values |
| Initial Sampling → Local Light Sampling | `localLightSamplingMode` and `activeLocalLightSamples()` (per-mode counts, as NVIDIA's `neeLocalLightSampling`) |
| ReGIR RIS Fallback Sampling Mode | disabled, the engine's value |
| Initial BRDF Samples (0–8) | `brdfSamples` |
| Initial Infinite Light Samples (0–32) | `infiniteLightSamples`; disabled while exact directional lights are on |
| Initial Environment Samples | (not in engine) |
| Enable Initial Visibility | `isInitialVisibilityEnabled` |
| BRDF Sample Cutoff, BRDF Ray Min T | disabled, the engine's values |
| Temporal → Enable Fallback Sampling | `isFallbackSamplingEnabled` |
| Enable Previous Frame TLAS/BLAS | disabled, off |
| Enable Permutation Sampling | `isPermutationSamplingEnabled` |
| Temporal Bias Correction | `temporalBiasCorrection` |
| (advanced) Temporal Depth / Normal Threshold | `depthThreshold` / `normalThreshold`; help: one pair serves temporal and spatial |
| (advanced) Permutation Sampling Threshold | disabled, the engine's value |
| Max History Length (1–100) | `maxHistoryLength` |
| Boiling Filter (checkbox + strength) | `isBoilingFilterEnabled`, `boilingFilterStrength` |
| Spatial → DI Neighbor Offset Sampling | disabled, `Stratified Low Discrepancy` (the reason spatial counts are even) |
| Spatial Bias Correction | `spatialBiasCorrection` |
| Spatial Samples, Disocclusion Boost Samples | `spatialSamples`, `disocclusionBoostSamples` (even) |
| Spatial Sampling Radius | `samplingRadius` |
| (advanced) Spatial Depth / Normal Threshold | the same shared pair |
| (advanced) Discount Naive Samples | disabled, the engine's value |
| Final Shading → Enable Final Visibility | disabled, on |
| Discard Invisible Samples | `isVisibilityShortcutEnabled` |
| Reuse Final Visibility | `isFinalVisibilityReused` |
| (advanced) Final Visibility – Max Distance / Max Age | disabled, 16 px / 4 frames |
| `Engine` node | light counts line, `Sample triangle lights`, `Exact directional lights`, `Boost history length`, `Material similarity`, per-pixel light tiles |

### 2.5 Indirect Lighting

| NVIDIA | Engine |
|---|---|
| Indirect Lighting Mode (None / BRDF / ReSTIR GI / ReSTIR PT) | the engine's `Indirect mode` (`NRC + ReSTIR GI`, `NRC`, `ReSTIR GI`); help maps `ReSTIR GI` to NVIDIA's `ReSTIR GI` and the NRC modes to the RTXGI sample |
| Min Secondary Roughness | `isSecondaryRoughnessFloorEnabled` as a checkbox showing `0.5` |
| Secondary Surface Light Sampling (local / infinite / environment samples) | disabled: path vertices use the primary surface's counts (`sampleLightsAt`) |
| Reuse Primary Samples → Reuse RTXDI samples for secondary surface | `isSecondaryResamplingEnabled` |
| Secondary Bias Correction, Samples, Radius, Depth / Normal Threshold | disabled, `secondaryResamplingParams`: Ray Traced, 2, 4 px, 0.1, 0.9 |
| `NRC` node (RTXGI's `NRC:` header) | `Train The Cache`: `isTrainingEnabled`; `Resolve Mode`: `resolveMode`; Training Bounces, Learn Irradiance, Include Direct Illumination, Skip delta vertices, Heuristic Threshold, Max Average Radiance: disabled, the engine's values; status line |
| Resampling Mode (None / Temporal / Spatial / Temporal + Spatial / Fused) | None: `isGiReference`; Temporal + Spatial otherwise; the others disabled |
| Temporal → Temporal Depth / Normal Threshold | `depthThreshold` / `normalThreshold` (GI pair) |
| Max reservoir age (NVIDIA 1–100) | `maxReservoirAge`, range to `kGiUnlimitedSampleAge` (1024 = never) |
| Max history length, permutation, fallback, temporal bias correction, boiling filter | the matching `GiSettings` fields |
| Spatial → Num spatial samples (NVIDIA 1–7), Sampling Radius, thresholds, Spatial Bias Correction Mode | `spatialSamples` (0–8), `samplingRadius`, the GI pair, `spatialBiasCorrection` |
| Final visibility, Final MIS | `isFinalVisibilityEnabled`, `isFinalMisEnabled` |
| `Engine` node, `ReSTIR PT DLSS-RR profile` sub-node with NVIDIA's PT labels | decorrelation (mode, factor, stagnancy exponent, EMA), firefly replacement (strength, full tile, bound), duplication history reduction; then `Firefly clamp`, `Exact directional lights##gi`, `Ground-truth paths`, `Bounces after X2`, NRC `idle train frames` and `training iterations`, the `Defaults` / `NVIDIA defaults` / `Converged` buttons |

### 2.6 Denoising

| NVIDIA | Engine |
|---|---|
| Denoiser mode | `Off`, `DLSS-RR` (§4) |
| Auto-apply denoiser compatibility preset | §4.3 |
| (RTXGI: DLSS Mode) | `DLSS-RR` sub-node: `Mode` (quality), `Preset`, `Specular hit distance guide`, `Reset history`, the render → display line; disabled while the denoiser is off |

### 2.7 Post-Processing

| NVIDIA | Engine |
|---|---|
| AA radio | `No AA`, `Accumulation` (§4.4); `TAAU`, `DLSS SR` disabled `(not in engine)`. With DLSS-RR the radio is disabled with NVIDIA's "Post processing provided by DLSS-RR" |
| Accum. Frame Limit (0–1024) | `Renderer::accumulationFrameLimit`, with NVIDIA's `// N frame(s)` counter in Accumulation |
| Reference Image (Store, Toggle, Split Display) | left out (§2.9) |
| Apply Textures in Compositing | (not in engine) |
| Tone mapping | disabled, on (ACES, Hill's fit) |
| Exposure bias | `compensationEv` with auto exposure, `manualEv` without |
| Bloom | (not in engine) |
| `Engine` node | exposure readout, auto exposure, adaptation speeds, exposure range, `Snap to target`, `Defaults##post` |

### 2.8 Debug

Not copied. The engine's `Debug view` panel stays in the `Engine` window.

### 2.9 Left out

Reload Shaders, Use RayQuery, Rasterize G-Buffer, FPS Limit (§2.2); NRD denoisers and their settings (§2.6); shader debug print, path visualization (§2.8); the `Performance` window; benchmark and loading screens. None has an engine counterpart. The reference image split view (§2.7) is left out by decision (§9).

---

## 3. Presets

The DI `Preset` combo holds NVIDIA's list in NVIDIA's order, then the engine's sets:

`(Custom)`, `Fast`, `Medium`, `Unbiased`, `Ultra`, `Reference`, `Shipped`, `Converged`.

- NVIDIA's five come from `UIData::ApplyPreset`, ported into `RtxdiContext` as `nvidiaPreset(QualityPreset)` next to `nvidiaSettings()` (today's `NVIDIA defaults` = `Medium`). Fields the engine lacks are skipped and listed in a comment per preset: checkerboard (`Fast`), gradients, `enablePrevTLAS`. `Reference` maps to `isReference`.
- `Shipped` is `defaultSettings()`, `Converged` is `convergedSettings()`. They replace the `Defaults##di`, `NVIDIA defaults` and `Converged` buttons.
- NVIDIA re-applies the DLSS-RR compatibility preset after a quality preset (`autoApplyDLSSRRPreset`); the engine does the same with §4.3.
- On start the combo shows `Shipped`, and any edit turns it to `(Custom)`.
- Lab: the presets become DI set names (`di=fast`, `di=ultra`, …) beside `shipped`, `nvidia` and `converged`, so a plan can compare against any NVIDIA preset.

---

## 4. Denoiser off

### 4.1 Behavior

`Renderer::denoiser` (`DenoiserMode::DlssRr` default, `DenoiserMode::Off`), read once per recorded frame like `indirectMode`.

| | DLSS-RR (today) | Off |
|---|---|---|
| Render extent | the DLSS mode's | the display extent, as DLAA. NVIDIA's samples render native; this keeps the comparison 1:1 ([settings.md](../../../docs/settings.md) §1, Resolution) |
| Jitter | `frame.jitter` | `No AA`: none, nothing resolves it; `Pixel Jitter` is disabled and shows off. `Accumulation`: `frame.jitter`, as NVIDIA keeps advancing its jitter so the average is antialiased |
| `Lit` | combine → DLSS-RR → post → present | `No AA`: combine → post → present, post reads `noisyColor` instead of DLSS-RR's output. `Accumulation`: combine → accumulate → post → present (§4.4) |
| DLSS-RR | evaluated | not evaluated; the feature is kept; history is invalid on the way back |
| Exposure and tonemap | on DLSS-RR's output | on `noisyColor`; fireflies can pull auto exposure down, `Exposure bias` and manual exposure behave as before |
| Guides, glass guide blend | written, consumed | written, unused |
| `frame.inputHistory`, `frame.inputFrozen`, pre-denoiser clamp (I16) | shape DLSS-RR's input | shape the presented image: they act on `noisyColor` |
| GPU timer `DLSS-RR` | measured | 0 |
| Display capture | post output | post output |

A switch happens at a frame boundary. It changes the render extent unless the DLSS mode is already DLAA, through the same path as a DLSS mode change: targets, NRC configure and the DLSS feature are recreated, history resets. `isDlssPresented()` splits in two: `isLitPresented()` (Lit and lights) for post and capture, and `isDlssEvaluated()` (also needs `DlssRr`) for the reconstruction pass and its barriers.

### 4.2 Config and checks

- Lab switches, all part of `frame=shipped`, carried by the config string, so a copied config reproduces the view and `ENGINE_CONFIG` sets it for the interactive engine and bench runs:
  - `frame.denoiser`, enum `dlssRr` / `off`, shipped `dlssRr` (`LabConfig::denoiser`);
  - `frame.aa`, enum `none` / `accumulation`, shipped `none`, read only while the denoiser is off (`LabConfig::aaMode`);
  - `frame.accumulationFrames`, uint, 0 = no limit, shipped 0 (`LabConfig::accumulationFrameLimit`).
- The engine checks ignore `ENGINE_CONFIG` and run with DLSS-RR, unchanged. Lab kinds work in both denoiser modes; with `off`, `stability` measures the raw image. `accumulation` is for viewing: it converges by design, so the lab's temporal kinds have nothing to measure in it.
- No new environment variable.

### 4.3 Compatibility preset

As NVIDIA's `ApplyDenoiserCompatDefaults`: the checkbox (default on) applies the denoiser-dependent GI fields when the mode changes, and leaves the rest alone.

| Field | → Off | → DLSS-RR |
|---|---|---|
| `maxHistoryLength` | `GetDefaultReSTIRPTTemporalResamplingParams(false)` | `defaultGiSettings()` |
| `maxReservoirAge` (from `enableAgeBasedRejection`) | the same `(false)` | `defaultGiSettings()` |
| `isDuplicationHistoryReductionEnabled` | the same `(false)` | `defaultGiSettings()` |
| decorrelation | `GetDefaultReSTIRPTDecorrelationParams(false)` | `defaultGiSettings()` |
| GI boiling filter | `GetDefaultReSTIRPTBoilingFilterParams(false)` | `defaultGiSettings()` |

The way back uses the shipped values, not NVIDIA's `(true)` profile: the shipped GI set is the engine's DLSS-RR profile (I5, I15 deviations). DI has no denoiser-dependent fields in NVIDIA's preset and is not touched.

### 4.4 Accumulation

NVIDIA's `Accumulation` AA mode, as `ProcessAccumulationModeLogic` and `AccumulationPass.hlsl` do it:

- **Average.** A running mean of `noisyColor` (linear, before exposure) at the render extent, which is the display extent while the denoiser is off: `accumulated = lerp(accumulated, noisyColor, 1 / n)`, and a plain copy at `n = 1`. The target is `RGBA32F`, so a mean over 1,024 frames keeps its low bits. Post reads `accumulated`.
- **Count.** `n` grows by one per frame while the camera is still and nothing reset it, capped at `accumulationFrameLimit` when that is above 0. Otherwise `n = 1`.
- **Still camera.** The unjittered view matrix equals last frame's (NVIDIA compares `GetViewMatrix()`).
- **Reset** (`n = 1`) on:
  - every history reset the renderer already raises: camera cut, resize, scene layout change, light set change, indirect state change, DI history reset;
  - any change of a lab switch's value between two frames (`differingSwitches` of last frame's and this frame's `captureLabConfig`), which covers every UI edit as NVIDIA's `resetAccumulation` does;
  - a switch into or out of `Accumulation`, and the frame limit lowered below `n`.
- **NRC.** NRC keeps training while the frames add up, so early frames carry an untrained cache. The average includes them, as in NVIDIA's NRC sample. `NRC idle train frames` bounds how long the cache changes after a reset.
- **Exposure.** Auto exposure meters `accumulated` and settles as it converges.
- **UI.** `Accum. Frame Limit` and NVIDIA's `// N frame(s)` counter in Post-Processing; the stats overlay shows `accumulating N` in this mode.
- **Cost.** One full-screen pass with a timer scope `Accumulation`; nothing runs in the other modes.

---

## 5. Compare with

A `Compare with` combo at the top of the window highlights every row whose value differs from a comparison config. Its entries:

| Entry | DI | GI | Frame |
|---|---|---|---|
| None | no highlighting | | |
| Shipped (default) | `defaultSettings()` | `defaultGiSettings()` | `frame=shipped` |
| NVIDIA `Fast` … `Reference` | `nvidiaPreset(…)` (§3) | `nvidiaGiSettings()` | `frame=shipped` with NVIDIA's frame: `frame.indirectMode=restirGi` (the sample has no NRC), `frame.dlssQuality=dlaa` (it renders native) |
| Converged | `convergedSettings()` | `convergedGiSettings()` | `frame=shipped` |

- **One source of truth.** The comparison is `differingSwitches(comparison, captureLabConfig(current))` from the lab registry, computed once per UI frame. Every row that edits a setting names its lab switch, and it is highlighted when that switch is in the set. The registry's `static_assert`s already force a switch for every settings field, so no row can miss its comparison. Disabled rows (fixed values) are never highlighted.
- **Look.** The label is drawn in an orange outside NVIDIA's two header colors (`0xff40a0ff`), and the `(?)` text gains a first line `<entry>: <value>` with the comparison's value, formatted by `formatSwitchValue`.
- **Sections.** A section or node that holds a highlighted row shows the count after its label (`Direct Lighting · 3`, with a stable `###` id), so a closed section still shows that it differs.
- **Config string.** It stays at the bottom and is unchanged. It writes the nearest named set plus overrides, independent of this combo.

---

## 6. Code

- `src/ui/settings_window.{hpp,cpp}`: the window, §2's sections as one function each, `coloredTreeNode` and `helpMarker` ported from NVIDIA's helpers. `engine_ui.cpp` is 1,659 lines, past the 1,200 at which [engine_ui_split.md](../done/engine_ui_split.md) splits it by panel. `EngineUi` owns it and calls it after the `Engine` window.
- Moved rows keep their fields; only labels, ranges, order and help change.
- `src/render/denoiser_mode.hpp`: `DenoiserMode` and `AaMode` with their labels, as `indirect_mode.hpp`.
- `RtxdiContext`: `nvidiaPreset()`, the `Temporal` resampling mode, read-only accessors for the disabled rows' fixed values.
- `PostProcess::record` takes its input image from the caller.
- `shaders/passes/accumulate.slang` and an `accumulated` target in `RenderTargets` (created only while `Accumulation` is selected, freed with the slot's retirement when it is left).
- The comparison (§5) runs in the UI from `src/lab/settings_registry.hpp`; the settings window depends on the registry, not the reverse.

---

## 7. Steps

| Step | Work | Done when |
|---|---|---|
| S1 · Window | §2 with today's controls moved, disabled rows, `Engine` nodes, help markers; `Ray tracing`, `Sun`, `Post` headers removed; the stale tooltips of today's DI and GI `Defaults` buttons ("no permutation", "no age limit") replaced by the shipped sets' text | every current lighting control is reachable in the new window; side by side with a FullSample screenshot, sections, labels and order match; the config string of an untouched engine is unchanged |
| S2 · Presets and resampling mode | §3; `Temporal` DI resampling | each NVIDIA preset's fields match `ApplyPreset` field by field (listed skips aside); `lab_registry_test` parses `di=fast` … `di=reference` |
| S3 · Denoiser off | §4 | `Lit` with `Off` shows the tonemapped raw image at display resolution with no jitter; switching both ways at DLAA and at Quality loses no history beyond one reset and keeps validation clean; a bench row (GPU total, frame time) with `frame.denoiser=off` on the RTX 5070; `lab_registry_test` round-trips `frame.denoiser` |
| S4 · Accumulation | §4.4; `frame.aa`, `frame.accumulationFrames` | on a still Sponza courtyard, accumulated `Lit` changes by less than 0.1% in mean between frames 512 and 1,024 (ReSTIR GI mode, so no cache trains meanwhile); any UI edit, a camera move or a mode switch resets the counter to 1; validation clean; `lab_registry_test` round-trips both switches |
| S5 · Compare with | §5 | with `Shipped`, an untouched engine highlights nothing; with `NVIDIA Medium`, the highlighted rows are exactly `differingSwitches(nvidia, shipped)` (one test listing both); counts on closed sections match |
| S6 · Guide | [docs/settings.md](../../../docs/settings.md) names the new window's sections; its §1 deviations list points to NVIDIA's labels; its §6 (checking a change) explains `Accumulation` and `Compare with` | the guide's setting names are the window's |

---

## 8. Not in scope

- NVIDIA's reference image (`Store`, `Toggle`, `Split Display`).
- NVIDIA's `Performance` window and profiler UI; the stats overlay and the `Culling` panel's timings stay.

## 9. Decisions

| Date | Decision |
|---|---|
| 2026-11-25 | User: copy the RTXDI sample's settings layout; add a denoiser `Off` mode |
| 2026-11-25 | User: add NVIDIA's `Accumulation` (§4.4) and the `Compare with` highlighting (§5); leave out the reference image split view |
