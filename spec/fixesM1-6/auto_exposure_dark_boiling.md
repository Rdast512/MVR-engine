# Fix: Auto-exposure boiling in shaded (indirect-only) areas

**Date:** 2026-11-20 · **Status:** diagnosed, ready for implementation · **Milestone:** [M8](../m8/README.md) (fixes M3 H2 / H1 + M4 G5)
**Why:** Turning on auto exposure in Sponza areas with no direct sunlight (the +Z gallery, under the arches) causes severe temporal boiling. In the gallery, `indirectRadiance` has a mean of ~`0.00019` linear ([m8_optimization_and_fixes.md](./m8_optimization_and_fixes.md) §2.6): auto exposure amplifies it by ~1000× (+10 EV) and up to 65,536× (+16 EV), feeding that extreme, fluctuating gain back into DLSS-RR.

**Sources checked**

- Engine: `shaders/passes/exposure.slang`, `shaders/passes/post.slang`, `shaders/passes/indirect.slang`, `src/render/vk_post.{hpp,cpp}`, `src/render/vk_dlss.cpp`, `src/render/vk_renderer.cpp`, `src/render/vk_rtxdi.{hpp,cpp}`, `src/render/vk_world_lights.cpp`.
- RTXDI sample: `reference/RTXDI/Samples/FullSample/Source/App/SceneRenderer.cpp` (`GetDLSSSRParams`, `ToneMapping`), `Source/UserInterface.h`, `Shaders/LightingPasses/ShadeSecondarySurfaces.hlsl`.
- RTXGI 2.9.0 sample: `reference/RTXGI/Samples/Shared/PostProcess/AutoExposurePass.{h,cpp}`, `Shared/Shaders/{AutoExposure.hlsl,AutoExposureCb.h,Tonemapping.hlsl}`, `IndirectDiffuse/{Pathtracer.cpp,UI.h}`.
- `vk_denoise_dlssrr`: `reference/vk_denoise_dlssrr/dlss_rr/src/dlssrr_wrapper.cpp`.
- `vk_gltf_renderer`: `reference/vk_gltf_renderer/src/dlss_wrapper.cpp`, `src/renderer.cpp`.

---

## 1. Root causes vs NVIDIA references

| Aspect | Engine today | NVIDIA references | Effect in shaded Sponza |
|---|---|---|---|
| **Max exposure gain / luminance floor** | `maxEv = +16.0` (65,536×), `kExposureMinLog2 = -16.0` (`1.5e-5`), `vk_post.hpp:28-29` | **RTXDI** (`SceneRenderer.cpp:1783`): `minAdaptedLuminance = 0.002f` (≈ −9 EV → max ~90× / +6.5 EV, ~+5.5 EV with `exposureBias = -1.0`). **RTXGI** (`AutoExposurePass.h:32`): `minLogLuminance = -12.0f` (~+9.5 EV cap) | Engine boosts `~0.00019` gallery radiance by +10 to +16 EV, dragging sub-percent 1-bounce noise into bright mid-tones |
| **Black pixels in histogram** | `exposure.slang:21-29, 73`: pixels `< 2^-16` go to `bin 0`, and `bin 0` is **excluded** from `total` (C-H2-3) | **RTXGI** (`AutoExposure.hlsl:50-80`): dark/sub-floor pixels clamp into `bin 0` and **count** in `pixelCount`; `lowPercentile = 0.40` drops the bottom 40% naturally | When most pixels are near black, the 40th–95th percentile meters only the sparse, noisy lit fraction, causing frame-to-frame target exposure jitter |
| **Histogram bin quantization** | Nearest/floor bin (`exposure.slang:28`) | **RTXGI** (`AutoExposure.hlsl:54-61`): 2-bin linear splat with 6 fractional bits | Adds quantization step jitter (~0.125 EV) when sparse pixels straddle bin boundaries |
| **Adaptation speed** | Symmetric `1 - exp(-dt * 1.5)` (`exposure.slang:107`) | **RTXDI** (`SceneRenderer.cpp:1786-1787`): up `2.0`, down `1.0`. **RTXGI** (`AutoExposurePass.h:36-37`, `AutoExposure.hlsl:116-117`): up `3.0`, down `1.0` with `exp2(-dt * speed)` | Darkening is ~2.2× faster than RTXDI/RTXGI, amplifying pumping |
| **DLSS-RR `pInExposureTexture`** | Passes `targets.exposure()` (up to 65,536×) into `NGX_VULKAN_EVALUATE_DLSSD_EXT` (`vk_dlss.cpp:294`), metered from last frame's DLSS-RR output (`upscaledSampled`) | **`vk_denoise_dlssrr`** (`dlssrr_wrapper.cpp:271,358`) and **`vk_gltf_renderer`** (`dlss_wrapper.cpp:441,543`): `pInExposureTexture = nullptr`, `AutoExposure` flag off on RR. **RTXDI**: clamps at `minAdaptedLuminance = 0.002f` (`dlssExposureScale = 2.0f`). **RTXGI** (`Pathtracer.cpp:1504-1507`, `AutoExposurePass.cpp:150-156`): notes RR's denoised output is metered after RR with `minLogLuminance = -12.0f`, black pixels counted, and `(1 - 0.18)` scale alignment | **Primary boiling amplifier:** scaling `noisyColor` by 100×–10,000× inside DLSS-RR pushes sparse ReSTIR GI variance past DLSS-RR's temporal rejection thresholds every frame; metering DLSS-RR's boiling output back into `pInExposureTexture` closes a pumping feedback loop |
| **Indirect signal density** | Delta sun only (`illuminance = 10`), no sky/environment light, 1-bounce ReSTIR GI (`NRC` paused) | **RTXDI** (`SceneRenderer.cpp:1144`): environment map lights miss rays. **RTXGI** (`UI.h:111,120-121`): constant sky `(0.5, 0.75, 1.0)` + multi-bounce NRC | Shaded Sponza has near-zero hit probability for 1-bounce rays reaching sunlit floor, producing high-variance blotches (`maxReservoirAge = 30`, permutation sampling on) |
| **Secondary firefly clamp** | `isFireflyClampEnabled` off by default (`vk_rtxdi.hpp:159`); when on, clamps at `10 / exposure` (`indirect.slang:187-196`) | **RTXDI** (`ShadeSecondarySurfaces.hlsl:34,133-135`): unconditionally clamps secondary radiance at `c_MaxIndirectRadiance = 10` in scene units | With auto exposure on, `10 / exposure` couples the clamp to the auto-exposure loop (at `exposure = 1000`, clamp drops to `0.01`, darkening and pumping the indirect pass) |

---

## 2. Diagnostic check before editing

Reproduce in the +Z gallery (startup Sponza pose, `Lit` view, `ReSTIR GI` mode):

1. Turn **Auto exposure ON**, let it settle, and record the displayed `Exposure` EV and `Average luminance` in the Post panel.
2. Turn **Auto exposure OFF** and set **Manual exposure** to that same EV.
   - Boiling that disappears under locked manual EV comes from the **auto-exposure feedback loop** (F1, F2, F3 below).
   - Boiling that remains at high manual EV comes from **extreme exposure gain feeding DLSS-RR `pInExposureTexture`** (F4) and **sparse 1-bounce ReSTIR GI noise** (F5).

---

## 3. Fixes

### F1 · Count dark pixels in `bin 0` and raise the histogram luminance floor (`shaders/passes/exposure.slang`, `src/core/types.hpp`)

Match RTXGI's `AutoExposure.hlsl`:

- Change `kExposureMinLog2` from `-16.0` to `-12.0` (`kExposureLog2Range = 24.0`, covering `[-12, +12]` EV like `AutoExposurePass::Settings`).
- Map finite non-negative luminances with `saturate((log2(max(lum, 0)) - minLog2) / range)` into bins `0..255` (skip only NaN/Inf). Use 2-bin linear splatting with 6 fractional bits (`c_FixedPointScale = 64`) so bin boundaries do not quantize-jitter the mean.
- Sum `total` over `bin = 0..255` (including dark/black pixels clamped to `bin 0`), instead of skipping `bin 0`. The `0.40..0.95` percentile window still ignores up to 40% black pixels, while scenes that are >40% dark smoothly approach the `minLog2` floor instead of chasing the brightest 1% of noisy pixels.
- Keep `readback.pixels` reporting finite metered pixels (or add a non-zero count if needed for the UI).

### F2 · Tighter default exposure clamp and asymmetric adaptation speeds (`src/render/vk_post.{hpp,cpp}`, `shaders/passes/exposure.slang`, `src/ui/engine_ui.cpp`)

Match RTXDI (`SceneRenderer.cpp:1783-1787`) and RTXGI (`AutoExposurePass.h:36-37`):

- Tighten default `PostProcess::Settings::maxEv` from `+16.0f` to **`+6.0f`** (64× max gain, close to RTXDI's `minAdaptedLuminance = 0.002f` → `+6.5 EV` / `+5.5 EV` with `-1 EV` bias). Keep the UI `Exposure range` control able to widen it manually when inspecting dark scenes.
- Split `adaptationRate` into `adaptationSpeedUp = 2.0f` and `adaptationSpeedDown = 1.0f` (in EV half-lives via `exp2(-dt * speed)`, as in RTXGI `AutoExposure.hlsl:116-117`), so dark dips cannot rapidly yank exposure upward on transient denoiser noise.

### F3 · Stabilize or decouple DLSS-RR's `pInExposureTexture` (`src/render/vk_dlss.{hpp,cpp}`, `src/render/vk_renderer.cpp`, `shaders/passes/exposure.slang`)

Test and pick one of two clean behaviors against the gallery + courtyard stability probe:

- **Option A (clamp DLSS exposure separately):** Keep `pInExposureTexture` wired for scenes with real dynamic range, but clamp the exposure value exposed to DLSS-RR (or enforce F2's `maxEv <= +6.0` ceiling + optional deadband) so DLSS-RR never receives extreme gains or frame-to-frame micro-oscillations. Also verify whether aligning the DLSS scale by `(1.0 - kKeyValue)` (`0.82`, per `AutoExposurePass::GetDlssExposureScale`) improves stability.
- **Option B (`pInExposureTexture = nullptr` on DLSS-RR):** Match `vk_denoise_dlssrr` and `vk_gltf_renderer` by not passing `pInExposureTexture` to DLSS-RR (keeping `NVSDK_NGX_DLSS_Feature_Flags_AutoExposure` off). Verify on both bright courtyard (`illuminance = 10` and `100`) and dark gallery poses that DLSS-RR temporal stability does not regress at high radiance.

Record A/B stability numbers (gallery + courtyard, auto exposure ON and OFF) before locking Option A vs B.

### F4 · Decouple O6 firefly clamp from auto exposure (`shaders/passes/indirect.slang`)

- Replace the exposure-multiplied clamp (`luminance(radiance) * exposure > 10.0`) with a fixed scene-unit ceiling matching RTXDI's `ShadeSecondarySurfaces.hlsl:34,133-135` (`kMaxIndirectRadiance = 10.0`, independent of `exposureStorage`).
- This removes the `exposure -> indirect.slang -> ReSTIR GI -> DLSS-RR -> exposure` feedback path when O6 is enabled (e.g. for M7/M8 many-light scenes).

### F5 · Longer-term signal quality in shaded Sponza (separate from exposure)

Not an exposure bug, noted so expectations stay clear:

- Without an environment/sky light (C-F3-5, currently unscheduled) and with 1-bounce ReSTIR GI (`NRC` paused for C-G5-3), deep shaded areas of Sponza have very sparse non-zero bounce hits.
- Fixing C-G5-3 (M8 I5, re-enabling multi-bounce NRC) and/or adding a sky/environment light will raise shaded-area hit density by an order of magnitude, eliminating the residual low-frequency blotchiness that high exposure reveals.

---

## 4. Done when

- On Sponza's shaded +Z gallery with **Auto exposure ON**:
  - Exposure settles smoothly within `maxEv` without visible frame-to-frame pumping (`Exposure` readback stable to ≤ 0.05 EV on a still camera).
  - `Lit` temporal stability in the gallery and sunlit courtyard (auto exposure ON) matches locked manual exposure at the same EV and shows no regression on the courtyard baseline ([m8_optimization_and_fixes.md](./m8_optimization_and_fixes.md) §2.5: std ≤ 1.6%, flickering pixels ≤ 5.0%).
- Existing checks stay green: `ENGINE_GUIDE_CAPTURE=1` (all shots + ReSTIR DI convergence), `ENGINE_GI_CHECK=1`, `ENGINE_MOTION_TEST=1`, `ENGINE_CULL_CHECK=1`, `ctest`, validation clean.
- Update [considerations.md](../considerations.md) (`C-H2-3` resolved by F1; `C-H2-2` updated if Option B is chosen).
