# M8 · I13: thin-walled glass

**Status:** planned 2026-11-22, built 2026-11-23 (§8) · **Milestone:** M8 ([README](./README.md))
**Implements:** finding F21 ([m8_optimization_and_fixes.md](./m8_optimization_and_fixes.md) §4): `KHR_materials_transmission` is parsed and uploaded but no shader reads it, so glass renders as an opaque white dielectric and blocks every ray
**Depends on:** I1 (runners), I2 (shading fixes), I6 (fresh final DI visibility, which the shadow transmittance multiplies)
**Considerations:** opens C-I13-1 … C-I13-4 (§7), and C-I13-5 (the throughput check's reference) and C-I13-6 (the blended specular guide) when built; updates engine spec §12.4
**References:**
- `reference/RTXDI/Samples/FullSample/Shaders/GlassPass.hlsl` and `Source/App/SceneRenderer.cpp:1143-1146`: glass composited after lighting and before DLSS-RR, up to 8 layers, Fresnel reflection plus transmission throughput; `RasterizedGBuffer.hlsl:168-182` discards transmissive surfaces from the G-buffer; `RAB_LightSampling.hlsli:186,200` traces DI shadows against `INSTANCE_MASK_OPAQUE` only
- `reference/vk_gltf_renderer/docs/denoising.md` ("Clear glass: primary surface replacement") and `shaders/pathtrace_functions.h.slang:1146-1250` (`dlssIsClearGlass`, `dlssTraceSkipThinGlass`): DLSS-RR guides from the first surface behind clear glass; `pInColorBeforeTransparency` measured worse and not wired
- `reference/vk_denoise_dlssrr/dlss_rr/shaders/primary_rgen.slang:265-380`: the same guide replacement along mirror chains
- glTF `KHR_materials_transmission` (thin-walled without `KHR_materials_volume`: no refraction offset, tint by base color, Fresnel from `KHR_materials_ior` / `KHR_materials_specular`)

## Scope

Stage 1 of transmission: **smooth, thin-walled glass**, which is all of Bistro's glass. Bistro has 18 transmissive materials (factor 0.66–0.95, metallic 0, no `volume`, 11 double-sided): café windows, bottles, glasses, headlights. Refraction, volume attenuation and rough transmission are stage 2, the engine spec's path-traced transmission (§15, C-I13-1).

| Item | Change | Files |
|---|---|---|
| Classification | `GpuMaterialFlag::Transmissive` for a material with `transmissionFactor > 0`, `roughnessFactor ≤ kGlassMaxRoughness` (0.3) and no roughness texture; other transmissive materials stay opaque and are counted in the load log | `src/core/types.hpp`, `src/core/assets_loader.cpp` |
| Raster | transmissive primitives leave the raster templates, so the vis buffer, Hi-Z and every lighting pass see the opaque surface behind | `src/render/vk_meshlet_culling.cpp` |
| Ray masks | `kRayMaskOpaque = 0x01` and `kRayMaskGlass = 0x02` from a build flag in `GpuRayInstanceBuild`; every existing trace switches from `0xFF` to `kRayMaskOpaque` | `src/render/vk_rt_scene.cpp`, `shaders/passes/rt_instances.slang`, `shaders/raytracing/{trace,ser}.slang`, `shaders/geometry/hit.slang` |
| Glass pass | new compute pass `glass.slang` after `combine`, before DLSS-RR: walks the glass layers in front of the vis-buffer hit and rewrites `noisyColor` (§2) | `shaders/passes/glass.slang`, `src/render/vk_renderer.cpp`, `src/render/vk_shader_objects.*` |
| Reflection shading | the X2 shading of ReSTIR GI (`shadeRayHit`, `secondaryDirectLight`) moves to a shared module so the glass pass can shade its reflection hits | `shaders/lighting/path_shading.slang`, `shaders/passes/indirect.slang` |
| Shadow transmittance | `glassTransmittance(origin, dir, tMax)` multiplies final DI shading, X2's direct light and ReSTIR GI final shading (§3) | `shaders/lighting/glass.slang` (new), `shaders/passes/rtxdi_shade.slang`, `rtxdi_gi_shade.slang`, `indirect.slang` |
| DLSS-RR guides | A/B: guides of the surface behind vs guides blended with the glass layers (§4) | `shaders/passes/glass.slang` |
| Runners | `ENGINE_MODEL` (startup model for any run) and `ENGINE_CHECK_SCENE=bistro` (Bistro poses in `check_poses.hpp`); a glass throughput check in the guide capture (§5) | `src/core/vk_engine.cpp`, `src/core/check_poses.hpp`, `src/core/engine_temporal_check.*`, `src/core/engine_guide_capture.*` |
| Settings, timer, debug view | `Glass` section in the Ray tracing panel, `GpuTimerScope::Glass`, `Glass` debug view (throughput / reflection) | `src/ui/engine_ui.cpp`, `src/render/vk_gpu_timer.hpp`, `shaders/passes/debug_view.slang` |

## Decisions

| Decision | Rule | Why |
|---|---|---|
| Composite before DLSS-RR, not a forward pass | Glass is traced in compute after `combine`, as the RTXDI FullSample does | The engine spec rules out a raster transparency pass (§12.4); DLSS-RR then denoises and upscales one color, so no second upscale path is needed |
| Glass out of the vis buffer | Transmissive primitives are not rasterized; the vis buffer holds the first opaque surface | ReSTIR DI/GI, NRC and every guide then describe what the camera sees through thin glass, which is vk_gltf_renderer's primary surface replacement at no extra cost |
| No any-hit | Glass instances keep `FORCE_OPAQUE`; the pass and `glassTransmittance` loop closest hits on `kRayMaskGlass` (`tMin` past each hit) and skip a back face of a single-sided material in the shader | Engine rule (no any-hit); a few glass instances make the loop cheap, and Sponza has none |
| Every other ray ignores glass | Visibility, GI sample generation, ReSTIR GI reconnection, NRC training and the GI reference trace `kRayMaskOpaque` | Matches RTXDI's DI shadows; keeps temporal / spatial visibility tests and sample generation consistent. Light through glass is attenuated only where a sample is shaded (§3) |
| Rough or textured-rough transmission stays opaque | Above `kGlassMaxRoughness` the material renders as today | A sharp straight-through view of frosted glass is more wrong than an opaque one; stage 2 adds rough transmission (C-I13-1). Bistro's `LMBR_000009e_Glass` has no `pbrMetallicRoughness` block (glTF default roughness 1) and stays opaque |
| Reflection without indirect light | A reflection hit gets emission and one direct-light sample (X2 shading), no NRC query, no GI | The pass runs after NRC resolve; a query would need its own slots (C-I13-2) |
| No `pInColorBeforeTransparency` / transparency layer | Not wired | vk_gltf_renderer measured it worse on top of the guide replacement; the transparency layer targets particles |
| Sponza unchanged | Scenes without transmissive primitives skip the pass (host flag) and every runner reads byte-identical | The masks change nothing when no instance carries `kRayMaskGlass` |
| NVIDIA sets unaffected | Glass is material handling, not a lighting setting; `nvidiaSettings()` / `nvidiaGiSettings()` keep their values | M8 rule (C-M4-2) |

## 1. Material and scene

- **Loader:** `KHR_materials_transmission` (factor + texture, red channel), `KHR_materials_ior` and `KHR_materials_specular` already reach `GpuMaterialExtension`. The glass pass is the first reader of `loadMaterialExtension`.
- **Flag:** `GpuMaterialFlag::Transmissive` on the classification of the scope table; the loader logs the count of transmissive materials kept opaque, with the reason.
- **Raster:** `MeshletCulling::buildTemplates` (`vk_meshlet_culling.cpp:218`) drops primitives whose material is transmissive. Their meshlets still feed `RayTracingScene`.
- **Ray instances:** `RayTracingScene::buildTemplates` sets `kRayInstanceBuildFlagGlass`; `rt_instances.slang` writes `instanceMask = kRayMaskGlass` for those rows and `kRayMaskOpaque` for the rest. `RayTracingScene` reports the glass row count; zero skips the pass.

## 2. Glass pass

Per pixel, at render resolution, after `combine` (one `GpuTimerScope::Glass`):

1. The camera ray of the pixel (jittered, as the vis buffer), `tMax` = distance to the vis-buffer hit, or `kMissDistance` on an empty pixel.
2. Up to `maxGlassLayers` (default 4) closest hits on `kRayMaskGlass`, front to back. At each layer, with `n` the shading normal turned toward the ray, `μ = |dot(n, −d)|`:
   - `F0 = ((ior − 1) / (ior + 1))² · specularFactor · specularColorFactor`, clamped to 1; dielectric `F = F0 + (1 − F0)(1 − μ)⁵`; for metallic `m`, `F_m = baseColor + (1 − baseColor)(1 − μ)⁵` and `F_layer = lerp(F, F_m, m)`
   - `t` = transmission factor × texture; throughput `T = (1 − m) · t · (1 − F) · baseColor` (base color once: RTXDI's GlassPass multiplies it twice, `ms.baseColor` and `baseOrDiffuseColor`)
   - reflection `R = F_layer · L(reflect(d, n))`, the reflection ray traced on `kRayMaskOpaque` and shaded as X2 (`shadeRayHit` + `secondaryDirectLight`); a miss returns the sky, 0 until M7's environment light (C-I13-2)
   - diffuse share `D = (1 − m)(1 − t)(1 − F) · baseColor · E_direct`: one direct-light sample at the glass surface with Lambert, no indirect
   - emission `E` of the glass material
   - coverage: `alphaMode: BLEND` with opacity `α` scales `R + D + E` by `α` and `T` by `α` plus `(1 − α)` pass-through; `MASK` is already cut by the OMM
3. Composite: `C = Σᵢ (Πⱼ<ᵢ Tⱼ)(Eᵢ + Rᵢ + Dᵢ) + (Πᵢ Tᵢ) · C_behind`, written back to `noisyColor`.
4. Stop early when `luminance(ΠT) < 0.01`. A layer limit reached with throughput left keeps the composite so far (logged as a debug counter).

Settings (Ray tracing panel, `Glass`): enable, max layers, reflection, diffuse share, shadow transmittance (§3). Debug view `Glass`: `ΠT` in RGB, the reflection term with a toggle.

## 3. Shadow transmittance

`glassTransmittance(origin, dir, tMax)` returns `Π T` (Fresnel at each crossing's angle) over the glass hits on the segment, with the same closest-hit loop and layer cap. It multiplies:

- `rtxdi_shade`'s final DI shading (the fresh visibility ray of I6's defaults);
- `secondaryDirectLight` (X2, glass reflection hits, glass diffuse share);
- `rtxdi_gi_shade`'s final shading along the primary-to-X2 reconnection.

Target functions and the temporal / spatial visibility tests stay glass-free, so the resampling weights ignore glass. The estimator stays unbiased, since the shaded value carries the transmittance and the target is non-zero wherever it is. Without the term, light through a window is too bright by `1 / T` (about 10% for Bistro's clean panes, `t = 0.95`, up to about 60% for its dirty glass, `t = 0.66`). It is a switch, measured on and off (§6).

## 4. DLSS-RR guides

The vis buffer already gives the guides of the surface behind (depth, motion, normal, roughness, albedo), which is correct for straight-through glass. The glass terms `R`, `D`, `E` reach the color with no guide. A/B on the Bistro window poses with the stability and screen-edge runners:

- **(a)** guides untouched;
- **(b)** specular albedo `+= (Πⱼ<ᵢ Tⱼ) · F_layer`, diffuse albedo `= ΠT · albedo_behind + Σ (Πⱼ<ᵢ Tⱼ)(1 − m)(1 − t)(1 − F) · baseColor`, both clamped to 1.

Keep the variant with the lower `Lit` static pixel error and temporal std; (a) on a tie (fewer writes).

## 5. Checks

- **Sponza:** GI check, stability, light change and screen edge read byte-identical to the post-I6 logs (the NRC phases of the GI check excepted, C-T-5).
- **Glass throughput check** (guide capture, `TransmissionTest` and the Bistro window pose): with reflection, diffuse share and emission off, the noisy color at glass pixels divided by the same pixels with the pass off equals the analytic `ΠT` of §2. Gate: median relative error ≤ 1%, p90 ≤ 3% over pixels behind exactly one or two layers.
- **Bistro runners:** `ENGINE_MODEL=models/bistro/bistro.gltf ENGINE_CHECK_SCENE=bistro` selects two poses recorded in `check_poses.hpp` when built: the café windows from the street, and the bar shelf with bottles and glasses. Stability and screen edge on both, pass on and off.
- **Guide capture PNGs:** `TransmissionTest`, `TransmissionSuzanne` (thin, roughness 0.1) and both Bistro poses, `Lit` and the `Glass` debug view.
- **Regression:** CTest, `ENGINE_RT_CHECK` (primary rays on `kRayMaskOpaque` match the vis buffer), `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST`, `ENGINE_GUIDE_CAPTURE`; validation core + sync + best practices; GPU-AV on the new shaders once C-E1-3 allows it.

## 6. Measurements to record

| Item | Where | Decision it feeds |
|---|---|---|
| Glass pass cost, pass on vs off (ABBA, release, no layers) | both Bistro poses; Sponza (must read 0) | `maxGlassLayers` default |
| Shadow transmittance on vs off: cost and the lit floor behind the café windows | Bistro street pose | switch default |
| Guides (a) vs (b): `Lit` temporal std, flicker, static pixel error, pan / dolly settling | both Bistro poses | guide variant |
| Layers reached and early-stop share | `Glass` debug counters | layer cap |

## 7. Considerations opened

- **C-I13-1 · Limitation:** no refraction, `KHR_materials_volume` (Beer–Lambert attenuation), rough or textured-rough transmission, dispersion or diffuse transmission: stage 2, engine spec §15 path-traced transmission, with vk_gltf_renderer's refracted guide chain. `DragonAttenuation`, `AttenuationTest` and `TransmissionRoughnessTest` render opaque.
- **C-I13-2 · Approximation:** glass reflections carry emission and direct light only; misses are black until M7's environment light.
- **C-I13-3 · Approximation:** glass is absent from GI sample generation, NRC training and the GI reference, and attenuates only at final shading: no light reflected off glass reaches indirect lighting.
- **C-I13-4 · Unchanged:** `alphaMode: BLEND` without transmission (Bistro's three decal materials) stays a 0.5 cutout.

## 8. Results

Built 2026-11-23 (RTX 5070, release, 1280×720 window, DLSS-RR Quality, no validation layers unless stated). Bistro's two poses run under `kBistroGlassSun*` (azimuth 110°, elevation 35°). Bistro loads 17 thin-walled glass materials (337 instances, out of the raster templates) and keeps `LMBR_000009e_Glass` opaque (roughness 1).

**Glass throughput check** (guide capture, transmission only, pass on / off at the same frame indices and exposure against the `Glass` view's `ΠT`):

| Shot | Compared px (1–2 layers) | Median | p90 | Pixels without glass changed |
|---|---|---|---|---|
| `TransmissionTest_sun` | 2208 | 0.023% | 0.044% | 0 |
| `Bistro_street_windows` | 38097 (56785 below 0.1% of the mean skipped) | 0.029% | 0.208% | 0 |

The reference is the pass's own `ΠT`, not a CPU evaluation (C-I13-5). Bistro's maximum (`124–543%` across runs) comes from a few pixels that do not repeat between the phases. The `TransmissionTest` PNGs show the smooth row tinted and see-through, the textured and masked panes with their pattern, and the rough and metal rows opaque (8 materials kept opaque, logged). `TransmissionSuzanne`'s `Glass` view shows its two crossings.

**Pass cost** (ABBA, 2000 frames per run):

| Pose | Glass pass on | Pass off | GPU total on / off |
|---|---|---|---|
| Street windows (99K px behind glass) | 1.06–1.10 ms | 0.0007–0.0009 ms | 6.59–6.84 / 5.11–5.80 ms |
| Bar shelf (24K px) | 0.82–0.83 ms | 0.0008 ms | 6.29–6.38 / 5.49–5.54 ms |
| Sponza courtyard (no glass) | 0.0006 ms (the empty scope) | 0.0006 ms | 3.16 / 3.20 ms |

With shadow transmittance off (the default below), the pass reads 0.98 ms on the street and 0.79 ms on the bar shelf. Its parts:

| Configuration | Street | Bar shelf |
|---|---|---|
| shipped | 0.98 ms | 0.79 ms |
| reflection off | 0.39 ms | 0.38 ms |
| diffuse share off | 0.75 ms | 0.58 ms |
| walk only (reflection, diffuse share and emission off) | 0.22 ms | 0.15 ms |
| `maxLayers` 2 | 0.79 ms | 0.57 ms |
| `maxLayers` 8 | 1.26 ms | 1.10 ms |

**Layers** (`[Bench]` glass line, one frame):

| Pose | Pixels by layers 1 / 2 / 3 / 4 (cap 4) | At the cap | With cap 8: past 4 |
|---|---|---|---|
| Street windows | 93229 / 1695 / 3110 / 948 | 947 px (1.0%) | 948 px, up to 8 layers |
| Bar shelf | 9843 / 7769 / 985 / 5735 | 5715 px (23%) | 1700 px (7%), up to 8 layers |

**Decision: `maxLayers` stays 4.** 2 truncates 59% of the bar shelf's glass pixels. 8 costs `+0.28 / +0.31 ms` for 1% / 7% of them, which are already behind four layers of throughput.

**Shadow transmittance** (street pose):

| | Off | On |
|---|---|---|
| GPU total | 5.90 ms | 6.58 ms (`+0.68 ms`: DI shade `+0.14`, Indirect `+0.25`, GI shade `+0.12`, glass pass `+0.15`) |
| `Direct lighting` mean | 0.012254 | 0.012149 (`−0.86%`) |
| `Lit` mean | 0.078177 | 0.076113 (`−2.6%`) |
| Pixels darkened by more than 3% | | 1158 (0.3% of the frame), median on / off 0.860 |

No direct sun reaches the café floor behind the windows. A search over seven sun angles (azimuth 110–140°, elevation 6–55°) found the awning blocking a high sun and the buildings across the street a low one. The term then changes only scattered glass-lit pixels.

**Decision: shadow transmittance off by default**, as in RTXDI's FullSample, whose DI shadows trace `INSTANCE_MASK_OPAQUE` only. It is a 12% GPU cost for a change that is visible on 0.3% of the frame here. The switch stays (`Shadow transmittance`, `glass.shadowTransmittance`).

**DLSS-RR guides** (`ENGINE_CHECK_SCENE=bistro`; these runs had shadow transmittance on as the reference state, with its off state measured beside it):

| Pose | State | `Lit` mean | Temporal std | Flicker |
|---|---|---|---|---|
| Street windows | (a) guides of the surface behind | 0.076113 | 8.66% | 71.4% |
| | (b) blended guides | 0.076174 | 7.98% | 67.2% |
| | shadow transmittance off | 0.078177 | 8.73% | 71.6% |
| | pass off (glass invisible) | 0.047882 | 11.66% | 81.0% |
| Bar shelf | (a) | 0.021569 | 24.51% | 63.8% |
| | (b) | 0.021203 | 26.40% | 69.5% |
| | shadow transmittance off | 0.022144 | 24.08% | 63.7% |
| | pass off | 0.020030 | 27.37% | 70.9% |

Screen edge: static pixel error (band / disoccluded), and the band's pixel error 64 frames after the move:

| Pose, motion | (a) | (b) | Shadow transmittance off | Pass off |
|---|---|---|---|---|
| Street, pan 10° | 5.2% / 16.0%, 16.0% | 4.8% / 14.9%, 15.0% | 5.3% / 15.9%, 16.0% | 5.5% / 24.3%, 16.7% |
| Street, dolly 1 m | 9.3% / 9.0%, 22.3% | 8.6% / 8.2%, 22.3% | 9.5% / 9.0%, 23.0% | 9.3% / 14.9%, 23.1% |
| Bar, pan 10° | 7.5%, 31.3% | 7.5%, 31.9% | 7.4%, 30.5% | 7.7%, 30.8% |
| Bar, dolly 1 m | 22.7% / 36.9%, 40.6% | 23.5% / 40.2%, 40.2% | 22.5% / 33.8%, 40.7% | 23.7% / 58.8%, 40.0% |

**Decision: guides (a).** (b) wins on the street windows (large panes: `Lit` std `−0.7` points, static error `−0.4…−1.1` points) and loses on the bar shelf (bottles and glasses: std `+1.9` points, dolly disoccluded error `+3.3` points). That is a tie across the two poses, so (a) stays, per §4's rule (fewer writes). (b) stays a switch (`Blend DLSS guides with glass`, `glass.blendGuides`); its specular blend deviates from §4 (C-I13-6). Glass itself steadies the image: with the pass off, the windows show the noisier interior (street `Lit` std 11.66%, disoccluded error 24.3%).

**Sponza** (against a HEAD build, `abc011787`, same machine and session):
- Stability (29 lines), screen edge (37) and light change (17) are byte-identical.
- The GI check's non-NRC lines are identical. Its NRC phases differ run to run on both builds (C-T-5); the gallery's NRC-mode phase sits at its gate (`−5.36%` FAIL on HEAD's run, `−4.80%` PASS on this one).
- The first run differed in `Lit` only. A `.spv` swap traced it to `rtxdi_gi_shade`: multiplying the GI sample by a transmittance of exactly `1.0` changed the driver's FMA contraction. The shading now keeps its pre-I13 expressions unless `kGlassFlagShadowTransmittance` is set.

**Regression and validation:**
- CTest 8 of 8 (the layout test mirrors `GpuGlassCounters`, `GpuFrameRoot`'s glass fields and the glass guide images).
- `ENGINE_RT_CHECK` 32 of 32; `ENGINE_CULL_CHECK` 16 of 16; `ENGINE_MOTION_TEST` 13 of 13.
- `ENGINE_GUIDE_CAPTURE`: 2 of 2 normal, 3 of 3 convergence and 2 of 2 throughput checks, 506 PNGs.
- Validation (core + sync + best practices, Bistro street, shadow transmittance and blended guides on): only NGX's known messages (C-H1-2).
- GPU-AV (`ENGINE_GPUAV_RESOLVE_PER_BIN=1`, C-E1-3) over two Bistro glass stability steps (shadow transmittance, blended guides): no shader errors.

**Post-audit fixes (2026-11-23):**
- Primary glass emission (`passes/glass.slang`) and delta mirror reflections (`path_shading.slang` `shadeRayHit(..., isLightEmissionKept = true)`) now keep raw `material.emissive` instead of zeroing emissive triangle lights through `hitEmission()`. On Bistro street windows, `Lit` mean rises from `0.078177` to `0.079269` (`+1.4%`) with reflected café lamps, and temporal std drops from `8.73%` to `8.46%` (`8.33%` with shadow transmittance, `7.77%` with blended guides).
- `qualifyOmmPrimitive()` (`assets_loader.cpp`) skips OMM baking on `AlphaBlend` glass so textured translucent panes blend smoothly in `evaluateGlassLayer()` instead of being hard-cut at `alpha < 0.5`; non-glass `BLEND` decals remain untouched (C-I13-4).
- `classifyTransmission()` (`material_store.hpp`) keeps pure-metallic transmissive materials (`metallicFactor >= 1.0`) opaque (`KeptOpaqueMetal`).
- `DebugView::Glass` (`debug_view.slang`) returns `(0, 0, 0)` / `(1, 1, 1)` with `alpha = 0` when `kGlassFlagPass` is clear instead of leaking `noisyColor`.
- `lambertLightAt()` (`path_shading.slang`) guards against back-face lighting with `dot(surface.geoNormal, lightSample.l) <= 0`.
- `evaluateGlassLayer()` (`lighting/glass.slang`) uses `sampleShadingNormal()` (`material/pbr.slang`), samples `baseColor` once, applies `KHR_materials_specular` `F90 = saturate(extension.specularFactor)`, clamps `transmission` and `mu`, and skips `sampleEmissive()` during `glassTransmittance()` walks; `passes/glass.slang` initializes its RNG lazily on the first glass hit and uses distinct seed `kGlassLightSeed = 45` (off `kGiDecorrelationSeed = 44`) and shared `kGlassHitsPerLayer = 2`.
- `rtxdi_gi_shade.slang` tests opaque `isReconnectionVisible()` before `reconnectionTransmittance()` and skips `reconnectionTransmittance()` when radiance is zero (F13 far-miss placeholders).
- `vk_renderer.cpp` allocates `glassCounters` in `eHostCached` memory (no uncached WC reads, MG-B2), reads/zeroes it only when glass recorded in that slot, and resets DLSS-RR history when `glass` settings change on a scene with glass.
- Re-verified after fixes: CTest 8/8, `ENGINE_RT_CHECK` 32/32, `ENGINE_CULL_CHECK` 16/16, `ENGINE_MOTION_TEST` 13/13, `ENGINE_GUIDE_CAPTURE` (2/2 normal, 3/3 DI convergence, 2/2 glass throughput: `TransmissionTest` 0.023% / 0.044%, Bistro 0.029% / 0.207%), Bistro core+sync+best-practices validation clean, Bistro GPU-AV clean on shipped, `shadow transmittance` and `blended guides` steps (`0` shader errors).

## Done when

- [x] Transmissive materials classified; the opaque-kept ones logged with their reason
- [x] Glass out of the vis buffer and on its own ray mask; every other ray on `kRayMaskOpaque`
- [x] Glass pass composites thin-walled layers before DLSS-RR; glass throughput check passes on `TransmissionTest` and Bistro
- [x] Shadow transmittance measured on and off, default decided
- [x] DLSS-RR guide variant decided with stability and screen-edge numbers on both Bistro poses
- [x] Sponza runners byte-identical to post-I6 (NRC phases excepted); pass cost on Sponza 0
- [x] Glass pass cost recorded on both Bistro poses (ABBA)
- [x] CTest, RT check, cull check, motion test, guide capture pass; validation clean
- [x] Engine spec §12.4, `docs/settings.md` and the considerations updated
