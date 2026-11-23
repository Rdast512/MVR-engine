# M8 · I14: Bistro sampling, reuse, DLSS-RR and rays

**Status:** built 2026-11-23 (Phases 1, 2, 4 landed; Phase 3 measured, NVIDIA values kept); regression green except the gallery `NRC mode` gate, which is flaky before and after I14 (§8.3, C-I14-5) · **Milestone:** M8 ([README](./README.md))
**Implements:** findings F24–F27 ([m8_optimization_and_fixes.md](./m8_optimization_and_fixes.md) §4) from the 2026-11-22 four-path Bistro audit; removes correctness defects before I12's final Bistro tuning of F22 (café DI confetti) and F23 (café GI dim-tile excess)
**Depends on:** I1 (check runners), I2 (shading correctness), I6 (DI defaults and `Power_RIS` baseline)
**Considerations:** opens C-I14-1 … C-I14-4 (§7); updates `docs/settings.md` if any user-visible setting changes
**References:**
- `ThirdParty/_deps/src/rtxdi/Include/Rtxdi/{DI,GI}/{Initial,Temporal,Spatial}Resampling.hlsli` and `Utils/Math.hlsli` (read-only; no vendor header edits)
- `reference/RTXDI/Samples/FullSample/Shaders/RAB_Surface.hlsli:230` (geometric hemisphere reject), `RAB_LightSampling.hlsli`, `PrepareLights.hlsl`, `LightSampling.hlsli`
- `reference/RTXGI/Samples/IndirectDiffuse/Shaders/Pathtracer.hlsl:832` (off-screen behind-camera motion sentinel)
- `reference/vk_denoise_dlssrr/dlss_rr/shaders/primary_rgen.slang` (`EnvBRDFApprox2` with `f0` guard) and `dlssrr_wrapper.cpp` (DLSS-RR input packing and I2 F17 matrix check)
- `reference/vk_gltf_renderer/docs/denoising.md` (DLSS-RR guide contracts)
- Bistro empirical audit (`models/bistro/bistro.gltf` + `bistro.bin`): 76,248 emissive triangles across 21 materials (58,743 in café bounds); 254 materials (207 normal-mapped = 81.5%, 234 `KHR_materials_pbrSpecularGlossiness` = 92.1%); 1,753,630 unique triangles (16 zero-area, 1,365,242 sub-millimeter `< 5e-7 m²`)

## Scope

Four code paths audited at Bistro scale after the café runners exposed F22 (`Lit` temporal std 26.85% / 87% flicker on shipped DI) and F23 (`+22.9% / +18.0%` GI dim-tile excess). Executed in four phases so unconditional ray and denoiser-input fixes land first, followed by similarity tests and Bistro-scale emitter PDF accuracy, before I12 tunes F22/F23 on top of clean inputs.

| Phase | Finding | Change | Files |
|---|---|---|---|
| 1 · Ray origins & hemisphere | F24 | Reject `dot(surface.geoNormal, sample.l) <= 0` in `shadeLightSample`; remove `side = -1` opaque punch-through in `rasterRayOrigin` and `safeSpawnPoint`; guard `rsqrt` on zero-area triangles in `safeSpawnPoint`; add `dot(surface.geoNormal, brdf.l) > 0` in `RAB_SurfaceImportanceSampleBrdf` and pass an offset origin with `TMin = 0` into `RAB_TraceRayForLocalLight`; compute local-light and GI-reconnection visibility from the offset origin to a bounded endpoint margin instead of `distance * (1 - 1e-3)` | `shaders/lighting/rtxdi_bridge.slang`, `shaders/geometry/hit.slang`, `shaders/lighting/path_shading.slang`, `src/render/vk_rtxdi.cpp` |
| 2 · DLSS-RR inputs & check | F25 | Off-screen motion sentinel (`-2.0 * targetSize`) when `previousClip.w <= 0`; jitter-match `resolveBackground`; replace Karis `envBrdfApprox` with `EnvBRDFApprox2` (`f0 == 0` guard) and A/B `SpecAlbedo` `RGBA16F` vs `RGBA8_UNORM`; fix 1-frame startup `isDlssPresented()` ordering; add automated DLSS-RR input & history-trust check (`ENGINE_DLSS_CHECK`) | `shaders/passes/resolve.slang`, `shaders/material/brdf.slang`, `src/render/vk_renderer.cpp`, `src/render/vk_render_targets.cpp`, `src/render/vk_dlss.cpp`, `src/core/engine_temporal_check.*` |
| 3 · Reuse similarity tests | F26 | A/B `RAB_GetSurfaceNormal` returning `surface.geoNormal` vs `surface.normal` with tightened `normalThreshold`; scale spatial `samplingRadius` with render resolution, tighten `depthThreshold`, and A/B a same-frame plane-distance guard in `RAB_AreMaterialsSimilar`; A/B `isFallbackSamplingEnabled = false` on GI and DI; tighten `RAB_AreMaterialsSimilar` (relative/RGB diffuse albedo, dielectric-vs-metal F0, additive + relative roughness floor) | `shaders/lighting/rtxdi_bridge.slang`, `src/render/vk_rtxdi.cpp`, `src/render/vk_rtxdi.hpp`, `src/ui/engine_ui.cpp` |
| 4 · Bistro light PDF & emitters | F27 | Add independent Bistro emissive PDF/radiance check (`areTriangleLightsSampled = false` / direct textured emission reference on `bistro_cafe`); fix textured emitter flux estimation & zero-power dropout (mip-clamped multi-tap `light_world` + load/build culling of zero-flux triangles + A/B exact barycentric `sampleEmissive` evaluation during shading); evaluate scaled `Power_RIS` tiles (`localLightRISBufferParams`) vs tuned `ReGIR_RIS` for Bistro's exterior-dominated flux (`LMBR_000018b_bulb` ≥ 46.3% upper-bound, 0 triangles in café) and secondary-bounce NEE | `shaders/passes/light_world.slang`, `shaders/lighting/triangle_lights.slang`, `shaders/lighting/rtxdi_bridge.slang`, `shaders/lighting/path_shading.slang`, `src/render/vk_rtxdi.cpp`, `src/render/vk_world_lights.*`, `src/core/engine_guide_capture.*` |

## Decisions

| Decision | Rule | Why |
|---|---|---|
| No vendor header edits | All RTXDI 3.2 hooks stay in `rtxdi_bridge.slang` and `vk_rtxdi.cpp`; `ThirdParty/` is never modified | Engine rule; keeps RTXDI upgrades clean |
| Phase order: 1 → 2 → 3 → 4 → I12 | Ray/hemisphere fixes and DLSS-RR verification land before tuning reuse similarity or light sampling | Light leaking through walls or a denoiser distrusting its history distorts every noise and bias reading |
| NVIDIA sets stay callable | `RtxdiContext::nvidiaSettings()` and `nvidiaGiSettings()` keep NVIDIA's parameter values; only unconditional bug fixes (Phase 1, Phase 2) affect them | M8 rule (C-M4-2) |
| One change, one measurement | Every sub-change in Phases 1–4 is measured individually on Sponza and `bistro_cafe` before combining | Prevents regressions from hiding behind wins, especially across F22 (DI stability) and F23 (GI dim-tile bias) |
| No transmission in Phase 1 | Opaque surfaces never offset to the back side (`side = -1` removed); thin-walled glass stays on I13's separate ray mask (`kRayMaskGlass`) | Removing opaque punch-through does not conflict with I13 |
| No dynamic exposure feedback to DLSS-RR | `InPreExposure` remains uncoupled from auto-exposure (F10); any pre-exposure A/B in Phase 2 uses a static scene constant only | Avoids re-introducing F10's exposure-denoiser boiling loop (C-H2-1) |
| Keep I2 F17 matrix layout | `pInWorldToViewMatrix` and `pInViewToClipMatrix` stay untransposed GLM layout; Phase 2 verifies `proj[1][1]` Y-flip sign on moving poses | I2 F17 already proved current `DLUnified` ignores both matrices on Sponza; Phase 2 only verifies moving-camera and depth behavior |

## 1. Phase 1 · Ray-origin offsets & geometric hemisphere guards (F24)

All four items in Phase 1 are unconditional correctness fixes with zero or negligible GPU cost.

### 1.1 Geometric hemisphere guard and opaque ray-origin sign
- **`shadeLightSample` (`shaders/lighting/rtxdi_bridge.slang:487-501`):** add `dot(surface.geoNormal, sample.l) <= 0.0` alongside `nDotL <= 0.0`, matching `shadeGiSample` (`rtxdi_bridge.slang:625`) and `reference/RTXDI/.../RAB_Surface.hlsli:230`. On Bistro's 207 normal-mapped materials (81.5%), normal maps tilt `surface.normal` past the geometric plane; without this guard, a light behind a single-sided wall or floor gets nonzero target PDF and shades through the surface.
- **`rasterRayOrigin` (`rtxdi_bridge.slang:517-522`) and `safeSpawnPoint` (`shaders/geometry/hit.slang:176-178`):** remove the `side = dot(geoNormal, direction) < 0.0 ? -1.0 : 1.0` flip for opaque surfaces. Both `surface.geoNormal` (raster) and `worldNormal` (traced hit) must be oriented toward the incoming view/incident ray so every opaque ray spawns strictly on the front side of the triangle, never pushed through thin walls, doorframes, or floor/wall junctions.
- **Degenerate-triangle NaN guard (`hit.slang:159-161`):** guard `dot(worldNormal, worldNormal)` with `kMinGeoNormalLengthSq = 1e-30` (matching `surface.slang:100,134`) before `rsqrt`, returning `worldPos` on degenerate hits. Bistro has 16 zero-area triangles and 1,365,242 sub-millimeter triangles (`area < 5e-7 m²`).

### 1.2 BRDF candidate ray hemisphere and origin offset
- **`RAB_SurfaceImportanceSampleBrdf` (`rtxdi_bridge.slang:318-325`):** reject directions with `dot(surface.geoNormal, brdf.l) <= 0.0`, matching `scatterBrdf` (`path_shading.slang:118`).
- **`RAB_TraceRayForLocalLight` (`rtxdi_bridge.slang:561-598`):** `RTXDI_SampleBrdf` (`InitialSampling.hlsli:522-528`) passes raw `RAB_GetSurfaceWorldPos(surface)` and `brdfRayMinT = 0.001` (1 mm). At grazing angles (`cos θ < 0.1`), 1 mm along the ray leaves `< 0.1 mm` normal clearance (self-hit); at junctions and thin lamp cages, 1 mm skips `< 1 mm` occluders. Without modifying RTXDI headers:
  - In `RAB_SurfaceImportanceSampleBrdf`, stash `gRtxdiBridge.brdfRayOrigin = rasterRayOrigin(surface, direction)` (each `RTXDI_SampleBrdf` iteration calls `RAB_SurfaceImportanceSampleBrdf` immediately before `RAB_TraceRayForLocalLight`, while leaving `RAB_GetSurfaceWorldPos` untouched for shading and Jacobians).
  - In `RAB_TraceRayForLocalLight`, trace from `gRtxdiBridge.brdfRayOrigin` with `TMin = 0.0` (and set `initialSampling.brdfRayMinT = 0.0f` in `vk_rtxdi.cpp`).

### 1.3 Offset-origin visibility segment and bounded endpoint margin
- Today `rasterRayOrigin` shifts the origin by `s = (1 / 65536) * max(|p|, viewDepth)` (~0.15–0.31 mm in the café), while `sample.l` and `sample.distance` are computed from un-offset `surface.position` and shortened by `distance * (1 - 1e-3)` (`kLightSampleShortening` at `:347`, `kReconnectionShortening` at `:647`, and `shadeLightReservoir` in `path_shading.slang:54`).
  - **Close range (`d < ~25–30 cm`):** `1e-3 * d < s * dot(geoNormal, l)` causes the ray to overshoot target `Q` and self-occlude on `Q`'s own triangle (wall sconces mounted on walls, corners, niches, table undersides).
  - **Far range (`d = 15 m`):** `1e-3 * d = 1.5 cm` stops short of thin opaque lamp cages, frames, and mullions right in front of an emitter or `X2`.
- **Fix:** add a shared visibility helper `traceSegmentVisibility(float3 origin, float3 target, bool isAreaTarget)` used by `RAB_GetConservativeVisibility`, `RAB_GetTemporalConservativeVisibility`, `isReconnectionVisible`, and `shadeLightReservoir`:
  - Compute `toTarget = target - origin` from the **already-offset origin** `origin` (`rasterRayOrigin` or `safeSpawnPoint`), `dist = length(toTarget)`, `dir = toTarget / dist`.
  - For delta lights (point/spot), trace `tMax = dist` (no target triangle to self-hit); for infinite lights, trace `sample.l` to `kInfiniteDistance`.
  - For area targets (triangle lights and GI `X2`), subtract a **bounded** endpoint margin `endMargin = clamp(dist * 1e-4, kMinEndOffset, kMaxEndOffset)` (starting values: `kMinEndOffset = 5e-4` m, `kMaxEndOffset = 2e-3` m, or offset `target` along the target's known face normal when available) and require `tMax = dist - endMargin > 0.0`.
- **Verification on Bistro junctions:** verify `kRasterOriginScale = 1/65536` against vis-buffer barycentric reconstruction error on Bistro wall/floor junctions and grazing surfaces; record self-hit / dark-seam counts before and after on `bistro_cafe` and `Sponza_terminator`.

## 2. Phase 2 · DLSS-RR input fixes & history-trust check (F25)

Before tuning reuse or light sampling, confirm that DLSS-RR trusts its temporal history and receives clean guides on Bistro's materials and camera motion.

### 2.1 Guide & ordering fixes
- **Behind-previous-camera motion sentinel (`shaders/passes/resolve.slang:21-24`):** when `hasHistory(root)` is true and `previousClip.w <= 0.0` (a point that crossed behind last frame's camera plane), return an off-screen motion vector `float3(-2.0 * targetSize, 0.0)` instead of `float3(0.0, 0.0, 0.0)`, matching `reference/RTXGI/.../Pathtracer.hlsl:832`. Keep `(0, 0, 0)` only when `!hasHistory(root)` (camera cut / reset frame).
- **Background ray jitter (`resolve.slang:49`):** `resolveBackground` uses `float2(pixel) + 0.5` without subtracting `camera.jitterOffset`, whereas `resolveSurface` evaluates the rasterized (jittered) pixel. Verify and align background motion with `camera.jitterOffset` so sky/miss pixels do not carry a 1-pixel subpixel offset under camera rotation (`ENGINE_MOTION_TEST`).
- **Specular albedo `f0 == 0` guard (`shaders/material/brdf.slang:179-187`, `resolve.slang:84-85`):** Karis's `envBrdfApprox` returns nonzero `scaleBias.y` at grazing angles even when `f0 == 0` (Bistro has 234 `KHR_materials_pbrSpecularGlossiness` materials, 92.1%, many with near-zero `specularFactor`/texels). Replace with NVIDIA's `EnvBRDFApprox2` (`reference/vk_denoise_dlssrr`), scaling the additive bias by `saturate(f0.g * 50.0)` (or `saturate(max(f0.r, max(f0.g, f0.b)) * 50.0)`).
- **`SpecAlbedo` precision A/B (`src/render/vk_render_targets.cpp`):** A/B `kGuideSpecAlbedo` at `R16G16B16A16_SFLOAT` vs current `R8G8B8A8_UNORM` on `bistro_cafe` (dark low-F0 wood/stone surfaces quantize heavily in 8-bit linear). Keep `RGBA16F` only if it improves `Lit` stability or removes dark banding without measurable bandwidth regression.
- **Startup 1-frame ordering (`src/render/vk_renderer.cpp:211-213`):** verify that `camera.updateCameraData` and `targets.prepareFrame` read the post-prepare `isDlssPresented()` state on frame 0 / resize / mode toggle so the first jittered frame never mismatches DLSS-RR's jitter offset.

### 2.2 Automated DLSS-RR input & history-trust check (`ENGINE_DLSS_CHECK`)
Nothing currently verifies whether DLSS-RR actually trusts its history or agrees with the engine's jitter, normal, roughness, and depth conventions. Add a deterministic check runner (or mode of `engine_temporal_check`):
1. **Clean-input baseline (denoiser self-boiling test):** feed a temporally constant, noise-free synthetic radiance signal (e.g., direct albedo + smooth analytical shading, bypassing stochastic DI/GI) into DLSS-RR on a static and slowly panning pose (`bistro_cafe` and Sponza). Gate: static `Lit` temporal std `< 0.5%`, flickering px `< 1%`. Any boiling here isolates a DLSS-RR input/guide defect immediately.
2. **Perturbation controls (sign & convention verification):** on static + moving poses, run short paired sequences toggling each convention individually against the shipped encoding:
   - jitter sign (`+jitter` vs `-jitter` passed to NGX `InJitterOffsetX/Y`),
   - normal space/sign (world-space vs inverted/view-space),
   - roughness packing (linear vs squared),
   - depth polarity (reverse-Z `Depth_Type_HW` vs inverted) and `proj[1][1]` Y-flip sign on a moving camera.
   Assert that the shipped encoding strictly beats every inverted control on both static `Lit` temporal std and moving-camera edge error / smearing.
3. **Optional A/B — low-luminance café pre-exposure & specular hit distance:** once (1) and (2) pass, A/B a fixed static `InPreExposure` (e.g., `1.0` vs `8.0` / `16.0` on `bistro_cafe`, keeping F10's auto-exposure feedback loop broken) and wiring `pInSpecularHitDistance` (from GI hit distance on glossy pixels) to test whether DLSS-RR's neural prior under-denoises very low linear radiance (`~0.01–0.05` in the café). Land only if it gives a clear `Lit` stability win with no bias or moving-camera ghosting.

## 3. Phase 3 · Reuse similarity tests (F26)

Measure each similarity change separately on Sponza (gallery, courtyard, screen-edge) and Bistro (`bistro_cafe`, exterior), recording raw DI/GI bias, dim-tile bias (F23), and `Lit` stability (F22).

### 3.1 Geometric normal vs shading normal in `RTXDI_IsValidNeighbor`
- `RAB_GetSurfaceNormal` (`rtxdi_bridge.slang:265-268`) currently returns normal-mapped `surface.normal`. In the pinned RTXDI 3.2 headers, `RAB_GetSurfaceNormal` is called **only** by `RTXDI_IsValidNeighbor` in DI/GI temporal and spatial resampling (ReSTIR PT also uses it for Jacobians, which this engine does not compile; verify with a grep before changing).
- Under Halton jitter on Bistro's 207 normal-mapped materials, high-frequency bump normals cause adjacent subpixels of the same flat wall/table to fail `dot(n_a, n_b) >= 0.6`, dropping temporal history and falling back to unguarded fallback sampling; across beveled edges, `0.5 / 0.6` is simultaneously too loose.
- **A/B:** switch `RAB_GetSurfaceNormal` to `surface.geoNormal` and sweep `normalThreshold` (`0.6`, `0.8`, `0.9`) across DI and GI (keeping `surface.normal` everywhere in BRDF/target evaluation). Optionally keep a softer shading-normal guard inside `RAB_AreMaterialsSimilar` if normal-mapped grooves leak across grazing shading lobes.

### 3.2 Spatial radius, depth threshold and plane-distance guard
- Current defaults (`vk_rtxdi.cpp`) use `samplingRadius = 32 px` (designed for 1080p, which is **3.75% of screen width** at `853×480` Quality render res) and `depthThreshold = 0.10` (`±1.0 m` at `10 m` depth, admitting cross-surface reuse across thin tables, chairs, and wall junctions while rejecting valid grazing floors at 32 px).
- **Resolution-scaled radius:** scale `samplingRadius` proportionally to `renderHeight / 1080.0f` (clamped, e.g., `~14–16 px` at `480p`), A/B testing `12 / 16 / 24 / 32 px` on `bistro_cafe` and Sponza.
- **Depth threshold:** A/B `depthThreshold = 0.05` vs `0.10`.
- **Same-frame plane-distance guard (no vendor header edits):** `GI/SpatialResampling.hlsli:83` and `DI/SpatialResampling.hlsli:121` (when `enableMaterialSimilarityTest != 0`) both call `RAB_AreMaterialsSimilar(RAB_GetMaterial(a), RAB_GetMaterial(b))` with the comment *"Test material similarity and perform any other app-specific tests"*. Carry `position`, `geoNormal`, `viewDepth`, and `isPreviousFrame` in `RAB_Material`:
  - When `a.isPreviousFrame == b.isPreviousFrame` (spatial reuse, same frame), reject if `max(|dot(a.geoNormal, b.position - a.position)|, |dot(b.geoNormal, a.position - b.position)|) > kPlaneDistFactor * max(a.viewDepth, b.viewDepth)` (A/B `kPlaneDistFactor` in `0.005–0.02`).
  - Skip the world-position plane check when `a.isPreviousFrame != b.isPreviousFrame` (temporal reuse) so moving instances are never falsely rejected; temporal reuse continues to use `expectedPrevLinearDepth` via `motion.z`.

### 3.3 Fallback sampling (`isFallbackSamplingEnabled`)
- In `DI/TemporalResampling.hlsli:113` and `GI/TemporalResampling.hlsli:107`, the fallback sample reuses the same screen pixel from the previous frame while **bypassing `RTXDI_IsValidNeighbor`**. In `GI/TemporalResampling`, paired with `Basic` bias correction (no visibility ray), this pulls bright unoccluded neighbor reservoirs into disoccluded or jitter-rejected dim pixels — directly contributing to F23's `+22.9% / +18.0%` dim-tile over-brightening.
- **A/B:** test `isFallbackSamplingEnabled = false` on GI first, then DI, measuring `bistro_cafe` dim-tile bias, `Lit` stability, and Sponza screen-edge pan/dolly settling (`ENGINE_SCREEN_EDGE_CHECK`).

### 3.4 `RAB_AreMaterialsSimilar` thresholds
- Current implementation (`rtxdi_bridge.slang:286-301`) uses:
  - `abs(luminance(a.diffuseAlbedo) - luminance(b.diffuseAlbedo)) <= 0.25`: on Bistro's dark interior materials (albedo 0.01–0.20), this passes virtually every pair regardless of hue or 20× luminance ratio;
  - `abs(luminance(a.specularF0) - luminance(b.specularF0)) <= 0.25`: merges glossy dielectrics (`F0 = 0.04`) with dark metals (`F0 = 0.28`);
  - `RTXDI_CompareRelativeDifference(a.roughness, b.roughness, 0.5)`: has no additive floor near 0 (rejects `0.02` vs `0.05`) and allows `0.25` vs `0.50` across glossy/rough boundaries.
- **A/B replacement:**
  - Diffuse albedo: relative-plus-floor RGB distance (e.g., `maxComponent(abs(a.diffuseAlbedo - b.diffuseAlbedo)) <= max(0.05, 0.35 * maxComponent(max(a.diffuseAlbedo, b.diffuseAlbedo)))`).
  - Specular F0: dielectric-vs-conductor separation plus tighter delta (e.g., `abs(luminance(a.specularF0) - luminance(b.specularF0)) <= 0.10` or relative test with `0.04` floor).
  - Roughness: additive + relative test `abs(a.roughness - b.roughness) <= max(0.05, 0.25 * max(a.roughness, b.roughness))`.

## 4. Phase 4 · Bistro-scale light-selection PDF & textured emitters (F27)

### 4.1 Independent Bistro light-PDF & textured emission check
The three existing DI convergence checks (`LightsPunctualLamp`, `Sponza_terminator`, `EmissiveStrengthTest`) have 0–5,985 untextured, unoccluded lights and never exercise textured emissive maps or exterior-vs-interior flux imbalance.
- Add a Bistro café DI reference check (extending `engine_guide_capture` or `engine_temporal_check`):
  1. Compare `Power_RIS` converged DI against an independent path-traced emissive reference where direct triangle light sampling is disabled (BRDF-only / high-sample Monte Carlo evaluating exact per-hit `sampleEmissive`, or brute-force textured emission), measuring total bias, per-tile p90, and per-material discrepancy on Bistro's textured fixtures (`mat 212` wall lights, `mat 228`/`251` lanterns, `mat 231` ceiling lamps, `mat 220` bakery, `mat 229`).
  2. Emit a light-table diagnostic summary at load / first frame (optional env flag `ENGINE_LIGHT_PDF_STATS=1`): total triangle lights, count with `power == 0`, top-10 materials by PDF share, and fraction of total PDF mass inside vs outside the current camera frustum / room.

### 4.2 Textured emissive triangle fidelity & zero-power dropout
Today `light_world.slang:77-117,159` calls `sampleEmissive` **once** at the triangle UV centroid using an anisotropic footprint `centroidFootprint`. That single RGB value is baked into `GpuTriangleLight.radiance` and used for **both** the PDF weight and final shading (`sampleTriangleLight`, `triangle_lights.slang:127`). On Bistro's textured lamps (`VK_FORMAT_BC7_SRGB_BLOCK`, UV spans up to `1.107` on `mat 228`/`251`, `0.724` on `mat 231`, `0.495` on `mat 220`, `0.286` on `mat 212`):
- Large housing triangles sharing a fixture material sample coarse mips that bleed neighboring bulb texels into dark geometry (phantom emitters wasting PDF mass and adding spurious glow).
- Partially-lit triangles whose centroid lands on a dark texel get `power == 0`: they are dropped from `Power_RIS`, `RAB_EvaluateLocalLightSourcePdf` returns 0 for BRDF hits on them, **and** `hitEmission` (`path_shading.slang:83-86`) zeroes their emission because they are still registered in the triangle light table — silently dropping real light energy.

**Fixes and A/B:**
1. **Multi-tap / mip-clamped flux estimation in `light_world.slang`:** replace the single wide-footprint centroid tap with a small stratified tap pattern (e.g., 4-tap Dunavant / sub-triangle centroids at clamped mip `max(0, baseMip - 1)`) for `light.radiance` / `power`, eliminating coarse-mip bleed across UV-atlas boundaries while catching small glowing patches on large triangles.
2. **Cull zero-flux triangles from the triangle light list:** for static emissive primitives whose emissive map evaluates to zero across the triangle (dark housings sharing an emissive material), exclude them from `triangleLightTasks` / `triangleLightTable` (at load time or in a one-time build pass) instead of registering all 76,248 triangles. This shrinks the active emitter count (e.g., from 76K toward truly glowing bulb triangles), tightens the PDF texture, and eliminates wasted RIS slots.
3. **Fix `hitEmission` / zero-power dropout:** any triangle whose baked `power == 0` must either be excluded from the triangle light table (so `hitEmission` keeps its exact textured emission on BRDF hits) or return `material.emissive` in `hitEmission` when `lightPower == 0`.
4. **A/B exact textured radiance during shading:** compare constant per-triangle `light.radiance` against evaluating `sampleEmissive` at the sampled barycentric UV in `sampleTriangleLight` (storing primitive/triangle UV lookup or evaluating only in `rtxdi_shade` / final shading while keeping `light.radiance` as the RIS target approximation). Measure bias against the §4.1 independent reference and ABBA GPU cost on `bistro_cafe`.

### 4.3 Exterior-dominated PDF collapse inside `bistro_cafe`
Bistro's exterior streetlamp bulbs (`mat 216`, `LMBR_000018b_bulb`, `ef = [100, 100, 100]`, 2,016 tris, **0 inside the café**) hold **46.28%** of upper-bound flux — and after masking dark housings in interior textured fixtures, an estimated **65–80%** of actual global `Power_RIS` PDF mass. Every pixel inside the café therefore spends most of its 32 `Power_RIS` candidates sampling occluded exterior streetlamps. Meanwhile `ImportanceSamplingContext` defaults to `128 tiles × 1,024 entries` (only 131K slots across 76K lights, ~1.7× oversampling).

**A/B levers on `bistro_cafe` (and Bistro exterior + Sponza):**
1. **Culled light count (from §4.2):** re-measure `Power_RIS` café noise and raw DI variance after removing zero-flux housing triangles.
2. **Scaled `Power_RIS` presampling (`localLightRISBufferParams` in `vk_rtxdi.cpp`):** test `tileSize` (`1024` vs `2048`) and `numLocalLightSamples` (`32` vs `64`), plus `brdfSamples` (`1` vs `2`) when emissive triangles are present.
3. **Localized `ReGIR_RIS` re-evaluation:** I6 F12 chose `Power_RIS` on Bistro's **exterior** (`0.436 → 0.032 ms` presample) where exterior streetlamps match global power, and noted ReGIR's BRDF PDF mismatch (`vk_rtxdi.cpp:430`) and `bistro_cafe` F22 ReGIR regression (`29.72%` with `s0_p0`). Test whether ReGIR with tuned onion cell size (matching café room scale instead of default world scale), tuned build candidates, and proper BRDF-vs-ReGIR sampling (or spatial reuse) beats global `Power_RIS` inside the occluded café, and measure async-compute presample cost (ABBA).
4. **Secondary-bounce NEE (`indirect.slang`, `nrc_update.slang`):** `sampleLightsAt` (`path_shading.slang:31`) uses the same global `Power_RIS` with `numBrdfSamples = 0`, so every `X2` inside the café also wastes 65–80% of its NEE candidates on exterior streetlamps — inflating GI initial sample variance and feeding F23's firefly replacement. Measure `Indirect` raw variance and `bistro_cafe` GI dim-tile bias after §4.2 culling and §4.3 local presampling.

## 5. Checks

- **Phase 1 (rays):**
  - `Sponza_terminator`: 0 light leak, DI convergence pass;
  - `bistro_cafe` junction & wall-sconce inspection (raw `Direct lighting`, `Indirect radiance`, `Lit`): verify no light leaking from exterior streetlamps through café walls, no close-range `< 25 cm` self-occlusion around sconces/niches, no NaN/Inf pixels on degenerate triangles.
- **Phase 2 (DLSS-RR):**
  - `ENGINE_DLSS_CHECK` clean-input static + pan pass (`Lit` std `< 0.5%`, flicker `< 1%`);
  - Perturbation matrix passes (shipped jitter, normal, roughness, depth, `proj[1][1]` beat inverted controls on static + moving poses);
  - `ENGINE_MOTION_TEST` passes (including background rotation and behind-camera clip test).
- **Phase 3 (similarity):**
  - GI check (Sponza gallery + courtyard + `bistro_cafe` against 1-bounce ground truth, tracking dim-tile bias F23);
  - `ENGINE_STABILITY_CHECK` on Sponza and `ENGINE_CHECK_SCENE=bistro_cafe` (tracking `Lit` std/flicker F22);
  - `ENGINE_SCREEN_EDGE_CHECK` (pan + backward dolly settling after fallback and similarity changes).
- **Phase 4 (Bistro PDF):**
  - Independent Bistro café textured-emission convergence check (§4.1);
  - Existing 3 DI convergence checks (`LightsPunctualLamp`, `Sponza_terminator`, `EmissiveStrengthTest`) stay within gate;
  - `ENGINE_LIGHT_PDF_STATS=1` log recorded before/after zero-flux culling.
- **Regression (after every phase):** CTest, `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST`, `ENGINE_GUIDE_CAPTURE`; validation `core + sync + best practices` clean, plus GPU-AV on all modified shaders.

## 6. Measurements to record

| Item | Where | Decision it feeds |
|---|---|---|
| Phase 1 before/after: raw DI/GI bias, dim tiles, NaN count, `Sponza_terminator` leak, ABBA frame cost | Sponza + `bistro_cafe` | Unconditional land; `kMinEndOffset` / `kMaxEndOffset` values |
| Phase 2 `ENGINE_DLSS_CHECK`: clean-input std/flicker, perturbation control table, `SpecAlbedo` RGBA16F vs RGBA8, optional pre-exposure / `pInSpecularHitDistance` | Sponza + `bistro_cafe` (static + pan) | Confirms DLSS-RR history trust; decides `SpecAlbedo` format & optional inputs |
| Phase 3 A/B matrix: `geoNormal` vs `shadingNormal`, `normalThreshold`, scaled `samplingRadius`, `depthThreshold`, plane-distance factor, `isFallbackSamplingEnabled`, `RAB_AreMaterialsSimilar` | Sponza (gallery, courtyard, screen-edge) + `bistro_cafe` | Shipped similarity defaults; measures share of F22/F23 resolved before I12 |
| Phase 4 before/after: active triangle light count, interior vs exterior PDF share, independent textured-emission bias, `Power_RIS` (scaled tiles) vs tuned `ReGIR_RIS` quality & ABBA cost | `bistro_cafe` + Bistro exterior + Sponza | Emissive culling, shading-time texture eval, local light presampling choice (feeds I12) |

## 7. Considerations opened

- **C-I14-1 · Workaround:** `RAB_TraceRayForLocalLight` receives raw `surface.position` from RTXDI 3.2's `InitialSampling.hlsli` without a geometric normal; the bridge stashes `gRtxdiBridge.brdfRayOrigin` in `RAB_SurfaceImportanceSampleBrdf` to keep vendor headers untouched. **Exit:** an RTXDI release that passes `RAB_Surface` (or a ray-origin hook) into `RAB_TraceRayForLocalLight`.
- **C-I14-2 · Workaround:** same-frame plane-distance rejection lives in `RAB_AreMaterialsSimilar` via extra fields on `RAB_Material` (`position`, `geoNormal`, `viewDepth`, `isPreviousFrame`) because `RTXDI_IsValidNeighbor` takes only normal and linear depth and vendor headers are read-only. **Exit:** an RTXDI release with a plane-distance neighbor hook.
- **C-I14-3 · Limitation:** load/build-time zero-flux culling of textured emissive triangles assumes static emissive textures and UV coordinates; animated emissive textures (not yet supported) would require skipping static culling for animated materials. **Exit:** re-check if animated emissive textures are added.
- **C-I14-4 · Unverified:** `ENGINE_DLSS_CHECK` verifies DLSS-RR behavior on the RTX 5070 driver/SDK; re-run on the RTX 2060 Super in I11 and on every DLSS SDK upgrade.
- **C-I14-5 · Open · the gallery `NRC mode` gate is flaky.** NRC training is not reproducible run to run (C-T-5), and on the gallery pose one binary and one seed read anywhere from −2.9 % to −7.0 % bias with tile p90 from 9.6 % to 28.5 % (§8.3). The ±5 % / p90 18 % gate fails on the pre-I14 build too (seed 1). **Exit:** gate the NRC phases on the median of several runs, or widen their gallery limits from a measured spread; the decision is the user's.
- **C-I14-6 · Unverified · the RT check depends on the command order of a model upload.** Rewriting `AssetBuffers::uploadAssetBuffers` to issue every growth copy before the staging copies (same buffers, same bytes; with or without a barrier between them) made `ENGINE_RT_CHECK` report 25 frames in which every primary ray misses, deterministically, right after the guide capture loads `MetalRoughSpheres_flat_normals_front` (24,281 meshlets). The original per-upload order passes 8890 / 8890. That looks like a missing dependency between a model load and the RT scene of the following frames, which one timing exposes; not root-caused. **Exit:** find the missing wait (candidates: the RT check's frame slot TLAS after a template rebuild, the CLAS build of a large new model) or show the check reads a stale slot.
- **C-I14-7 · Limitation · `ENGINE_DLSS_CHECK` has teeth only for the jitter sign.** On the clean analytic input, inverting the normal, squaring the roughness, flipping depth polarity or `proj[1][1]` changes `Lit` std by < 0.1 pp and pan f2f by < 0.1 pp on Sponza and the café; only the inverted jitter sign separates (static std ×11–18, pan f2f +23–46 %). Those controls stay `MEASURED`. **Exit:** a guide-sensitive input (e.g. a specular highlight under motion) that DLSS-RR visibly mis-handles with a wrong guide.

## 8. Results

Built 2026-11-23 on the RTX 5070, release build, vkconfig and validation off unless stated. Commit `39b57b88f` ("almost i14") holds Phases 1–4; the regression fixes of §8.2–§8.4 came after it. The A/B tables of Phases 2–4 were produced in the build session but only reached its task outputs; they were not recorded, so §8.1 gives each decision as the code carries it, with the numbers that survive.

### 8.1 What landed

| Phase | Item | Outcome |
|---|---|---|
| 1 | geometric hemisphere guard in `shadeLightSample`, `RAB_SurfaceImportanceSampleBrdf`, `lambertLightAt` | landed |
| 1 | opaque `side = -1` removed: `rasterRayOrigin(surface)` and `safeSpawnPoint(hit, geoNormal)` offset along the facing-corrected geometric normal; glass transmission still passes the ray direction | landed |
| 1 | `safeSpawnPoint` zero-area guard (`kMinSpawnNormalLengthSq = 1e-30`) | landed |
| 1 | BRDF candidate ray from `gRtxdiBridge.brdfRayOrigin` with `TMin = 0`, `brdfRayMinT = 0` (C-I14-1) | landed |
| 1 | `visibilitySegment` / `traceLightVisibility` / `traceSegmentVisibility`: from the offset origin, area targets end `clamp(d · 1e-4, 0.5 mm, 2 mm)` short; used by DI, GI reconnection, path NEE, glass transmittance | landed |
| 2 | off-screen motion sentinel `(-2 · targetSize, Δw)` when `previousClip.w ≤ 0` | landed |
| 2 | `resolveBackground` subtracts `camera.jitterOffset` | landed |
| 2 | `EnvBRDFApprox2` with the `saturate(max(f0) · 50)` bias guard | landed |
| 2 | `rtScene.prepareFrame` before `isDlssPresented()` is read on frame 0 | landed |
| 2 | `ENGINE_DLSS_CHECK` (`src/core/engine_dlss_check.*`, `FrameFlag::CleanDlssInput` analytic radiance in `combine`, perturbation flags in `resolve` / `vk_dlss`) | landed; gated on static `Lit` std < 0.5 %, flicker < 1 % and the jitter control (C-I14-7) |
| 2 | `SpecAlbedo` RGBA16F | A/B'd, not landed: stays `R8G8B8A8_UNORM` |
| 2 | static `InPreExposure = 16` | A/B'd, not landed: stays 1.0 |
| 2 | `pInSpecularHitDistance` | not wired |
| 3 | `geoNormal` in `RAB_GetSurfaceNormal`, resolution-scaled radius (`rad_24` among others), depth threshold, plane guard, fallback off, tighter `RAB_AreMaterialsSimilar` | A/B'd on Bistro café stability and Sponza screen edge; none kept. The bridge, `vk_rtxdi.*` and NVIDIA's sets are unchanged, so C-I14-2 did not open |
| 4 | `ENGINE_LIGHT_PDF_STATS=1` (one-shot readback, per-material PDF share) and `ENGINE_EMISSIVE_CHECK=1` (`EngineTemporalCheck::Kind::EmissiveRef`: BRDF-only exact textured emission against Power_RIS DI on the café poses) | landed |
| 4 | 4-tap `light_world` flux at a clamped finer mip | landed |
| 4 | load-time zero-flux culling (`cullGltfEmissiveTriangles`, 1-texel-padded UV box over a mip-0 max(RGB) summed-area table; `GpuTriangleLightRef` + sparse remap tails in `rayInstanceLights`) | landed. Bistro: 13,699 of 19,353 unique emissive triangles culled (70.8 %), 30,587 instanced triangle lights instead of 76,248. `AssetBuffers::rebaseCatalog` / `flush` carry the new `emissiveActiveTriangles` slice |
| 4 | `hitEmission` keeps emission of a light triangle whose baked radiance is 0 | landed |
| 4 | exact barycentric `sampleEmissive` in `rtxdi_shade` | A/B'd, not landed: 4-tap baked radiance read lower tile p90 on both café poses and lower dim bias on dining |
| 4 | `Power_RIS` tile size, 16 / 32 samples, `brdfSamples = 2`, tuned `ReGIR_RIS` cells | A/B'd on café stability and Bistro exterior bench; shipped `Power_RIS` set unchanged (C-M4-2) |

### 8.2 Regression

| Check | Result |
|---|---|
| CTest | 8 / 8 (`brdf_test` reference updated to `EnvBRDFApprox2`) |
| `ENGINE_MOTION_TEST` + RT check | 13 / 13; 318 / 318 frames |
| `ENGINE_CULL_CHECK` + RT check | 16 / 16 identical; 454 / 454 frames |
| `ENGINE_GUIDE_CAPTURE` + RT check | normal checks 2 / 2, convergence 3 / 3 (after §8.4: lamp −0.079 %, `Sponza_terminator` +0.000 %, `EmissiveStrengthTest` +0.081 %), glass throughput 2 / 2, RT 8890 / 8890 frames |
| `ENGINE_DLSS_CHECK` Sponza / café | PASS: shipped static `Lit` std 0.333 % / 0.289 %, flicker 0.028 % / 0.040 %; inverted jitter 3.672 % / 5.199 % std, pan f2f 4.33 % / 6.79 % against 3.53 % / 4.64 % |
| `ENGINE_GI_CHECK` | 13 / 14: gallery `NRC mode` flaky (§8.3); every non-NRC phase passes (gallery traced paths +0.61 %, courtyard +1.29 %) |
| `ENGINE_STABILITY_CHECK` café, shipped `Lit` (F22) | tables **13.25 %** std / 79.4 % flicker, dining **11.24 %** / 70.7 % (before I14: 26.85 % / 16.73 % std). Shipped `Direct lighting` against the DI reference: −0.14 % / −0.60 %, dim tiles −2.93 % / −1.20 % |
| `ENGINE_STABILITY_CHECK` Sponza, `ENGINE_SCREEN_EDGE_CHECK` | ran clean, no non-finite pixels; courtyard shipped `Lit` std 0.99 % |
| `ENGINE_EMISSIVE_CHECK` café | Power_RIS DI against the BRDF-only exact emission: tables +0.84 % (p90 27.5 %, dim +8.5 %), dining +0.32 % (p90 16.4 %, dim +4.3 %) |
| Validation core + sync + best practices (guide capture, DLSS check) | only the known external messages (C-H1-2 `WorkgroupSize` / NGX WAW, C-G3-3 NRC at `vkDestroyDevice`) after §8.5 |

### 8.3 Gallery `NRC mode` (GI check, ±5 % / p90 18 %)

Seen first as a 3-of-3 FAIL at −5.75 / −5.17 / −4.99 %, against the I7 record of −3.63 … −0.98 %. Bisected against a pre-I14 build of `a94224210` in `build/h`:

| Gallery `NRC mode` | seed 0 | seed 1 | seed 2 | seed 3 |
|---|---|---|---|---|
| pre-I14 | −3.83 %, p90 14.4 % | **−4.98 %, p90 21.2 % FAIL** | −4.53 %, p90 16.8 % | −4.74 %, p90 14.6 %; rerun −2.88 %, p90 9.6 % |
| I14 | −5.75 / −5.17 / −4.99 / −5.12 % (four runs) | −5.10 %, p90 20.3 % | −4.57 %, p90 19.1 % | −6.97 %, p90 28.5 %; rerun **−4.26 %, p90 14.1 %** |

Variants of the I14 build, seed 0: without the new NEE geometric guard −5.09 %; area margin widened to `clamp(d · 1e-3, 2 mm, 2 cm)` (between NVIDIA's conservative 2 mm and final-visibility 2 cm, `RAB_VisibilityTest.hlsli`) −5.34 %; pre-I14 `indirect` / `nrc_update` / `nrc_resolve` SPIR-V −4.89 %, and −4.41 % on seed 3; pre-I14 `rtxdi_gi_shade` SPIR-V −4.49 %. Every reading lies inside the 2–4 pp spread one binary shows on one seed, so no I14 change is singled out. Conclusion: no I14 regression; the gate was already marginal and fails the pre-I14 build on seed 1 (C-I14-5). The courtyard NRC phases pass in every run.

### 8.4 `LightsPunctualLamp` DI convergence

After zero-flux culling (5,424 of the lamp's 5,985 emissive triangles culled, 561 kept) the check failed at −0.429 % (RGB −0.246 / −0.528 / −0.556 %, limit 0.5 %); with culling disabled it read +0.059 %. Bisected with temporary overrides of the check's test phase:

| Test phase | Bias over 512 frames |
|---|---|
| reference mode itself (no reuse, 32 candidates) | −0.132 % (the realization noise of the check) |
| temporal bias correction Basic / Off, history 1 | −0.429 / −0.434 / −0.435 % |
| 32 candidates with reuse | −0.148 % |
| no reuse, 8 candidates / 16 candidates | −0.438 % / −0.094 % |
| no reuse, 8 candidates, **2048 frames** | **−0.091 %** (per-frame error 57 %) |

Reuse plays no part. Exact RIS is unbiased for any candidate count, and the same 8-candidate stream converges once it runs 2048 frames: the first 512 frames of that run are the failing window. With 561 lights the 8-candidate estimator's per-frame error is high enough that one 512-frame window undershoots by 0.4 %. **Fix:** `kConvergenceFrames` 512 → 2048 (`engine_guide_capture.hpp`); all three convergence checks then pass, the lamp at −0.079 % with culling and +0.149 % without. The guide capture takes ~86 s instead of ~53 s.

### 8.5 `MaterialBuffer` / `PbrExtBuffer` sync hazard (found by this regression)

Sync validation reported `SYNC-HAZARD-READ-AFTER-WRITE` on `vkCmdCopyBuffer` in the guide capture. `appendMaterialUploads` emits one upload per run of contiguous dirty material rows (free-list reuse plus appended rows), so one batch can hold several uploads of a buffer; `uploadAssetBuffers` treated each as alone. A later run that grew the buffer copied the old bytes an earlier in-place run had just written, without a barrier; and a run after a grow would have grown again from the stale old buffer, dropping the earlier run's rows at install. **Fix:** a grow is sized for every upload of that buffer in the batch, later uploads land in the grown buffer, and a transfer barrier precedes a copy that depends on an earlier one; the single-upload path records the same commands as before (C-I14-6 explains why that matters). Validation is clean afterwards and the RT check stays 8890 / 8890.

## Done when

- [x] Phase 1 landed: `shadeLightSample` and `RAB_SurfaceImportanceSampleBrdf` reject `dot(geoNormal, l) <= 0`; opaque `side = -1` removed; `safeSpawnPoint` NaN-guarded; BRDF ray offset + `TMin = 0`; offset-origin visibility with bounded endpoint margin; Sponza + `bistro_cafe` verified clean
- [x] Phase 2 landed: behind-camera off-screen motion sentinel, jittered `resolveBackground`, `EnvBRDFApprox2` `f0 == 0` guard, startup `isDlssPresented()` verified, `SpecAlbedo` precision decided, and `ENGINE_DLSS_CHECK` passing (clean-input baseline + all perturbation controls) — the normal, roughness, depth and Y-flip controls do not separate on clean input and are measured only (C-I14-7)
- [x] Phase 3 landed: `geoNormal` vs `shadingNormal`, resolution-scaled `samplingRadius`, `depthThreshold`, same-frame plane-distance guard, `isFallbackSamplingEnabled`, and `RAB_AreMaterialsSimilar` each A/B-measured and decided on Sponza + `bistro_cafe` — all kept at NVIDIA's values; the A/B tables were not recorded (§8)
- [x] Phase 4 landed: independent Bistro textured-emission check passing; zero-flux emitter culling and multi-tap `light_world` flux landed; shading-time texture eval and `Power_RIS` vs tuned `ReGIR_RIS` decided with ABBA timings on `bistro_cafe` and exterior — decided (4-tap baked, `Power_RIS`), the timings were not recorded (§8)
- [x] `NVIDIA defaults` (`nvidiaSettings()` / `nvidiaGiSettings()`) remain callable and unchanged (C-M4-2); no edits under `ThirdParty/`
- [ ] CTest, GI check, 3 DI convergence checks, `ENGINE_STABILITY_CHECK` (Sponza + `bistro_cafe`), `ENGINE_SCREEN_EDGE_CHECK`, `ENGINE_DLSS_CHECK`, `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST`, `ENGINE_GUIDE_CAPTURE` pass; validation and GPU-AV clean — all pass except the GI check's gallery `NRC mode` (13 of 14, flaky before I14 too, C-I14-5); validation clean (§8.5); GPU-AV not run (C-E1-3)
- [x] Updated baseline numbers handed off to I12 for final F22/F23 closure; `m8_optimization_and_fixes.md`, `considerations.md`, and `docs/settings.md` updated — the baseline is §8.2

## Verification of the findings (2026-11-22)

Every cited line was checked against the tree and against NVIDIA's code in `reference/`. The line numbers are correct. The Bistro statistics are correct against `bistro.gltf`: 254 materials, 207 normal-mapped, 234 spec-gloss, 21 emissive, 76,248 instanced emissive triangles, and material 216 at factor 100 with 2,016 triangles. Two items are not verified: the 46.28 % flux share needs triangle areas, and the 65–80 % share is an estimate.

| Item | Verdict |
|---|---|
| 1.1 `shadeLightSample` geometric check | **real bug.** FullSample rejects at `RAB_Surface.hlsli:230, 316`. Combined with `rasterRayOrigin`'s back-side flip, a shadow ray toward a light behind the geometric plane starts behind the surface, so light leaks through single-sided walls. The highest-value item of I14 |
| 1.1 opaque `side = -1` removal | defensive only. Once 1.1's check is in, no opaque ray goes below the plane. The traced-hit `worldNormal` (`hit.slang:159`) is not facing-corrected, so it must be oriented toward the incoming ray before the flip goes |
| 1.1 `rsqrt` guard | real gap, cheap. Whether zero-area triangles are ever reported as hits depends on the implementation |
| 1.2 BRDF hemisphere and origin | **not an NVIDIA difference.** FullSample's `RAB_SurfaceImportanceSampleBrdf` tests the shading normal only, and `RTXDI_SampleBrdf` traces from the raw position with `brdfRayMinT = 0.001`, as ours does. After 1.1, a candidate below the plane gets target 0: a wasted ray, not a leak. Low value against C-I14-1's bridge state |
| 1.3 endpoint margin | real. FullSample `setupVisibilityRay` uses `TMin = 1 mm` and `TMax = length − 2 mm`; ours ends `1e-3 · d` short (0.25 mm at 25 cm, 1.5 cm at 15 m). The fix moves toward NVIDIA |
| 2.1 motion sentinel | real; NVIDIA's code. The source is `reference/RTXGI/Samples/Pathtracer/Shaders/Pathtracer.hlsl:822-832`, not `IndirectDiffuse` |
| 2.1 background jitter | overstated. A direction at infinity moves with the rotation only. Using the pixel center instead of the jittered position changes its motion by the motion's derivative over a subpixel, not by a pixel. Verify only |
| 2.1 `EnvBRDFApprox2` | real. The source is `reference/vk_gltf_renderer/shaders/dlss_util.h:38` (also `vk_lod_clusters`), not `vk_denoise_dlssrr`. Not verified: how many Bistro texels have `f0 ≈ 0` |
| 2.1 `SpecAlbedo` RGBA8, startup order | the format is a fact; the order is a check item, with no defect shown |
| Phase 3 (all) | the facts are correct, but every value is NVIDIA's: the RTXDI library defaults for radius, depth and normal thresholds and fallback sampling; FullSample's `RAB_GetSurfaceNormal` returns the shading normal; its `RAB_AreMaterialsSimilar` thresholds are the same 0.5 / 0.25 / 0.25. So these are tuning A/Bs, not correctness fixes, and the stated causes (jitter rejections, fallback feeding F23) are hypotheses. 3.1, 3.2's plane guard and 3.4 are bridge code, so they also change NVIDIA's sets unless they sit behind a switch. The C-M4-2 decision above covers only Phases 1 and 2 |
| 4.2 single centroid tap | true, and it is FullSample's method: `PrepareLights.hlsl:164` `SampleGrad` at `centerUV` with short and long gradients; `centroidFootprint` is its port. The dropout follows from NVIDIA's design, since the light table and the `hitEmission` rule match FullSample. These are improvements beyond NVIDIA, so 4.1's reference check comes first, as planned |
| 4.3 RIS tiles, NEE BRDF samples, I6 numbers | correct: `ImportanceSamplingContext.h:34` `{1024, 128}`, `sampleLightsAt` `numBrdfSamples = 0`, I6 `0.436 → 0.032 ms` |
