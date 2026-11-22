# M8 · I7: NRC bias

**Status:** built 2026-11-22 · **Milestone:** M8 ([README](./README.md))
**Implements:** findings Q7, Q1, Q2 (ND5), Q3 (ND1), Q4, Q5, Q6 (ND6) ([m8_optimization_and_fixes.md](./m8_optimization_and_fixes.md) §3.2–§3.3)
**Depends on:** I1 ([i1_check_runners.md](./i1_check_runners.md)), I2 ([i2_shading_correctness.md](./i2_shading_correctness.md))
**References:**
- `ThirdParty/_deps/src/nrc/Include/{Nrc.hlsli,NrcHelpers.hlsli,NrcCommon.h}`
- `reference/RTXGI/Samples/IndirectDiffuse/Shaders/{NrcIndirectDiffuse.hlsli,Pathtracer.hlsl}`, `Pathtracer.cpp`
- `reference/RTXGI/Samples/Shared/Nrc/NrcIntegration.cpp`
- `reference/RTXGI/Docs/NrcGuide.md`
- `spec/m4/g3_nrc.md`, `spec/m4/g7_indirect_modes.md`, `spec/considerations.md` (`C-G3-3`–`C-G3-7`, `C-G5-3`, `C-T-5`)

## Scope

In `ENGINE_GI_CHECK=1`, both `NRC + ReSTIR GI (converged settings)` (`+11.03%` gallery / `+10.68%` courtyard) and `NRC mode` without reuse (`+10.50%` gallery / `+9.51%` courtyard) read **~10% bright** against the 8-bounce traced ground truth (`C-G5-3`), while ReSTIR GI on traced paths stays within `< 1%` using the same `directLightAt`, `scatterBrdf`, and `shadeRayHit`. Because bounces `2..9` hold `39.5%` of total indirect luminance in gallery (`52.65 / 133.4`) and `13.0%` in courtyard (`281 / 2157`), the `+10.5% / +9.5%` total bias represented a **`+26.6%` (`1.27×`)** over-prediction on the cache term in gallery and **`+73.0%` (`1.73×`)** in courtyard.

I7 bisected and resolved `C-G5-3` in the order set by findings §3.3 (Q7, Q1, Q2/ND5, Q3/ND1, Q4, Q5, Q6/ND6).

## Root cause (`C-G5-3` / `C-G3-7`)

**Upstream defect in `NRC_Vulkan.dll` 0.16 under `includeDirectLighting = false` + `hasExitedScene` (`NrcUpdateOnMiss`):**

When a training path exits the scene (`NrcUpdateOnMiss(path)` sets `nrcPathFlagHasExitedScene`), `NrcWriteFinalPathInfo` (`Nrc.hlsli:536`) finalizes `v[last]` with `(radiance = D_last + w_last * L_sky, throughput = w_last)` and skips the self-training query. Under `includeDirectLighting = false`, `NRC_Vulkan.dll` 0.16 still trains `v[last]` on `v[last].radiance` when `hasExitedScene` is set — leaking `v[last]`'s own direct lighting `D_last` into its indirect-radiance training target (presumably assuming `v[last].radiance` contains only sky radiance `w_last * L_sky`, which cannot be separated from `D_last` in a single `radiance` field).

Controlled bounce-isolation proof (`ENGINE_GI_CHECK=1`, Sponza, `includeDirectLighting = false`, `proportionPrimarySegmentsToTrainOn = 1e-5`, `proportionTertiaryPlusSegmentsToTrainOn = 0.0`, `selfTrainingAttenuation = 0.0` so **only `v[1]`** trains and its true indirect radiance is identically `0`):

| Probe | Gallery `NRC mode` G bias | Courtyard `NRC mode` G bias | What it proves |
|---|---:|---:|---|
| 1-bounce floor (zero cache) | −37.98% | −12.65% | Exact zero-cache baseline |
| `green_b1` (`(0,10,0)` at `bounce=1`, `0` at `bounce≠1`), `includeDirectLighting = true` | +364,996% | +28,992% | Full direct-light scale at `v[1]` |
| `green_b2` (`(0,10,0)` at `bounce=2`, `0` elsewhere), `includeDirectLighting = false` | +84,239% | +6,334% | Expected `w_1 · (0,10,0)` (`≈ 0.22–0.23×`) |
| `green_b2` + forced `NrcUpdateOnMiss` at `bounce=3` | +84,215% | +6,337% | `v[last].radiance` IS propagated backward to `v[last−1]` on miss |
| `green_b1`, `includeDirectLighting = false`, `NrcUpdateOnMiss` on sky miss | **+1,033.9%** | **+679.0%** | **Direct light `D_1` leaks into `v[1]`** (`0.29%` gallery / `2.39%` courtyard of `D_1`; `p90 +7,307%` / `max +14,542%`) |
| `green_b1` + `throughput = 0` on sky miss (still calling `NrcUpdateOnMiss`) | +1,035.3% | +671.7% | `throughput = 0` does NOT stop the `hasExitedScene` leak |
| `green_b1` + zero `radiance` on sky miss | **−37.69%** | **−12.64%** | Leak drops to exact `0` (proves `hasExitedScene` paths are the sole carrier), but drops `D_last` from `v[last−1]` (`−4.18%` unbiased gallery) |
| `green_b1` + sentinel zero-radiance miss vertex (`terminateZeroContinuation`) | **−37.42%** | **−12.64%** | **Zero leak** while keeping `D_last` for `v[last−1]` and training `v[last]` on `w_last · 0 = 0` |

## Fix landed

1. **Sentinel zero-continuation vertex in [`shaders/passes/nrc_update.slang`](../../shaders/passes/nrc_update.slang) (`C-G5-3`, Q7, `C-G3-7`):**
   - `terminateZeroContinuation` appends one zero-radiance sentinel vertex at `NrcEncodedPosition(0, 0, 0)` (the `kBoundsMargin = 1%` padded scene-bounds corner, disjoint from all scene geometry, with `diffuseReflectance = 1`, `specularF0 = 0`, `roughness = 1`) via `NrcUpdateOnHit`, followed by `NrcUpdateOnMiss(path)` and `NrcSetDebugPathTerminationReason(path, reason)`.
   - Used for both sky misses (`!traceClosest`, `PathMissExit`) and `BRDFAbsorption` (`!scatterBrdf`, Q7) where continuation radiance is identically `0` under the one-emission rule (`path_shading.slang`).
   - This finalizes `v[last]` as an interior vertex — preserving `D_last` for `v[last−1]` and training `v[last]` on `w_last · 0 = 0` — and places `hasExitedScene` on the zero-radiance sentinel vertex so `NRC_Vulkan.dll` leaks `0`. Primary G-buffer misses (`vertexCount == 0`) keep `NrcUpdateOnMiss(path)` alone.
2. **SDK debug resolve modes (`Q2` / `ND5`) in [`src/render/vk_nrc.hpp`](../../src/render/vk_nrc.hpp), [`src/render/vk_nrc.cpp`](../../src/render/vk_nrc.cpp), [`src/render/vk_renderer.cpp`](../../src/render/vk_renderer.cpp), [`src/render/vk_render_targets.hpp`](../../src/render/vk_render_targets.hpp), [`src/render/vk_render_targets.cpp`](../../src/render/vk_render_targets.cpp), [`src/core/vk_frame_sync.hpp`](../../src/core/vk_frame_sync.hpp), and [`src/ui/engine_ui.cpp`](../../src/ui/engine_ui.cpp):**
   - `globalSettings.enableDebugBuffers = true` (~0.25 MiB VMA-owned at 853×480); `kRaySegmentsPerFrame` bumped `7 → 9` for isolated `NRC Resolve`.
   - `NrcCache::Settings::resolveMode` exposed as `NRC resolve mode` in ImGui (`GetImGuiResolveModeComboString()`). Default `AddQueryResultToOutput` runs the custom GI reservoir resolve with **0 bytes** of debug target memory; any other mode lazily allocates an `R32G32B32A32_SFLOAT` target matching NRC's `OpTypeImage Rgba32f` storage descriptor (`Undefined-Value-StorageImage-FormatMismatch-ImageView` clean) and blits the render-extent region into `indirectRadiance` (`eTransferDst`).
3. **Gated NRC phases in [`src/core/engine_gi_check.hpp`](../../src/core/engine_gi_check.hpp):**
   - `NRC + ReSTIR GI (converged settings)` gated at `±5.0%` gallery / `±2.0%` courtyard (`RGB ±7.5% / ±3.0%`, tile p90 `22.0%`).
   - `NRC mode` gated at `±5.0%` gallery / `±1.5%` courtyard (`RGB ±7.5% / ±2.25%`, tile p90 `18.0%`).
   - Total gated phases in `ENGINE_GI_CHECK=1`: `10 → 14`.
   - `IndirectMode::RestirGi` stays default until I8 evaluates NRC's per-frame cost (`C-G3-6` host wait, NP2, NP5).

## Results

### 1. Bisect sweep across Q1, Q3, Q4, Q5, Q6, Q7 (pre-fix baseline, RTX 5070, release, no layers)

| Lead / Config | Gallery `NRC+GI` / `NRC mode` | Courtyard `NRC+GI` / `NRC mode` | Verdict |
|---|---:|---:|---|
| Baseline (`includeDirect=0, learnIrr=1, iters=1, maxRad=1.0, lr=1e-2`) | +11.03% / +10.50% | +10.68% / +9.51% | Baseline (`C-G5-3`) |
| **Q7** `throughput=0 + NrcUpdateOnMiss` on absorption | +14.81% / +12.21% | +15.99% / +14.87% | **Worse** because `NrcUpdateOnMiss` triggered the same `hasExitedScene` leak; fixed via sentinel vertex (`−2.3 / −1.1 pp`, removing absorption self-training inflation) |
| **Q7** `throughput=0` only on absorption | +10.52% / +10.43% | +11.03% / +9.41% | Neutral (`NRC_Vulkan.dll` does not scale `Q_last` by `v[last].throughput`) |
| **Q1** `selfTrainingAttenuation = 0` | +2.19% / −0.78% *(p90 53%)* | +8.01% / +6.44% | Still +6.4% on courtyard even truncated |
| **Q1** `proportionUnbiased = 1.0, selfTraining = 0` | +8.29% / +6.15% | +8.71% / +7.84% | Still +6–9% with zero self-training & 8-vertex paths |
| **Q1** `learnIrradiance = false` | +13.46% / +10.17% | +10.16% / +9.10% | Closed (not `learnIrradiance`) |
| **Q1** `includeDirectLighting = true` | −9.92% / −10.27% | −1.27% / −2.16% | Eliminated bright leak (confirming bug is specific to `includeDirectLighting = false`), but under-predicted sunlit patches from dark gallery by −10% |
| **Q1** resolve `ao = 1.0` | +10.5% / +10.5% | +10.7% / +9.5% | Neutral on Sponza |
| **Q4** `numTrainingIterations = 4` (64K records) | +14.42% / +10.60% | +10.02% / +8.60% | **Closed**: no bias gain over `1` iteration |
| **Q4** `learningRate` `3e-3 / 1e-3 / 3e-4` | +11.1 / ~+10 / +9.9% | ~+9.8 / +9.8 / +9.9% | **Closed**: keep SDK default `1e-2` |
| **Q5** `maxExpectedAverageRadianceValue` `0.05 / 0.25 / 4 / 10 / 50` | +9.7 / +10.8 / +6.1 / +7.1 / −18.7% | +8.6 / +9.2 / +8.2 / +7.8 / +6.0% | **Closed**: `≥ 10` collapses dark tiles (`p90 38–81%`); `1.0` stays best |
| **Q6** (`ND6` root attributes) `zero_b0`, `prim = 0.0 / 0.2` | +10.8% / +10.5% | +9.5% / +9.5% | **Closed**: vertex 0 contributes negligibly (`prim = 0.02`) |

### 2. Q3 (`ND1` query termination heuristic) — closed on cost vs gain

Measured on top of the sentinel sky-miss fix (`1200` frames × 6 ABBA runs, `ENGINE_INDIRECT_MODE=nrc-restir-gi`, Sponza gallery, release, no layers):

| Variant | `Indirect` | `NRC train` | `GPU total` | Gallery `NRC+GI` / `NRC mode` | Courtyard `NRC+GI` / `NRC mode` |
|---|---:|---:|---:|---:|---:|
| **Q3 off (shipped)** | **0.666 ms** | **0.406 ms** | **3.486 ms** | +1.60% / +0.56% *(sky-only)* | +2.02% / +0.96% *(sky-only)* |
| Q3 `0.01` | 0.837 ms (+0.171 ms) | 0.421 ms | 3.628 ms (+0.142 ms) | +1.12% / +1.81% | +2.47% / +1.48% |
| Q3 `0.1` | 1.276 ms (+0.610 ms) | 0.506 ms | 4.161 ms (+0.675 ms) | −0.06% / +0.02% | +1.53% / +0.63% |

Q3 at `0.1` nearly doubles `Indirect` (`+0.610 ms` / `+91.6%`, `+0.675 ms` / `+19.4%` GPU total) while querying at `X₂` with the sentinel fix already gates cleanly on both poses. Q3 is closed with these numbers.

### 3. Five-seed GI check spread (`ENGINE_GI_CHECK_SEED=0..4`, shipped sentinel fix on sky miss + `BRDFAbsorption`)

| Seed | Gallery `NRC+GI` (bias / p90) | Gallery `NRC mode` (bias / p90) | Courtyard `NRC+GI` (bias / p90) | Courtyard `NRC mode` (bias / p90) |
|---:|---:|---:|---:|---:|
| 0 | −2.16% / 12.90% | −1.76% / 6.71% | +0.96% / 7.61% | −0.11% / 3.28% |
| 1 | −2.84% / 15.67% | −0.98% / 6.10% | +0.98% / 8.94% | −0.21% / 4.37% |
| 2 | −2.78% / 17.94% | −1.27% / 6.52% | +1.07% / 11.73% | −0.06% / 9.03% |
| 3 | −1.74% / 11.61% | −2.66% / 13.10% | +0.84% / 8.80% | −0.15% / 4.92% |
| 4* | −3.93% / 13.31% | −3.63% / 7.56% | +0.80% / 7.99% | −0.16% / 3.63% |
| **Range** | **−3.93…−1.74% / 11.6…17.9%** | **−3.63…−0.98% / 6.1…13.1%** | **+0.80…+1.07% / 7.6…11.7%** | **−0.21…−0.06% / 3.3…9.0%** |
| **Gate** | **±5.0% / 22.0%** | **±5.0% / 18.0%** | **±2.0% / 22.0%** | **±1.5% / 18.0%** |

*\*Seed 4's gallery 2048-frame ground truth contains a high-energy multi-bounce firefly (`traced paths` itself reads `−1.35%`, `max 64.2%`).*

### 4. Verification (2026-11-22, `windows-clang-release`, RTX 5070)

- **CTest:** `7 of 7` passed (`hiz_footprint`, `brdf`, `lights`, `rtxdi_math`, `layout`, `nrc_library`, `dlss`).
- **`ENGINE_GI_CHECK=1`:** **`14 of 14` passed** (gallery `NRC+GI` p90 `14.64%`, `NRC mode` `−1.39%` / p90 `6.22%`; courtyard `NRC+GI` `+0.72%` / p90 `8.11%`, `NRC mode` `−0.10%` / p90 `5.77%`).
- **`ENGINE_STABILITY_CHECK=1`:** `28 steps, 0 failed` (courtyard shipped `Lit`: `0.99%` temporal std / `8.60%` flicker / `0.06%` swing).
- **`ENGINE_LIGHT_CHANGE_CHECK=1`:** `32 steps, 0 failed`.
- **`ENGINE_SCREEN_EDGE_CHECK=1`:** `80 steps, 0 failed`.
- **`ENGINE_RT_CHECK=1`:** exited 0 (`32/32`).
- **`ENGINE_CULL_CHECK=1`:** `16 of 16` poses identical.
- **`ENGINE_MOTION_TEST=1`:** `13 of 13` scenarios passed.
- **`ENGINE_GUIDE_CAPTURE=1`:** `2 of 2` normal checks, `3 of 3` DI convergence checks passed.
- **Vulkan validation** (core + sync + best practices + NVIDIA best practices, `GPUAV=false`, `ENGINE_INDIRECT_MODE=cycle`, `120` frames): clean — only known external SDK messages (NGX `WorkgroupSize` + `nv.ngx.dlssd.resource` `C-H1-2` and NRC `vkDestroyDevice` `C-G3-3`).

## Done when

- [x] Q7 (`BRDFAbsorption` zero-continuation) tested and landed via `terminateZeroContinuation` sentinel vertex
- [x] Q1 (self-training attenuation / unbiased / `learnIrradiance` / `includeDirectLighting` bisect) recorded with exact bias numbers and upstream root cause isolated
- [x] Q2 (SDK debug resolve modes, ND5) built with validation-clean lazy `RGBA32F` target + ImGui combo
- [x] Q3 (ND1 query termination heuristic) measured for bias and ABBA cost (`+0.610 ms` `Indirect` / `+0.675 ms` GPU total at `0.1`), closed with numbers
- [x] Q4 (training iterations / learning rate) and Q5 (`maxExpectedAverageRadianceValue`) measured and closed
- [x] Q6 (ND6 root attributes) ruled out and closed
- [x] Both NRC phases gated in `ENGINE_GI_CHECK` (`14 of 14` pass across 5 seeds), `C-G5-3` resolved (`C-G3-7` workaround documented); all runners and Vulkan validation pass
