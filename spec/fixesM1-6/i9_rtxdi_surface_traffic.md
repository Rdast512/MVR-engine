# M8 · I9: RTXDI surface traffic

**Status:** built 2026-11-22 · **Milestone:** M8 ([README](./README.md))
**Implements:** finding O4 ([m8_optimization_and_fixes.md](./m8_optimization_and_fixes.md) §2.4)
**Depends on:** I4 ([i4_frame_profile.md](./i4_frame_profile.md)), I5 ([i5_restir_gi_tuning.md](./i5_restir_gi_tuning.md)), I6 ([i6_restir_di_tuning.md](./i6_restir_di_tuning.md))
**References:**
- `reference/RTXDI/Samples/FullSample/Shaders/LightingPasses/RtxdiApplicationBridge/{RAB_Surface.hlsli,RAB_Material.hlsli}`
- `reference/RTXDI/Samples/FullSample/Shaders/GBufferHelpers.hlsli`
- `docs/engine_spec.md` §9.3 (resolve guide images)

## Scope

Every `RAB_GetGBufferSurface(pixel, previousFrame)` call in ReSTIR DI and ReSTIR GI (initial, temporal, spatial neighbor lookups, and final shading) currently performs **5 texture loads (36 B across 5 images)**:

| Image | Format | Size | Consumers outside `rtxdi_bridge.slang` |
|---|---|---:|---|
| `Position` | `RGBA32F` | 16 B | none |
| `NormalRoughness` | `RGBA16F` | 8 B | DLSS-RR (`pInNormalRoughness`), `debug_view` |
| `GeoNormal` | `RG16_SNORM` | 4 B | `debug_view` |
| `DiffuseAlbedo` | `RGBA8_UNORM` | 4 B | DLSS-RR (`pInDiffuseAlbedo`), `combine`, `nrc_resolve`, `motion_check`, `debug_view` |
| `SpecularF0` | `RGBA8_UNORM` | 4 B | `debug_view` |

Three of those images (`Position`, `GeoNormal`, `SpecularF0`) exist solely for `rtxdi_bridge.slang` and `debug_view.slang`, and `resolve.slang` writes 8 storage images (52 B/px) into each frame's guide set.

## Design (O4)

### 1. Single packed RTXDI surface guide (`kGuideSurface`)

Replace `Position` (`RGBA32F`, 16 B), `GeoNormal` (`RG16_SNORM`, 4 B), and `SpecularF0` (`RGBA8_UNORM`, 4 B) with one `GuideImage::Surface` (`kGuideSurface`, `VK_FORMAT_R32G32B32A32_UINT`, 16 B):

| Word | Bits | Contents | Notes |
|---|---|---|---|
| `x` | 32 | `asuint(viewDepth)` | Exact fp32 `currentClip.w` (`0u` = empty/background; matches `motion.z` and early-outs before loading albedo) |
| `y` | 16+16 | `packOct16(material.n)` | `snorm16 × 2` octahedral shading normal (`< 0.0025°` max error; unit length guaranteed by `octDecode`) |
| `z` | 16+16 | `packOct16(surface.geoNormal)` | `snorm16 × 2` octahedral geometric normal (identical precision to `RG16_SNORM`) |
| `w` | 8+8+8+8 | `f0.rgb` (8-bit unorm × 3) \| `a` (`0` = unlit; `1 + round(saturate(roughness) * 254)` = lit) | Preserves exact 8-bit `f0` (including dielectric `0.04`) and exact `roughness` endpoints `0.0`, `0.5`, `1.0` (`1/254 ≈ 0.0039` step) |

Guide set shrinks from **8 images → 6 images** (`NormalRoughness`, `DiffuseAlbedo`, `SpecAlbedo`, `Emissive`, `Motion`, `Surface`):

| Metric | Before | After (O4) | Delta |
|---|---:|---:|---:|
| `RAB_GetGBufferSurface` loads | 5 textures (36 B) | **2 textures (20 B)** (`kGuideSurface` + `kGuideDiffuseAlbedo`) | **−60% loads, −44% bytes** |
| `resolve.slang` storage writes | 8 writes (52 B/px) | **6 writes (44 B/px)** | **−25% writes, −15% bytes** |
| Guide-set VRAM (2 sets) | 104 B/px | **88 B/px** | **−16 B/px** (−6.2 MiB at 853×480, −23.8 MiB at 1707×913) |

DLSS-RR's four guide inputs (`NormalRoughness`, `DiffuseAlbedo`, `SpecAlbedo`, `Motion`) stay untouched. `debug_view.slang` unpacks `geoNormal`, `specularF0`, and `isLit` from `kGuideSurface`.

### 2. Exact world-position reconstruction from `viewDepth` with jitter

`resolveSurface` evaluates triangle barycentrics at `pixelNdc = pixelToNdc(pixel + 0.5, targetSize)` against jittered clip corners `clip.xy + jitterOffset * clip.w`. Thus the unjittered clip coordinates of `surface.position` satisfy `c.xy / c.w = pixelNdc - jitterOffset` and `c.w = -v.z = viewDepth > 0`.

In `Camera::updateCameraData()` (`src/scene/vk_camera.cpp`), precompute three world-space ray basis vectors at unit `viewDepth` in `double` precision (using `invView`, `proj`, `invRenderTargetSize`, and `jitterOffset`), and carry the previous frame's basis (`prevRayOrigin`, `prevRayDx`, `prevRayDy`, equal to current on `isCutFrame`):

```cpp
// right = invView[0], up = invView[1], forward = -invView[2]
// invP00 = 1.0 / proj[0][0], invP11 = 1.0 / proj[1][1]  (proj[1][1] already Vulkan Y-flipped)
rayDx     = right * (2.0 * invSize.x * invP00);
rayDy     = up    * (2.0 * invSize.y * invP11);
rayOrigin = forward
          + right * ((-1.0 + invSize.x - jitterOffset.x) * invP00)
          + up    * ((-1.0 + invSize.y - jitterOffset.y) * invP11);
```

In `RAB_GetGBufferSurface(int2 pixel, bool previousFrame)` (3 FMAs, no matrix multiply):

```hlsl
const float3 rayDirScaled = origin + float(pixel.x) * dx + float(pixel.y) * dy;
surface.position = cameraPos + rayDirScaled * viewDepth;
surface.viewDir = normalize(-rayDirScaled);
```

Because `prevRay*` stores last frame's exact `invView`, `proj`, and `jitterOffset`, previous-frame temporal neighbor positions reconstruct to within fp32 precision (`~1e-6 · viewDepth`) with zero jitter mismatch and zero `cameraPos - position` cancellation in `viewDir`.

## Results

### 1. Paired ABBA benchmarks (pre-I9 vs O4)

RTX 5070, driver `621.01`, `windows-clang-release`, validation layers off, DLSS-RR Quality (`853×480` render at `1280×720`; `1707×913` render at `2560×1369`), true interleaved `A1 / B1 / B2 / A2` swapping pre-I9 (`A`) and O4 (`B`) binaries/shaders (`2400` measured frames at `1280×720`, `1200` at `2560×1369`). `Surface sum (6)` is `Vis-buffer resolve + DI (initial + temporal + shade) + GI (temporal + shade)`:

| Scenario | `Resolve` | `DI temp` | `DI shade` | `GI temp` | `GI shade` | `Surface sum (6)` | `GPU total` |
|---|---:|---:|---:|---:|---:|---:|---:|
| Sponza gallery `1280×720` (shipped) | 0.0963 → 0.0971 (+0.8%) | 0.0579 → **0.0523** (−9.7%) | 0.0325 → **0.0306** (−6.0%) | 0.1134 → **0.1089** (−4.1%) | 0.4907 → **0.4529** (**−7.7%**) | 0.9016 → **0.8523** (**−0.049 ms, −5.5%**) | 2.7976 → **2.7424** (**−0.055 ms, −2.0%**) |
| Sponza gallery `1280×720` (`di=nvidia gi=nvidia`) | 0.0966 → 0.0972 (+0.5%) | 0.0603 → **0.0539** (−10.5%) | 0.0892 → **0.0725** (**−18.8%**) | 0.1180 → **0.1138** (−3.6%) | 0.4362 → **0.3775** (**−13.4%**) | 0.9111 → **0.8238** (**−0.087 ms, −9.6%**) | 2.7994 → **2.7096** (**−0.090 ms, −3.2%**) |
| Sponza courtyard `1280×720` (shipped) | 0.1004 → 0.1005 (0.0%) | 0.0985 → **0.0916** (−6.9%) | 0.1280 → **0.1264** (−1.3%) | 0.1156 → **0.1098** (−5.0%) | 0.5588 → **0.5227** (**−6.5%**) | 1.1746 → **1.1245** (**−0.050 ms, −4.3%**) | 3.2075 → **3.1498** (**−0.058 ms, −1.8%**) |
| Sponza gallery `2560×1369` (`1707×913` render) | 0.2787 → **0.2757** (−1.1%) | 0.3528 → **0.2994** (**−15.1%**) | 0.2229 → **0.1927** (**−13.6%**) | 0.4296 → **0.4126** (−4.0%) | 1.8169 → **1.7024** (**−6.3%**) | 3.5121 → **3.2936** (**−0.218 ms, −6.2%**) | 9.6723 → **9.4560** (**−0.216 ms, −2.2%**) |
| Bistro street `1280×720` (shipped) | 0.0608 → **0.0576** (−5.2%) | 0.0510 → **0.0473** (−7.4%) | 0.0268 → **0.0241** (−10.1%) | 0.0507 → 0.0523 (+3.3%) | 0.2256 → **0.2100** (**−6.9%**) | 0.6184 → **0.5926** (**−0.026 ms, −4.2%**) | 2.4214 → **2.3274** (**−0.094 ms, −3.9%**) |

Every scenario wins cleanly across `A1/A2` vs `B1/B2` (`GPU total` **−0.055 to −0.216 ms**, `ReSTIR GI shade` **−6.3% to −13.4%**, `ReSTIR DI shade` up to **−18.8%** when spatial reuse is active), plus **−16 B/px** VRAM across the two guide sets. O4 lands.

### 2. Verification

- `ctest --test-dir build/windows-clang-release`: **8/8 passed** (including `layout`, verifying `GpuCameraData` 592 B, `GpuGuideSet` 48 B, `GpuTargets` 200 B, and `kImageFormatRgba32ui`).
- `ENGINE_GUIDE_CAPTURE=1`: **2/2 normal checks** (`mean 0.004° / max 0.040°`, `mean 0.017° / max 0.066°`) + **3/3 DI convergence checks** passed.
- `ENGINE_GI_CHECK=1`: **14/14 passed** (gallery & courtyard: ReSTIR GI converged/shipped, NRC + ReSTIR GI, NRC mode, ground-truth floors).
- `ENGINE_MOTION_TEST=1`: **13/13 passed**.
- `ENGINE_RT_CHECK=1`: **32/32 passed** (`0` misses/mismatches).
- `ENGINE_CULL_CHECK=1`: **16/16 passed**.
- `ENGINE_STABILITY_CHECK=1`: **28/28 passed (0 failed)** (`courtyard shipped Lit` temporal std `0.99%`).
- `ENGINE_LIGHT_CHANGE_CHECK=1`: **32/32 passed (0 failed)**.
- `ENGINE_SCREEN_EDGE_CHECK=1`: **80/80 passed (0 failed)**.
- Vulkan validation (`core + sync + best practices + NVIDIA best practices`, `GPUAV=false`, `ENGINE_INDIRECT_MODE=cycle`): **0 engine warnings/errors**.

### 3. Files changed

- `src/core/types.hpp`, `shaders/types.slang`, `tests/layout_test.cpp` (`GpuCameraData` ray basis, `GuideImage::Surface`, `kGuideImageCount = 6`, `kImageFormatRgba32ui`)
- `src/scene/vk_camera.{hpp,cpp}` (double-precision `rayOrigin`/`rayDx`/`rayDy` and `prevRay*`)
- `src/render/vk_render_targets.cpp` (6-guide table, `eR32G32B32A32Uint` `Surface` guide)
- `shaders/core/binding.slang`, `shaders/core/math.slang` (`kImageFormatRgba32ui`, `uint4Tex2D`, `packOct16`/`unpackOct16`, `packSurfaceMaterial`/`unpackSurfaceMaterial`)
- `shaders/passes/resolve.slang`, `shaders/lighting/rtxdi_bridge.slang`, `shaders/passes/debug_view.slang`
- `docs/engine_spec.md` §9.3, `spec/fixesM1-6/{m8_optimization_and_fixes.md,README.md}`
