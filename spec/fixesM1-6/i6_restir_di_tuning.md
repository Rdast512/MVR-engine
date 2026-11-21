# M8 · I6: ReSTIR DI tuning

**Status:** planned 2026-11-21 · **Milestone:** M8 ([README](./README.md))
**Implements:** findings F5, F6, O8, O10, permutation sampling ([m8_optimization_and_fixes.md](./m8_optimization_and_fixes.md) §2.4, §2.7, §4), F3 DI screen-edge behavior (§4), F12 default presampling mode ([i4_frame_profile.md](./i4_frame_profile.md))
**Depends on:** I1 ([i1_check_runners.md](./i1_check_runners.md)), I2 ([i2_shading_correctness.md](./i2_shading_correctness.md)), I4 ([i4_frame_profile.md](./i4_frame_profile.md))
**References:**
- `ThirdParty/_deps/src/rtxdi/Include/Rtxdi/DI/{InitialSampling,TemporalResampling,SpatialResampling,ReSTIRDIParameters}.hlsli`, `Source/ReSTIRDI.cpp`
- `reference/RTXDI/Samples/FullSample/Source/UserInterface.cpp` (`Fast`, `Medium`, `Unbiased`, `Ultra` presets), `Doc/NoiseAndBias.md`
- `docs/settings.md` §3.4 (pre-M4 DI stability table on sunlit Sponza courtyard)

## Scope

I6 ranked **#1** in I4's frame profile: Bistro `ReGIR_RIS` adds **+0.420 ms (+13.8%)** over `Power_RIS` (`light prep` 0.477 vs 0.058 ms), and ReSTIR DI on sunlit Sponza courtyard costs **0.704 ms** (`initial` 0.241 / `temporal` 0.141 / `shade` 0.322 ms; gallery 0.351 ms). I6 evaluates every DI ray, decides the default presampling mode (F12), lands O8, evaluates O10 (`isFinalVisibilityReused`) and permutation sampling together under post-I5 GI + M6 OMM, compares NVIDIA `Medium` vs `convergedSettings` vs tuned defaults through DLSS-RR (F6), and checks DI screen-edge controls (F3).

| Item | Lever / Question | Verification |
|---|---|---|
| O8 | Skip the BRDF-candidate ray and BRDF sampling in `rtxdi_initial.slang` (with a defensive guard in `rtxdi_bridge.slang` `RAB_TraceRayForLocalLight`) when `triangleLights.lightCount == 0 && environmentLightParams.lightPresent == 0` | ABBA bench on Sponza gallery & courtyard; verify emissive scenes (`EmissiveStrengthTest`, `LightsPunctualLamp`, Bistro) still trace BRDF rays |
| F12 | Decide default `localLightSamplingMode`: `Power_RIS` vs `ReGIR_RIS` from I4's −0.42 ms Bistro ABBA plus convergence (`LightsPunctualLamp`, `EmissiveStrengthTest`) and Bistro stability | I4 ABBA + `ENGINE_GUIDE_CAPTURE` + Bistro stability check |
| F5 · DI ray budget | Measure cost, DI convergence (`LightsPunctualLamp`, `Sponza_terminator`, `EmissiveStrengthTest`), and `Lit` stability for every DI ray: initial visibility, temporal correction (`Raytraced` + shortcut vs `Basic`), spatial correction (`Raytraced` vs `Pairwise` vs `Basic` vs `spatialSamples = 0`), final visibility reuse (O10) | Bench on Sponza courtyard, gallery, and Bistro (`Power_RIS`); `ENGINE_GUIDE_CAPTURE` (shipped DI); `ENGINE_STABILITY_CHECK` |
| O10 & Permutation | Evaluate the 2×2 matrix (`isPermutationSamplingEnabled` × `isFinalVisibilityReused`) with `Raytraced` and `Pairwise` spatial correction on post-I5 GI + M6 OMM (re-testing `docs/settings.md` §3.4) | `ENGINE_STABILITY_CHECK` (`Lit` + `Direct lighting`), `ENGINE_GUIDE_CAPTURE` (`Sponza_terminator` shadow-edge leak), ABBA bench |
| F6 | Compare NVIDIA `Medium`, `convergedSettings`, current shipped, and tuned shipped through DLSS-RR on sunlit Sponza courtyard and gallery | `ENGINE_STABILITY_CHECK`, ABBA bench |
| F3 · DI screen edges | Test `disocclusionHistoryLength` (`0`, `4`, `20`), `disocclusionBoostSamples` (`0`, `4`, `8`), and `isFallbackSamplingEnabled` (`1`, `0`) on revealed bands and disocclusions | `ENGINE_SCREEN_EDGE_CHECK` (`Lit` and `Direct lighting`) |

## Decisions

| Decision | Rule | Why |
|---|---|---|
| NVIDIA `Medium` untouched | `RtxdiContext::nvidiaSettings()` stays callable and unchanged (`ReGIR_RIS`, `Basic` spatial, `permutation = true`, `reuseFinalVisibility = true`) | M8 rule (`C-M4-2`) |
| No vendor header edits | All changes stay in engine shaders and host code (`shaders/`, `src/`) | Vendor boundary (`ThirdParty/`) |
| Convergence check gate preserved | `EngineGuideCapture` keeps gating `convergedSettings()` (`3 of 3` pass); tuned shipped DI is also measured against the same 3 convergence shots | Matches `rtxdi_3_2_update.md` §6.1 while quantifying shipped DI bias |
| Benchmark hygiene | Every landed perf lever is ABBA-measured on release with no layers (`VK_LOADER_LAYERS_DISABLE=all`, `VK_KHRONOS_VALIDATION_GPUAV_ENABLE=false`) on Sponza gallery, courtyard, and Bistro | Eliminates drift and covers both 1-light and 76K-light scenes |

## Results

*(To be recorded as each experiment runs.)*

## Done when

- [ ] O8 landed and ABBA-measured on Sponza (with emissive scenes verified unaffected)
- [ ] F12 default presampling mode (`Power_RIS` vs `ReGIR_RIS`) decided with Bistro perf, convergence, and stability numbers
- [ ] Every DI ray (initial visibility, temporal correction, spatial correction, final visibility reuse O10, BRDF candidate) and permutation sampling landed or closed with cost, convergence, and stability numbers
- [ ] F6 comparison (NVIDIA `Medium` vs `converged` vs shipped through DLSS-RR) recorded on sunlit Sponza
- [ ] F3 DI screen-edge settings (`disocclusionBoostSamples`, `disocclusionHistoryLength`, fallback) measured and decided
- [ ] `ENGINE_GI_CHECK` (10/10), `ENGINE_STABILITY_CHECK`, `ENGINE_SCREEN_EDGE_CHECK`, `ENGINE_GUIDE_CAPTURE` (3/3 DI convergence + 2/2 normal), `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST`, CTest, and Vulkan validation pass
