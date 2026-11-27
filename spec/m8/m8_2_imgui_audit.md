# M8.2 · ImGui & Settings Wiring Audit

**Date:** 2026-11-26 · **Status:** complete (audit + all 12 fixes + `C-NS-1` GPU gates verified; ready for M8.3) · **Milestone:** [M8](./README.md)
**Baseline:** post-M8.1 Wave 1 & Wave 2 (`14/14` GI check, `8,890/8,890` RT check, `7/7` + `8/8` ctest, clean validation)
**Links:** [M8 README](./README.md) · [M8.1 Audit](./m8_1_pipeline_audit.md) · [NVIDIA Settings Layout](../features/planned/nvidia_settings_layout.md) · [Settings Guide](../../docs/settings.md) · [Considerations](../considerations.md)

---

## 1. Objective

Audit every widget in `SettingsWindow` (`src/ui/settings_window.{hpp,cpp}`) and `EngineUi` (`src/ui/engine_ui.{hpp,cpp}`) end-to-end through `Renderer`, `RtxdiContext`, `NrcCache`, `Dlss`, `PostProcess`, `WorldLights`, `RayTracingScene`, `MeshletCulling`, `Camera`, and the consuming Slang shaders to verify:

1. Every editable control mutates live GPU state (no dead, overwritten, or silently ignored fields).
2. Every control that changes temporal or denoiser state cleanly resets DLSS-RR history, ReSTIR DI/GI history, and `Accumulation` frame count as appropriate.
3. Disabled and read-only rows display the true runtime value (never stale/misleading values when overridden by `isReference`, `IndirectMode::Nrc`, or `arePathInfiniteLightsExact`).
4. `Defaults`, `Preset`, `Compare with`, `Copy config`, and `ENGINE_CONFIG` round-trip accurately.
5. `C-NS-1` GPU gates (S3 denoiser `Off`, S4 `Accumulation`, S5 `Compare with`) pass on the RTX 5070, and `BUG-E5` from M8.1 (`glass.slang` `dlssInputScale` + `combine.slang` accumulation reset) is closed cleanly.

---

## 2. End-to-End Widget Inventory

### 2.1 `SettingsWindow` (`src/ui/settings_window.cpp`)

| Section / Node | Widget(s) | Host State | GPU Consumer | Status |
|---|---|---|---|---|
| **Top** | `Compare with` | `SettingsWindow::compareEntry`, `comparison`, `differing` | UI highlight + closed-node count (`settings_rows.hpp`) | **OK** (S5 test added) |
| **Scene** | `Transparent Geometry` | `renderer.glass.isPassEnabled` | `frameGlassFlags` → `glass.slang`, `rtxdi_shade`, `path_shading`, `rtxdi_gi_shade` | **OK** (resets DLSS history when `glassRowCount > 0`) |
| **Scene → Engine (Sun)** | `Enabled`, `Azimuth`, `Elevation`, `Illuminance`, `Color`, `Defaults` | `worldLights.sun` | `WorldLights::recordUpdate` → `light_world.slang` | **OK** (`isEnabled` resets light-set history; all fields wake NRC & reset Accumulation) |
| **Scene → Engine (Glass)** | `Max glass layers`, `Glass reflection`, `Glass diffuse share`, `Glass emission`, `Shadow transmittance`, `Blend DLSS guides with glass` | `renderer.glass.*` | `GpuFrameRoot.glassFlags` / `glassMaxLayers` → `glass.slang`, `rtxdi_shade`, `path_shading`, `rtxdi_gi_shade`, `Renderer::recordReconstruction` | **OK** (`BUG-E5` fix for `dlssInputScale` on glass overlay) |
| **General Rendering** | `Resolution Scale (%)`, `Pixel Jitter` | read-only `dlss` / `100`; `camera.isJitterEnabled` | `Camera::updateCameraData` → `jitterOffset` → raster, resolve, DLSS | **`BUG-UI-9` (jitter toggle misses DLSS/Accumulation reset when not via `Engine::run`)** |
| **General Rendering → Engine** | `Instances per partition`, `Force full PTLAS build` | `rtScene.settings.*` | `RayTracingScene::prepareFrame` (`buildTemplates`), `recordBuild` (`RtInstancesFlag::FullBuild`) | **OK** |
| **Static ReSTIR Context** | `Checkerboard`, `ReGIR Context` (read-only) | `rtxdi.getAppliedParameters()` | read-only mirror of applied RTXDI/ReGIR static params | **OK** |
| **Direct Lighting** | `Preset`, `Show Advanced Settings`, `Resampling Mode`, `ReGIR Presampling` (read-only) | `rtxdi.settings`, `isReference`, `isSpatialOnly`, `isTemporalOnly` | `RtxdiContext::configure()` → `GpuRtxdiConstants` → `rtxdi_{initial,temporal,shade}` | **`BUG-UI-3`** (Temporal/Spatial nodes stay enabled in Spatial-only / Temporal-only); **`BUG-UI-6`** (DI settings/preset edits do not reset DLSS/Accumulation history cleanly) |
| **Direct → Initial Sampling** | `Local Light Sampling` radios & per-mode sliders, `Initial BRDF Samples`, `Initial Infinite Light Samples`, `Enable Initial Visibility`, fixed `BRDF Cutoff` / `Min T` | `rtxdi.settings.*` | `RtxdiContext::configure()` → `constants.initialSampling` → `rtxdi_initial.slang`, `path_shading.slang` | **`BUG-UI-2`** (`Enable Initial Visibility` stays enabled in `Resampling Mode == None` despite `BUG-C1` forcing it `0`) |
| **Direct → Temporal / Spatial / Final** | `Fallback`, `Permutation`, `Temporal Bias Correction`, `Depth/Normal Threshold`, `Max History`, `Boiling Filter`, `Spatial Bias Correction`, `Spatial Samples`, `Disocclusion Boost Samples`, `Radius`, `Discard Invisible Samples`, `Reuse Final Visibility` | `rtxdi.settings.*` | `RtxdiContext::configure()` → `GpuRtxdiConstants` → `rtxdi_temporal.slang`, `rtxdi_shade.slang` | **OK** (once gated by **`BUG-UI-3`** & **`BUG-UI-6`**) |
| **Direct → Engine** | `Sample triangle lights`, `Exact directional lights`, `Boost history length`, `Material similarity`, `Per-pixel light tiles` | `worldLights.areTriangleLightsSampled`, `rtxdi.settings.*` | `WorldLights`, `RtxdiContext::configure()`, `DiFlag::*` → `rtxdi_{initial,shade}`, `path_shading` | **OK** (`areTriangleLightsSampled` resets history; `Defaults` button misses it — **`BUG-UI-5`**) |
| **Indirect Lighting** | `Indirect Lighting Mode`, `Min Secondary Roughness`, `Secondary Surface Light Sampling` (read-only), `Reuse Primary Samples`, `Final visibility`, `Final MIS` | `renderer.indirectMode`, `rtxdi.giSettings.*` | `Renderer::recordIndirect`, `RtxdiContext::configureGi`, `indirect.slang`, `rtxdi_gi_{temporal,shade}.slang` | **`BUG-UI-1`** (wrong read-only secondary light sample counts + typo `Inifinite`); **`BUG-UI-4`** (GI reuse rows stay enabled and `Resampling Mode` shows `Temporal + Spatial` in `IndirectMode::Nrc`) |
| **Indirect → NRC** | `Train The Cache`, `Resolve Mode`, read-only context/frame params | `nrc.settings.isTrainingEnabled`, `resolveMode` | `NrcCache::prepareFrame` / `frameSettings` / `recordUpdate` / `recordDebugResolve` | **OK** (disable node controls when `renderer.isNrcPaused()`) |
| **Indirect → Resampling** | `Resampling Mode`, `Temporal Resampling`, `Spatial Resampling` | `rtxdi.isGiReference`, `rtxdi.giSettings.*` | `RtxdiContext::configureGi` → `rtxdi_gi_{temporal,shade}.slang` | **`BUG-UI-4`** (not disabled in `IndirectMode::Nrc`; shows `Temporal + Spatial` when `giResamplingMode == None`) |
| **Indirect → Engine** | `ReSTIR PT DLSS-RR profile`, `Firefly clamp`, `Exact directional lights##gi`, `Ground-truth paths`, `Bounces after X2`, `NRC idle train frames`, `NRC training iterations`, `Defaults`/`Tuned`/`Converged` | `rtxdi.giSettings.*`, `rtxdi.giReferenceBounces`, `nrc.settings.*` | `RtxdiContext::prepareFrame`, `NrcCache::prepareFrame`, `indirect.slang`, `rtxdi_gi_shade.slang`, `rtxdi_gi_duplication.slang` | **`BUG-UI-7`** (changing `Bounces after X2` while `> 0` misses `IndirectState`, DLSS, and Accumulation reset); **`BUG-UI-8`** (`fireflyReplacement` rows disabled in `Uniform` decorrelation even though `rtxdi_gi_shade.slang:345` runs them in both `Uniform` and `Stagnancy`) |
| **Denoising** | `Denoiser mode`, `Auto-apply denoiser compatibility preset`, `DLSS-RR` (`Mode`, `Preset`, `Specular hit distance guide`, `Reset history`) | `renderer.denoiser`, `dlss.settings.*`, `dlss.guides.*`, `isCompatPresetAutoApplied` | `Renderer::drawFrame` (target recreate), `Dlss::recreate` / `evaluate`, `indirect.slang`, `rtxdi_gi_shade.slang`, `glass.slang` | **OK** (stale tooltip text on compat preset updated to match `defaultGiSettings()` = NVIDIA library defaults) |
| **Post-Processing** | `No AA` / `Accumulation`, `Accum. Frame Limit`, `Exposure bias`, `Engine` (`Auto exposure`, `Light/Dark adaptation`, `Exposure range`, `Snap to target`, `Defaults##post`) | `renderer.aaMode`, `accumulationFrameLimit`, `post.settings.*` | `Renderer::prepareAccumulation` / `recordAccumulation` (`accumulate.slang`), `PostProcess::record` (`exposure.slang`, `post.slang`) | **`BUG-UI-10`** (`Accum. Frame Limit` stays enabled when `Denoiser == DLSS-RR` or `aaMode == No AA`; dead `isReference != isReferenceEnabled()` check in `prepareAccumulation()`) |
| **Config Footer** | `Defaults`, `Copy config`, config string | all `LabConfig` fields + `rtScene.settings` | `applyLabConfig`, `captureLabConfig`, `formatLabConfig` | **`BUG-UI-5`** (`Defaults` button omits `camera.isJitterEnabled` and `worldLights.areTriangleLightsSampled`) |

### 2.2 `EngineUi` (`src/ui/engine_ui.cpp`)

| Panel | Widget(s) | Host State | GPU Consumer | Status |
|---|---|---|---|---|
| **Stats Overlay** | FPS, frame ms, active `DebugView`, unlit notice, `Accumulating N` | `renderer.debugView`, `isLitPresented()`, `isAccumulationActive()` | read-only | **OK** |
| **Load model / Loaded models / Assets / Instance field** | folder scan, model load/unload, spawn/clear field, material/light inspector | `UiRequests` → `Engine::applyUiRequests` | `AssetBuffers`, `FrameData`, `RayTracingScene`, `WorldLights` | **OK** |
| **Camera** | position readout, `Coordinates` + `Teleport` | `camera.teleportTo()` | sets `isCutPending = true` → resets history, NRC, Accumulation | **OK** |
| **Debug view** | `View` combo, `Triangle edges`, `Hi-Z Mip`, `Reflection term`, `ReSTIR DI reference`, `Entity` transform drag controls | `renderer.debugView`, `rtxdi.isReference`, `scene.objectStorage.transforms` | `debug_view.slang`, `rtxdi_*`, `FrameData`, `RayTracingScene::updateDynamicEntities` | **`BUG-UI-6`** (`ReSTIR DI reference` checkbox leaves `isSpatialOnly` / `isTemporalOnly` set); **`BUG-UI-11`** (stale `"no lighting before M3: the diffuse albedo guide"` text under `DebugViewCategory::Lit`) |
| **Culling** | 9 culling checkboxes, `Meshlets / sequence`, stats tables, GPU/async timings | `culling.settings.*` | `MeshletCulling::prepareFrame` / `writeCullView` → `GpuCullView`, `triangleCullFlags` | **OK** |

---

## 3. Verified Bugs & Fixes

| ID | Severity | File(s) | Root Cause | Fix |
|---|---|---|---|---|
| **BUG-UI-1** | P1 | `src/ui/settings_window.cpp:947-954` | `Secondary Surface Light Sampling` read-only rows display `rtxdi.settings.*` instead of `applied.initialSampling` and ignore `arePathInfiniteLightsExact()` (which forces `numInfiniteLightSamples = 0` in `path_shading.slang:71-74`), plus typo `"Indirect Inifinite Light Samples"`. | Read `applied.initialSampling.numLocalLightSamples` and `rtxdi.arePathInfiniteLightsExact() ? 0u : applied.initialSampling.numInfiniteLightSamples`; fix spelling. |
| **BUG-UI-2** | P1 | `src/ui/settings_window.cpp:726-730` | `Enable Initial Visibility` is placed after `ImGui::EndDisabled()` for `rtxdi.isReference`, even though `RtxdiContext::configure()` (`BUG-C1`) forces `enableInitialVisibility = 0` in `Resampling Mode == None`. | Move `Enable Initial Visibility` inside `ImGui::BeginDisabled(rtxdi.isReference)` and display `applied.initialSampling.enableInitialVisibility != 0` while disabled. |
| **BUG-UI-3** | P1 | `src/ui/settings_window.cpp:645-648` | `Temporal Resampling` and `Spatial Resampling` are both disabled only on `rtxdi.isReference`, leaving Temporal enabled in `Spatial`-only mode and Spatial enabled in `Temporal`-only mode. | Disable `drawDirectTemporal()` on `rtxdi.isReference || rtxdi.isSpatialOnly` and `drawDirectSpatial()` on `rtxdi.isReference || rtxdi.isTemporalOnly`. |
| **BUG-UI-4** | P1 | `src/ui/settings_window.cpp:936-1193` | In `IndirectMode::Nrc` (shipped default), `rtxdi.isGiReuseEnabled()` is `false` (`giResamplingMode == None`), yet `Resampling Mode` displays `Temporal + Spatial`, `Min Secondary Roughness` displays `0.5` enabled, and `Temporal`/`Spatial`/`Final visibility`/`Final MIS`/`ReSTIR PT DLSS-RR profile` stay enabled despite being skipped on GPU. | Gate all GI-reuse-dependent controls on `isGiReuse = (indirectMode != IndirectMode::Nrc && !rtxdi.isGiReference)`: show `Resampling Mode` as `None` (disabled in `IndirectMode::Nrc`), show `Min Secondary Roughness` as `0.0` disabled when `!isGiReuse`, and disable `Temporal`, `Spatial`, `Final visibility`, `Final MIS`, and `ReSTIR PT DLSS-RR profile` when `!isGiReuse`; disable `NRC:` edit controls when `renderer.isNrcPaused()`. |
| **BUG-UI-5** | P1 | `src/ui/settings_window.cpp:1406-1428` | `Settings -> Defaults` manually resets fields but omits `camera.isJitterEnabled = true` (leaving `frame.jitter=off` stuck in the config string and highlighted orange) and `worldLights.areTriangleLightsSampled = true`. | Call `applyLabConfig(LabConfig{}, renderer, camera)` plus `renderer.getRayTracingScene().settings = {}` and `renderer.getWorldLights().areTriangleLightsSampled = true`. |
| **BUG-UI-6** | P1 | `src/ui/engine_ui.cpp:685-688`, `src/render/vk_renderer.{hpp,cpp}`, `src/render/vk_rtxdi.hpp` | (1) `EngineUi`'s `ReSTIR DI reference` checkbox sets `isReference = true` without clearing `isSpatialOnly` / `isTemporalOnly`, creating contradictory `LabConfig` states. (2) `Renderer::drawFrame()` never resets `isDlssHistoryValid` when `rtxdi.settings` changes (e.g. DI `Preset` switch or local light sampling / spatial / visibility edits), unlike `giSettings`. | (1) Clear `isSpatialOnly = false; isTemporalOnly = false;` when `isReference` is toggled on in `engine_ui.cpp`. (2) Expose `RtxdiContext::isSettingsDirty()` (before `prepareFrame`), reset `isDlssHistoryValid = false` on DI settings changes, and pass `isDiChanged` into `prepareAccumulation()`. |
| **BUG-UI-7** | P1 | `src/render/vk_renderer.hpp:231-240`, `src/render/vk_renderer.cpp:225-241` | `IndirectState` stores `bool isGroundTruthPaths = rtxdi.giReferenceBounces != 0` instead of `uint32_t giReferenceBounces`. Dragging `Bounces after X2` (e.g. `8 → 1`) fails to reset `isIndirectHistoryReset`, `isDlssHistoryValid`, or `Accumulation`. | Store `uint32_t giReferenceBounces = 0;` in `IndirectState`. |
| **BUG-UI-8** | P2 | `src/ui/settings_window.cpp:1134-1182` | `Decorrelation firefly replacement`, `strength`, `full-tile`, `bias reduction`, and `multiplier bound` are disabled under `!isStagnancy`, even though `rtxdi_gi_shade.slang:342-389` executes them for both `Uniform` and `Stagnancy` whenever `isDecorrelated` (`decorrelationMode != None && decorrelationFactor > 0`). Also stale tooltip on `Auto-apply denoiser compatibility preset`. | Close `!isStagnancy` after `Decorrelation EMA factor` and gate the firefly rows on `!isDecorrelated || gi.decorrelationFactor <= 0.0f`; update compat preset help text to accurately describe `defaultGiSettings()`. |
| **BUG-UI-9** | P2 | `src/render/vk_renderer.{hpp,cpp}` | Toggling `camera.isJitterEnabled` (`Pixel Jitter`) does not reset `isDlssHistoryValid` or `Accumulation` outside `Engine::run()`'s lab-switch diff. | Track `appliedJitterEnabled` in `Renderer::drawFrame()` and reset `isDlssHistoryValid` + `Accumulation` on change. |
| **BUG-UI-10** | P2 | `src/ui/settings_window.cpp:1340-1350`, `src/render/vk_renderer.cpp:1309-1317` | (1) `Accum. Frame Limit` remains enabled when `Denoiser == DLSS-RR` or `aaMode == No AA`. (2) `prepareAccumulation()` checks `rtxdi.isReference != rtxdi.isReferenceEnabled()` *after* `rtxdi.prepareFrame()` already updated `isReferenceApplied` (dead code), and does not reset on `isDiModeChanged`, `isDiSettingsChanged`, `isGlassChanged`, `isNrcResolveModeChanged`, or lowering `accumulationFrameLimit < accumulatedFrames`. | (1) Disable `Accum. Frame Limit` unless `denoiser == DenoiserMode::Off && aaMode == AaMode::Accumulation`. (2) Pass `isLightingStateChanged` (captured before `rtxdi.prepareFrame`) into `prepareAccumulation()` and reset if `accumulationFrameLimit > 0 && accumulationFrameLimit < lastAccumulationFrameLimit`. |
| **BUG-UI-11** | P2 | `src/ui/engine_ui.cpp:647-649` | `Debug view` panel still displays M1 placeholder `"no lighting before M3: the diffuse albedo guide"` for `DebugViewCategory::Lit`. | Update text to `"tone-mapped Lit output (DLSS-RR, or raw/Accumulation with denoiser Off)"`. |
| **BUG-E5** | P2 | `shaders/passes/glass.slang:225-226`, `src/render/vk_renderer.cpp:216-219` | From M8.1 §4.5: `glass.slang` added unscaled `overlay` after `combine.slang` scaled by `root.exposure` (`dlssInputScale`), and `dlssInput != appliedDlssInput` did not clear `dlssInputAccumulation`. | Scale `overlay` by `max(root.exposure, 1e-6)` when `root.exposure != 1.0` in `glass.slang`; request `targets.requestDlssInputAccumulationClear()` when `dlssInput != appliedDlssInput` in `vk_renderer.cpp`. |

---

## 4. `C-NS-1` & M8.2 Verification Plan

1. **Unit tests (`tests/lab_registry_test.cpp`, `ctest` on both presets):**
   - Existing round-trip, preset, row-table, and plan checks (`8/8` checks, `7/7` release).
   - Add S5 `Compare with` test: verify `Shipped` highlights 0 rows; `NVIDIA Medium` highlights exact `{frame.indirectMode, frame.dlssQuality}`; closed-section distinct counts match `settingsRows`.
2. **S3 · Denoiser `Off` GPU & Validation Gate:**
   - `ENGINE_BENCH_FRAMES=1500 ENGINE_CONFIG="frame.denoiser=off"` on Sponza gallery & courtyard (no layers): record native `1280x720` render extent, `DLSS-RR = 0.000 ms`, GPU total, and CPU frame time.
   - `VK_LAYER_KHRONOS_validation` (`VALIDATE_SYNC=true`, `VALIDATE_BEST_PRACTICES=true`, `VALIDATE_BEST_PRACTICES_NVIDIA=true`, `GPUAV=false`) on `frame.denoiser=off` and `frame.denoiser=off frame.aa=accumulation frame.accumulationFrames=16`.
3. **S4 · `Accumulation` Convergence Gate:**
   - Run Sponza courtyard in `ReSTIR GI` mode with `frame.denoiser=off frame.aa=accumulation` (`ENGINE_LAB` stability / 512→1,024 mean convergence): verify `< 0.1%` mean change and zero non-finite/negative pixels.
4. **Regression:**
   - `ENGINE_DLSS_CHECK=1`, `ENGINE_GUIDE_CAPTURE=1 ENGINE_RT_CHECK=1`, and validation pass clean after all edits.

---

## 5. Verification Results (2026-11-26, RTX 5070)

- **Builds & CTest:** `windows-clang-release` (`7/7` passed) and `windows-clang-release-checks` (`8/8` passed, including S5 `Compare with` verification in `tests/lab_registry_test.cpp`: `Shipped` highlights `0`, `NVIDIA Medium` highlights exact `{frame.indirectMode, frame.dlssQuality}`, closed-section deduplicated counts `Indirect=1, Denoising=1, Direct=0`, shared advanced threshold deduplicated).
- **S3 · Denoiser `Off` clean bench (`ENGINE_BENCH_FRAMES=1500`, `ENGINE_CONFIG="frame.denoiser=off"`, 1280×720 native, no layers):**
  - Sponza gallery: `DLSS-RR = 0.0003 ms`, `Accumulation = 0.0003 ms`, `GPU total = 4.1096 ms`, `CPU drawFrame = 4.0002 ms`.
  - Sponza courtyard: `DLSS-RR = 0.0003 ms`, `Accumulation = 0.0003 ms`, `GPU total = 5.1485 ms`, `CPU drawFrame = 5.0899 ms`.
- **S4 · `Accumulation` convergence (`ENGINE_LAB` `sponza.courtyard`, `Lit`, `ReSTIR GI` mode, 300 warm-up + 480 captured frames, live transitions `DLSS-RR → Off (No AA) → Off (Accumulation)`):**
  - `DLSS-RR` baseline: `mean 0.1729`, `std 1.322%`, `flicker 5.882%`, `f2f 0.136%`, `swing 0.1434%`.
  - `Denoiser Off + No AA`: `mean 0.1971`, `std 33.52%`, `flicker 87.21%`, `f2f 26.22%`, `swing 0.3604%`.
  - `Denoiser Off + Accumulation`: `mean 0.1795`, `std 0.8974%`, `flicker 6.458%`, **`f2f 0.0499%`**, **`swing 0.1415%`** (`0 failed`, `0 nonFinite`).
- **S3/S4 Vulkan Validation (`VK_LAYER_KHRONOS_validation`, `VALIDATE_SYNC=true`, `VALIDATE_BEST_PRACTICES=true`, `VALIDATE_BEST_PRACTICES_NVIDIA=true`, `GPUAV=false`, `ENGINE_BENCH_FRAMES=24`):**
  - `frame.denoiser=off`: exit `0`, zero engine errors/warnings (only `C-H1-2` NGX `WorkgroupSize` at feature creation and `C-G3-3` NRC shutdown leaks).
  - `frame.denoiser=off frame.aa=accumulation frame.accumulationFrames=16`: exit `0`, zero engine errors/warnings.
- **Regression (`ENGINE_DLSS_CHECK=1`, Sponza):** exit `0`, **11 steps, 0 failed** (`static shipped` `std 0.335%`, `flicker 0.024%`; `jitter inverted` `std 3.690%`).
- **`C-NS-1` closed** in [considerations.md](../considerations.md).

## 6. Done Criteria for M8.2

- [x] Every `SettingsWindow` and `EngineUi` control audited end-to-end to GPU consumers.
- [x] All verified UI/settings/history-reset bugs (`BUG-UI-1`–`BUG-UI-11`) and `BUG-E5` fixed.
- [x] `C-NS-1` GPU gates (S3 denoiser `Off`, S4 `Accumulation`, S5 `Compare with`) verified clean on RTX 5070.
