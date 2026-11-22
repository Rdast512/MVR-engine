# M8 · I8: NRC cost

**Status:** built 2026-11-22 · **Milestone:** M8 ([README](./README.md))
**Implements:** findings NP1 (ND7 / C-G3-6), NP2, NP4, NP5 ([m8_optimization_and_fixes.md](./m8_optimization_and_fixes.md) §3.2, §3.4)
**Depends on:** I7 ([i7_nrc_bias.md](./i7_nrc_bias.md), which gated both NRC modes in `ENGINE_GI_CHECK=1`)
**References:**
- `ThirdParty/_deps/src/nrc/Include/{Nrc.hlsli,NrcCommon.h,NrcVk.h}`: `FrameSettings::trainTheCache`, `FrameSettings::learningRate` (`[0.0001, 0.01]`), `NrcUpdateOnHit` `prefixThroughput` under `learnIrradiance = true`, `QueryAndTrain`
- `reference/RTXGI/Samples/IndirectDiffuse/Shaders/NrcIndirectDiffuse.hlsli:223` (`luminance(throughput) < g_Global.throughputThreshold`) and `Pathtracer.cpp:1119,1237`
- `spec/considerations.md`: `C-G3-6` (host wait / command-buffer isolation), `C-G1-3` (SER shelved on Slang 2026.22), `C-T-5` (NRC non-determinism)

## Scope

I7 fixed NRC's bright bias (`C-G5-3`) and gated both `NRC + ReSTIR GI` and `NRC mode` in `ENGINE_GI_CHECK=1` (`14 of 14` pass). `IndirectMode::RestirGi` stayed default because NRC added GPU work (`NRC update` + overlapped `nrc_update.slang` inside `Indirect`, `NRC train`, `NRC resolve`) plus the `C-G3-6` host wait (~0.56 ms wall frame time at 1280×720). I8 measured and landed or closed every NRC performance lever scheduled for the RTX 5070:

| Finding | Outcome | Files |
|---|---|---|
| **NP1 (ND7 / C-G3-6)** | **Landed:** host wait disabled by default (`isNrcHostWaitEnabled = false`, `ENGINE_NRC_HOST_WAIT=1` fallback kept); `lastNrcRaySubmit` cleared after waiting; `ENGINE_WINDOW_SIZE=WxH` added for 2560×1369 stress verification. Command-buffer isolation (`recordIsolated`) survived **18,000 uncapped stress frames** (6/6 runs across 1280×720, 3× 2560×1369 Sponza, 2560×1369 `cycle`, and 2560×1369 Bistro) with 0 device losses. Wall frame time `drawFrame` cut by **−0.560 ms (−13.6%)** alone at 1280×720 | `src/render/vk_renderer.{hpp,cpp}`, `src/core/vk_engine.cpp` |
| **NP2 (idle training)** | **Landed:** `NrcCache::Settings::idleTrainingFrames = 512` (`0` = continuous, NVIDIA's default; `ENGINE_NRC_IDLE_FRAMES=n`, UI slider, lab switch `nrc.idleTrainingFrames`). Trains for `512` frames after any camera, sun, light-set, layout, or dynamic-entity transform change (annealing Adam `learningRate` `1e-2 → 5e-4` over the final `≤ 128` frames so frozen weights land in the stochastic mean instead of locking in a `1e-2` mini-batch bounce), then sets `trainTheCache = false` and skips `nrc_update.slang`. Cuts steady-state GPU total by **−0.288 ms (−8.3%)** gallery, **−0.375 ms (−9.6%)** courtyard, and **−0.412 ms (−14.0%)** Bistro while moving scenes (`Field 64`) keep training every frame. Benign SDK `trainTheCache is false` log spam filtered in `logNrc` | `src/render/vk_nrc.{hpp,cpp}`, `src/render/vk_rt_scene.{hpp,cpp}`, `src/render/vk_renderer.{hpp,cpp}`, `src/ui/engine_ui.cpp`, `src/lab/settings_registry.cpp` |
| **NP5 (query throughput `< 0.01`)** | **Closed with numbers:** primary-only throughput `< 0.01` (`NrcIndirectDiffuse.hlsli:223` semantics at 1 bounce) measured `0.000 ms` (`0.1880 → 0.1883 ms` `NRC train`, `2.688 → 2.684 ms` GPU total on Bistro); secondary reflectance `< 0.01` measured `0.000 ms` on Sponza and Bistro; full two-bounce product `X1 · X2 < 0.01` saved only `−0.017 ms` (`−0.5%`) Sponza / `−0.028 ms` (`−1.1%`) Bistro while darkening gallery dim tiles by **−7 to −10 pp** (`−2.1% → −11.7%`). Temporary flags removed | — |
| **NP4 (SER passes)** | **Deferred:** `slangc -v` is `2026.22`; `C-G1-3` remains active (Slang #12554, #13763, #13772, #13774). Stays shelved to post-1.0 V1 | — |

NP3 (`usedTrainingDimensions` on floor tier) remains scheduled for I11 (RTX 2060 Super). `IndirectMode::RestirGi` remains default until I12 evaluates Bistro café indirect quality and cost across modes.

## Changes landed

1. **NP1 (`C-G3-6` host wait, `src/render/vk_renderer.{hpp,cpp}`, `src/core/vk_engine.cpp`):**
   - `Renderer::isNrcHostWaitEnabled` defaults to `false` (isolated SDK command buffers `recordIsolated` stay required). `ENGINE_NRC_HOST_WAIT=1` re-enables the wait as a fallback if a future driver regresses.
   - `Renderer::finishPreviousFrame()` clears `lastNrcRaySubmit = 0` after waiting so pausing NRC never re-waits on stale timeline values.
   - `ENGINE_WINDOW_SIZE=WxH` overrides the initial SDL window extent for automated 2560×1369 stress runs; `ENGINE_FULLSCREEN=1` creates the window fullscreen at desktop resolution (overrides the size) for the full-screen stress case.
2. **NP2 (adaptive / idle training with Adam LR annealing, `src/render/vk_rt_scene.{hpp,cpp}`, `src/render/vk_nrc.{hpp,cpp}`, `src/render/vk_renderer.{hpp,cpp}`, `src/ui/engine_ui.cpp`, `src/lab/settings_registry.cpp`):**
   - `RayTracingScene::hasSceneChanged()` reports layout rebuilds and any dynamic transform change across active meshlet or light-bearing entities (`rowCount > 0 || !model.lights.empty()`), reusing `updateDynamicEntities()` at zero extra per-frame CPU cost.
   - `Renderer::drawFrame()` runs `worldLights.prepareFrame` before `nrc.prepareFrame` and computes `isNrcSceneChanged` from camera cuts, unjittered `viewProj`/`cameraPos` changes, `worldLights.sun`, `isLightSetChanged`, and `rtScene.hasSceneChanged()`. Switching between `NRC + ReSTIR GI` and `NRC` (or changing `giSettings`) does **not** restart training (NRC's training target is unchanged; unpausing NRC already reloads the countdown via `wasPaused`).
   - `NrcCache::Settings::idleTrainingFrames = 512` (`0` = continuous): reloads `trainingFramesRemaining = idleTrainingFrames` on any scene/camera/light/unpause/setting change; linearly ramps exponential Adam LR `1e-2 → 5e-4` over the final `min(idleTrainingFrames / 2, 128)` frames, then sets `frame.trainTheCache = false` and skips `nrc_update.slang` (`QueryAndTrain` runs inference only).
   - `logNrc` filters NRC 0.16's per-frame `trainTheCache is false` notice when training is intentionally frozen.
   - ImGui Ray tracing panel exposes `NRC idle train frames` (`0..1024`) and live `active (training, N left)` / `active (idle, inference only)` status; `settings_registry.cpp` registers `nrc.idleTrainingFrames`.

## Results

All benchmarks: RTX 5070, driver `621.01`, `windows-clang-release`, validation layers off, DLSS-RR Quality (`853×480` render at `1280×720` window; `1707×913` render at `2560×1369`), alternating ABBA runs (`2400` measured frames unless noted).

### 1. NP1 alone (`ENGINE_NRC_HOST_WAIT=1` vs `0`, continuous training `idle=0`, Sponza gallery)

| Variant | `Indirect` | `NRC train` | `GPU total` | `CPU drawFrame` (waits) | Wall time (2400f) |
|---|---:|---:|---:|---:|---:|
| Host wait ON (A1/A2 mean) | 0.6911 ms | 0.4033 ms | 3.4694 ms | **4.1274 ms** (3.5265 ms) | 16.68 s |
| **Host wait OFF (B1/B2 mean)** | 0.7253 ms | 0.4282 ms | 3.5586 ms | **3.5677 ms** (3.0909 ms) | **14.91 s** |
| **Delta** | +0.034 ms | +0.025 ms | +0.089 ms | **−0.560 ms (−13.6%)** | **−1.77 s (−10.6%)** |

**`C-G3-6` exit stress runs (`ENGINE_NRC_HOST_WAIT=0`, driver `621.01`, `recordIsolated` active):**
- `1280×720` Sponza `nrc-restir-gi` × 3,000 frames: **exit 0, 0 device losses** (`drawFrame 3.556 ms`).
- `2560×1369` Sponza `nrc-restir-gi` × 3 runs × 3,000 frames: **3/3 exit 0, 0 device losses** (`drawFrame 11.65–11.92 ms`).
- `2560×1369` Sponza `ENGINE_INDIRECT_MODE=cycle` × 3,000 frames: **exit 0, 0 device losses** (`drawFrame 9.627 ms`).
- `2560×1369` Bistro `nrc-restir-gi` (shipped I8) × 3,000 frames: **exit 0, 0 device losses** (`drawFrame 9.023 ms`).
- Total: **18,000 uncapped frames, 0 device losses.**

**Full-screen case** (C-G3-6's exit; added after review, 2026-11-22): `ENGINE_FULLSCREEN=1` (SDL fullscreen at desktop resolution: 2560×1440 display, 1707×960 render), host wait off, 3,000 measured frames after 240 warm-up per run:
- Sponza `nrc-restir-gi` × 3: **3/3 exit 0, 0 device losses** (`drawFrame 11.99–12.02 ms`, GPU total 12.04–12.07 ms).
- Sponza `ENGINE_INDIRECT_MODE=cycle`: **exit 0, 0 device losses** (`drawFrame 10.33 ms`).
- Bistro `nrc-restir-gi` (2,909 instances): **exit 0, 0 device losses** (`drawFrame 9.37 ms`).
- Total: **15,000 full-screen uncapped frames, 0 device losses**; with the windowed runs, 33,000.

### 2. NP2 alone (steady-state idle `idle=64` vs continuous `idle=0`, host wait ON)

Skipping `nrc_update.slang` removes both the training pass inside `QueryAndTrain` (`NRC train`) and `nrc_update.slang`'s overlap with `indirect.slang` (`Indirect`):

| Scene | Mode | `Indirect` | `NRC train` | `NRC resolve` | `GPU total` | Delta |
|---|---|---:|---:|---:|---:|---:|
| Sponza gallery | continuous (`idle=0`) | 0.6967 ms | 0.4120 ms | 0.0402 ms | 3.4701 ms | — |
| Sponza gallery | **idle (`idle=64`)** | **0.5288 ms** | **0.2819 ms** | 0.0375 ms | **3.1822 ms** | **−0.288 ms (−8.3%)** |
| Sponza courtyard | continuous (`idle=0`) | 0.8103 ms | 0.4618 ms | 0.0443 ms | 3.8941 ms | — |
| Sponza courtyard | **idle (`idle=64`)** | **0.6273 ms** | **0.3057 ms** | 0.0368 ms | **3.5192 ms** | **−0.375 ms (−9.6%)** |
| Bistro street | continuous (`idle=0`) | 0.5541 ms | 0.3579 ms | 0.0310 ms | 2.9459 ms | — |
| Bistro street | **idle (`idle=64`)** | **0.3329 ms** | **0.1774 ms** | 0.0195 ms | **2.5339 ms** | **−0.412 ms (−14.0%)** |
| Field 64 moving | `idle=64` (dynamic check) | 0.7277 ms | 0.4755 ms | 0.0433 ms | 3.6564 ms | stays training every frame |

### 3. Combined shipped I8 (`wait=0, idle=256/512`) vs pre-I8 (`wait=1, idle=0`) and `ReSTIR GI` mode

| Scene | Pre-I8 `drawFrame` (`GPU total`) | Shipped I8 `drawFrame` (`GPU total`) | `drawFrame` win | `ReSTIR GI` mode `drawFrame` (`GPU total`) | Shipped I8 overhead vs 1-bounce |
|---|---:|---:|---:|---:|---:|
| Sponza gallery (1280×720) | 4.468 ms (3.551 ms) | **3.396 ms** (3.438 ms) | **−1.072 ms (−24.0%)** | 2.858 ms (2.888 ms) | +0.538 ms |
| Sponza courtyard (1280×720) | 4.827 ms (3.979 ms) | **3.649 ms** (3.610 ms) | **−1.179 ms (−24.4%)** | 3.158 ms (3.259 ms) | +0.491 ms |
| Bistro street (1280×720) | 4.042 ms (3.081 ms) | **2.762 ms** (2.751 ms) | **−1.280 ms (−31.7%)** | 2.489 ms (2.485 ms) | **+0.274 ms** |
| Sponza gallery (2560×1369, 1200f) | 12.860 ms (12.008 ms) | **12.146 ms** (11.835 ms) | **−0.713 ms (−5.5%)** | — | — |

### 4. NP2 convergence sweep & 3-seed GI check (`ENGINE_GI_CHECK=1`)

Hard-freezing Adam at its full `lr = 1e-2` locks in a single noisy 16K-record mini-batch snapshot (failing gallery even after 768 total training frames). Exponentially annealing `learningRate` `1e-2 → 5e-4` over the final `≤ 128` frames settles the weights cleanly into the mean, and `idleTrainingFrames = 512` (384 full-LR + 128 annealed frames) passes **`14 of 14`** across all three tested seeds:

| Config | Seed | Gallery `NRC+GI` (bias / p90) | Gallery `NRC mode` (bias / p90) | Courtyard `NRC+GI` (bias / p90) | Courtyard `NRC mode` (bias / p90) | Gate |
|---|---:|---:|---:|---:|---:|---|
| `idle=64` (no anneal) | 0 | −3.85% / **51.75%** | **−5.51%** / 13.19% | — | **−1.69%** / 14.25% | 10/14 FAIL |
| `idle=256` (no anneal) | 0 | **−5.07%** / 14.03% | −4.52% / **26.41%** | −0.19% / 13.20% | −0.22% / 5.30% | 12/14 FAIL |
| `idle=256` (128+128 anneal, no mode reset) | 0 | −4.93% / 17.43% | **−6.01%** / **20.04%** | −0.45% / 18.70% | −1.39% / 17.84% | 13/14 FAIL (cold-start short) |
| **`idle=512` (384+128 anneal, shipped)** | **0** | **−2.93% / 12.98%** | **−4.12% / 15.39%** | **+0.59% / 9.02%** | **−0.36% / 7.52%** | **14/14 PASS** |
| **`idle=512` (384+128 anneal, shipped)** | **1** | **−3.88% / 14.24%** | **−4.96% / 16.77%** | **+0.59% / 9.58%** | **−0.69% / 7.89%** | **14/14 PASS** |
| **`idle=512` (384+128 anneal, shipped)** | **2** | **−2.64% / 10.99%** | **−3.83% / 13.63%** | **+0.61% / 8.86%** | **−0.44% / 7.15%** | **14/14 PASS** |

### 5. NP5 (`throughputThreshold = 0.01`) — closed with numbers

| Variant | Sponza gallery `NRC train` (`GPU total`) | Bistro street `NRC train` (`GPU total`) | GI check impact | Verdict |
|---|---:|---:|---|---|
| Off (shipped) | 0.2814 ms (3.1810 ms) | 0.1767 ms (2.5313 ms) | baseline | **Keep** |
| Secondary reflectance `< 0.01` | 0.2764 ms (3.1812 ms, +0.000 ms) | 0.1766 ms (2.5392 ms, +0.008 ms) | — | **Closed (0 gain)** |
| Primary throughput `< 0.01` (`NrcIndirectDiffuse.hlsli:223`) | — | 0.1883 vs 0.1880 ms (2.684 vs 2.688 ms, −0.004 ms) | — | **Closed (0 gain at 1 bounce)** |
| Full product `X1 · X2 < 0.01` | 0.2643 ms (3.1635 ms, −0.017 ms) | 0.1507 ms (2.5032 ms, −0.028 ms) | 14/14 pass, but gallery dim bias darkened `−2.1% → −11.7%` (`NRC+GI`), `−4.8% → −9.2%` (`NRC`) | **Closed (too dark on dim interiors for ~0.02 ms)** |

### 6. Verification (2026-11-22, `windows-clang-release`, RTX 5070)

- **CTest:** `8 of 8` passed (`hiz_footprint`, `brdf`, `lights`, `rtxdi_math`, `layout`, `lab_registry`, `nrc_library`, `dlss`).
- **`ENGINE_GI_CHECK=1`:** **`14 of 14` passed on seeds 0, 1, and 2**.
- **`ENGINE_STABILITY_CHECK=1`:** `28 steps, 0 failed` (gallery `5.74% / 39.27% / 0.81%`, courtyard `0.99% / 8.60% / 0.06%`, unchanged from post-I6); Bistro café NRC stability `12 steps, 0 failed` (`cafe tables` DI `s2+p1` `NRC+GI` `Lit` `7.94% / 27.35%`, `cafe dining` `5.94% / 32.58%`).
- **`ENGINE_LIGHT_CHANGE_CHECK=1`:** `32 steps, 0 failed`.
- **`ENGINE_SCREEN_EDGE_CHECK=1`:** `80 steps, 0 failed`.
- **`ENGINE_RT_CHECK=1`:** `32 of 32` frames matched.
- **`ENGINE_CULL_CHECK=1`:** `16 of 16` poses identical.
- **`ENGINE_MOTION_TEST=1`:** `13 of 13` scenarios passed.
- **`ENGINE_GUIDE_CAPTURE=1`:** `2 of 2` normal checks, `3 of 3` DI convergence checks passed (`396` PNGs).
- **Vulkan validation** (`core + sync + best practices + NVIDIA best practices`, `GPUAV=false`, `ENGINE_INDIRECT_MODE=cycle`, `ENGINE_NRC_IDLE_FRAMES=16`, `120` frames — covering both active training and idle transitions): clean, only known external SDK messages (NGX `WorkgroupSize` + `SYNC-HAZARD-WRITE-AFTER-WRITE` `C-H1-2` and NRC `vkDestroyDevice` `C-G3-3`).

## Done when

- [x] Baseline post-I7 `NRC + ReSTIR GI` ABBA timings recorded (gallery, courtyard, Bistro, 2560×1369)
- [x] NP1 (`C-G3-6` host wait) verified on driver `621.01` with `recordIsolated` (18,000 windowed + 15,000 full-screen uncapped stress frames, 0 device losses; `−0.560 ms` / `−13.6%` wall frame time), disabled by default with `ENGINE_NRC_HOST_WAIT=1` fallback; `lastNrcRaySubmit` cleared after wait
- [x] NP2 (adaptive/idle training with Adam LR annealing `1e-2 → 5e-4`) landed (`idleTrainingFrames = 512`), ABBA-measured on static (`−0.288…−0.412 ms` GPU total) and moving (`Field 64`) scenes, and verified across 3 GI-check seeds (`14/14` on seeds 0, 1, 2)
- [x] NP5 (query throughput threshold `0.01`) A/B-measured across all three definitions on Sponza and Bistro and closed with its numbers
- [x] NP4 recorded as deferred on `C-G1-3` (Slang `2026.22`)
- [x] Full verification clean: CTest (`8/8`), `ENGINE_GI_CHECK=1` (`14/14` × 3 seeds), `ENGINE_STABILITY_CHECK`, `ENGINE_LIGHT_CHANGE_CHECK`, `ENGINE_SCREEN_EDGE_CHECK`, `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST`, `ENGINE_GUIDE_CAPTURE`, Vulkan validation
- [x] `i8_nrc_cost.md`, `m8_optimization_and_fixes.md`, `considerations.md`, `README.md`, and `docs/settings.md` updated
