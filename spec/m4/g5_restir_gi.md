# M4 · G5: ReSTIR GI

**Started:** 2026-11-18 · **Status:** built 2026-11-18; ReSTIR GI converges to the ground truth (§3). Open: 2060 Super numbers. The after-DLSS-RR check passed with G6 (2026-11-19) · **Milestone:** M4
**Implements:** engine spec §11.5, the §14 ground truth; plan G5 ([pbr_implementation_plan.md](../pbr_implementation_plan.md))
**Depends on:** [G2](./g2_indirect_pass.md) (initial samples), [G3](./g3_nrc.md) (resolved sample radiance), F1 (RTXDI context)
**Used by:** [G6](./g6_combine.md), [G7](./g7_indirect_modes.md) (runs in the NRC → ReSTIR GI and ReSTIR GI modes; one-bounce samples when NRC is paused)
**Related:** [rtxdi_3_2_update.md](../features/done/rtxdi_3_2_update.md) (pinned RTXDI version)
**Reference:** RTXDI 3.2 sample (`reference/RTXDI`, commit `fdbd9b3` "3.2 Update", cloned 2026-11-18): `Samples/FullSample/Shaders/LightingPasses/GI/{TemporalResampling,SpatialResampling,FinalShading}.hlsl`, `RtxdiApplicationBridge/RAB_{LightSampling,SpatialHelpers,VisibilityTest}.hlsli`, `Source/RenderPasses/LightingPasses/ReSTIRGIRenderPasses.cpp`, `UserInterface.cpp`
**Considerations:** C-G5-1 (sample age limit), C-G5-2 (Basic bias correction), C-G5-3 (NRC bias against the ground truth)

RTXDI ReSTIR GI on the indirect pass's samples: temporal resampling, then spatial resampling and final shading into `indirectRadiance` (RGBA16F, render resolution). A ground-truth mode of the indirect pass and the `ENGINE_GI_CHECK` runner check it against engine spec §14.

---

## 1. Decisions

| Decision | Rule | Why |
|---|---|---|
| Passes | `rtxdi_gi_temporal` (temporal + boiling filter, 8×8, as the sample's `GI/TemporalResampling.hlsl`), then `rtxdi_gi_shade` (spatial + final shading, the sample's `GI/SpatialResampling.hlsl` and `GI/FinalShading.hlsl` fused), each behind a memory barrier, after the NRC resolve in `Renderer::recordIndirectResampling` | the sample runs temporal, spatial and final shading as three passes; fusing spatial and shading saves a dispatch and a reservoir round trip, as `rtxdi_shade` does for DI |
| Resampling mode | `TemporalAndSpatial`, the sample's default; `None` is the reference (`RtxdiContext::isGiReference`), which skips the temporal pass and shades the initial sample alone | the reference is the 1-sample estimate the reuse must converge to |
| Reservoir arrays | 3: RTXDI's two plus `kGiInitialReservoirArray` (2), where the indirect pass and the NRC resolve write the initial samples. The temporal pass reads that array and writes `temporalResamplingOutputBufferIndex` (0); the shade pass reads its neighbors from 0 and writes `spatialResamplingOutputBufferIndex` (1), next frame's temporal input. 38.0 MiB at 853×480 render (1280×720 window) | the final MIS needs each pixel's initial sample after the temporal pass, and with two arrays the temporal output overwrites it. The sample keeps it in a separate secondary G-buffer instead |
| No history (reset frame) | the temporal pass copies the initial sample to its output instead of early-returning | the spatial pass then always reads this frame's reservoirs |
| Settings | `RtxdiContext::GiSettings`, applied by `prepareFrame` like DI's. `defaultGiSettings()`: the library defaults, which the sample uses without a preset (history 8, age 30, temporal and spatial bias correction Basic, fallback on, 2 spatial samples in 32 px, boiling filter 0.2, final visibility and final MIS on). `convergedGiSettings()`: ray-traced bias correction, no age limit (`kGiUnlimitedSampleAge`), no fallback, permutation or boiling filter | NVIDIA's defaults for the cost comparison (C-M4-2); the check pins its own (§3) |
| Target function | `RAB_GetGISampleTargetPdfForSurface` = luminance of `evalBrdf · Lo(X2) · NdotL` toward the sample (`shadeGiSample`, bridge) | the sample's `RAB_GetReflectedBrdfRadianceForSurface` |
| Jacobian | `RAB_ValidateGISampleWithJacobian`: reject past 10×, clamp to [1/3, 3], the sample's bridge | measured within 1% on Sponza (§3) |
| Reconnection rays | `isReconnectionVisible`: from `rasterRayOrigin` of the guide surface to X2, ending 0.1% of the distance short of it (X2 lies on the triangle that traced it). Temporal bias correction traces from the current surface through this frame's TLAS | no previous TLAS, as the sample without `enablePrevTLAS` |
| Final shading | `W · evalBrdf · Lo · NdotL` of the final sample, zeroed by a reconnection ray (final visibility); final MIS: weight `1 − w(y)` for the final sample and `w(x)` for the initial one, `w = (lum(f) / lum(f + f_rough))³` with roughness raised to 0.3 | the sample's `FinalShading.hlsl`, which demodulates for NRD; DLSS-RR takes the combined radiance, so nothing is demodulated. The sample loads the reservoir to shade from `secondarySurfaceReSTIRDIOutputBufferIndex`, which in `TemporalAndSpatial` is the temporal output; the engine shades the spatial output (`finalShadingInputBufferIndex`), as its `GIReservoirViz` does |
| Failed BRDF sample | a lit primary whose BRDF sample has pdf 0 stores a zero-radiance reservoir with M = 1, like a miss | an empty reservoir (M = 0) counts as no candidate, so temporal reuse keeps an older sample at full weight where the 1-sample estimate reads 0: +15% on the gallery before the fix (§3). The sample stores an empty reservoir there |
| Ground truth | `RtxdiContext::giReferenceBounces` > 0: the indirect pass continues each path from X2 for that many bounces (`referencePathRadiance`, path_shading: full-BRDF scatter, emission + one RIS light per vertex, no roulette) instead of the cache term, and the renderer pauses NRC (`NrcCache::pauseFrame`: zero constants, no NRC commands, weights kept). With the reference resampling mode, accumulated, it is engine spec §14's ground truth. `kGiGroundTruthBounces` = 8 | §14: the same path code, cache off, high bounce count. NRC's update loop shares `scatterBrdf` with it; the loop itself stays separate because every NRC call sits inside it |
| History on a mode change | none yet: reservoirs age out within `maxHistoryLength` / `maxReservoirAge`, and the check warms up past that | G7 adds the reset of its mode switches |
| Debug view | `Indirect radiance` (29): `indirectRadiance`. `Indirect samples` (27) now reads the initial array. The guide capture writes both (14 views) | |
| UI | `ReSTIR GI` under `Indirect (M4)`: ground-truth paths and their bounce count, the reference, every `GiSettings` field, `NVIDIA defaults` / `Converged` | |
| GPU timers | `ReSTIR GI temporal` (`GpuTimerScope::GiTemporal`), `ReSTIR GI shade` (`GiShade`); Tracy zone `RestirGi` | |

---

## 2. Files

| File | Change |
|---|---|
| `src/render/vk_rtxdi.{hpp,cpp}` | `GiSettings`, `defaultGiSettings`, `convergedGiSettings`, `configureGi`, `isGiReference`, `giReferenceBounces`, 3 GI arrays; `GpuRtxdiConstants` gains the GI resampling mode, temporal, boiling, spatial and final-shading blocks and the bounce count (392 → 496 B) |
| `shaders/lighting/rtxdi_constants.slang`, `tests/layout_test.cpp` | mirror, `kGiInitialReservoirArray`; offsets checked |
| `shaders/lighting/rtxdi_bridge.slang` | `shadeGiSample`, `RAB_GetGISampleTargetPdfForSurface`, `RAB_ValidateGISampleWithJacobian`, `isReconnectionVisible`, the GI `RAB_GetConservativeVisibility` / `RAB_GetTemporalConservativeVisibility` |
| `shaders/passes/rtxdi_gi_temporal.slang`, `rtxdi_gi_shade.slang` | new |
| `shaders/passes/indirect.slang` | initial array, failed samples as zero samples, ground-truth paths |
| `shaders/lighting/path_shading.slang`, `shaders/passes/nrc_update.slang` | `kMissDistance`, `scatterBrdf`, `referencePathRadiance` |
| `shaders/passes/nrc_resolve.slang`, `debug_view.slang`, `shaders/types.slang`, `src/render/debug_view.hpp` | initial array; `Indirect radiance` view |
| `src/core/types.hpp`, `src/render/vk_render_targets.{hpp,cpp}` | `indirectRadiance` target, `GpuTargets` 200 → 208 B |
| `src/render/vk_renderer.{hpp,cpp}`, `vk_nrc.{hpp,cpp}`, `vk_gpu_timer.hpp`, `vk_shader_objects.{hpp,cpp}`, `CMakeLists.txt` | `recordIndirectResampling`, NRC pause, timers, shaders |
| `src/core/engine_gi_check.{hpp,cpp}`, `convergence_metrics.{hpp,cpp}`, `vk_engine.{hpp,cpp}`, `engine_guide_capture.{hpp,cpp}` | the GI check; the tile metric shared with the ReSTIR DI check |
| `src/ui/engine_ui.cpp` | `ReSTIR GI` section |

---

## 3. Verification

**GI check** (`ENGINE_GI_CHECK=1`, release, no layers, jitter off, 2026-11-18, RTX 5070 driver 621.01). Per pose of the startup Sponza with the sun (gallery: the startup camera; courtyard: `lookAt({6, 2.5, 1.5}, {-10, 1, -1})`):

1. **Ground truth:** 8 bounces after X2, NRC paused, no reuse, 2048 frames of `Indirect radiance` averaged.
2. **Noise floor:** the same again for 512 frames. This is what the metrics read for an unbiased estimate of that length.
3. **ReSTIR GI on the same paths**, `convergedGiSettings`, 512 frames after 64. Gated.
4. **NRC + ReSTIR GI**, `convergedGiSettings`, 512 frames after 512. Measured, not gated: NRC's bias (C-G5-3).
5. **NRC + ReSTIR GI, NVIDIA defaults**, 512 frames after 64. Measured, not gated (C-G5-1, C-G5-2).

Metrics are shares of the ground truth's summed luminance. Bias is over the whole frame. Tile errors use 32×32 tiles, signed within a tile.

| Phase | Gallery: bias / tile p90 | Courtyard: bias / tile p90 | Limit |
|---|---|---|---|
| ground truth again (noise floor) | +0.19% / 3.74% | +0.13% / 2.66% | 1.0% / 6% |
| ReSTIR GI, traced paths | **+0.64% / 3.97%** | **+0.90% / 5.56%** | 1.5% / 8% |
| NRC + ReSTIR GI, converged | +9.34% / 18.3% | +10.18% / 33.3% | measured |
| NRC + ReSTIR GI, NVIDIA defaults | −15.90% / 27.8% | −0.69% / 22.9% | measured |

Per-frame RMS error against the ground truth:

| | Gallery | Courtyard |
|---|---|---|
| ground truth's 1-sample noise | 785% | 346% |
| ReSTIR GI, traced paths | 177% (4.4× lower) | 97% (3.6× lower) |

The bisect that found the failed-sample bias and C-G5-1 ran on the gallery, with 1024 ground-truth frames and 16×16 tiles, all with temporal and spatial bias correction:

| Variant | Bias |
|---|---|
| no reuse | −0.6% |
| spatial only | +3.8% |
| temporal only | +15.0% |
| temporal only, Basic or Off bias correction | +15.0% (same) |
| temporal only, gray radiance (LogLuv chroma ruled out) | unchanged |
| temporal only, output ×1000 (half precision ruled out) | unchanged |
| failed BRDF samples counted as zero samples, temporal only | +8.5% |
| that, with no age limit | −0.6% |
| that, temporal + spatial, no age limit | +0.53% |
| NRC samples: no reuse | +10.0% |
| NRC samples: converged reuse | +11.9% |
| NRC samples: default reuse | −13.2% |
| NRC samples: default reuse with training frozen | −14.9% |

**Other checks** (2026-11-18):

| Check | Result |
|---|---|
| Validation: core + sync + best practices, `ENGINE_GUIDE_CAPTURE=1 ENGINE_RT_CHECK=1` | clean apart from the known NGX `SpirvDeprecated_WorkgroupSize` ×3 and `VUID-vkDestroyDevice-device-05137` (C-G3-3) |
| ReSTIR DI convergence (shared tile metric now) | passed: −0.055%, p90 2.23% (the added view shifts RTXDI's RNG) |
| Normal checks, ray check | 2 of 2, 282 of 282 |
| GPU-AV, `ENGINE_MOTION_TEST=1` and `ENGINE_GI_CHECK=1` | clean (`WARNING-Setting-Limit-Adjusted`, 05137); motion 12 of 13 (C-T-1 under GPU-AV), GI check 4 of 4 |
| Sync validation, `ENGINE_GI_CHECK=1` (NRC pause and resume) | clean apart from the known NGX WAW ×2 on the first DLSS-RR evaluation (C-H1-2), `SpirvDeprecated` ×3 and 05137; GI check 4 of 4 |
| C-G3-6 (device loss at large render extents): `ENGINE_BENCH_FRAMES=3000` at a 2560×1369 window (render 1707×913), 2 runs | no device loss; ReSTIR GI temporal 0.46 ms, shade 1.35–1.39 ms, GPU total 11.2 ms at that extent |
| Bounce light under the arches, stable after DLSS-RR | passed with G6, 2026-11-19 ([g6_combine.md](./g6_combine.md) §3) |

---

## 4. Measurements

Release, no layers, startup gallery, 1280×720 window (853×480 render, DLSS-RR Quality), `ENGINE_BENCH_FRAMES=2000`, NVIDIA defaults (2026-11-18):

| Scope | Mean | Best window |
|---|---|---|
| ReSTIR GI temporal | 0.109 ms | 0.106 ms |
| ReSTIR GI shade (spatial + final visibility + MIS) | 0.384 ms | 0.356 ms |
| GPU total | 3.31 ms (2.8 ms before G5) | |

Total ReSTIR GI is 0.49 ms, under engine spec §2.3's 1.2 ms on the reference tier. The 2060 Super is not measured (C-G2-1).

---

## 5. Log

| Date | Change |
|---|---|
| 2026-11-16 | created from plan G5 |
| 2026-11-18 | built: temporal and shade passes, 3 GI arrays, GI settings and UI, `Indirect radiance` view, ground-truth paths with the NRC pause, `ENGINE_GI_CHECK`. The check found and the fix removed a +15% temporal bias: failed BRDF samples stored as empty reservoirs. RTXDI's age limit (C-G5-1) and Basic bias correction (C-G5-2) are biased on static frames; `convergedGiSettings` drops both, and NRC reads +10% against the ground truth (C-G5-3) |
