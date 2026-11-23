# M8 · I12: many-light and glossy re-check

**Status:** planned 2026-11-23 · **Milestone:** M8 ([README](./README.md))
**Implements:** the Bistro exits of findings O2, O6, O8 and O9 ([m8_optimization_and_fixes.md](./m8_optimization_and_fixes.md) §2.4, §2.6, §2.7, §2.9), I5's and I6's decisions re-run on many-light poses, and the Bistro re-check of F22 and F23 after I15's fixes (§4)
**Depends on:** I5, I6 (the decisions re-run here), I14 (correct rays, reuse and light PDF on Bistro), I15 (F22 / F23 fixes). Phase 0 has no dependency and can land now
**Considerations:** closes C-I12-1 and C-I12-2 with I15, or keeps them open with the Bistro numbers; opens C-I12-3 … C-I12-5 (§7)
**References:**
- `reference/RTXDI/Samples/FullSample/Source/UserInterface.cpp:196-310` (`ApplyPreset`): `Medium`, `Unbiased` and `Ultra` set `enableSecondaryResampling = true` (secondary spatial samples 1 / 1 / 2) and `numBrdfSamples = 1`; `Fast` turns both off
- `reference/RTXDI/Samples/FullSample/Shaders/LightingPasses/ShadeSecondarySurfaces.hlsl:95-136`: secondary resampling against the shading-input DI reservoirs at X₂'s pixel; the firefly clamp `c_MaxIndirectRadiance = 10` is unconditional
- `reference/RTXDI/Samples/FullSample/Shaders/LightingPasses/BrdfRayTracing.hlsl:240` and `Source/RenderPasses/LightingPasses/LightingPasses.cpp:49`: `minSecondaryRoughness = 0.5`
- `ThirdParty/_deps/src/rtxdi/Source/ReSTIRDI.cpp:46`: the library's `numBrdfSamples = 1`
- RTXDI `Doc/NoiseAndBias.md` §Denoiser friendliness and `Doc/RestirGI.md:47` (the roughness floor's purpose)

## Status on 2026-11-23

| I12 item | Built | Measured on Bistro | Decided |
|---|---|---|---|
| O2 secondary DI resampling, ray-traced (`GiSettings::isSecondaryResamplingEnabled`, `path_shading.slang` `secondaryResamplingParams`: `Ultra`'s 2 samples, 4 px, `RTXDI_BIAS_CORRECTION_RAY_TRACED`; lab `gi.secondaryResampling`) | yes, off | no; Sponza only (+0.62% courtyard bias, +0.058 ms, §2.7) | no |
| O6 X₂ firefly clamp at scene luminance 10 (`isFireflyClampEnabled`; lab `gi.fireflyClamp`) | yes, off | no; F23 tried it once on the café tables (dim tiles still +23.1%) | no |
| O8 exit: the BRDF candidate's value (`Settings::brdfSamples`, 1 in every set but `converged`; lab `di.brdfSamples`) | the switch, and I6's skip on scenes without emissive or environment light | no | no |
| O9 roughness floor 0.5 at X₂ (`isSecondaryRoughnessFloorEnabled`, on in the shipped and NVIDIA sets; lab `gi.roughnessFloor`) | yes, on | no; neutral on Sponza, whose X₂ is almost all rougher than 0.5 (§2.9) | no |
| A glossy scene for O9 | no: the lab's `glossy.floor` / `glossy.metal` cases wait for one (C-FT-3). `models/bistro_mirrors` (bar and archway mirrors, metallic-roughness textures, in Bistro's world frame) is in the repository but never loaded | – | – |
| Many-light poses | yes: `bistro_cafe` (tables, dining; emissive only), `bistro` (street windows, bar shelf; sun + emitters, I13), lab cases `cafe.tables`, `cafe.dining`, `bistro.street`, `glass.window` | – | – |
| I5 (GI) and I6 (DI) decisions on a many-light pose | – | partly: I6's `Power_RIS` was decided on Bistro's exterior for cost (F12); I6's DI and I5's GI were measured for stability only on Sponza and the exterior; the café runs (F22, F23) found both wrong there | no |
| F22 / F23 Bistro re-check | – | diagnosed (C-I12-1, C-I12-2, lab `f22_quick`, `di-ofat`, `gi-ofat`) | fixes moved to I15 (planned); shipped DI still `spatialSamples = 0`, no permutation |
| NVIDIA sets match the FullSample `Medium` | – | – | **no:** `nvidiaGiSettings()` leaves O2 and O6 off, while `Medium` resamples at X₂ and always clamps (§1.2) |

So I12 has its switches and runners, but no Bistro number and no decision. Its order is Phase 0 now, then Phases 1–3 on the tree after I14 and I15 Phase 1. Decisions taken before I14 would be made on rays and reuse that I14 changes (F24–F27).

## Scope

| Item | Change | Files |
|---|---|---|
| Glossy scene | `ENGINE_CHECK_SCENE=bistro_glossy`: Bistro plus `models/bistro_mirrors/bistro_bar_mirrors.gltf` at the origin, two poses facing the café's bar mirrors; lab cases `glossy.floor` (renamed `glossy.mirror`) and `glossy.metal` built on it or dropped (§1.1) | `src/core/check_poses.hpp`, `src/core/vk_engine.cpp`, `src/lab/lab_cases.hpp` |
| NVIDIA GI set | `nvidiaGiSettings()` turns O2 and O6 on, as `Medium` does (§1.2) | `src/render/vk_rtxdi.cpp` |
| I12 lab plan | `tests/lab/i12.lab`: the four levers one at a time on every many-light case, both directions, 3 seeds (§2) | `tests/lab/i12.lab` |
| Decisions | O2, O6, O8, O9 shipped on or off by §2's rules; I5 / I6 settings changed only by §3's rules | `src/render/vk_rtxdi.cpp` (`defaultSettings`, `defaultGiSettings`) |
| Docs | findings rows O2, O6, O8, O9, F22, F23; `docs/settings.md` §3, §7; considerations | `spec/…`, `docs/settings.md` |

## Decisions

| Decision | Rule | Why |
|---|---|---|
| Measure after I14 and I15 Phase 1 | Phases 1–3 run on a tree with I14's four phases and I15's DI fix landed. If I15 Phase 2 (GI) is still open, Phase 1's GI numbers are taken on its tree, recorded as such, and re-read when it lands | F24–F27 change shadow rays, BRDF rays, reuse similarity and Bistro's light PDF, which every I12 lever reads |
| Lab first, runners for the decision | The lab plan ranks the levers by effect in noise floors; the shipped change is then confirmed with the check runners (GI check, stability, screen edge, convergence) and ABBA benches | the lab is for search, the checks measure what ships (`docs/settings.md` §9) |
| One shipped set for both scenes | A lever ships only if it holds on Sponza and Bistro; no per-scene defaults | the engine has one default set; per-scene tuning hides regressions |
| NVIDIA sets mirror the sample | `nvidiaSettings()` / `nvidiaGiSettings()` follow the FullSample's `Medium` preset, including O2 and O6; tuning never edits them | NVIDIA-defaults rule (C-M4-2): they are the comparison baseline |
| Glossy scene from the repository's own asset | Bistro + its bar mirrors, not a new download | the mirrors are already in the repository in Bistro's frame, and glossy X₂ in a many-light interior is the case O9 and O2 target |

## 1. Phase 0 · Tooling (now, no dependency)

### 1.1 Glossy scene

- **Load:** `CheckScene::BistroGlossy` (`ENGINE_CHECK_SCENE=bistro_glossy`) loads `bistro/bistro.gltf` and then `bistro_mirrors/bistro_bar_mirrors.gltf` at `(0, 0, 0)` through `Engine::loadModel`. `checkSceneModel` stays one path; a new `checkSceneExtraModels(scene)` returns the list. The bar mirrors' nodes sit at `x −1.39…0.07`, `y 2.41`, `z 12.68…15.38` (rotation −29° about y), on the café wall beside the street windows.
- **Placement check:** a frame PNG of the `Lit` and `Roughness` views must show the mirrors flush with the wall, reflecting the café. If they float or intersect, the asset is not in Bistro's frame: record its offset in `check_poses.hpp` beside the poses, or drop the scene and fall back to the café's glossiest bottles (bar shelf pose), recording that O9 has no strong glossy case.
- **Poses:** two `kBistroGlossyCheckPoses` picked from frame PNGs, each with at least 5% of the frame's pixels at roughness below 0.1 in the `Roughness` view (the mirrors), mirrors facing lit walls: one from the dining tables toward the mirrors, one at a grazing angle along them.
- **Runners:** the GI check, stability and screen edge accept the scene like `bistro_cafe` (measured, not gated). The emissive-only café interior is its light; the sun stays at the default.
- **Lab:** `glossy.floor` becomes `glossy.mirror` on pose 1, `glossy.metal` on pose 2 if it differs enough (another surface class), else removed. The built-in `stability-levers` plan (`lab_plan.cpp`) names both cases and is updated with them; C-FT-3 updated.

### 1.2 NVIDIA GI set

`nvidiaGiSettings()` sets `isSecondaryResamplingEnabled = true` and `isFireflyClampEnabled = true`, which is what the FullSample's `Medium` runs:

- `ShadeSecondarySurfaces.hlsl:105` resamples when `enableSecondaryResampling`, which `Medium` sets;
- `ShadeSecondarySurfaces.hlsl:134` clamps unconditionally.

The engine's O2 parameters stay `Ultra`'s (2 samples, 4 px, ray-traced): `Medium`'s 1 sample rounds up to 2 in `NormalizeDISpatialSampleCount`, and its 1 px radius truncates every offset to `(0, 0)` (§2.6). Record both differences in the function's comment. The GI check's `NVIDIA defaults` lines and the `nvidia` lab set move; the shipped and converged sets do not. Sponza runners on the shipped set read byte-identical.

## 2. Phase 1 · The four levers on Bistro (after I14, I15 Phase 1)

**Cases:**

| Case | Scene, pose | Light | Kinds |
|---|---|---|---|
| `cafe.tables`, `cafe.dining` | `bistro_cafe` | 58K emissive triangles, no sun | stability, DI bias, GI bias, screen edge |
| `bistro.street` | Bistro startup | sun + streetlamps (46% of the flux, F27) | stability, DI bias, cost |
| `glass.window` | `bistro` street windows | sun 110° / 35° + emitters | stability, GI bias |
| `glossy.mirror` (+ `glossy.metal`) | `bistro_glossy` | café emitters | stability, GI bias |
| `sponza.gallery`, `sponza.courtyard` | Sponza | sun | all (regression) |

**Lab plan** `tests/lab/i12.lab`: `base` = the shipped sets of the tree, `ofat` over `gi.secondaryResampling`, `gi.fireflyClamp`, `di.brdfSamples` (1 → 0), `gi.roughnessFloor` (on → off), both directions, 3 seeds, views `Lit`, `Direct lighting`, `Indirect radiance`. Then `<plan>_cost.ps1` (ABBA) for the levers whose effect passes 2 noise floors.

**Per lever:**

| Lever | What decides it | Ships on when | Otherwise |
|---|---|---|---|
| O2 secondary DI resampling | GI check bias and dim tiles on `bistro_cafe` and `glossy.mirror` (X₂'s light choice is the many-light case); `Lit` and `Indirect radiance` stability; `Indirect` cost | GI bias magnitude falls by more than 2 floors on at least one Bistro case, no case or Sponza pose regresses by more than 1 floor, and the cost is ≤ 0.10 ms GPU on `bistro.street` | stays off; the NVIDIA set keeps it on (§1.2) |
| O6 X₂ firefly clamp | `Lit` temporal std, flicker and the largest 8-frame swing on every Bistro case; GI check bias (the clamp loses energy by design) | flicker or swing falls by more than 2 floors on a Bistro case, and GI bias moves by less than 1% on every case | stays off |
| O8 BRDF candidate (`brdfSamples` 1 vs 0) | DI bias against RTXDI's reference (`bistro_cafe` stability's `Direct lighting` reference step), the three DI convergence checks (`guide.emissive` pins its own 1), `ReSTIR DI initial` cost | 0 ships if it saves ≥ 0.03 ms on `bistro.street` with DI bias and stability within 1 floor on every case; else 1 stays (the library's and `Medium`'s) | 1 stays; O8 closes either way |
| O9 roughness floor 0.5 | `glossy.mirror` GI check bias (the floor darkens or brightens glossy X₂ against the traced BRDF) and `Lit` stability | stays on if its bias on `glossy.mirror` is within ±3% of the floor-off phase, or its stability gain exceeds 2 floors | off in the shipped set (the NVIDIA set keeps it on) |

A lever that wins on Bistro and is neutral on Sponza ships; one that wins on Bistro only with a Sponza regression beyond 1 floor is recorded with its numbers and stays off.

## 3. Phase 2 · I5 and I6 on many-light poses

Re-run on the Phase 1 tree, on `cafe.tables`, `cafe.dining` and `bistro.street`, with I15's shipped sets as the base:

| Decision | Step | Re-checked as | Changed only if |
|---|---|---|---|
| `Power_RIS` over `ReGIR_RIS` (F12) | I6 | DI bias, dim tiles and `Lit` stability for both modes on the café poses; cost on `bistro.street` (I14 Phase 4 already tunes `ReGIR_RIS` for the exterior) | `ReGIR_RIS` wins DI bias or stability by more than 2 floors on both café poses at ≤ 0.10 ms extra |
| fresh final visibility (O10), disocclusion boost 8 while `M < 4` | I6 | stability and screen edge with each reverted | reverting wins by more than 2 floors on a Bistro case with no Sponza regression |
| GI spatial samples 1, ray-traced correction, no duplication reduction, unlimited age | I5 | GI check and stability, each reverted to NVIDIA's value | as above |
| the DLSS-RR profile's swap and its bound | I5 / I15 Phase 2 | read from I15's result; re-run only if I14 Phase 3 landed after I15 Phase 2 | I15's rule |

The built-in `stability-levers` plan already sweeps DI spatial reuse, permutation and ReGIR (levers L1, L2) over these cases. Add the O10, disocclusion-boost and I5 reverts as levers in `tests/lab/i12.lab`, run both, and confirm the decision lines with the runners.

## 4. Phase 3 · F22 and F23 on Bistro after I15

- `ENGINE_CHECK_SCENE=bistro_cafe` stability (all DI sets, NRC modes) and GI check on 5 seeds over the final shipped sets.
- **F22 closes** when shipped café `Lit` temporal std and flicker are within 2 floors of the best DI set I15 measured, with DI bias within ±2% of RTXDI's reference and no dark leak.
- **F23 closes** when shipped café GI bias is within the noise floor (±5%) and dim tiles within 2 floors of the one-bounce ground truth, or stays open with the numbers and C-I12-2.
- Bistro exterior (`bistro.street`, `glass.window`) and Sponza read no regression beyond 1 floor against the pre-I12 tree.

## 5. Checks

- **Phase 0:** Sponza GI check, stability, light change and screen edge byte-identical on the shipped set against the pre-I12 tree (the NRC phases excepted, C-T-5), since only the NVIDIA set and new scenes change. The `NVIDIA defaults` GI-check lines are recorded before and after. A frame PNG of each glossy pose. CTest (`lab_registry_test` for the renamed cases).
- **Each shipped change:** GI check (Sponza gated, Bistro scenes measured), the 3 DI convergence checks, stability and screen edge on Sponza and `bistro_cafe`, `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST`, `ENGINE_GUIDE_CAPTURE`, I14's `ENGINE_DLSS_CHECK`; validation core + sync + best practices; GPU-AV with `ENGINE_GPUAV_RESOLVE_PER_BIN=1` while C-E1-3 holds.
- **Cost:** ABBA benches, release, no layers, on `bistro.street`, Bistro's glass poses (`ENGINE_BENCH_POSE`) and the Sponza courtyard.

## 6. Measurements to record

| Item | Where | Decision it feeds |
|---|---|---|
| Each lever on / off: GI bias, dim tiles, `Lit` std / flicker / swing, effect in floors | every case of §2 | §2's rules |
| Each lever's cost (ABBA) | `bistro.street`, Sponza courtyard | §2's cost limits |
| `Power_RIS` vs `ReGIR_RIS` and I5 / I6 reverts on the café | §3's poses | §3's rules |
| Final café stability and GI bias, 5 seeds | `bistro_cafe` | F22, F23 closure |
| NVIDIA GI set before / after §1.2 | GI check `NVIDIA defaults` lines | record only |

## 7. Considerations opened

- **C-I12-3 · Limitation:** the glossy scene is Bistro's bar mirrors only: one glossy surface class in one interior. Glossy floors and metals elsewhere stay untested unless §1.1 finds a second pose.
- **C-I12-4 · Deviation:** the NVIDIA GI set's O2 runs `Ultra`'s 2 samples at 4 px, not `Medium`'s 1 sample at 1 px (§1.2).
- **C-I12-5 · Open (if any):** a lever that wins on Bistro but regresses Sponza, kept off with its numbers.

## 8. Results

To be filled when built.

## Done when

- [ ] Phase 0: `bistro_glossy` loads Bistro + bar mirrors with a placement PNG, two poses recorded; glossy lab cases built or dropped (C-FT-3); `nvidiaGiSettings()` carries O2 and O6; Sponza shipped runners byte-identical
- [ ] Phase 1: O2, O6, O8, O9 each shipped on or off by §2's rule with Bistro and Sponza numbers; findings rows closed
- [ ] Phase 2: `Power_RIS`, O10, the disocclusion boost and I5's GI settings re-checked on the café and street, changed only by §3's rule
- [ ] Phase 3: F22 and F23 closed on Bistro after I15, or left open with the numbers (C-I12-1, C-I12-2)
- [ ] Every check of §5 passes; validation and GPU-AV clean; NVIDIA sets callable and matching `Medium`
- [ ] `m8_optimization_and_fixes.md`, `docs/settings.md` §3 and §7, the considerations and this §8 updated
