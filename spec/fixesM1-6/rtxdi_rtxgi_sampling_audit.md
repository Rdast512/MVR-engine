# RTXDI and RTXGI sampling audit

Reviewed 2026-11-20 against RTXDI 3.2 and the RTXGI 2.9 Indirect Diffuse sample. The engine pins RTXDI-Library at `b67f61c` and NRC at `7de30e4` (0.16). This audit covers ReSTIR DI, ReSTIR GI, and the RTXGI NRC training and query paths. The related M8 measurements are in [m8_optimization_and_fixes.md](./m8_optimization_and_fixes.md).

The comparison draws on four NVIDIA references. [RTXDI integration guide](https://github.com/NVIDIA-RTX/RTXDI/blob/v3.2.0/Doc/Integration.md), [ReSTIR GI](https://github.com/NVIDIA-RTX/RTXDI/blob/v3.2.0/Doc/RestirGI.md), [Noise and Bias](https://github.com/NVIDIA-RTX/RTXDI/blob/v3.2.0/Doc/NoiseAndBias.md), and the [RTXGI NRC guide](https://github.com/NVIDIA-RTX/RTXGI/blob/v2.9.0/Docs/NrcGuide.md).

## Correctness findings

### High · Reflection rays can cross the geometric surface (fixed)

The primary GI bounce and the shared scatter used by NRC training and reference paths previously accepted a BRDF sample whenever its PDF was positive. A normal map can tilt the shading normal enough for a direction to lie above it and below the triangle's geometric normal. Both `rasterRayOrigin` and `safeSpawnPoint` then pushed the ray origin to the far side of the triangle, where it could collect light from behind the reflecting surface. Fixed to match NVIDIA's GI sample (`dot(geoNormal, brdf.l) > 0.0`): rejected primary GI directions contribute zero radiance while still counting as a sample (`M = 1`) in the reservoir, and `scatterBrdf` terminates the bounce. [Primary bounce](../../shaders/passes/indirect.slang#L61) · [Shared scatter](../../shaders/lighting/path_shading.slang#L106) · [NVIDIA hemisphere check](../../reference/RTXDI/Samples/FullSample/Shaders/LightingPasses/BrdfRayTracing.hlsl#L134)

### High · GI boiling filter reads unwritten reservoir padding (fixed)

At the usual render width of 853 pixels, the last 8-wide workgroup has three columns of lanes outside the image. The initial pass skips their writes, while the block-padded VMA reservoir allocation (16×16 blocks) is uninitialized, so `CheckedBuffer` passed those in-allocation padding addresses. Fixed in `rtxdi_gi_temporal.slang` by initializing out-of-bounds lanes to `RTXDI_EmptyGIReservoir()` and skipping `RTXDI_LoadGIReservoir` for them while keeping all lanes in the boiling filter's group synchronization. [Initial write guard](../../shaders/passes/indirect.slang#L146) · [Temporal load and filter](../../shaders/passes/rtxdi_gi_temporal.slang#L34) · [Reservoir allocation](../../src/render/vk_rtxdi.cpp#L92)

### Medium · DI temporal visibility uses geometry from two frames (fixed)

The DI temporal visibility callback previously traced from `previousSurface` through the current TLAS. Fixed to match NVIDIA's non-previous-TLAS fallback (`RAB_VisibilityTest.hlsli`): `RAB_LightSample` now carries `position` for local lights, and `RAB_GetTemporalConservativeVisibility` traces from `currentSurface` through the current TLAS (keeping `sample.l` for infinite lights and recomputing direction and distance to `sample.position` for local lights). [Engine DI callback](../../shaders/lighting/rtxdi_bridge.slang#L386) · [Engine GI callback](../../shaders/lighting/rtxdi_bridge.slang#L483) · [NVIDIA visibility fallback](../../reference/RTXDI/Samples/FullSample/Shaders/LightingPasses/RtxdiApplicationBridge/RAB_VisibilityTest.hlsli#L68)

### Medium · Sharp GGX peak lost precision (fixed)

The original audit reproduced a **0.21%** error in both `f × cos θ` and the solid-angle PDF for the off-axis glossy case at roughness `0.05`. The GGX denominator subtracted nearly equal values near the peak. BRDF evaluation and PDF evaluation shared the affected term, so their consistency check passed while both analytic assertions failed. Fixed by evaluating `1 - (N·H)²` with `cross(n, h)` in `ggxD`. [GGX evaluation](../../shaders/material/brdf.slang#L36) · [Analytic test case](../../tests/brdf_test.cpp#L258)

### Low · Ray cone double-counted bounce distance on training and reference paths (fixed)

`decodeHit` (`shaders/geometry/hit.slang:124`) already adds `cone.spread * hit.t` to `cone.width`, so `shadeRayHit` expects the path length from the camera to `ray.Origin`. `indirect.slang` passed the origin distance correctly, whereas `nrc_update.slang` and `referencePathRadiance` in `path_shading.slang` added `hit.t` before calling `shadeRayHit`, double-counting the segment length and sampling a coarser mip on long bounces. Fixed both callers to pass `pathLength` before adding `hit.t`. [Hit decode](../../shaders/geometry/hit.slang#L124) · [GI hit sampling](../../shaders/passes/indirect.slang#L169) · [NRC hit sampling](../../shaders/passes/nrc_update.slang#L128) · [Reference path](../../shaders/lighting/path_shading.slang#L149)

## Sampling behavior against NVIDIA's sample

- **ReSTIR DI.** The engine uses initial RIS, temporal reuse, spatial reuse, and final visibility. Uniform local-light selection is valid for the current punctual-light path. NVIDIA's Medium preset uses ReGIR for local candidates, which matters as the light count and scene size grow. The engine's default BRDF candidate spends a ray even though its bridge cannot return a mesh light or environment sample (tracked in `C-R32-3`). More light candidates for Sponza's lone delta sun cannot increase the number of indirect rays that reach sunlit geometry. [DI defaults](../../src/render/vk_rtxdi.cpp#L140) · [Light bridge](../../shaders/lighting/rtxdi_bridge.slang#L409) · [M7 scope](../m7/README.md)
- **ReSTIR GI.** The engine stores the secondary hit's position, normal, and outgoing radiance. The inverse of the primary ray's solid-angle PDF initializes the reservoir weight. Final shading applies the reservoir weight and uses NVIDIA's glossy blend with the original sample. The secondary hit retains full material gloss while the reservoir carries a single radiance value across viewing directions. NVIDIA's sample defaults secondary roughness to at least `0.5` to limit sharp, view-dependent contributions. [Final shading](../../shaders/passes/rtxdi_gi_shade.slang#L36) · [NVIDIA roughness default](../../reference/RTXDI/Samples/FullSample/Source/RenderPasses/LightingPasses/LightingPasses.cpp#L49)
- **RTXGI NRC.** The training path matches NVIDIA's order for recording hits, adding local lighting, and passing the scatter PDF to the SDK. The query path disables the spread heuristic and always queries the first secondary hit. NVIDIA uses the heuristic to trace farther when the path footprint is still small, such as through smooth reflections or in corners. Fixed query placement could contribute to the previously measured **~10% bright bias** in NRC modes. Its contribution remains unmeasured. The default ReSTIR GI mode pauses NRC. [Engine query](../../shaders/passes/indirect.slang#L130) · [NVIDIA termination heuristic](https://github.com/NVIDIA-RTX/RTXGI/blob/v2.9.0/Docs/NrcGuide.md#path-termination-heuristic) · [Recorded NRC bias](../considerations.md#m4--g5-restir-gi)

## Verification

Verified on the RTX 5070 (release) after all fixes above:
- **CTest:** 7 of 7 passed (`hiz_footprint`, `brdf`, `lights`, `rtxdi_math`, `layout`, `nrc_library`, `dlss`).
- **`ENGINE_GUIDE_CAPTURE=1`:** passed (`EXIT_CODE=0`, 14.8 s) — normal checks 2 of 2, ReSTIR DI convergence 2 of 2 (`LightsPunctualLamp_front` bias `+0.111%` / p90 `1.82%`, `Sponza_terminator` bias `0.000%` / p90 `0.00%`).
- **`ENGINE_GI_CHECK=1`:** passed (`EXIT_CODE=0`, 139 s) — 8 of 8 gated phases passed (gallery traced `+0.46%` / p90 `3.32%`, gallery 1-bounce `+0.11%` / p90 `11.21%`, courtyard traced `+1.08%` / p90 `4.89%`, courtyard 1-bounce `+0.39%` / p90 `10.36%`; 0 non-finite/negative pixels and `0.000%` dark-tile leak). Note: rejecting rays below `geoNormal` reduced gallery ground-truth luminance from `139.5` to `133.3` (removing back-side normal-map light leak) and cut gallery max tile error from `23.23%` to `11.20%`.
