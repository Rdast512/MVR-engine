# M8 · I4: frame profile

**Status:** built 2026-11-21 · **Milestone:** M8 ([README](./README.md))
**Implements:** findings F4, F7, F8, F9 (RTX 5070 part), F11, F12 ([m8_optimization_and_fixes.md](./m8_optimization_and_fixes.md) §4), C-F1-5 ([considerations.md](../considerations.md))
**Depends on:** I2 ([i2_shading_correctness.md](./i2_shading_correctness.md)), I3 ([i3_mega_geometry_fixes.md](./i3_mega_geometry_fixes.md))
**Tools:** engine `GpuTimer`, compute-queue timestamp pools (`RayTracingScene`, `RtxdiContext`), Nsight Graphics 2026.3.1 (`ngfx --activity="GPU Trace Profiler"`), Nsight Systems 2026.5.1 (`nsys`)

## Scope

Measure the fixed post-I2/I3 frame on the RTX 5070 before any tuning or perf step, so every lever in I5, I6, I9 and I10 is ordered by measured gain rather than expectation.

| Item | Change / Measurement | Files |
|---|---|---|
| F4 | Add `GpuTimerScope::TlasWait` ("TLAS wait") between `Resolve` and `RtxdiInitial` in `recordDirectLight` so `ReSTIR DI initial` measures only `rtxdi_initial` | `src/render/vk_gpu_timer.hpp`, `src/render/vk_renderer.cpp` |
| C-F1-5 | Add a compute-queue timestamp query pool in `RtxdiContext` timing `light_world`, `light_pdf_mips`, and `presample` (`Power_RIS` / `ReGIR`), reported in `EngineBench` and `EngineUi` | `src/render/vk_rtxdi.{hpp,cpp}`, `src/render/vk_renderer.hpp`, `src/core/engine_bench.{hpp,cpp}`, `src/ui/engine_ui.cpp` |
| F8 | Record rolling CPU frame breakdown (`drawFrame`, `FrameData::write`, `recordCommands`, `submit`, `waits`) in `Renderer` and log in `EngineBench` against the 2 ms CPU budget | `src/render/vk_renderer.{hpp,cpp}`, `src/core/engine_bench.{hpp,cpp}` |
| Bench selectors | Optional `ENGINE_BENCH_MODEL`, `ENGINE_BENCH_POSE=courtyard`, and `ENGINE_DI_SAMPLING=regir\|power_ris\|uniform` for reproducible profile matrices | `src/core/engine_bench.{hpp,cpp}`, `src/core/vk_engine.cpp`, `src/render/vk_rtxdi.cpp` |
| Profile captures | Per-scope GPU + CPU + async-compute tables in all three indirect modes; Nsight GPU Trace of the default frame (per-pass limiters); async-compute overlap capture (F9); re-measurement of F7 (PTLAS), F8 (`FrameData::write`), F11 (binned resolve), and F12 (ReGIR vs `Power_RIS` on Sponza & Bistro) | Recorded in §Results below |

## Decisions

| Decision | Rule | Why |
|---|---|---|
| `TlasWait` in `GPU total` | Keep `GpuTimerScope::TlasWait` in `GPU total` (unlike `AcquireWait`, which is swapchain pacing idle) | Any non-zero `TlasWait` is critical-path GPU frame time stalled on async compute; previously it sat inside `RtxdiInitial`, so keeping it preserves continuity |
| Async light prep timer | Own compute-queue `vk::raii::QueryPool` in `RtxdiContext` (`4` queries × `MAX_FRAMES_IN_FLIGHT`), matching `RayTracingScene`'s pattern | `RtxdiContext::prepareFrame` runs right after `ComputeWait`, so slot collection needs no extra sync and adds zero overhead on graphics |
| Zero behavior change | Instrumentation only; no pass, shader, or default setting changes in I4 | Keeps the profile apples-to-apples with I2/I3 and leaves all tuning decisions to I5, I6, I9, I10 |

## Results

RTX 5070, release, no layers (`VK_LOADER_LAYERS_DISABLE=all`, `VK_KHRONOS_VALIDATION_GPUAV_ENABLE=false`), 1280×720 window (853×480 render, DLSS-RR Quality), 2026-11-21.

### 1. Per-scope GPU & CPU tables across all three indirect modes

**Sponza gallery (startup pose, 103 instances, 1,200 measured frames after 240 warm-up):**

| Scope (mean ms) | `ReSTIR GI` (default) | `NRC + ReSTIR GI` | `NRC` |
|---|---:|---:|---:|
| Early cull + DGC + draw + alpha + Hi-Z | 0.083 | 0.080 | 0.080 |
| Late cull + DGC + draw + alpha + Hi-Z | 0.040 | 0.039 | 0.039 |
| Material classify + Vis-buffer resolve (F11) | 0.115 (0.018 + 0.097) | 0.111 (0.017 + 0.094) | 0.110 (0.018 + 0.093) |
| **TLAS wait (F4)** | **0.017** | **0.010** | **0.010** |
| ReSTIR DI initial / temporal / shade | 0.156 / 0.080 / 0.114 (**0.351**) | 0.147 / 0.064 / 0.089 (**0.300**) | 0.147 / 0.065 / 0.088 (**0.300**) |
| NRC update | 0.001 | 0.027 | 0.027 |
| **Indirect** | **0.553** | **0.694** | **0.696** |
| NRC train / resolve | 0.001 | 0.409 / 0.041 | 0.405 / 0.041 |
| ReSTIR GI temporal | 0.117 | 0.102 | 0.001 |
| **ReSTIR GI shade** | **0.744** | **0.915** | 0.038 |
| Combine | 0.051 | 0.050 | 0.008 |
| **DLSS-RR** | **1.211** | **1.109** | **1.262** |
| Post + Present + ImGui | 0.044 | 0.044 | 0.045 |
| **GPU total** | **3.327 ms** | **3.926 ms** | **3.057 ms** |
| Async compute (overlapped): light prep / RT inst / PTLAS | 0.001 / 0.001 / 0.018 | 0.001 / 0.001 / 0.016 | 0.001 / 0.001 / 0.016 |
| **Active CPU (`drawFrame − waits`)** | **0.504 ms** (`FrameData::write` 0.001) | **0.641 ms** (`waits` +1.17 ms C-G3-6) | **0.705 ms** |

**Sponza courtyard (sunlit pose `lookAt({6,2.5,1.5}, {-10,1,-1})`, 1,200 measured frames):**

| Scope (mean ms) | `ReSTIR GI` (default) | `NRC + ReSTIR GI` | `NRC` |
|---|---:|---:|---:|
| Early + Late cull / raster / Hi-Z | 0.125 | 0.128 | 0.126 |
| Material classify + Vis-buffer resolve (F11) | 0.117 (0.018 + 0.099) | 0.114 (0.018 + 0.097) | 0.115 (0.017 + 0.098) |
| **TLAS wait (F4)** | **0.016** | **0.013** | **0.014** |
| ReSTIR DI initial / temporal / shade | 0.241 / 0.141 / 0.322 (**0.704**) | 0.214 / 0.122 / 0.292 (**0.627**) | 0.212 / 0.122 / 0.286 (**0.619**) |
| NRC update | 0.000 | 0.075 | 0.074 |
| **Indirect** | **0.736** | **0.816** | **0.806** |
| NRC train / resolve | 0.001 | 0.683 / 0.045 | 0.640 / 0.044 |
| ReSTIR GI temporal | 0.122 | 0.104 | 0.001 |
| **ReSTIR GI shade** | **0.823** | **0.851** | 0.040 |
| Combine + Post + Present + ImGui | 0.094 | 0.081 | 0.055 |
| **DLSS-RR** | **1.216** | **1.184** | **1.114** |
| **GPU total** | **3.954 ms** | **4.720 ms** | **3.645 ms** |
| **Active CPU (`drawFrame − waits`)** | **0.511 ms** (`FrameData::write` 0.001) | **0.758 ms** | **0.710 ms** |

### 2. Re-measured findings (F4, C-F1-5, F7, F8, F9, F11, F12)

- **F4 (`TlasWait`) & F9 (async-compute overlap, RTX 5070):** Hardware GPU-clock timestamps across graphics (`[gfxStart, gfxResolveEnd]`) and async compute (`[lightPrepStart, ptlasEnd]`):

| Scene | Async compute (ms) | Pre-ray gfx (ms) | Start offset vs `gfxStart` | Overlap (ms) | Spill past resolve (ms) | `TLAS wait` mean (ms) |
|---|---:|---:|---:|---:|---:|---:|
| Sponza gallery still | 0.021 | 0.216 | +0.047 | **0.021 (100%)** | 0.000 | 0.018 |
| Sponza full PTLAS (103w) | 0.114 | 0.217 | +0.041 | **0.114 (100%)** | 0.000 | 0.070 |
| Field 64 still | 0.021 | 0.242 | +0.038 | **0.021 (100%)** | 0.000 | 0.018 |
| Field 64 moving (4,096w) | 0.193 | 0.258 | +0.047 | **0.193 (100%)** | 0.000 | 0.118 |
| Bistro `Power_RIS` (76K lights) | 0.085 | 0.230 | +0.036 | **0.085 (100%)** | 0.000 | 0.035 |
| Bistro `ReGIR_RIS` (76K lights) | **0.499** | 0.345 | +0.032 | **0.313 (63%)** | **0.186** | **0.385** |

  - Async compute genuinely overlaps pre-ray graphics on the RTX 5070 (`0.193 ms` full overlap on Field 64 moving; `0.313 ms` concurrent overlap on Bistro `ReGIR`).
  - Two latency findings: (1) `rayTimeline` is signaled after `DLSS-RR` (`1.21 ms`), delaying async compute's start until `+0.03–0.05 ms` after `gfxStart`; (2) `QueueSubmitRayPresent` waits on CPU `AcquireImage` + `RecordPresentCommands` before submitting `rayCmdInfos`, adding submit-gap latency to `TLAS wait` on non-trivial compute submits.
  - **Nsight Graphics / Nsight Systems note:** CLI `ngfx --activity="GPU Trace Profiler"` returned `TARGET ERROR: GPU Performance Counters unavailable. Please enable access to GPU performance counters` (NVIDIA Control Panel developer permission), and `nsys --trace=vulkan` requires admin HKLM registry write permission. Hardware-counter per-pass Nsight captures wait on enabling GPU Performance Counters in NVIDIA Control Panel; hardware GPU-clock overlap and per-pass timings above were captured directly via Vulkan timestamp queries.

- **F7 (PTLAS update minimum cost):** Confirmed post-I3 on RTX 5070: still PTLAS `0.017–0.022 ms` (Sponza and Field 64); Sponza forced full build (103w) `0.123 ms`; Field 64 moving incremental (4,096w) `0.187 ms` vs forced full build (4,199w) `0.194 ms`. Fully overlapped with pre-ray graphics on the 5070.
- **F8 (O(entities) `FrameData::write` against 2 ms CPU budget):**
  - Sponza (1 entity): active CPU `0.504 ms`, `FrameData::write` **`0.0012 ms`**.
  - Bistro (2,909 instances, 1 entity): active CPU `0.559 ms`, `FrameData::write` **`0.060 ms`**.
  - Field 64 still (4,097 entities): active CPU **`1.012 ms`**, `FrameData::write` **`0.444 ms`** (~0.11 µs/entity → hits 2.0 ms active CPU budget around ~14,000 entities). Defers to M9 / AU4.
- **F11 (Binned vis-buffer resolve):** `Classify` `0.018 ms` + `Resolve` `0.097 ms` = **`0.115 ms`** on Sponza gallery (`0.117 ms` courtyard, `0.110 ms` Field 64, `0.080 ms` Bistro `Power_RIS`). At 3.0–3.5% of GPU total (~0.045 ms overhead over single dispatch), F11 does not rank above lighting/RT levers.
- **F12 & C-F1-5 (`ReGIR_RIS` vs `Power_RIS` vs `Uniform` on Bistro, 76,248 triangle lights in 115 emissive instances, ABBA 600 frames × 4):**

| Metric (mean ms) | `ReGIR_RIS` (A1 / A2, mean) | `Power_RIS` (B1 / B2, mean) | `Uniform` (single) | `Power_RIS` vs `ReGIR_RIS` |
|---|---:|---:|---:|---:|
| Async `light prep` total | 0.475 / 0.480 (**0.477**) | 0.057 / 0.059 (**0.058**) | 0.026 | **−0.420 ms (−87.9%)** |
| — `world` / `mips` / **`presample`** | 0.024 / 0.012 / **0.442** | 0.024 / 0.011 / **0.022** | 0.025 / 0.001 / 0.001 | **presample 20.1× faster** |
| `TLAS wait` (graphics stall) | 0.398 / 0.371 (**0.385**) | 0.042 / 0.027 (**0.035**) | 0.025 | **−0.350 ms** |
| `Vis-buffer resolve` (contention) | 0.092 / 0.093 (**0.092**) | 0.062 / 0.058 (**0.060**) | 0.059 | **−0.032 ms** |
| `ReSTIR DI initial` | 0.196 / 0.198 (**0.197**) | 0.214 / 0.211 (**0.212**) | 0.241 | +0.015 ms |
| **GPU total** | 3.064 / 3.031 (**3.047 ms**) | 2.692 / 2.563 (**2.628 ms**) | 2.598 ms | **−0.420 ms (−13.8%)** |

  `rtxdi_presample_regir` fills 2,229 cells × 512 slots × 8 samples = 9.13M light evaluations every frame vs `Power_RIS`'s 131K (70× fewer), overflowing the raster window by 0.186 ms and adding GPU contention during vis-buffer resolve.

### 3. Ranked list of levers (orders I6 → I5 → I10 → I9)

| Rank | Step | Lever(s) | Target Scope(s) | Expected Gain (RTX 5070) |
|---:|---|---|---|---|
| **1** | **I6 · ReSTIR DI tuning** | **F12** (`Power_RIS` default vs `ReGIR` on Bistro), **F5** (DI ray budget: spatial/temporal/final vis), **O8** (skip unused BRDF ray without emissives/env) | Bistro `light prep` `0.477 ms` + `TLAS wait` `0.385 ms`; sunlit Sponza DI **`0.704 ms`** (gallery `0.351 ms`) | **−0.42 ms (−13.8%) on Bistro** (F12 ABBA-proven) + **−0.10 to −0.30 ms on sunlit Sponza** (F5 + O8) |
| **2** | **I5 · ReSTIR GI tuning** | Ray-traced spatial correction vs sample count/radius, O11 follow-ups (age limit, duplication map, swap bound), screen-edge tuning | `ReSTIR GI shade` **`0.744 ms` gallery / `0.823 ms` courtyard** (#1 engine pass, 21–22% of frame) | **−0.10 to −0.25 ms** |
| **3** | **I10 · Mega Geometry perf** | `MG-P2` (7-bit position truncation), `MG-P4` (drop 36 B dead pos load + `normalMatrix` ALU), `MG-P3` (spatial meshlets), `MG-P1` (split OMM CLAS), `MG-P5`/`MG-P7` (+ signaling `rayTimeline` before `DLSS-RR` when NRC is paused) | All ray passes combined (`DI + Indirect + GI shade` = **1.65–2.26 ms**, 50–57% of GPU frame) + `TLAS wait` on moving scenes | **−0.10 to −0.25 ms** across ray passes + **−1.8 MiB** CLAS VRAM + **−0.05 to −0.10 ms** moving `TLAS wait` |
| **4** | **I9 · RTXDI surface traffic** | **O4**: compact packed surface guide (36 B → ~20 B per neighbor, position from depth) | `DI temporal/shade` + `GI temporal/shade` = **1.06–1.41 ms** | **−0.05 to −0.15 ms** (larger share on RTX 2060 Super) |
| — | *Deferred (do not rank)* | **F11** (binned resolve `0.115 ms`, ~0.045 ms overhead), **F8** (`FrameData::write` `0.001–0.444 ms`, active CPU `0.50–1.01 ms` vs 2 ms budget) | < 3.5% GPU / < 50% CPU budget | No separate step; F8 waits for M9 AU4 |

### 4. Verification

- **Vulkan validation** (core + sync + best practices + NVIDIA best practices, GPU-AV off per C-E1-3; `ENGINE_INDIRECT_MODE=cycle`, 120 frames): **clean** (only known NGX C-H1-2 and NRC C-G3-3 messages, 29 lines).
- **Regression:** CTest **7/7**, `ENGINE_RT_CHECK=1` **32/32** (zero shader or rendering changes in I4).

## Done when

- [x] `GpuTimerScope::TlasWait` (F4) and `RtxdiContext` async light preparation timer (C-F1-5) built and validation-clean
- [x] Per-scope tables recorded in all three indirect modes (`ReSTIR GI`, `NRC + ReSTIR GI`, `NRC`) on gallery and courtyard
- [x] Frame limiters analyzed across Sponza, Field 64, and Bistro *(note: Nsight Graphics hardware PM counter capture requires enabling GPU Performance Counters in NVIDIA Control Panel)*
- [x] CPU frame and `FrameData::write` measured against the 2 ms budget (F8) on Sponza, Bistro, and Field 64
- [x] F7 (PTLAS update), F9 (async-compute overlap via GPU-clock timestamps, RTX 5070), F11 (binned resolve), and F12 (`ReGIR_RIS` vs `Power_RIS` ABBA on Bistro) measured
- [x] Ranked list of levers with expected gains recorded, ordering **I6 → I5 → I10 → I9**
