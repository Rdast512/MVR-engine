# M8.3 · Composable Pipeline Toggle System & Demo-Analog Presets

**Date:** 2026-11-26 · **Status:** complete (implemented & verified across CPU tests, GPU checks, parity lab, validation/GPU-AV, and ABBA release benchmarks)  
**Plan:** [M8 README](./README.md) step M8.3 · **Depends on:** [M8.1](./m8_1_pipeline_audit.md) (done), [M8.2](./m8_2_imgui_audit.md) (done)  
**Feeds into:** M8.4 (`SHaRC` multi-bounce cache toggle), [M8.5](./m8_5_perf_and_nvidia_benchmark.md) (P5/P6 target lighting stack & NVIDIA demo benchmarks), M8.6 (full toggle verification)  
**References:**
- `reference/RTXPT-main/Rtxpt/Shaders/IntroSample/` (`RTXDIStreamingRISLightingPass.hlsl`, `IntroRTXDIBridge.hlsli`, `IndirectPathTracer.hlsl`)
- `reference/RTXDI/Samples/FullSample/` (`UserInterface.cpp`, `LightingPasses.cpp`)
- `reference/RTXGI/Samples/IndirectDiffuse/Shaders/Pathtracer.hlsl`
- [Engine spec §11.2–§11.7](../../docs/engine_spec.md) · [Settings guide](../../docs/settings.md) · [Considerations](../considerations.md)

---

## 1. Objective & Design Principles

Replace the current coupled lighting switches — fixed-on direct lighting with three overlapping booleans (`isReference`, `isSpatialOnly`, `isTemporalOnly`) and the 3-way `IndirectMode { NrcRestirGi, Nrc, RestirGi }` — with **four independent, composable runtime pipeline toggles** plus **one-click demo-analog presets**:

1. **Independent stages:** Direct Lighting, Indirect Bounce 1, Multi-Bounce Radiance Cache, and Denoiser/Upscaler toggle independently at any frame boundary without a scene reload or `waitIdle` (except swapchain/extent rebuild when Denoiser switches render resolution, already built in M8.2).
2. **Zero-cost skipping:** Every disabled stage skips its GPU dispatches, unnecessary async light-preparation work, and clear passes (`combine.slang` and `debug_view.slang` mask disabled terms via `FrameFlag` bits), verified by ABBA total GPU frame time and `RayStartWait` (§8.3).
3. **Lean fast paths (`PERF-C1`, `PERF-D1`):**
   - `DirectMode::StreamingRis` executes as a **single fused compute shader** (`passes/rtxdi_streaming_ris.slang`) with zero DI reservoir reads/writes, decoupled from `Reference`'s forced candidate overrides.
   - `IndirectBounceMode::RawPt` writes `indirectRadiance` directly from `indirect.slang` (and adds NRC directly in `nrc_resolve.slang` via a compact 8 B/px `RGBA16F` primary-weight scratch when `Cache == Nrc`), completely eliminating GI reservoir LogLuv round-trips and the `rtxdi_gi_temporal` + `rtxdi_gi_shade` passes (which currently cost `0.17–0.27 ms` with reuse off in [M8.5 §1.1](./m8_5_perf_and_nvidia_benchmark.md#11-per-scope)).
4. **Lazy allocation & frame-safe retirement (`PERF-D2`):** Optional buffers/targets (`RtxdiReservoirs`, `RtxdiGiReservoirs`, `giStagnancy[2]`, `giSampleIds`, `giDuplication`, `rawIndirectWeight`) are allocated only when their stage/feature is active and retired once `frameNumber - lastUsedFrame >= MAX_FRAMES_IN_FLIGHT`, with zero `waitIdle` on toggle switches. NRC continues to pause rather than destroy its context (`C-G3-3`).
5. **Compatibility with existing checks & lab plans:** All **57** `tests/lab/*.lab` files + 5 built-in plans (62 plan entries in `tests/lab_registry_test.cpp`) and all self-test runners (`ENGINE_GUIDE_CAPTURE`, `ENGINE_GI_CHECK`, `ENGINE_STABILITY_CHECK`, `ENGINE_DLSS_CHECK`, `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`) keep parsing and running cleanly.

---

## 2. Composable Toggle Model & Data Structures

Create **`src/render/pipeline_modes.hpp`** and **delete `src/render/indirect_mode.hpp`** once all callers are migrated (§7). Legacy text forms (`ENGINE_INDIRECT_MODE`, `frame.indirectMode=...`) are handled at the CLI/registry parsers rather than keeping a duplicate enum header.

### 2.1 Stage Enums

```cpp
// Stage 10: primary direct lighting
enum class DirectMode : uint8_t
{
    Off = 0,       // skips all rtxdi_* passes, retires DI reservoirs
    StreamingRis,  // single-pass fused RIS + final shadow ray, user candidate counts, 0 DI reservoirs (PERF-C1)
    RestirDi,      // multi-pass ReSTIR DI controlled by DirectResamplingMode
    Count,
};

// Active when DirectMode == DirectMode::RestirDi
enum class DirectResamplingMode : uint8_t
{
    TemporalAndSpatial = 0, // RTXDI ReSTIRDI_ResamplingMode::TemporalAndSpatial
    Temporal,               // Temporal ON, Spatial OFF (Engine Target ReSTIR DI candidate)
    Spatial,                // Spatial ON, Temporal OFF
    Reference,              // Ground-truth override: Power_RIS, 32 local, 8 inf, 0 BRDF, no reuse (2-pass reference path)
    Count,
};

// Stage 11–12: bounce-1 indirect lighting
enum class IndirectBounceMode : uint8_t
{
    Off = 0,   // skips indirect, cache, and ReSTIR GI; pauses NRC; retires GI buffers
    RawPt,     // raw uncorrelated 1-bounce PT writing indirectRadiance directly (PERF-D1)
    RestirGi,  // 1-bounce PT -> GI reservoirs -> ReSTIR GI temporal + spatial + shade
    Count,
};

// Multi-bounce tail (active when IndirectBounceMode != Off and giReferenceBounces == 0)
enum class RadianceCacheMode : uint8_t
{
    Off = 0, // 1-bounce indirect only; pauses NRC (C-G3-3); zero NRC SDK isolation segments
    Nrc,     // NVIDIA NRC 0.16
    Sharc,   // placeholder in M8.3 (disabled in UI, refused as isPlanned="M8.4" in LabConfig); built in M8.4
    Count,
};

// One-click demo-analog presets (fixed toggle + settings bundles; no separate renderer code paths)
enum class PipelinePreset : uint8_t
{
    Custom = 0,
    EngineTargetRis,      // Streaming RIS + Raw 1-Bounce PT + NRC + DLSS-RR
    EngineTargetTemporal, // ReSTIR DI Temporal (Basic, History 6, Spatial OFF) + Raw 1-Bounce PT + NRC + DLSS-RR
    RtxptAnalog,          // reference/RTXPT-main IntroSample analog
    RtxdiAnalog,          // reference/RTXDI/Samples/FullSample analog
    RtxgiAnalog,          // reference/RTXGI/Samples/IndirectDiffuse analog
    Count,
};
```

`DenoiserMode { DlssRr, Off }` and `AaMode { None, Accumulation }` remain in `src/render/denoiser_mode.hpp` (built and verified in M8.2).

### 2.2 Validity & Effective-State Rules

| Rule | Effective Behavior |
|---|---|
| `IndirectBounceMode == Off` | Radiance cache is **inactive** (`effectiveCache == Off`): NRC is paused (`isNrcPaused() == true`), `nrc_*` and `rtxdi_gi_*` are skipped, UI shows `Radiance Cache` row disabled with tooltip `"Requires Indirect Bounce 1"`. The stored `radianceCache` choice is preserved so re-enabling Indirect restores it. |
| `rtxdi.giReferenceBounces > 0` | Ground-truth multi-bounce path tracing (`ENGINE_GI_CHECK`): forces `effectiveCache == Off` (pauses NRC) and forces GI reuse off (`giResamplingMode = None`). Works cleanly in both `RawPt` (direct write, zero reservoir overhead) and `RestirGi` (reservoir reference path). |
| `RadianceCacheMode == Sharc` (M8.3) | `isRadianceCacheBuilt(RadianceCacheMode::Sharc) == false`. UI renders the combo entry disabled (`SHaRC (M8.4)`). `parseLabConfig("frame.radianceCache=sharc")` returns `{.config = std::nullopt, .error = "...", .isPlanned = true}` via the per-value planned mechanism in §6.3. |
| `DirectMode != RestirDi` + `giSettings.isSecondaryResamplingEnabled` (O2) | O2 reads `rtxdi.reservoirs` at X₂. When `DirectMode` is `Off` or `StreamingRis`, DI reservoirs are not written/allocated; `RtxdiContext::prepareFrame` **forces `GiFlag::SecondaryResampling` OFF** and `arePathInfiniteLightsExact()` uses `giSettings.areInfiniteLightsShadedExactly`. UI disables the O2 checkbox with reason `"Requires ReSTIR DI reservoirs"`. |
| `DirectMode == RestirDi` + `DirectResamplingMode::Reference` | Preserves the exact ground-truth semantics of `isReference`: forces `Power_RIS`, `32` local, `8` infinite, `0` BRDF, `enableInitialVisibility = 0`, `areDiInfiniteLightsExact() = false`, `resamplingMode = None`. **Stays on the two-pass `rtxdi_initial + rtxdi_shade` path** (ground-truth cost does not matter; keeps `DebugView::RtxdiReservoirs` populated and gives `rtxdi_streaming_ris.slang` an independent ground-truth shader for §8.2 parity testing!). |
| `isLitPresented()` | Unchanged: `debugView.view == DebugView::Lit && worldLights.hasLights()`. With `Direct == Off` in a lit scene, Lit presents Indirect + Emissive through DLSS-RR / Post; with both `Direct == Off` and `Indirect == Off`, Lit presents pure Emissive (+ Glass if enabled). |

### 2.3 Shipped Default & Expected Baseline Drift

Until M8.5 benchmarks `EngineTargetRis` vs `EngineTargetTemporal` and promotes the winner to the shipped default ([M8 README §1](./README.md#1-new-10-target-lighting-stack-all-composable-toggles)):

- **Shipped default (startup):** `DirectMode::RestirDi` (`TemporalAndSpatial`, NVIDIA Medium `Settings`) + `IndirectBounceMode::RawPt` + `RadianceCacheMode::Nrc` + `DenoiserMode::DlssRr`.
- **Expected numerical drift vs pre-M8.3 `IndirectMode::Nrc`:** Switching startup indirect from non-reused GI reservoirs to `RawPt` is **not bit-exact** — it removes four sources of numerical difference present in the old GI-reservoir path:
  1. Two LogLuv quantizations (`indirect.slang` pack → `nrc_resolve.slang` unpack/repack → `rtxdi_gi_shade.slang` unpack).
  2. Evaluating the primary BRDF along `brdf.l` (from the offset ray origin) instead of `normalize(reservoir.position - surface.position)`.
  3. Glass transmittance segment: `RawPt` must still call `visibilitySegment(origin, x2Position)` (gated on `isGiShadowTransmitted(root)` so no `×1.0` factor changes FMA contraction, per `rtxdi_gi_shade.slang:174-179`), rather than `(brdf.l, hitT)`.
  4. Single-pass accumulation order vs two-pass `shadeInitialSample`.
- **Baselines requiring re-baselining / A/B gating (§8.2):**
  - Byte-repeatable NRC-mode stability lines (`ENGINE_STABILITY_CHECK`),
  - `frame=shipped` lab runs,
  - Guide-capture NRC shots (`ENGINE_GUIDE_CAPTURE`),
  - `ENGINE_GI_CHECK` "NRC mode" phase.
  Wave B gates `RawPt + Nrc` with an explicit A/B run against a pre-M8.3 baseline build to confirm equal-or-better accuracy (less quantization loss) and expected variance bounds before accepting the new baseline.

### 2.4 New `FrameFlag` Bits (`src/core/types.hpp` & `shaders/types.slang`)

| Flag | Bit | Meaning |
|---|---|---|
| `FrameFlag::DirectDisabled` | `1u << 12` | `DirectMode == Off`. `combine.slang` (both center pixel and `neighborLuminance`) treats direct radiance as `0` without sampling `directRadianceSampled`; `debug_view.slang` `DirectLighting` returns `emissive` only. |
| `FrameFlag::IndirectDisabled` | `1u << 13` | `IndirectBounceMode == Off`. `combine.slang` (both center pixel and `neighborLuminance`) treats indirect radiance as `0` without sampling `indirectRadianceSampled`; `debug_view.slang` `IndirectRadiance` and `NrcCache` return `0`. |
| `FrameFlag::RawIndirect` | `1u << 14` | `IndirectBounceMode == RawPt`. `indirect.slang` writes `indirectRadiance` directly (and `rawIndirectWeight` when NRC is active) instead of packing a GI reservoir; `nrc_resolve.slang` adds NRC directly onto `indirectRadiance`. |

Existing `FrameFlag::DlssInputDirectOnly` / `DlssInputIndirectOnly` (settings lab attribution) compose naturally with `DirectDisabled` / `IndirectDisabled`.

---

## 3. Zero-Cost Pipeline Execution & Fast Paths

### 3.1 Frame Execution Matrix (Ray Command Buffer & Async Compute)

Define `isDebugViewUsingLights = debugView.view == DebugView::UnshadowedLights || debugView.view == DebugView::ShadowedLights` (`debug_view.slang:282-285`).

| Stage / Pass | Active Condition | When Skipped |
|---|---|---|
| **Async `rtScene.recordBuild` (PTLAS)** | Always (incremental; ~0.01 ms static, keeps `ptlasState` synced) | — |
| **Async `rtxdi.recordLightPreparation`** (`light_world` → `light_pdf_mips` → `rtxdi_presample`) | `Direct != Off \|\| Indirect != Off \|\| isGlassPassActive \|\| isDebugViewUsingLights` | Skipped when no consumer reads world lights or RIS buffers; **must clear `isLightPrepTimed[frameSlot] = false`** so `collectLightPrepResults` does not read stale timestamps. `WorldLights::prepareFrame` still updates host counts so `hasLights()` stays truthful. |
| **Resolve→Ray Barrier + `RayStartWait`** | Always at start of `recordRayCommands` (moved out of `recordDirectLight`) | Never skipped — keeps resolve→indirect/combine/debug synchronization and `RayStartWait` valid when `Direct == Off`. |
| **`rtxdi_streaming_ris.slang`** (`RtxdiShade` scope; `RtxdiInitial` / `RtxdiTemporal` end immediately) | `Direct == StreamingRis` | Skipped otherwise. Zero DI reservoir reads/writes. |
| **`rtxdi_initial.slang`** (`RtxdiInitial`) | `Direct == RestirDi && hasRisCandidates` | Skipped in `Off`, `StreamingRis`, or exact-sun-only scenes. |
| **`rtxdi_temporal.slang`** (`RtxdiTemporal`) | `Direct == RestirDi && Resampling ∈ {Temporal, TemporalAndSpatial} && hasRisCandidates` | Skipped in `Off`, `StreamingRis`, `Spatial`, `Reference`. |
| **`rtxdi_shade.slang`** (`RtxdiShade`) | `Direct == RestirDi` (including `Reference`) | Skipped in `Off` and `StreamingRis`. |
| **`nrc_begin` + `nrc_update.slang`** (`NrcUpdate`) | `Indirect != Off && Cache == Nrc && giReferenceBounces == 0 && nrc.isActive()` | Skipped in `Indirect == Off` or `Cache != Nrc`; **no `recordIsolated` SDK split**. |
| **`indirect.slang`** (`Indirect`) | `Indirect != Off` | Skipped in `Indirect == Off`. |
| **`nrc_train`** (`NrcTrain`) | Same as `nrc_update` | Skipped; **no `recordIsolated` SDK split**. |
| **`nrc_resolve.slang`** (`NrcResolve`) | `isNrcRecorded && !isCacheView && !isNrcDebugResolve` | Skipped in `Indirect == Off`, `Cache != Nrc`, or debug cache view. In `RawPt` mode, runs `rawIndirect` branch directly into `indirectRadiance`. |
| **NRC SDK Debug Resolve** | `isNrcRecorded && isNrcDebugResolve` | Skipped when `resolveMode == AddQueryResultToOutput` or NRC inactive. |
| **`rtxdi_gi_temporal.slang`** (`GiTemporal`) | `Indirect == RestirGi && !isNrcDebugResolve && rtxdi.isGiReuseEnabled()` | Skipped in `Off`, `RawPt`, and `isGiReference`. |
| **`rtxdi_gi_shade.slang`** (`GiShade`) | `Indirect == RestirGi && !isNrcDebugResolve` | **Skipped completely in `Off` and `RawPt`** (`PERF-D1`). |
| **`rtxdi_gi_duplication.slang`** (timed in `GiShade`) | `Indirect == RestirGi && !isNrcDebugResolve && rtxdi.isGiDuplicationMapEnabled()` | Skipped in `Off`, `RawPt`, or when duplication reduction is off. |
| **`combine.slang`** (`Combine`) | Always | Zero-cost flag checks skip sampling `directRadianceSampled` (`DirectDisabled`) and/or `indirectRadianceSampled` (`IndirectDisabled`) in both center and 3×3 neighbor paths. |
| **`glass.slang`** (`Glass`) | `rtScene.glassRowCount() > 0 && glass.isPassEnabled` | Already skipped without glass. |
| **`DLSS-RR` / `Accumulation` / `Post`** | Unchanged (verified in M8.2) | Already skipped per `DenoiserMode` / `AaMode` / `isLitPresented()`. |

**MG-P7 pre-acquire submit behavior:**
- When `isNrcRecorded == false` and `DLSS-RR` is active (same as today's ReSTIR GI mode), all pure-Vulkan passes before DLSS-RR (`recordDirectLight`, `indirect`, `combine`, `glass`) run in one pre-acquire segment (`preSdkRaySegmentCount == 1`), with `DLSS-RR` + `post` in the post-acquire submit.
- When **both** NRC and DLSS-RR are off (`DenoiserMode::Off`, `Cache != Nrc`), no `recordIsolated` split runs, so `preSdkRaySegmentCount == 0` and all ray segments submit together post-acquire (unchanged from today).

### 3.2 Fast Path 1: Fused Streaming RIS (`shaders/passes/rtxdi_streaming_ris.slang`, `PERF-C1`)

Modeled on `reference/RTXPT-main/Rtxpt/Shaders/IntroSample/RTXDIStreamingRISLightingPass.hlsl`, preserving **all** existing behavior of `rtxdi_initial.slang` and `rtxdi_shade.slang`:

- **Single fullscreen compute dispatch** (`[numthreads(8, 8, 1)]`, timed under `GpuTimerScope::RtxdiShade`; `RtxdiInitial` and `RtxdiTemporal` end immediately).
- **Empty-light fast clear (`rtxdi_shade.slang:57-64`):** if `gRtxdiBridge.lightCount == 0`, write `float4(0, 0, 0, 1)` to `directRadianceStorage` for in-bounds pixels and return.
- **One G-buffer surface decode** (`RAB_GetGBufferSurface(int2(id.xy), false)` — vs 2× in `rtxdi_initial` + `rtxdi_shade`), gated on `RAB_IsSurfaceValid(surface) && surface.isLit` (`M8.1 BUG-C3`).
- **Decoupled from `Reference`:** uses the user's `RtxdiContext::Settings` (`localLightSamplingMode` = `Uniform` / `Power_RIS` / `ReGIR_RIS`, `localLightSamples`, `infiniteLightSamples`, `brdfSamples`, `areInfiniteLightsShadedExactly`, `areLightTilesPerPixel`), while forcing `initialSampling.enableInitialVisibility = 0` so initial sampling shoots no candidate shadow ray before the final shading shadow ray (note: BRDF candidates still trace their directional bounce ray when `sampling.numBrdfSamples > 0`).
- **Exact parity rules carried from `rtxdi_initial.slang:41-52` and `rtxdi_shade.slang:72-135`:**
  1. `const bool isInfiniteExact = (constants.diFlags & kDiFlagExactInfiniteLights) != 0;`
  2. If `RAB_IsSurfaceValid(surface) && surface.isLit && hasRisLightCandidates(constants, isInfiniteExact)`:
     - Copy `sampling = constants.initialSampling;`
     - If `gRtxdiBridge.triangleLights.lightCount == 0 && constants.lightBuffer.environmentLightParams.lightPresent == 0`, set `sampling.numBrdfSamples = 0;` (`m8 I6 O8`).
     - If `isInfiniteExact`, set `sampling.numInfiniteLightSamples = 0;` (`m8 I16 F28`).
     - Call `RTXDI_SampleLightsForSurface(rng, coherentRng, surface, sampling, constants.lightBuffer, constants.localLightRisBuffer, constants.environmentLightRisBuffer, regirParameters(), lightSample)` — returns `(reservoir, lightSample)` directly in registers.
     - If `RTXDI_IsValidDIReservoir(reservoir)` and `RTXDI_GetDIReservoirInvPdf(reservoir) > 0`:
       - Evaluate `shade = shadeLightSample(surface, lightSample)` (reuses `lightSample` in registers — skips `RAB_LoadLightInfo` + `RAB_SamplePolymorphicLight`; BRDF evaluation stays).
       - If `any(shade > 0.0)` and `RAB_GetConservativeVisibility(surface, lightSample)`:
         - `radiance = shade * RTXDI_GetDIReservoirInvPdf(reservoir);`
         - If `any(radiance > 0.0)`: `radiance *= glassTransmittance(root, surface, lightSample);`
  3. If `isInfiniteExact && RAB_IsSurfaceValid(surface) && surface.isLit`:
     - `radiance += shadeInfiniteLightsExactly(root, surface, constants.lightBuffer.infiniteLightBufferRegion);` (shared helper extracted from `rtxdi_shade.slang:22-46` into `shaders/lighting/rtxdi_shading.slang` so `rtxdi_shade.slang` and `rtxdi_streaming_ris.slang` share one definition).
  4. Write `storageTex2D<float4, kImageFormatRgba16f>(output)[id.xy] = float4(radiance, 1.0);`
- **Zero DI reservoir buffer reads or writes** (`reservoirs` BDA is `0`, `reservoirCount == 0`).
- **Work eliminated vs 2-pass no-reuse (`rtxdi_initial` + `rtxdi_shade`):** removes 1 full-screen dispatch, 1 compute→compute barrier, 1 G-buffer surface decode, 1 light decode/sample, and **3 DI reservoir accesses/px** (`3 × sizeof(RTXDI_PackedDIReservoir)` = `72 B/px`: write in `rtxdi_initial`, read + unconditional write-back at `rtxdi_shade.slang:133-134`).

### 3.3 Fast Path 2: Raw 1-Bounce PT (`PERF-D1`)

Eliminates GI reservoir LogLuv pack/unpack and `rtxdi_gi_shade` when `IndirectBounceMode == RawPt`:

#### Exact AO & Glass Transmittance Contract

In the current codebase:
- `RAB_Surface` has **no** `ao` field, and `shadeGiSample` (`shaders/lighting/rtxdi_bridge.slang:697-718`) applies **no** AO to traced bounce-1 radiance.
- Material AO is loaded from `tex2D(gRtxdiBridge.current.sampled[kGuideDiffuseAlbedo]).Load(int3(int2(id.xy), 0)).a` in `nrc_resolve.slang:93-96` and applied to the **NRC cache term only** (engine spec §11.6, `M8.1 BUG-E2`).
- Glass transmittance on the primary-to-X₂ segment is `reconnectionTransmittance(root, surface, x2Position)` (`visibilitySegment(rasterRayOrigin(surface), x2Position, true, l, tMax)` → `glassTransmittance(root, origin, l, tMax)`), multiplied **only when** `(root.glassFlags & kGlassFlagShadowTransmittance) != 0` so no `×1.0` factor is folded in on scenes without glass transmittance (`rtxdi_gi_shade.slang:174-179`).

In `RawPt` mode (`(root.frameFlags & kFrameFlagRawIndirect) != 0`):
- `indirect.slang` already holds `brdf.weight` (`evalBrdf * nDotL / pdf`), `x2Position`, and `x2Traced` (`emissive + secondaryDirectLight + referenceBounces`, clamped by `clampIndirectRadiance` if `kGiFlagFireflyClamp`).
- Compute `primaryWeight`:
  ```hlsl
  float3 primaryWeight = brdf.weight;
  if ((root.glassFlags & kGlassFlagShadowTransmittance) != 0)
  {
      primaryWeight *= reconnectionTransmittance(root, primary, x2Position);
  }
  ```
- **Case A: `RawPt` + `Cache == Off` (or `giReferenceBounces > 0`):**
  - `indirect.slang` writes `indirectRadianceStorage[pixel] = float4(x2Traced * primaryWeight, 1.0)` directly (or `float4(0, 0, 0, 1)` on miss/sky/unlit/empty).
  - **Zero** GI reservoir writes, **zero** `rawIndirectWeight` writes, **zero** `nrc_*` passes, **zero** `rtxdi_gi_*` passes.
  - `specularHitDistanceStorage` is still written directly by `indirect.slang` (same formula as today).
- **Case B: `RawPt` + `Cache == Nrc` (`NrcResolveMode::AddQueryResultToOutput`):**
  - `indirect.slang` writes:
    - `indirectRadianceStorage[pixel] = float4(x2Traced * primaryWeight, 1.0)`
    - `rawIndirectWeightStorage[pixel] = float4(min(primaryWeight, 65504.0), min(luminance(x2Traced), 65504.0))` (8 B/px `R16G16B16A16_SFLOAT` lazy target; `float4(0.0)` on miss/sky/unlit/empty). Note: `65504.0` is purely an FP16 finite guard (no artificial `kMaxBrdfValue = 1e4` clamp, since `rtxdi_gi_shade.slang`'s `shadeInitialSample` has no clamp — `kMaxBrdfValue` is only used inside `glossyMisWeight`).
  - `nrc_resolve.slang` (when `(root.frameFlags & kFrameFlagRawIndirect) != 0`):
    - Loads `weightAndLum = storageTex2D<float4, kImageFormatRgba16f>(targets.rawIndirectWeightStorage)[id.xy]` (for in-bounds pixels, else `0`).
    - `const bool isValid = isInBounds && any(weightAndLum.rgb > 0.0);`
    - Calls `countNrcHealth(nrcConstants, isQueried && isValid, isAnswerFinite, luminance(cached), isValid ? weightAndLum.a : 0.0);` **before any early return** (matching the exact wave-uniform signature at `nrc_resolve.slang:81-82`).
    - If `!isValid` or `!any(cached > 0.0)`, returns immediately (leaving `indirectRadianceStorage[id.xy]` untouched with `x2Traced * primaryWeight`).
    - Otherwise loads `ao = tex2D(gRtxdiBridge.current.sampled[kGuideDiffuseAlbedo]).Load(int3(int2(id.xy), 0)).a`, computes `added = cached * ao` (matching `nrc_resolve.slang:93-96` — AO on the cache term **only**!).
    - Loads `tracedOut = storageTex2D<float4, kImageFormatRgba16f>(targets.indirectRadianceStorage)[id.xy].rgb` and computes `total = tracedOut + added * weightAndLum.rgb`.
    - If `(constants.giFlags & kGiFlagFireflyClamp) != 0`:
      - `const float totalX2Lum = weightAndLum.a + luminance(added);`
      - If `totalX2Lum > kMaxIndirectRadiance`, scales `total *= kMaxIndirectRadiance / totalX2Lum;` (algebraically identical to `nrc_resolve.slang:101-108` clamping `x2Traced + ao * cached` before multiplying by `primaryWeight`!).
    - Writes `storageTex2D<float4, kImageFormatRgba16f>(targets.indirectRadianceStorage)[id.xy] = float4(total, 1.0)` (guarded by `isFiniteFloat3(total)`).
    - **Skips `rtxdi_gi_temporal`, `rtxdi_gi_shade`, and `rtxdi_gi_duplication` completely.**
- **Case C: `RawPt` + `Cache == Nrc` + `DebugView::NrcCache` or `isNrcDebugResolve`:**
  - `DebugView::NrcCache`: `indirect.slang` early-branches on `isCacheView` (`M8.1 PERF-E2`) and queries NRC at the primary vertex; `debug_view.slang` reads `nrcQueryResult` directly (unchanged).
  - `isNrcDebugResolve` (`resolveMode != AddQueryResultToOutput`): SDK's `nrc.recordDebugResolve` overwrites `indirectRadiance` directly (unchanged).
- **Case D: `RawPt` + `Cache == Sharc` (M8.4 hook):**
  - `indirect.slang` queries SHaRC inline at X₂, adds `cached * ao` to `x2Traced` before multiplying by `primaryWeight`, and writes `indirectRadiance` directly — zero `rawIndirectWeight` target and zero resolve pass.

### 3.4 `combine.slang` & `debug_view.slang` Gating

- **`shaders/passes/combine.slang`:**
  - Define `isDirectEnabled = (root.frameFlags & kFrameFlagDirectDisabled) == 0 && !isIndirectOnly` and `isIndirectEnabled = (root.frameFlags & kFrameFlagIndirectDisabled) == 0 && !isDirectOnly`.
  - Apply these gates in **both**:
    1. Center pixel lighting (`direct` / `indirect` loads), **and**
    2. **`neighborLuminance(GpuTargets targets, int2 pixel, bool isDirectEnabled, bool isIndirectEnabled)`** (`combine.slang:16-40`, used by `preDenoiserClamp` spatial mode) so 3×3 neighbor loads never read stale `directRadianceSampled` or `indirectRadianceSampled` when a stage is disabled!
- **`shaders/passes/debug_view.slang`:**
  - `kDebugViewDirectLighting`: `((root.frameFlags & kFrameFlagDirectDisabled) != 0 ? float3(0.0) : tex2D(targets.directRadianceSampled).Load(int3(pixel, 0)).rgb) + emissive`.
  - `kDebugViewIndirectRadiance`: `(root.frameFlags & kFrameFlagIndirectDisabled) != 0 ? float3(0.0) : tex2D(targets.indirectRadianceSampled).Load(int3(pixel, 0)).rgb`.
  - `kDebugViewRtxdiReservoirs`: return `float3(0.0)` when `constants.reservoirs == 0 || constants.reservoirCount == 0`.
  - `kDebugViewIndirectSamples`: return `float3(0.0)` when `constants.giReservoirs == 0 || constants.giReservoirCount == 0`.
  - `kDebugViewNrcCache`: return `float3(0.0)` when `(root.frameFlags & kFrameFlagIndirectDisabled) != 0` or `loadNrcConstants(root).queryRadiance == 0`.

---

## 4. Lazy Allocation & Retirement (`PERF-D2` & DI Reservoirs)

### 4.1 Lifecycle Pattern

Standardize on one lazy target lifecycle across `RenderTargets` and `RtxdiContext` (avoiding `submitAndWait` during runtime toggles):
- **Allocate** between frames after the slot's fence via `ensure*()` when the resource's active condition becomes true; write image descriptors into free heap slots only.
- **Image layout init:** extend `RenderTargets::recordPendingInit(cmd)` (recorded at the start of `recordFrameCommands`, currently only handling `dlssInputAccumulation`) with a pending-transition flag per lazy target (`giStagnancy[2]`, `giSampleIds`, `giDuplication`, `rawIndirectWeight`) that emits a single `eUndefined → eGeneral` `ImageMemoryBarrier2` before compute passes on the first frame after creation. (Remove those targets from `RenderTargets::recreate()`'s unconditional `submitAndWait` creation block; on resize, destroy them and let the next frame's `ensure*()` re-create only what is active.)
- **First-frame read safety:**
  - `RtxdiReservoirs`: entering `RestirDi` (from `Off` or `StreamingRis`) sets `isDiHistoryReset` (`FrameFlag::HistoryReset`), so `rtxdi_temporal` skips reading previous reservoirs on frame 0.
  - `RtxdiGiReservoirs`, `giStagnancy[2]`, `giSampleIds`, `giDuplication`: entering `RestirGi` or enabling `Stagnancy` decorrelation / `DuplicationHistoryReduction` changes `IndirectState` and sets `isIndirectHistoryReset` (`FrameFlag::IndirectHistoryReset`), which causes `rtxdi_gi_temporal` to skip reading previous GI reservoirs and `giDuplication`, and `smoothStagnancy` (`rtxdi_gi_shade.slang:75`) to skip reading `giStagnancyPrevious`.
- **Retire** when `frameNumber - lastUsedFrame >= MAX_FRAMES_IN_FLIGHT` (guaranteed no in-flight frame references the buffer/image — **zero `waitIdle` on toggle switches**), freeing descriptor heap slots and VMA memory.

### 4.2 Lazy Resource Inventory & VRAM Savings

| Resource | Owner | Format / Layout | Exact Active Condition | VRAM @ 1707×960 (1440p Quality) | VRAM @ 2560×1440 (Native / Off) |
|---|---|---|---|---|---|
| `RtxdiReservoirs` | `RtxdiContext` | `c_NumReSTIRDIReservoirBuffers × sizeof(RTXDI_PackedDIReservoir)` per padded tile pixel (`3 × 24 B = 72 B/px`) | `Direct == RestirDi` (includes `Reference`) | **~113 MiB** | **~253 MiB** |
| `RtxdiGiReservoirs` | `RtxdiContext` | `3 × sizeof(RTXDI_PackedGIReservoir)` (`96 B/px`) | `Indirect == RestirGi` | **~150 MiB** | **~338 MiB** |
| `giStagnancy[2]` | `RenderTargets` | 2 × `R32_SFLOAT` (`8 B/px`) | `Indirect == RestirGi && rtxdi.isGiReuseEnabled() && giSettings.decorrelationMode == RTXDI_PT_DECORRELATION_MODE_STAGNANCY` (`rtxdi_gi_shade.slang:356-363`) | **~12.5 MiB** | **~28.1 MiB** |
| `giSampleIds` + `giDuplication` | `RenderTargets` | `R32_UINT` + `R32_SFLOAT` (`8 B/px`) | `Indirect == RestirGi && rtxdi.isGiDuplicationMapEnabled()` | **~12.5 MiB** | **~28.1 MiB** |
| `rawIndirectWeight` *(new)* | `RenderTargets` | `R16G16B16A16_SFLOAT` (`8 B/px`) | `Indirect == RawPt && Cache == Nrc && giReferenceBounces == 0` | **~12.5 MiB** | **~28.1 MiB** |
| `NrcCache` SDK buffers | `NrcCache` | NRC 0.16 MLP + query/training buffers | Persistent once configured (**never destroyed on toggle**, `C-G3-3`); paused when `Cache != Nrc` or `Indirect == Off` | unchanged | unchanged |

**Net VRAM savings at 1707×960 (1440p Quality) vs current always-allocated baseline:**
- **`EngineTargetRis`** (`StreamingRis + RawPt + Nrc`): retires `RtxdiReservoirs` (~113 MiB) + `RtxdiGiReservoirs` (~150 MiB) + `giStagnancy[2]` (~12.5 MiB) + `giSampleIds/giDuplication` (~12.5 MiB), adds `rawIndirectWeight` (~12.5 MiB) → **~275 MiB saved** (~620 MiB at 1440p native).
- **`EngineTargetTemporal`** (`RestirDi Temporal + RawPt + Nrc`): keeps `RtxdiReservoirs`, retires GI reservoirs + decorrelation targets, adds `rawIndirectWeight` → **~162 MiB saved** (~366 MiB at 1440p native).
- **`RawPt + Cache Off`** (or M8.4 `SHaRC`): also retires `rawIndirectWeight` (**+12.5 MiB** saved).

### 4.3 `GpuTargets` Addition (`src/core/types.hpp` & `shaders/types.slang`)

Add `uint32_t rawIndirectWeightStorage = kNoneIndex;` at the end of `GpuTargets` (`sizeof(GpuTargets)` grows from **216 → 220 B**, 4-byte aligned, no padding needed; update `static_assert` and `tests/layout_test.cpp`).  
When `giStagnancy` / `giSampleIds` / `giDuplication` / `rawIndirectWeight` are retired, `RenderTargets::prepareFrame` writes `kNoneIndex` into their `GpuTargets` slots.

---

## 5. State Transitions, History Resets & Synchronization Contracts

### 5.1 Frame-Boundary Change Detection (`Renderer::drawFrame`)

Reuse the existing DI change detection (`vk_renderer.cpp:284-294`: `isDiModeChanged`, `rtxdi.isSettingsDirty()`, `isDiHistoryReset`) rather than adding a duplicate `DirectState` struct, and update `Renderer::IndirectState` to replace `IndirectMode` with `(IndirectBounceMode bounce, RadianceCacheMode cache)`:

| State Change | Required Resets |
|---|---|
| `DirectMode` or `DirectResamplingMode` changes | `isDiModeChanged = true` → `resetDlssHistory()`, `resetAccumulation()`. If entering `RestirDi` temporal reuse (from `Off`, `StreamingRis`, `Spatial`, or `Reference`), set `isDiHistoryReset = true` → emits `FrameFlag::HistoryReset` for 1 frame. *(Note: `FrameFlag::HistoryReset` is global — it zeroes motion and `prevWorldLights` and resets ReSTIR GI and DLSS-RR, matching today's `isDiHistoryReset` behavior.)* |
| `RtxdiContext::Settings` changes (while `Direct != Off`) | `isDiSettingsChanged = rtxdi.isSettingsDirty()` → `resetDlssHistory()`, `resetAccumulation()` (unchanged from M8.2 `BUG-UI-6`). |
| `IndirectBounceMode` or `RadianceCacheMode` changes | `isIndirectHistoryReset = true` (`FrameFlag::IndirectHistoryReset`), `resetDlssHistory()`, `resetAccumulation()`. NRC `prepareFrame` receives `isPaused = isNrcPaused()`, preserving weights across pause/resume (`C-G3-3`). |
| `isNrcPaused()` helper | `return indirectBounce == IndirectBounceMode::Off \|\| radianceCache != RadianceCacheMode::Nrc \|\| rtxdi.giReferenceBounces != 0;` |
| `isSpecularHitDistanceWritten` (`vk_renderer.cpp:188-189`) | Keep the full existing expression gated on Indirect: `((isLitDlss && dlss.guides.isSpecularHitDistanceUsed) \|\| debugView.view == DebugView::SpecularHitDistance) && indirectBounce != IndirectBounceMode::Off`. Track `appliedSpecularHitDistanceWritten`; if it flips while `isLitDlss`, reset `isDlssHistoryValid = false` (DLSS-RR input guide set changed). |

### 5.2 Synchronization & Barrier Rules (No Redundant Barriers)

1. **Resolve → Ray passes (`recordRayCommands`):** Move the existing resolve→ray `pipelineBarrier2` and `gpuTimer.end(cmd, GpuTimerScope::RayStartWait)` from `recordDirectLight` to the top of `recordRayCommands`. When `Direct == Off`, `recordDirectLight` is skipped and the barrier directly orders resolve's guide writes before `indirect` / `combine` / `glass` / `debug_view`.
2. **`indirect.slang` → `nrc_resolve.slang` (`RawPt + Nrc`):** Already ordered by **`nrcBoundary(cmd)`** at the end of `NrcCache::recordTrain` (`src/render/vk_nrc.cpp:481`, `eAllCommands → eAllCommands`). Do **not** add an extra barrier before `nrc_resolve`.
3. **`indirect.slang` / `nrc_resolve.slang` → `combine.slang` (`RawPt + Off` and `RawPt + Nrc`):** Already ordered by **`combineBarrier`** at the top of `Renderer::recordReconstruction` (`src/render/vk_renderer.cpp:1209-1215`, `ComputeShader(StorageWrite) → ComputeShader(SampledRead)`). Do **not** add an extra barrier after `indirect` or `nrc_resolve`.
4. **Best Practices + `C-G3-6`:** Zero extra fences, zero `waitIdle` on toggle, `ONE_TIME_SUBMIT` on all command buffers, and `preSdkRaySegmentCount` / `needsPostTlasWait` (`BUG-B4`) remain exact when NRC or DLSS-RR is skipped.

---

## 6. Demo-Analog Presets, ImGui (`SettingsWindow`), `SettingsRegistry` & `ENGINE_CONFIG`

### 6.1 Demo-Analog Presets (`PipelinePreset`)

Each preset is a single fixed configuration tuple (so `SettingsWindow` preset-match detection and `pipeline=<name>` round-trips are 100% deterministic):

| Preset (`pipeline=*`) | `DirectMode` | `DirectResamplingMode` & DI Settings | `IndirectBounceMode` | `RadianceCacheMode` | `DenoiserMode` | Reference / Notes |
|---|---|---|---|---|---|---|
| **Engine Target (RIS)** (`pipeline=targetRis`) | `StreamingRis` | `Power_RIS`, 16 local / 1 inf / 0 BRDF, `di.exactInfinite=on`, `di.perPixelLightTiles=on` | `RawPt` | `Nrc` | `DlssRr` (Quality) | Target candidate 1 (~275 MiB VRAM saved vs baseline) |
| **Engine Target (Temporal)** (`pipeline=targetTemporal`, alias `pipeline=target`) | `RestirDi` | `Temporal` (Spatial OFF), `Power_RIS`, 8 local / 1 inf / 0 BRDF, `Max History = 6`, `Basic` temporal bias correction, `di.exactInfinite=on`, `di.perPixelLightTiles=on`, `di.reuseFinalVisibility=off` | `RawPt` | `Nrc` | `DlssRr` (Quality) | Target candidate 2 (~162 MiB VRAM saved vs baseline; M8.5 benchmarks both and selects the 1.0 default) |
| **RTXPT Analog** (`pipeline=rtxpt`) | `StreamingRis` | `Uniform`, **8 local / 1 inf / 0 BRDF**, `di.exactInfinite=off` | `RawPt` | `Off` *(1-bounce PT until M8.4 enables `Sharc`)* | `DlssRr` (Quality) | Matches `RTXPT-main` `IntroRTXDIBridge.hlsli:76-96` (`Uniform`, 8 local, 1 inf, 0 BRDF, infinite lights in RIS, `ENABLE_SHARC 0`); deviations noted in `spec/considerations.md` (engine local region includes emissive triangles; SHaRC lands in M8.4) |
| **RTXDI Analog** (`pipeline=rtxdi`) | `RestirDi` | `TemporalAndSpatial`, NVIDIA `Medium` (`nvidiaSettings()`) | `RestirGi` (`nvidiaGiSettings()`) | `Off` | `DlssRr` (Quality) | Mirrors `reference/RTXDI/Samples/FullSample` |
| **RTXGI Analog** (`pipeline=rtxgi`) | `StreamingRis` | `Uniform`, **8 local / 1 inf / 0 BRDF**, `di.exactInfinite=off` | `RawPt` | `Nrc` | `DlssRr` (Quality) | Matches `RTXGI` `IndirectDiffuse/Shaders/Pathtracer.hlsl:35` (`RIS_CANDIDATES_LIGHTS 8`); deviation noted in `spec/considerations.md` (engine uses RTXDI Streaming RIS instead of RTXGI's custom analytic RIS loop) |

### 6.2 `SettingsWindow` & `settings_rows.hpp` Layout

Keep the NVIDIA layout verified in M8.2 (`nvidia_settings_layout.md`), adding a compact **`Pipeline`** block at the top of `SettingsWindow` (above `Direct Lighting`):

1. **`Pipeline` header (always open by default):**
   - `Pipeline Preset`: `Custom` | `Engine Target (RIS)` | `Engine Target (Temporal)` | `RTXPT Analog` | `RTXDI Analog` | `RTXGI Analog`
   - `Direct Lighting`: `Off` | `Streaming RIS` | `ReSTIR DI`
   - `Indirect (Bounce 1)`: `Off` | `Raw 1-Bounce PT` | `ReSTIR GI`
   - `Radiance Cache`: `Off` | `NRC` | `SHaRC (M8.4)` *(disabled when `Indirect == Off`; `SHaRC` item disabled until M8.4)*
   - *(Denoiser stays in the existing `Denoiser / AA` section right below, unchanged.)*
2. **`Direct Lighting (RTXDI)` section:**
   - Disabled / collapsed note when `Direct == Off`.
   - When `Direct == StreamingRis`: shows **Initial Sampling** rows (`Local Light Sampling`, `Local Samples`, `Infinite Samples`, `BRDF Samples`, `Exact Infinite Lights`, `Per-Pixel Light Tiles`); disables `Initial Visibility` (forced `Off` in Streaming RIS) and disables Temporal / Spatial / Final Visibility Reuse subsections with reason `"Streaming RIS (no reuse)"`.
   - When `Direct == RestirDi`: keeps the existing single **`Resampling Mode`** combo (`settings_window.cpp:618-623`, now backed directly by `DirectResamplingMode`: `Reference (None)` | `Temporal` | `Spatial` | `Temporal + Spatial`) and NVIDIA **`Quality Preset`** (`Fast` | `Medium` | `Unbiased` | `Ultra` | `Reference`, where `QualityPreset::Reference` sets both `nvidiaPreset(Reference)` settings and `DirectResamplingMode::Reference`, preserving `settings_window.cpp:393-399`).
3. **`Indirect Lighting` section:**
   - Disabled / collapsed note when `Indirect == Off`.
   - Replaces the old 3-way `Indirect Mode` combo (`settings_rows.hpp:54`) with the `Pipeline` section's `Indirect (Bounce 1)` + `Radiance Cache` combos.
   - `NRC` sub-rows (`Train Cache`, `Idle Training Frames`, `Training Iterations`, `Resolve Mode`, `Reset Network`) enabled only when `Indirect != Off && Cache == Nrc && giReferenceBounces == 0`.
   - `ReSTIR GI` Temporal / Spatial / Decorrelation sub-rows enabled only when `Indirect == RestirGi` (and `giReferenceBounces == 0`).
   - `Secondary DI Resampling (O2)` disabled when `Direct != RestirDi` (with tooltip `"Requires ReSTIR DI reservoirs"`).
4. **`Compare with` & `settings_rows.hpp`:**
   - Register the four new switches (`frame.directMode`, `frame.directResampling`, `frame.indirectBounce`, `frame.radianceCache`) in `settings_rows.hpp` so every preset difference highlights cleanly in yellow and closed-section counts remain exact (`tests/lab_registry_test.cpp`).
   - Add the five `PipelinePreset` configurations to `Compare with` alongside `Shipped`, `NVIDIA Fast–Reference`, `Tuned`, and `Converged`.

### 6.3 `SettingsRegistry`, `ENGINE_CONFIG` & Backwards Compatibility

#### Per-Value Planned Mechanism (`src/render/settings_registry.hpp`)

Extend `LabSwitch` with an optional per-enumerator planned span (parallel to `valueNames`):
```cpp
// Enum only: non-empty span parallel to valueNames; non-null entries mark an unbuilt enumerator (e.g. "M8.4")
std::span<const char* const> plannedValues{};
```
- `parseLabConfig`: when an `Enum` switch matches an index with `index < entry.plannedValues.size() && entry.plannedValues[index] != nullptr`, return `{std::nullopt, std::format("{}={} is planned ({})", entry.name, value, entry.plannedValues[index]), /*isPlanned=*/true}`.
- `tests/lab_registry_test.cpp`: `otherValue()` skips planned enumerators during round-trip tests and adds an explicit assertion that `parseLabConfig("frame.radianceCache=sharc")` returns `!parse.config && parse.isPlanned`.

#### New Primary Switches in `labSwitches()`

| Switch | Type | Values | Default (Shipped) |
|---|---|---|---|
| `frame.directMode` | Enum | `off`, `streamingRis`, `restirDi` | `restirDi` |
| `frame.directResampling` | Enum | `temporalAndSpatial`, `temporal`, `spatial`, `reference` | `temporalAndSpatial` |
| `frame.indirectBounce` | Enum | `off`, `rawPt`, `restirGi` | `rawPt` |
| `frame.radianceCache` | Enum | `off`, `nrc`, `sharc` (`plannedValues = {nullptr, nullptr, "M8.4"}`) | `nrc` |

#### Domain Set Precedence & Semantics (`parseLabConfig`)

1. **Two-pass parsing (extending existing set pass):**
   - **Pass 1a (`pipeline=<preset>`):** `pipeline=targetRis | targetTemporal | target | rtxpt | rtxdi | rtxgi` applies first so subsequent `di=*`, `gi=*`, or `frame=*` sets can override one domain on top of a pipeline preset.
   - **Pass 1b (`di=<set>`, `gi=<set>`, `frame=shipped`):** applied left to right:
     - `frame=shipped` resets all `frame.*` fields **including** `frame.directMode`, `frame.directResampling`, `frame.indirectBounce`, `frame.radianceCache`.
     - `di=<set>` (other than `di=reference`) resets `frame.directMode = DirectMode::RestirDi` and `frame.directResampling = DirectResamplingMode::TemporalAndSpatial` (preserving today's behavior where `di=<set>` clears `isReference`/`isSpatialOnly`/`isTemporalOnly`).
     - `di=reference` sets `di = tunedSettings()`, `frame.directMode = DirectMode::RestirDi`, `frame.directResampling = DirectResamplingMode::Reference` (identical to today).
   - **Pass 2 (`domain.switch=value`):** individual overrides applied left to right.
2. **Legacy Switch Aliases (Parsed in Pass 2, Not Duplicated in `labSwitches()`):**
   All existing **57** `tests/lab/*.lab` files and 5 built-in plans parse without modification:

| Legacy Token | Canonical Mapping |
|---|---|
| `frame.indirectMode=nrcRestirGi` | `indirectBounce = RestirGi`, `radianceCache = Nrc` |
| `frame.indirectMode=nrc` | `indirectBounce = RawPt`, `radianceCache = Nrc` |
| `frame.indirectMode=restirGi` | `indirectBounce = RestirGi`, `radianceCache = Off` |
| `di.reference=on\|1\|true` | `directMode = RestirDi`, `directResampling = Reference` |
| `di.reference=off\|0\|false` | if `directResampling == Reference`, set `directResampling = TemporalAndSpatial` (does not clobber `Temporal`/`Spatial`!) |
| `di.spatialOnly=on\|1\|true` | `directMode = RestirDi`, `directResampling = Spatial` (unless `Reference` was already set? Note: left-to-right override matches current priority if set after `di.reference=off`; if only `=on`, sets `Spatial`) |
| `di.spatialOnly=off\|0\|false` | if `directResampling == Spatial`, set `directResampling = TemporalAndSpatial` |
| `di.temporalOnly=on\|1\|true` | `directMode = RestirDi`, `directResampling = Temporal` |
| `di.temporalOnly=off\|0\|false` | if `directResampling == Temporal`, set `directResampling = TemporalAndSpatial` |

---

## 7. Implementation Plan (3 Phased Waves)

### Wave A · Composable Enums, Direct `Off` & Fused Streaming RIS (`PERF-C1`)

- **New files:**
  - `src/render/pipeline_modes.hpp` (`DirectMode`, `DirectResamplingMode`, `IndirectBounceMode`, `RadianceCacheMode`, `PipelinePreset`, helpers)
  - `shaders/lighting/rtxdi_shading.slang` (shared `glassTransmittance` + `shadeInfiniteLightsExactly` extracted from `rtxdi_shade.slang`)
  - `shaders/passes/rtxdi_streaming_ris.slang` (fused single-pass Streaming RIS)
- **Modified files:**
  - `CMakeLists.txt` (compile `rtxdi_streaming_ris.slang`)
  - `src/core/types.hpp` & `shaders/types.slang` (`FrameFlag::DirectDisabled`, `IndirectDisabled`, `RawIndirect`; `GpuTargets::rawIndirectWeightStorage`, `216 → 220 B`)
  - `src/render/vk_shader_objects.{hpp,cpp}` (`frameShaders.rtxdiStreamingRis`)
  - `src/render/vk_rtxdi.{hpp,cpp}` (replace `isReference`/`isSpatialOnly`/`isTemporalOnly` with `DirectMode` + `DirectResamplingMode`; lazy `ensureDiReservoirs()` / `retireDiReservoirs()`; force `GiFlag::SecondaryResampling` off when `Direct != RestirDi`; clear `isLightPrepTimed[slot]` when light prep is skipped)
  - `src/render/vk_renderer.{hpp,cpp}` (move resolve→ray barrier & `RayStartWait` to top of `recordRayCommands`; skip `recordDirectLight` when `Direct == Off`; dispatch `rtxdiStreamingRis` in `StreamingRis` and keep `Reference` on 2-pass `rtxdi_initial + rtxdi_shade`; skip `recordLightPreparation` when no lighting consumer is active; reuse `isDiModeChanged`/`isSettingsDirty`/`isDiHistoryReset`)
  - `shaders/passes/combine.slang` (gate center pixel AND `neighborLuminance` 3×3 loads on `kFrameFlagDirectDisabled` / `kFrameFlagIndirectDisabled`)
  - `shaders/passes/debug_view.slang` (flag gates + null-reservoir guards)
- **Done when:** `StreamingRis` with reference counts matches independent 2-pass `di=reference` on Sponza & Bistro (< 0.5% Monte Carlo variance), retires DI reservoirs, and beats 2-pass `initial + shade` in ABBA total GPU time.

### Wave B · Indirect `Off`, Raw 1-Bounce PT Fast Path (`PERF-D1`), Cache Toggle, Lazy GI Buffers (`PERF-D2`) & Caller Migration

- **Deleted file:** `src/render/indirect_mode.hpp`
- **Modified files:**
  - `src/render/vk_render_targets.{hpp,cpp}` (remove `giStagnancy`, `giSampleIds`, `giDuplication` from `recreate()`'s unconditional `submitAndWait` block; add lazy `ensureGiStagnancy()` / `retireGiStagnancy()`, `ensureGiDuplication()` / `retireGiDuplication()`, `ensureRawIndirectWeight()` / `retireRawIndirectWeight()`; extend `recordPendingInit(cmd)` with `eUndefined → eGeneral` transitions; write `kNoneIndex` when retired)
  - `src/render/vk_rtxdi.{hpp,cpp}` (lazy `ensureGiReservoirs()` / `retireGiReservoirs()`)
  - `shaders/passes/indirect.slang` (`kFrameFlagRawIndirect` fast path: write `indirectRadiance = x2Traced * primaryWeight` directly — NO AO on traced bounce, `reconnectionTransmittance` via `visibilitySegment` gated on `isGiShadowTransmitted`, plus `rawIndirectWeight = float4(min(primaryWeight, 65504.0), min(luminance(x2Traced), 65504.0))` when NRC active)
  - `shaders/passes/nrc_resolve.slang` (`kFrameFlagRawIndirect` path: wave-uniform `countNrcHealth`, load `ao` from `kGuideDiffuseAlbedo.a`, add `cached * ao * weight` to `indirectRadiance`, apply `kGiFlagFireflyClamp` using `tracedLum + luminance(cached * ao)`)
  - `src/render/vk_renderer.{hpp,cpp}` (replace `IndirectMode` with `IndirectBounceMode` + `RadianceCacheMode`; skip `recordIndirect` when `Indirect == Off`; skip `recordIndirectResampling` when `Indirect == RawPt`; rely on existing `nrcBoundary` and `combineBarrier` with zero redundant barriers; preserve full `isSpecularHitDistanceWritten` expression `&& indirectBounce != Off` with DLSS-RR history reset on change; lazy allocate/retire GI & raw-weight targets)
  - **All callers migrated:**
    - `src/core/vk_engine.cpp` (`ENGINE_INDIRECT_MODE` + `cycle` mapped to `(IndirectBounceMode, RadianceCacheMode)`)
    - `src/checks/check_harness.cpp` (L282, L416)
    - `src/checks/engine_gi_check.{hpp,cpp}`
    - `src/checks/engine_guide_capture.{hpp,cpp}` (including L644–646 mode suffix preservation)
    - `src/checks/engine_temporal_check.cpp`
    - `src/checks/engine_dlss_check.cpp`
    - `src/checks/lab/engine_lab.cpp`
    - `src/checks/lab/lab_plan.cpp` (built-in plans verified via legacy alias or canonical syntax)
- **Done when:** A/B comparison of `RawPt + Nrc` vs pre-M8.3 `IndirectMode::Nrc` passes `ENGINE_GI_CHECK=1` (`14/14`) and `ENGINE_GUIDE_CAPTURE=1`; `RawPt + Off` matches 1-bounce unbiased PT; ABBA total GPU time drops by at least the removed `GiTemporal + GiShade` cost.

### Wave C · Presets, `SettingsWindow`, `SettingsRegistry`, Tests & Docs

- **Modified files:**
  - `src/render/settings_registry.{hpp,cpp}` (per-value `plannedValues` on `LabSwitch`; 4 new primary switches; `pipeline=targetRis|targetTemporal|target|rtxpt|rtxdi|rtxgi`; `frame=shipped` + `di=<set>` + legacy aliases)
  - `src/ui/settings_rows.hpp` (replace `frame.indirectMode` at L54 with the 4 new switches)
  - `src/ui/settings_window.{hpp,cpp}` (`Pipeline` section + 5 `PipelinePreset`s + updated `Direct Lighting` / `Indirect Lighting` disabled-row rules + `Compare with` presets)
  - `tests/lab_registry_test.cpp` (new switches round-trip, `frame.radianceCache=sharc` refused as `isPlanned`, all 5 `pipeline=*` presets, legacy alias edge cases like `di.temporalOnly=on di.reference=off`, all 57 `.lab` files + 5 built-in plans, `Compare with` highlights)
  - `tests/layout_test.cpp` (`GpuTargets` `220 B` layout assertion)
  - `docs/settings.md`, `spec/considerations.md` (new M8.3 entries for RTXPT/RTXGI preset approximations, per-value `sharc` refusal, and light-prep skipping), and `spec/m8/README.md`
- **Done when:** All §8 unit tests, GPU checks, ABBA timing gates, and validation (including GPU-AV) pass.

---

## 8. Verification & Acceptance Criteria

### 8.1 CPU Unit Tests (`ctest`, both `windows-clang-release` and `windows-clang-release-checks`)

- `lab_registry_test`:
  - All 4 new switches round-trip; `frame.radianceCache=sharc` is rejected with `isPlanned == true`.
  - All 5 `pipeline=*` presets parse, apply exact tuples, and match `SettingsWindow` preset detection; `pipeline=rtxpt di=medium` applies `pipeline=` before `di=`.
  - Legacy aliases (`frame.indirectMode=nrc|restirGi|nrcRestirGi`, `di.reference=on/off`, `di.temporalOnly=on di.reference=off`, `di.spatialOnly=on/off`, `di=reference`, `frame=shipped`) and all 57 `tests/lab/*.lab` files + 5 built-in plans parse cleanly.
  - Every `settingsRows::kAll` entry exists in `labSwitches()`, and every preset diff has a settings row.
- `layout_test`: `GpuTargets` (220 B) and `GpuFrameRoot` C++/Slang layout assertions pass.

### 8.2 GPU Correctness & Convergence Gates (`windows-clang-release-checks`)

| Gate | Command / Config | Pass Criterion |
|---|---|---|
| **GI & NRC convergence + A/B vs baseline** | `ENGINE_GI_CHECK=1` (Sponza + Bistro café) + A/B vs pre-M8.3 build | `14/14` pass; `RawPt + Nrc` within check bounds and ≤ baseline bias (no LogLuv quantization loss); re-baseline NRC stability/guide shots if drift is strictly from LogLuv removal |
| **DI & Guide capture + RT check** | `ENGINE_GUIDE_CAPTURE=1 ENGINE_RT_CHECK=1` | All DI convergence, glass throughput, normal, and RT primary checks pass |
| **Streaming RIS parity (independent shader)** | `ENGINE_CONFIG="frame.directMode=streamingRis di.sampling=power_ris di.localSamples=32 di.infiniteSamples=8 di.brdfSamples=0 di.exactInfinite=0"` vs independent 2-pass `di=reference` (accumulated 256 frames, Sponza gallery + courtyard + Bistro café) | Mean luminance & per-channel error within Monte Carlo variance (< 0.5%) |
| **Raw 1-Bounce PT + Cache Off parity** | `frame.indirectBounce=rawPt frame.radianceCache=off` vs `frame.indirectBounce=restirGi frame.radianceCache=off` with `isGiReference=1` (accumulated 256 frames) | Matches 1-bounce unbiased PT (< 0.5%) |
| **AO isolation check** | `frame.indirectBounce=rawPt frame.radianceCache=off` vs `frame.radianceCache=nrc` on a high-AO-contrast Sponza pose | Bounce-1 traced radiance (`Cache=Off`) is identical regardless of primary AO; AO darkens only the NRC cache contribution |
| **DLSS-RR, Cull, Motion checks** | `ENGINE_DLSS_CHECK=1`, `ENGINE_CULL_CHECK=1`, `ENGINE_MOTION_TEST=1` | `11/11` DLSS, `16/16` cull, `13/13` motion pass |

### 8.3 Zero-Cost Timing & Lazy VRAM Verification (Release, No Layers, 2560×1440 Quality, ABBA)

Because skipped `GpuTimerScope` intervals always read ~0 ms even if hidden barriers or stalls leak into neighboring scopes, every zero-cost claim is gated on **ABBA total GPU frame time and `RayStartWait`** plus VMA allocation/retirement logs:

| Config (`ENGINE_CONFIG`) | Required ABBA GPU Time Proof | Required VRAM / Log Proof |
|---|---|---|
| `frame.directMode=off` | Total GPU time drops by full baseline `RtxdiInitial + RtxdiTemporal + RtxdiShade`; `RayStartWait` does not regress | `RtxdiReservoirs` retired after `MAX_FRAMES_IN_FLIGHT` frames |
| `frame.directMode=streamingRis` | Total GPU time beats 2-pass `di=reference` / no-reuse `initial + shade` (`PERF-C1`) | `RtxdiReservoirs` not allocated / retired |
| `frame.indirectBounce=off` | Total GPU time drops by full baseline `NrcUpdate + Indirect + NrcTrain + NrcResolve + GiTemporal + GiShade` | `RtxdiGiReservoirs`, `giStagnancy`, `giSampleIds`, `giDuplication`, `rawIndirectWeight` retired |
| `frame.indirectBounce=rawPt frame.radianceCache=off` | Total GPU time drops by full baseline `NrcUpdate + NrcTrain + NrcResolve + GiTemporal + GiShade` | Zero GI reservoirs & zero `rawIndirectWeight` allocated |
| `frame.indirectBounce=rawPt frame.radianceCache=nrc` | Total GPU time beats pre-M8.3 `IndirectMode::Nrc` by full removed `GiTemporal + GiShade` cost (`~0.17–0.27 ms`, `PERF-D1`) | `RtxdiGiReservoirs` + decorrelation targets retired; only 8 B/px `rawIndirectWeight` live (`PERF-D2`) |
| `frame.directMode=off frame.indirectBounce=off` (Sponza, no glass) | Total GPU time matches vis-buffer + resolve + combine + DLSS-RR/Post only; async light-prep `totalMs == 0` (no stale timestamps) | Zero DI/GI/raw-weight buffers live |

### 8.4 Vulkan Validation Matrix (`windows-clang-release-checks`)

1. **Core + Synchronization + Best Practices + NVIDIA Best Practices** (`VK_KHRONOS_VALIDATION_REPORT_FLAGS=error,warn,perf`):
   - Static runs on all 5 presets (`pipeline=targetRis`, `targetTemporal`, `rtxpt`, `rtxdi`, `rtxgi`) plus `frame.directMode=off`, `frame.indirectBounce=off`, `frame.radianceCache=off`.
   - **Live toggle stress test** cycling through `Direct: Off → StreamingRis → RestirDi (Temporal / Spatial / TemporalAndSpatial / Reference)` and `Indirect/Cache: Off → RawPt+Off → RawPt+Nrc → RestirGi+Nrc → RestirGi+Off → Off` across frames in one session, plus toggling `DebugView::RtxdiReservoirs` / `IndirectSamples` / `DirectLighting` / `IndirectRadiance` / `NrcCache` / `SpecularHitDistance` while stages are off: **0 new validation errors, 0 sync hazards, 0 VMA leak assertions at exit**.
2. **GPU-AV** (default no-DGC path, `windows-clang-release-checks`):
   - Run static + live toggle transitions across `Off` / `StreamingRis` / `RawPt` / `RestirGi` to verify lazily allocated/retired BDA buffers (`reservoirs`, `giReservoirs`) and `kNoneIndex` descriptor heap slots produce **0 GPU-AV out-of-bounds or invalid-descriptor errors**.

---

## 9. Verification Results (2026-11-26)

Hardware: NVIDIA GeForce RTX 5070 · Builds: `windows-clang-release` and `windows-clang-release-checks` (Clang 23.1.5, zero errors).

### 9.1 CPU Unit Tests (`ctest`)
- `windows-clang-release`: **7/7 passed** (`hiz_footprint`, `brdf`, `lights`, `rtxdi_math`, `layout`, `nrc_library`, `dlss`).
- `windows-clang-release-checks`: **8/8 passed** (adds `lab_registry`: all 57 `.lab` files + 5 built-in plans, 4 new switches, 5 `pipeline=*` presets, `frame.radianceCache=sharc` rejected as `isPlanned`, legacy alias semantics including `di.temporalOnly=on di.reference=off`, `Compare with` closed-section counts, and `GpuTargets == 220 B` in `layout`).

### 9.2 GPU Self-Tests & Parity Lab (`windows-clang-release-checks`, layers off)
- `ENGINE_CULL_CHECK=1`, `ENGINE_MOTION_TEST=1`, `ENGINE_DLSS_CHECK=1`, `ENGINE_GI_CHECK=1` (`RawPt + Nrc` `14/14`), `ENGINE_GUIDE_CAPTURE=1 ENGINE_RT_CHECK=1`: **all passed (`rc=0`)**.
- **Fused Streaming RIS parity** (`Power_RIS`, 32 local / 8 inf / 0 BRDF, `di.exactInfinite=0`) vs independent 2-pass `di=reference` (256-frame accumulation): `sponza.gallery` bias **`0.000%`**, `sponza.courtyard` **`0.000%`**, `cafe.tables` **`-0.070%`** (noise floor `0.041%`, `darkLeak = 0`).
- **Raw 1-Bounce PT + Cache Off parity** vs 1-bounce unbiased `RestirGi` ground truth: `sponza.gallery` `1b.bias` **`-0.093%`** (floor `-0.078%`), `sponza.courtyard` **`+0.046%` / `+0.092%`** (floor `+0.081%`), `cafe.tables` **`-0.089%`** (floor `+0.223%`), `darkLeak = 0`.

### 9.3 Vulkan Validation & GPU-AV (`windows-clang-release-checks`)
- **Core + Synchronization + Best Practices + NVIDIA (`REPORT_FLAGS=error,warn,perf`)**: **10/10 clean (`rc=0, unexpected=0`)** across `targetRis`, `targetTemporal`, `rtxpt`, `rtxdi`, `rtxgi`, `directOff`, `indirectOff`, `cacheOff`, `bothOff`, and 200-frame live mode cycling.
- **GPU-AV** (default no-DGC path): **4/4 clean (`rc=0, unexpected=0`)** across `targetRis`, `targetTemporal`, `bothOff`, and 140-frame live cycling.

### 9.4 ABBA Release Timing & Lazy VRAM Gates (2560×1440 Quality / 1707×960 render, Sponza, layers off, `ENGINE_BENCH_FRAMES=600`)

| Gate / Config | GPU Total (ms) | `RayStartWait` (ms) | Key Pass Scopes (ms) | Lazy Allocations / Async Light Prep |
|---|---:|---:|---|---|
| **ABBA `PERF-C1` A** (`di=reference`, `indirect=off`) | **5.701** (`5.716 / 5.687`) | `0.015` | DI `0.461 + 0.000 + 0.436 = 0.897` | DI reservoirs `112.9 MiB`, GI `0 MiB` |
| **ABBA `PERF-C1` B** (fused `streamingRis`, 32/8/0, `indirect=off`) | **5.409** (`5.373 / 5.445`) | `0.013` | DI `0.000 + 0.000 + 0.602 = 0.602` (**-0.295 ms / -32.9% DI**, **-0.292 ms total**) | DI `0 MiB`, GI `0 MiB` |
| **ABBA `PERF-D1` A** (`targetRis` + `restirGi + nrc`) | **11.280** (`11.269 / 11.291`) | `0.015` | `NrcResolve 0.252`, `GiTemporal 0.453`, `GiShade 1.465` | GI reservoirs `150.5 MiB` + decorrelation targets |
| **ABBA `PERF-D1` B** (`targetRis` + `rawPt + nrc`) | **9.323** (`9.383 / 9.263`) | `0.015` | `NrcResolve 0.144` (**-0.109 ms**), `GI 0.000` (**-1.957 ms total**) | GI `0 MiB`; `rawIndirectWeight` RGBA16F (`12.5 MiB`) only |
| `frame.directMode=off` (`rawPt + nrc`) | **8.871** (`-0.452`) | `0.014` | `DI 0.000/0.000/0.000` | DI `0 MiB` |
| `frame.indirectBounce=off` (`targetRis` DI) | **5.313** (`-4.010`) | `0.011` | `NRC/Ind/GI ~0.000` | GI `0 MiB`, `rawIndirectWeight 0 MiB`, NRC unconfigured |
| `frame.indirectBounce=rawPt frame.radianceCache=off` | **7.480** (`-1.843`) | `0.012` | `Indirect 2.349` (`-0.543` vs NRC), `NRC/GI ~0.000` | GI `0 MiB`, `rawIndirectWeight 0 MiB`, NRC unconfigured |
| `frame.directMode=off frame.indirectBounce=off` | **4.782** | `0.012` | All DI/NRC/Ind/GI **`0.000`**, `Combine 0.019`, `DLSS-RR 4.170` | **Async light prep `0.0000 ms`**; `0 MiB` DI/GI/NRC/raw-weight |
| `pipeline=targetTemporal` | **9.473** | `0.013` | `DI 0.000/0.000/0.430` (exact sun on Sponza), `Ind 2.867`, `GI 0.000` | DI `112.9 MiB`, GI `0 MiB`, NRC `140.3 MiB` |
| `pipeline=rtxpt` | **7.577** | `0.012` | `DI 0.42`, `Ind 2.35`, `NRC/GI 0.000` (1-bounce PT until M8.4 SHaRC) | DI `0 MiB`, GI `0 MiB`, NRC `0 MiB`, `rawIndirectWeight 0 MiB` |
| `pipeline=rtxgi` | **9.398** | `0.011` | `DI 0.423`, `Ind 2.857`, `GI 0.000` | DI `0 MiB`, GI `0 MiB`, NRC `140.3 MiB` |
| `pipeline=rtxdi` | **11.886** | `0.012` | `DI 0.406/0.326/1.294`, `Ind 2.916`, `GI 0.450/1.433` | DI `112.9 MiB`, GI `150.5 MiB`, NRC `0 MiB` |
| Live mode cycling (`ENGINE_INDIRECT_MODE=cycle`) | — | — | Live mid-session allocate + retire of `RtxdiGiReservoirs` (`150.5 MiB`) × 2 with zero `waitIdle` | `rc=0`, 0 validation / GPU-AV / VMA leak errors |
