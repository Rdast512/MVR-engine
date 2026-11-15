# Feature: RTXDI 3.2 update

**Date:** 2026-11-15 · **Status:** built and checked (2026-11-15, RTX 5070) · **Milestone:** M4 (current), a dependency update of M3's ReSTIR DI
**Implements:** a new pin of `NVIDIA-RTX/RTXDI-Library` (`f12037fa` 3.1 → `b67f61cd` 3.2, upstream commit "3.2 Update", 2026-11-09); changes PBR plan F1 and F3 ([pbr_implementation_plan.md](../pbr_implementation_plan.md))
**Touches:** considerations C-F2-2, C-F3-1, C-F3-2, C-F3-3, C-F3-8, C-M4-1, C-M4-2; adds C-R32-1, C-R32-2 ([considerations.md](../considerations.md))

RTXDI 3.2 removes the fused spatiotemporal DI path that the engine's `rtxdi_shade` pass is built on. The update therefore splits that pass in two, the library's `TemporalAndSpatial` mode. The workaround and the measurements that depended on the fused code (C-F3-2, C-F3-3) are redone against the separate passes. That re-measurement needs a convergence check, which F3 only had as temporary code (C-F3-8), so this update makes it permanent.

---

## 1. What 3.2 changes (verified 2026-11-15 against the two commits)

One upstream commit lies between the pins: 12 files, +370 −707 lines.

| Change | Source | Consequence for the engine |
|---|---|---|
| `DI/SpatioTemporalResampling.hlsli` deleted, with `RTXDI_DISpatioTemporalResampling`, `RTXDI_DISpatioTemporalResamplingParameters`, `ReSTIRDI_SpatioTemporalBiasCorrectionMode`, `ResamplingMode::FusedSpatiotemporal`, `Get/SetSpatioTemporalResamplingParameters` and `GetDefaultReSTIRDISpatioTemporalResamplingParams` | `CMakeLists.txt`, `DI/ReSTIRDI.h`, `DI/ReSTIRDIParameters.h`, `Source/ReSTIRDI.cpp` | the engine no longer builds; `rtxdi_shade`, `GpuRtxdiConstants`, `RtxdiContext` and the UI move to separate temporal and spatial resampling (§3) |
| `RTXDI_DITemporalResamplingParameters` gains `enableFallbackSampling` + 3 pads (+16 B). With it, a failed surface match tries one zero-motion sample at the same pixel, permuted, without the similarity tests; its age is reset so visibility is never reused. Default on | `DI/TemporalResampling.hlsli`, `Source/ReSTIRDI.cpp` | aimed at disocclusions at screen edges: C-M4-1 |
| The temporal visibility shortcut now requires `age == 0` and `spatialDistance == 0` on the reused reservoir | `DI/TemporalResampling.hlsli` | the shortcut (off by default) no longer skips the ray for samples that were not tested where they came from |
| `RTXDI_DISpatialResamplingParameters.pad1` becomes `neighborOffsetSamplingMode` (`LowDiscrepancy` 0, `StratifiedLowDiscrepancy` 1, size unchanged). Stratified walks groups of four offsets, one per quadrant, and needs a matching neighbor-offset table and even sample counts | `DI/ReSTIRDIParameters.h`, `Utils/SpatialNeighborSampling.hlsli` (new), `RtxdiUtils.h` | `FillNeighborOffsetBuffer` gains an overload with the mode; the engine's two-argument call fills the legacy layout and must change (§3.4) |
| Spatial defaults: `numSamples` 1 → 2, `discountNaiveSamples` true → false, sampling mode `StratifiedLowDiscrepancy` | `Source/ReSTIRDI.cpp` | the naive-sample discount behind C-F3-2's darkening is off by default |
| `RTXDI_TargetPdfHelper` returns 0 for an invalid reservoir instead of loading its light | `DI/PairwiseStreaming.hlsli` | the pairwise path no longer loads light 0 for an empty result (C-F3-1) |
| `SetFrameIndex` rehashes `temporalResamplingParams.uniformRandomNumber`, and so does `SetTemporalResamplingParameters` | `Source/ReSTIRDI.cpp` (unchanged logic, now the only consumer) | C-F3-3's second half (a setter call per frame) disappears |
| `RTXDI_DI_NEIGHBOR_OFFSETS_BUFFER` may replace `RTXDI_NEIGHBOR_OFFSETS_BUFFER` (the old name still works) | `DI/SpatialResampling.hlsli` | none |
| `RTXDI_RuntimeParameters.pad2` renamed `pad1` | `RtxdiParameters.h` | none (padding) |
| Unchanged: `InitialSampling.hlsli`, `BoilingFilter.hlsli`, `ReservoirStorage.hlsli`, `Reservoir.hlsli`, the RAB set they need | `git diff --stat` | `rtxdi_initial` and the bridge keep working; a scratch unit with the bridge, `TemporalResampling`, `SpatialResampling` and `BoilingFilter` compiles with the 3.2 headers |

**Still in 3.2:**
- **E30081.** Compiled with Slang 2026.20 and the warning enabled, the 3.2 temporal, spatial and boiling-filter headers emit 8 implicit `uint`→`bool` warnings (`TemporalResampling.hlsli` 59, 77, 100; `SpatialResampling.hlsli` 121, 131, 240, 258; `Utils/Math.hlsli` 41). `RTXDI_SLANG_FLAGS` stays (C-F2-2 open).
- **3.1's fused sample count included the temporal reservoir.** `RTXDI_DISpatioTemporalResampling` used neighbor slot 0 for the temporal sample whenever a temporal surface matched, so `numSamples` 1 (the default the engine kept) took no spatial neighbors except the disocclusion boost. 3.2's separate spatial pass at its default of 2 adds two neighbors every frame. That is new behavior for the engine, not a like-for-like port (§5).
- **`RTXDI_ALLOWED_BIAS_CORRECTION`** is defaulted only in `SpatialResampling.hlsli`. A unit that includes `TemporalResampling.hlsli` first evaluates its `#if` with the macro undefined (0, Slang warning E15205) and compiles the temporal bias correction out. The fused header used to hide this. The temporal pass defines the macro itself (§3.1).

---

## 2. Effect on the open considerations

| ID | Before | After this update |
|---|---|---|
| C-F3-2 · Pairwise instead of `Basic` | fused `Basic` 35% too dark | resolved: the fused code is gone. Spatial keeps Pairwise and temporal takes `Basic`, both on measurement (§6.1) |
| C-F3-3 · permutation sampling off | also needed a setter per frame | the setter is no longer needed. Permutation stays off: on the lamp it is far worse with 3.2's passes (§6.1). The A/B through DLSS-RR remains |
| C-F3-1 · bounds-checked bridge reads | fused path read off-screen reservoirs and light 0 | the reads that faulted are gone or guarded upstream; the bridge keeps its checks (cheap, and other RTXDI paths assume robust buffers). The comments name 3.2's paths |
| C-F3-8 · no permanent convergence check | temporary instrumentation in F3 | resolved by §4; it caught the spatial-reuse regression of §6.1 |
| C-F2-2 · E30081 suppressed per unit | | unchanged (§1) |
| C-M4-1 · lighting fades in at the screen edges | | fallback sampling is on (library default). Unmeasured until C-M4-1's scripted pan |
| C-M4-2 · untuned defaults | | the defaults change (§3.3); the tuning itself stays with C-M4-2 |

---

## 3. Design

### 3.1 Passes

Resampling mode `TemporalAndSpatial` (the library default), reference `None` as before. Three compute passes, 8×8 tiles, in `Renderer::recordDirectLight`, each behind a memory barrier:

| Pass | Reads | Writes | Notes |
|---|---|---|---|
| `rtxdi_initial` (unchanged) | guides, lights, TLAS | `initialSamplingOutputBufferIndex` | |
| `rtxdi_temporal` (new) | its own initial reservoir; `temporalResamplingInputBufferIndex` (last frame's final reservoirs); previous guides; `Motion.xyz` | `temporalResamplingOutputBufferIndex` | `RTXDI_DITemporalResampling`, then the boiling filter (group-synchronized: no early return before it). Uniform early-out without temporal resampling or without history |
| `rtxdi_shade` (spatial + shading) | `spatialResamplingInputBufferIndex` at its own pixel and at neighbors | `shadingInputBufferIndex`, `directRadiance` | `RTXDI_DISpatialResampling` when spatial resampling is on, then the final visibility ray and `shadeLightSample · W`, as before |

- **In-place temporal output.** With three buffers, `UpdateBufferIndices` gives `temporalResamplingOutputBufferIndex == initialSamplingOutputBufferIndex` in every mode. The temporal pass therefore writes over its own pixel's initial reservoir, and its early-out leaves the initial reservoir as the temporal output. `RtxdiContext::prepareFrame` asserts this, and that the spatial input and output differ (debug builds).
- **Spatial and shading fused.** The spatial pass reads `spatialResamplingInputBufferIndex` (the temporal output) and writes `spatialResamplingOutputBufferIndex`, which is also `shadingInputBufferIndex`. The two differ, so neighbors never see a pixel's new reservoir. Shading right after the spatial step saves a dispatch and a reservoir round trip. In mode `None`, `spatialResamplingInputBufferIndex` is the initial output, so the same load serves the reference.
- **History reset.** Only the temporal pass needs history; the spatial pass reuses this frame's neighbors and runs after a cut too. That differs from the fused pass, which shaded the initial reservoir on a reset frame. On reset frames every reservoir has M = 1, below `targetHistoryLength`, so the disocclusion boost applies.
- **No lights:** the host skips `rtxdi_initial` and `rtxdi_temporal`; `rtxdi_shade` writes zeros, as before.
- **Mode flags.** `GpuRtxdiConstants.isFusedResampling` becomes `resamplingMode`, the `rtxdi::ReSTIRDI_ResamplingMode` value. Its values are bit flags (Temporal 1, Spatial 2, TemporalAndSpatial 3); `vk_rtxdi.hpp` asserts that, and the shaders test bit 0 and bit 1.
- **`RTXDI_ALLOWED_BIAS_CORRECTION`** is defined to `RTXDI_BIAS_CORRECTION_RAY_TRACED` in `rtxdi_temporal` before the RTXDI headers (§1).
- **GPU timer:** a `ReSTIR DI temporal` span (`GpuTimerScope::RtxdiTemporal`) between initial and shade.

### 3.2 Constants

`GpuRtxdiConstants` drops the 48 B spatiotemporal block and its temporal block grows by 16 B: 360 → 328 B, `reservoirs` @ 304. The Slang mirror (`lighting/rtxdi_constants.slang`) and `layout_test` follow.

### 3.3 Settings

`RtxdiContext::Settings` keeps one set of thresholds for both passes (the library defaults are equal) and splits the rest:

| Setting | Goes to | Default |
|---|---|---|
| local / infinite candidates, initial visibility | initial sampling | library (8 / 1 / on) |
| `maxHistoryLength` | temporal; also spatial `targetHistoryLength` | 20 |
| temporal bias correction (`Off`, `Basic`, `Raytraced`) | temporal | `Basic` (library; §6.1) |
| visibility shortcut, fallback sampling (new) | temporal | off / on |
| spatial samples, disocclusion boost samples, sampling radius | spatial | **0** / 8 / 32: neighbors only while history is short, as the fused pass did (§1, §6.1; C-R32-1) |
| spatial bias correction (`Off`, `Basic`, `Pairwise`, `Raytraced`) | spatial | **Pairwise** (§6.1) |
| material similarity | spatial | on |
| depth / normal thresholds | both | 0.1 / 0.5 |
| boiling filter, strength | temporal pass | on / 0.2 |

- **`targetHistoryLength` = `maxHistoryLength`.** The library defaults spatial `targetHistoryLength` to 0, which never triggers the disocclusion boost. The fused pass boosted below `maxHistoryLength`, so the engine keeps that.
- **Permutation sampling** stays off (C-F3-3).
- **Even spatial counts.** Stratified sampling wants even sample counts, so the UI rounds the spatial and boost counts down to even.

### 3.4 Neighbor offsets

The stratified table: `FillNeighborOffsetBuffer(buffer, 8192, StratifiedLowDiscrepancy)`, with the mode in one constant that also sets `neighborOffsetSamplingMode`. Converting from RG8_SNORM to `float2` stays as it is.

---

## 4. Convergence check (closes C-F3-8)

The guide capture gains one check after its shots: `LightsPunctualLamp` (5 point lights, static camera, jitter off).

- **Reference:** the shot's `Shadowed lights` capture, the exact punctual sum with one shadow ray per light, kept as linear luminance.
- **Tested:** `Lighting only` (ReSTIR DI `directRadiance` + the same emissive), 16 warm-up frames, then 64 captured frames.
- **Metrics,** as in F3: luminance differences as a share of the reference's summed luminance:
  - bias of the 64-frame mean;
  - 16×16 tiles with any reference signal: median, p90 and max of the tile's |mean − reference| share;
  - per-frame error, and frame-to-frame change.
- **Pass:** |bias| ≤ 0.5% and tile p90 ≤ 3%. Under this check (853×480 render size, DLSS-RR Quality) 3.1's fused pass reads +0.129% and 2.43%. F3's temporary capture at 1280×720 read −0.001% and 1.72%, its darkening bug −35%, and `Basic` without the discount −3.1%.
- **Determinism:** the frame indices and the scene are fixed, so repeated runs give identical numbers; a changed number is a real change.
- **Log:** `[GuideCapture]` lines, closing with `Convergence check: passed` or `FAILED`.

---

## 5. Verification

On the RTX 5070, release build:

1. The convergence check on the 3.1 fused path, before the port: the baseline.
2. After the port, the check with spatial `Basic` and `Pairwise`, and temporal `Basic` and `Raytraced`. The defaults become the variant that passes with the lowest bias and frame-to-frame change.
3. Validation (core, sync, best practices + NVIDIA) on the guide capture with `ENGINE_RT_CHECK`, the reference configuration, the motion test (resize) and the cull check. The guide capture's light-less shots cover the no-light path. GPU-AV once on the guide capture.
4. Regressions: motion test 13 of 13, cull check 16 of 16, ray check every frame, guide normal checks 2 of 2, ctest 7 of 7.
5. Cost: the default bench (sunlit Sponza, 6000 frames, layers off), before and after, two rounds.

---

## 6. Results (2026-11-15, RTX 5070, release)

### 6.1 Convergence on `LightsPunctualLamp`

Every row is one deterministic guide-capture run (§4). Shares of the reference's summed luminance.

| Configuration | Bias | Tiles median / p90 / max | Per-frame error | Frame-to-frame |
|---|---|---|---|---|
| 3.1 fused, Pairwise (before) | +0.129% | 0.55% / 2.43% / 6.21% | 4.20% | 0.76% |
| reference: `None`, 32 + 8 candidates (3.1 and 3.2 identical) | −0.005% | 0.22% / 0.79% / 5.95% | 10.78% | 11.47% |
| 3.2 library defaults: temporal `Basic`, spatial `Basic`, 2 samples | −1.279% | 12.99% / 49.60% / 124.45% | 48.54% | 20.54% |
| 3.2, spatial Pairwise, 2 samples | −2.178% | 2.17% / 21.71% / 65.09% | 9.00% | 3.85% |
| 3.2, spatial Pairwise, 1 sample | −1.375% | 1.54% / 11.43% / 40.18% | 6.50% | 2.72% |
| 3.2, spatial `Basic`, 0 samples (boost only) | −0.575% | 2.05% / 6.28% / 26.18% | 6.90% | 1.53% |
| **3.2, spatial Pairwise, 0 samples (boost only): the new default** | **−0.134%** | **0.79% / 2.87% / 9.52%** | **4.75%** | **1.50%** |
| same, temporal `Raytraced` | −0.286% | 0.82% / 3.13% / 9.54% | 4.65% | 1.50% |
| temporal only (no spatial pass work) | +0.070% | 0.50% / 2.03% / 4.46% | 4.29% | 1.49% |
| initial only (no reuse, 8 + 1 candidates) | −0.052% | 0.56% / 2.27% / 9.01% | 31.35% | 33.00% |
| spatial only, `Basic`, 2 samples | −0.331% | 1.56% / 7.79% / 22.01% | 36.39% | 48.33% |
| spatial only, Pairwise, 2 samples | −0.638% | 1.06% / 4.37% / 22.67% | 26.07% | 31.51% |
| 3.2 defaults with permutation sampling on | −1.344% | 7.85% / 23.63% / 49.99% | 53.95% | 66.91% |
| temporal only with permutation sampling on | −1.944% | 7.03% / 21.69% / 45.92% | 43.37% | 53.71% |

- **The library defaults fail the check.** With spatial reuse every frame, the spatial output becomes the next frame's temporal input, and neighbor samples spread and persist as blotches (tile p90 up to 50%). Alone, spatial reuse stays nearly unbiased: Pairwise lowers the per-frame error against initial-only (26% against 31%), `Basic` raises it. On this scene (a thin glossy stand, 5 lights inside a shade) neighbors' samples transfer badly.
- **Ruled out while isolating it:** the neighbor table (`LowDiscrepancy` reads the same as stratified), visibility (spatial `Raytraced` reads the same), fallback sampling, the boiling filter and initial visibility (no effect on a static camera), and the material similarity test. Discarding occluded samples in the shade pass made the bias worse (−5.6%) and was dropped.
- **The new default matches 3.1's behavior:** neighbors only while a reservoir's history is shorter than `maxHistoryLength`, as the fused pass did (§1). Its bias and tile errors are within the check; frame-to-frame change doubles because 3.2's temporal reuse has no Pairwise MIS (C-R32-2).
- **Temporal `Basic`** stays: `Raytraced` costs a ray per pixel and measures no better.

### 6.2 Validation and regressions

- Validation (core, sync, best practices + NVIDIA, no GPU-AV) on the guide capture with `ENGINE_RT_CHECK`, the guide capture in the reference configuration, the motion test and the cull check. The engine produced no messages. NGX's known messages remain: `BestPractices-SpirvDeprecated_WorkgroupSize` three times at pipeline creation, and `SYNC-HAZARD-WRITE-AFTER-WRITE` on `nv.ngx.dlssd.resource` in its first evaluations (C-H1-2).
- GPU-AV on the guide capture: no messages beyond its own setup notices and NGX's warning; the convergence check reads the same.
- Regressions: motion test 13 of 13, cull check 16 of 16, ray check 234 of 234 frames, guide normal checks 2 of 2, convergence check passed (default and reference), ctest 7 of 7 (layout test with the 328 B `GpuRtxdiConstants`).
- Debug build, sunlit Sponza in both resampling modes: the buffer-index asserts of `prepareFrame` hold.

### 6.3 Cost (sunlit Sponza bench, 6000 frames, 853×480 render size, layers off)

| Span | 3.1 fused (before) | 3.2, round 1 | 3.2, round 2 |
|---|---|---|---|
| ReSTIR DI initial | 0.1356 ms | 0.1504 ms | 0.1482 ms |
| ReSTIR DI temporal | — | 0.0703 ms | 0.0705 ms |
| ReSTIR DI shade (3.2: spatial + shade) | 0.0975 ms | 0.0361 ms | 0.0360 ms |
| ReSTIR DI total | 0.233 ms | 0.257 ms | 0.255 ms |
| DLSS-RR | 1.1430 ms | 1.2120 ms | 1.2140 ms |
| GPU total | 1.624 ms | 1.726 ms | 1.719 ms |

The before run came about 40 minutes earlier in the same session; the old binary could not be re-run alternately. The unchanged initial pass and DLSS-RR read 6–10% higher in the after rounds, so most of the +0.02 ms is drift. The temporal and spatial passes together (0.106 ms) against the fused pass (0.098 ms) are within the ±5% bench noise plus that drift.
