# Feature: M8 optimization and fixes — RTXDI and NRC sample reviews, pipeline findings

**Date:** 2026-11-20 · **Status:** review; O1 done in a changed form (§2.5), O2 and O6 built behind flags (default off) and measured (§2.6) · **Milestone:** [M8](../m8/README.md) (engine spec §16.2)
**Why:** ReSTIR GI mode became the default indirect mode on 2026-11-20 (engine spec §11.7), because NRC reads ~10% bright (considerations C-G5-3). The 1.0 goal is the full RTXDI stack with NRC as a comparison switch (engine spec §1.1). This document merges two reviews. The first compares the engine with the RTXDI sample and the 3.1 / 3.2 library changes. The second compares it with the RTXGI NRC sample. It also adds the frame and check findings of the same day. M8 turns all of it into steps.

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
| PTLAS update | ~0 (still frame) | ~0 | at least ~0.11 ms once anything moves (C-C4-4) |
| ReSTIR DI (initial / temporal / shade) | 0.34 (0.18 / 0.07 / 0.09) | 0.34 | initial includes any TLAS wait (C-F3-9) |
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
| `b67f61c` 3.2 (2026-11-09) | DI stratified low-discrepancy spatial sampling ("all pixels sample the same quadrants at the same time, improving cache coherence and performance"); DI temporal fallback sampling; fused DI pass removed; fixes for excessive DI weights (Raytraced temporal + visibility shortcut + reused final visibility) and for a light load from an invalid pairwise reservoir | **all adopted** on 2026-11-15 ([rtxdi_3_2_update.md](./rtxdi_3_2_update.md)): stratified table and even counts, fallback on, separate passes. The fixes are library code |
| — | ReSTIR GI | **no functional change** in 3.1 or 3.2: renames, comment fixes, and a simpler Box–Muller in a random helper. GI spatial reuse still walks the neighbor table linearly (`neighborSampleStartIdx + i`); stratified sampling is DI-only |

So the "lot of optimizations" in 3.1 and 3.2 come down to two items:

- DI's stratified sampling, which the engine has taken.
- ReSTIR PT's DLSS-RR profile. It is PT-only, but its settings could carry over to GI; §2.5 measured that they do not.

### 2.2 3.1's DLSS-RR profile (ReSTIR PT)

`GetDefaultReSTIRPT*Params(dlssRREnabled)` (`Source/ReSTIRPT.cpp`). The sample applies it on entering DLSS-RR (`ApplyDLSSRRPreset`):

| Setting | Without RR | With RR | GI counterpart in the engine |
|---|---|---|---|
| temporal `maxHistoryLength` | 8 | 20 | `GiSettings::maxHistoryLength` (8, library default) |
| age-based rejection | on | **off** | `maxReservoirAge` 30 (C-G5-1) |
| boiling filter | on (0.2) | **off**, replaced by a firefly replacement filter | GI boiling filter on (0.2) |
| decorrelation | none | stagnancy-driven: final shading falls back to the initial sample where reservoirs are duplicated | none; C-R32-5 suspects RR prefers independent samples |
| duplication-based history reduction | off | on | none |

NVIDIA's reason (ChangeLog 3.1): DLSS-RR "prefers temporally independent samples".

### 2.3 Sample techniques against the engine

| Technique | Sample | Engine | Status |
|---|---|---|---|
| GI spatial reuse in the final shading pass | separate passes; `Doc/RestirGI.md` allows merging | `rtxdi_gi_shade.slang` does spatial + shade | has |
| GI temporal reuse in the initial-sample shader | allowed by `Doc/RestirGI.md` ("whichever works faster"); the sample keeps it separate | separate `rtxdi_gi_temporal.slang` (0.11 ms) | candidate O3 |
| Fused spatiotemporal GI | `GI/FusedResampling.hlsl`; the doc calls it "likely not great for performance" | not used | skip |
| Per-pixel randomized `maxReservoirAge` (×0.5–1) | `FusedResampling.hlsl`, temporal pass | `rtxdi_gi_temporal.slang` | has |
| Final MIS against the initial sample | `GI/FinalShading.hlsl` (`kMISRoughness` 0.3) | `rtxdi_gi_shade.slang` | has |
| Secondary-surface DI resampling | `ShadeSecondarySurfaces.hlsl`: X₂ projected to the screen; if on screen, `RTXDI_DISpatialResampling` from the primary DI reservoirs there (Medium: 1 sample, radius 1 px, `Basic`; Ultra: 2, 4 px, `Raytraced`) | built 2026-11-20 behind `GiSettings::isSecondaryResamplingEnabled`, off | O2, §2.6 |
| Firefly clamp on the X₂ radiance | luminance clamped to 10 (`c_MaxIndirectRadiance`) before the GI reservoir | built 2026-11-20 behind `GiSettings::isFireflyClampEnabled`, off | O6, §2.6 |
| Delta / specular rays kept out of GI reservoirs | `isSpecularRay && isDeltaSurface` shaded directly | no delta materials (GGX only) | not needed |
| Trace and shade split by a compact secondary G-buffer | `BrdfRayTracing` → `SecondaryGBufferData` (~48 B) → `ShadeSecondarySurfaces` | one compute pass traces, decodes, samples the material and the light, and traces visibility (`indirect.slang`) | removed from the candidates on 2026-11-20 (was O5) |
| Neighbor surface loads | depth R32F + packed normals (uint oct) + packed geo normal + packed material, ~20 B, position rebuilt from depth (`RAB_Surface.hlsli`) | `Position` RGBA32F + `NormalRoughness` RGBA16F + `SpecularF0` + `GeoNormal` + `DiffuseAlbedo`, 36 B (`rtxdi_bridge.slang` `RAB_GetGBufferSurface`) | candidate O4 |
| Checkerboard | `Fast` preset: DI checkerboard `Black` | off for DI and GI (C-G2-1) | floor-tier candidate O7 |
| `Fast` DI preset | Power_RIS 4 local, **0 BRDF samples**, bias correction `Off`, 1 spatial + 2 boost samples, boiling 0.2, final visibility reuse | `Medium`, with 1 BRDF candidate that finds no light until M7 (C-R32-3, 0.04 ms) | candidate O8 |
| Local light presampling (Power_RIS, ReGIR) | `Presampling/*.hlsl`; `Medium` uses ReGIR | uniform until M7 | M7 (plan F1 `Power_RIS`) |
| Stratified table shared with GI | one table, filled in the selected DI mode | the same. GI reads it linearly, which is valid (each entry is an offset), just not stratified | no action; a stratified GI walk needs a library change |

### 2.4 Candidates

Each candidate lands only when three conditions hold:

- the GI check (`ENGINE_GI_CHECK`) passes, with ReSTIR GI mode gated against the one-bounce ground truth;
- validation is clean;
- an ABBA bench shows the gain (±5% noise).

| # | Change | Expected | Result or plan | M8 step |
|---|---|---|---|---|
| O1 | GI settings for DLSS-RR, after 3.1's PT profile | stability under RR | **done 2026-11-20, changed:** the PT profile measured darker and flickered 3× more. The shipped GI default is NVIDIA's with ray-traced spatial bias correction (§2.5) | follow-up in I3 |
| O2 | Secondary-surface DI resampling at X₂ (sample `Medium`) | X₂'s direct light inherits the primary DI reservoirs' light choice | **built, off:** −24% courtyard bias, no stability gain with one light (§2.6) | I9 (after M7) |
| O3 | GI temporal reuse inside the indirect pass | drops one dispatch and one reservoir write + read (up to the 0.11 ms of the temporal pass) | — | I7 |
| O4 | Compact surface loads for RTXDI: one packed guide (oct normals, roughness, f0 / albedo) and position from depth | 36 → ~20 B per neighbor surface in DI temporal / shade and GI temporal / shade | `Position` is a guide by F2's decision (both frames); rebuilding it needs exact previous view matrices with jitter | I7 |
| O6 | Firefly clamp on the X₂ radiance (display luminance 10 × exposure) | stability under DLSS-RR | **built, off:** no measurable effect on Sponza (§2.6) | I9 (after M7) |
| O7 | Checkerboard GI (and DI) on the floor tier | roughly halves `Indirect` and the reuse passes there | quality under DLSS-RR; floor tier only (C-G2-1) | I8 |
| O8 | No DI BRDF candidate until M7 | 0.04 ms | it stays for parity with NVIDIA's `Medium` (C-R32-3); the user's call | I4 |

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

- **The darkening comes from the spatial pass's `Basic` correction** (C-G5-2): it counts neighbors from which the selected sample is hidden. Ray-traced correction in the temporal pass alone leaves the darkening; in the spatial pass alone it removes it.
- **3.1's PT profile does not carry over to GI.** With all three guards gone (boiling filter, age limit, short history), fireflies pass into DLSS-RR: the `Lit` mean rose 7% and the flickering pixels 3×. Restoring any one of the three restores stability (std 1.58–1.63%).
- **The remaining +8% in the gallery is the age limit** (C-G5-1): without it the probe reads as converged. The limit stays for now, because it is what retires stale samples after a lighting change, and no check measures that yet (I1, I3).

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
  - runs `RTXDI_DISpatialResampling` on the shade-input DI reservoirs (2 samples, radius 1 px, `Basic`, material test off);
  - re-evaluates the chosen light.
- The firefly clamp limits X₂'s radiance to display luminance 10 / exposure, reading the 1×1 exposure image (`GpuTargets.exposureStorage`).
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
- Both stay off and return in I9, once M7 brings many lights (the case the sample built them for).

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
| ND5 | Debug resolve modes | every `NrcResolveMode`: training bounce heat map, primary / secondary vertex training radiance (raw and smoothed), query index, direct cache view | only `NRC cache` (the direct cache view, through the custom resolve) | the engine cannot see the training targets, which is what C-G5-3 needs (Q2) |
| ND6 | Training root surface | `CreateRootSurface`: the material evaluated like a traced hit | the guides: 8-bit albedo and f0, RGBA16F normal and roughness | small attribute precision loss at vertex 0 only |
| ND7 | Submit around NRC | no host wait | C-G3-6 host wait before the next frame's ray commands (device loss otherwise), +11% frame time at 1280×720 | perf only (NP1) |
| ND8 | Miss radiance | sky color on a miss | none (no environment light yet) | none until an environment light exists (M7, C-F3-5) |

### 3.3 Quality: leads for C-G5-3

C-G5-3 numbers:

- NRC + ReSTIR GI reads +9.3% / +10.2% (gallery / courtyard).
- NRC mode reads +11.9% / +8.1%.
- ReSTIR GI on traced paths stays within 1%, with the same `directLightAt`, the same `sampleBrdf` and 8 bounces.

So the bias is in the cache term. Each lead is a GI-check run of `NRC mode` with one setting changed. The setting comes from a temporary env var read in `applyPhase`, following the bisect pattern of [g5_restir_gi.md](../m4/g5_restir_gi.md) §3. A run takes about 1 min with the courtyard dropped and 1024 ground-truth frames.

| # | Lead | Why | Test | If confirmed |
|---|---|---|---|---|
| Q1 | **Self-training feedback** | `NrcCommon.h`: "If there's an error in the path tracer that breaks energy conservation … the self training feedback can sometimes lead to the cache getting brighter and brighter." The engine runs the SDK defaults (`proportionUnbiased` 0.0625, `selfTrainingAttenuation` 1), and threshold 0.01 ends training paths early in cache lookups, so most training targets contain the cache's own output | `selfTrainingAttenuation` 0, then 0.5; `proportionUnbiased` 1 | a bias that falls with attenuation is an energy gain amplified by the loop: look for it per vertex (Q2) rather than shipping attenuation |
| Q2 | **Training targets** | the SDK's `PrimaryVertexTrainingRadianceSmoothed` / `SecondaryVertexTrainingRadianceSmoothed` show what the network is trained toward | add a debug view that runs the SDK's `Resolve` in one of those modes into a scratch image (the custom resolve stays for the image); compare its mean with the ground truth's `Indirect radiance` on the gallery pose | if the target is already ~10% bright, the engine's path is wrong (vertex attributes, `directLightAt` at training vertices, emission order). If the target is correct and the query is bright, the query side is wrong (ND1, attributes at X₂) |
| Q3 | **Query placement (ND1)** | NVIDIA's heuristic exists to keep cache lookups away from small footprints, where the network's error shows; the engine never lets it run | first count the queries the heuristic would place beyond X₂ (heuristic on, query path continued with the ground-truth path loop, `referencePathRadiance`); then GI check with it on | ship the heuristic in the query pass; it costs extra rays only where it continues |
| Q4 | **Training budget** | the sample trains 1 iteration per frame for frame rate; the SDK default is 4 (64 K records). `learningRate` 1e-2 (SDK default) | NRC mode bias and tile p90 at 1, 2 and 4 iterations; train cost per iteration | a quality knob per tier (engine spec P1: tiers scale NRC settings) |
| Q5 | **Radiance scale** | `maxExpectedAverageRadianceValue` should be "the average radiance in a bright scene"; the engine keeps the sample's 1 under a 10 lx sun | 0.25 / 1 / 4 | set it from the exposure meter's mean, once, not per frame (the SDK asks for a stable value) |
| Q6 | Root attributes (ND6) | 8-bit guides at vertex 0 | training root from the decoded vis-buffer hit, as the sample does | low priority; only vertex 0 differs |

**Order:**

1. Q1 and Q2 first: they are cheap, and they split the problem in two.
2. Then Q3.
3. Q4 and Q5 are tuning, after the bias is understood.

NRC returns as a default only when the GI check gates its phase (C-G5-3's exit). As the 1.0 comparison switch it needs at least a known cause.

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
| F1 | GI-check realizations depend on the frame index, so adding a phase shifts every later phase's noise. On 2026-11-20 the courtyard's converged ReSTIR GI gate read +1.51% against its 1.5% limit (7 of 8). This was a realization shift, not a regression: the same settings read within the limit before the O1 phases were added, and again at +0.63% (8 of 8) once the O2 / O6 phases were appended | C-G5-1 note | I1 |
| F2 | The stability probe (static-camera `Lit` / `Indirect` temporal std, flickering px, mean per settings set) exists only as a scratch patch. In each batch, the first process run reads abnormal (a courtyard `Lit` mean half the normal one), so batches need a warm-up run | §2.5, §2.6 | I1 |
| F3 | No check measures how fast lighting settles after a change (age limit, C-G5-1), or how the screen-edge band converges (C-M4-1) | C-G5-1, C-M4-1 | I1, I3, I4 |
| F4 | `ReSTIR DI initial` includes the wait for the async TLAS | C-F3-9 | I2 |
| F5 | The DI ray budget with one light: initial-candidate visibility, the ray-traced temporal correction ray, final visibility (reused up to 4 frames), and a BRDF candidate that finds no light (O8). Final visibility reuse leaks light up to ~16 px past hard shadow edges | C-R32-3, C-R32-4 | I4 |
| F6 | NVIDIA's DI defaults fail the convergence check, which holds its own settings; RR flickers more on noise-free input | C-R32-1, C-R32-5 | I4 |
| F7 | A non-empty PTLAS update costs at least ~0.11 ms, more than the full build it replaced when everything moves | C-C4-4 | I2 |
| F8 | Per-frame CPU work is O(entities) (`FrameData::write`) | C-M0-5, audit AU4 | I2 |
| F9 | The async-compute overlap capture and every 2060 Super number are still open | M0 / M2 status, C-M0-1, C-F3-7, C-G3-2 | I2, I8 |

## 5. Not applicable

- ReSTIR PT (CGNS, decorrelation, duplication maps, hybrid shift): engine spec §15, after 1.0.
- NRD settings, the RTXDI sample's TAA, the NRC sample's NRD path: the engine resolves with DLSS-RR only.
- Primary surface replacement (PSR): the engine has no mirror path.
- SHaRC: removed (C-G3-1); the sample's SHaRC path is not compared.
- Transmission and delta lobes: no transmissive or mirror material (`kMinAlpha`), so `isDeltaLobe` stays false.
