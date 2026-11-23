# M8 · I15: Café DI and GI settings from the lab (F22, F23)

**Status:** planned 2026-11-22 · **Milestone:** M8 ([README](./README.md))
**Implements:** the settings fixes for F22 and F23 ([m8_optimization_and_fixes.md](./m8_optimization_and_fixes.md) §5) found by the settings lab's first runs (`f22_quick`, `di-ofat`, `gi-ofat`, 2026-11-22, [features/planned/full_testing.md](../features/planned/full_testing.md)). Also T6's validation half for the lab (does the lab name the culprit the hand runs found)
**Depends on:** I1, I5, I6, the settings lab (T1–T5). Phase 2 lands after I14 Phase 3 ([i14_bistro_sampling_and_rays.md](./i14_bistro_sampling_and_rays.md))
**Feeds:** I12 (re-checks both phases on Bistro after I14)
**References:**
- RTXDI `Doc/NoiseAndBias.md` §Denoiser friendliness (one spatial sample breaks up DLSS-RR's blotches) and `Doc/RestirPT.md` §Decorrelation ("firefly replacement on its own brightens the image")
- `Source/ReSTIRPT.cpp` `GetDefaultReSTIRPTDecorrelationParams(true)`: factor 0.4, exponent 0.5, EMA 0.2, firefly strength 0.7, bound on, `MultiplyBound` 15
- `src/render/vk_rtxdi.cpp:485-503` (`defaultGiSettings`: bound off since I5), `shaders/passes/rtxdi_gi_shade.slang:310-313` (the fresh-swap bound)
- lab logs and CSVs: `build/cafe-release/logs/lab/{f22_quick,di-ofat,gi-ofat}.{log,csv}`

## Scope

### Where the lab's findings stand in the other steps

| Finding (lab, 2026-11-22) | Already in a step? | Here |
|---|---|---|
| F22: DI spatial reuse and permutation stop the café confetti | no. I12 lists F22 as a Bistro re-check; I6 shipped `spatialSamples = 0`, permutation off, from Sponza and Bistro exterior numbers | Phase 1 |
| F23: the GI firefly-replacement bound changes the café's dim-tile excess | no. The F23 row suspects the bound; I12 re-checks; I14 fixes correctness only | Phase 2 |
| GI stagnancy decorrelation helps Sponza and hurts the café | no. I5 kept it from Sponza numbers | Phase 2 |
| Ray-traced GI spatial correction is what brings NVIDIA's set off its Sponza darkening | **yes:** I5 shipped NVIDIA + ray-traced correction ([i5_restir_gi_tuning.md](./i5_restir_gi_tuning.md)) | nothing new; the lab agrees |
| The `gi-ofat` bisection ended as noise on `cafe.dining` | not a step item | Phase 2 uses 5 seeds and reads dim tiles on `cafe.tables` |

### The `gi-ofat` numbers

These are absolute numbers, not deltas. They correct one reading: turning the bound on does not fix F23, it overshoots.

One-bounce GI bias against the traced truth. DI is shipped, jitter is off, seed 0 unless a range is given. Signed %; above 0 is too bright.

| Config | `cafe.tables` bias / dim tiles | `cafe.dining` bias / dim tiles | `sponza.gallery` bias / dim tiles |
|---|---|---|---|
| shipped (3 seeds) | +9.3…+10.7 / +25.3…+27.5 | +7.6…+11.8 / +10.0…+30.8 | −2.0…+0.9 / +2.4…+6.1 |
| `gi.fireflyBound=on` | −9.1 / −4.7 | −15.5 / −13.7 | −6.6 / −14.5 |
| `gi.decorrelation=none` | −4.1 / +3.9 | −5.6 / −0.7 | +0.5 / −1.2 |
| `gi.fireflyReplacement=off` | −2.0 / +6.6 | −8.4 / −6.6 | −3.9 / −9.8 |
| `gi.age=30` | +4.8 / +15.4 | +0.2 / +8.1 | −0.3 / +2.1 |
| NVIDIA GI | −19.0 / −18.7 | −41.7 / −7.7 | −25.5 / −32.5 |

`Lit` stability (temporal std % / flickering px %), DI shipped:

| Config | `cafe.tables` | `cafe.dining` | `sponza.gallery` |
|---|---|---|---|
| shipped (3 seeds) | 22.9…27.4 / 86–87 | 14.4…17.9 / 81–84 | 5.5…5.6 / 36–40 |
| `gi.decorrelation=none` | 13.3 / 78 | 9.0 / 52 | **11.3 / 69** |
| `gi.fireflyBound=on` | 32.4 / 89 | 15.4 / 79 | 5.8 / 43 |
| `gi.age=30` | 22.6 / 86 | 15.0 / 81 | 5.2 / **20** |

What the tables show:

- **The excess rides on the decorrelation swap,** as F23 suspected: every switch that weakens it pulls the café toward 0.
- **The bound is the right lever at the wrong strength.** At NVIDIA's ×15 it overshoots on all three cases, and it brings back the Sponza darkening I5 turned it off for (dim tiles −14.5).
- **The café stability numbers in `gi-ofat` are confounded by F22.** DI was the shipped DI, and the café's `Lit` std is mostly DI confetti. The hand runs found the reverse once DI is fixed: with decorrelated DI, shipped GI is the stablest set (F23 row: `8.10% / 6.31%` with GI decorrelation, `13.05% / 8.76%` without). So every Phase 2 run uses Phase 1's DI.
- **Out of scope here:** the 8-bounce bias (−22 to −39 %) is the missing multi-bounce energy that NRC adds back (F23 row), not a ReSTIR GI fault.

## Phase 1 · F22: DI reuse in the café

Two candidates, both measured by the lab (`cafe.tables` / `cafe.dining`):

| DI config | `Lit` std | `Lit` flicker | DI bias against RTXDI's reference (hand runs) |
|---|---|---|---|
| shipped (`spatialSamples = 0`, permutation off) | 26.7 / 16.7 | 87 / 83 | −0.04 / −1.05 |
| **A:** `di.spatialSamples=2` | 10.8 / 6.6 | 35 / 32 | −0.11 / −0.70 |
| **B:** A + `di.permutation=on` | 7.7 / 6.3 | 28 / 33 | −0.57 / −3.82 |

The std noise floor on the café is 6.25 points (seeds 20.5–26.7), so A and B do not differ in std. Flicker separates them (floor ~1 point): A against B is 35 against 28 on the tables and 32 against 33 on dining. The `di-ofat` ranking agrees: permutation and spatial samples are the only DI switches with an effect on `Lit`, at 2.6–3.0 floors on std and 41–60 floors on flicker.

- **Rule:**
  - ship B only if its tables flicker gain over A holds on 5 seeds;
  - and its DI bias passes on both café poses (`diBias`, 1024-frame reference) and on the convergence check (3 of 3; the lamp read −2.2 % under permutation, findings §2.7).
  - Otherwise ship A.
- **Also measured:**
  - Sponza gallery / courtyard and `bistro.street` stability, no worse than I6's numbers (`ENGINE_STABILITY_CHECK`);
  - ABBA cost of the DI spatial pass on Sponza gallery, courtyard and Bistro (bench mode has no café pose). I6 traded this cost away, so the cost goes in the decision line.
- **NVIDIA's DI set stays callable** (C-M4-2). The change is to `RtxdiContext::defaultSettings()` only.

Lab plan `tests/lab/i15_di.lab`; it must parse in `lab_registry_test`:

```
# I15 phase 1: the F22 fix, A against B, 5 seeds, on the café and the scenes I6 tuned on
base    di=shipped gi=shipped
seeds   5
cases   cafe.tables cafe.dining sponza.gallery sponza.courtyard bistro.street
views   Lit "Direct lighting"
kinds   stability diBias
sets    di.spatialSamples=2
sets    di.spatialSamples=2 di.permutation=on
```

## Phase 2 · F23: GI swap strength over Phase 1's DI

**Timing:**
- Run the plan now, for direction.
- Land the result only after I14 Phase 3 (F26: reuse similarity), which changes which neighbors GI reuses and swaps with. Then re-run the plan on the I14 tree.

Lab plan `tests/lab/i15_gi.lab`. The `fix` is Phase 1's DI, so every lever runs over the fixed DI and on the shipped base:

```
# I15 phase 2: the F23 GI excess over the F22 DI fix; 5 seeds (dining's dim tiles spread +10..+31 at 3)
base    di=shipped gi=shipped
seeds   5
cases   cafe.tables cafe.dining sponza.gallery sponza.courtyard
views   Lit "Indirect radiance"
kinds   stability giBias
fix     di.spatialSamples=2 di.permutation=on
lever   G1 bound: gi.fireflyBound=on
sweep   gi.fireflyMultiplyBound 15 30 60 120 240 over fix gi.fireflyBound=on lever G1
sweep   gi.fireflyStrength 0.5 0.7 0.9 over fix lever G2
sweep   gi.decorrelationFactor 0.1 0.2 0.3 0.4 over fix lever G3
lever   G4 no decorrelation: gi.decorrelation=none
lever   G5 no replacement: gi.fireflyReplacement=off
lever   G6 age 30: gi.age=30
stack   "Indirect radiance:1b.dimBias"
```

Use Phase 1's choice in `fix`; the B shown is the current front-runner.

- **What each lever does:**
  - **G1:** the bound between off (+10 %) and ×15 (−9 to −15 %). NVIDIA's ×15 is tuned for ReSTIR PT, not for one-bounce GI.
  - **G2, G3:** a weaker swap.
  - **G4–G6:** the measured alternatives, now over the fixed DI.
  - **G6:** finite age halved Sponza's flicker (39 → 20) and kept its bias. I5 rejected it for the courtyard's darkening (`SpatialAgeReset` off), which `sponza.courtyard` re-measures.
- **Rule:** pick the config with the smallest café dim-tile bias, read on `cafe.tables` (on dining read bias only, since its dim-tile metric is noisy at 3 seeds). It must also meet all of the following:
  - café one-bounce bias within ±5 % (the F23 noise floor);
  - Sponza gallery and courtyard inside the GI check's per-pose limits;
  - Sponza `Lit` std and flicker within 2 floors of shipped;
  - café `Lit` flicker no worse than Phase 1's.
- **If no single lever qualifies:** take the `stack` line's pair, then run a `pairs` of the two best.
- **NVIDIA's GI set stays callable;** the change is to `defaultGiSettings()` only.
- **Lab validation (T6, GI half):** record whether the `gi-ofat` ranking named the lever that wins here. The bound ranked first on dining's dim tiles and the bisection ended as noise, so T6 also notes that a dim-tile bisection needs 5 seeds.

## Verification

- `ENGINE_GI_CHECK` 8 of 8 on five seeds (`ENGINE_GI_CHECK_SEED`), the café runners (`ENGINE_CHECK_SCENE=bistro_cafe`: stability, DI bias, GI bias), `ENGINE_STABILITY_CHECK`, `ENGINE_LIGHT_CHANGE_CHECK`, `ENGINE_SCREEN_EDGE_CHECK`, the convergence check (3 of 3), `ENGINE_MOTION_TEST`, `ENGINE_GUIDE_CAPTURE`, `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`.
- ABBA cost for Phase 1 (release, every layer off, ±5 % noise). Phase 2 levers are parameters and need no bench unless G6 changes the GI pass's work.
- Validation clean. Neither phase changes a shader, so GPU-AV is not required.
- The café checks' expected lines move with the new defaults. Their limits change only with the numbers recorded here.
- Runs go in a separate build tree.

## Done when

- [ ] Phase 1: A or B shipped in `defaultSettings()` with the 5-seed café, Sponza and Bistro numbers and the cost line; F22 marked fixed in the findings
- [ ] Phase 2: the plan run now and again on the I14 Phase 3 tree; the winning GI change shipped in `defaultGiSettings()` by the rule above, or F23 left open with the numbers if nothing qualifies
- [ ] every runner in Verification passes; NVIDIA's DI and GI sets unchanged and callable
- [ ] T6's GI-half note in [full_testing.md](../features/planned/full_testing.md) §9; I12's F22 / F23 item reduced to the Bistro re-check of both phases
