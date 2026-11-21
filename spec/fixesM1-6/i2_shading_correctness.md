# M8 · I2: shading correctness

**Status:** built 2026-11-21 · **Milestone:** M8 ([README](./README.md))
**Implements:** findings F20 (bisect + fix), F13, F14, F15, F16, F17, F18, F19 ([m8_optimization_and_fixes.md](./m8_optimization_and_fixes.md) §4)
**Depends on:** I1 check runners ([i1_check_runners.md](./i1_check_runners.md))
**References:**
- `reference/RTXDI/Samples/FullSample/Shaders/LightingPasses/ShadeSecondarySurfaces.hlsl` and `LightingPasses.cpp:54-75` (`enableInitialVisibility = false`, F18)
- `reference/RTXDI/Samples/FullSample/Source/RtxdiResources.cpp:209-214`, `RAB_Buffers.hlsli:111-112`, `Doc/ShaderAPI.md:30-36` (separate `LowDiscrepancy` GI and `StratifiedLowDiscrepancy` DI neighbor offset buffers, F16)
- `ThirdParty/_deps/src/rtxdi/Include/Rtxdi/GI/{Reservoir,TemporalResampling,SpatialResampling}.hlsli`, `PT/Reservoir.hlsli`, `Utils/BoilingFilter.hlsli` (F13, F14, F15, F20)
- `reference/vk_denoise_dlssrr/dlss_rr/src/dlssrr_wrapper.cpp:379-385`, `reference/vk_gltf_renderer/src/dlss_wrapper.cpp:571-575`, `reference/vk_lod_clusters/src/dlss_wrapper.cpp:504-507`, `nvsdk_ngx_helpers_dlssd_vk.h:106-107,357-358` (DLSS-RR matrix layout, F17)
- `reference/RTXGI/Samples/IndirectDiffuse/Shaders/Pathtracer.hlsl:420-424,468-471` (`shadingNormal` hemisphere alignment with `geometryNormal`, F19 / ND9)

## Scope

One change per finding, measured in order with `ENGINE_GI_CHECK` and `ENGINE_STABILITY_CHECK` (plus `ENGINE_SCREEN_EDGE_CHECK` for F17 and ABBA `ENGINE_BENCH_FRAMES` for F18). Every change that does not improve or hold correctness/stability is reverted and closed with its numbers.

| Finding | Target change | Reference / Verification |
|---|---|---|
| F20 | Bisect static-courtyard `Lit` bright blotches (24.7% std / 22.6% swing on shipped; 1.3% / 0.16% with NVIDIA's age limit) to separate GI reservoir/Jacobian behavior under jitter from DLSS-RR | `ENGINE_STABILITY_CHECK=1` with `ENGINE_JITTER=1` (jittered `Indirect radiance`, no DLSS-RR) and `ENGINE_JITTER=0` (unjittered `Lit`, with DLSS-RR), then targeted bisect |
| F13 | Store the far miss placeholder `(origin + dir * kMissDistance, -dir)` for zero-radiance GI hits so dark reservoirs do not fail Jacobian validation and drop `M` from `piSum`; with NRC active, apply the placeholder in `nrc_resolve.slang` when radiance remains zero after resolve | `indirect.slang`, `nrc_resolve.slang`; GI check + stability |
| F14 | Gate O11 `stagnancy` on `reservoir.weightSum > 0.0` (matching `duplicationSampleId` and PT's `<` behavior) and gate `fireflyReplacementBiasReduction` on `reservoir.weightSum > 0.0` so zero-weight reservoirs never zero a valid `initial` hit; re-measure the swap bound | `rtxdi_gi_shade.slang`; GI check (with and without swap bound) + stability |
| F15 | Compare RTXDI's `RTXDI_BoilingFilterInternal` (averages over `count(weight > 0)`, blind when `k <= M`) against averaging over all in-bounds lanes in an 8×8 tile, for both temporal boiling and O11 firefly replacement; keep NVIDIA's filter callable | `rtxdi_gi_temporal.slang`, `rtxdi_gi_shade.slang`; GI check + stability |
| F16 | Give DI its own `StratifiedLowDiscrepancy` neighbor table (`RTXDI_DI_NEIGHBOR_OFFSETS_BUFFER`) and fill `RTXDI_NEIGHBOR_OFFSETS_BUFFER` with `LowDiscrepancy` for GI spatial reuse | `vk_rtxdi.{hpp,cpp}`, `rtxdi_constants.slang`, `rtxdi_bridge.slang`, `layout_test.cpp`; GI check + stability |
| F17 | A/B removing `glm::transpose` from `pInWorldToViewMatrix` and `pInViewToClipMatrix` in `Dlss::evaluate` | `vk_dlss.cpp`; stability + screen-edge runners on gallery and courtyard `Lit` |
| F18 | Clear `sampling.enableInitialVisibility = 0` in `sampleLightsAt` to match `LightingPasses.cpp:64` (`ShadeSecondarySurfaces.hlsl`), removing the duplicate `rasterRayOrigin` visibility ray on secondary/training hits | `path_shading.slang`; GI check + ABBA bench |
| F19 | Flip `result.n` into `surface.geoNormal`'s hemisphere when `dot(surface.geoNormal, result.n) < 0.0` in `sampleMaterial` | `pbr.slang`; `ENGINE_GUIDE_CAPTURE`, GI check, stability |

## Decisions

| Decision | Rule | Why |
|---|---|---|
| Order | Bisect F20 before landing F13–F19 | F20's 24.7% courtyard swing masks every other stability delta on `shipped` |
| NVIDIA sets stay callable | `nvidiaGiSettings()` and `nvidiaSettings()` keep NVIDIA's parameters; any new filter mode stays selectable | M8 rule (C-M4-2) |
| Exception: bridge fixes apply to every set | F20's `RAB_ValidateGISampleWithJacobian` (reject outside [1/3, 3], no clamp), F13's far miss placeholder and F19's normal flip are bridge / material code, not settings: the `Nvidia` set runs them too and no longer reproduces the RTXDI sample's accept-[0.1, 10]-then-clamp Jacobian. `Nvidia` numbers from before I2 are not comparable with later ones | They are correctness fixes (F20 pumped reservoirs to +inf under jitter in every set without an age limit); a switch back to the clamp would only keep a known bias callable |
| Zero-radiance NRC resolve path | `indirect.slang` keeps the true `X2` position/normal when an NRC query is emitted; `nrc_resolve.slang` rewrites to the far placeholder along `normalize(reservoir.position - primary.position)` only when `all(cached <= 0.0)` and `all(reservoir.radiance <= 0.0)` | Preserves true `X2` when NRC supplies nonzero indirect radiance while preventing Jacobian drops when both direct and cached radiance are zero |
| Separate DI/GI neighbor tables | `GpuRtxdiConstants::neighborOffsets` holds `LowDiscrepancy` (for `RTXDI_NEIGHBOR_OFFSETS_BUFFER`, used by GI); `GpuRtxdiConstants::diNeighborOffsets` holds `StratifiedLowDiscrepancy` (for `RTXDI_DI_NEIGHBOR_OFFSETS_BUFFER`, used by DI) | Matches `RTXDI/Doc/ShaderAPI.md:30-36` and `FullSample/RtxdiResources.cpp:209-214` with no vendor header edits |

## Results

RTX 5070, release, no layers, 1280×720 window (853×480 render, DLSS-RR Quality), 2026-11-21. Every finding was landed and measured one at a time in order (`ENGINE_GI_CHECK=1`, `ENGINE_STABILITY_CHECK=1`).

### 1. F20 bisect and fix (`shaders/lighting/rtxdi_bridge.slang`)

- **Bisect:** Running `ENGINE_STABILITY_CHECK=1` with `ENGINE_JITTER=1` (forcing camera jitter on raw `Indirect radiance`, without DLSS-RR) reproduced F20 directly inside ReSTIR GI: `shipped` failed with **64 non-finite px**, `shipped without decorrelation` **232 non-finite px**, `shipped without firefly replacement` **22 non-finite px**, and `converged` exploded from mean `0.0046` to `1.1757` (255×, swing `112.66%`). Every set with `maxReservoirAge = 30` had 0 non-finite pixels.
- **Root cause:** `RAB_ValidateGISampleWithJacobian` copied NVIDIA's `RAB_SpatialHelpers.hlsli:40-45`, accepting `[0.1, 10]` and clamping `jacobian = clamp(jacobian, 1/3, 3)`. Clamping breaks multi-hop Jacobian telescoping around spatial/temporal reuse cycles (`J(A→B) · J(B→C) · J(C→A) > 1` whenever a hop in `[0.1, 1/3)` is clamped up to `1/3` while return hops `≤ 3` are unclamped). Under subpixel jitter with unlimited sample age, surviving reservoir `weightSum` compounds exponentially until RGBA16F overflow (`> 65504 → +Inf`), which DLSS-RR turns into growing bright blotches.
- **Variants:**
  - Unclamped `[0.1, 10]`: eliminated F20 (`0` non-finite px, courtyard `shipped` `Lit` `1.29% / 8.06% / 0.16%`), but failed GI check 6 of 8 (courtyard converged traced `+2.25%` > `1.5%`, one-bounce `+1.55%` > `1.5%`) because `3×–10×` shifts entered `GI/SpatialResampling.hlsli` loop 2's non-Jacobian MIS denominator.
  - **Shipped fix — unclamped symmetric `[1/3, 3]` rejection** (`return jacobian <= 3.0 && jacobian >= 1.0 / 3.0;`): keeps telescoping exact and drops both F20 and the converged GI bias.
- **Effect:** Courtyard `shipped` `Lit` fell from **`24.65% / 91.0% / 22.57%`** to **`1.28% / 7.76% / 0.14%`**; courtyard `converged` `Lit` fell from `4.55% / 32.2% / 5.54%` to `1.49% / 15.99% / 0.16%`. GI check passed 8 of 8: courtyard converged traced fell from `+0.97…+1.25%` to `+0.81%`, one-bounce converged dropped to `+0.00%` (gallery) / `+0.07%` (courtyard).

### 2. Per-finding progression (cumulative)

| Step | Change | GI check (8/8): converged 1-bounce (G / C) | GI check: shipped vs 1-bounce (G / C, tile p90) | `Lit` stability: shipped (G / C: std / flicker / swing) |
|---|---|---|---|---|
| I1 baseline | — | −0.31…+0.27% / +0.51…+0.85% | −0.75% (21.4%) / −0.86% (27.4%) | 6.14% / 48.7% / 1.16% · **24.65% / 91.0% / 22.57%** |
| **F20** | Unclamped `[1/3, 3]` Jacobian reject (`rtxdi_bridge.slang`) | +0.00% / +0.07% | −1.02% (19.8%) / −1.30% (30.2%) | 6.11% / 46.8% / 0.98% · **1.28% / 7.8% / 0.14%** |
| **F13** | Far miss placeholder on zero-radiance hits (`indirect.slang`, `nrc_resolve.slang`) | +0.03% / +0.02% | −1.13% (18.4%) / −1.43% (30.6%) | 6.14% / 49.1% / 1.34% · 1.28% / 8.2% / 0.14% |
| **F14** | Zero-weight guard on `stagnancy` & swap bound (`rtxdi_gi_shade.slang`) | +0.03% / +0.02% | −1.39% (18.4%) / −1.45% (30.7%) | 6.32% / 49.8% / 1.46% · 1.30% / 8.3% / 0.14% |
| **F15** | Full-tile 8×8 average on firefly replacement only (`rtxdi_gi_shade.slang`) | +0.03% / +0.02% | **−0.40%** (21.4%) / −1.45% (29.0%) | **5.90% / 46.1% / 1.28%** · 1.28% / 8.0% / 0.13% |
| **F16** | Separate `LowDiscrepancy` GI and `StratifiedLowDiscrepancy` DI neighbor tables | −0.11% (p90 11.4→9.8%) / +0.03% | −0.05% (21.4%) / −1.43% (28.8%) | 5.90% / 45.9% / 1.28% · 1.29% / 8.1% / 0.13% |
| **F17** | Remove `glm::transpose` on DLSS-RR `worldToView`/`viewToClip` (`vk_dlss.cpp`) | bit-identical | bit-identical | bit-identical (also 80/80 screen-edge steps bit-identical) |
| **F18** | `sampling.enableInitialVisibility = 0` in `sampleLightsAt` (`path_shading.slang`) | −0.11% / +0.01% | −0.15% (21.4%) / −1.35% (29.0%) | 5.92% / 45.9% / 1.14% · 1.29% / 8.1% / 0.16% |
| **F19** | Flip `result.n` into `surface.geoNormal` hemisphere (`pbr.slang`) | −0.11% (p90 9.6%) / +0.05% (9.5%) | −0.21% (21.7%) / −1.48% (29.6%) | **5.80% / 46.0% / 0.83%** · **1.30% / 8.3% / 0.15%** |

### 3. Finding-specific notes

- **F14 swap bound re-measurement:** Temporarily enabling `isFireflyReplacementBounded = true` after F14 gave GI check 8 of 8 PASS with shipped bias `-6.96%` gallery (−5.6 pp vs `-1.39%` off; improved from −7.6 pp before F14) and `-3.78%` courtyard (−2.3 pp vs `-1.45%` off; improved from −3.2 pp before F14, p90 `30.7% → 15.0%`, max `425% → 99%`). Kept off by default in `defaultGiSettings()` because one-sided clamping still darkens GI; I5 revisits it.
- **F15 temporal vs firefly split:** Averaging over all 64 in-bounds lanes helped O11's **unbiased firefly replacement** (which swaps to `initial`: gallery `Lit` `6.32% / 49.76% / 1.46% → 5.90% / 46.12% / 1.28%`, gallery GI bias `-1.39% → -0.40%`, max tile `486% → 305%`), but severely regressed the **discarding temporal boiling filter** (`rtxdi_gi_temporal.slang`, `shipped without DLSS-RR profile`): gallery tile p90 jumped `23.51% → 69.91%`, courtyard `18.87% → 37.73%`, because dividing by 64 flags isolated `k = 1` valid pixels in dark tiles (`W > (W/64) · 41`). `rtxdi_gi_temporal.slang` was reverted to NVIDIA's `RTXDI_GIBoilingFilter`; full-tile averaging stays on firefly replacement only (`GiFlag::FullTileBoilingFilter`, `Full-tile firefly test` in UI, off in `nvidiaGiSettings()` and `convergedGiSettings()`).
- **F16 NVIDIA set stability:** Giving GI its own `LowDiscrepancy` table also improved `NVIDIA` `Lit` stability: gallery `7.71% / 43.95% → 7.56% / 42.27%`, courtyard `1.43% / 14.36% → 1.42% / 13.81%`, and dropped gallery converged 1-bounce tile p90 from `11.44%` to `9.84%`.
- **F17 DLSS-RR matrices:** `ENGINE_STABILITY_CHECK` (28 steps) and `ENGINE_SCREEN_EDGE_CHECK` (80 steps, gallery + courtyard pan/dolly) were bit-for-bit identical with and without `glm::transpose`. Current NGX `DLUnified` ignores `pInWorldToViewMatrix` and `pInViewToClipMatrix`. The untransposed GLM layout is kept defensively so future NGX presets/versions receive valid row-major left-multiply matrices (`(M^T)^T = M`, matching `reference/vk_denoise_dlssrr/dlss_rr/src/dlssrr_wrapper.cpp:379-385`).
- **F18 cost (`ENGINE_BENCH_FRAMES=2400`, startup gallery, 2 runs before vs 2 after):** `Indirect` fell from `0.6066 / 0.5999 ms` (mean `0.603 ms`) to **`0.5769 / 0.5740 ms` (mean `0.575 ms`, −0.028 ms / −4.6%)**; `GPU total` fell from `3.586 ms` to `3.560 ms` (−0.026 ms).

### 4. Final post-I2 verification

- **GI check (`ENGINE_GI_CHECK=1`):** **8 of 8 passed.**
  - Gallery: traced converged `+0.50%` (p90 `3.58%`), 1-bounce converged `-0.11%` (p90 `9.57%`), shipped `-0.21%` (p90 `21.66%`).
  - Courtyard: traced converged `+0.80%` (p90 `6.24%`), 1-bounce converged `+0.05%` (p90 `9.53%`), shipped `-1.48%` (p90 `29.59%`).
- **Stability (`ENGINE_STABILITY_CHECK=1`, `Lit`, 480 static frames):** **28 of 28 passed, 0 failed.**
  - Gallery: `shipped` `5.80% / 46.02% / 0.83%` (I1: `6.14% / 48.7% / 1.16%`), `shipped + age limit` `5.13% / 21.14% / 1.08%`, `NVIDIA` `7.56% / 41.80% / 1.06%` (I1: `7.71% / 44.0% / 1.36%`), `converged` `8.37% / 58.57% / 0.94%`.
  - Courtyard: `shipped` `1.30% / 8.27% / 0.15%` (I1: `24.65% / 91.0% / 22.57%`), `shipped + age limit` `1.29% / 7.91% / 0.16%`, `NVIDIA` `1.41% / 13.68% / 0.15%` (I1: `1.44% / 14.6% / 0.15%`), `converged` `1.48% / 15.75% / 0.17%` (I1: `4.55% / 32.2% / 5.54%`).
- **Light change (`ENGINE_LIGHT_CHANGE_CHECK=1`, courtyard `Indirect radiance`):** **32 of 32 passed, 0 failed.**
  - Sun move (azimuth +30°): `shipped` **56 frames** (I1: `400`, error at 16: `+29.2%`, at 64: `+1.4%`), `shipped + age limit` `16` (`+3.5%`), `NVIDIA` `24` (I1: `32`), `converged` `32`.
  - Sun toggle (off → on): `shipped` `24`, `shipped + age limit` `8`, `NVIDIA` `0`, `converged` **`0`** (I1: `344`).
- **Regression suite:**
  - CTest: **7 of 7 passed** (`hiz_footprint`, `brdf`, `lights`, `rtxdi_math`, `layout`, `nrc_library`, `dlss`).
  - `ENGINE_RT_CHECK=1`: **32 of 32 frames** matched the vis buffer.
  - `ENGINE_CULL_CHECK=1`: **16 of 16 poses** identical.
  - `ENGINE_MOTION_TEST=1`: **13 of 13 scenarios** passed.
  - `ENGINE_GUIDE_CAPTURE=1`: **2 of 2 normal checks** and **3 of 3 DI convergence checks** passed (`LightsPunctualLamp` `+0.030%`, `Sponza_terminator` `-0.000%`, `EmissiveStrengthTest` `+0.063%`).
- **Vulkan validation** (core + sync + best practices + NVIDIA best practices, GPU-AV off per C-E1-3; `ENGINE_INDIRECT_MODE=cycle`, 120 frames): **clean** — only the known third-party NGX (`BestPractices-SpirvDeprecated_WorkgroupSize` ×3, `SYNC-HAZARD-WRITE-AFTER-WRITE` ×2, C-H1-2) and NRC (`VUID-vkDestroyDevice-device-05137`, 51 leaked objects, C-G3-3) messages.

## Done when

- [x] F20 bisected and root cause recorded (and fixed)
- [x] F13–F19 each implemented or closed with before/after GI-check and stability numbers
- [x] Every gated GI-check phase passes (8 of 8); shipped bias against one bounce recorded (−0.21% gallery / −1.48% courtyard)
- [x] Stability runner reads no worse than I1's baseline on both poses (improved on both)
- [x] Validation clean (core + sync + best practices; GPU-AV off per C-E1-3)
- [x] `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST`, `ENGINE_GUIDE_CAPTURE`, and CTest pass
