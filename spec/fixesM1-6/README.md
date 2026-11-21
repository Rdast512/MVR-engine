# M8 · Optimization and fixes

**Status** — Milestone planned on 2026-11-20; steps re-planned on 2026-11-21 as I1–I12 in execution order, covering every open finding of the three review documents. I1 ([i1_check_runners.md](./i1_check_runners.md)), I2 ([i2_shading_correctness.md](./i2_shading_correctness.md)), and I3 ([i3_mega_geometry_fixes.md](./i3_mega_geometry_fixes.md)) built 2026-11-21; next is I4.\
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
| I4 · Frame profile | `i4_frame_profile.md` | F4, F7, F8, F9 (RTX 5070 part), F11, F12, C-F1-5 | I2, I3 |
| I5 · ReSTIR GI tuning | `i5_restir_gi_tuning.md` | O11 follow-ups, age limit (§2.5), ray-traced spatial cost (§2.5), F3 (GI) | I1, I2, I4 |
| I6 · ReSTIR DI tuning | `i6_restir_di_tuning.md` | F5, F6, O8, O10, permutation (§2.7), F3 (DI), F12 (mode choice) | I1, I2, I4 |
| I7 · NRC bias | `i7_nrc_bias.md` | Q7, Q1, Q2 (ND5), Q3 (ND1), Q4, Q5, Q6 (ND6) | I1, I2 |
| I8 · NRC cost | `i8_nrc_cost.md` | NP1 (ND7), NP2, NP4, NP5 | I7 |
| I9 · RTXDI surface traffic | `i9_rtxdi_surface_traffic.md` | O4 | I4 |
| I10 · Mega Geometry perf | `i10_mega_geometry_perf.md` | MG-P1–MG-P7 | I3, I4 |
| I11 · Floor tier | `i11_floor_tier.md` | F9 (2060 Super part), O7, NP3, DLSS-RR mode per tier | I4 (needs the 2060 Super) |
| I12 · Many-light and glossy re-check | `i12_many_light_recheck.md` | O2, O6, O8 (exit), O9, I5 / I6 decisions on Bistro | I5, I6 |

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

MG-P1–MG-P7, in I4's ranked order:

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
- I5's and I6's decisions re-run on a many-light pose.

**Done when:** each switch is decided with its numbers on Bistro, and the shipped sets hold on both scenes.

## Order

```
I1 → I2 → I4 → I5, I6, I9, I10   (independent; each ABBA-measured against I4's table)
I3 → I4                          (I3 can run alongside I1 and I2)
I1 + I2 → I7 → I8
I4 → I11                         (needs the 2060 Super)
I5 + I6 → I12
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

M8 adds no frame pass; I1 adds runners only.

M8 is done when:

- the shipped ReSTIR GI phase is gated in the GI check on both poses, or its remaining bias is a choice documented with the light-change runner's numbers;
- the NRC phases are gated, or C-G5-3 has a known cause and stays a logged limitation of the comparison mode;
- the stability runner's `Lit` numbers on both poses are no worse than I1's baseline;
- every lever in I4's ranked list has either landed with its gain or been closed with its number, and the per-scope table is recorded before and after on the RTX 5070 and the 2060 Super (I11);
- every runner passes, the GI check passes on every gated phase, and validation and GPU-AV are clean.

## Baseline

2026-11-20, RTX 5070, 1280×720 window (853×480 render, DLSS-RR Quality), release, no layers. I1 and I4 re-record it.

| Item | Value |
|---|---|
| GPU total, ReSTIR GI mode | 2.72–2.76 ms (DLSS-RR 1.10, `Indirect` 0.52, GI shade 0.41, GI temporal 0.11, DI 0.34) |
| GPU total, NRC + ReSTIR GI mode | 3.30 ms (2026-11-19) |
| Shipped ReSTIR GI bias vs one bounce, gallery / courtyard | −0.21% / −1.48% (tile p90 21.7% / 29.6%; I2, 2026-11-21; was −0.75% / −0.86% after O11, +8.1% / +1.7% before O11) |
| NRC mode bias, gallery / courtyard | +11.3% / +9.4% (I2, 2026-11-21; was +10.5% / +8.3%) |
| `Lit` stability, 480 static frames (temporal std / flicker of lit px / largest 8-frame swing) | shipped: gallery 5.80% / 46.0% / 0.83%, courtyard 1.30% / 8.3% / 0.15%; NVIDIA: 7.56% / 41.8% / 1.06%, 1.41% / 13.7% / 0.15% (I2, 2026-11-21; I1 shipped was 6.14% / 48.7% / 1.16% gallery, 24.65% / 91.0% / 22.57% courtyard with F20) |
| Courtyard sun move, frames until `Indirect radiance` settles | shipped 56, shipped + age limit 16, NVIDIA 24, converged 32 (I2, 2026-11-21; I1 shipped was 400 with F20) |
| GI check | 8 of 8 gated phases pass (I2: converged 1-bounce −0.11% gallery / +0.05% courtyard, traced +0.50% / +0.80%; I1 verified 5 seeds) |

## References

- `reference/RTXDI` (sample `fdbd9b3`), `RTXDI-Library` `b67f61c`: presets, `ShadeSecondarySurfaces.hlsl`, `GI/*.hlsl`, `Doc/RestirGI.md`, `ChangeLog.md`.
- `reference/RTXGI` (2.9.0): `Samples/IndirectDiffuse`, NRC SDK 0.16 `Include/NrcCommon.h`, `Docs/NrcGuide.md`.
- `reference/RTXMG-main`, `reference/vk_lod_clusters`, `reference/vk_partitioned_tlas-main`: Mega Geometry (I3, I10).
- `reference/vk_denoise_dlssrr`: DLSS-RR matrix convention (I2 F17).
- Nsight Graphics GPU Trace for I4; Tracy for the CPU frame.
