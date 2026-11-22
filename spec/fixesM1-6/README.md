# M8 · Optimization and fixes

**Status** — Milestone planned on 2026-11-20; steps re-planned on 2026-11-21 as I1–I12 in execution order, covering every open finding of the three review documents. I1 ([i1_check_runners.md](./i1_check_runners.md)), I2 ([i2_shading_correctness.md](./i2_shading_correctness.md)), I3 ([i3_mega_geometry_fixes.md](./i3_mega_geometry_fixes.md)), I4 ([i4_frame_profile.md](./i4_frame_profile.md)), I5 ([i5_restir_gi_tuning.md](./i5_restir_gi_tuning.md)), I6 ([i6_restir_di_tuning.md](./i6_restir_di_tuning.md)), I7 ([i7_nrc_bias.md](./i7_nrc_bias.md)), I8 ([i8_nrc_cost.md](./i8_nrc_cost.md)), and I9 ([i9_rtxdi_surface_traffic.md](./i9_rtxdi_surface_traffic.md)) built 2026-11-21–22; remaining I4-ranked step: **I10**. I13 (thin-walled glass, [i13_glass_transmission.md](./i13_glass_transmission.md)), I14 (Bistro sampling, reuse, DLSS-RR and rays, [i14_bistro_sampling_and_rays.md](./i14_bistro_sampling_and_rays.md)), and I15 (café DI and GI settings from the lab, [i15_cafe_di_gi_settings.md](./i15_cafe_di_gi_settings.md)) planned 2026-11-22.\
**Scope** — Make the whole pipeline produce its best image per millisecond. That means fixing the measured bias and stability problems of the default lighting, closing or explaining the NRC bias, and landing the measured perf levers. Each step is backed by a check that keeps it fixed.
[Engine spec](../../docs/engine_spec.md) · [Findings](./m8_optimization_and_fixes.md) · [Mega Geometry findings](./mg_optimizations_fixes.md) · [PBR plan](../pbr_implementation_plan.md) · [Considerations](../considerations.md)

Finding ids refer to [m8_optimization_and_fixes.md](./m8_optimization_and_fixes.md) (O, Q, ND, NP, F) and [mg_optimizations_fixes.md](./mg_optimizations_fixes.md) (MG-B, MG-P). Each step gets its own spec in this folder (`i1_check_runners.md`, …) before its code, written after the step's reference is studied in `reference/`.

## Done ahead of the steps

| Item | Result | Where |
|---|---|---|
| O1 GI settings for DLSS-RR | shipped GI = NVIDIA's + `Ray traced` spatial correction (2026-11-20), superseded by O11 | findings §2.5 |
| O2, O6 secondary DI resampling, X₂ firefly clamp | built behind flags, off; no gain on Sponza | findings §2.6, §2.7; re-check in I12 |
| Shipped DI set | NVIDIA `Medium` + `Ray traced` spatial correction, bridge fix under permutation (2026-11-21) | findings §2.7 |
| O3 GI temporal reuse in the indirect pass | measured without gain, reverted (2026-11-21) | findings §2.8; closed |
| O9 secondary roughness floor | built, on by default, neutral on Sponza | findings §2.9; glossy check in I12 |
| O11 ReSTIR PT's DLSS-RR profile on ReSTIR GI | built and shipped without history reduction and swap bound | findings §2.10, C-O11-1; follow-ups in I2, I5 |
| F1 GI-check seeding | seeded per phase | findings F1; spread measured in I1 |
| F10 auto-exposure boiling | fixed (F1–F4 of the fix document) | [auto_exposure_dark_boiling.md](./auto_exposure_dark_boiling.md); its F5 waits on I7 and an environment light (C-F3-5) |
| Sampling audit | five correctness fixes (reflection rays below the geometric surface, boiling-filter padding reads, DI temporal visibility, GGX peak precision, ray-cone distance) | [rtxdi_rtxgi_sampling_audit.md](./rtxdi_rtxgi_sampling_audit.md) |

## Steps

| Step | Spec | Findings | Depends on |
|---|---|---|---|
| I1 · Check runners | [`i1_check_runners.md`](./i1_check_runners.md) (built) | F1 (spread), F2, F3 | — |
| I2 · Shading correctness | [`i2_shading_correctness.md`](./i2_shading_correctness.md) (built) | F20, F13, F14, F15, F16, F17, F18, F19 (ND9) | I1 |
| I3 · Mega Geometry fixes | [`i3_mega_geometry_fixes.md`](./i3_mega_geometry_fixes.md) (built) | MG-B1, MG-B2, MG-B3, MG-B4, C-C4-5 | — |
| I4 · Frame profile | [`i4_frame_profile.md`](./i4_frame_profile.md) (built) | F4, F7, F8, F9 (RTX 5070 part), F11, F12, C-F1-5 | I2, I3 |
| I5 · ReSTIR GI tuning | [`i5_restir_gi_tuning.md`](./i5_restir_gi_tuning.md) (built) | O11 follow-ups, age limit (§2.5), ray-traced spatial cost (§2.5), F3 (GI) | I1, I2, I4 |
| I6 · ReSTIR DI tuning | [`i6_restir_di_tuning.md`](./i6_restir_di_tuning.md) (built) | F5, F6, O8, O10, permutation (§2.7), F3 (DI), F12 (mode choice) | I1, I2, I4 |
| I7 · NRC bias | [`i7_nrc_bias.md`](./i7_nrc_bias.md) (built) | Q7, Q1, Q2 (ND5), Q3 (ND1), Q4, Q5, Q6 (ND6) | I1, I2 |
| I8 · NRC cost | [`i8_nrc_cost.md`](./i8_nrc_cost.md) (built) | NP1 (ND7), NP2, NP4, NP5 | I7 |
| I9 · RTXDI surface traffic | [`i9_rtxdi_surface_traffic.md`](./i9_rtxdi_surface_traffic.md) (built) | O4 | I4 |
| I10 · Mega Geometry perf | [i10_mega_geometry_perf.md](./i10_mega_geometry_perf.md) | MG-S1–MG-S3, MG-P1–MG-P7 | I3, I4 |
| I11 · Floor tier | `i11_floor_tier.md` | F9 (2060 Super part), O7, NP3, DLSS-RR mode per tier | I4 (needs the 2060 Super) |
| I12 · Many-light and glossy re-check | `i12_many_light_recheck.md` | O2, O6, O8 (exit), O9, I5 / I6 decisions on Bistro, F22, F23 | I5, I6, I14 |
| I13 · Thin-walled glass | [`i13_glass_transmission.md`](./i13_glass_transmission.md) (planned) | F21 | I1, I2, I6 |
| I14 · Bistro sampling, reuse, DLSS-RR and rays | [`i14_bistro_sampling_and_rays.md`](./i14_bistro_sampling_and_rays.md) (planned) | F24, F25, F26, F27 (feeds F22, F23) | I1, I2, I6 |
| I15 · Café DI and GI settings | [`i15_cafe_di_gi_settings.md`](./i15_cafe_di_gi_settings.md) (planned) | F22, F23 (settings fixes from the lab) | I1, I5, I6, settings lab; Phase 2 after I14 Phase 3 |

Not scheduled: O5 (removed from the candidates on 2026-11-20), ND2–ND4 (design choices), ND8 (waits on an environment light, C-F3-5), and the post-1.0 RTXMG features (mg findings §4).

### I1 · Check runners

Every later decision is read from these runners, so they come first.

- **GI-check spread (F1):** run the courtyard's converged gate over several seeds and set its limit from that spread.
- **Stability runner** `ENGINE_STABILITY_CHECK` (F2), from the scratch probe of findings §2.5:
  - static camera on the gallery and courtyard poses, per settings set (shipped, NVIDIA, converged, O11 parts off);
  - reads `Lit` and `Indirect` temporal std, flickering px and mean;
  - records its lit-pixel cutoff, and finds or handles the first-run artifact.
- **Light-change runner** (F3): sun toggle and sun move; frames until `Indirect radiance` is within 2% of its new converged mean.
- **Screen-edge runner** (F3): scripted pan and backward dolly; frames until the revealed band converges.

**Done when:** each runner is deterministic per frame index, documented in memory, and has recorded a baseline of the current shipped set (O11) on both poses. That baseline replaces the 2026-11-20 courtyard numbers, which are not comparable (F2).

### I2 · Shading correctness

The open bugs from the 2026-11-21 dark-boiling audit, after F20. One change per finding, each with the GI check and the stability runner before and after:

- **F20 first:** the shipped set's bright blotches on a static courtyard (`Lit` temporal std 24.7%, 8-frame swing 22.6%; 1.3% / 0.2% with NVIDIA's age limit). Bisect GI reservoirs against DLSS-RR's response to jittered input; the age-limit decision itself stays with I5, but every other I2 measurement reads through this.

- **F13:** a zero-radiance GI hit stores the far miss placeholder, so dark reservoirs no longer fail Jacobian validation and drop their M. With NRC on, the placeholder is written after the NRC resolve.
- **F14:** stagnancy reads 0 for a reservoir with `weightSum == 0`, and the swap bound no longer zeroes a valid initial sample. Re-measure the swap bound's darkening afterwards (findings §2.10).
- **F15:** compare RTXDI's boiling filter (blind when 5 or fewer pixels of a dark 8×8 tile are non-zero) against a test averaged over every in-bounds lane, in both the temporal pass and the firefly replacement. NVIDIA's filter stays callable.
- **F16:** GI spatial reuse reads its own `LowDiscrepancy` neighbor table, as the RTXDI sample binds one.
- **F17:** A/B DLSS-RR's world-to-view and view-to-clip matrices without `glm::transpose`, checked against `reference/vk_denoise_dlssrr` and NGX's header first. Measure on static and moving dark Sponza shots.
- **F18:** secondary-surface light sampling matches the RTXDI sample (`ShadeSecondarySurfaces.hlsl`), so it stops tracing a visibility ray that `shadeLightReservoir` then repeats. Measure the cost too.
- **F19:** the shading normal is flipped into the geometric normal's hemisphere, as NVIDIA's path tracer does. This also lowers NRC's `BRDFAbsorption` exits (I7 Q7).

**Done when:** every gated GI-check phase passes, the shipped set's bias against one bounce is recorded, and the stability runner reads no worse than I1's baseline on both poses. Validation is clean, with GPU-AV for the changed shaders.

### I3 · Mega Geometry fixes

Independent of the lighting work, and it can run alongside I1 and I2.

- **MG-B1:** micromap storage allocated at its 256-byte alignment (`VUID-VkMicromapCreateInfoEXT-offset-07438`).
- **MG-B2:** GPU-to-CPU readbacks from host-cached memory; BLAS addresses kept on the CPU.
- **MG-B3:** PTLAS growth clamped to `maxInstanceCount`; sizes logged as 64-bit.
- **MG-B4 / C-C4-5:** scene bounds (NRC) grow when an entity leaves its layout-time box; partition reassignment is measured on the moving instance field.

**Done when:** mg findings §5 holds: validation clean, every runner green, and `ENGINE_RT_CHECK` unchanged.

### I4 · Frame profile

Measured after I2 and I3, so the profile times the fixed code.

- A separate TLAS-wait span (F4) and a timer for the async light preparation (C-F1-5).
- Per-scope tables in all three indirect modes; an Nsight GPU Trace of the default frame with each pass's limiter (ray throughput, memory, occupancy); Tracy's CPU frame against the 2 ms budget.
- Re-measured: PTLAS update minimum cost (F7), O(entities) frame writes (F8), the binned resolve's overhead (F11), and ReGIR against `Power_RIS` beside the RTXDI sample on the same GPU (F12).
- The async-compute overlap capture (F9, RTX 5070 part).

**Done when:** a ranked list of levers with expected gains is recorded. It orders I5, I6, I9 and I10. A lever outside these steps gets its own step spec only if it ranks.

### I5 · ReSTIR GI tuning

- **O11 follow-ups:**
  - stability of the shipped profile under DLSS-RR, against the profile parts off and NVIDIA's set;
  - why duplication-based history reduction darkens GI;
  - duplication read at the temporal search's chosen neighbor, as NVIDIA's version does, instead of at the reprojected pixel;
  - the swap bound re-checked after I2's F14 fix.
- **Age limit:** the shipped set runs without one since O11. Keep that, or bring the limit back, from the light-change runner's settling time against the bias (findings §2.5).
- **Ray-traced spatial correction:** its +0.15 ms traded against spatial sample count and radius.
- **Screen edges (F3):** GI's history and disocclusion behavior read from the screen-edge runner.

NVIDIA's set stays callable and measured. **Done when:** the shipped phase is gated in the GI check on both poses, or its bias is a choice documented with the light-change numbers; stability is no worse than I1's baseline.

### I6 · ReSTIR DI tuning

- **Ray budget (F5):** candidate visibility, temporal correction, final visibility and the BRDF candidate, each measured for cost and for its effect on the convergence check and the stability runner.
- **O8:** no BRDF-candidate ray without emissive triangles or an environment light.
- **O10:** fresh final visibility against its reuse: shadow-edge leak, thin-shadow fading and flicker against ~0.7 extra rays per pixel.
- **Permutation sampling** (findings §2.7): keep or drop it, from the stability runner.
- **F6:** NVIDIA `Medium` against converged settings through DLSS-RR, on sunlit Sponza; the shipped DI cost re-measured on a sunlit pose.
- **F12:** the default presampling mode decided from I4's ReGIR cost.
- **Screen edges (F3):** `disocclusionBoostSamples`, candidate counts and fallback sampling tuned from the screen-edge runner; a guard band only if tuning is not enough.

NVIDIA's `Medium` stays callable. **Done when:** every DI ray is landed or closed with its numbers, and the convergence check and stability runner pass.

### I7 · NRC bias

In this order (findings §3.3):

1. Q7 (`BRDFAbsorption` exits with zero continuation; a bug fix), Q1 (self-training attenuation) and Q2 (an SDK training-target debug view, ND5).
2. Q3: the query placement by NVIDIA's termination heuristic (ND1).
3. Q4 and Q5: training budget and radiance scale.
4. Q6 (training root attributes, ND6), only if the root still differs.

**Done when:** the NRC GI-check phases are gated, or C-G5-3's cause is known and logged.

### I8 · NRC cost

- NP1 (the C-G3-6 host wait, ND7): rechecked per driver and reported to NVIDIA.
- NP2: train only after camera, light or scene changes.
- NP5: skip queries whose throughput is below 0.01.
- NP4: the SER update and query passes, only once C-G1-3 lifts.

**Done when:** each lever is landed or closed with its numbers. NP3 is measured in I11.

### I9 · RTXDI surface traffic

- **O4:** one packed surface guide (octahedral normals, roughness, f0 / albedo) and position from depth, 36 → ~20 B per neighbor surface in the DI and GI reuse passes. Rebuilding position needs exact previous view matrices with jitter; `Position` is a guide by F2's decision.

**Done when:** an ABBA bench shows the gain against I4's table, and the GI check, the convergence check and `ENGINE_MOTION_TEST` pass. Otherwise O4 is closed with its numbers.

### I10 · Mega Geometry perf

Spec: [i10_mega_geometry_perf.md](./i10_mega_geometry_perf.md). First the items skipped before I10 (re-check of 2026-11-22): MG-S1 triangle BLAS vs CLAS on static content (F2 / E4, never measured), MG-S2 the per-primitive BLAS recheck at Bistro, MG-S3 Bistro and the café runners in the verification set. Then MG-P1–MG-P7, in I4's ranked order:

- MG-P1: opaque and OMM CLAS split;
- MG-P2: 7-bit position truncation with matching raster positions;
- MG-P3: spatial meshlets;
- MG-P4: ray-hit decode without dead loads;
- MG-P5: PTLAS partition layout and explicit instance boxes;
- MG-P6: load-time scratch sizing (the GPU-only build pipeline belongs to M9);
- MG-P7: `rt_instances` overlapping the light preparation.

**Done when:** each lever is landed with an ABBA gain on Sponza and the moving instance field, or closed with its numbers, under mg findings §5.

### I11 · Floor tier

On the RTX 2060 Super:

- every scope of I4 (F9: M0 baseline, full-screen DI, NRC support and cost);
- DLSS-RR cost and mode per tier;
- O7 checkerboard GI (and DI), only if `Indirect` exceeds its floor budget;
- NP3 (fewer training dimensions), if NRC runs there.

**Done when:** the per-scope table is recorded on both GPUs, and the floor tier's settings are decided.

### I12 · Many-light and glossy re-check

On Bistro, with M7's emissive triangles and presampling (spec-gloss materials, C-M7-1):

- O2 with ray-traced resampling, and O6;
- the BRDF candidate's value (O8 exit);
- O9 on glossy secondary surfaces;
- I5's and I6's decisions re-run on a many-light pose;
- the café's emissive-only interior (`ENGINE_CHECK_SCENE=bistro_cafe`): the Bistro re-check of I15's F22 (DI confetti under DLSS-RR) and F23 (GI dim-tile excess) fixes, after I14.

**Done when:** each switch is decided with its numbers on Bistro, and the shipped sets hold on both scenes and in the café (DI bias, dim tiles, `Lit` stability).

### I13 · Thin-walled glass

Bistro's glass (18 `KHR_materials_transmission` materials, smooth, no volume) renders opaque and blocks every ray (F21). Following the RTXDI FullSample's glass pass:

- glass leaves the vis buffer and gets its own ray mask, so lighting and the DLSS-RR guides describe the surface behind;
- a compute pass after `combine` composites Fresnel reflection, the glass's diffuse share and transmission throughput over up to 4 layers;
- shadow transmittance in final DI and GI shading, and the DLSS-RR guide variant, decided with Bistro numbers.

Refraction, volume and rough transmission are stage 2 (engine spec §15).

**Done when:** the glass throughput check passes on `TransmissionTest` and Bistro, the Sponza runners read byte-identical, and the pass cost is recorded on two Bistro poses.

### I14 · Bistro sampling, reuse, DLSS-RR and rays

Spec: [i14_bistro_sampling_and_rays.md](./i14_bistro_sampling_and_rays.md). Correctness fixes and checks from the 2026-11-22 four-path Bistro audit (F24–F27), landing ahead of I12's final tuning of F22 and F23:

- **Phase 1 (F24, rays):** geometric hemisphere guard in `shadeLightSample` and `RAB_SurfaceImportanceSampleBrdf`, removal of opaque `side = -1` punch-through, degenerate-triangle NaN guard in `safeSpawnPoint`, offset BRDF ray origin with `TMin = 0`, and offset-origin visibility with bounded endpoint margins;
- **Phase 2 (F25, DLSS-RR):** off-screen behind-camera motion sentinel, jittered `resolveBackground`, `EnvBRDFApprox2` (`f0 == 0` guard) and `SpecAlbedo` precision A/B, startup ordering check, and an automated `ENGINE_DLSS_CHECK` (clean-input baseline + perturbation controls);
- **Phase 3 (F26, reuse similarity):** `geoNormal` vs `shadingNormal` in `RAB_GetSurfaceNormal`, resolution-scaled `samplingRadius`, `depthThreshold`, same-frame plane-distance guard in `RAB_AreMaterialsSimilar`, `isFallbackSamplingEnabled` A/B, and tighter material similarity thresholds;
- **Phase 4 (F27, Bistro light PDF):** independent Bistro textured-emission check, multi-tap `light_world` flux and zero-flux emitter culling (`hitEmission` dropout fix), shading-time texture eval A/B, and scaled `Power_RIS` vs tuned `ReGIR_RIS` for Bistro's exterior-dominated flux (primary and secondary NEE).

**Done when:** all four phases land with Sponza and `bistro_cafe` measurements, `ENGINE_DLSS_CHECK` and the independent Bistro textured-emission check pass, validation and GPU-AV are clean, and the clean baseline is handed off to I12.

### I15 · Café DI and GI settings

Spec: [i15_cafe_di_gi_settings.md](./i15_cafe_di_gi_settings.md). The settings fixes the lab's first runs (`f22_quick`, `di-ofat`, `gi-ofat`, 2026-11-22) found for F22 and F23, which no other step decided:

- **Phase 1 (F22, DI):** `spatialSamples = 2`, with or without permutation, cuts café `Lit` flicker from 87 % / 83 % to 28–35 %. It ships by flicker on 5 seeds, the DI bias against RTXDI's reference, the convergence check and the cost;
- **Phase 2 (F23, GI):** the café's +10 % (dim tiles +25 %) excess rides on the decorrelation swap. The firefly bound at NVIDIA's ×15 overshoots (−9 to −15 %, Sponza dim tiles −14.5 %), so its multiplier, the swap strength, the decorrelation factor and age 30 are swept over Phase 1's DI. Landed after I14 Phase 3.

**Done when:** both phases are shipped by their rules with café, Sponza and Bistro numbers, or F23 stays open with them, and every runner passes.

## Order

```
I1 → I2 → I4 → I5, I6, I9, I10   (independent; each ABBA-measured against I4's table)
I3 → I4                          (I3 can run alongside I1 and I2)
I1 + I2 → I7 → I8
I4 → I11                         (needs the 2060 Super)
I1 + I2 + I6 → I14 → I12         (I5 + I6 + I14 → I12; audit fixes before final Bistro tuning)
I5 + I6 + lab → I15 → I12        (I15 Phase 1 now; Phase 2 lands after I14 Phase 3)
I1 + I2 + I6 → I13
```

- **I1 first:** I2's fixes and every tuning step are judged by runners that do not exist yet.
- **I2 before tuning:** the audit's bugs distort every measurement after them. I2 comes before I7 too, since F19 changes NRC's absorption rate.
- **I4 before perf:** it orders the perf steps by measured gain, so no lever is taken on expectation.

The milestone number gives order of completion, not of start.

## Rules

- **NVIDIA's sets stay callable.** Every tuned default keeps NVIDIA's set behind the `NVIDIA defaults` button and in a measured GI-check phase, for the cost and quality comparison with NVIDIA's samples (C-M4-2).
- **A change lands with its numbers:**
  - GI check and stability runner before and after;
  - an ABBA bench for perf (release, no layers, ±5% noise);
  - validation clean, with GPU-AV on for shader changes;
  - `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST` and `ENGINE_GUIDE_CAPTURE` still pass.
- **A measured no-gain lever is closed with its numbers** in the findings document, as O1, O2, O3 and O6 were.

## Frame and acceptance

M8 adds one frame pass, I13's glass composite between `combine` and DLSS-RR; I1 adds runners only.

M8 is done when:

- the shipped ReSTIR GI phase is gated in the GI check on both poses, or its remaining bias is a choice documented with the light-change runner's numbers;
- the NRC phases are gated, or C-G5-3 has a known cause and stays a logged limitation of the comparison mode;
- the stability runner's `Lit` numbers on both poses are no worse than I1's baseline;
- every lever in I4's ranked list has either landed with its gain or been closed with its number, and the per-scope table is recorded before and after on the RTX 5070 and the 2060 Super (I11);
- every runner passes, the GI check passes on every gated phase, and validation and GPU-AV are clean.

## Baseline

2026-11-21 (post-I6), RTX 5070, 1280×720 window (853×480 render, DLSS-RR Quality), release, no layers.

| Item | Value |
|---|---|
| ReSTIR DI ABBA win (I6, gallery / courtyard) | DI sum **−36.9% (−0.138 ms)** / **−40.3% (−0.366 ms)**; GPU total **−4.1% (−0.179 ms)** / **−10.1% (−0.509 ms)**; O8 `initial` −25.6% / −20.0%, `shade` −71.5% / −54.0%, `temporal` −12.3% / −30.8% (I6) |
| GPU total, ReSTIR GI mode (gallery / courtyard, matched full boost) | **2.99 / 3.61 ms** (post-I5 baseline before I6's −0.18 / −0.51 ms DI win; I6 ABBA at lower P-state clock read `4.38 → 4.20 ms` / `5.03 → 4.52 ms`) |
| GPU total, NRC + ReSTIR GI / NRC mode (gallery / courtyard) | **3.93 / 4.72 ms** (`NRC + ReSTIR GI`), **3.06 / 3.65 ms** (`NRC`; I4) |
| Bistro (76K triangle lights) | **`Power_RIS` default (I6 F12):** `presample` **0.436 → 0.032 ms (−92.7%)**, `Ray start wait` **0.577 → 0.092 ms (−84.1%)**, `Lit` stability `0.48% / 2.05% / 0.31%` vs `0.55% / 2.19% / 0.31%` (`ReGIR_RIS`) |
| Active CPU frame (`drawFrame − waits`) | **0.50 ms** Sponza (`FrameData::write` 0.001 ms), **0.56 ms** Bistro (`0.060 ms`), **1.01 ms** Field 64 (`0.444 ms`; I4) |
| Shipped ReSTIR DI convergence (3 shots) | **3/3 pass at shipped defaults** (`LightsPunctualLamp +0.084% / p90 1.84%`, `Sponza_terminator −0.000% / p90 0.03% / 0 leak`, `EmissiveStrengthTest +0.077% / p90 2.13%`; I6) |
| Shipped ReSTIR GI bias vs one bounce, gallery / courtyard | **+1.09% / −1.21%** (seed 0; 5-seed spread `+0.67…+1.09%` / `−1.36…−0.90%`, tile p90 23.6% / 27.7%; **gated ±2.0% / 35% p90**; O2 secondary DI improved to `+0.90% / −0.92%` after I6) |
| NRC mode bias, gallery / courtyard | +11.3% / +9.4% (I2, 2026-11-21; was +10.5% / +8.3%) |
| `Lit` stability, 480 static frames (temporal std / flicker of lit px / largest 8-frame swing) | shipped: gallery **5.74% / 39.3% / 0.81%**, courtyard **0.99% / 8.60% / 0.06%** (I6; I5 was 5.55% / 38.9% / 1.09%, 1.28% / 7.98% / 0.13%); NVIDIA GI + I6 DI: 7.96% / 42.4% / 1.28%, 1.15% / 14.0% / 0.12% |
| Courtyard sun move, frames until `Indirect radiance` settles | shipped 56 (+22.6% at 16f; toggle **8**), shipped + age limit 16, NVIDIA 24, converged 32 (I5/I6) |
| GI check | **10 of 10** gated phases pass (8 converged/floor + 2 shipped ReSTIR GI) |

## References

- `reference/RTXDI` (sample `fdbd9b3`), `RTXDI-Library` `b67f61c`: presets, `ShadeSecondarySurfaces.hlsl`, `GI/*.hlsl`, `Doc/RestirGI.md`, `ChangeLog.md`.
- `reference/RTXGI` (2.9.0): `Samples/IndirectDiffuse`, NRC SDK 0.16 `Include/NrcCommon.h`, `Docs/NrcGuide.md`.
- `reference/RTXMG-main`, `reference/vk_lod_clusters`, `reference/vk_partitioned_tlas-main`: Mega Geometry (I3, I10).
- `reference/vk_denoise_dlssrr`: DLSS-RR matrix convention (I2 F17).
- Nsight Graphics GPU Trace for I4; Tracy for the CPU frame.
