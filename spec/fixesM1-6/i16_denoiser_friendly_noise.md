# M8 · I16: Noise that DLSS-RR can denoise (F28)

**Status:** Phase 0 measured 2026-11-24 (§7.1); 1a, 2a shipped; 2b, 2c measured (§7.5, §7.6); Phase 3 next · **Milestone:** M8 ([README](./README.md))
**Implements:** F28 ([m8_optimization_and_fixes.md](./m8_optimization_and_fixes.md) §4): with a still camera the lit image "lives". Light seems to move every frame, and blotches fade in and out over seconds. Games that ship ReSTIR don't show it. Target mode: `NRC + ReSTIR GI`, the default and tuning target since 2026-11-24 ([engine_spec.md](../../docs/engine_spec.md) §11.7). The goal is a still camera that looks still.
**Depends on:** I14 (correct rays, reuse and light PDF), I15 (DI with two spatial samples, GI sample age 30, NRC + ReSTIR GI default), the settings lab ([full_testing.md](../features/planned/full_testing.md))
**Feeds:** I12 (final Bistro tuning re-checks this step's decisions), I11 (cost on the 2060 Super)
**References:**
- RTXDI `Doc/NoiseAndBias.md` §Denoiser friendliness. With one temporal pass the signal is "too stable", and the denoiser leaves it unblurred, "so the result will be blotchy. The blotches will also fade in and out slowly". Spatial samples break the patterns up; shorter histories add temporal variation at the cost of noise.
- RTXDI `Doc/RestirPT.md` §Decorrelation for DLSS Ray Reconstruction: "DLSS-RR expects temporally uncorrelated input noise, whereas ReSTIR's temporal reuse deliberately correlates samples across frames". Stochastic swap to the initial-sampling reservoir, stagnancy scaling, a smoothed duplication map, firefly replacement. `Libraries/Rtxdi/Include/Rtxdi/PT/Decorrelation.hlsli`.
- RTXDI FullSample `UserInterface.cpp` `ApplyDenoiserCompatDefaults`: the DLSS-RR profile touches ReSTIR PT only. ReSTIR DI under DLSS-RR relies on the quality presets (`Medium`: spatial samples and permutation on).
- `reference/vk_denoise_dlssrr` `dlssrr_wrapper.cpp:364` feeds `pInSpecularHitDistance`. `README.md:95`: "Providing specular motion vectors is superior."
- DLSS-RR inputs of the pinned SDK: `nvsdk_ngx_helpers_dlssd_vk.h` `NVSDK_NGX_VK_DLSSD_Eval_Params` (`pInSpecularHitDistance`, `pInDiffuseHitDistance`, `pInMotionVectorsReflections`, `pInSpecularRayDirectionHitDistance`, `pInDisocclusionMask`, …). The engine fills color, output, depth, motion, albedos, normals / roughness and the matrices (`vk_dlss.cpp:286-307`).

## Scope

### What the measurements say (2026-11-24, RTX 5070, 5 seeds)

`Lit` through DLSS-RR, static camera, 480 frames after 300, NRC + ReSTIR GI, shipped DI and GI (lab `i15_nrc_dia`):

| `Lit` | café tables | café dining | Sponza gallery | Sponza courtyard | Bistro street |
|---|---|---|---|---|---|
| temporal std | 6.70 % | 6.83 % | 3.98 % | 1.02 % | 0.46 % |
| flickering px (CV > 10 %) | 32.0 % | 36.1 % | 6.0 % | 6.1 % | 12.4 % (spread 1.7–26.8) |
| lag-1 correlation | 0.99 | 0.99 | 0.99 | 0.98 | 0.94 |

The floor DLSS-RR reaches on a clean, noise-free input (`ENGINE_DLSS_CHECK`, I14) is 0.29–0.33 % std and 0.03–0.04 % flicker. The shipped café is 20× above it in std and 800× in flicker.

- **The noise is slow, not grainy.** A lag-1 correlation of 0.99 means a pixel's error carries from one frame to the next almost unchanged. DLSS-RR is fed blotches that live for many frames, which is NoiseAndBias.md's failure.
- **DI is one source.** In the café, two spatial samples halved `Lit` flicker (I15: 77.6 → 32.0 %). Permutation on top brings it to 24.1 %, but it darkens DI and is off (I15: café dim tiles −8…−10 %, lamp convergence shot −1.3 % over 2048 frames).
- **The sun became stochastic.** Spatial reuse took the courtyard's raw `Direct lighting` from 0.00 % to 21.4 % temporal std (I15 lab `i15_di`). With one directional light and no reuse, the sun had been exact. DLSS-RR absorbs most of that (courtyard `Lit` 1.00 → 1.02 %), but the input is no longer clean.
- **GI is the other source.** ReSTIR GI keeps samples by design. Stagnancy decorrelation (shipped) is NVIDIA's counter-measure; NRC + ReSTIR GI is calmer than ReSTIR GI mode on Sponza (gallery flicker 6.0 % against 39.4 %). Phase 0 shows how much of the café's flicker is GI.
- **DLSS-RR gets fewer guides than NVIDIA's samples give it.** No specular or diffuse hit distance and no reflection motion vectors. Under motion, DLSS-RR then cannot tell a reflection from the surface it lies on.

### How shipped ReSTIR renderers avoid it, and the three approaches

1. **Decorrelate before the denoiser** (Phase 1): spatial reuse, permutation sampling, stagnancy decorrelation, short histories. The input noise becomes new each frame, which DLSS-RR averages away, instead of stable blotches it keeps.
2. **Keep the dominant lights deterministic** (Phase 2): the sun, and lights that dominate a region, are not re-picked stochastically per pixel and frame. ReSTIR handles the many small lights where it pays off.
3. **Give DLSS-RR every guide it takes** (Phase 3): hit distances and reflection motion, so it can accumulate without smearing or rejecting.

### Targets

The rule for "ideal": at a still camera, `Lit` on the café and Sponza poses comes within reach of DLSS-RR's clean-input floor. Bias stays within today's gates. Phase 0 proposes the numbers from what the best decorrelated and unbiased configuration reaches; the user confirms them before Phase 1 lands. Starting proposal:

| Pose | `Lit` temporal std | `Lit` flicker |
|---|---|---|
| café tables, café dining | ≤ 2 % | ≤ 5 % |
| Sponza gallery, courtyard, Bistro street | ≤ 1 % | ≤ 2 % |

Bias gates, all phases:
- **DI:** shipped DI within ±1 % of RTXDI's reference on both café poses, with dim tiles within 2 floors of the reference against itself. Shipped DI within the convergence limits (0.5 % bias, 3 % tile p90) on the convergence check's three shots over 2048 frames, the new phase of §5.
- **GI:** every gated GI check phase, and NRC + ReSTIR GI against the 8-bounce truth within its per-pose limits.

## Decisions

| Item | Decision | Reason |
|---|---|---|
| Target mode | every number is read in NRC + ReSTIR GI unless the line says otherwise | the user's target (2026-11-24) |
| NVIDIA sets | `nvidiaSettings()` / `nvidiaGiSettings()` stay callable and unchanged | C-M4-2 |
| Vendor code | no edits under `ThirdParty/`; DI decorrelation is engine code in `rtxdi_shade` / the bridge | RTXDI DI has no decorrelation |
| Order | Phase 0 → 1 → 2 → 3; each phase measures over the previous phase's result | Phase 1 changes what Phases 2–3 see |
| Seeds | 5 seeds per candidate: each finalist is the base of its own plan, since `seeds` seeds only the base | lab rule (I15) |
| Cost | ABBA bench for every shipped change (gallery, courtyard, Bistro street), release, layers off | I4 method |

## 0. Phase 0 · Measure the boiling

- **Lab plan `tests/lab/boiling.lab`** (built 2026-11-24, 153 runs): cases `cafe.tables`, `sponza.gallery`, `sponza.courtyard`; views `Lit`, `Direct lighting`, `Indirect radiance`. Shipped against:
  - DI: `di=reference` (no reuse, independent per frame), permutation, history 4 / 8, 32 candidates;
  - GI: decorrelation factor 1, history 4 / 8;
  - both decorrelated;
  - DLSS-RR presets D / E;
  - ReSTIR GI mode.

  `ENGINE_LAB_PNG=1` writes the mean, noise and error maps.
- **New lab metrics** (`lab_metrics.cpp`, both kinds of stability run):
  - `corrFrames`: the lag at which a pixel's temporal autocorrelation falls below 1/e, median over lit pixels. The blotch lifetime: ~1 for white noise, the history length for stuck samples.
  - `blotchPx`: the spatial correlation length of the per-frame deviation from the temporal mean, in pixels (where the radial autocorrelation falls below 1/e). The blotch size.

  Both are read on the raw views (what DLSS-RR is fed) and on `Lit` (what it keeps).
- **Output:** a table per case, with std, flicker, lag1, `corrFrames` and `blotchPx` per view and config. Each lever's share of the café's `Lit` flicker. The target numbers.

## 1. Phase 1 · Decorrelate before the denoiser (approach 1)

### 1a. Permutation sampling without its bias

NVIDIA's DI presets use permutation sampling under DLSS-RR. Ours darkens:
- lamp convergence shot −1.29 % over 2048 frames;
- `EmissiveStrengthTest` −0.32 % (tile p90 4.6 %);
- café DI −0.51 % / −1.91 %, dim tiles −9.7 % / −8.4 % (I15).

I6 fixed one bridge bug under permutation (`RAB_GetTemporalConservativeVisibility` now traces from `previousSurface`, m8 §2.7). The remainder was never root-caused.

- **Method:** lab `bisect` with `target` = shipped + permutation, on `guide.lamp` and `cafe.tables` `Direct lighting:dimBias`. Then read the bridge paths that differ when the temporal neighbor is another pixel:
  - the visibility shortcut, which keeps `M` for shadowed samples (`isVisibilityShortcutEnabled`, the same mechanism as I6's `Pairwise` darkening);
  - `RAB_GetGBufferSurface(pixel, true)` and `RAB_AreMaterialsSimilar` on the permuted pixel;
  - the jittered previous position;
  - the BRDF stash `gRtxdiBridge.brdfRayOrigin` (C-I14-1), filled for the current pixel.

  Compare each with FullSample's `RAB_*` (reference-first).
- **Ship** permutation when it passes the DI bias gates; otherwise record the cause and the numbers.

### 1b. DI stagnancy decorrelation

RTXDI decorrelates only ReSTIR PT. Port the pattern to DI final shading:
- **Keep the initial reservoir:** keep the initial-sampling reservoir (before temporal reuse) in its own slot of the DI reservoir buffer.
- **Smoothed stagnancy:** compute stagnancy from the final reservoir's age, smoothed by a 5×5 average and an EMA (`RTXDI_PTComputeSmoothedDuplicationMap`'s method). The raw per-pixel value biases, as RestirPT.md notes.
- **The swap:** with probability `factor · 4 · stagnancy^exponent`, shade the initial reservoir instead of the resampled one. The stored reservoirs keep their history.
- **Firefly replacement:** force the swap where the DI boiling test flags an outlier.

Switches: `di.decorrelation` (`none` / `stagnancy`), `di.decorrelationFactor`, `di.decorrelationExponent`, `di.decorrelationEma`, `di.fireflyReplacement`. Lab registry entries; `static_assert`s on `RtxdiContext::Settings`. NVIDIA's set keeps `none`.

### 1c. Histories and filters

Sweep, over 1a / 1b's result:
- DI `maxHistoryLength` 4 / 8 / 12 / 20;
- DI boiling filter (`di.boilingFilter`, `di.boilingStrength`);
- GI `maxHistoryLength` 4 / 8 / 20;
- GI decorrelation factor 0.4 / 0.6 / 0.8 / 1.0.

Shorter histories add noise per frame (NoiseAndBias.md), which DLSS-RR handles better than blotches.

- **Rule:** the lowest café `Lit` flicker, then the lowest `corrFrames`, among the configs that pass the bias gates. Sponza and Bistro street stay within 2 floors of their best. Cost goes in the decision line.

## 2. Phase 2 · Deterministic dominant lights (approach 2)

### 2a. The sun outside ReSTIR DI

Directional lights reach DI today as RTXDI infinite lights (`infiniteLightBufferRegion`, `vk_rtxdi.cpp:695`), so spatial reuse resamples them (§Scope).
- **Change:** shade directional lights analytically in the DI shade pass, with one shadow ray per pixel and frame.
  - The direction is drawn inside the sun's cone from a per-frame independent sequence (spatiotemporal blue noise).
  - Leave them out of RTXDI's infinite region (`numInfiniteLightSamples = 0`).
- **ReSTIR GI and NRC:** their secondary-surface NEE keeps its own sun sample, unchanged.
- **Reference-first:** read how FullSample and `vk_gltf_renderer` sample the sun first, and record the departure (C-I16-2).
- **Measured:**
  - courtyard raw `Direct lighting` std (target: per-frame white noise only, `corrFrames` ≈ 1);
  - `Sponza_terminator` convergence;
  - cost (the sun's NEE ray replaces its RTXDI candidates and spatial rays).

### 2b. Dominant emitters in emissive-only interiors

Conditional: only if the café is still above target after 1 and 2a.
- **Cheap levers first:** more initial candidates (`di.localSamples` 8 / 16 / 32), `di.brdfSamples`, and Power_RIS tile size and count.
- **Then a dominant-light split:** the N lights with the largest PDF share in a 16×16 tile (from the Power_RIS tile build, `ENGINE_LIGHT_PDF_STATS`) get one deterministic sample each, MIS-combined with ReSTIR for the rest.
- **Ship** only with the DI bias gates and an ABBA cost line.

### 2c. GI variance in emissive interiors

Proposed by 2b (§7.5): DI no longer limits the café, and the raw GI input is at ~470 % std.
- **Built:** `GiSettings::areInfiniteLightsShadedExactly` (lab `gi.exactInfinite`, UI "Exact directional lights" under GI).
  - Path vertices (X₂, NRC training paths, the glass pass's diffuse share) drop directional lights from their RIS candidates and shade each one with one shadow ray (`exactInfiniteLightSample` in `lighting/path_shading.slang`).
  - With O2 on, the host takes DI's switch instead. X₂'s candidates and the reused DI reservoirs then hold the same lights, which closes C-I16-4.
- **Levers measured** (`i16_gi_variance.lab`):
  - O2;
  - the exact sun at path vertices;
  - 32 local candidates (`di.localSamples` is shared with path-vertex NEE);
  - the firefly clamp;
  - DLAA (2.25× the samples per output pixel);
  - the two other indirect modes.
- **Diagnostics:**
  - NRC training (`i16_nrc_training.lab`);
  - per-pixel CV maps (`i16_cv_maps.lab` with `ENGINE_LAB_PNG=1`);
  - a no-jitter control (`i16_cv_nojitter.lab`).
- **Candidates on 5 seeds:** `i16_gi_o2`, `i16_gi_exact` and `i16_gi_o2_32` on the café and Sponza poses.

## 3. Phase 3 · Give DLSS-RR every guide (approach 3)

| Input | Source in the engine | Notes |
|---|---|---|
| `pInSpecularHitDistance` | distance from the primary hit to the specular lobe's secondary hit: the ReSTIR GI reconnection hit on glossy lobes, I13's glass reflection ray | R16F, render resolution; vk_denoise_dlssrr feeds it |
| `pInDiffuseHitDistance` | the ReSTIR GI sample's hit distance on the diffuse lobe | R16F |
| `pInMotionVectorsReflections` | reprojection of the reflected point (virtual image of the specular hit) for roughness below a threshold | "superior" to hit distance (vk_denoise_dlssrr README) |
| `pInDisocclusionMask` | the vis buffer's previous-frame test (already computed for the screen-edge check's masks) | optional |

Each input is a frame flag first, measured, then shipped.
- **Static camera:** since §7.6, the café's remaining `Lit` flicker sits on edges, metal and glass, where DLSS-RR has no specular guide. Measure café `Lit` flicker and the flickering share on edges (`cv_edges` analysis of §7.6) per guide.
- **Motion:** `ENGINE_SCREEN_EDGE_CHECK` settle frames, and the DLSS check's pan f2f.
- **C-I14-7's exit:** a guide-sensitive case for `ENGINE_DLSS_CHECK`, a specular highlight under motion.

## 4. Checks

- **Lab plans per phase:** `boiling`; `i16_permutation` (bisect); `i16_di_decorrelation`; `i16_histories`; `i16_sun`; `i16_guides`. Each finalist on 5 seeds as its own base.
- **`ENGINE_STABILITY_CHECK`:** add the shipped set in NRC + ReSTIR GI mode to the Sponza and café step lists. The shipped ReSTIR GI-mode steps stay, for comparison and repeatability. NRC steps do not repeat byte for byte (C-T-5); read them against 5-seed floors.
- **Convergence of the shipped DI:** a measured (not gated) phase in the guide capture that runs `defaultSettings()` over the three convergence shots, next to the gated Converged phase. I15 ran this by hand: A 2 of 3 by the Converged limits (lamp tile p90 14.6 %), B 1 of 3.
- **Every runner at the end:**
  - GI check on 5 seeds;
  - café runners;
  - stability, light change and screen edge;
  - convergence 3 of 3;
  - motion, guide capture, RT and cull checks;
  - DLSS check;
  - validation clean, and GPU-AV for the new buffers (Phases 1b, 3).

## 5. Measurements to record

- Phase 0's table and the target numbers the user confirms.
- Per shipped change:
  - café, Sponza and Bistro street `Lit` std, flicker, lag1, `corrFrames` and `blotchPx`;
  - DI bias and dim tiles against the reference;
  - shipped-DI convergence;
  - GI 1-bounce and 8-bounce bias;
  - ABBA cost.
- The before / after noise maps of `cafe.tables` and `sponza.courtyard` (lab PNGs).

## 6. Considerations opened

- **C-I16-1 · Deviation:** DI stagnancy decorrelation (1b) is the engine's port of ReSTIR PT's; RTXDI's DI has none. **Exit:** an RTXDI release with DI decorrelation.
- **C-I16-2 · Deviation:** directional lights outside ReSTIR DI (2a), if shipped, depart from the RTXDI sample. **Exit:** none planned; recheck if an environment light arrives (it stays in RTXDI).
- **C-I16-3 · Limitation:** the NRC + ReSTIR GI stability steps don't repeat byte for byte (NRC training, C-T-5); they are read against 5-seed floors.
- **C-I16-5 · Workaround · the NRC network can diverge, and stays diverged (2c, 2026-11-24).**
  - **Seen:** in the first `i16_gi_variance` run, the DLAA lever grew the render extent from 853×480 to 1280×720 and `NrcCache::recreate` reconfigured the SDK.
  - **From then on, every NRC-mode run was wrong:**
    - café `Indirect radiance` mean 2981 instead of 0.0011;
    - 99.98 % of pixels lit, sky included;
    - 1-bounce bias +3.3e8 %;
    - the same numbers on all 5 seeds;
    - the `Indirect radiance` view saturated magenta.
  - **Not affected:** ReSTIR GI-only mode in the same process.
  - **Training does not recover it:** every camera change restarts the 512-frame countdown.
  - **Intermittent:** 1 failure in 3 resizes. Not reproduced by a base → DLAA plan, by the same café-tables order rerun, nor by the dining rerun, which resized too.
  - **What it is:** the network itself diverged; the engine reads its output correctly.
    - Every pixel rewrites its query path info each frame (`NrcWriteFinalPathInfo` runs on every lane of the indirect pass), so no stale rows are read.
    - A NaN answer would come out dark: the resolve drops it, since `any(NaN > 0)` is false.
    - The training countdown was not stuck: `setRenderExtent` makes the next frame a camera cut, and the lab cuts at every phase, so the cache trained after the reconfigure.
    - NrcGuide.md §Step 3: `Configure` "reloads the neural network configuration", so the resize started a fresh network, which then diverged during training.
    - Retraining does not bring a diverged network back. `requestReset` keeps the weights (C-G3-5).
    - The root cause inside the SDK is not known. Suspects: C-G3-6 (CPU-side work overlapping NRC's CUDA kernels) and stochastic training of a fresh network on the café's emitters.
  - **Fix built (detector and fresh network):**
    - **Measure:** `passes/nrc_resolve.slang` compares, per wave, the sum of the cache answers with the sum of the radiance traced at the same X₂ vertices. A wave diverges when an answer is non-finite or the ratio exceeds 50. One atomic per wave writes the counts to `GpuNrcHealth`, a host-visible buffer per frame slot.
    - **Decide:** `NrcCache::isNetworkDiverged` reads the slot after its fence. When more than half of the waves diverged for 64 frames in a row, with at least 256 waves that have traced light per frame, it waits for the device. `recreateNetwork` then destroys the SDK context, creates a new one and configures it: a fresh network, retrained from the next frame.
    - **Logged:** "the network diverged: …".
  - **Calibration** (temporary hooks, removed):
    - healthy café and Sponza poses: at most 13 % of waves above the ratio (dining), 19 % for a moment while retraining after a scene load;
    - single waves reach 3e5 where X₂'s traced light is near zero, which is why the test is per frame share, not per wave;
    - a network trained on radiance × 1e9 read 85 % of waves (largest ratio 1e7). It was replaced after 16 frames (the first threshold), and the plan's runs read healthy afterwards: indirect mean 0.00127, `Lit` 0.071;
    - the guide capture then caught a false positive: a fresh network on a small test scene read 53 % of only 57 waves for 16 frames, an early-training transient. That set the floor of 256 waves and the 64 frames. The poisoned failure stays above 50 % for its whole run, so 64 frames still catch it.
  - **Cost:** two `WaveActiveSum` calls and up to three atomics per wave in the resolve. Each reset leaks the SDK objects of one `Configure` and context (C-G3-3).
  - **Exit:** the root cause on an NRC SDK update.

## 7. Results

### 7.1 Phase 0 (2026-11-24, `boiling.lab`)

`boiling.lab` ran its 153 runs on 2026-11-24, in NRC + ReSTIR GI, release, layers off, with noise maps in `build/cafe-release/logs/lab`. Shipped means I15's DI (two spatial samples) and GI (age 30). The shipped config ran on 5 seeds, the others on seed 0. Each cell reads temporal std % / flickering px % / lag-1 correlation.

| `cafe.tables` | `Lit` (DLSS-RR output) | raw `Direct lighting` | raw `Indirect radiance` |
|---|---|---|---|
| floor (5 seeds) | 0.25 / 2.6 | 0.20 / 0.07 | 8.8 / 0.01 |
| **shipped** | 6.65 / 32.1 / 0.99 | 7.98 / 94.7 / **0.65** | 469 / 100 / 0.01 |
| `di=reference` (no reuse) | **3.86 / 19.0** / 0.98 | 56.5 / 92.5 / **0.00** | 426 / 100 / 0.01 |
| `di.permutation=on` | 4.74 / 24.7 / 0.99 | 21.5 / 95.8 / 0.04 | 488 / 100 / 0.01 |
| `di.history=4` / `8` | 5.83 / 36.7 · 6.15 / 36.4 | 11.5 / 93.7 / 0.53 · 9.4 / 94.3 / 0.60 | unchanged |
| `di.localSamples=32` | 6.09 / 30.0 | 7.90 / 94.8 / 0.65 | 436 / 100 / 0.01 |
| `gi.decorrelationFactor=1`, `gi.history=4` / `8` | 6.60–6.75 / 32.6–34.2 | unchanged | 455–495 / 100 / 0.00–0.02 |
| `di=reference gi.decorrelationFactor=1` | 3.73 / 19.2 / 0.98 | 58.8 / 92.5 / 0.00 | 476 / 100 / 0.00 |
| `frame.dlssPreset=d` / `e` | 6.58 / 32.0 · 6.77 / 31.8 | unchanged | unchanged |
| ReSTIR GI mode | 6.89 / 34.4 | unchanged | 550 / 100 / 0.01 |

| `Lit` · raw `Direct lighting` | Sponza gallery | Sponza courtyard (sun only) |
|---|---|---|
| shipped | 4.02 / 6.2 · 11.0 / 64.3 / 0.80 | 1.02 / 5.9 · 21.4 / 98.6 / 0.54 |
| `di=reference` | 3.97 / 5.7 · 0.00 (exact) | **0.73** / 6.2 · 0.00 (exact) |
| `di.permutation=on` | 4.16 / 5.7 · 18.5 / 98.0 / 0.12 | 0.86 / 5.8 · 31.1 / 100 / 0.01 |
| `di.history=4` | 3.92 / 5.7 · 7.8 / 25.3 / 0.68 | 0.90 / 5.9 · 17.9 / 94.4 / 0.45 |
| `gi.decorrelationFactor=1` | 3.73 / 5.1 | 1.04 / 6.2 |
| ReSTIR GI mode | 5.31 / 23.9 | 1.00 / 8.7 |

Findings:

1. **The boiling is DI's temporal correlation.** The shipped DI input carries lag-1 0.65 in the café (0.80 gallery, 0.54 courtyard): a pixel's error persists for frames.
   - DI without reuse is 7× noisier per frame (raw std 56 % against 8 %) but uncorrelated (lag-1 0.00). DLSS-RR turns it into the calmest café of all: `Lit` flicker 32 → 19 %, std 6.7 → 3.9 %. This is NoiseAndBias.md's "too stable" signal, measured.
   - Permutation decorrelates too (lag-1 0.04), at 21 % raw std; `Lit` 24.7 %.
   - A shorter DI history keeps the correlation (0.53–0.60) and adds noise: café flicker rises to 36–37 %.
2. **GI is not the boiling.** The raw GI input is already uncorrelated (lag-1 ≈ 0.01) thanks to stagnancy decorrelation. No GI lever moves café `Lit` beyond its floor. NRC + ReSTIR GI is calmer than ReSTIR GI mode on Sponza (gallery flicker 6.2 % against 23.9 %).
3. **The sun proves approach 2.** With spatial reuse, the courtyard's raw sun-only DI reads 21 % std at lag-1 0.54; without reuse it is exact, and `Lit` std drops 1.02 → 0.73 %.
4. **DLSS-RR's model presets change nothing** at a still camera (D / E against the driver default). Approach 3 is a motion item.
5. **Decorrelation alone does not reach the targets.** With DI and GI both uncorrelated, the café keeps 3.7 % std / 19 % flicker. The rest is per-frame variance: raw GI at ~470 % std, raw DI without reuse at 56 %. After Phases 1–2, the café needs a variance item too: better candidates, or more GI samples where NRC allows. It goes in Phase 2b's conditional list.
6. **`Lit`'s lag-1 is 0.98–0.99 in every config:** it measures DLSS-RR's accumulation, not the input. The raw views' lag-1 answered Phase 0's question, so `corrFrames` and `blotchPx` are deferred until a decision needs them.

Consequence for Phase 1: the goal is DI that is decorrelated and low-variance. Either fixed permutation sampling (1a: raw lag-1 0.04 at 21 % std) or stagnancy decorrelation (1b). Shorter DI histories (1c) are closed by finding 1. The proposed targets stand until Phase 1's numbers.

## Done when

- [x] Phase 0: the boiling table (§7.1); `corrFrames` and `blotchPx` deferred (raw-view lag-1 sufficed); target numbers proposed, to be confirmed with Phase 1
- [ ] Phase 1: permutation shipped or its bias root-caused; DI decorrelation built and decided; histories decided, all by the rule with 5-seed numbers and cost
- [x] Phase 2: the sun shaded exactly outside ReSTIR DI (§7.4); 2b run (§7.5): cheap levers ≤ 1 floor, DI no longer limits the café; 2c (§7.6): the café's flicker is edge, metal and glass noise that more samples don't reach, the sun exact at path vertices too
- [ ] Phase 3: specular / diffuse hit distance and reflection motion vectors wired and decided; DLSS check gains its guide-sensitive case
- [ ] Targets met on the café and Sponza poses, or the gap recorded per approach with F28 left open
- [ ] Every runner of §4 passes; NVIDIA's DI and GI sets unchanged and callable; no edits under `ThirdParty/`
- [ ] `docs/settings.md`, `m8_optimization_and_fixes.md` (F28), `considerations.md` and the README updated; I12's re-check list includes I16's decisions

### 7.2 Phase 1a: what darkens permutation sampling (2026-11-24, `i16_permutation.lab`)

Base: I15's B (shipped DI + permutation) on 5 seeds. Each set turns one feature off, one of those the unbiased Converged set turns off. DI bias against RTXDI's reference, bias % / dim-tile bias %:

| config | café tables | café dining | EmissiveStrengthTest | lamp |
|---|---|---|---|---|
| floor | 0.08 / 0.66 | 0.34 / 0.87 | 0.17 / 0.96 | 1.57 / 3.25 |
| shipped + permutation (5 seeds) | −0.47 / −9.70 | −1.87 / −8.15 | −0.27 / −0.93 | −0.74 / −2.83 |
| … boiling filter off | **−0.10 / −1.02** | **−0.30 / +0.00** | −0.03 / −0.88 | +0.33 / +0.31 |
| … visibility shortcut off | −0.50 / −9.79 | −2.22 / −8.31 | −0.22 / −0.89 | −0.96 / −2.34 |
| … fallback off | −0.52 / −10.33 | −1.83 / −9.07 | −0.51 / −1.52 | −1.05 / −1.64 |
| … disocclusion boost 0 | −0.49 / −9.79 | −2.01 / −8.01 | −0.24 / −1.56 | +0.01 / −3.69 |
| … BRDF candidates 0 | −0.66 / −8.48 | −1.87 / −8.40 | −0.52 / −2.43 | −0.10 / −2.27 |
| … spatial samples 0 | −0.26 / −2.99 | −0.31 / −2.51 | −0.41 / −1.66 | −1.70 / −2.04 |
| … material test off | −0.47 / −9.79 | −1.83 / −8.41 | −0.42 / −0.81 | −0.63 / −3.13 |
| … jitter off | −0.45 / −9.46 | −1.45 / −8.76 | −0.25 / −1.33 | −0.74 / −2.77 |
| Converged + permutation | −0.17 / +0.31 | −0.48 / −0.13 | −0.05 / −0.05 | +0.01 / +1.85 |
| Converged | −0.40 / +0.12 | −0.19 / −0.31 | −0.00 / +0.09 | −0.03 / +0.33 |
| shipped (A, no permutation) | −0.24 / −4.69 | −0.34 / −2.58 | −0.01 / +0.16 | −0.08 / −2.48 |

- **Permutation sampling is unbiased:** Converged + permutation reads within the floors.
- **The DI boiling filter darkens.** The filter is RTXDI's, NVIDIA `Medium`'s default, on in the shipped set and off in Converged. It drops reservoirs that are bright against their 8×8 group. Permutation adds per-frame variance, so the filter fires more and darkens more: café dim tiles −9.7 / −8.2 % with it, −1.0 / 0.0 % without. It also causes about half of the shipped A's dim-tile darkening (−4.7 / −2.6 %).
- **Spatial reuse feeds the filter:** reused bright samples spread, and the filter then drops them. Without spatial samples the dim tiles read −3.0 / −2.5 %.
- **The visibility shortcut, fallback, boost, BRDF candidates, material test and jitter are not the cause.** Each moves the dim tiles by less than 2 floors. I6's bridge fix (temporal visibility from `previousSurface`) holds.
- **Candidate:** permutation on, DI boiling filter off. Its `Lit` noise (fireflies the filter removed now reach DLSS-RR), its DI bias on 5 seeds, the 2048-frame convergence and the cost follow (`i16_perm_noboil`, `i16_noboil`).

### 7.3 Phase 1a: permutation without the boiling filter (2026-11-24, `i16_perm_noboil`, `i16_noboil`)

5 seeds each, NRC + ReSTIR GI, shipped GI. `Lit` std % / flicker %; raw `Direct lighting` lag-1; DI bias % (dim tiles %) against RTXDI's reference:

| DI config | café tables `Lit` | café dining `Lit` | gallery / courtyard `Lit` std | raw DI lag-1 tables / gallery / courtyard | DI bias tables / dining | lamp / terminator / emissive |
|---|---|---|---|---|---|---|
| A (I15 shipped) | 6.70 / 32.0 | 6.83 / 36.1 | 3.98 / 1.02 | 0.65 / 0.80 / 0.54 | −0.24 (−4.7) / −0.34 (−2.6) | −0.08 / +0.01 / −0.01 |
| B (A + permutation) | 4.79 / 24.1 | 6.18 / 32.7 | 4.01 / 0.86 | 0.04 / 0.12 / 0.01 | −0.47 (−9.7) / −1.87 (−8.2) | −0.74 / −0.01 / −0.27 |
| **C (B, boiling filter off)** | **5.03 / 27.8** | **6.60 / 34.5** | **4.00 / 0.86** | **0.04 / 0.12 / 0.01** | **−0.17 (−1.8) / −0.04 (+0.4)** | **−0.10 / −0.02 / 0.00** |
| A, boiling filter off | 6.77 / 34.3 | 6.89 / 37.0 | 3.98 / 1.02 | 0.65 / 0.80 / 0.54 | −0.17 (−1.5) / −0.05 (+0.3) | +0.54 / −0.08 / +0.04 |

- **C decorrelates DI as B does** (lag-1 0.01–0.12), with DI bias at the level of the unbiased set. Dim tiles are within ~2 floors of the reference, and the convergence scenes are within their floors.
- **B's lower flicker was partly the filter's darkening:** it dropped the bright samples DLSS-RR would otherwise see. C keeps most of the gain over A: tables flicker −4.2 pts (std −1.7 pts), dining −1.6 pts, courtyard `Lit` std 1.02 → 0.86 %.
- **The filter without permutation only darkens.** Off, A's flicker rises by ~2 pts and its dim tiles lose their darkening. DLSS-RR reads filtered fireflies as signal either way.
- **Shipped:** C in `RtxdiContext::defaultSettings()` (`isPermutationSamplingEnabled = true`, `isBoilingFilterEnabled = false`). `nvidiaSettings()` is unchanged (permutation on, boiling filter on). The 2048-frame convergence of the shipped set and the ABBA cost follow.
- **Still above target:** café `Lit` 5.0 % / 27.8 % against ≤ 2 % / ≤ 5 %. Phase 0's finding 5 applies: the remaining noise is per-frame variance, not correlation. Next: 1b (DI stagnancy decorrelation) only if it lowers variance-driven flicker, and Phase 2a (the sun) for Sponza.
- **Convergence of the shipped set** (guide capture over 2048 frames with `defaultSettings()` in place of the Converged set, a temporary hook, removed):
  - lamp −0.53 % (RGB −0.41 / −0.59 / −0.77 %), tile p90 4.5 %;
  - `Sponza_terminator` −0.05 % / p90 0.5 %;
  - `EmissiveStrengthTest` +0.11 % / p90 1.9 %.

  That is 2 of 3 by the Converged limits (0.5 % / 3 %). The lamp's 5-seed lab spread for C is −1.04…+0.33 % (mean −0.10 %), so −0.53 % is one window inside it. For comparison: A read +0.36 % / p90 14.6 %, B −1.29 % / 7.1 %. The gated check (Converged set) stays 3 of 3, RT 8890 / 8890.
- **Cost** (ABBA ×2, 6000 frames, layers off, C against A): DI temporal 0.054 / 0.057 ms gallery, 0.093 / 0.097 courtyard, 0.041 / 0.045 Bistro street. DI shade 0.099 / 0.096, 0.371 / 0.374, 0.063 / 0.062. GPU total within the runs' noise. Permutation and the missing boiling filter cost nothing measurable.

### 7.4 Phase 2a: directional lights shaded exactly (2026-11-24, `i16_sun.lab`)

**Built:**
- `RtxdiContext::Settings::areInfiniteLightsShadedExactly` → `GpuRtxdiConstants::diFlags` `DiFlag::ExactInfiniteLights`, in the former `pad0`.
- `rtxdi_initial` sets `numInfiniteLightSamples = 0` for the primary surface only. Path vertices (`sampleLightsAt`) keep the sun as RIS candidates.
- `rtxdi_shade` adds every directional light with one shadow ray (`shadeInfiniteLightsExactly`), glass transmittance included. The shared transmittance moved to `glassTransmittance`.

Our directional lights are deltas (`evalLight`: fixed direction, `solidAnglePdf = 0`), so this is exact and needs no random numbers. On in the shipped set, and so in Converged, which derives from it: the gated `Sponza_terminator` shot now checks the exact sun. `nvidiaSettings()` keeps the RTXDI path. Lab switch `di.exactInfinite`; UI checkbox "Exact directional lights". The departure from FullSample, which puts the sun in RTXDI as `kDirectional`, is C-I16-2.

5 seeds (exact, shipped) against seed 0 (`di.exactInfinite=off`), target mode:

| | raw `Direct lighting` std | DI bias (dim) | `Lit` std / flicker |
|---|---|---|---|
| Sponza courtyard | **0.00** / 31.1 % | **0.00** / −0.04 % | **0.74 / 6.3** / 0.86 / 5.9 |
| Sponza gallery | **0.00** / 18.6 % | **0.00** / −0.19 % | 4.00 / 6.1 / 4.07 / 5.8 |
| Bistro street | **0.00** / 9.9 % | **0.00** / −0.01 % | 0.36 / 9.9 / 0.42 / 6.5 (street flicker: 2–35 % seed spread) |
| café tables | 14.5 / 23.2 % | −0.18 (−1.5) / −0.16 (−1.8) % | **4.06 / 20.6** / 4.92 / 25.8 |
| `guide.terminator` | — | **0.00** / +0.05 % | — |

- **Guide capture:**
  - `Sponza_terminator` against `Shadowed lights` reads +0.000 % bias, tile p90 0.00 %: the exact sun is the reference.
  - Normals 2 / 2, convergence 3 / 3, glass 2 / 2, RT 8890 / 8890.
- **The café gains too:** the Bistro sun's RIS candidates no longer compete with the emissive triangles (raw DI 23 → 15 % std, `Lit` flicker 25.8 → 20.6 %).
- **Cost** (ABBA ×2, 6000 frames, layers off, exact / RIS), GPU total:
  - courtyard **3.66 / 4.02 ms** (DI initial 0.026 / 0.174, temporal 0.046 / 0.095, shade 0.253 / 0.411);
  - gallery 3.20 / 3.21;
  - Bistro street 2.92 / 2.95.

  ReSTIR DI has nothing to resample on Sponza now. The −0.36 ms repays I15's spatial-reuse cost on the courtyard.
- **C-I16-4 · Limitation (closed in 2c, §7.6):** O2 secondary DI resampling (off, I12) would mix X₂'s own sun candidates with primary reservoirs that no longer hold the sun. Its bias correction then assumes the neighbors could have drawn it, which darkens the sun at X₂. If O2 ships, X₂ needs the same exact path.

### 7.5 Phase 2b: variance levers in the café (2026-11-24, `i16_variance.lab`)

Shipped (1a + 2a) on 5 seeds; levers on seed 0. Target mode.

| config | tables `Lit` std / flicker | dining `Lit` std / flicker | tables raw DI / GI std | DI bias tables (dim) |
|---|---|---|---|---|
| floor | 0.33 / 4.2 | 0.69 / 3.1 | 1.25 / 12.2 | 0.12 (0.68) |
| **shipped** | **4.03 / 19.0** | **4.71 / 23.3** | 14.5 / 470 | −0.18 (−1.5) |
| `di.localSamples=16` | 3.79 / 17.0 | 4.25 / 21.7 | 13.5 / 445 | −0.13 (−1.1) |
| `di.localSamples=32` | 3.69 / 16.9 | 4.13 / 19.9 | 12.1 / 439 | −0.12 (−1.0) |
| `di.brdfSamples=0` | 4.98 / 24.8 | 6.02 / 30.0 | 25.8 / 476 | −0.27 (−0.3) |
| `di.brdfSamples=2` | 3.93 / 19.1 | 4.36 / 22.0 | 13.7 / 465 | −0.15 (−2.2) |
| `di.spatialSamples=4` | 4.15 / 19.2 | 4.53 / 22.5 | 13.3 / 463 | −0.21 (−2.5) |
| `gi.spatialSamples=2` / `0` | 3.99 / 18.5 · 3.96 / 19.1 | 4.50 / 23.3 · 4.55 / 22.8 | 15.2 / 464 · 14.3 / 475 | −0.10 · −0.16 |
| 32 candidates + DI spatial 4 + GI spatial 2 | 3.68 / 16.4 | 4.22 / 21.2 | 11.7 / 466 | −0.12 (−0.8) |

- **1a and 2a together roughly halved the café's flicker:** tables 32.0 → 19.0 %, dining 36.1 → 23.3 % against I15's DI. Dining gained most from the exact sun.
- **The cheap levers move it by about one floor at best:** 32 local candidates, −2 pts tables, −3 pts dining. One BRDF candidate is needed (0 doubles DI noise); more spatial samples do nothing. Not shipped: one floor, unmeasured cost, and the count is shared with path-vertex NEE (`sampleLightsAt`).
- **DI no longer limits the café.** With DI 4× noisier but uncorrelated (Phase 0, `di=reference`: raw 56 % std), café flicker was the same ~19 %. The remaining noise does not follow DI's level; it is the GI input's per-frame variance (raw ~470 % std, unchanged by every GI reuse setting).
- **The dominant-light split (2b's second part) is not built:** it lowers DI variance, which no longer limits the café.
- **Next (proposed Phase 2c, GI variance in emissive interiors):** the GI sample's own light estimate at X₂ is one RIS sample among 30k emissive triangles. Levers:
  - O2 secondary DI resampling (built, off; needs C-I16-4's exact-sun path at X₂);
  - more path-vertex candidates;
  - more than one GI sample per pixel in the café.

  Each is measured on café `Lit` and the 8-bounce bias, with ABBA cost.

Phase 2 status: 2a shipped; 2b's cheap levers measured, none shipped; café `Lit` 4.0 / 19.0 % and 4.7 / 23.3 % against the proposed ≤ 2 % / ≤ 5 %.

### 7.6 Phase 2c: where the café's flicker comes from (2026-11-24)

- **Plans:** `i16_gi_variance` (levers), `i16_nrc_training`, `i16_cv_maps`, `i16_cv_nojitter`.
- **Mode and data:** target mode; shipped base on 5 seeds, levers on seed 0.
- **Sources:** tables come from the clean café-tables rerun and dining from the clean dining rerun. The first full run broke at its DLAA lever (C-I16-5); its runs before the break agree.

| config | tables `Lit` std / flicker | dining `Lit` std / flicker | raw GI std tables / dining | 8-bounce bias tables / dining |
|---|---|---|---|---|
| **shipped** (5 seeds) | **4.03 / 18.8** (17.2–21.5) | **4.69 / 23.2** | 468 / 383 | −3.6 (−1.7 … −4.2) / −4.1 |
| O2 (`gi.secondaryResampling`) | 3.86 / 16.6 | 4.70 / 23.2 | 368 / 339 | −2.9 / −4.3 |
| exact sun at path vertices | 3.92 / 19.3 | 4.63 / 23.8 | 412 / 364 | −3.9 / −3.5 |
| 32 local candidates | 3.64 / 16.4 | 4.23 / 20.4 | 434 / 357 | −2.1 / −4.1 |
| O2 + 32 local candidates | 3.67 / 15.0 | 4.42 / 21.3 | 336 / 324 | −3.9 / −4.4 |
| firefly clamp | 3.96 / 18.4 | 4.60 / 24.1 | 426 / 340 | −6.3 / −7.2 |
| DLAA | 3.86 / 22.8 | 4.54 / 23.4 | 474 / 387 | −3.7 / −6.5 |
| NRC mode | 3.89 / 17.3 | 4.49 / 22.4 | 538 / 440 | −1.4 / +0.5 |
| ReSTIR GI mode | 4.11 / 20.2 | 4.87 / 24.1 | 550 / 482 | −18.8 / −24.4 (one bounce, as expected) |

Reading guide:
- Seed-0 levers carry the base's seed spread: tables flicker 17.2–21.5 %; the 8-bounce dim-tile bias spreads 0.2–8.8 % on the same config. One run is not a decision.
- The first run's "O2 takes the tables' dim-tile bias from +12.8 to +0.8 %" sits inside that spread and is not claimed.

**Findings:**
1. **GI sample variance does not set the café's flicker.**
   - O2 and the exact sun lower the raw GI std by 21 % and 12 % on the tables (O2 + 32 candidates: 28 %). `Lit` flicker moves by at most one seed spread.
   - In dining, O2 leaves `Lit` unchanged (23.2 → 23.2 %) at −11 % raw GI std.
   - DLAA puts 2.25× the samples into each output pixel and changes nothing.
   - With Phase 0's DI reference (4× DI noise, no correlation) flicker was also ~19 %. Neither the DI nor the GI input's level sets it.
2. **NRC training does not set it either** (`i16_nrc_training`, 3 seeds).
   - The 512-frame countdown covers the first 212 frames of the 480-frame window.
   - Freezing the cache before the window (`nrc.idleTrainingFrames=256`) gives 16.7 / 23.9 %, and training every frame (`0`) gives 18.9 / 24.3 %. The base is 18.2–21.0 / 22.3–25.6 %.
3. **The flicker sits on edges, detail, metal and glass** (`i16_cv_maps`, per-pixel CV over the 480 frames).
   - Edge mask: Sobel on the log mean image, threshold 0.5, dilated 1 px. It covers 64 % (tables) and 52 % (dining) of lit pixels, texture detail included.
   - 77–87 % of the flickering pixels (CV > 10 %) lie on it.
   - Off it, flicker is 5–13 % (tables) and 6–7 % (dining), with a median CV of 2.7–3.9 %.
   - The bright spots of the CV map: the beer taps and glass jars, the window panes and curtain edges against them, and the counter's rim highlight. The walls, floor and bar front stay dark.
4. **It is not jitter aliasing** (`i16_cv_nojitter`, 3 seeds).
   - Without camera jitter, café flicker rises to 26.8 / 25.5 % (std 4.9 / 4.9), with 78–83 % of the flickering pixels still on edges.
   - The edges flicker from lighting noise that DLSS-RR cannot average across a discontinuity. Jitter helps it.
5. **Consequence:** more samples do not get the café to the proposed ≤ 5 % flicker; better guides might.
   - The remaining noise is where DLSS-RR has no neighbors to borrow from and no specular guide (Phase 3: specular hit distance, reflection motion vectors).
   - Phase 3's static-camera measurement now targets this: café `Lit` flicker and the share of flickering pixels on edges, per guide.
   - The `cv_edges` analysis (numpy, scipy) runs on the lab's `*_cv.png` / `*_mean.png`. It belongs in the lab as an `edgeFlicker` metric before Phase 3 decides on it.

**Candidates on 5 seeds** (`i16_gi_o2`, `i16_gi_exact`, `i16_gi_o2_32`; café tables have a sixth run from the plan's `sets` line):

| config | tables `Lit` std / flicker [range] | dining `Lit` | raw GI std tables / dining | 8-bounce bias tables / dining | courtyard 8b bias / dim tiles / p90 | gallery dim tiles |
|---|---|---|---|---|---|---|
| shipped | 4.03 / 18.8 [17.2–21.5] | 4.69 / 23.2 | 468 / 383 | −3.6 / −4.1 | −2.4 / −7.0 / 12.6 | −10.0 |
| O2 | 3.87 / 17.2 [15.7–20.1] | 4.81 / 23.9 | 379 / 355 | −3.3 / −5.3 | −2.4 / −6.9 / 11.7 | −8.8 |
| exact sun at path vertices | 3.89 / 17.4 [16.4–19.3] | 4.67 / 23.6 | 424 / 358 | −4.2 / −4.3 | −2.2 / **−0.7** / **7.0** | −10.6 |
| O2 + 32 local candidates | **3.66 / 15.2** [14.8–15.8] | 4.31 / 21.3 | 332 / 322 | −3.9 / −4.9 | −2.4 / −4.3 / 9.5 | −8.5 |

- **Café noise:**
  - O2 and the exact sun overlap the base's flicker range on the tables, and dining doesn't move.
  - O2 + 32 candidates is the only row outside the range (−3.6 pts tables, −1.9 dining). 32 candidates alone read 16.4 / 20.4 % on one seed, so the gain is DI's candidate count (§7.5), not O2.
  - This confirms findings 1 and 5.
- **Sponza** `Lit` is unchanged by all three (gallery 4.05–4.10 / 6.1–6.5 %, courtyard 0.74 / 6.3–6.4 %).
- **The exact sun at path vertices fixes a GI bias:** the courtyard's dim tiles go from −7.0 to −0.7 % (p90 12.6 → 7.0 %), without changing noise.
  - Taking the sun out of X₂'s RIS removes the sun-versus-emitter selection at X₂. Why the dim tiles gain most is not measured.
- **O2:** with the exact sun it no longer darkens the courtyard (8-bounce −2.4 %, the same as shipped; −23 % in O2's original measurement), but it buys nothing in `Lit`.
- **Cost** of the exact sun at path vertices (ABBA ×2, 6000 frames, layers off, exact / shipped), GPU total:
  - gallery 3.498 / 3.489 ms;
  - courtyard 3.700 / 3.709 ms;
  - Bistro street 2.957 / 2.932 ms.

  `Indirect`, NRC update and train are unchanged. On Sponza the exact ray replaces the one RIS spent on the sun.
- **Decision:**
  - **Shipped:** the exact sun at path vertices (`defaultGiSettings`: `areInfiniteLightsShadedExactly = true`). Its effect is a bias fix in sunlit GI at no cost, not a noise fix. NVIDIA's GI set and the converged truths keep RIS.
  - **Not shipped:**
    - O2: no `Lit` gain;
    - 32 candidates: the §7.5 decision stands;
    - the firefly clamp: biased, −3 pts on the 8-bounce bias;
    - DLAA: no gain.
  - C-I16-4 is closed: O2 takes DI's exact-sun switch at X₂ (§2c).
- **Next:** Phase 3. The café's remaining flicker is on edges, metal and glass (findings 3–5).
