# M7 · F1: local light presampling

**Status:** planned 2026-11-20 · **Milestone:** M7
**Implements:** engine spec §11.2 (local light sampling); plan F1, M7 part, and §2 #12 ([pbr_implementation_plan.md](../pbr_implementation_plan.md)); [shader_architecture_plan.md](../shader_architecture_plan.md) row 10 (`rtxdi_presample`)
**Depends on:** [E2](./e2_emissive_triangles.md) (triangle lights and their power), F1–F3 (`RtxdiContext`, the bridge, the DI passes)
**Considerations:** C-F3-5 (presampling part closed here); the NVIDIA-defaults deviation "uniform local light sampling until ReGIR" (`vk_rtxdi.cpp`, `engine_ui.cpp`, engine spec §11.2) is removed here
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
| PDF texture | size from `rtxdi::ComputePdfTextureSize(local light capacity)`, recreated when the capacity grows, with the world-row growth; one copy, like the RIS buffers | the sample; `R32_FLOAT` for the range of flux values when reduced. One copy serves both slots: the compute submit already waits for the previous frame's ray passes on `FrameSync::rayTimeline` (`vk_rt_scene.cpp`, C-F2-1), and no pass reads last frame's PDF |
| Mips | a reduction pass (average of 2×2), one dispatch per level or a single-pass reducer from `reference/vk_compute_mipmaps` | `RTXDI_PresampleLocalLights` walks the mip chain from the top |
| Presampling | `rtxdi_presample` (`RTXDI_PresampleLocalLights`, one thread per RIS entry) then `rtxdi_presample_regir` (`RTXDI_PresampleLocalLightsForReGIR`, 256 threads per group) each frame, after the PDF mips | the sample's `PresampleLights` order; ReGIR fills its cells from the Power_RIS tiles |
| Queue | the PDF write, mips and both presample passes run on async compute after `light_world`, before the compute timeline signal the ray passes already wait on | they read only world rows and the camera; no new semaphore |
| Compact light info | `RAB_StoreCompactLightInfo` / `RAB_LoadCompactLightInfo` pack a world row into 32 B (two `uint4`): a triangle as the sample packs it (center, two octahedral edge directions, two f16 lengths, packed radiance), a punctual row by its own fields; round-trip covered by a unit test | presampled tiles carry the light so the initial pass skips the world-row load |
| Source pdf | `RAB_EvaluateLocalLightSourcePdf(i)` = PDF texel of `i` / total power (top mip × texel count), as the sample | the BRDF candidate's MIS weight against the light candidates |
| ReGIR mode | `RTXDI_REGIR_MODE` defined in the bridge before the RTXDI headers (`RTXDI_REGIR_ONION`) | RTXDI selects it by macro (`GetRegirMacro`); a vendor-mandated define, not an engine permutation |
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

## Log

| Date | Change |
|---|---|
| 2026-11-20 | Spec written |
