# M8 · I6: ReSTIR DI tuning

**Status:** built 2026-11-21 · **Milestone:** M8 ([README](./README.md))
**Implements:** findings F5, F6, O8, O10, permutation sampling ([m8_optimization_and_fixes.md](./m8_optimization_and_fixes.md) §2.4, §2.7, §4), F3 DI screen-edge behavior (§4), F12 default presampling mode ([i4_frame_profile.md](./i4_frame_profile.md))
**Depends on:** I1 ([i1_check_runners.md](./i1_check_runners.md)), I2 ([i2_shading_correctness.md](./i2_shading_correctness.md)), I4 ([i4_frame_profile.md](./i4_frame_profile.md))
**References:**
- `ThirdParty/_deps/src/rtxdi/Include/Rtxdi/DI/{InitialSampling,TemporalResampling,SpatialResampling,ReSTIRDIParameters}.hlsli`, `Source/ReSTIRDI.cpp`
- `reference/RTXDI/Samples/FullSample/Source/UserInterface.cpp` (`Fast`, `Medium`, `Unbiased`, `Ultra` presets), `Doc/NoiseAndBias.md`
- `docs/settings.md` §3.4 (pre-M4 DI stability table on sunlit Sponza courtyard)

## Scope

I6 ranked **#1** in I4's frame profile: Bistro `ReGIR_RIS` adds **+0.420 ms (+13.8%)** over `Power_RIS` (`light prep` 0.477 vs 0.058 ms), and ReSTIR DI on sunlit Sponza courtyard costs **0.704 ms** (`initial` 0.241 / `temporal` 0.141 / `shade` 0.322 ms; gallery 0.351 ms). I6 evaluates every DI ray, decides the default presampling mode (F12), lands O8, evaluates O10 (`isFinalVisibilityReused`) and permutation sampling together under post-I5 GI + M6 OMM, compares NVIDIA `Medium` vs `convergedSettings` vs tuned defaults through DLSS-RR (F6), and checks DI screen-edge controls (F3).

| Item | Lever / Question | Verification |
|---|---|---|
| O8 | Skip the BRDF-candidate ray and BRDF sampling in `rtxdi_initial.slang` (with a defensive guard in `rtxdi_bridge.slang` `RAB_TraceRayForLocalLight`) when `triangleLights.lightCount == 0 && environmentLightParams.lightPresent == 0` | ABBA bench on Sponza gallery & courtyard; verify emissive scenes (`EmissiveStrengthTest`, `LightsPunctualLamp`, Bistro) still trace BRDF rays |
| F12 | Decide default `localLightSamplingMode`: `Power_RIS` vs `ReGIR_RIS` from I4's −0.42 ms Bistro ABBA plus convergence (`LightsPunctualLamp`, `EmissiveStrengthTest`) and Bistro stability | I4 ABBA + `ENGINE_GUIDE_CAPTURE` + Bistro stability check |
| F5 · DI ray budget | Measure cost, DI convergence (`LightsPunctualLamp`, `Sponza_terminator`, `EmissiveStrengthTest`), and `Lit` stability for every DI ray: initial visibility, temporal correction (`Raytraced` + shortcut vs `Basic`), spatial correction (`Raytraced` vs `Pairwise` vs `Basic` vs `spatialSamples = 0`), final visibility reuse (O10) | Bench on Sponza courtyard, gallery, and Bistro (`Power_RIS`); `ENGINE_GUIDE_CAPTURE` (shipped DI); `ENGINE_STABILITY_CHECK` |
| O10 & Permutation | Evaluate the 2×2 matrix (`isPermutationSamplingEnabled` × `isFinalVisibilityReused`) with `Raytraced` and `Pairwise` spatial correction on post-I5 GI + M6 OMM (re-testing `docs/settings.md` §3.4) | `ENGINE_STABILITY_CHECK` (`Lit` + `Direct lighting`), `ENGINE_GUIDE_CAPTURE` (`Sponza_terminator` shadow-edge leak), ABBA bench |
| F6 | Compare NVIDIA `Medium`, `convergedSettings`, current shipped, and tuned shipped through DLSS-RR on sunlit Sponza courtyard and gallery | `ENGINE_STABILITY_CHECK`, ABBA bench |
| F3 · DI screen edges | Test `disocclusionHistoryLength` (`0`, `4`, `20`), `disocclusionBoostSamples` (`0`, `4`, `8`), and `isFallbackSamplingEnabled` (`1`, `0`) on revealed bands and disocclusions | `ENGINE_SCREEN_EDGE_CHECK` (`Lit` and `Direct lighting`) |

## Decisions

| Decision | Rule | Why |
|---|---|---|
| NVIDIA `Medium` untouched | `RtxdiContext::nvidiaSettings()` stays callable and unchanged (`ReGIR_RIS`, `Basic` spatial, `permutation = true`, `reuseFinalVisibility = true`) | M8 rule (`C-M4-2`) |
| No vendor header edits | All changes stay in engine shaders and host code (`shaders/`, `src/`) | Vendor boundary (`ThirdParty/`) |
| Convergence check gate preserved | `EngineGuideCapture` keeps gating `convergedSettings()` (`3 of 3` pass); tuned shipped DI is also measured against the same 3 convergence shots | Matches `rtxdi_3_2_update.md` §6.1 while quantifying shipped DI bias |
| Benchmark hygiene | Every landed perf lever is ABBA-measured on release with no layers (`VK_LOADER_LAYERS_DISABLE=all`, `VK_KHRONOS_VALIDATION_GPUAV_ENABLE=false`) on Sponza gallery, courtyard, and Bistro | Eliminates drift and covers both 1-light and 76K-light scenes |

## Results

RTX 5070, release, no layers (`VK_LOADER_LAYERS_DISABLE=all`, `VK_KHRONOS_VALIDATION_GPUAV_ENABLE=false`), 1280×720 window (853×480 render, DLSS-RR Quality), post-I5 ReSTIR GI + M6 OMM, 2026-11-21.

### 1. O8: Skip BRDF candidate ray without emissives or environment light

- **Code:** `shaders/passes/rtxdi_initial.slang` zeroes `sampling.numBrdfSamples` when `gRtxdiBridge.triangleLights.lightCount == 0 && constants.lightBuffer.environmentLightParams.lightPresent == 0`; `shaders/lighting/rtxdi_bridge.slang` (`RAB_TraceRayForLocalLight`) adds the matching defensive early return. Scenes with emissive triangles (`EmissiveStrengthTest`, `LightsPunctualLamp`, Bistro) continue tracing the BRDF candidate unchanged.
- **Isolated O8 ABBA** (2,400 frames × 4, `rtxdi_initial.spv` swap):
  - Sponza gallery: `ReSTIR DI initial` `0.1518 → 0.1129 ms` (**−0.039 ms / −25.6%**).
  - Sponza courtyard: `ReSTIR DI initial` `0.2185 → 0.1747 ms` (**−0.044 ms / −20.0%**), `GPU total` `3.7148 → 3.6562 ms` (**−0.059 ms**).

### 2. F12: `Power_RIS` vs `ReGIR_RIS` on Bistro (76,248 triangle lights)

600-frame bench + 480-frame `Lit` stability (`ENGINE_STABILITY_CHECK` on Bistro startup view, post-O8):

| Config | `presample` (ms) | `Ray start wait` (ms) | `ReSTIR DI` (ms) | `GPU total` (ms) | Bistro `Lit` (mean / std / flicker / swing) |
|---|---:|---:|---:|---:|---|
| `regir_shipped` (`ReGIR_RIS, s=2 RT, p1, r1`) | 0.4237 | 0.7971 | 0.2948 | 4.240 | 0.04367 *(+3.2% leak)* / 0.55% / 2.19% / 0.31% |
| `pow_p1_r0_RT` (`Power_RIS, s=2 RT, p1, r0`) | 0.0214 | 0.1380 | 0.4097 | 3.399 | 0.04229 / 0.42% / 2.13% / 0.33% |
| `pow_p0_r0_RT` (`Power_RIS, s=2 RT, p0, r0`) | 0.0421 | 0.0790 | 0.4031 | 3.446 | 0.04231 / 0.45% / 2.11% / 0.34% |
| **`pow_s0_p0_r0_b4`** (`Power_RIS, s=0 + b8@4, p0, r0`) | **0.0221** | **0.1116** | **0.3810** | **3.562** | **0.04230 / 0.48% / 2.05% / 0.31%** |
| `pow_s0_p1_r0_b4` (`Power_RIS, s=0 + b8@4, p1, r0`) | 0.0428 | 0.1115 | 0.4088 | 3.637 | 0.04230 / 0.40% / 2.09% / 0.32% |

- **Decision (F12):** Ship `ReSTIRDI_LocalLightSamplingMode::Power_RIS` in `defaultSettings()`. It cuts `presample` by ~93% and `Ray start wait` by ~84–89% on 76K-light Bistro, eliminates ReGIR's volume-vs-power PDF mismatch with BRDF candidates, and improves Bistro `Lit` stability. `nvidiaSettings()` keeps `ReGIR_RIS`.

### 3. F5, O10, Permutation, and F6: Ray budget, DI convergence, and DLSS-RR stability

All candidates below run post-O8 + post-I5 GI + M6 OMM (`p` = permutation, `r` = `isFinalVisibilityReused`, `s` = `spatialSamples`, `b4` = `disocclusionBoostSamples = 8, disocclusionHistoryLength = 4`, `sc` = `isVisibilityShortcutEnabled`):

| Config | Courtyard DI (ms) | `LightsPunctualLamp` bias / p90 (1f err) | `Sponza_terminator` bias / p90 / leak | `EmissiveStrengthTest` bias / p90 (1f err) | Courtyard `Lit` (mean / std / flick / swing) | Gallery `Lit` (std / flick / swing) |
|---|---:|---|---|---|---|---|
| `nvidia(Medium)` (`ReGIR, s=2 Basic, p1, r1`) | 0.482 | FAIL −3.89% / 16.4% (58.0%) | FAIL −37.94% / 63.8% / 0.13% (94t) | FAIL −4.22% / 38.5% (13.4%) | 0.1780 *(−6.5%)* / 1.32% / 7.67% / 0.13% | 5.58% / 39.3% / 0.97% |
| Pre-I6 `shipped` (`ReGIR, s=2 RT, p1, r1`) | 0.628 | FAIL −0.72% / 9.7% (58.6%) | FAIL +3.39% / 27.8% / 0.27% (94t) | FAIL −0.14% / 4.8% (14.0%) | 0.1912 / 1.28% / 7.98% / 0.13% | 5.55% / 38.9% / 1.09% |
| `converged` (`Power, s=0, p0, r0, sc0, brdf0`) | 0.496 | **PASS** +0.07% / 1.75% (8.7%) | **PASS** −0.00% / 0.00% / 0 | **PASS** +0.00% / 1.97% (5.2%) | 0.1896 / 1.25% / 9.57% / 0.07% | 5.83% / 39.3% / 0.86% |
| `p0_r0_pairwise` (`Power, s=2 Pairwise, p0, r0`) | 0.440 | FAIL −8.38% / 30.1% (14.9%) | FAIL −56.07% / 80.8% / 0 | FAIL −6.04% / 52.5% (9.9%) | 0.1729 *(−9.1%)* / 1.09% / 7.12% / 0.13% | 5.78% / 39.2% / 0.90% |
| `p1_r1_pairwise` (`Power, s=2 Pairwise, p1, r1`) | 0.420 | FAIL −5.09% / 19.7% (45.0%) | FAIL −39.63% / 64.1% / 0.13% (93t) | FAIL −5.39% / 47.5% (11.7%) | 0.1779 *(−6.5%)* / 1.28% / 7.22% / 0.13% | 5.57% / 39.0% / 1.14% |
| `p1_r0_RT` (`Power, s=2 RT, p1, r0`) | 0.649 | FAIL −0.89% / 10.3% (57.5%) | **PASS** +0.02% / 1.05% / 0 | FAIL −0.29% / 5.5% (14.0%) | 0.1904 / 0.87% / 8.05% / 0.05% | 5.77% / 38.9% / 0.89% |
| `p0_r0_RT` (`Power, s=2 RT, p0, r0`) | 0.647 | FAIL +0.46% / 23.9% (53.1%) | **PASS** +0.08% / 1.41% / 0 | **PASS** +0.06% / 2.29% (12.1%) | 0.1902 / 1.01% / 8.16% / 0.07% | 5.77% / 39.1% / 0.82% |
| `p1_r0_RT_sc0` (`sc=0`) | 0.807 | FAIL −0.85% / 9.9% (58.8%) | **PASS** −0.08% / 2.94% / 0 | FAIL −0.30% / 5.1% (14.0%) | 0.1901 / 1.11% / 8.58% / 0.13% | 5.83% / 38.9% / 0.95% |
| `p0_r0_RT_tBasic` (`temp=Basic`) | 0.629 | FAIL +0.90% / 22.2% (53.4%) | **PASS** +0.08% / 1.42% / 0 | **PASS** +0.06% / 2.25% (12.1%) | 0.1866 *(−1.8%)* / 1.05% / 8.07% / 0.06% | 5.77% / 39.1% / 0.82% |
| `s0_p1_r0_b4` (`Power, s=0+b8@4, p1, r0`) | 0.434 | FAIL −0.99% / 7.6% (49.1%) | **PASS** −0.02% / 1.00% / 0 | FAIL −0.46% / 4.8% (12.0%) | 0.1901 / 0.82% / 8.34% / 0.06% | 5.77% / 39.0% / 0.92% |
| **`s0_p0_r0_b4` (shipped)** | **0.429** | **PASS +0.084% / 1.84% (8.99%)** | **PASS −0.000% / 0.03% / 0** | **PASS +0.077% / 2.13% (4.73%)** | **0.1901 / 0.99% / 8.60% / 0.06%** | **5.74% / 39.3% / 0.81%** |

**Closed levers (with numbers):**
- **`isInitialVisibilityEnabled = false`:** slower on gallery (`DI` `0.257 → 0.452 ms`, +0.195 ms) and Bistro (`0.310 → 0.416 ms`, +0.106 ms) because occluded candidates survive into downstream passes. Kept `true`.
- **`isVisibilityShortcutEnabled = false`:** slower on courtyard (`0.649 → 0.807 ms`, +0.158 ms) and worse courtyard `Lit` stability (`1.11% / 8.58% / 0.13%` vs `0.87–0.99% / 0.05–0.06%`). Kept `true`.
- **`Pairwise` spatial bias correction:** catastrophic darkening bias (`−56.1%` `Sponza_terminator`, `−8.4%` `LightsPunctualLamp`, `−6.0%` `EmissiveStrengthTest`, `−9.1%` courtyard mean) because it traces no visibility rays while `enableVisibilityShortcut` zeroes shadowed reservoirs keeping `M`. Closed.
- **`Basic` temporal bias correction:** saves only ~0.018 ms on courtyard (0 ms gallery/Bistro) while raising `LightsPunctualLamp` bias (`+0.46% → +0.90%`) and darkening courtyard `Lit` (`0.190 → 0.187`). Kept `Raytraced`.
- **Pre-M4 F6 reversal:** the 2026-11-15 finding that `spatialSamples = 0` flickers 4–5× more under DLSS-RR no longer holds with M4 GI + M6 OMM + O10: `s0_p0_r0_b4` cuts courtyard `Lit` temporal std from `1.28%` to **`0.99%`** and 8-frame swing from `0.13%` to **`0.06%`**, cuts single-frame direct error 5–6× on `LightsPunctualLamp` (`8.99%` vs `53–59%`), and passes all 3 DI convergence checks at shipped defaults.

### 4. F3: DI screen-edge behavior (`ENGINE_SCREEN_EDGE_CHECK`)

Courtyard `dolly back 1 m` (49.4% revealed band / 8.5% disoccluded) and `pan 10 deg` (18.3% shadowed band):

| Config | Courtyard `pan` `Direct` band | Courtyard `dolly` `Direct` band (static / f1 px) | Courtyard `dolly` `Direct` disocc (static / f1 px / f4 mean) | Courtyard `dolly` `Lit` band (f1 mean / px) | Courtyard `dolly` `Lit` disocc (f4 px / f16 px) |
|---|---|---|---|---|---|
| `shipped_pow` (`s=2 RT, p1, r1, fb1`) | **leaked sun** (static 167.9%) | 28.9% / 29.2% | 81.4% / 67.9% / 11.0% | 8.1% / 12.4% | 17.4% / 12.4% |
| `p1_r0_RT` (`s=2 RT, p1, r0, b0, fb1`) | black (clean) | 20.2% / 23.0% | 30.4% / 30.2% / 6.0% | 1.5% / 7.9% | 17.1% / 11.0% |
| `p1_r0_RT_b4` / `b20` (`b8@4` / `b8@20`) | black (clean) | 20.2% / 22.9% | 30.4% / 30.2% / 6.0% | 1.5% / 7.9% | 17.3% / 11.2% |
| `p1_r0_RT_fb0` (`fb=0`) | black (clean) | 20.2% / 12.1% | 30.4% / 22.8% / **16.4%** *(px 42.0%)* | 1.5% / 7.3% | 17.0% / 11.2% |
| `s0_p1_r0_b4` (`s=0+b8@4, p1, r0, fb1`) | black (clean) | 9.0% / 10.6% | 7.9% / 8.4% / 2.0% | 1.4% / 7.1% | 15.8% / 9.9% |
| **`s0_p0_r0_b4` (shipped)** | **black (clean)** | **0.0% / 5.2%** | **0.0% / 2.6% / 0.2%** | **1.3% / 7.1%** | **15.5% / 9.9%** |
| `s0_p0_r0_b0` (`s=0, b0`) | black (clean) | 0.0% / 4.6% | 0.0% / 2.6% / 0.2% | 1.4% / 7.0% | 15.4% / 10.0% |

- **Decisions (F3):** Keep `isFallbackSamplingEnabled = true` (turning it off regressed disoccluded frame-4 direct error from `6.0%` to `16.4%`). Set `disocclusionHistoryLength = 4` with `disocclusionBoostSamples = 8` (`Raytraced` spatial bias correction) so newly revealed and disoccluded multi-light pixels share neighbors for 4 frames while steady-state pixels (`M >= 4`) pay zero spatial cost. `disocclusionHistoryLength = 20` added no edge gain over `4`. No guard band needed for DI.

### 5. Final `defaultSettings()` and ABBA benchmark

`RtxdiContext::defaultSettings()` (`src/render/vk_rtxdi.cpp`) overrides `nvidiaSettings()` with:
- `localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Power_RIS` (F12)
- `isPermutationSamplingEnabled = false` (F5/F6)
- `spatialSamples = 0`, `disocclusionHistoryLength = 4` (`disocclusionBoostSamples = 8` inherited), `spatialBiasCorrection = Raytraced` (F5/F6/F3)
- `isFinalVisibilityReused = false` (O10)

Final ABBA (`A` = post-I5/pre-I6 DLL + pre-O8 `rtxdi_initial.spv`, `B` = post-I6, release, no layers):

| Scene | `presample` (ms) | `Ray start wait` (ms) | `DI initial` (ms) | `DI temporal` (ms) | `DI shade` (ms) | `ReSTIR DI` sum (ms) | `GPU total` (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|
| Sponza gallery (2,400f × 4) | 0.000 → 0.000 | 0.034 → 0.043 | 0.190 → 0.143 (−25.1%) | 0.069 → 0.061 (−12.3%) | 0.114 → 0.033 (−71.5%) | **0.374 → 0.236 (−0.138 ms / −36.9%)** | **4.379 → 4.200 (−0.179 ms / −4.1%)** |
| Sponza courtyard (2,400f × 4) | 0.000 → 0.000 | 0.033 → 0.032 | 0.224 → 0.179 (−20.0%) | 0.207 → 0.143 (−30.8%) | 0.476 → 0.219 (−54.0%) | **0.907 → 0.541 (−0.366 ms / −40.3%)** | **5.027 → 4.518 (−0.509 ms / −10.1%)** |
| Bistro 76K lights (1,200f × 4) | **0.436 → 0.032 (−92.7%)** | **0.577 → 0.092 (−84.1%)** | 0.198 → 0.255 | 0.051 → 0.067 | 0.050 → 0.033 (−33.8%) | 0.299 → 0.355 *(+0.057)* | **3.643 → 3.465 (−0.178 ms / −4.9%)*** |

*\*Bistro run 4 (`[A]2`) boosted to a higher GPU clock (`DLSS-RR 1.17 ms` vs `1.66–1.85 ms` on runs 1–3); at matched clock (`[A]1` vs `[B]1,[B]2`), Bistro `GPU total` dropped `4.418 → 3.465 ms` (−0.953 ms / −21.6%, `Ray start wait` `0.807 → 0.092 ms`).*

### 6. Verification

- `ctest --test-dir build/windows-clang-release --output-on-failure`: **7/7 passed**.
- `ENGINE_RT_CHECK=1`: **32/32 frames matched**.
- `ENGINE_CULL_CHECK=1`: **16/16 poses identical**.
- `ENGINE_MOTION_TEST=1`: **13/13 scenarios passed**.
- `ENGINE_GUIDE_CAPTURE=1`: **2/2 A4 normal + 3/3 DI convergence passed** (`LightsPunctualLamp +0.030%`, `Sponza_terminator −0.000%`, `EmissiveStrengthTest +0.063%`; 396 PNGs).
- `ENGINE_GI_CHECK=1`: **10/10 passed** (gallery shipped `+1.09%`, courtyard `−1.21%`; O2 secondary DI resampling improved to `+0.90%` / p90 `21.0%` gallery and `−0.92%` / p90 `21.2%` courtyard).
- `ENGINE_STABILITY_CHECK=1`: **28/28, 0 failed** (shipped `Lit`: gallery `5.74% / 39.27% / 0.81%`, courtyard **`0.99% / 8.60% / 0.06%`**).
- `ENGINE_LIGHT_CHANGE_CHECK=1`: **32/32, 0 failed** (courtyard sun move `56` frames, toggle `8` frames).
- `ENGINE_SCREEN_EDGE_CHECK=1`: **80/80, 0 failed**.
- Vulkan validation (`core + sync + best practices + NVIDIA best practices`, `GPUAV=false`, `ENGINE_INDIRECT_MODE=cycle`, 120 frames): **clean** (37 lines, only known third-party NGX `C-H1-2` and NRC `C-G3-3`).

## Done when

- [x] O8 landed and ABBA-measured on Sponza (with emissive scenes verified unaffected)
- [x] F12 default presampling mode (`Power_RIS` vs `ReGIR_RIS`) decided with Bistro perf, convergence, and stability numbers
- [x] Every DI ray (initial visibility, temporal correction, spatial correction, final visibility reuse O10, BRDF candidate) and permutation sampling landed or closed with cost, convergence, and stability numbers
- [x] F6 comparison (NVIDIA `Medium` vs `converged` vs shipped through DLSS-RR) recorded on sunlit Sponza
- [x] F3 DI screen-edge settings (`disocclusionBoostSamples`, `disocclusionHistoryLength`, fallback) measured and decided
- [x] `ENGINE_GI_CHECK` (10/10), `ENGINE_STABILITY_CHECK`, `ENGINE_SCREEN_EDGE_CHECK`, `ENGINE_GUIDE_CAPTURE` (3/3 DI convergence + 2/2 normal), `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST`, CTest, and Vulkan validation pass
