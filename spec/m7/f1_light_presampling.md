# M7 · F1: local light presampling

**Status:** built 2026-11-21 (open: C-F1-2 ReGIR cost, C-F1-4, C-F1-5, C-F1-6; workaround C-F1-3) · **Milestone:** M7
**Implements:** engine spec §11.2 (local light sampling); plan F1, M7 part, and §2 #12 ([pbr_implementation_plan.md](../pbr_implementation_plan.md)); [shader_architecture_plan.md](../shader_architecture_plan.md) row 10 (`rtxdi_presample`)
**Depends on:** [E2](./e2_emissive_triangles.md) (triangle lights and their power), F1–F3 (`RtxdiContext`, the bridge, the DI passes)
**Considerations:** C-F3-5 (presampling part closed here), C-E2-1 (closed here), C-F1-2 to C-F1-6 (opened here); the NVIDIA-defaults deviation "uniform local light sampling until ReGIR" (`vk_rtxdi.cpp`, `engine_ui.cpp`, engine spec §11.2) is removed here
**Reference:** RTXDI FullSample: `Source/UserInterface.cpp` `ApplyPreset` (`Medium`: `ReGIR_RIS`, 8 local samples for all three modes), `Source/App/SceneRenderer.cpp` (`ImportanceSamplingContext`, `PrepareLights` → `PresampleLights` → lighting, ReGIR center = camera position), `Source/RtxdiResources.cpp` (RIS buffer `RG32_UINT`, RIS light data 2 × `RGBA32_UINT` per entry, `LocalLightPdf` `R32_FLOAT` sized by `rtxdi::ComputePdfTextureSize`), `Source/RenderPasses/GenerateMipsPass.*`, `Shaders/PrepareLights.hlsl` (flux at `RTXDI_LinearIndexToZCurve`), `Shaders/LightingPasses/Presampling/PresampleLights.hlsl`, `PresampleReGIR.hlsl`, `Source/RenderPasses/LightingPasses/LightingPasses.cpp` (`GetRegirMacro`). SDK (`ThirdParty/_deps/src/rtxdi`): `ImportanceSamplingContext.h` (RIS segments `{1024, 128}`), `ReGIR/ReGIR.h` (static: `Onion`, 512 lights per cell; dynamic: cell size 1, `Power_RIS` presampling and fallback, 8 build samples), `LightSampling/PresamplingFunctions.hlsli`, `LocalLightSelection.hlsli`

Give RTXDI the presampled light tiles it was built around, so local light sampling follows power (`Power_RIS`) and position (ReGIR), with NVIDIA's `Medium` preset as the default.

## Scope

| Item | Today | After F1 |
|---|---|---|
| Local light sampling | `Uniform` (a recorded deviation from `Medium`) | `ReGIR_RIS`, 8 samples (`Medium`); `Power_RIS` and `Uniform` selectable in the UI and settings |
| Secondary-vertex light sampling (`directLightAt`) | uniform | `neeLocalLightSampling`: the same mode and counts as the primary |
| Light PDF | none | `LocalLightPdf`, `R32_FLOAT` with a full mip chain, local-light power at each light's Z-curve texel |
| RIS buffers | none | RIS buffer (`uint2` per entry) and compact light data (32 B per entry), segments from the SDK's allocator: local-light tiles, then ReGIR cells |
| Passes | — | PDF write in `light_world`, `light_pdf_mips`, `rtxdi_presample` (Power_RIS tiles), `rtxdi_presample_regir` (ReGIR cells) |
| `RTXDI_ENABLE_PRESAMPLING` | 0 | 1 |

## Decisions

| Decision | Rule | Why |
|---|---|---|
| Host context | `RtxdiContext` owns an `rtxdi::ImportanceSamplingContext` (ReSTIR DI, ReSTIR GI, ReGIR and the RIS segment allocator) instead of the separate DI and GI contexts | the sample's `SceneRenderer`; the allocator and `ReGIRContext` live only there |
| Defaults | `Medium`: `ReGIR_RIS`; `numLocalLightUniformSamples`, `numLocalLightPowerRISSamples`, `numLocalLightReGIRRISSamples` = 8; `numLocalLightSamples` = 8; ReGIR static and dynamic parameters and the RIS segments at the SDK defaults the sample keeps; ReGIR center = this frame's camera position | NVIDIA's defaults are the engine's defaults (comparison with the sample). The convergence check keeps its own `convergedSettings` and gets the mode that passes it, measured, not assumed |
| Power | written by `light_world` for every local light: triangle `area · π · luminance(radiance)` (×2 when double-sided, E2), point `4π · luminance(intensity · color)`, spot the same as point, at `RTXDI_LinearIndexToZCurve(index)`; texels past the light count stay 0 | the sample's `PrepareLights` and `PolymorphicLight::GetPower` (its point and spot lights use `4π` flux with shaping; the engine's spot cone is a shaping factor RTXDI's power does not need for correctness, only for sampling quality) |
| PDF texture | square, its side the width `rtxdi::ComputePdfTextureSize` gives for the local light count rounded up to a power of two, recreated when the count passes side² (after the ray timeline reaches the last submitted frame); one copy, like the RIS buffers. As built: square, so every 2×2 footprint of the mip chain stays inside the image, and sized by the count `light_world` writes, which follows the load | the sample; `R32_FLOAT` for the range of flux values when reduced. One copy serves both slots: the compute submit already waits for the previous frame's ray passes on `FrameSync::rayTimeline` (`vk_rt_scene.cpp`, C-F2-1), and no pass reads last frame's PDF |
| Mips | a reduction pass (average of 2×2), one dispatch per level or a single-pass reducer from `reference/vk_compute_mipmaps` | `RTXDI_PresampleLocalLights` walks the mip chain from the top |
| Presampling | `rtxdi_presample` (`RTXDI_PresampleLocalLights`, one thread per RIS entry) then `rtxdi_presample_regir` (`RTXDI_PresampleLocalLightsForReGIR`, 256 threads per group) each frame, after the PDF mips | the sample's `PresampleLights` order; ReGIR fills its cells from the Power_RIS tiles |
| Queue | the PDF write, mips and both presample passes run on async compute after `light_world`, before the compute timeline signal the ray passes already wait on | they read only world rows and the camera; no new semaphore |
| Compact light info | `RAB_StoreCompactLightInfo` / `RAB_LoadCompactLightInfo` pack a world row into 32 B (two `uint4`, `lighting/compact_lights.slang`): a triangle as the sample packs it (centroid, two octahedral edge directions, two f16 lengths, f16 radiance), a point light by its own fields (f16 color). As built: spot rows are not packed, as the sample's shaped lights (their cone needs more than 32 B), and a row whose radiance, color or edge length passes 65,504 stays unpacked; RTXDI then loads the world row. Round-trip covered by a unit test | presampled tiles carry the light so the initial pass skips the world-row load |
| Source pdf | `RAB_EvaluateLocalLightSourcePdf(i)` = PDF texel of `i` / total power (top mip × texel count), as the sample, in `Power_RIS` and `ReGIR_RIS`; `1 / count` in `Uniform` | the BRDF candidate's MIS weight against the light candidates. The sample reads the PDF in every mode, which biases the MIS weight of a uniform light candidate; `Uniform` therefore skips the PDF and its mips |
| ReGIR mode | `RTXDI_REGIR_MODE` defined in the bridge before the RTXDI headers (`RTXDI_REGIR_ONION`) | RTXDI selects it by macro (`GetRegirMacro`); a vendor-mandated define, not an engine permutation |
| ReGIR parameters | RTXDI's functions receive `RegirParametersView`, which reads each field through the constants pointer (`#define ReGIR_Parameters`); the SDK stays unmodified | the by-value 1.3 KB struct put its onion arrays in local memory: 1.4 ms build, 1.2 ms initial sampling (C-F1-3) |
| Convergence check | `convergedSettings` pins `Power_RIS`; its `RtxdiNone` reference samples with `Power_RIS` and 32 candidates over 1,024 frames; 512 captured frames | §3 |
| Settings | the mode and counts sit in `RtxdiContext::Settings` beside the existing DI settings, saved and restored with them | the comparison with the sample needs each mode callable |

## 1. Tasks

1. `vk_rtxdi.{hpp,cpp}`: `ImportanceSamplingContext`, RIS and PDF resources (named), `GpuRtxdiConstants` grows by the RIS segment and ReGIR blocks (layout test); the DI and GI accessors keep their callers.
2. `passes/light_world.slang`: the power write; `passes/light_pdf_mips.slang`; `passes/rtxdi_presample.slang` (two entries).
3. Bridge: `RTXDI_ENABLE_PRESAMPLING 1`, the RIS buffer macros, the compact light info pair, `RAB_EvaluateLocalLightSourcePdf`; remove the "Not built yet" lines for presampling.
4. `directLightAt` (indirect, ground truth, NRC update) takes `neeLocalLightSampling`.
5. UI and settings: local light sampling mode and counts; the `Medium` tooltip loses "uniform until ReGIR".
6. Debug: a `Light PDF` view of mip 0 is optional; the ReGIR cell view of the sample is not ported.

## 2. Verification

| Check | Expected |
|---|---|
| Convergence check | passes on `LightsPunctualLamp` and `EmissiveStrengthTest` with each mode (`Uniform`, `Power_RIS`, `ReGIR_RIS`) under `convergedSettings`; the mode it pins is recorded. The lamp's shot runs with its triangle lights sampled again (`ConvergenceShot::areTriangleLightsSampled` true) against an `RtxdiNone` reference: E2 had to turn them off, since uniform selection could not converge it (C-E2-1) |
| Noise | `DamagedHelmet` (many unequal triangle lights): per-frame RMS error against the reference at a fixed frame count, `Uniform` > `Power_RIS` ≥ `ReGIR_RIS`; recorded |
| `ENGINE_GI_CHECK` | 8 of 8; Sponza's numbers move with the sampling mode (few lights), recorded |
| Compact light info test | every light type round-trips within f16 precision |
| `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST`, guide capture | unchanged, and the guide capture's emissive shots match E2's |
| Validation and GPU-AV | clean, including a capacity growth (load a larger emitter after a smaller one) |
| Timing (release, no layers, RTX 5070) | PDF write + mips, both presample passes, and `rtxdi_initial` per mode on Sponza, `EmissiveStrengthTest` and `DamagedHelmet` |

## Done when

- `ReGIR_RIS` with `Medium`'s counts is the default, and `Power_RIS` and `Uniform` are callable;
- the convergence check passes with the pinned mode, and the noise and timing comparisons are recorded;
- the presampling part of C-F3-5 and the "uniform until ReGIR" deviation are gone from the code, the UI and the specs.

## Must not

- read the PDF or the RIS buffers on the CPU;
- size the PDF texture or the RIS segments from a fixed light count (the capacity follows the load; the RIS tile counts are the SDK defaults);
- retune `Medium`'s counts to pass a check (the check pins its own settings).

## 3. Results (2026-11-21)

Release, RTX 5070, driver 621.01. Convergence runs: 16 warm-up + 512 captured frames under `convergedSettings` against a 1,024-frame `RtxdiNone` reference (`Power_RIS`, 32 local + 8 infinite candidates, no reuse). Gates: |bias| ≤ 0.5%, 16×16 tile p90 ≤ 3%.

**Convergence per mode** (bias / tile p90 / per-frame error):

| Mode | `LightsPunctualLamp` (triangles sampled) | `EmissiveStrengthTest` (1 BRDF candidate) | `Sponza_terminator` |
|---|---|---|---|
| `Uniform` | −3.04% / 46.4% / 171% (fail) | +0.072% / 1.38% / 4.67% | −0.001% / 0.00% |
| `Power_RIS` (pinned) | **+0.030% / 1.20% / 8.57%** | **+0.063% / 1.90% / 4.72%** | −0.001% / 0.00% |
| `ReGIR_RIS` | −0.036% / 2.00% / 8.38% | +0.193% / 3.19% / 4.68% (fail, p90) | −0.001% / 0.00% |

- `Uniform` cannot converge the lamp: 2 punctual lights among 5,985 bulb triangles (C-E2-1's cause). `ReGIR_RIS` misses `EmissiveStrengthTest`'s tile gate by 0.19 points: its BRDF candidate is weighed against the power PDF, not the cell's selection (C-F1-4). The spec's "passes with each mode" is met by `Power_RIS` alone, which the check pins.
- The reference: a 1,024-frame uniform reference was still off on the lamp (`Power_RIS` read +1.95% against it, +0.32% against 8,192 uniform frames, −0.056% against 8,192 `Power_RIS` frames). Where the uniform reference converges (`EmissiveStrengthTest`), `Power_RIS` agrees with it (+0.06%).
- Frame count: at 64 frames `Power_RIS` read p90 4.27% on the lamp and 4.17% on `EmissiveStrengthTest`, at 256 frames 2.36% / 3.13%, at 512 1.20% / 1.90%. RTXDI draws one RIS tile per 16×16 screen tile, the check's tile (C-F1-6).

**Noise on `DamagedHelmet_front`** (15,452 triangle lights, 1 BRDF candidate, 512 frames, temporary fourth convergence shot): per-frame error `Uniform` 0.18% > `Power_RIS` 0.15% ≈ `ReGIR_RIS` 0.15%; tile p90 85.8% > 65.9% > 62.2%; bias +0.087% in every mode. The order the spec expects holds. The `Direct lighting` view includes the helmet's own emission, a constant that dominates the summed luminance, so the relative per-frame error is small and the dark tiles' p90 large.

**Timings** (ms; no layers; guide-capture shots, one frame at a time, so the async passes ran without overlap; `rtxdi_initial` from `GpuTimer`, the async passes from a temporary timestamp pool on the compute command buffer, C-F1-5):

| Scene, local lights | Mode | `light_world` | PDF mips | `Power_RIS` tiles | ReGIR build | `rtxdi_initial` |
|---|---|---|---|---|---|---|
| lamp, 5,990 | `Uniform` | 0.0008 | — | — | — | 0.047 |
| | `Power_RIS` | 0.0008 | 0.0020 | 0.023 | — | 0.039 |
| | `ReGIR_RIS` | 0.0007 | 0.0021 | 0.023 | 0.386 | 0.332 |
| `EmissiveStrengthTest`, 60 | `Uniform` | 0.0008 | — | — | — | 0.056 |
| | `Power_RIS` | 0.0008 | 0.0020 | 0.021 | — | 0.065 |
| | `ReGIR_RIS` | 0.0008 | 0.0020 | 0.021 | 0.391 | 0.368 |
| `DamagedHelmet`, 15,452 | `Uniform` | 0.0008 | — | — | — | 0.077 |
| | `Power_RIS` | 0.0008 | 0.0021 | 0.027 | — | 0.067 |
| | `ReGIR_RIS` | 0.0008 | 0.0020 | 0.028 | 0.409 | 0.367 |
| `Sponza_terminator`, 0 | any | 0.0007 | — | — | — | 0.116–0.119 |

Before the ReGIR parameter view (C-F1-3), `ReGIR_RIS` read 1.44 ms build and 1.22 / 1.58 ms initial (lamp / `EmissiveStrengthTest`), `Power_RIS` 0.076 / 0.142 ms initial and `Uniform` 0.074 / 0.095 ms. ReGIR's remaining cost is open (C-F1-2): the default (`Medium`, `ReGIR_RIS`) costs ~0.7 ms more than `Power_RIS` on a scene with local lights.

**Other checks:**

| Check | Result |
|---|---|
| Compact light test (`rtxdi_math`) | 7 cases pass: 3 triangles (one double-sided and mirrored, one degenerate) and 2 points (one with range 0) round-trip within 1e-3; a spot row and a triangle past f16 are rejected |
| Unit tests | 7 of 7 |
| Guide capture | 374 PNGs; normal checks 2 of 2; convergence 3 of 3. The emissive shots' lit views were not diffed against E2's: their default-mode noise changes with the sampling mode by design |
| `ENGINE_GI_CHECK` | 8 of 8. Courtyard traced paths +1.21% (gate 1.5%; +1.42% after M6 D4). Startup Sponza has no local lights, so F1's passes do not run there. The numbers still move: `RTXDI_ENABLE_PRESAMPLING` adds one random draw to `RTXDI_SampleLightsForSurface` (the environment combine), which changes the realization. The ungated courtyard line "shipped + secondary DI resampling" moved most: −38.8% (−22.9% in [m8_optimization_and_fixes.md](../fixesM1-6/m8_optimization_and_fixes.md) §2.6). Not A/B-confirmed |
| `ENGINE_MOTION_TEST` + `ENGINE_RT_CHECK` | 13 of 13; 318 of 318 frames matched |
| `ENGINE_CULL_CHECK` | 16 of 16 |
| Validation (core, sync, best practices + NVIDIA) | guide capture clean apart from the known third-party messages: 3 × `BestPractices-SpirvDeprecated_WorkgroupSize` (NGX, C-H1-2) and `VUID-vkDestroyDevice-device-05137` (NRC leaks, C-G3-3; 216 objects over 17 shots). The run grows the PDF (4×4 to 128×128 at the lamp's 5,990 lights). GPU-AV cannot run since E1 (C-E1-3) |
| RIS buffers | 1,403,392 entries: 10.7 MiB (`uint2`) + 42.8 MiB (compact rows); local tiles 128 × 1,024, environment 128 × 1,024 (unused, C-F3-5), ReGIR 2,229 cells × 512 at 262,144 |

## Log

| Date | Change |
|---|---|
| 2026-11-20 | Spec written |
| 2026-11-21 | Built. `RtxdiContext` owns `ImportanceSamplingContext`; `light_world` writes the power at each light's Z-curve texel and zeros past the local light count (it dispatches max(light count, texel count) threads); `light_pdf_mips` and `rtxdi_presample` (two entries) on async compute; compact lights in `lighting/compact_lights.slang`; `RAB_GetLightTargetPdfForVolume` (triangle: solid angle × luminance with the one-sided cull; punctual: I/d² at the sphere's mean distance with the range and cone cull). `directLightAt` uses the same mode and counts. UI: mode combo and the active mode's sample count. Deviations: square PDF, `Uniform` source pdf, spot and f16-overflow rows unpacked, the ReGIR parameter view, the `Power_RIS` reference and pin of the convergence check, 512 check frames (§Decisions, §3) |
