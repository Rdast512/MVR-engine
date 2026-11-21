# Settings: tuning the NVIDIA tech

How to tune the engine's NVIDIA components: RTXDI (ReSTIR DI and ReSTIR GI), NRC, DLSS Ray Reconstruction and RTX Mega Geometry (CLAS, PTLAS). Covers what each setting changes, what it costs, and how to check a change. Every number here was measured on the RTX 5070, release build, validation layers off.

All settings are in the ImGui panels: **Ray tracing** (Mega Geometry, RTXDI, DLSS-RR, and the `Indirect (M4)` section: indirect mode, NRC, ReSTIR GI), **Sun** and **Post**. Settings are not saved; every run starts from the defaults.

---

## 1. Defaults

The defaults are the ones NVIDIA ships in its own samples, so the engine's cost can be compared with them:

| Component | Default taken from | Values |
|---|---|---|
| RTXDI | RTXDI-Library sample (`Samples/FullSample`), its default `Medium` preset | §3 |
| ReSTIR GI | RTXDI-Library defaults, which the sample uses in every preset | §7 |
| NRC | RTXGI `IndirectDiffuse` sample and the SDK defaults | 1 training iteration per frame, training on (§7) |
| DLSS-RR | `vk_denoise_dlssrr` | Quality mode, preset `Default` |
| CLAS, cluster BLAS | `vk_lod_clusters` | fast trace, minimum position truncation |
| PTLAS | `vk_partitioned_tlas` | fast trace |

Deviations, where the engine lacks the feature:
- **ReGIR.** RTXDI's `Medium` preset uses ReGIR for local-light sampling, which needs light presampling (M7). Until then the engine samples local lights uniformly, with the same candidate count (8).
- **ReSTIR GI spatial bias correction.** Ray traced instead of NVIDIA's `Basic`, which darkens occluded places (§7, considerations C-G5-2). `NVIDIA defaults` in the ReSTIR GI section restores the library's set.
- **Indirect mode.** ReSTIR GI mode (one bounce, NRC paused) is the default; NVIDIA's NRC sample corresponds to the `NRC` mode (§7, considerations C-G5-3).
- **Resolution.** The RTXDI sample renders at native resolution, which is DLSS-RR's DLAA mode in the engine. Set the mode to DLAA before comparing images or costs with that sample.

Buttons:
- **Ray tracing panel → Defaults:** resets PTLAS, RTXDI and DLSS-RR.
- **RTXDI section → NVIDIA defaults / Converged:** switch between the two RTXDI sets (§3.2).
- **Sun and Post panels:** each has its own Defaults button.

---

## 2. Why the image is less stable than NVIDIA's demos

NVIDIA's demos render complete lighting. The engine is missing two pieces of it. Each makes noise and flicker easier to see, and neither is a setting:

| Missing | Effect now | Fixed in |
|---|---|---|
| Opacity micromaps for alpha-tested geometry | Rays see the leaf cards of plants as solid quads (every instance is `FORCE_OPAQUE`). The plants' shadows are rectangles, and raster pixels on a leaf get their lighting from the solid quads. Under jitter the leaf edges flip between frames | M6 (D1–D4) |
| Emissive triangles and an environment light | Sponza is lit by one delta sun, so its shadows are perfectly hard. NVIDIA's scenes use area lights, whose soft shadows hide approximations such as visibility reuse (§3.3) | M7 |

Settings still matter a lot: on the same frame they move the share of flickering pixels from 1% to 42% (§3.4).

---

## 3. RTXDI (ReSTIR DI)

The pipeline runs three passes:
1. **Initial:** picks one light per pixel from random candidates (RIS).
2. **Temporal:** merges the pixel's choice with last frame's.
3. **Shade:** merges with nearby pixels (spatial reuse), traces one final visibility ray, and shades.

The result is noisy by design. DLSS-RR turns it into the final image, and RR is tuned for this kind of noise (§4).

### 3.1 Settings

| Setting | What it does | Cost | NVIDIA default |
|---|---|---|---|
| Local / infinite light candidates | RIS candidates per pixel from point/spot lights and from directional lights | Cheap; scales with candidates | 8 / 1 |
| BRDF candidates | Rays along BRDF samples. Until emissive triangles (M7) no ray can find a light | ~0.04 ms, no effect on the image now | 1 |
| Initial visibility | One shadow ray in the initial pass; drops occluded candidates before reuse | 1 ray/px | on |
| Max history length | How many frames of temporal reuse a pixel's reservoir can carry (M cap). Longer is smoother but slower to react | none | 20 |
| Temporal bias correction | `Off`, `Basic` (no rays), `Ray traced` (one ray to check the reused sample) | `Ray traced` +~0.01 ms with the shortcut | `Ray traced` |
| Visibility shortcut | Skips the temporal correction ray for samples already tested at that pixel. In the shade pass it also drops an occluded sample | saves rays | on |
| Fallback sampling | When no history pixel matches, reuses last frame's reservoir at the same pixel. Aimed at disocclusions at the screen edges | none | on |
| Permutation sampling | Takes history from a shuffled neighbor pixel, to decorrelate frames for the denoiser | none | on |
| Spatial samples | Neighbor reservoirs merged per pixel, every frame (even counts) | at 2, the NVIDIA set's shade pass costs +0.06 ms over Converged's (visibility reuse included) | 2 |
| Disocclusion boost samples / boost history length | Extra neighbors while a pixel's history is shorter than the boost length; 0 never boosts | only on disocclusion | 8 / 0 |
| Sampling radius | Neighbor search radius in render pixels | none | 32 |
| Spatial bias correction | `Off`, `Basic`, `Pairwise` (balance against each neighbor, no rays), `Ray traced` (one ray per neighbor) | `Ray traced` adds rays | `Basic` |
| Material similarity, depth / normal thresholds | Rejects neighbors and history on other materials or surfaces | none | on, 0.1 / 0.5 |
| Boiling filter, strength | Suppresses reservoirs far brighter than their 8×8 tile, which would spread as bright blotches | cheap | on, 0.2 |
| Reuse final visibility | Skips the final visibility ray for a sample tested at most 4 frames ago within 16 render px (§3.3) | saves up to 1 ray/px | on |
| Reference (no reuse) | 32 + 8 candidates, no reuse: the unbiased ground truth, very noisy | expensive | off |

### 3.2 The built-in sets

- **Defaults** (`RtxdiContext::defaultSettings`, shipped since 2026-11-21): NVIDIA's set with `Ray traced` spatial correction. Under NVIDIA's `Basic`, the convergence check's Sponza_terminator read −53.6%: `Basic` counts neighbors whose sample is hidden from them. With `Ray traced` the convergence checks read +3.3% there and −2.2% on the lamp; the rest is permutation sampling ([m8 findings](../spec/fixesM1-6/m8_optimization_and_fixes.md) §2.7). The shade pass costs +0.003 ms on the gallery bench.
- **NVIDIA defaults** (`RtxdiContext::nvidiaSettings`): the table's last column (the RTXDI sample's `Medium` preset). ReSTIR DI total ≈ 0.35 ms on the sunlit Sponza bench (853×480 render size).
- **Converged** (`RtxdiContext::convergedSettings`): no spatial reuse except the disocclusion boost (history < 20), Pairwise, `Basic` temporal, no permutation, no BRDF candidate, no visibility reuse. Its raw output converges to the reference (the guide capture's convergence check runs it), but DLSS-RR flickers more on it (§3.4). ≈ 0.24 ms.

### 3.3 Known artifacts and the setting behind each

| You see | Cause | Try |
|---|---|---|
| Shadows boil, worst on small geometry (plants, railings) | No spatial reuse each frame: RR gets too little to work with | Spatial samples 2 |
| Light leaks past hard shadow edges; thin shadows (poles) fade | Reuse final visibility: a neighbor's visibility up to 16 render px away is reused for this pixel | Reuse final visibility off (+1 ray/px) |
| Fine flicker everywhere in lit areas, even on a still camera | Permutation sampling | Permutation sampling off |
| Dark or blotchy patches in `Direct lighting` | Basic spatial or temporal correction across shadow edges, or spatial samples feeding back through history | Pairwise; compare against Reference |
| Lighting appears late in areas entering from screen edges | No history there | Disocclusion boost history length > 0 (e.g. 20); considerations C-M4-1 |
| ReSTIR DI too slow | Rays per pixel | Fewer spatial samples, BRDF candidates 0, Reuse final visibility on, temporal `Basic` |

### 3.4 Measured stability (2026-11-15)

Setup:
- **Scene:** sunlit Sponza courtyard (camera `lookAt({6, 2.5, 1.5}, {-10, 1, -1})`, default sun), static camera, 1280×720, DLSS-RR Quality.
- **Capture:** 120 frames after 300 warm-up frames.
- **Raw ReSTIR change:** the `Direct lighting` view (no jitter), mean frame-to-frame change as a share of the signal.
- **RR columns:** the `Lit` view (DLSS-RR output, jittered). "Temporal std" is each pixel's standard deviation over the frames, as a share of the signal. "Flickering px" is the share of lit pixels whose deviation exceeds 10% of their mean, for the whole frame and the center third.

| Settings | Raw ReSTIR change | RR temporal std | RR flickering px (frame / center) |
|---|---|---|---|
| NVIDIA defaults | 47.1% | 1.36% | 7.5% / 12.6% |
| NVIDIA, permutation off | 20.5% | 1.47% | 7.3% / 10.4% |
| NVIDIA, visibility reuse off | 31.6% | 4.73% | 24.4% / 24.0% |
| NVIDIA, both off | 18.8% | 1.10% | 1.1% / 1.4% |
| **NVIDIA, both off, Pairwise spatial** | 13.4% | **0.94%** | **0.9% / 1.1%** |
| NVIDIA, both off, `Ray traced` spatial | 13.0% | 1.14% | 2.4% / 2.9% |
| NVIDIA, both off, shortcut off | 18.8% | 1.21% | 4.1% / 6.8% |
| NVIDIA, both off, no spatial | 0.0% | 2.47% | 4.7% / 6.2% |
| NVIDIA, no spatial | 49.2% | 4.71% | 41.6% / 37.5% |
| Converged | 0.0% | 2.82% | 7.2% / 9.6% |

- **Spatial reuse is what makes RR stable.** Taking it away raises the flickering pixels 4–5×. A noise-free input does not help: Converged's raw output is bit-stable, yet RR flickers more than on the noisy NVIDIA input.
- **On this scene the most stable set is NVIDIA's minus permutation sampling and visibility reuse, with Pairwise spatial correction.** Its shadows are also correct; visibility reuse leaks light past the hard sun shadow. The cost is the final visibility ray on every pixel, about what Converged's shade pass pays.
- **The BRDF candidate changes nothing** in these numbers: no light can be hit yet.
- **RR is not perfectly still even on a perfect input.** With Converged settings and jitter off, its output still varies 1.6% (1.5% of pixels flicker).

Re-measure after M4 (GI) and M6 (OMM). With soft indirect light the NVIDIA defaults may come out ahead.

---

## 4. DLSS Ray Reconstruction

| Setting | What it does | Notes |
|---|---|---|
| Mode | Render resolution: DLAA (native), Quality (0.67×), Balanced, Performance, Ultra Performance | A change recreates the feature and the render targets, and resets history. The RTXDI sample runs native: use DLAA to compare |
| Preset | Model hint: `Default` (the driver picks), D, E (transformer), F | An NVIDIA app override wins over it (NGX logs a DRS override) |
| Reset history | The next Lit frame starts without accumulated history | Use it to see how fast the image converges |

- RR is evaluated only in the `Lit` view, and only when the scene has lights. Debug views bypass RR and are not jittered.
- RR expects ReSTIR-style noise. It flickers more on a noise-free input (§3.4), so judge RTXDI settings on the `Lit` view, not only on `Direct lighting`.
- RR receives scene-linear RGBA16F color with unity pre-exposure. Exposure is applied in post. Very high sun illuminance can overflow the RGBA16F input (considerations C-H2-1).

---

## 5. RTX Mega Geometry

| Setting | What it does | Notes |
|---|---|---|
| Instances per partition | Grid density of the PTLAS partitions. A moving instance rewrites only its own partition | A change rebuilds the PTLAS. Sponza: 103 instances in 4 partitions, build ≈ 0.02 ms |
| Force full PTLAS build | Rewrites every instance each frame instead of the changed ones only | For cost comparisons only |

CLAS and cluster BLAS are built once at load, with fast trace. They have no runtime settings.

---

## 6. Checking a change

| Check | Command (from `build/windows-clang-release`) | Pass |
|---|---|---|
| Cost | `ENGINE_BENCH_FRAMES=6000 VK_LOADER_LAYERS_DISABLE='*validation*' VK_KHRONOS_VALIDATION_GPUAV_ENABLE=false ./engine.exe` → `[Bench]` lines in `logs/engine.log` | compare spans; noise is ±5%, so alternate variants over 2 rounds |
| ReSTIR port correctness | `ENGINE_GUIDE_CAPTURE=1 ./engine.exe` → `Convergence check: passed` | runs the Converged set, not the current settings |
| Alpha in raster and rays | `ENGINE_GUIDE_CAPTURE=1 ENGINE_RT_CHECK=1 ./engine.exe` → `AlphaBlendModeTest_sun`, `Sponza_plants` and `Sponza_terminator` PNGs in `logs/guides`, `[RayCheck]` lines | `Ray check: N of N frames matched`; `alpha edges` (raster vs OMM next to a cut) are logged, not failures ([d4_ray_contract.md](../spec/m6/d4_ray_contract.md)) |
| Ray contract | any runner from `build/windows-clang-debug` → `[RayContract]` lines | `Ray contract: 0 ray-query candidates over N frames`; Release does not count |
| ReSTIR GI against the ground truth | `ENGINE_GI_CHECK=1 VK_LOADER_LAYERS_DISABLE='*validation*' VK_KHRONOS_VALIDATION_GPUAV_ENABLE=false ./engine.exe` (~3.5 min; `ENGINE_GI_CHECK_SEED=n` for another realization) → `[GiCheck]` lines | `ReSTIR GI convergence: 8 of 8 passed` (G5's phases and ReSTIR GI mode against a one-bounce ground truth); the `MEASURED` lines give the bias of NRC, NRC mode and the NVIDIA GI defaults ([g5_restir_gi.md](../spec/m4/g5_restir_gi.md) §3, [g7_indirect_modes.md](../spec/m4/g7_indirect_modes.md) §3) |
| Stability, light change, screen edges | `ENGINE_STABILITY_CHECK=1`, `ENGINE_LIGHT_CHANGE_CHECK=1` or `ENGINE_SCREEN_EDGE_CHECK=1` with the layer variables of the GI check (~2–3 min each) → `[TemporalCheck]` `MEASURED` lines | `0 failed`; compare the lines with the baseline in [i1_check_runners.md](../spec/fixesM1-6/i1_check_runners.md) §5. Runs repeat byte for byte |
| Indirect modes under switching | `ENGINE_INDIRECT_MODE=cycle ENGINE_BENCH_FRAMES=3000` at a 2560×1369 window | no device loss; a mode id instead of `cycle` pins one mode for per-mode timings |
| Bias of a setting | `Direct lighting` view with the setting, then with Reference on (`ENGINE_RTXDI_REFERENCE=1` or the checkbox) | same brightness after a few seconds |
| Validation | `VK_LOADER_LAYERS_ENABLE='*validation' VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true VK_KHRONOS_VALIDATION_VALIDATE_BEST_PRACTICES=true VK_KHRONOS_VALIDATION_VALIDATE_BEST_PRACTICES_NVIDIA=true VK_KHRONOS_VALIDATION_GPUAV_ENABLE=false VK_KHRONOS_VALIDATION_LOG_FILENAME=<file>` | only NGX's known messages (considerations C-H1-2) |

When judging stability by eye, hold the camera still for a few seconds and watch the `Lit` view at 100% scale. Windows Auto HDR brightens the window in screen captures; the swapchain itself is correct.

---

## 7. ReSTIR GI

`Indirect mode` at the top of the `Indirect (M4)` section of the Ray tracing panel ([g7_indirect_modes.md](../spec/m4/g7_indirect_modes.md)): `ReSTIR GI` (default on both tiers since 2026-11-20: one bounce, NRC paused; GPU total 2.72 ms vs 3.27–3.30 ms at 1280×720, no C-G3-6 host wait), `NRC + ReSTIR GI` (both tiers; NRC reads ~10% bright, considerations C-G5-3), `NRC` (the cache samples without reuse). A switch resets ReSTIR GI and DLSS-RR history; `ENGINE_INDIRECT_MODE` sets it from the environment.

The `ReSTIR GI` part of the `Indirect (M4)` section. Defaults (`RtxdiContext::defaultGiSettings`, since 2026-11-21): the library's, which the RTXDI sample uses unchanged in every preset, with ray-traced spatial bias correction and ReSTIR PT's DLSS-RR profile (RTXDI 3.1: DLSS-RR "prefers temporally independent samples"), ported to GI without its history reduction and swap bound ([m8_optimization_and_fixes.md](../spec/fixesM1-6/m8_optimization_and_fixes.md) O11, §2.10). `NVIDIA defaults` (`nvidiaGiSettings`) restores the library's exactly, plus the sample's secondary roughness floor. Why and what it costs: [m8_optimization_and_fixes.md](../spec/features/m8_optimization_and_fixes.md) §2.5.

| Setting | Default | Converged | Effect |
|---|---|---|---|
| Max history length | 20 (NVIDIA: 8) | 8 | frames of temporal reuse |
| Max sample age | 1024, never (NVIDIA: 30) | 1024 (never) | discards a reused sample this old; reacts to light changes, biases a still frame bright (ReSTIR GI mode with NVIDIA's 30: +7.7% gallery / +1.8% courtyard against one bounce, considerations C-G5-1) |
| Temporal / spatial bias correction | Basic / Ray traced (NVIDIA: Basic / Basic) | Ray traced | spatial Basic darkens occluded places (ReSTIR GI mode −27% gallery / −9% courtyard, C-G5-2); Ray traced adds a ray per merged sample (spatial +0.15 ms at 1280×720); the temporal pass's Basic does not darken |
| Fallback sampling | on | off | reuse last frame's sample at the same pixel when no history matches |
| Spatial samples, radius | 2, 32 px | 2, 32 px | |
| Boiling filter | off (NVIDIA: on, 0.2) | off | drops reservoirs after temporal reuse that are far brighter than their 8×8 group; the DLSS-RR profile replaces it with firefly replacement |
| Decorrelation, factor, stagnancy exponent / EMA | Stagnancy, 0.4, 0.5 / 0.2 (NVIDIA: None) | None | final shading shades the pixel's initial sample instead of the reused one with probability 4 · factor · stagnancy^exponent; stagnancy = min(sample age / 40, 1), smoothed against last frame's 5×5 map. The stored reservoirs keep their history. Raw `Indirect radiance` gets noisier per frame (per-frame RMS 241% → 485% gallery) and less correlated over time; GI shade +0.07 ms |
| Firefly replacement, strength | on, 0.7 (NVIDIA: off) | off | the boiling filter's test on the final reservoir; a detected one is always swapped for the initial sample |
| Bound fresh swaps, swap bound | off, 15× (PT: on) | off | a swap where the final sample is fresh keeps at most 15× its weight. Darkens GI: −3 / −8 pp gallery / courtyard |
| Duplication history reduction, strength | off, 0.5 (PT: on) | off | caps temporal history where many of the 17×17 neighbors shared last frame's sample (one more pass, `rtxdi_gi_duplication`). Darkens GI: −8 / −7 pp |
| Final visibility, final MIS | on, on | on, on | reconnection ray; blend with the pixel's own sample on glossy lobes |
| Secondary DI resampling | off | off | X₂'s light reuses this frame's ReSTIR DI reservoirs where X₂ is on screen (the RTXDI sample's `ShadeSecondarySurfaces`). With one light it only darkens: −23% in the courtyard, as the DI visibility shortcut keeps M for shadowed samples ([m8_optimization_and_fixes.md](../spec/features/m8_optimization_and_fixes.md) §2.6, O2) |
| Firefly clamp | off | off | caps X₂'s luminance at 10 in scene units, matching the RTXDI sample's `c_MaxIndirectRadiance` |
| Secondary roughness floor | on (0.5) | off | X₂ is shaded with roughness at least 0.5 while reuse runs (the RTXDI sample's `minSecondaryRoughness`): reservoirs store X₂'s radiance as non-directional, so sharp glossy lobes at X₂ become outliers that reuse spreads. Biased on glossy X₂; the NRC query keeps the raw roughness ([m8_optimization_and_fixes.md](../spec/fixesM1-6/m8_optimization_and_fixes.md) O9) |

- **Ground-truth paths:** trace 8 bounces after X2 instead of querying NRC, which pauses. With `Reference (no reuse)` on as well, the `Indirect radiance` view is the ground truth, one sample per frame.
- **Cost** (gallery, 1280×720 window (853×480 render, DLSS-RR Quality), 2026-11-20 ABBA): temporal 0.11 ms, shade 0.41–0.42 ms with the shipped defaults of that day (0.26–0.27 ms with NVIDIA's Basic spatial correction). The DLSS-RR profile adds 0.07 ms to the shade (2026-11-21 ABBA, 3 runs each).
- **Bias** against one bounce (ReSTIR GI mode, 2026-11-21): shipped −0.75% gallery / −0.86% courtyard (tile p90 21.4% / 27.4%); without the DLSS-RR profile +7.7% / +1.8% (23.4% / 19.3%). Stability under DLSS-RR has no runner yet (m8 F2): judge `Lit` on screen.

### 7.1 NRC

The NRC controls under `Indirect mode` act only in the NRC modes; ReSTIR GI mode pauses the cache. Defaults from the RTXGI `IndirectDiffuse` sample and the SDK ([g3_nrc.md](../spec/m4/g3_nrc.md)).

| Setting | Default | Effect |
|---|---|---|
| Train NRC | on | off freezes the cache and skips the training paths |
| NRC training iterations | 1 (the sample's; the SDK default is 4) | 16K training records each; the training paths grow with it |

- **Cost** (NRC + ReSTIR GI mode, gallery, 1280×720 window (853×480 render, DLSS-RR Quality), 2026-11-19): update 0.021 ms, train 0.42 ms, resolve 0.03 ms, the query ~0.1 ms inside `Indirect`, plus the C-G3-6 host wait (0.57–0.62 ms frame time).
- **Bias:** NRC reads ~10% bright against the ground truth (considerations C-G5-3); the leads are in [m8_optimization_and_fixes.md](../spec/features/m8_optimization_and_fixes.md) §3.3.

## 8. Components not built yet

OMM (M6) has no settings yet; its defaults will come from the OMM SDK. SER is shelved to post-1.0 (engine spec §16.5 V1, considerations C-G1-3) and has no runtime switch in 1.0.
