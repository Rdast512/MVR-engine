# M8 · I16: Noise that DLSS-RR can denoise (F28)

**Status:** planned 2026-11-24 · **Milestone:** M8 ([README](./README.md))
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

## 3. Phase 3 · Give DLSS-RR every guide (approach 3)

| Input | Source in the engine | Notes |
|---|---|---|
| `pInSpecularHitDistance` | distance from the primary hit to the specular lobe's secondary hit: the ReSTIR GI reconnection hit on glossy lobes, I13's glass reflection ray | R16F, render resolution; vk_denoise_dlssrr feeds it |
| `pInDiffuseHitDistance` | the ReSTIR GI sample's hit distance on the diffuse lobe | R16F |
| `pInMotionVectorsReflections` | reprojection of the reflected point (virtual image of the specular hit) for roughness below a threshold | "superior" to hit distance (vk_denoise_dlssrr README) |
| `pInDisocclusionMask` | the vis buffer's previous-frame test (already computed for the screen-edge check's masks) | optional |

Each input is a frame flag first, measured, then shipped.
- **Static camera:** no `Lit` change expected; the check shows none.
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

## 7. Results

To be filled when built.

## Done when

- [ ] Phase 0: the boiling table, `corrFrames` and `blotchPx` built, target numbers confirmed by the user
- [ ] Phase 1: permutation shipped or its bias root-caused; DI decorrelation built and decided; histories decided, all by the rule with 5-seed numbers and cost
- [ ] Phase 2: the sun decided (outside ReSTIR DI or kept, with numbers); 2b run if the café is still above target
- [ ] Phase 3: specular / diffuse hit distance and reflection motion vectors wired and decided; DLSS check gains its guide-sensitive case
- [ ] Targets met on the café and Sponza poses, or the gap recorded per approach with F28 left open
- [ ] Every runner of §4 passes; NVIDIA's DI and GI sets unchanged and callable; no edits under `ThirdParty/`
- [ ] `docs/settings.md`, `m8_optimization_and_fixes.md` (F28), `considerations.md` and the README updated; I12's re-check list includes I16's decisions
