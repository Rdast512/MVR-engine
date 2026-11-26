# M0–M7 follow-up · Changes to reach the 2026-11-25 engine spec target

**Date:** 2026-11-26 · **Status:** planned; each item is owned by an M8 step and lands there · **Baseline:** `feature-pbr` `3d24416c6` + working tree
**Target:** [engine spec](../../docs/engine_spec.md) §1.1, §2.3, §5, §11.2–11.7, §17 (2026-11-25 row): Direct `Off` / Streaming RIS / ReSTIR DI (target: Streaming RIS or Temporal-only `Basic`, history 4–8, Spatial off), Indirect `Off` / Raw 1-Bounce PT / ReSTIR GI (target: Raw PT), Cache `Off` / NRC / SHaRC (target: NRC), Denoiser DLSS-RR / `Off`; a disabled stage costs 0 ms.
[README](./README.md) · [M8](../m8/README.md) · [M8.1 audit](../m8/m8_1_pipeline_audit.md) · [Considerations](../considerations.md)

Every M0–M7 step spec was read against the engine spec. This document lists what each built step must change so the tree matches the new target, and which M8 step owns the change. Bugs and perf levers already in the M8.1 audit are referenced by their ids (`BUG-*`, `PERF-*`), not restated.

M0, M1, M2, M6 and M7 E1/E2 match the target's geometry, visibility and binding rules (§3, §4, §8–§10); their items are contract extensions for the new toggles and stale text. M3 F1–F3 and M4 G2–G7 carry the real changes: they were built around ReSTIR DI + ReSTIR GI as the shipped path.

---

## 1. Code changes

### 1.1 Cross-cutting rules for every item

| Rule | Source | Applies to |
|---|---|---|
| A disabled stage records no dispatch, no barrier and no SDK call, and its transient buffers are not allocated | spec §11.7, M8 §4 | every `Off` below, and modes that do not read a buffer (DI / GI reservoirs, ReGIR cells) |
| A consumer of a disabled stage's output reads a frame flag and skips the load; nothing dispatches to write zeros | spec §11.6 ("contribute zero without being sampled") | combine, glass, debug views |
| A toggle switches at a frame boundary without a reload and resets only the histories it invalidates, for one frame | spec §11.7 | `PipelineState` change detector (TF-M4-8) |
| One shading body per ray pass, separate from its trace, so the post-1.0 SER variant wraps it | spec §16.5 V1, [g1_ser_followups.md](../m4/g1_ser_followups.md) §4 | Raw PT fast path, Streaming RIS pass, SHaRC update |
| No second indirect pass or path loop: modes specialize `indirect.slang` by specialization constant | spec S9, [g7_indirect_modes.md](../m4/g7_indirect_modes.md) Must not | TF-M4-1, TF-M4-5 |
| Every new trace site goes through `traceVisibility` / `traceClosest` (OMM 2-state flag, `kRayMaskOpaque`, candidate counter) | [d4_ray_contract.md](../m6/d4_ray_contract.md) | TF-M3-2, TF-M4-5 |
| NVIDIA's sets stay callable; a tuned default keeps NVIDIA's set behind its button and in a measured check phase | README §Rules, C-M4-2 | TF-M3-4, TF-M4-6 |

### 1.2 M0 · Contracts

| ID | Step | Today | Change | Owner |
|---|---|---|---|---|
| TF-M0-1 | [A1](../m0/a1_gpu_contracts.md), [A2](../m0/a2_slang_modules.md) | `GpuFrameRoot.cacheConstants` is NRC's `GpuNrcConstants`. Frame flags: `HistoryReset` (bit 0), `IndirectHistoryReset` (bit 1). No stage toggle reaches shaders | Add `sharcConstants` beside `cacheConstants` (zero when SHaRC is inactive; keeps the NRC mirror and its layout test unchanged). Add frame flags `DirectOff`, `IndirectOff` (combine, glass, debug views skip the load) and `DirectHistoryReset` (Direct mode change). Mirror in `types.slang`; extend `tests/layout_test.cpp` | M8.3 (flags), M8.4 (`sharcConstants`) |
| TF-M0-2 | A1 device properties | SHaRC's `shaderBufferInt64Atomics` was dropped with C-G3-1 | Re-add it to the required feature table; Turing reports it. Device creation names it when missing, like the other required features | M8.4 |
| TF-M0-3 | [A10](../m0/a10_culling_baseline.md) | `[Bench]` aggregates raster, cull and resolve stages; lighting scopes are per pass (`NrcUpdate`, `Indirect`, `GiTemporal`, …) | Bench prints the §2.3 rows: Direct, Indirect bounce, Radiance cache (NRC or SHaRC scopes), ReSTIR GI. A disabled stage reads `0.000`, which is the 0 ms acceptance test. C-M0-1's floor-tier baseline is recorded with the target stack in M8.6, not with the M0 frame | M8.3 (rows), M8.6 (floor) |

### 1.3 M1 · Visibility buffer

| ID | Step | Today | Change | Owner |
|---|---|---|---|---|
| TF-M1-1 | [V1](../m1/v1_render_targets.md), [E1](../m1/e1_resolve_guides.md) | Two guide sets ping-ponged for temporal consumers | No code change. Streaming RIS + Raw PT + NRC reads no previous guide, but ReSTIR DI Temporal-only (one of the two target DI modes) and DLSS-RR's history need them, and the ping-pong costs VRAM only. Re-check only if M8.6's floor-tier VRAM needs it | — |

Culling, Hi-Z, raster bins and the vis-buffer payload are unaffected (§8). V8 / Q3 stay deferred.

### 1.4 M2 · Cluster RT

| ID | Step | Today | Change | Owner |
|---|---|---|---|---|
| TF-M2-1 | [C1](../m2/c1_cluster_acceleration_structures.md), [C2](../m2/c2_gpu_instances_async_compute.md), [C4](../m2/c4_partitioned_tlas.md) | M8.1 found missing AS-read access on the instance barrier, no barrier after BLAS builds, host-visible build inputs and PTLAS scratch sized for builds only (C-GA-1 / C-T-6) | Land `BUG-B1`–`BUG-B4`, `BUG-B6`; rerun C3's `ENGINE_RT_CHECK` and C4's still / moving PTLAS timings after them | M8.1 |
| TF-M2-2 | C2, C3 | `GpuRayInstance` (8 B) → `nodeInstances` → `GpuModel` (112 B) chain in every hit decode | `PERF-B2`: compact ray-instance row. Changes A1's 8 B contract and C3's `decodeHit`; RT check must stay 100% | M8.5 |
| TF-M2-3 | C2 | Async compute overlap capture open | Capture on both GPUs with the target stack (§13.1 X2) | M8.6 |

### 1.5 M3 · First lit frame

| ID | Step | Today | Change | Owner |
|---|---|---|---|---|
| TF-M3-1 | [F3](../m3/f3_restir_di.md), [F1](../m3/f1_rtxdi_context.md) | Direct lighting always runs. A no-light frame still dispatches `rtxdi_shade` to zero `directRadiance` | `DirectMode { Off, StreamingRis, RestirDi }`. `Off`: no RTXDI pass, no DI reservoirs, `DirectOff` flag in combine (TF-M0-1). The no-light frame takes the same flag path instead of the zeroing dispatch. `light_world` keeps running (glass, X₂ NEE, debug views); `light_pdf_mips` and presampling run only while a consumer exists (TF-M7-2) | M8.3 |
| TF-M3-2 | F3 | No-reuse DI exists only as `isReference` (`ResamplingMode::None` with 32 local / 8 infinite reference candidates), so Streaming RIS is conflated with the ground truth | Streaming RIS: initial sampling with the shipped candidate counts, one final visibility ray, no reservoir store or load, written straight to `directRadiance` (`PERF-C1`, from `reference/RTXPT-main/Rtxpt/Shaders/IntroSample/RTXDIStreamingRISLightingPass.hlsl`). `isReference` stays the convergence reference only. F1 allocates no DI reservoir array in this mode | M8.3 (mode), M8.5 (fused pass) |
| TF-M3-3 | F3, F1 | `isTemporalOnly` (RTXDI `ResamplingMode::Temporal`) is a lab flag; temporal and shade stay two passes; F1 allocates the spatial array regardless | Promote to the ReSTIR DI `Temporal` sub-mode with a target set: `maxHistoryLength` 4–8, `Basic` temporal bias correction, spatial off. Fused `rtxdi_temporal_shade` without spatial code (`PERF-C2`). F1 allocates only the arrays the mode reads | M8.3 (mode), M8.5 (fused pass, history value) |
| TF-M3-4 | F3, F1 | `defaultSettings()` = NVIDIA `Medium` (`TemporalAndSpatial`); `tunedSettings()` = the I6–I16 set for NRC + ReSTIR GI | Shipped Direct default = Streaming RIS or Temporal-only `Basic`, picked by M8.5 numbers (spec §11.2). `Medium` becomes the RTXDI Analog preset's DI. `tunedSettings()` stays callable but is no longer a tuning target; its I16 parts follow `BUG-E5` / `BUG-D5` | M8.3 (presets), M8.5 (pick) |
| TF-M3-5 | F2 | Bridge defects from the M8.1 audit | `BUG-C1` (initial visibility in `None`), `BUG-C2` (even spatial counts), `BUG-C3` (skip unlit pixels) | M8.1 |
| TF-M3-6 | [H1](../m3/h1_dlss_ray_reconstruction.md) | RR history resets on camera cuts, layout and light-set changes, debug/Lit transitions and `IndirectHistoryReset`. Denoiser `Off` + `Accumulation` built, GPU gates not run (C-NS-1) | RR (or `Accumulation`) resets on every pipeline-axis change through TF-M4-8's detector. `Off` skips evaluation entirely. Set `FreeMemOnReleaseFeature` (`BUG-E8`); release the feature while `Off` only if the floor tier's VRAM needs it | M8.2 (C-NS-1), M8.3 |

B1–B3, E1–E3 and H2 are unaffected; `BUG-A1`–`BUG-A3` (BRDF) are M8.1 fixes, not target changes.

### 1.6 M4 · Indirect

| ID | Step | Today | Change | Owner |
|---|---|---|---|---|
| TF-M4-1 | [G2](../m4/g2_indirect_pass.md), [G6](../m4/g6_combine.md) | The indirect pass always writes an RTXDI GI reservoir (initial array); in `Nrc` mode (`kDefaultIndirectMode`) `nrc_resolve` adds the cache term into it and `rtxdi_gi_shade` shades it without reuse into `indirectRadiance` | Raw 1-Bounce PT fast path (`PERF-D1`): `indirect.slang`, specialized when ReSTIR GI is off, writes `w₁ · Lo(X₂)` to `indirectRadiance` (`w₁ = f · cos / pdf`). With NRC, `nrc_resolve` adds `ao · w₁ · prefixThroughput · cache` to `indirectRadiance`. Where `w₁` lives (NRC query throughput or an RGB16F target) is decided in the M8.3 spec from RTXGI `NrcIndirectDiffuse.hlsli`. No `rtxdi_gi_shade` dispatch, no LogLuv round trip | M8.3 (mode), M8.5 (pass) |
| TF-M4-2 | G2 | `directLightAt` at X₂ uses the primary's local sampling mode and counts; O2 (secondary DI resampling) reads the DI reservoirs | Decouple X₂ NEE: its own mode (`ReGIR_RIS` / `Power_RIS` / simple) and 2–8 candidates (`PERF-D4`). O2, O6 and O9 are valid only with ReSTIR GI, and O2 only while Direct = ReSTIR DI (no DI reservoirs otherwise): the settings layer forces them off outside those modes | M8.3 (rules), M8.5 (counts) |
| TF-M4-3 | G2 | `specHitDistance` is written by the indirect pass (from its own ray) and by `rtxdi_gi_shade` (from the reconnection) | No change at full resolution. If the floor tier's half-resolution indirect lands (C-G2-1), `specHitDistance` and `indirectRadiance` keep a full-resolution writer (spec §9.3) | M8.6 |
| TF-M4-4 | [G3](../m4/g3_nrc.md) | NRC runs only in the `NrcRestirGi` and `Nrc` modes; `ReSTIR GI` mode pauses it | NRC follows the Cache axis: `Cache = NRC` runs update, query, `QueryAndTrain`, resolve; `Off` / `SHaRC` pause it (context and weights kept, C-G3-3, C-G3-5). Indirect `Off` pauses it too. `includeDirectLighting = false`, the C-G3-7 sentinel and the isolated SDK segments stay. Land `BUG-E1`–`BUG-E4`, `BUG-E6`, `BUG-E7`; `BUG-E5` goes with the I16 cleanup (TF-M3-4) | M8.1 (bugs), M8.3 |
| TF-M4-5 | G3, new | SHaRC removed (C-G3-1) | `Cache = SHaRC`: `sharc_update` (sparse paths, 1 per 4×4 block) over `path_shading`'s `scatterBrdf` / `directLightAt`, `sharc_resolve`, query inline in `indirect.slang` at X₂, lazy BDA hash / accumulation / resolved buffers, `sharcConstants` (TF-M0-1), `shaderBufferInt64Atomics` (TF-M0-2), timers, debug view. Ported from `reference/RTXPT-main/Rtxpt/Sharc/` and `ThirdParty/_deps/src/sharc` | M8.4 |
| TF-M4-6 | [G5](../m4/g5_restir_gi.md) | ReSTIR GI's 3 arrays (38.0 MiB at 853×480), stagnancy, sample-id and duplication targets are allocated at every resize | Off by default (spec §11.5). Allocate its arrays and targets only while Indirect = ReSTIR GI (`PERF-D2`, −112 B/px); a switch to it resets GI history (exists through `IndirectHistoryReset`). `nvidiaGiSettings`, `tunedGiSettings`, `convergedGiSettings` stay, gated in `ENGINE_GI_CHECK`. Land `BUG-D1`, `BUG-D3`, `BUG-D4`, `BUG-D6` | M8.1 (bugs), M8.3 |
| TF-M4-7 | G5, G7, `ENGINE_GI_CHECK` | Gated phases are built around ReSTIR GI and the two NRC modes; the ground-truth paths run through the GI reservoir with resampling `None` | Ground truth (`giReferenceBounces`) writes `indirectRadiance` through TF-M4-1's path. Phases: Raw PT + NRC vs the multi-bounce truth (today's `NRC mode`, gated since I7); Raw PT + SHaRC vs the same truth (new, M8.4); Raw PT fast path vs today's reservoir path (same mean within the noise floor); the ReSTIR GI phases unchanged | M8.3, M8.4 |
| TF-M4-8 | [G7](../m4/g7_indirect_modes.md) | `IndirectMode { NrcRestirGi, Nrc, RestirGi }`, `ENGINE_INDIRECT_MODE`, `IndirectState` change detection, guide capture per mode | Replace with independent axes `DirectMode`, `IndirectMode { Off, RawPt, RestirGi }`, `CacheMode { Off, Nrc, Sharc }`, `DenoiserMode` (exists), plus presets Engine Target / RTXPT / RTXDI / RTXGI (spec §11.7). Old modes map to `RestirGi + Nrc`, `RawPt + Nrc`, `RestirGi + Off`. One `PipelineState` detector sets `DirectHistoryReset`, `IndirectHistoryReset` and the RR / accumulation reset per changed axis. `ENGINE_INDIRECT_MODE` becomes `ENGINE_CONFIG` keys in `SettingsRegistry`; `cycle` cycles presets. Guide capture and the temporal runners iterate presets. G7's rules stay: no NRC context recreation, one indirect pass, a default changes only by spec decision | M8.3 |

[G1](../m4/g1_ser_rt_pipeline.md) stays shelved (C-G1-3, spec §16.5 V1); TF-M4-1 and TF-M4-5 keep its shading-body rule (§1.1).

### 1.7 M6 · Alpha

| ID | Step | Today | Change | Owner |
|---|---|---|---|---|
| TF-M6-1 | [D1](../m6/d1_omm_bake.md) | The OMM SDK keeps `alpha > cutoff`, raster keeps `alpha >= cutoff`; `alphaCutoff` is passed verbatim | `BUG-B5`: pass `nextafter(alphaCutoff, -inf)` in `OmmBaker::bake`, as RTXPT's `OmmBuildQueue.cpp`; rerun D4's `alpha-edge` counts | M8.1 |
| TF-M6-2 | D1, D2 | 4-state bake, forced 2-state at trace (spec §7.1 step 5) | `PERF-B1` (`OC1_2_State`, half the OMM memory) is a spec change: land it only with §7.1 step 5 and D1 updated together | M8.5 |

D3's alpha bin and D4's ray contract are unaffected; the contract applies to the new trace sites (§1.1).

### 1.8 M7 · Material binning and emissive lights

| ID | Step | Today | Change | Owner |
|---|---|---|---|---|
| TF-M7-1 | [E1](../m7/e1_material_binning.md) | GPU-AV needs `ENGINE_GPUAV_RESOLVE_PER_BIN=1` (C-E1-3) | No change. M8.6's GPU-AV sweep over every preset runs with the switch until a layer release closes C-E1-3 | M8.6 |
| TF-M7-2 | [F1](../m7/f1_light_presampling.md) | `light_pdf_mips`, `rtxdi_presample` and `rtxdi_presample_regir` run every frame; the ReGIR and environment RIS segments are always allocated (56.6 MiB of RIS buffers) | Presampling runs only while a consumer exists (Direct ≠ `Off` with `Power_RIS` / `ReGIR_RIS`, or X₂ NEE using them); ReGIR cells and their pass only while primary or X₂ NEE uses `ReGIR_RIS`, environment segment `{1, 1}` until an environment light exists (`PERF-C4`, −49 MiB). Land `PERF-C3` (5 mips per dispatch, wave + LDS) | M8.3, M8.5 |
| TF-M7-3 | [E2](../m7/e2_emissive_triangles.md), README | "Bistro emissives lit" never run; `BUG-A4` / `BUG-A5` (compact triangle-light quantization, back-face emission on single-sided hits) | Land the bugs in M8.1; run the Bistro emissive check under the Engine Target preset in M8.6 | M8.1, M8.6 |

E2's one-emission rule holds in every mode: with Direct `Off`, light triangles stop lighting X₁ by design, and X₂ NEE still samples them.

---

## 2. Check runners

| Runner | Change | Owner |
|---|---|---|
| Guide capture, ReSTIR DI convergence | Add Streaming RIS and Temporal-only `Basic` shots against the `RtxdiNone` reference, same gates (\|bias\| ≤ 0.5%, tile p90 ≤ 3%); a gate changes only with numbers. `convergedSettings` keeps its shot | M8.3 |
| `ENGINE_GI_CHECK` | TF-M4-7 | M8.3, M8.4 |
| `ENGINE_STABILITY_CHECK`, `ENGINE_LIGHT_CHANGE_CHECK`, `ENGINE_SCREEN_EDGE_CHECK` | Run per preset; the I1 baseline is re-recorded for Engine Target on Sponza and `bistro_cafe` | M8.6 |
| `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST` | Unchanged; run once per preset in the M8.6 sweep | M8.6 |
| Settings lab | Cases and plans read the new axes; `tests/lab_registry_test.cpp` covers the presets | M8.2, M8.3 |

---

## 3. Stale text in the M0–M7 step specs

Fix in the step spec when its owning M8 step touches the code, or in one doc pass now. Facts owned elsewhere (spec §20.4) are linked, not copied.

| Spec | Stale statement | Current (owner) |
|---|---|---|
| [m0/a1](../m0/a1_gpu_contracts.md) | `GpuFrameRoot` 192 B, `GpuTargets` 200 B, root offsets | both grew in M6–M8 (D4 `rayDebug`, E1 bins, I13 glass, I16 hit distance); cite the `static_assert`s in `types.hpp` instead of numbers (spec §20.4) |
| m0/a1, [m0/README](../m0/README.md) | "M4 uses the SER properties" | SER is post-1.0 V1 (spec §16.5) |
| [m0/a3](../m0/a3_cleanup.md) | "The remaining SHaRC cleanup belongs to M4 G3, under C-G3-1" | SHaRC returns in M8.4 (spec §11.4, C-G3-1 note) |
| [m1/e1](../m1/e1_resolve_guides.md), [m3/f2](../m3/f2_rtxdi_bridge.md) | `GeoNormal`, `SpecularF0`, `Position` (RGBA32F) guides | one packed `Surface` guide (RGBA32_UINT), position rebuilt from view depth (M8 O4 / I9; spec §9.3); `specHitDistance` R16F (I16) |
| [m2/c1](../m2/c1_cluster_acceleration_structures.md) | one CLAS build call per model | opaque and OMM clusters in two calls (MG-P1), spatial meshlets (MG-P3; spec §7.1, §10.2) |
| [m2/c3](../m2/c3_ray_hit_decode.md) | "M4 adds a separate SER-to-query comparison" | shelved with G1 (C-G1-3) |
| [m2/c4](../m2/c4_partitioned_tlas.md) | XZ grid of ~64 instances per cell; a moved instance keeps its cell (C-C4-5); mask `0xFF` | Z-order grid refined above 1,024 per cell (MG-P5), moved entities reassigned (MG-B4), `kRayMaskOpaque` / `kRayMaskGlass` (I13; spec §10.3, §12.4) |
| [m3/f1](../m3/f1_rtxdi_context.md), [m3/f3](../m3/f3_restir_di.md) | `TemporalAndSpatial` with uniform local sampling is the runtime mode | `Power_RIS` default (I6 F12, spec §17); target Direct modes of TF-M3-2 / TF-M3-3 |
| [m4/g2](../m4/g2_indirect_pass.md) | output is always the initial GI reservoir | TF-M4-1 |
| [m4/g3](../m4/g3_nrc.md) | "SHaRC removed"; host wait before the next frame's ray recording | SHaRC toggle (M8.4); host wait off since I8 |
| [m4/g5](../m4/g5_restir_gi.md) | ReSTIR GI runs in the shipped modes | optional toggle, off in the target (spec §11.5) |
| [m4/g7](../m4/g7_indirect_modes.md) | 3-way `IndirectMode`; tier default `ReSTIR GI` (2026-11-20) | superseded by TF-M4-8 and spec §11.7 |
| [m7/README](../m7/README.md), m7/f1 | default local sampling `ReGIR_RIS` (`Medium`) | `Power_RIS` default; `ReGIR_RIS` callable (spec §11.2, §17) |

---

## 4. Order

```
M8.1: TF-M2-1, TF-M3-5, TF-M4-4/6 bugs, TF-M6-1, TF-M7-3 bugs
  └─► M8.2: TF-M3-6 (C-NS-1)
        └─► M8.3: TF-M0-1 flags, TF-M0-3, TF-M3-1..4, TF-M4-1/2/4/6/7/8, TF-M7-2, §2 runners
              ├─► M8.4: TF-M0-1 sharcConstants, TF-M0-2, TF-M4-5, TF-M4-7 SHaRC phase
              └─► M8.5: fused passes (TF-M3-2/3, TF-M4-1), TF-M2-2, TF-M4-2 counts, TF-M6-2, TF-M7-2 perf, DI default pick
                    └─► M8.6: TF-M0-3 floor, TF-M2-3, TF-M4-3, TF-M7-1, TF-M7-3 Bistro, §2 per-preset sweep
```

TF-M4-8 comes first inside M8.3: the axes and the `PipelineState` detector are what every other M8.3 item switches on.

## 5. Done when

- every TF item is landed in its M8 step with that step's numbers (ABBA bench, release, no layers), or closed with its numbers;
- every disabled stage reads `0.000` ms in the §2.3 bench rows, with no dispatch in an Nsight capture;
- §3's stale statements are fixed;
- the §2 runners pass for every preset, and validation (GPU-AV with the C-E1-3 switch) is clean.
