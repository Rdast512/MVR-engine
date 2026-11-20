# Settings: tuning the NVIDIA tech

How to tune the engine's NVIDIA components: RTXDI (ReSTIR DI), DLSS Ray Reconstruction and RTX Mega Geometry (CLAS, PTLAS). Covers what each setting changes, what it costs, and how to check a change. Every number here was measured on the RTX 5070, release build, validation layers off.

All settings are in the ImGui panels: **Ray tracing** (Mega Geometry, RTXDI, DLSS-RR), **Sun** and **Post**. Settings are not saved; every run starts from the defaults.

---

## 1. Defaults

The defaults are the ones NVIDIA ships in its own samples, so the engine's cost can be compared with them:

| Component | Default taken from | Values |
|---|---|---|
| RTXDI | RTXDI-Library sample (`Samples/FullSample`), its default `Medium` preset | §3 |
| DLSS-RR | `vk_denoise_dlssrr` | Quality mode, preset `Default` |
| CLAS, cluster BLAS | `vk_lod_clusters` | fast trace, minimum position truncation |
| PTLAS | `vk_partitioned_tlas` | fast trace |

Deviations, where the engine lacks the feature:
- **ReGIR.** RTXDI's `Medium` preset uses ReGIR for local-light sampling, which needs light presampling (M7). Until then the engine samples local lights uniformly, with the same candidate count (8).
- **Resolution.** The RTXDI sample renders at native resolution, which is DLSS-RR's DLAA mode in the engine. Set the mode to DLAA before comparing images or costs with that sample.

Buttons:
- **Ray tracing panel → Defaults:** resets PTLAS, RTXDI and DLSS-RR.
- **RTXDI section → NVIDIA defaults / Converged:** switch between the two RTXDI sets (§3.2).
- **Sun and Post panels:** each has its own Defaults button.

---

## 2. Why the image is less stable than NVIDIA's demos

NVIDIA's demos render complete lighting. The engine is missing three pieces of it. Each makes noise and flicker easier to see, and none is a setting:

| Missing | Effect now | Fixed in |
|---|---|---|
| Indirect light (NRC, ReSTIR GI) | Shadows are pitch black, so every shadow edge has maximum contrast; flicker that GI would hide is fully visible | M4 (G1–G6) |
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

### 3.2 The two built-in sets

- **NVIDIA defaults** (`RtxdiContext::defaultSettings`): the table's last column. ReSTIR DI total ≈ 0.35 ms on the sunlit Sponza bench (853×480 render size).
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
- RR reads the last post pass's exposure as an input; its color input is not pre-exposed. Very high sun illuminance can overflow that RGBA16F input (considerations C-H2-1).

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
| ReSTIR GI against the ground truth | `ENGINE_GI_CHECK=1 VK_LOADER_LAYERS_DISABLE='*validation*' VK_KHRONOS_VALIDATION_GPUAV_ENABLE=false ./engine.exe` (~10 min) → `[GiCheck]` lines | `ReSTIR GI convergence: 8 of 8 passed` (G5's phases and ReSTIR GI mode against a one-bounce ground truth); the `MEASURED` lines give the bias of NRC, NRC mode and the NVIDIA GI defaults ([g5_restir_gi.md](../spec/m4/g5_restir_gi.md) §3, [g7_indirect_modes.md](../spec/m4/g7_indirect_modes.md) §3) |
| Indirect modes under switching | `ENGINE_INDIRECT_MODE=cycle ENGINE_BENCH_FRAMES=3000` at a 2560×1369 window | no device loss; a mode id instead of `cycle` pins one mode for per-mode timings |
| Bias of a setting | `Direct lighting` view with the setting, then with Reference on (`ENGINE_RTXDI_REFERENCE=1` or the checkbox) | same brightness after a few seconds |
| Validation | `VK_LOADER_LAYERS_ENABLE='*validation' VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true VK_KHRONOS_VALIDATION_VALIDATE_BEST_PRACTICES=true VK_KHRONOS_VALIDATION_VALIDATE_BEST_PRACTICES_NVIDIA=true VK_KHRONOS_VALIDATION_GPUAV_ENABLE=false VK_KHRONOS_VALIDATION_LOG_FILENAME=<file>` | only NGX's known messages (considerations C-H1-2) |

When judging stability by eye, hold the camera still for a few seconds and watch the `Lit` view at 100% scale. Windows Auto HDR brightens the window in screen captures; the swapchain itself is correct.

---

## 7. ReSTIR GI

`Indirect mode` at the top of the `Indirect (M4)` panel ([g7_indirect_modes.md](../spec/m4/g7_indirect_modes.md)): `ReSTIR GI` (default on both tiers since 2026-11-20: one bounce, NRC paused; GPU total 2.72 ms vs 3.27–3.30 ms at 1280×720, no C-G3-6 host wait), `NRC + ReSTIR GI` (both tiers; NRC reads ~10% bright, considerations C-G5-3), `NRC` (the cache samples without reuse). A switch resets ReSTIR GI and DLSS-RR history; `ENGINE_INDIRECT_MODE` sets it from the environment.

The `ReSTIR GI` section of the `Indirect (M4)` panel. Defaults (`RtxdiContext::defaultGiSettings`, since 2026-11-20): the library's, which the RTXDI sample uses unchanged in every preset, with ray-traced spatial bias correction. `NVIDIA defaults` (`nvidiaGiSettings`) restores the library's exactly. Why and what it costs: [m8_optimization_and_fixes.md](../spec/features/m8_optimization_and_fixes.md) §2.5.

| Setting | Default | Converged | Effect |
|---|---|---|---|
| Max history length | 8 | 8 | frames of temporal reuse |
| Max sample age | 30 | 1024 (never) | discards a reused sample this old; reacts to light changes, biases a still frame bright (+5.6% in the gallery, considerations C-G5-1) |
| Temporal / spatial bias correction | Basic / Ray traced (NVIDIA: Basic / Basic) | Ray traced | spatial Basic darkens occluded places (ReSTIR GI mode −27% gallery / −9% courtyard, C-G5-2); Ray traced adds a ray per merged sample (spatial +0.15 ms at 1280×720); the temporal pass's Basic does not darken |
| Fallback sampling | on | off | reuse last frame's sample at the same pixel when no history matches |
| Spatial samples, radius | 2, 32 px | 2, 32 px | |
| Boiling filter | on, 0.2 | off | suppresses reservoirs far brighter than their 8×8 group |
| Final visibility, final MIS | on, on | on, on | reconnection ray; blend with the pixel's own sample on glossy lobes |

- **Ground-truth paths:** trace 8 bounces after X2 instead of querying NRC, which pauses. With `Reference (no reuse)` on as well, the `Indirect radiance` view is the ground truth, one sample per frame.
- **Cost** (gallery, 1280×720 render, defaults): temporal 0.11 ms, shade 0.38 ms.

## 8. Components not built yet

NRC, OMM and SER have no settings yet. Their defaults will come from NVIDIA as well: NRC from `reference/NrcGuide.md` and NVIDIA's NRC sample, OMM from the OMM SDK's own defaults.
