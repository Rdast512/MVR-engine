# Settings: tuning the NVIDIA tech

How to tune the engine's NVIDIA components: RTXDI (ReSTIR DI and ReSTIR GI), NRC, DLSS Ray Reconstruction and RTX Mega Geometry (CLAS, PTLAS). Covers what each setting changes, what it costs, and how to check a change. Every number here was measured on the RTX 5070, release build, validation layers off.

All settings are in the **Settings** window (top left, open with the UI), which copies the RTXDI FullSample's layout ([nvidia_settings_layout.md](../spec/features/planned/nvidia_settings_layout.md)): **Scene** (glass; its `Engine` node holds the sun), **General Rendering** (pixel jitter; `Engine`: RTX Mega Geometry), **Static ReSTIR Context Settings**, **Direct Lighting** (ReSTIR DI), **Indirect Lighting** (indirect mode, NRC, ReSTIR GI), **Denoising** (denoiser mode, DLSS-RR) and **Post-Processing** (Accumulation, exposure). Labels, ranges and help text are NVIDIA's; rows the engine fixes or lacks are drawn disabled with the engine's value, and each section's `Engine` node holds the engine's own rows. A row's (?) marker gives NVIDIA's help, then the engine's notes after `Engine:`. Settings are not saved; every run starts from the defaults.

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

Since 2026-11-25 the engine ships NVIDIA's sets unchanged (`Medium` DI, library ReSTIR GI, `NRC` indirect mode, DLSS-RR) while **M8** ([spec/m8/README.md](../spec/m8/README.md)) audits the pipeline and UI and transitions the engine to composable runtime toggles covering RTXPT, RTXDI, and RTXGI in one renderer. The **1.0 target lighting stack** (M8.3–M8.5, [engine_spec.md](./engine_spec.md) §11.2–11.7) is:
- **Denoiser + Upscaler:** DLSS-RR (toggleable to `Off` + `Accumulation`);
- **Direct Lighting:** RTXDI — either **Streaming RIS** (Initial Sampling only) or **ReSTIR DI** with **Temporal ON (`Max History = 4–8`), `Basic` bias correction, and Spatial OFF** (full ReSTIR DI modes/presets and `Off` selectable as toggles);
- **Indirect Lighting (Bounce 1):** **raw uncorrelated 1-bounce PT** (1 probabilistic diffuse-or-specular ray/pixel, OMM enabled, SER when Slang adds it, `ReGIR` or simple/`Power_RIS` NEE for bounce lights; ReSTIR GI and `Off` selectable as toggles);
- **Multi-Bounce Tail:** **NRC** integrated into the pipeline, with a runtime toggle to **SHaRC** (and `Off` for 1-bounce only).

The prior `fixesM1-6` tuning stays callable as `Tuned`; its deviations from NVIDIA's, named by their rows in the Settings window:
- **ReSTIR DI (`RtxdiContext::tunedSettings`, Direct Lighting → Preset `Tuned`).** Uses `Local Light Power RIS` instead of `Local Light ReGIR RIS` (removes Bistro's async presample stall and the ReGIR/BRDF PDF mismatch), fresh final visibility (`Reuse Final Visibility` off, removing shadow-edge light leak), directional lights (the sun) shaded exactly (`Engine` → `Exact directional lights`) with one shadow ray outside the RIS candidates (M8 I16 phase 2a: spatial reuse had made the delta sun noisy; −0.36 ms on the courtyard), `Enable Permutation Sampling` without the `Boiling Filter` (M8 I16: permutation decorrelates what DLSS-RR sees; the filter dropped the extra variance and darkened dim tiles by 8–10%), `Spatial Samples = 2` (M8 I15: without spatial reuse the café's DI turns to confetti under DLSS-RR), `Max History Length = 4` (M8 I16: at 20 a lucky sample lives on for up to 20 frames, which DLSS-RR shows as local light pops), `Engine` → `Per-pixel light tiles` with `Local Light Power RIS Samples = 32` (M8 I16 §7.9: a 16×16 tile's shared RIS tile made its lucky draws pop as a block; +0.54 ms on Bistro street) and ray-traced disocclusion boost (`Disocclusion Boost Samples = 8` while `M < 4`, `Engine` → `Boost history length`). Preset `Medium` (shipped) is NVIDIA's start state; `Fast`, `Unbiased`, `Ultra` and `Reference` are NVIDIA's other presets (Fast without its checkerboard rendering) ([i6_restir_di_tuning.md](../spec/fixesM1-6/i6_restir_di_tuning.md)).
- **ReSTIR GI (`RtxdiContext::tunedGiSettings`, Indirect Lighting → Engine → `Tuned`).** Ray-traced spatial bias correction (`1` sample, radius `32`) instead of NVIDIA's `Basic` (which darkens occluded places, §7, C-G5-2), plus ReSTIR PT's DLSS-RR decorrelation and full-tile firefly replacement profile without history reduction or swap bound (`Engine` → `ReSTIR PT DLSS-RR profile`, [i5_restir_gi_tuning.md](../spec/fixesM1-6/i5_restir_gi_tuning.md)). `Defaults` there restores the library's set, which ships.
- **Indirect Lighting Mode.** `NRC` ships (NVIDIA's NRC sample); `NRC + ReSTIR GI` is the tuning target the `Tuned` sets were measured in, and `ReSTIR GI` is the RTXDI sample's mode (§7).
- **Resolution.** The RTXDI sample renders at native resolution, which is DLSS-RR's DLAA mode in the engine (Denoising → DLSS-RR → `Mode`). Set the mode to DLAA before comparing images or costs with that sample, or set `Denoiser mode` to `Off`, which renders at the display resolution and shows the raw estimate after exposure and tonemap, as the sample does without a denoiser.
- **Denoiser compatibility preset.** On a denoiser switch (and after a DI preset with DLSS-RR) the ReSTIR GI fields that depend on the denoiser follow it, as with NVIDIA's `Auto-apply denoiser compatibility preset`: `Off` takes ReSTIR PT's non-DLSS-RR profile, `DLSS-RR` the shipped values.

Buttons:
- **Settings → Defaults** (bottom of the window): resets PTLAS, RTXDI, DLSS-RR, NRC, the indirect mode, the denoiser, Accumulation and the glass settings.
- **Direct Lighting → Preset:** NVIDIA's presets (`Medium` ships), then `Tuned` and `Converged` (§3.2); it shows `(Custom)` when the settings match none.
- **Indirect Lighting → Engine → Defaults / Tuned / Converged:** the ReSTIR GI sets (§7).
- **Scene → Engine (sun) and Post-Processing → Engine (exposure):** each has its own Defaults button.

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
| Local Light … Samples / Initial Infinite Light Samples | RIS candidates per pixel from point/spot lights and from directional lights | Cheap; scales with candidates | 8 / 1 |
| Initial BRDF Samples | Rays along BRDF samples. Until emissive triangles (M7) no ray can find a light | ~0.04 ms, no effect on the image now | 1 |
| Enable Initial Visibility | One shadow ray in the initial pass; drops occluded candidates before reuse | 1 ray/px | on |
| Max History Length | How many frames of temporal reuse a pixel's reservoir can carry (M cap). Longer is smoother but slower to react | none | 20 |
| Temporal Bias Correction | `Off`, `Basic` (no rays), `Raytraced` (one ray to check the reused sample) | `Ray traced` +~0.01 ms with the shortcut | `Ray traced` |
| Discard Invisible Samples | Skips the temporal correction ray for samples already tested at that pixel. In the shade pass it also drops an occluded sample | saves rays | on |
| Enable Fallback Sampling | When no history pixel matches, reuses last frame's reservoir at the same pixel. Aimed at disocclusions at the screen edges | none | on |
| Enable Permutation Sampling | Takes history from a shuffled neighbor pixel, to decorrelate frames for the denoiser | none | on |
| Spatial Samples | Neighbor reservoirs merged per pixel, every frame (even counts) | at 2, the NVIDIA set's shade pass costs +0.06 ms over Converged's (visibility reuse included) | 2 |
| Disocclusion Boost Samples / `Engine` → Boost history length | Extra neighbors while a pixel's history is shorter than the boost length; 0 never boosts | only on disocclusion | 8 / 0 |
| Spatial Sampling Radius | Neighbor search radius in render pixels | none | 32 |
| Spatial Bias Correction | `Off`, `Basic`, `Pairwise` (balance against each neighbor, no rays), `Ray traced` (one ray per neighbor) | `Ray traced` adds rays | `Basic` |
| `Engine` → Material similarity, Temporal / Spatial Depth and Normal Threshold (Show Advanced Settings; one pair serves both passes) | Rejects neighbors and history on other materials or surfaces | none | on, 0.1 / 0.5 |
| Boiling Filter (checkbox, strength) | Suppresses reservoirs far brighter than their 8×8 tile, which would spread as bright blotches | cheap | on, 0.2 |
| Reuse Final Visibility | Skips the final visibility ray for a sample tested at most 4 frames ago within 16 render px (§3.3) | saves up to 1 ray/px | on |
| Resampling Mode `None` | 32 + 8 candidates, no reuse: the unbiased ground truth, very noisy | expensive | off |

### 3.2 The built-in sets

- **Tuned** (Preset `Tuned`, `RtxdiContext::tunedSettings`; shipped from M8 I6, 2026-11-21, to 2026-11-25): `Power_RIS` local-light sampling (avoids ReGIR's async presample stall on Bistro and its PDF mismatch with BRDF candidates), fresh final visibility (`Reuse final visibility` off, removing shadow-edge light leak and activating the temporal visibility shortcut every frame), no temporal permutation, and `Spatial samples = 0` with ray-traced disocclusion boost (`8` samples while `M < 4`, fallback on). Passes all three DI convergence checks at shipped defaults (`+0.084% / −0.000% / +0.077%`, 0 dark leak) while cutting Sponza DI time −36.9% gallery / −40.3% courtyard and improving courtyard `Lit` stability (`0.99%` std / `0.06%` swing, [i6_restir_di_tuning.md](../spec/fixesM1-6/i6_restir_di_tuning.md)). **M8 I15 (2026-11-24) set `Spatial samples = 2`** ([i15_cafe_di_gi_settings.md](../spec/fixesM1-6/i15_cafe_di_gi_settings.md)): in Bistro's emissive-lit café, DI without spatial reuse is sticky per pixel and DLSS-RR keeps it as confetti (F22). Two spatial samples cut café `Lit` flicker from 79% / 70% to 33% / 37% (std 13.0% / 11.3% → 6.9% / 7.0%, 5 seeds), leave Sponza and the Bistro street within their noise, and cost `+0.06 ms` (gallery), `+0.25 ms` (courtyard) and `+0.03 ms` (Bistro street) of `ReSTIR DI shade`. Permutation on top would cut the tables' flicker further (25%) but darkens DI (café dim tiles −8…−10%, lamp convergence shot −1.3%), so I15 left it off. **M8 I16 (2026-11-24) turned permutation on and the DI boiling filter off** ([i16_denoiser_friendly_noise.md](../spec/fixesM1-6/i16_denoiser_friendly_noise.md) §7.2–7.3): the darkening was the boiling filter, which drops the bright samples permutation's extra variance produces; permutation over the Converged set is unbiased. In NRC + ReSTIR GI, the raw DI input's lag-1 correlation falls from 0.65 to 0.04 in the café, café `Lit` flicker from 32% to 28%, courtyard `Lit` std from 1.02% to 0.86%, at the unbiased set's DI bias and no measurable cost. **M8 I16 (2026-11-24) set `Max history length = 4`** (§7.8 there): with permutation a lucky sample walks between neighbors for up to 20 frames, and DLSS-RR shows it as a patch that lights up for ~18 frames ("pops"). At 4, café pops are 38% fewer and 40% dimmer, café `Lit` std falls 13% / 21%, Bistro street `Lit` flicker 27% → 6%, Sponza is unchanged and DI bias is unchanged (5 seeds). `Converged` keeps NVIDIA's 20: it converges through history, not through the denoiser. **M8 I16 (2026-11-25) set per-pixel light tiles and `Local light samples = 32`** (§7.9 there): RTXDI shares one RIS tile per 16×16 screen tile, so a tile's lucky draw lit all its pixels at once; per pixel, the 32 candidates are independent across pixels. Café `Lit` std 3.36% / 3.93% → 2.76% / 2.95%, dining pops +56% → +27%, café DI p90 error 7.2% / 4.7% → 6.0% / 4.2%, Sponza and Bistro street unchanged (5 seeds), `+0.54 ms` on Bistro street (Sponza's DI is the exact sun: no change). `Converged` inherits both. ReGIR with the same pair is quieter but leaks light around the café's sconces (DI dim bias +3.4–5.5%), so it stays off.
- **NVIDIA** (Presets `Fast`, `Medium`, `Unbiased`, `Ultra`, `Reference`, `RtxdiContext::nvidiaPreset`; `nvidiaSettings` and, since 2026-11-25, `defaultSettings` are `Medium`): the table's last column is `Medium` ( `ReGIR_RIS`, `Basic` spatial, permutation on, reuse final visibility on).
- **Converged** (Preset `Converged`, `RtxdiContext::convergedSettings`): `Power_RIS`, ray-traced temporal/spatial bias correction, no visibility shortcut, no fallback, no permutation, no spatial reuse (`0`, boost `0`), no boiling filter, no BRDF candidate, no visibility reuse. The guide capture's convergence check runs it.

### 3.3 Known artifacts and the setting behind each

| You see | Cause | Try |
|---|---|---|
| Shadows boil, worst on small geometry (plants, railings) | No spatial reuse each frame: RR gets too little to work with | Spatial Samples 2 |
| Light leaks past hard shadow edges; thin shadows (poles) fade | Reuse Final Visibility: a neighbor's visibility up to 16 render px away is reused for this pixel | Reuse Final Visibility off (+1 ray/px) |
| Fine flicker everywhere in lit areas, even on a still camera | Enable Permutation Sampling | Enable Permutation Sampling off |
| Dark or blotchy patches in `Direct lighting` | Basic spatial or temporal correction across shadow edges, or spatial samples feeding back through history | Pairwise; compare against Resampling Mode `None` |
| Lighting appears late in areas entering from screen edges | No history there | `Engine` → Boost history length > 0 (e.g. 20); considerations C-M4-1 |
| ReSTIR DI too slow | Rays per pixel | Fewer Spatial Samples, Initial BRDF Samples 0, Reuse Final Visibility on, temporal `Basic` |

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

Re-measured after M4 (GI) and M6 (OMM) in M8 I6 (2026-11-21, [i6_restir_di_tuning.md](../spec/fixesM1-6/i6_restir_di_tuning.md)): with soft GI and OMM leaf shadows, `spatialSamples = 0` + `disocclusionHistoryLength = 4` + no permutation + fresh final visibility beats NVIDIA `Medium` on courtyard `Lit` (`0.99%` std / `0.06%` swing vs `1.32%` / `0.13%`), passes all three DI convergence checks, and cuts Sponza DI time by 37–40%. `Pairwise` spatial was closed due to severe darkening bias (−56% on `Sponza_terminator`) under `enableVisibilityShortcut`.

---

## 4. DLSS Ray Reconstruction

Denoising → `DLSS-RR` node, active while `Denoiser mode` is `DLSS-RR` (the default).

| Setting | What it does | Notes |
|---|---|---|
| Mode | Render resolution: DLAA (native), Quality (0.67×), Balanced, Performance, Ultra Performance | A change recreates the feature and the render targets, and resets history. The RTXDI sample runs native: use DLAA to compare |
| Preset | Model hint: `Default` (the driver picks), D, E (transformer), F | An NVIDIA app override wins over it (NGX logs a DRS override) |
| Specular hit distance guide | Off. Feeds `pInSpecularHitDistance`: the primary bounce's hit distance when it took the specular lobe, as reference/vk_gltf_renderer does (debug view "Specular hit distance") | No measured effect on a still camera or on screen-edge settle (M8 I16 §7.7); kept for a motion metric that sees reflections |
| Reset history | The next Lit frame starts without accumulated history | Use it to see how fast the image converges |

- RR is evaluated only in the `Lit` view, and only when the scene has lights. Debug views bypass RR and are not jittered.
- RR expects ReSTIR-style noise. It flickers more on a noise-free input (§3.4), so judge RTXDI settings on the `Lit` view, not only on `Direct lighting`.
- RR receives scene-linear RGBA16F color with unity pre-exposure. Exposure is applied in post. Very high sun illuminance can overflow the RGBA16F input (considerations C-H2-1).

---

## 5. RTX Mega Geometry

General Rendering → `Engine` node.

| Setting | What it does | Notes |
|---|---|---|
| Instances per partition | Grid density of the PTLAS partitions. A moving instance rewrites only its own partition | A change rebuilds the PTLAS. Sponza: 103 instances in 4 partitions, build ≈ 0.02 ms |
| Force full PTLAS build | Rewrites every instance each frame instead of the changed ones only | For cost comparisons only |

CLAS and cluster BLAS are built once at load, with fast trace. They have no runtime settings.

---

## 6. Checking a change

The checks, the bench, the settings lab and `ENGINE_CONFIG` live in `engine_checks.dll`, built only with `-DENGINE_BUILD_CHECKS=ON`: use the `windows-clang-release-checks` preset (`build/windows-clang-release-checks`). A default build ignores their variables and logs that once ([checks_dll.md](../spec/features/planned/checks_dll.md)).

| Check | Command (from `build/windows-clang-release-checks`) | Pass |
|---|---|---|
| Cost | `ENGINE_BENCH_FRAMES=6000 VK_LOADER_LAYERS_DISABLE='*validation*' VK_KHRONOS_VALIDATION_GPUAV_ENABLE=false ./engine.exe` → `[Bench]` lines in `logs/engine.log` | compare spans; noise is ±5%, so alternate variants over 2 rounds. `GPU total` sums the work scopes only (no ray start wait, acquire wait or self-tests since 2026-11-27); `GPU span` adds the ray start wait back and matches the total logged before |
| Bench scene selectors | with `ENGINE_BENCH_FRAMES`: `ENGINE_BENCH_FIELD=n` (n×n instance field), `ENGINE_BENCH_FIELD_MOVE=1` (field rows move across the PTLAS partitions every frame), `ENGINE_BENCH_MODEL=path` (startup model), `ENGINE_BENCH_POSE=courtyard` (the GI check's sunlit pose), `ENGINE_BENCH_POSE='street windows'` / `'bar shelf'` (Bistro's glass poses under their sun) or `'cafe tables'` / `'cafe dining'` (the café interior, default sun), the Bistro poses with `ENGINE_MODEL=models/bistro/bistro.gltf`. Without a pose Bistro starts outside the café looking across the scene, which understates its cost (M8.5 §1.1); an unknown pose logs an error. Any run: `ENGINE_MODEL=path` (startup model, relative to the repository root). Any run: `ENGINE_DI_SAMPLING=regir\|power_ris\|uniform` (ReSTIR DI local light sampling), `ENGINE_PTLAS_REASSIGN=0` (keep layout-time partitions), `ENGINE_PTLAS_FULL_BUILD=1` (write every instance every frame) | `[Bench]` also logs async light prep, the CPU frame split and the last frame's async overlap |
| ReSTIR port correctness | `ENGINE_GUIDE_CAPTURE=1 ./engine.exe` (~86 s) → `Convergence check: passed` | runs the Converged set, not the current settings, over 2048 frames per check (M8 I14) |
| Alpha in raster and rays | `ENGINE_GUIDE_CAPTURE=1 ENGINE_RT_CHECK=1 ./engine.exe` → `AlphaBlendModeTest_sun`, `Sponza_plants` and `Sponza_terminator` PNGs in `logs/guides`, `[RayCheck]` lines | `Ray check: N of N frames matched`; `alpha edges` (raster vs OMM next to a cut) are logged, not failures ([d4_ray_contract.md](../spec/m6/d4_ray_contract.md)) |
| Ray contract | any runner from `build/windows-clang-debug` → `[RayContract]` lines | `Ray contract: 0 ray-query candidates over N frames`; Release does not count |
| ReSTIR GI against the ground truth | `ENGINE_GI_CHECK=1 VK_LOADER_LAYERS_DISABLE='*validation*' VK_KHRONOS_VALIDATION_GPUAV_ENABLE=false ./engine.exe` (~3.5 min; `ENGINE_GI_CHECK_SEED=n` for another realization) → `[GiCheck]` lines | `ReSTIR GI convergence: 8 of 8 passed` (G5's phases and ReSTIR GI mode against a one-bounce ground truth); the `MEASURED` lines give the bias of NRC, NRC mode and the NVIDIA GI defaults ([g5_restir_gi.md](../spec/m4/g5_restir_gi.md) §3, [g7_indirect_modes.md](../spec/m4/g7_indirect_modes.md) §3) |
| Stability, light change, screen edges | `ENGINE_STABILITY_CHECK=1`, `ENGINE_LIGHT_CHANGE_CHECK=1` or `ENGINE_SCREEN_EDGE_CHECK=1` with the layer variables of the GI check (~2–3 min each) → `[TemporalCheck]` `MEASURED` lines | `0 failed`; compare the lines with the baseline in [i1_check_runners.md](../spec/fixesM1-6/i1_check_runners.md) §5. Runs repeat byte for byte |
| Bistro café interior | `ENGINE_CHECK_SCENE=bistro_cafe` with `ENGINE_GI_CHECK=1` or `ENGINE_STABILITY_CHECK=1` (light change and screen edge too) and the layer variables (GI ~6 min, stability ~10 min) → the same `MEASURED` lines on two café poses | measured, not gated. Stability adds `Direct lighting` per DI set against RTXDI's reference (bias, dark leak, dim tiles), the DI sets and the NRC modes through DLSS-RR; compare with F22 / F23 in [m8_optimization_and_fixes.md](../spec/fixesM1-6/m8_optimization_and_fixes.md). `ENGINE_CHECK_STEPS='a\|b'` keeps the stability steps whose label contains `a` or `b`; `ENGINE_CHECK_PNG=1` writes frame, mean, noise and error maps to `logs/checks`. Café runs repeat to ~0.2% (Bistro's async loads), not byte for byte |
| Glass (M8 I13) | `ENGINE_GUIDE_CAPTURE=1` → `Glass throughput checks: 2 of 2 passed`; `ENGINE_CHECK_SCENE=bistro` with `ENGINE_STABILITY_CHECK=1` or `ENGINE_SCREEN_EDGE_CHECK=1` (~3 min each) → `MEASURED` lines per glass state (shipped, pass off, shadow transmittance, blended guides) on the street-windows and bar-shelf poses | throughput: median ≤ 1%, p90 ≤ 3%, no pixel without glass changed; the Bistro lines are measured, compare with [i13_glass_transmission.md](../spec/fixesM1-6/i13_glass_transmission.md) §8 |
| DLSS-RR history trust (M8 I14) | `ENGINE_DLSS_CHECK=1` with the layer variables of the GI check, on Sponza and with `ENGINE_CHECK_SCENE=bistro_cafe` → `[DlssCheck]` lines, `DLSS-RR check: N steps, 0 failed` | analytic clean input: static `Lit` std < 0.5 %, flicker < 1 %, inverted jitter separates; the normal, roughness, depth and Y-flip controls are measured only (C-I14-7) |
| Bistro textured emission (M8 I14) | `ENGINE_CHECK_SCENE=bistro_cafe ENGINE_EMISSIVE_CHECK=1` → `[TemporalCheck]` `MEASURED` lines (Power_RIS DI against BRDF-only exact emission); `ENGINE_LIGHT_PDF_STATS=1` → one `[LightPdfStats]` dump of the light PDF share per material | measured; compare with [i14_bistro_sampling_and_rays.md](../spec/fixesM1-6/i14_bistro_sampling_and_rays.md) §8.2 |
| DGC vs plain indirect | default: no DGC command; `ENGINE_DGC=1` with any run or the bench takes the DGC path. Without DGC the raster draws each (phase, bin) with `vkCmdDrawMeshTasksIndirectCountEXT` over the same sequences, the resolve dispatches each material bin with `vkCmdDispatchIndirect`. Startup only; the startup log and `[Bench]` name the command path ([dgc_toggle.md](../spec/features/planned/dgc_toggle.md)) | bench both values alternately over 2 rounds; the image must not change (`ENGINE_STABILITY_CHECK` lines identical) |
| Indirect modes under switching | `ENGINE_INDIRECT_MODE=cycle ENGINE_BENCH_FRAMES=3000` at a 2560×1369 window | no device loss; a mode id instead of `cycle` pins one mode for per-mode timings |
| Bias of a setting | `Direct lighting` view with the setting, then with Direct Lighting → Resampling Mode `None` (`ENGINE_RTXDI_REFERENCE=1`) | same brightness after a few seconds |
| Converged image of a setting | Denoising → `Denoiser mode` `Off`, Post-Processing → `Accumulation`, camera still (the stats overlay shows `Accumulating N`) | compare against the same with the other setting |
| Validation | `VK_LOADER_LAYERS_ENABLE='*validation' VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true VK_KHRONOS_VALIDATION_VALIDATE_BEST_PRACTICES=true VK_KHRONOS_VALIDATION_VALIDATE_BEST_PRACTICES_NVIDIA=true VK_KHRONOS_VALIDATION_GPUAV_ENABLE=false VK_KHRONOS_VALIDATION_LOG_FILENAME=<file>` | only NGX's known messages (considerations C-H1-2) |

**Accumulation** (Post-Processing, with the denoiser `Off`; NVIDIA's AA mode of the same name): averages the linear `noisyColor` over still frames before exposure, so the image converges to what the settings estimate, bias included, without DLSS-RR's reconstruction. `Accum. Frame Limit` caps the count (0 = no limit; at the limit the mean becomes an exponential average of that length). Any setting edit, a camera, sun, light-set or scene change, or a history reset starts over. Lab switches: `frame.denoiser`, `frame.aa`, `frame.accumulationFrames`.

**Compare with** (top of the Settings window): rows whose lab switch differs from the chosen configuration (`Shipped`, NVIDIA's presets with their ReSTIR GI defaults, ReSTIR GI mode and DLAA, or `Converged`) are drawn in orange, their (?) starts with that configuration's value, and each section and node counts its differing rows after its label (`Direct Lighting · 3`), so a closed section still shows where the difference is. With `Shipped`, an untouched engine highlights nothing.

When judging stability by eye, hold the camera still for a few seconds and watch the `Lit` view at 100% scale. Windows Auto HDR brightens the window in screen captures; the swapchain itself is correct.

---

## 7. ReSTIR GI

`Indirect Lighting Mode` at the top of the Indirect Lighting section ([g7_indirect_modes.md](../spec/m4/g7_indirect_modes.md)): `ReSTIR GI` (default 2026-11-20 – 11-24: one bounce, NRC paused; no C-G3-6 host wait), `NRC + ReSTIR GI` (default 2026-11-24 – 11-25, and the tuning target: DI and GI are tuned for its `Lit` noise; gated in `ENGINE_GI_CHECK=1` since M8 I7, [i7_nrc_bias.md](../spec/fixesM1-6/i7_nrc_bias.md)), `NRC` (**shipped since 2026-11-25**, NVIDIA's NRC sample: the cache samples without reuse; gated since M8 I7). A switch resets ReSTIR GI and DLSS-RR history; `ENGINE_INDIRECT_MODE` sets it from the environment.

The resampling rows under Indirect Lighting (`Temporal Resampling`, `Spatial Resampling`; the DLSS-RR profile and the engine's rows in its `Engine` node). Shipped since 2026-11-25: the library's (`defaultGiSettings` is `nvidiaGiSettings`), as the RTXDI sample runs with DLSS-RR. The table's Default column is the tuned set (`RtxdiContext::tunedGiSettings`, shipped 2026-11-21 – 11-25, which the GI check gates): the library's, which the RTXDI sample uses unchanged in every preset, with ray-traced spatial bias correction and ReSTIR PT's DLSS-RR profile (RTXDI 3.1: DLSS-RR "prefers temporally independent samples"), ported to GI without its history reduction and swap bound and, since M8 I15, with NVIDIA's 30-frame sample age ([m8_optimization_and_fixes.md](../spec/fixesM1-6/m8_optimization_and_fixes.md) O11, §2.10). `NVIDIA defaults` (`nvidiaGiSettings`) restores the library's exactly, plus the sample's secondary roughness floor. Why and what it costs: [m8_optimization_and_fixes.md](../spec/features/m8_optimization_and_fixes.md) §2.5.

| Setting | Default | Converged | Effect |
|---|---|---|---|
| Max history length | 20 (NVIDIA: 8) | 8 | frames of temporal reuse |
| Max reservoir age | 30 since M8 I15 (NVIDIA: 30; ReSTIR PT's DLSS-RR profile: off) | 1024 (never) | discards a reused sample this old (and turns off the spatial age reset); reacts to light changes. Before M4 it biased a still frame bright (+7.7% gallery / +1.8% courtyard, C-G5-1); over I15's DI it reads −0.3% gallery / −1.8% courtyard and cuts the café's dim-tile excess from +31% to +11% (F23), with gallery `Lit` flicker 39% → 21% ([i15_cafe_di_gi_settings.md](../spec/fixesM1-6/i15_cafe_di_gi_settings.md)) |
| Temporal / spatial bias correction | Basic / Ray traced (NVIDIA: Basic / Basic) | Ray traced | spatial Basic darkens occluded places (ReSTIR GI mode −27% gallery / −9% courtyard, C-G5-2); Ray traced adds a ray per merged sample (spatial +0.15 ms at 1280×720); the temporal pass's Basic does not darken |
| Enable fallback sampling | on | off | reuse last frame's sample at the same pixel when no history matches |
| Num spatial samples, Sampling Radius | 2, 32 px | 2, 32 px | |
| Boiling Filter | off (NVIDIA: on, 0.2) | off | drops reservoirs after temporal reuse that are far brighter than their 8×8 group; the DLSS-RR profile replaces it with firefly replacement |
| Decorrelation mode, factor, stagnancy exponent / EMA factor | Stagnancy, 0.4, 0.5 / 0.2 (NVIDIA: None) | None | final shading shades the pixel's initial sample instead of the reused one with probability 4 · factor · stagnancy^exponent; stagnancy = min(sample age / 40, 1), smoothed against last frame's 5×5 map. The stored reservoirs keep their history. Raw `Indirect radiance` gets noisier per frame (per-frame RMS 241% → 485% gallery) and less correlated over time; GI shade +0.07 ms |
| Decorrelation firefly replacement, firefly filter strength | on, 0.7 (NVIDIA: off) | off | the boiling filter's test on the final reservoir; a detected one is always swapped for the initial sample |
| Full-tile firefly test | on (NVIDIA: off) | off | averages the firefly test's 8×8 tile weight over all in-bounds lanes instead of nonzero lanes only (M8 I2 F15), so isolated fireflies in dark tiles are still caught; temporal boiling keeps NVIDIA's nonzero-lane filter |
| Firefly replacement bias reduction, multiplier bound | off, 15× (PT: on) | off | a swap where the final sample is fresh keeps at most 15× its weight (gated on `weightSum > 0` since I2 F14). Darkens GI: −5.6 / −2.3 pp gallery / courtyard |
| Duplication based history reduction, History reduction strength | off, 0.5 (PT: on) | off | caps temporal history where many of the 17×17 neighbors shared last frame's sample (one more pass, `rtxdi_gi_duplication`). Darkens GI: −8 / −7 pp |
| Final visibility, final MIS | on, on | on, on | reconnection ray; blend with the pixel's own sample on glossy lobes |
| Reuse RTXDI samples for secondary surface | off | off | X₂'s light reuses this frame's ReSTIR DI reservoirs where X₂ is on screen (the RTXDI sample's `ShadeSecondarySurfaces`). With the sun in RIS it darkened the courtyard by −23 %, as the DI visibility shortcut keeps M for shadowed samples ([m8_optimization_and_fixes.md](../spec/features/m8_optimization_and_fixes.md) §2.6, O2). Since M8 I16 the sun is exact at both ends: −2.4 % there, the same as without O2. It cuts the café's raw GI noise by about 19 % but leaves `Lit` unchanged, so it stays off ([i16_denoiser_friendly_noise.md](../spec/fixesM1-6/i16_denoiser_friendly_noise.md) §7.6) |
| Exact directional lights (`Engine`) | on | off | path vertices (X₂, NRC training paths, glass) shade directional lights with one shadow ray each instead of as RIS candidates, as DI's switch does at the primary. Shipped since M8 I16 phase 2c: the courtyard's dim tiles read −0.7 % against eight bounces instead of −7.0 %, noise and cost unchanged. With secondary DI resampling on, DI's switch applies instead: X₂'s candidates and the reused DI reservoirs must hold the same lights ([i16_denoiser_friendly_noise.md](../spec/fixesM1-6/i16_denoiser_friendly_noise.md) §2c, C-I16-4) |
| Firefly clamp (`Engine`) | off | off | caps X₂'s luminance at 10 in scene units, matching the RTXDI sample's `c_MaxIndirectRadiance` |
| Min Secondary Roughness | on (0.5) | off | X₂ is shaded with roughness at least 0.5 while reuse runs (the RTXDI sample's `minSecondaryRoughness`): reservoirs store X₂'s radiance as non-directional, so sharp glossy lobes at X₂ become outliers that reuse spreads. Biased on glossy X₂; the NRC query keeps the raw roughness ([m8_optimization_and_fixes.md](../spec/fixesM1-6/m8_optimization_and_fixes.md) O9) |

- **Ground-truth paths** (`Engine`): trace 8 bounces after X2 instead of querying NRC, which pauses. With Resampling Mode `None` as well, the `Indirect radiance` view is the ground truth, one sample per frame.
- **Cost** (gallery, 1280×720 window (853×480 render, DLSS-RR Quality), 2026-11-20 ABBA): temporal 0.11 ms, shade 0.41–0.42 ms with the shipped defaults of that day (0.26–0.27 ms with NVIDIA's Basic spatial correction). The DLSS-RR profile adds 0.07 ms to the shade (2026-11-21 ABBA, 3 runs each).
- **Bias and stability** (ReSTIR GI mode, after M8 I2, 2026-11-21): bias against one bounce is −0.21% gallery / −1.48% courtyard (tile p90 21.7% / 29.6%); `Lit` stability over 480 static frames (`ENGINE_STABILITY_CHECK`) is 5.80% temporal std / 46.0% flickering px / 0.83% swing in the gallery and 1.30% / 8.3% / 0.15% in the courtyard ([i2_shading_correctness.md](../spec/fixesM1-6/i2_shading_correctness.md)).

### 7.1 NRC

The `NRC:` node under Indirect Lighting (idle training and iterations are in its `Engine` node) acts only in the NRC modes; ReSTIR GI mode pauses the cache. Defaults from the RTXGI `IndirectDiffuse` sample and the SDK ([g3_nrc.md](../spec/m4/g3_nrc.md)).

| Setting | Default | Effect |
|---|---|---|
| Train The Cache | on | off freezes the cache and skips the training paths |
| NRC idle train frames | 512 (M8 I8; `0` = continuous, NVIDIA's default) | trains for this many frames after any camera, sun, light-set, layout, or dynamic-entity change (annealing Adam LR `1e-2 → 5e-4` over the final `≤ 128` frames), then freezes (`trainTheCache = false`, skips `nrc_update.slang`) until the next change. Env override: `ENGINE_NRC_IDLE_FRAMES=n`; lab switch: `nrc.idleTrainingFrames` |
| NRC training iterations | 1 (the sample's; the SDK default is 4) | 16K training records each; the training paths grow with it |
| Resolve Mode | `AddQueryResultToOutput` | default runs the custom GI reservoir resolve (`passes/nrc_resolve.slang`); any other mode runs NRC's SDK debug resolve (training radiance, bounce heat map, query index, etc.) into `Indirect radiance` (ND5, M8 I7) |

- **Cost** (NRC + ReSTIR GI mode, 1280×720 window (853×480 render, DLSS-RR Quality), post-I8 2026-11-22, [i8_nrc_cost.md](../spec/fixesM1-6/i8_nrc_cost.md)): `C-G3-6` host wait disabled by default (`recordIsolated` alone survived 18,000 uncapped stress frames across 1280×720 and 2560×1369; `ENGINE_NRC_HOST_WAIT=1` fallback kept), cutting wall `drawFrame` by **−0.560 ms (−13.6%)**; idle training cuts steady-state GPU total by **−0.288 ms (−8.3%)** Sponza gallery (`drawFrame` `4.468 → 3.396 ms`, −24.0%), **−0.375 ms (−9.6%)** courtyard (`4.827 → 3.649 ms`, −24.4%), and **−0.412 ms (−14.0%)** Bistro street (`4.042 → 2.762 ms`, −31.7%, only `+0.274 ms` above 1-bounce `ReSTIR GI` mode).
- **Bias** (after M8 I7–I8, 2026-11-22, [i7_nrc_bias.md](../spec/fixesM1-6/i7_nrc_bias.md), [i8_nrc_cost.md](../spec/fixesM1-6/i8_nrc_cost.md)): C-G5-3 resolved via the sentinel zero-continuation workaround for NRC 0.16's `hasExitedScene` direct-light leak (C-G3-7). Both NRC phases stay gated in `ENGINE_GI_CHECK=1` (`14 of 14` pass on seeds 0–2 with `idleTrainingFrames = 512`: gallery `NRC+GI −3.88…−2.64%` / `NRC mode −4.96…−3.83%`, courtyard `+0.59…+0.61%` / `−0.69…−0.36%`).

### 7.1.1 SHaRC

The `SHaRC` node under Indirect Lighting controls NVIDIA **SHaRC v1.8.3** ([m8_4_sharc_cache.md](../spec/m8/m8_4_sharc_cache.md)) when `Radiance Cache` is `SHaRC` (also used by `pipeline=rtxpt`). Pure Vulkan 1.4 compute + BDA + native 64-bit atomics with lazy buffer allocation (`40–320 MiB`, default `160 MiB` at $2^{22}$ entries; `0 MiB` when inactive), device-local occupancy & overflow counter readback, and zero CUDA/SDK isolation overhead. Emissive is always kept separate from cached radiance so vertex 0 respects the one-emission rule. Lab switches: `sharc.update`, `sharc.resolve`, `sharc.updateCamera`, `sharc.downscale`, `sharc.capacityShift`, `sharc.sceneScale`, `sharc.accumulationFrames`, `sharc.staleFrames`, `sharc.updateBounces`, `sharc.queryBounces`, `sharc.minRoughness`, `sharc.fireflyClamp`, `sharc.materialDemodulation`, `sharc.debugMode`. Debug view: `SHaRC cache` (`DebugView::SharcCache`).

### 7.2 Glass

Thin-walled smooth glass (`KHR_materials_transmission`, roughness ≤ 0.3) is traced and composited after `combine`, before DLSS-RR ([i13_glass_transmission.md](../spec/fixesM1-6/i13_glass_transmission.md), engine spec §12.4). Scene → `Transparent Geometry` toggles it; the `Glass` part of Scene → `Engine` shows the glass instance count, the pixels behind glass, early stops and pixels at the layer cap. The `Glass` debug view shows `ΠT` (white without glass), or the reflection term with its checkbox. Lab switches: `glass.pass`, `glass.maxLayers`, `glass.reflection`, `glass.diffuseShare`, `glass.emission`, `glass.shadowTransmittance`, `glass.blendGuides`.

| Setting | Default | Effect |
|---|---|---|
| Transparent Geometry | on | off: glass is neither rasterized nor composited, so it is invisible (a debug switch) |
| Max glass layers | 4 (RTXDI FullSample: 8) | crossings a camera or shadow ray composites. At 4, 1% of Bistro's street glass pixels and 23% of the bar shelf's reach the cap; 8 costs `+0.29 / +0.31 ms` for them |
| Glass reflection | on | mirror ray shaded as X₂ (emission + one light sample); `~60%` of the pass's cost |
| Glass diffuse share | on | the dielectric's non-transmitted share lit by one light sample, Lambert only |
| Glass emission | on | the glass material's emission |
| Shadow transmittance | off (as RTXDI's FullSample, whose shadows ignore glass) | final DI, X₂ direct light and final ReSTIR GI shading multiply the glass transmittance along their segment; `+0.68 ms` GPU total on the street pose, `−0.9%` Direct lighting |
| Blend DLSS guides with glass | off | on: DLSS-RR's albedo guides blend the glass layers with the surface behind (I13 §4 variant b); measured a tie with off (§8 of the step) |

- **Cost** (1280×720 window, DLSS-RR Quality, RTX 5070, 2026-11-23): glass pass `0.98–1.10 ms` on Bistro's street windows (99K px behind glass), `0.79–0.83 ms` on the bar shelf (24K px); walking the layers alone is `0.15–0.22 ms`, reflection `0.41–0.59 ms`, diffuse share `0.22 ms`. A scene without glass (Sponza) skips the pass: `0.0006 ms`, the empty scope.

## 8. Components not built yet

OMM (M6) has no settings yet; its defaults will come from the OMM SDK. SER is shelved to post-1.0 (engine spec §16.5 V1, considerations C-G1-3) and has no runtime switch in 1.0.

## 9. Settings lab: trying settings and finding the culprit

The engine checks (§6) measure what ships against how it should look. The settings lab tries other settings and ranks the switches by what they change ([full_testing.md](../spec/features/planned/full_testing.md)). The two never mix: the checks ignore `ENGINE_CONFIG`, and `ENGINE_LAB` refuses to start next to any check or bench variable.

| Variable | What it does |
|---|---|
| `ENGINE_CONFIG="di=shipped gi=nvidia di.spatialSamples=2"` | the interactive engine and bench runs start with this config: `pipeline=<preset>` (`targetRis\|targetTemporal\|target\|rtxpt\|rtxdi\|rtxgi`, modifying only the stage toggles, DI/GI sets, denoiser and DLSS quality) first, then domain sets (`di=shipped\|nvidia\|converged\|reference\|fast\|medium\|unbiased\|ultra\|tuned` — each `di=<set>` selects `frame.directMode=restirDi` to keep legacy plans faithful, so use `frame.directMode=streamingRis` after it when overriding to Streaming RIS; `gi=shipped\|nvidia\|converged\|tuned`; `frame=shipped`), then `domain.switch=value` overrides (including `frame.directMode=off\|streamingRis\|restirDi`, `frame.directResampling=temporalAndSpatial\|temporal\|spatial\|reference`, `frame.indirectBounce=off\|rawPt\|restirGi`, `frame.radianceCache=off\|nrc\|sharc`, plus legacy `frame.indirectMode` and `di.reference\|spatialOnly\|temporalOnly` aliases), over what `ENGINE_INDIRECT_MODE` and `ENGINE_DI_SAMPLING` set. A typo stops the engine. The Settings window's *Copy config* button (at its bottom, next to *Defaults*) copies the current settings in this form |
| `ENGINE_LAB=<plan>` | runs a plan and quits: a built-in name (`sets`, `di-ofat`, `gi-ofat`, `cafe`, `leak`, `stability-levers`) or `tests/lab/<plan>[.lab]` |
| `ENGINE_LAB_CASES=cafe.tables,sponza.courtyard` | keeps only these cases of the plan |
| `ENGINE_LAB_PNG=1` | each run's first frame, mean, CV map and error map in `logs/lab` |

Run it like the checks: from its own build tree when another session uses `build/windows-clang-release` (`cmake --preset windows-clang-release-checks -B build/<name> -DFMT_MODULE=OFF`), with `VK_LOADER_LAYERS_DISABLE='*validation*' VK_KHRONOS_VALIDATION_GPUAV_ENABLE=false`. A run is about 10 s (780 frames); `di-ofat` is ~15 min, `stability-levers` several hours.

**Output.** `[Lab] MEASURED <plan>, <case>, <view>, <config>, <kind>, seed n: …` per run in `logs/engine.log`; at the end, per case, view and metric, `largest effects on …` with each effect in noise floors (below 2 floors is "no effect"), `interaction candidate` and `pair … non-additivity` lines, `bisect …` results, `decision …` lines per lever, and `target:` (the café against the Sponza poses). Files: `logs/lab/<plan>.csv` (one row per run, every metric and its floor), `<plan>_decisions.csv`, `<plan>_cost.ps1` (bench runs, ABBA per config: run it afterwards, on its own; the lab never times its own frames).

**Plan format** (one directive per line, `#` comments; each run directive uses the `cases` / `views` / `kinds` lines above it). `base`, `target` and `sets` start from the shipped config, NVIDIA's in `NRC` mode since 2026-11-25; `fix`, `sweep`, `lever` and `pairs` start from `base`. The built-in plans name `di=tuned gi=tuned frame.indirectMode=nrcRestirGi` on those lines, the base they were written for; the plan files in `tests/lab` still say `di=shipped gi=shipped` and so run on NVIDIA's sets now (considerations C-NS-6):

```
base    di=shipped gi=shipped
target  di=nvidia gi=shipped
seeds   3
cases   cafe.tables cafe.dining
views   Lit "Direct lighting"
kinds   stability diBias
ofat                                   # each differing switch alone, both directions
sweep   di.radius 4 8 16 32            # on the base; "over fix gi.boilingFilter=on" starts elsewhere
pairs   di.spatialSamples di.permutation
sets    di=converged
bisect  Lit:flicker                    # smallest switch set behind base → target on one metric
fix     di.spatialSamples=2 di.permutation=on
lever   L1 both: di.spatialSamples=2 di.permutation=on
stack   Lit:std
```

Kinds: `stability` (temporal std, flicker share, f2f, 8-frame swing, lag-1 correlation), `diBias` (against RTXDI's no-reuse reference: bias, tile p50 / p90, dark leak, dim bias, hot bias), `giBias` (against the GI check's one-bounce `1b.*` and 8-bounce `8b.*` truths), `lightChange` (`move.settle`, `toggle.settle`), `screenEdge` (`pan.band`, `pan.disoccluded`, `dolly.*`), `cost` (the script). Cases: `sponza.gallery`, `sponza.courtyard`, `bistro.street`, `cafe.tables`, `cafe.dining`, `guide.lamp`, `guide.terminator`, `guide.emissive`; `cafe.door`, `glossy.*`, `glass.window`, `cafe.toggle` are listed but wait for their poses or features and are skipped.

**Finding a culprit.**

1. Name the symptom by its metrics, not by eye: run the base on the case (`sets` plan, or a plan with `seeds 3` and nothing else) and read the pattern off [full_testing.md](../spec/features/planned/full_testing.md) §4.1. Confetti under DLSS-RR with a calm raw view and a high `lag1` is temporal correlation; a high raw `std` is too few samples; `dimBias > 0` with `darkLeak` 0 is reuse brightening dim places; `darkLeak > 0` is visibility (also present in the reference) or a ray bug, not a setting.
2. Take the candidates from §3's "If wrong" column for that pattern and write a plan: `ofat` between the base and a config without the symptom (NVIDIA's set is usually one), on the case and the view that shows it.
3. Read the ranking. A switch on top with several floors is the lead; one with an effect from the base and none in the target (an `interaction candidate`) needs a partner: `pairs` it with the next switch.
4. If the one-factor effects do not add up to the base → target difference, `bisect` on the metric finds the smallest set.
5. Confirm with the mechanism (§3 "What it does", ideally in NVIDIA's words) and check the fix on the other cases: its bias and lag must stay within 2 floors everywhere (the `decision` lines do this for levers). Then the step that owns the setting changes it and re-runs its gates; the lab never ships a setting.
