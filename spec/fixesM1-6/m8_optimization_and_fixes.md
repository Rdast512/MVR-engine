# Feature: M8 optimization and fixes — RTXDI and NRC sample reviews, pipeline findings

**Date:** 2026-11-20 · **Status:** review; O1 done in a changed form (§2.5), O2 and O6 built behind flags (default off) and measured (§2.6); O2's bias traced to the shipped DI set, which now ships `Ray traced` spatial correction, with a bridge fix for permutation sampling (§2.7, 2026-11-21); O3 measured without gain and reverted (§2.8, 2026-11-21); O9 built, on by default, neutral on Sponza (§2.9, 2026-11-21); auto-exposure boiling fixed (F10, [auto_exposure_dark_boiling.md](./auto_exposure_dark_boiling.md)) · **Milestone:** [M8](./README.md) (engine spec §16.2)
**Why:** ReSTIR GI mode became the default indirect mode on 2026-11-20 (engine spec §11.7), because NRC reads ~10% bright (§3.3, was C-G5-3). The 1.0 goal is the full RTXDI stack with NRC as a comparison switch (engine spec §1.1), with default RTXDI / DLSS-RR / RTXGI settings tuned for cost and quality against NVIDIA's sample defaults (was C-M4-2; per-variant courtyard measurements in [docs/settings.md](../../docs/settings.md)). This document merges two reviews (RTXDI sample + 3.1/3.2 library, RTXGI NRC sample) with the frame, stability, and perf findings that M8 turns into steps.

**Sources**

- RTXDI:
  - `RTXDI-Library` at `b67f61c` (3.2, 2026-11-09). This is the engine's pin and upstream HEAD.
  - `RTXDI` sample at `fdbd9b3`, upstream HEAD.
  - The commit messages are only "3.1 Update" / "3.2 Update". Their descriptions are in `reference/RTXDI/ChangeLog.md`.
  - Sample code: `Samples/FullSample/Source/UserInterface.cpp` (presets), `Shaders/LightingPasses/{BrdfRayTracing,ShadeSecondarySurfaces}.hlsl`, `GI/*.hlsl` and `RtxdiApplicationBridge/RAB_Surface.hlsli`; documentation in `Doc/RestirGI.md`.
- NRC:
  - NRC SDK 0.16 (`7de30e4`, 2026-10-03) and RTXGI 2.9.0 (`04372d7`, 2026-11-12). Both are upstream HEAD and both are what G3 was built against on 2026-11-17.
  - The commit messages are only "Update to v…". Their descriptions are in `reference/RTXGI/Changelog.md`.
  - Sample: `Samples/IndirectDiffuse/{Pathtracer.cpp,UI.h,Shaders/NrcIndirectDiffuse.hlsli}`.
  - SDK: `Include/NrcCommon.h` (`FrameSettings` comments), `Include/NrcStructures.h` (`NrcResolveMode`), and `reference/RTXGI/Docs/NrcGuide.md`.
  - Engine: [g3_nrc.md](../m4/g3_nrc.md), `src/render/vk_nrc.cpp` and `shaders/passes/{nrc_update,indirect,nrc_resolve}.slang`.
- Upstream HEADs were checked with `git ls-remote` on 2026-11-20.

## 1. The frame today

RTX 5070, 1280×720 window (853×480 render, DLSS-RR Quality), release, no layers. GPU timer means.

| Scope | ReSTIR GI mode (default) | NRC + ReSTIR GI mode | Note |
|---|---|---|---|
| Cull + raster + resolve + guides | ~0.27 | ~0.27 | M1 table, engine spec §2.3, measured at 1280×720 render before DLSS-RR; less now |
| PTLAS update | ~0 (still frame) | ~0 | at least ~0.11 ms once anything moves (F7) |
| ReSTIR DI (initial / temporal / shade) | 0.34 (0.18 / 0.07 / 0.09) | 0.34 | initial includes any TLAS wait (F4) |
| `Indirect` | 0.52 | 0.66 | the NRC query adds ~0.1 ms |
| ReSTIR GI temporal / shade | 0.11 / 0.41 | 0.11 / 0.27 | the shade includes ray-traced spatial correction (+0.15 ms, §2.5) |
| NRC update / train / resolve | — | 0.02 / 0.42 / 0.03 | paused by default |
| DLSS-RR | 1.10 | 1.10 | **40% of the frame** |
| GPU total | 2.72–2.76 | 3.30 | 2026-11-19 (NRC), 2026-11-20 (ReSTIR GI ABBA) |

The frame sits far under the 16.6 ms budget on Sponza. M8's perf work is about two things: the per-pixel cost that scales to Bistro and the 2060 Super, and the bias and stability of the default image. DLSS-RR is the largest single scope, and no engine setting other than its mode and preset changes it.

## 2. RTXDI

### 2.1 Library commits since 3.0

| Commit | Change | Engine |
|---|---|---|
| `a14e079` 2026-03-10 ReSTIR PT; `d28e20f` 2026-04-29 | ReSTIR PT runtime; `RAB_RayPayloadGetCommittedHitT` fix in the PT path tracer context | not used (ReSTIR PT is engine spec §15, later) |
| `f12037f` 3.1 (2026-09-03) | RTX Kit renames; ReSTIR PT DLSS-RR profile (§2.2); compatibility-guided neighbor selection (CGNS) for PT foliage; glass-behind-foliage fix in the sample | renames taken with the 3.2 update; the rest is PT-only |
| `b67f61c` 3.2 (2026-11-09) | DI stratified low-discrepancy spatial sampling ("all pixels sample the same quadrants at the same time, improving cache coherence and performance"); DI temporal fallback sampling; fused DI pass removed; fixes for excessive DI weights (Raytraced temporal + visibility shortcut + reused final visibility) and for a light load from an invalid pairwise reservoir | **all adopted** on 2026-11-15 ([rtxdi_3_2_update.md](../features/rtxdi_3_2_update.md)): stratified table and even counts, fallback on, separate passes. The fixes are library code |
| — | ReSTIR GI | **no functional change** in 3.1 or 3.2: renames, comment fixes, and a simpler Box–Muller in a random helper. GI spatial reuse still walks the neighbor table linearly (`neighborSampleStartIdx + i`); stratified sampling is DI-only |

So the "lot of optimizations" in 3.1 and 3.2 come down to two items:

- DI's stratified sampling, which the engine has taken.
- ReSTIR PT's DLSS-RR profile. It is PT-only, but its settings could carry over to GI; §2.5 measured that they do not.

### 2.2 3.1's DLSS-RR profile (ReSTIR PT)

`GetDefaultReSTIRPT*Params(dlssRREnabled)` (`Source/ReSTIRPT.cpp`). The sample applies it on entering DLSS-RR (`ApplyDLSSRRPreset`):

| Setting | Without RR | With RR | GI counterpart in the engine |
|---|---|---|---|
| temporal `maxHistoryLength` | 8 | 20 | `GiSettings::maxHistoryLength` (8, library default) |
| age-based rejection | on | **off** | `maxReservoirAge` 30 (§2.5, was C-G5-1) |
| boiling filter | on (0.2) | **off**, replaced by a firefly replacement filter | GI boiling filter on (0.2) |
| decorrelation | none | stagnancy-driven: final shading falls back to the initial sample where reservoirs are duplicated | none; F6 suspects RR prefers independent samples |
| duplication-based history reduction | off | on | none |

NVIDIA's reason (ChangeLog 3.1): DLSS-RR "prefers temporally independent samples".

### 2.3 Sample techniques against the engine

| Technique | Sample | Engine | Status |
|---|---|---|---|
| GI spatial reuse in the final shading pass | separate passes; `Doc/RestirGI.md` allows merging | `rtxdi_gi_shade.slang` does spatial + shade | has |
| GI temporal reuse in the initial-sample shader | allowed by `Doc/RestirGI.md` ("whichever works faster"); the sample keeps it separate | separate `rtxdi_gi_temporal.slang` (0.11 ms) | O3 measured, no gain (§2.8) |
| Fused spatiotemporal GI | `GI/FusedResampling.hlsl`; the doc calls it "likely not great for performance" | not used | skip |
| Per-pixel randomized `maxReservoirAge` (×0.5–1) | `FusedResampling.hlsl`, temporal pass | `rtxdi_gi_temporal.slang` | has |
| Final MIS against the initial sample | `GI/FinalShading.hlsl` (`kMISRoughness` 0.3) | `rtxdi_gi_shade.slang` | has |
| Secondary-surface DI resampling | `ShadeSecondarySurfaces.hlsl`: X₂ projected to the screen; if on screen, `RTXDI_DISpatialResampling` from the primary DI reservoirs there (Medium: 1 sample, normalized to 2 for stratified, radius 1 px, `Basic`; Ultra: 2, 4 px, `Raytraced`) | built 2026-11-20 behind `GiSettings::isSecondaryResamplingEnabled`, off | O2, §2.6 |
| Firefly clamp on the X₂ radiance | luminance clamped to 10 (`c_MaxIndirectRadiance`) before the GI reservoir | built 2026-11-20 behind `GiSettings::isFireflyClampEnabled`, off | O6, §2.6 |
| Secondary-hit roughness floor | `BrdfRayTracing.hlsl:240`: `ms.roughness = max(ms.roughness, minSecondaryRoughness)`, default `0.5` (`LightingPasses.cpp:49,572`); `Doc/RestirGI.md:47` recommends clamping secondary roughness to avoid sharp glossy caustics in non-directional GI reservoirs | `GiSettings::isSecondaryRoughnessFloorEnabled`, on in the shipped and NVIDIA sets (2026-11-21) | O9 built (§2.9) |
| DI final visibility reuse | `ReSTIRDI.cpp:98` default `reuseFinalVisibility = true` (4 frames, 16 px; `Fast` and `Medium`); `Unbiased` and `Ultra` set `false` (`UserInterface.cpp:260,286`) | `defaultSettings()` inherits `true` (`vk_rtxdi.cpp:346`, check at `rtxdi_shade.slang:75-89`); `convergedSettings()` sets `false` (`vk_rtxdi.cpp:365`) | candidate O10 |
| Delta / specular rays kept out of GI reservoirs | `isSpecularRay && isDeltaSurface` shaded directly | no delta materials (GGX only) | not needed |
| Trace and shade split by a compact secondary G-buffer | `BrdfRayTracing` → `SecondaryGBufferData` (~48 B) → `ShadeSecondarySurfaces` | one compute pass traces, decodes, samples the material and the light, and traces visibility (`indirect.slang`) | removed from the candidates on 2026-11-20 (was O5) |
| Neighbor surface loads | depth R32F + packed normals (uint oct) + packed geo normal + packed material, ~20 B, position rebuilt from depth (`RAB_Surface.hlsli`) | `Position` RGBA32F + `NormalRoughness` RGBA16F + `SpecularF0` + `GeoNormal` + `DiffuseAlbedo`, 36 B (`rtxdi_bridge.slang` `RAB_GetGBufferSurface`) | candidate O4 |
| Checkerboard | `Fast` preset: DI checkerboard `Black` | off for DI and GI (G2 traces every pixel, no tier switch yet; was C-G2-1) | floor-tier candidate O7 |
| `Fast` DI preset / BRDF candidate | `Fast`: Power_RIS 4 local, **0 BRDF samples**, bias correction `Off`, 1 spatial + 2 boost samples, boiling 0.2, final visibility reuse | `Medium` (1 BRDF candidate). Since M7 E2 (C-R32-3 resolved), `RAB_TraceRayForLocalLight` (`rtxdi_bridge.slang:558-589`) names emissive triangle lights, but when `triangleLights.lightCount == 0` and no environment light exists (Sponza), line 566 still traces a visibility ray whose `hitAnything` result is unused (`InitialSampling.hlsli:554`) | candidate O8 |
| Local light presampling (Power_RIS, ReGIR) | `Presampling/*.hlsl`; `Medium` uses ReGIR | the same since M7 F1 (2026-11-20): `ReGIR_RIS` default, `Power_RIS` and `Uniform` selectable | built ([m7/f1_light_presampling.md](../m7/f1_light_presampling.md)) |
| Stratified table shared with GI | one table, filled in the selected DI mode | the same. GI reads it linearly, which is valid (each entry is an offset), just not stratified | no action; a stratified GI walk needs a library change |

### 2.4 Candidates

Each candidate lands only when three conditions hold:

- the GI check (`ENGINE_GI_CHECK`) passes, with ReSTIR GI mode gated against the one-bounce ground truth;
- validation is clean;
- an ABBA bench shows the gain (±5% noise).

| # | Change | Expected | Result or plan | M8 step |
|---|---|---|---|---|
| O1 | GI settings for DLSS-RR, after 3.1's PT profile | stability under RR | **done 2026-11-20, changed:** the PT profile measured darker and flickered 3× more. The shipped GI default is NVIDIA's with ray-traced spatial bias correction (§2.5) | follow-up in I3 |
| O2 | Secondary-surface DI resampling at X₂ (sample `Ultra`: 2 samples, 4 px, `Ray traced`) | X₂'s direct light inherits the primary DI reservoirs' light choice | **built, off:** its −38.8% courtyard bias came from the shipped DI reservoirs (`Basic` spatial + permutation). Fixed in DI, it reads +0.62% (shipped +1.90%) for +0.058 ms (§2.7) | I9 (many-light pose) |
| O3 | GI temporal reuse inside the indirect pass | drops one dispatch and one reservoir write + read (up to the 0.11 ms of the temporal pass) | **measured 2026-11-21, reverted:** neutral within noise; the temporal work moves into `Indirect` at the same cost (§2.8) | — |
| O4 | Compact surface loads for RTXDI: one packed guide (oct normals, roughness, f0 / albedo) and position from depth | 36 → ~20 B per neighbor surface in DI temporal / shade and GI temporal / shade | `Position` is a guide by F2's decision (both frames); rebuilding it needs exact previous view matrices with jitter | I7 |
| O6 | Firefly clamp on the X₂ radiance at scene luminance 10 (the sample's `c_MaxIndirectRadiance`; display luminance 10 / exposure until the auto-exposure fix's F4) | stability under DLSS-RR | **built, off:** no measurable effect on Sponza (§2.6) | I9 (many-light pose) |
| O7 | Checkerboard GI (and DI) on the floor tier | roughly halves `Indirect` and the reuse passes there | quality under DLSS-RR; floor tier only — enable `CheckerboardMode` in the GI context and trace the active field only if `Indirect` exceeds budget on the 2060 Super (was C-G2-1) | I8 |
| O8 | Skip DI BRDF ray when no emissive triangle or environment light is present (`rtxdi_bridge.slang:564-567`), or 0 BRDF samples on `Fast` | ~0.04 ms on non-emissive scenes like Sponza | M7 E2 resolved C-R32-3 for emissive scenes, but `triangleLights.lightCount == 0` still traces an unused visibility ray when `environmentLightParams.lightPresent == 0` | I4 |
| O9 | Secondary-hit roughness floor at X₂ (`max(roughness, 0.5)`, `BrdfRayTracing.hlsl:240`, `LightingPasses.cpp:49`) | reduces glossy-bounce fireflies and view-dependent noise in ReSTIR GI reservoirs (`Doc/RestirGI.md:47`) | **built 2026-11-21, on by default:** neutral on Sponza within the GI check's realization noise; courtyard per-frame RMS 113.2% → 109.6% (§2.9) | I3 (glossy scene) |
| O10 | Fresh DI final visibility (`isFinalVisibilityReused = false`, as `convergedSettings()` and RTXDI `Unbiased`/`Ultra`) | removes ~16 px shadow-edge light leak, thin-shadow fading (Sponza poles) and visibility-reuse flicker (was C-R32-4; `rtxdi_shade.slang:75-89`) | turning it off with permutation off and Pairwise spatial gave 0.9% flickering px vs 7.5% for NVIDIA defaults on courtyard; costs ~0.7 extra shadow rays/px (`Doc/NoiseAndBias.md:63` reports reuse cuts final visibility rays to ~30%); measure cost vs stability in I4 | I4 |

### 2.5 O1 result (2026-11-20)

**Method.**

- **GI check:** two measured phases per pose, `ReSTIR GI mode (shipped defaults)` and `ReSTIR GI mode (NVIDIA defaults)`, against the one-bounce ground truth. `RtxdiContext::defaultGiSettings` is now the shipped set and `nvidiaGiSettings` the library's.
- **Stability probe:** temporary and reverted, on the method of docs/settings.md §3.4.
  - Courtyard pose, static camera, 300 warm-up + 120 captured frames.
  - Reads the `Lit` temporal std (DLSS-RR output) and the share of lit pixels whose per-pixel deviation exceeds 10% of their mean ("flickering px").
  - Reads the `Indirect radiance` mean against `convergedGiSettings` as a quick bias reading.
- **Cost:** ABBA bench (6 runs × 3000 frames, library swap), 1280×720 window (853×480 render, DLSS-RR Quality), release, no layers, RTX 5070.

**Bias and stability** (ReSTIR GI mode):

| GI settings | Bias vs one bounce, gallery / courtyard (tile p90) | `Lit` temporal std / flickering px (courtyard) |
|---|---|---|
| Converged (gated) | +0.31% / +1.51% (14.9% / 10.7%) | 1.65% / 6.4% |
| NVIDIA (library defaults, shipped until 2026-11-20) | **−27.2% / −9.0%** (69.7% / 52.6%) | 1.53% / 4.4% |
| 3.1's ReSTIR PT DLSS-RR profile (history 20, no age limit, no boiling filter) | −36.0% / −12.3% | **6.33% / 14.6%** |
| NVIDIA + ray-traced temporal and spatial correction | +7.9% / +1.6% | 1.57% / 4.7% |
| **NVIDIA + ray-traced spatial correction (shipped)** | **+8.1% / +1.7%** (33.0% / 17.4%) | 1.57% / 5.0% |
| NVIDIA + ray-traced temporal correction only | about −28% / −18% (probe mean) | 1.58% / 4.9% |
| shipped + no age limit | about 0% / −0.7% (probe mean) | 1.57% / 6.0% |

- **The darkening comes from the spatial pass's `Basic` correction** (was C-G5-2): without reconnection rays the MIS denominator counts neighbors from which the selected sample is hidden, and final visibility then removes it (in NRC + ReSTIR GI under NVIDIA defaults, the same darkening read −15.9% gallery / −0.7% courtyard). Ray-traced correction in the temporal pass alone leaves the darkening; in the spatial pass alone it removes it.
- **3.1's PT profile does not carry over to GI.** With all three guards gone (boiling filter, age limit, short history), fireflies pass into DLSS-RR: the `Lit` mean rose 7% and the flickering pixels 3×. Restoring any one of the three restores stability (std 1.58–1.63%).
- **The remaining +8% in the gallery is the age limit** (was C-G5-1): `RTXDI_GITemporalResampling` drops the temporal reservoir when its selected sample exceeds `maxReservoirAge` (30 by default, per-pixel scaled by [0.5, 1)), which keeps reservoirs that a bright candidate won recently. On traced gallery paths, temporal reuse alone read +8.5% at age 30 vs −0.6% unlimited (`kGiUnlimitedSampleAge`, used by `convergedGiSettings`); temporal + spatial read +5.6% at 30 vs +0.5% unlimited. Without the limit the probe reads as converged with the same `Lit` stability. The limit stays until a lighting-change check measures settling time (I1, I3), or a sample-independent staleness test (re-validating the reused radiance) replaces it.

**Cost** (ABBA, mean of 3 runs each):

- ReSTIR GI shade: 0.26–0.27 → 0.41–0.42 ms.
- Temporal pass and `Indirect`: unchanged.
- GPU total: 2.54–2.60 → 2.72–2.76 ms (+0.17 ms).
- Ray-traced correction in both passes cost another 0.06 ms in the temporal pass, for no bias change.

### 2.6 O2 and O6 result (2026-11-20)

**Build.**

- `path_shading.slang` `directLightAt` is split into `sampleLightsAt` and `shadeLightReservoir`; the behavior is unchanged.
- `indirect.slang` `secondaryDirectLight` does the following:
  - projects X₂ with `camera.viewProj` and the jitter offset;
  - runs `RTXDI_DISpatialResampling` on the shade-input DI reservoirs (2 samples, `Basic`, material test off; measured below at radius 1 px, later raised to 4 px in `indirect.slang:77` because `int2(snormOffset * 1.0)` truncated all 8,192 offsets to `(0, 0)`);
  - re-evaluates the chosen light.
- The firefly clamp limited X₂'s radiance to display luminance 10 / exposure, reading the 1×1 exposure image (`GpuTargets.exposureStorage`). Since the auto-exposure fix's F4 ([auto_exposure_dark_boiling.md](./auto_exposure_dark_boiling.md)), it clamps at scene luminance 10 (`kMaxIndirectRadiance`) and no longer reads the exposure image. The numbers below ran at the default manual exposure (scale 1), where the two clamps are identical.
- Flags: `GpuRtxdiConstants::giFlags`, set from `GiSettings::isSecondaryResamplingEnabled` / `isFireflyClampEnabled`; both are off.

**Measured** with the stability probe (as in §2.5, shipped settings + flag) and with GI-check phases per flag:

| Variant | `Indirect radiance` mean, courtyard / gallery | `Lit` temporal std / flickering px, courtyard |
|---|---|---|
| shipped | 0.00465 / 0.00019 | 1.57% / 5.0% |
| + O2 | **0.00353 (−24%)** / 0.00019 | 1.63% / 5.5% |
| + O6 | 0.00463 / 0.00019 | 1.55% / 4.9% |
| + O2 + O6 | 0.00352 / 0.00019 | 1.63% / 5.6% |

GI check, ReSTIR GI mode against the one-bounce ground truth (bias, 32×32 tile p90 / max):

| Variant | Gallery | Courtyard |
|---|---|---|
| shipped | +8.07% (33.0% / 792%) | +1.48% (16.2% / 74%) |
| + O2 | +7.67% (32.5% / 111%) | **−22.93%** (33.7% / 256%) |
| + O6 | +8.39% (34.5% / 133%) | +1.15% (18.9% / 215%) |
| + O2 + O6 | +7.60% (30.2% / 1373%) | −23.20% (32.6% / 106%) |

The max tile is a single-tile outlier that moves between runs, so it reads as noise. The run passed all 8 gated phases: the courtyard's converged gate read +0.63%, which confirms F1.

- **O2 is biased with the shipped DI settings.** The DI reservoirs carry the visibility shortcut: shadowed samples are dropped but their M is kept. `Basic` resampling at X₂ then counts neighbors whose sample is shadowed from X₂. With one light there is no light choice to inherit, so O2 can only lose.
- **O6 changes nothing on Sponza.** The sun's single bounce produces no X₂ radiance above the clamp.
- Both stay off and return in I9 on a many-light pose (M7 E2/F1 emissives + presampling are built; Bistro materials are spec-gloss, C-M7-1).

### 2.7 O2's bias was the DI reservoirs' (2026-11-21)

§2.6's reading of O2's bias was wrong. After M7 F1 the courtyard O2 phase read **−38.8%**. With ray-traced correction at X₂ (the RTXDI sample's `Ultra` / `Unbiased` presets) it still read −37.5%. Bisected with temporary shader variants, each one a GI-check run:

| X₂ resampling variant | Courtyard O2 bias | Reading |
|---|---|---|
| 0 neighbor samples (plumbing) | +1.74% | the projection and re-evaluation are neutral (shipped without O2: +1.90%) |
| neighbor's light, X₂'s own weight | +1.74% | light choice and X₂ visibility are fine |
| `Off` correction (1/M) | −38.8% | the neighbors' stored weights average ~0.6× X₂'s own |
| only where X₂ is the surface on screen | −37.1% | not off-surface neighbors |
| temporal-output reservoirs (`Ray traced` / `Basic`) | −35.1% / −36.5% | not the shade pass's spatial step alone |
| shading-input reservoirs, **converged DI settings** | **+0.47%** | the shipped DI reservoirs are biased |

**The shipped DI was biased.** With NVIDIA's `Medium` set (the shipped DI defaults until this change), the guide capture's convergence checks read lamp −11.3% / p90 27.3%, **Sponza_terminator −53.6%** / 76.4%, and EmissiveStrengthTest −4.2% / 40.3%. Flipping one setting at a time to its converged value:

- `Basic` spatial correction causes most of it. `Ray traced` gives −7.3% / −20.6% / −0.26%; no per-frame spatial gives −6.0% / −17.5% / −0.51%. As in GI (C-G5-2), `Basic` counts neighbors whose selected sample is hidden from them; the visibility shortcut has already dropped those samples, but their M stays.
- Permutation sampling causes the rest: spatial off plus permutation off reads −0.04% / −0.00% / +0.19%.
- No other single setting moved Sponza by more than 0.7 pp: BRDF candidate, ReGIR, visibility shortcut, fallback, boiling filter, final visibility reuse.

**Bridge bug under permutation.** `RAB_GetTemporalConservativeVisibility` traced from the current surface. Under permutation sampling the temporal surface is another pixel, so the temporal correction ray tested the wrong domain. NVIDIA's sample has the same fallback without `enablePrevTLAS`, which its UI forces off when animations are off. It now traces from `previousSurface` through this frame's TLAS: exact on static geometry, the same staleness as before on moving geometry. NVIDIA's `Medium` then reads −5.4% / −37.9% / −4.1%. The converged gate is unchanged (permutation off: +0.030% / −0.000% / +0.063%, 3 of 3). The GI bridge's `RAB_GetTemporalConservativeVisibility` has the same shape but no shipped GI set reaches it (shipped GI temporal correction is `Basic`; converged GI turns permutation off).

**Shipped DI set** (`RtxdiContext::defaultSettings`): NVIDIA's `Medium` (`nvidiaSettings`, the `NVIDIA defaults` button) with `Ray traced` spatial correction:

- Convergence checks read lamp −2.2% / p90 12.2%, Sponza_terminator +3.3% / 27.4%, EmissiveStrengthTest −0.03% / 5.5%. The remainder is permutation sampling. It stays for now: docs/settings.md §3.4 measured permutation off with visibility reuse off as the most stable set under DLSS-RR, so that decision belongs with the stability runner (I1, I4).
- Cost (ABBA, startup gallery, 3 runs each): `ReSTIR DI shade` 0.086 → 0.089 ms, GPU total 2.78 → 2.79 ms. The gallery is mostly in shadow; recheck the cost on a sunlit pose in I4.
- Stability under DLSS-RR is unmeasured: there is no stability runner yet (F2).

**O2 on the shipped DI set** (GI check, bias / 32×32 tile p90):

| Variant | Gallery | Courtyard |
|---|---|---|
| shipped | +8.01% / 26.2% | +1.90% / 17.8% |
| + O2 | +7.35% / 25.8% | **+0.62%** / 18.3% |
| + O6 | +7.69% / 27.3% | +1.52% / 16.9% |
| + O2 + O6 | +7.82% / 27.4% | +0.36% / 17.9% |

8 of 8 gated phases pass. Every phase without O2 repeats its pre-change number, except NRC's, which never repeat (C-T-5).

O2 costs +0.058 ms in `Indirect` (0.526 → 0.584 ms, ABBA 3 runs each; GPU total 2.79 → 2.85 ms). On Sponza it lowers the bias by about 1 pp at equal tile p90; with one light there is no light choice to inherit. **It stays off** until I9 measures it on a many-light pose. It now runs NVIDIA's `Ultra` parameters (2 samples, 4 px, `Ray traced`).

### 2.8 O3 result (2026-11-21)

**Build (reverted).**

- `indirect.slang` ran the temporal reuse and the boiling filter after storing the initial sample, through one include shared with `rtxdi_gi_temporal.slang`.
- The fused path took the initial sample through a pack/unpack round trip, so it resampled the same LogLuv radiance and octahedral normal the separate pass reads.
- The out-of-bounds early return was replaced by a bounds flag, because the filter synchronizes the 8×8 group.
- It was fused only when no NRC resolve follows the indirect pass (ReSTIR GI mode, ground-truth paths), since the resolve adds the cache term to the initial sample. In NRC + ReSTIR GI mode the separate pass ran.
- Validation (core + sync + best practices) was clean, including `ENGINE_INDIRECT_MODE=cycle` over 36 mode switches.

**Cost** (bench, startup gallery, 3000 frames per run, release, no layers, RTX 5070; mean over runs):

| Variant | Runs | `Indirect` + `ReSTIR GI temporal` | GPU total |
|---|---|---|---|
| separate passes (shipped) | 5 | 0.658 ms | 2.910 ms |
| fused | 5 | 0.674 ms | 2.938 ms |
| fused, without the boiling filter | 2 | 0.651 ms | 2.85 ms |
| fused shader, fusion off on the host | 2 | 0.646 ms | 2.81 ms |

- The first round put fused at +0.03 ms. The second round had fused level with separate passes (0.657 vs 0.662 ms), and every scope read faster in later runs, so the difference is run position, not the variant.
- Neither the filter's group barrier (third row) nor the extra code's register cost (fourth row) is measurable.
- The dispatch, barrier and reservoir read that fusion saves come to about 0.01 ms; the temporal work itself costs the same inside `Indirect`.

No gain, so O3 does not land (§2.4). Recheck only if the 2060 Super (F9) shows the separate pass's fixed cost.

### 2.9 O9 result (2026-11-21)

**Build.**

- `indirect.slang` shades X₂ with `max(roughness, 0.5)` (`kMinSecondaryRoughness`, the RTXDI sample's `minSecondaryRoughness`), for its direct light and, with ground-truth paths, for the bounce leaving X₂. `shadeRayHit` is unchanged.
- The NRC query at X₂ keeps the raw roughness, which is what the cache trains on (`nrc_update.slang`).
- Flag: `GiFlag::SecondaryRoughnessFloor`, set from `GiSettings::isSecondaryRoughnessFloorEnabled` only while GI reuse runs, so the reference and both ground truths stay unbiased.
- On in `nvidiaGiSettings` (the sample's default) and so in the shipped set; off in `convergedGiSettings`. UI: `Secondary roughness floor`.
- The GI check measures the shipped set without it as its last phase. Validation (core + sync + best practices, `ENGINE_INDIRECT_MODE=cycle`) is clean.

**Measured** (GI check, ReSTIR GI mode against one bounce; bias / 32×32 tile p90 / per-frame RMS error):

| Variant | Gallery | Courtyard |
|---|---|---|
| shipped (floor on) | +8.06% / 27.9% / 243.0% | +1.73% / 17.0% / 109.6% |
| shipped without the floor | +7.95% / 27.0% / 242.8% | +1.91% / 17.7% / 113.2% |

- 8 of 8 gated phases pass; they run the converged set, without the floor.
- The two phases differ by less than a realization's spread (up to ~0.5 pp on traced-path phases). Sponza's X₂ surfaces are almost all rougher than 0.5, so the floor neither biases nor stabilizes it measurably.
- It is not the cause of the GI boiling seen under DLSS-RR on Sponza. Its case is glossy X₂ (Bistro, spec-gloss materials, C-M7-1); measure it there in I3 once the stability runner exists (F2).

## 3. NRC

### 3.1 Upstream changes since G3

None: G3 used the current SDK and sample. RTXGI 2.9.0's changelog items that concern NRC, checked against the engine:

| 2.9.0 item | Engine |
|---|---|
| IndirectDiffuse sample (NRC for indirect diffuse, G-buffer start) | G3's reference |
| SER on Vulkan through `VK_NV_ray_tracing_invocation_reorder` | shelved: the engine's SER is the EXT path, blocked by Slang (C-G1-3) |
| "Fixed the order in which emissive radiance is factored into the path contribution" | matches: `nrc_update.slang` adds emission and direct light after `NrcUpdateOnHit`, as the sample does |
| Fixed ray origin offsetting, RIS light sampling, DLSS-RR reset | the engine has its own: `safeSpawnPoint` / `rasterRayOrigin` (ray-tracing feature spec §6.1), RTXDI's RIS, `FrameFlag::IndirectHistoryReset` (G7) |

### 3.2 Engine against the sample

**Matching (no action):**

- context settings: `maxPathVertices` 8, `learnIrradiance`, `smallestResolvableFeatureSize` 0.01, training dimensions from `ComputeIdealTrainingDimensions`;
- frame settings: 1 training iteration, `maxExpectedAverageRadianceValue` 1, heuristic thresholds 0.01, `proportionPrimarySegmentsToTrainOn` 0.02, `skipDeltaVertices`;
- the training path:
  - the G-buffer pixel is picked like `SelectSparseGBufferPixel`;
  - the primary is vertex 0;
  - `NrcUpdateOnHit` runs before the vertex's own light;
  - `TerminateAfterDirectLighting` is honored;
  - scatter uses the full lobe.

| # | Difference | Sample | Engine | Effect |
|---|---|---|---|---|
| ND1 | Query placement | the termination heuristic decides: the primary is vertex 0 and the path traces on until the spread reaches the threshold | heuristic **off** in the query pass's constants: always queries at X₂ (`indirect.slang` `nrcQueryContext`) | where the footprint at X₂ is still small (corners, contact), the engine reads the cache where NVIDIA's sample would trace further. G3 assumed "almost always X₂ anyway" at threshold 0.01; never measured |
| ND2 | Cached light | `includeDirectLighting` on (UI default) | off: X₂'s direct light comes from RTXDI's light sampling (engine spec §11.3) | design; the network learns the indirect part only, a dimmer and noisier target |
| ND3 | Query lobe at the primary | diffuse only (indirect diffuse signal) | full BRDF (ReSTIR GI samples) | design |
| ND4 | Query samples per pixel | 1–16 (UI) | 1; ReSTIR GI reuses instead | design |
| ND5 | Debug resolve modes | every `NrcResolveMode`: training bounce heat map, primary / secondary vertex training radiance (raw and smoothed), query index, direct cache view | only `NRC cache` (the direct cache view, through the custom resolve) | the engine cannot see the training targets, which is what §3.3 needs (Q2) |
| ND6 | Training root surface | `CreateRootSurface`: the material evaluated like a traced hit | the guides: 8-bit albedo and f0, RGBA16F normal and roughness | small attribute precision loss at vertex 0 only |
| ND7 | Submit around NRC | no host wait | C-G3-6 host wait before the next frame's ray commands (device loss otherwise), +11% frame time at 1280×720 | perf only (NP1) |
| ND8 | Miss radiance | sky color on a miss | none (no environment light yet) | none until an environment light exists (M7, C-F3-5) |

### 3.3 Quality: NRC ~10% bright bias (was C-G5-3)

Measured numbers:

- NRC + ReSTIR GI with `convergedGiSettings` reads +9.3% / +10.2% (gallery / courtyard), 32×32 tile p90 18% / 33%; NRC without reuse +10.0% (gallery).
- G7's NRC mode (cache samples without any reuse) reads +11.9% / +8.1%, so reuse does not cause it.
- ReSTIR GI on traced paths stays within 1% on both poses, with the same `directLightAt`, `sampleBrdf` and 8 bounces.
- Freezing training after 850 frames left the bias unchanged. `nrc_update.slang` already calls `NrcUpdateOnHit` before adding a vertex's own light (matching NVIDIA's sample), and primary AO on the cache term would only darken.

So the bias is in the cache term (custom resolve, engine spec §11.4). Each lead is a GI-check run of `NRC mode` with one setting changed. The setting comes from a temporary env var read in `applyPhase`, following the bisect pattern of [g5_restir_gi.md](../m4/g5_restir_gi.md) §3. A run takes about 1 min with the courtyard dropped and 1024 ground-truth frames.

| # | Lead | Why | Test | If confirmed |
|---|---|---|---|---|
| Q1 | **Self-training feedback** | `NrcCommon.h:282-287` / `NrcGuide.md:333`: "If there's an error in the path tracer that breaks energy conservation … the self training feedback can sometimes lead to the cache getting brighter and brighter." The engine runs the SDK defaults (`proportionUnbiased` 0.0625, `selfTrainingAttenuation` 1), and threshold 0.01 ends training paths early in cache lookups, so most training targets contain the cache's own output | `selfTrainingAttenuation` 0, then 0.5; `proportionUnbiased` 1 | diagnostic control only (not a shipping noise fix): a bias that falls with attenuation is an energy gain amplified by the loop, so look for it per vertex (Q2) rather than shipping attenuation |
| Q2 | **Training targets** | the SDK's `PrimaryVertexTrainingRadianceSmoothed` / `SecondaryVertexTrainingRadianceSmoothed` show what the network is trained toward | add a debug view that runs the SDK's `Resolve` in one of those modes into a scratch image (the custom resolve stays for the image); compare its mean with the ground truth's `Indirect radiance` on the gallery pose | if the target is already ~10% bright, the engine's path is wrong (vertex attributes, `directLightAt` at training vertices, emission order). If the target is correct and the query is bright, the query side is wrong (ND1, attributes at X₂) |
| Q3 | **Query placement (ND1)** | NVIDIA's termination heuristic (`NrcGuide.md` §Path termination heuristic) controls the bias-vs-noise trade-off by keeping cache lookups away from small footprints (corners, smooth reflections); `indirect.slang:135` forces `enableTerminationHeuristic = 0` | first count the queries the heuristic would place beyond X₂ (requires calling `NrcUpdateOnHit` at vertex 0 to initialize `primarySpreadRadius`, `NrcSetBrdfPdf(nrcPath, bounce.pdf)` before X₂, and continuing the bounce loop while `NrcUpdateOnHit` returns `Continue`, per `Nrc.hlsli:360-369` and `NrcIndirectDiffuse.hlsli:98,163,190-222`); then GI check and stability run with it on | ship the heuristic in the query pass; it costs extra rays only where the spread is still small |
| Q4 | **Training budget** | the sample trains 1 iteration per frame for frame rate; the SDK default is 4 (64 K records). `learningRate` 1e-2 (SDK default) | NRC mode bias and tile p90 at 1, 2 and 4 iterations; train cost per iteration | a quality knob per tier (engine spec P1: tiers scale NRC settings) |
| Q5 | **Radiance scale** | `maxExpectedAverageRadianceValue` should be "the average radiance in a bright scene"; the engine keeps the sample's 1 under a 10 lx sun | 0.25 / 1 / 4 | set it from the exposure meter's mean, once, not per frame (the SDK asks for a stable value) |
| Q6 | Root attributes (ND6) | 8-bit guides at vertex 0 | training root from the decoded vis-buffer hit, as the sample does | low priority; only vertex 0 differs |

**Order:**

1. Q1 and Q2 first: they are cheap, and they split the problem in two.
2. Then Q3.
3. Q4 and Q5 are tuning, after the bias is understood.

NRC returns as a default only when the GI check gates its phase (I5 exit). As the 1.0 comparison switch it needs at least a known cause.

### 3.4 Perf (when NRC runs)

Measured 2026-11-19 in NRC + ReSTIR GI mode (RTX 5070, 1280×720 window (853×480 render, DLSS-RR Quality), release, no layers):

- NRC update: 0.021 ms.
- `Indirect` (query): 0.66 ms, against 0.56 in ReSTIR GI mode, so the query adds ~0.1 ms.
- NRC train: 0.42 ms.
- NRC resolve: 0.03 ms.
- GPU total: 3.30 ms, against 2.72 ms in ReSTIR GI mode.

With NRC paused (the default since 2026-11-20), the cache costs nothing per frame.

| # | Lever | Gain | Cost / risk |
|---|---|---|---|
| NP1 | Remove the C-G3-6 host wait (driver fix, or an NVIDIA answer on `VK_NVX_binary_import` work and queue operations) | ~11% frame time at 1280×720 (2.87 → 3.20 ms) | not in the engine's hands; recheck per driver |
| NP2 | Train less on still frames: `trainTheCache` off after N still frames, on at any camera, light or scene change | up to the 0.42 ms train | stale cache after slow changes; needs a change detector |
| NP3 | Lower `usedTrainingDimensions` on the floor tier | train and update scale with records | quality (Q4 measures it) |
| NP4 | SER for the update and query passes, once C-G1-3 lifts | the sample's 2.9.0 path. The SER update pass measured slower in G3 (fixed trip count), so a variable-length loop is needed | Slang |
| NP5 | Query only where needed: skip `NrcUpdateOnHit` at X₂ when the throughput to the camera is tiny (the sample's `throughputThreshold` 0.01, query side) | fewer queries for train / resolve | small |

## 4. Pipeline and check findings

| # | Finding | Source | M8 step |
|---|---|---|---|
| F1 | GI-check realizations previously depended on the absolute frame index, so adding a phase shifted every later phase's noise (courtyard converged gate read +1.51% against 1.5%, then +0.63% once O2/O6 phases were appended). **Per-phase seeding fixed** (`RtxdiContext::frameIndexOverride` in `vk_rtxdi.cpp:526`, set per `(pose, phase, phaseFrame)` in `engine_gi_check.cpp:63-64`); multi-seed courtyard spread check remains for I1 (`nrc_update.slang:72` still reads `root.frameIndex`, though NRC is non-deterministic, C-T-5) | was C-G5-1 note | I1 |
| F2 | The stability probe (static-camera `Lit` / `Indirect` temporal std, flickering px, mean per settings set) exists only as a scratch patch. In each batch, the first process run reads abnormal (courtyard `Lit` mean `0.0127` vs `0.0260` on every later run, flickering `14.9%` vs `5.0%`; later runs repeat to 3 digits; candidates: shader/pipeline cache, GPU clocks, exposure adaptation), so batches discard a warm-up process. The probe's lit-pixel cutoff was not recorded; the auto-exposure probe (mean > 1e-4 linear, std > 10% of mean) reads courtyard `Lit` at 1.549% / 8.682%, so the 5.0% baseline is not comparable until I1 fixes the cutoff and finds or detects the first-run condition | §2.5, §2.6, was C-I1-1, [auto_exposure_dark_boiling.md](./auto_exposure_dark_boiling.md) §5 | I1 |
| F3 | No check measures how fast lighting settles after a change (age limit, §2.5) or how the screen-edge border converges when turning or dollying backwards. At screen edges, DI temporal back-projection lands off screen (starting from initial candidates + `disocclusionBoostSamples` + spatial neighbors), 3.2 temporal fallback sampling is unmeasured under motion, and DLSS-RR lacks history; I1 adds a scripted pan/backward-dolly runner to tune `disocclusionBoostSamples`, candidate counts, `maxHistoryLength`, or add a guard band if tuning is not enough | was C-G5-1, was C-M4-1 | I1, I3, I4 |
| F4 | `ReSTIR DI initial` GPU timer includes the wait for the async TLAS build (first span of the ray command buffer); add a separate wait span if needed | was C-F3-9 | I2 |
| F5 | The DI ray budget with one light: initial-candidate visibility, the ray-traced temporal correction ray, final visibility (reused up to 4 frames / 16 render px, O10), and a BRDF candidate whose visibility ray in `rtxdi_bridge.slang:566` is unused when there are no emissive triangles or environment light (O8). Final visibility reuse (`ReSTIRDI.cpp:98`, `rtxdi_shade.slang:75-89`) leaks light up to ~16 px past hard shadow edges and fades thin shadows (Sponza poles); turning it off + permutation off + Pairwise spatial gave 0.9% flickering px vs 7.5% for NVIDIA defaults on courtyard | C-R32-3 (resolved), was C-R32-4 | I4 |
| F6 | NVIDIA's DI `Medium` defaults (2 `Basic` spatial samples/frame + permutation sampling) fail the convergence check on `LightsPunctualLamp` (bias −1.34%, tile p90 26.4%, per-frame error 54.9%, frame-to-frame change 68.2%; per-frame spatial alone hits p90 49.6% `Basic` / 21.7% Pairwise; permutation alone hits 23.6% and raises temporal-only error 4.29%→43.4% and bias +0.07%→−1.94%; +0.11 ms on Sponza), so the check holds `convergedSettings` (disocclusion-only Pairwise spatial, permutation off: −0.13% / 2.87%). Conversely, DLSS-RR flickers more on noise-free input: bit-stable `convergedSettings` raw yields 2.8% RR variation (1.6% jitter off) and 4.7–42% lit pixels >10% flicker without per-frame spatial reuse, vs 0.9–1.1% RR variation when per-frame spatial reuse changes raw by 13–19%. A/B both through DLSS-RR on sunlit `Sponza` and a many-light scene (M7 / Bistro) | was C-F3-3, was C-R32-1, was C-R32-5 | I4 |
| F7 | A non-empty PTLAS update costs at least ~0.11 ms — slower than the full build it replaced when every instance moves: 0.12 vs 0.101 ms on Sponza, 0.18 vs 0.150 ms on the side-64 field (RTX 5070); still frames cost 0.015–0.019 ms. Runs on async compute and overlaps the frame; global partition, build flags and grid sizes did not lower it. Re-measure on Bistro and 2060 Super; try near/far mode (global partition for far partitions only) if it shows up in the frame | was C-C4-4 | I2 |
| F8 | Per-frame CPU work is O(entities): `FrameData::write` rewrites every node instance and object row every frame and computes each entity's model matrix twice (`writeNodeInstances` and entity loop). Exit: AU4 GPU scene-update pass fed by a CPU delta list (at latest with M9 loading) | was C-M0-5, audit AU4 | I2 |
| F9 | The async-compute overlap capture and every RTX 2060 Super number are still open (2060 Super not in dev machine): M0 baseline per-stage timings and culling image; full-screen DI cost (RTX 5070 3.2 baseline at 853×480: 0.15 initial + 0.07 temporal + 0.04 spatial+shade ms); and NRC support/timing (SDK can return `UnsupportedHardware`, floor tier 0.6 ms cache budget was set for SHaRC) | M0 / M2 status, was C-M0-1, was C-F3-7, was C-G3-2 | I2, I8 |
| F10 | Auto exposure boiled in shaded areas: a +16 EV cap, unmetered black pixels, symmetric adaptation, and the exposure fed back into DLSS-RR. **Fixed 2026-11-20** ([auto_exposure_dark_boiling.md](./auto_exposure_dark_boiling.md) F1–F4): −12 EV histogram floor with bin 0 counted, +6 EV cap, asymmetric adaptation, no exposure texture to DLSS-RR, and a scene-unit O6 clamp. The gallery's remaining noise is the sparse one-bounce signal (its F5) | C-H2-2, C-H2-3 (resolved) | I5 (F5) |
| F11 | Binned vis-buffer resolve costs +0.045 ms at the bench pose (RTX 5070, 853×480 render, Sponza startup): `Material classify` 0.018 ms + `Vis-buffer resolve` 0.103 ms vs 0.071–0.076 ms for the old single dispatch (empty sequence overhead 0.015 ms; Standard-only 0.104 ms, ~0.015 ms slower than old dispatch). Unmeasured candidates: overlap guard `binIds` load, tile-list load, append-order vs screen-order tiles. Levers: cull pass writes a bin per visible row, screen-order tiles, skip guard on single-bin tiles | was C-E1-4 | I2 |
| F12 | ReGIR costs ~10× `Power_RIS` (~0.7 ms vs ~0.09 ms on RTX 5070, `convergedSettings` guide-capture shots, single-frame non-overlapped): `rtxdi_presample_regir` (2,229 cells × 512 slots × 8 samples) reads 0.39–0.41 ms flat across 60 / 5,990 / 15,452 local lights, and `rtxdi_initial` reads 0.32–0.38 ms vs 0.04–0.07 ms for `Power_RIS` (tiles 0.021–0.027 ms) and 0.05–0.08 ms for `Uniform`. Before C-F1-3 it was 1.44 + 1.2–1.6 ms; groupshared parameter staging made the build slower (0.80 ms). Unmeasured candidates: onion lookup loops (`WorldPosToCellIndex` / `CellIndexToWorldPos` with dependent global loads vs sample's constant bank), incoherent cell reads in initial sampling, per-thread `rtxdiBridgeInit`. Exit: Nsight GPU trace of both passes + RTXDI FullSample timed on the same GPU at `Medium` | was C-F1-2 | I2, I4 |

## 5. Not applicable

- ReSTIR PT (CGNS, decorrelation, duplication maps, hybrid shift): engine spec §15, after 1.0.
- NRD settings, the RTXDI sample's TAA, the NRC sample's NRD path: the engine resolves with DLSS-RR only.
- Primary surface replacement (PSR): the engine has no mirror path.
- SHaRC: removed (C-G3-1); the sample's SHaRC path is not compared.
- Transmission and delta lobes: no transmissive or mirror material (`kMinAlpha`), so `isDeltaLobe` stays false.
