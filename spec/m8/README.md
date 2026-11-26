# M8 · Unified Vulkan RTXPT / RTXDI / RTXGI Pipeline, Audits, Toggles, SHaRC & Perf

**Date:** 2026-11-25 · **Status:** planned (supersedes the earlier M8 optimization-and-fixes track in [spec/fixesM1-6](../fixesM1-6/README.md), whose landed fixes I1–I10 and I13–I16 remain in the tree)
**Goal:** Refocus the engine on its core mission: **one unified modern Vulkan 1.4+ renderer that reproduces NVIDIA's RTXPT, RTXDI, and RTXGI demos via runtime toggles — and beats them in FPS** using modern Vulkan extensions (`VK_EXT_descriptor_heap`, BDA, `VK_EXT_shader_object`, `VK_EXT_device_generated_commands`, `VK_EXT_mesh_shader`, `VK_NV_cluster_acceleration_structure`, `VK_NV_partitioned_acceleration_structure`, `VK_EXT_opacity_micromap`, `VK_EXT_ray_tracing_invocation_reorder` once Slang fixes EXT `HitObject`, async compute overlap) and lean shader execution.
**Links:** [Engine spec](../../docs/engine_spec.md) · [PBR plan](../pbr_implementation_plan.md) · [Shader plan](../shader_architecture_plan.md) · [Resource architecture](../resource_architecture.md) · [Settings guide](../../docs/settings.md) · [Considerations](../considerations.md) · [Prior M1–M7 fixes log](../fixesM1-6/README.md)

---

## 0. Current situation (2026-11-25)

1. **Solid foundation already built (M0–M4, M6–M7, plus `fixesM1-6` I1–I10, I13–I16):**
   - Visibility buffer + reverse-Z + 3-level Hi-Z + 2-phase GPU culling + DGC mesh-shader raster + DGC compute material binning (`Background` / `Standard` / `Unlit`).
   - RTX Mega Geometry: per-cluster CLAS (Spatial builder, OMM/opaque split), per-primitive cluster BLAS, GPU-written PTLAS updated in place on async compute, OMM SDK CPU bake + 2-state ray contract.
   - Lighting & reconstruction: glTF GGX VNDF BRDF, punctual + emissive-triangle lights, `Power_RIS` and `ReGIR_RIS` presampling, RTXDI 3.2 ReSTIR DI & ReSTIR GI, NRC 0.16 (with `C-G3-7` leak workaround, `C-G3-6` isolated SDK command buffers, `C-I16-5` divergence guard), thin-walled glass (`I13`), DLSS-RR, histogram exposure + ACES, and `SettingsWindow` with denoiser `Off` + `Accumulation` (`nvidia_settings_layout.md`).
2. **What went wrong during `fixesM1-6` I14–I16:**
   - Chasing light popping and boiling under DLSS-RR led into fighting ReSTIR's inherent temporal/spatial correlation (pre-denoiser clamp, input EMA/accumulation hacks, 32 per-pixel RIS candidates, O11 ReSTIR PT decorrelation ported onto ReSTIR GI).
   - **Key finding:** ReSTIR GI and spatial/long-history ReSTIR DI feed temporally correlated blotches that DLSS-RR cannot denoise cleanly, while ReSTIR GI alone costs ~0.85–0.95 ms per frame. Uncorrelated 1-bounce indirect PT + a radiance cache tail + short-history/uncorrelated direct lighting matches what DLSS-RR is actually trained for and aligns with NVIDIA's `RTXPT` and `RTXGI` designs.
3. **Current gaps this milestone closes:**
   - No full code audit yet comparing our BRDF, Mega Geometry, ReSTIR DI, ReSTIR GI, NRC, and NRC + ReSTIR GI directly against the full NVIDIA reference suite (including the newly added `reference/RTXPT-main`).
   - The new `SettingsWindow` (`3d24416c6` + uncommitted NVIDIA-defaults switch) has not been audited end-to-end to prove every ImGui control mutates live GPU state and resets history/accumulation properly (`C-NS-1`, `C-NS-6`).
   - Pipeline stages cannot be toggled independently: Direct Lighting cannot be disabled; Streaming RIS is conflated with `isReference` (which overrides candidate counts); Indirect Bounce 1 cannot be disabled; Bounce 1, ReSTIR GI, and Radiance Cache are conflated into a 3-way `IndirectMode { NrcRestirGi, Nrc, RestirGi }` enum, making raw 1-bounce PT without cache/GI or cache-swapped configurations impossible.
   - SHaRC was deleted in `C-G3-1` (2026-11-17) even though `ThirdParty/_deps/src/sharc` is still present and `reference/RTXPT-main` ships a full Vulkan/D3D12 SHaRC v1.9.0 integration.
   - Open correctness/regression items: `C-GA-1` / `C-T-6` (16-frame RT check miss after loading a large model), `C-NS-1` (GPU gates for `Off` / `Accumulation` / `Compare with`), `C-I14-5` (gallery NRC gate spread), `C-M0-1` (RTX 2060 Super floor tier measurements).

---

## 1. New 1.0 target lighting stack (all composable toggles)

Every stage below is built on the existing visibility-buffer + Mega Geometry + OMM base and controlled by runtime toggles with zero-cost skipping when disabled:

| Stage | Target default (1.0) | Runtime toggle options | Reference |
|---|---|---|---|
| **Denoiser + Upscaler** | **DLSS-RR** (Quality on both tiers) | `DLSS-RR` \| `Off` (`No AA` / `Accumulation`) | `reference/vk_denoise_dlssrr`, `reference/RTXPT-main`, `reference/RTXDI` |
| **Direct Lighting** | **RTXDI:** either **Streaming RIS** (Initial Sampling only) or **ReSTIR DI** with **Temporal ON (Max History = 4–8), `Basic` bias correction, Spatial OFF** *(default pick between the two decided by M8.5 measurements)* | `Off` \| `Streaming RIS` \| `ReSTIR DI` (`Temporal` / `Spatial` / `Temporal + Spatial` / `Reference`) + NVIDIA presets (`Fast`–`Ultra`) | `reference/RTXPT-main/Rtxpt/Shaders/IntroSample/RTXDIStreamingRISLightingPass.hlsl`, `IntroReSTIRDI_*.hlsl`, `reference/RTXDI` |
| **Indirect Lighting (Bounce 1)** | **Raw uncorrelated 1-bounce PT** (1 probabilistic diffuse-or-specular ray/pixel, OMM enabled, SER when Slang adds EXT `HitObject`, `ReGIR_RIS` / `Power_RIS` or simple NEE for bounce lights) | `Off` \| `Raw 1-Bounce PT` \| `ReSTIR GI` (temporal + spatial GI resampling over bounce-1 samples) | `reference/RTXPT-main` (`IntroSample/IntroPathTracer.hlsl`, `PathTracer/PathTracer.hlsli`), `reference/RTXGI`, `reference/RTXDI` |
| **Multi-Bounce Tail (Radiance Cache)** | **NRC** integrated into the pipeline | `Off` (1-bounce only) \| `NRC` \| `SHaRC` | `reference/RTXGI` (NRC 0.16), `reference/RTXPT-main/Rtxpt/Sharc/`, `ThirdParty/_deps/src/sharc` |

### One engine, four switchable demo-analog presets

No separate renderers or code forks — switching presets only flips runtime pipeline toggles:

| Preset | Direct | Indirect Bounce 1 | Multi-Bounce Cache | Denoiser | NVIDIA comparison target |
|---|---|---|---|---|---|
| **Engine Target** (shipped) | Streaming RIS *or* ReSTIR DI Temporal (`Max History 4–8`, `Basic`, Spatial OFF) | Raw 1-Bounce PT | NRC *(or SHaRC)* | DLSS-RR | Beat all three demos below in FPS |
| **RTXPT Analog** | Streaming RIS / NEE | Raw 1-Bounce PT | SHaRC | DLSS-RR | `reference/RTXPT-main` (`Rtxpt` & `IntroSample`) |
| **RTXDI Analog** | ReSTIR DI (`Medium` / Temporal+Spatial) | ReSTIR GI | Off | DLSS-RR | `reference/RTXDI/Samples/FullSample` |
| **RTXGI Analog** | Streaming RIS / ReSTIR DI | Raw 1-Bounce PT | NRC | DLSS-RR | `reference/RTXGI/Samples/IndirectDiffuse` |

---

## 2. Steps

Each step gets its own detailed spec in `spec/m8/` before implementation begins, grounded in the local NVIDIA reference sources.

| Step | Spec | Scope | Depends on |
|---|---|---|---|
| **M8.1 · Pipeline audit vs NVIDIA demos** | [`m8_1_pipeline_audit.md`](./m8_1_pipeline_audit.md) (audited; fix pass pending) | Deep code audit of current **BRDF**, **Mega Geometry + OMM**, **ReSTIR DI**, **ReSTIR GI**, **NRC**, and **NRC + ReSTIR GI** against `RTXPT-main`, `RTXDI`, `RTXGI`, `RTXMG-main`, `vk_lod_clusters`, `vk_partitioned_tlas-main`, `vk_denoise_dlssrr`, `vk_gltf_renderer`. Fix verified bugs (including `C-GA-1` / `C-T-6` 16-frame miss) and record every intentional deviation. | — |
| **M8.2 · ImGui & settings wiring audit** | `m8_2_imgui_audit.md` | Audit every control in `SettingsWindow` and `EngineUi` end-to-end (UI → `Renderer` / `RtxdiContext` / `NrcCache` / `Dlss` / `PostProcess` / `RayTracingScene` → GPU constants & shaders). Verify history/accumulation resets, disabled-row truth, presets, and `Compare with`. Close `C-NS-1` GPU gates and remove/isolate stale I16-only hacks. | M8.1 |
| **M8.3 · Composable pipeline toggle system** | `m8_3_pipeline_toggles.md` | Replace fixed-on DI and 3-way `IndirectMode` with independent runtime toggles: Direct (`Off` / `Streaming RIS` / `ReSTIR DI`), Indirect Bounce 1 (`Off` / `Raw 1-Bounce PT` / `ReSTIR GI`), Cache (`Off` / `NRC` / `SHaRC` placeholder), Denoiser (`DLSS-RR` / `Off`), plus one-click Engine Target / RTXPT / RTXDI / RTXGI analog presets. Disabled stages skip GPU dispatches, barriers, and transient allocations. Wire into `SettingsWindow`, `SettingsRegistry`, `ENGINE_CONFIG`, and check runners. | M8.2 |
| **M8.4 · SHaRC multi-bounce cache toggle** | `m8_4_sharc_cache.md` | Re-integrate NVIDIA SHaRC (`ThirdParty/_deps/src/sharc`) as a pure Vulkan 1.4 BDA + Slang compute toggle beside NRC, ported from `reference/RTXPT-main/Rtxpt/Sharc/` and `IntroSample/SHaRC*.hlsl`: enable `shaderBufferInt64Atomics`, lazy BDA hash/accum/resolved buffers, sparse `sharc_update` (1 per 4×4 block), `sharc_resolve`, X₂ query in `indirect.slang`, SHaRC debug view, and UI controls. | M8.3 |
| **M8.5 · Target lighting stack & Vulkan performance optimization** | `m8_5_perf_and_nvidia_benchmark.md` | Implement/tune the lean target fast-paths (fused Streaming RIS without reservoir round-trip; Temporal-only Basic ReSTIR DI without spatial neighbor traffic; Raw 1-Bounce PT writing `indirectRadiance` directly without GI reservoir round-trip when ReSTIR GI is off; ReGIR vs Power_RIS vs simple NEE at X₂). Apply modern Vulkan 1.4 + shader optimizations (occupancy, register count, BDA load chains, async compute overlap, minimal barriers, lazy buffers). Run head-to-head ABBA + FPS benchmarks against NVIDIA `RTXPT`, `RTXDI FullSample`, and `RTXGI IndirectDiffuse` on Sponza and Bistro. | M8.3, M8.4 |
| **M8.6 · Floor tier & full verification** | `m8_6_verification_and_floor.md` | Full runner sweep across all toggle combinations and demo-analog presets (validation core + sync + best practices + GPU-AV, `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST`, `ENGINE_GUIDE_CAPTURE`, `ENGINE_GI_CHECK`, `ENGINE_STABILITY_CHECK`, `ENGINE_DLSS_CHECK`). Record per-stage GPU + CPU timings and FPS comparisons on both RTX 5070 (reference) and RTX 2060 Super (floor, folding in old `I11`). | M8.5 |

### Execution order

```
M8.1 (pipeline audit + fixes)
  └─► M8.2 (ImGui & settings audit + C-NS-1)
        └─► M8.3 (composable pipeline toggles + Streaming RIS + Raw 1-Bounce PT fast path)
              ├─► M8.4 (SHaRC cache toggle beside NRC)
              └─► M8.5 (target stack tuning + Vulkan/shader perf + beat NVIDIA demos FPS bench)
                    └─► M8.6 (full toggle verification + RTX 5070 & 2060 Super tables)
```

---

## 3. Audit checklists (M8.1 & M8.2)

### M8.1 · Rendering pipeline audit scope

| Subsystem | Engine files to audit | NVIDIA reference files | Key questions |
|---|---|---|---|
| **BRDF & Materials** | `shaders/material/{brdf,pbr}.slang`, `shaders/lighting/lights.slang` | `RTXPT-main/Rtxpt/Shaders/PathTracer/BSDFs/{StandardBSDF,Microfacet,Fresnel}.hlsli`, `IntroSample/IntroCommon.hlsli`, `vk_gltf_renderer` | VNDF sampling & PDF exactness, lobe selection probability & clamping, energy conservation, Fresnel, `NdotL` / `NdotV` guards, `spec-gloss`, secondary-hit simplified vs full material cost |
| **Mega Geometry & OMM** | `src/core/vk_mega_geometry.{hpp,cpp}`, `src/core/omm_baker.{hpp,cpp}`, `src/render/vk_rt_scene.{hpp,cpp}`, `shaders/geometry/hit.slang`, `shaders/passes/rt_instances.slang`, `shaders/raytracing/trace.slang` | `RTXMG-main`, `vk_lod_clusters`, `vk_partitioned_tlas-main`, `RTXPT-main/Rtxpt/OpacityMicroMap/` | CLAS/BLAS build flags & sizing, PTLAS in-place update sync & partition reassignment, `C-GA-1`/`C-T-6` 16-frame miss root cause, `SafeSpawnPoint`, ray cones, OMM 2-state ray flag, zero-candidate invariant |
| **ReSTIR DI & Streaming RIS** | `src/render/vk_rtxdi.{hpp,cpp}`, `shaders/lighting/rtxdi_{bridge,constants}.slang`, `shaders/passes/rtxdi_{presample,initial,temporal,shade}.slang` | `RTXDI/Samples/FullSample/`, `RTXPT-main/Rtxpt/Shaders/IntroSample/{RTXDIStreamingRISLightingPass,IntroReSTIRDI_*}.hlsl` | Light buffer packing & PDF, `Power_RIS` vs `ReGIR_RIS` cell sizing/bias, initial visibility, temporal reprojection & `Basic` bias correction, bounds checks, reservoir lifetime when Spatial/Temporal are off |
| **ReSTIR GI** | `shaders/passes/rtxdi_gi_{temporal,shade,duplication}.slang`, `shaders/lighting/rtxdi_bridge.slang` | `RTXDI/Samples/FullSample/Shaders/LightingPasses/{BrdfRayTracing,ShadeSecondarySurfaces}.hlsl`, `GI/*.hlsl` | Surface reconstruction from packed guide, Jacobian validation, reservoir age/history handling, O11 deviations vs stock library |
| **NRC & NRC + ReSTIR GI** | `src/render/vk_nrc.{hpp,cpp}`, `shaders/gi/nrc_{bridge,constants}.slang`, `shaders/passes/{nrc_update,indirect,nrc_resolve}.slang` | `RTXGI/Samples/IndirectDiffuse/`, `RTXGI/Docs/NrcGuide.md`, `ThirdParty/_deps/src/nrc/Include/` | Sparse training path selection, `includeDirectLighting = false` + `C-G3-7` sentinel, query placement & spread heuristic, `maxExpectedAverageRadianceValue`, `C-G3-6` isolation overhead, `C-I16-5` divergence, NRC + Raw 1-Bounce PT vs NRC + ReSTIR GI composition |

### M8.2 · ImGui & settings audit scope

- Enumerate every widget in `src/ui/settings_window.cpp` and `src/ui/engine_ui.cpp`.
- Trace each widget's variable through `Renderer::drawFrame`, `FrameData::write`, `RtxdiContext::prepareFrame`, `NrcCache::prepareFrame`, `Dlss::evaluate`, `PostProcess::record`, `RayTracingScene`, and into the consuming Slang shader.
- Verify:
  1. Changing the widget changes the GPU output (no dead or overwritten fields).
  2. Changing any setting that invalidates temporal state resets DLSS-RR history, ReSTIR DI/GI history, and `Accumulation` frame count cleanly.
  3. Disabled rows display the true runtime value (never hardcoded stale numbers).
  4. `Preset` / `Compare with` / `Copy config` / `ENGINE_CONFIG` round-trip accurately (`tests/lab_registry_test.cpp` + live GPU verification).

---

## 4. Performance mandate & acceptance criteria

Performance is a hard requirement (engine spec §1.1, §2.3, §6 of `vulkan-dev` skill):
- Every pass has a `GpuTimerScope` and a Tracy CPU + GPU zone.
- Every optimization is measured on a **Release build with vkconfig and all validation layers off**, GPU at P0 clocks, ABBA rounds, on both Sponza and Bistro.
- **No paid work for disabled toggles:** turning off DI, Indirect, ReSTIR GI, NRC, SHaRC, or DLSS-RR must drop that stage's GPU time to 0 ms (no dummy dispatches, no redundant barriers, lazy buffer allocation/retirement for optional caches and accumulation targets).
- **Direct FPS comparison against NVIDIA demos:** record matched-scene, matched-resolution GPU frame time and FPS against NVIDIA's `RTXPT` (Vulkan & D3D12), `RTXDI FullSample`, and `RTXGI IndirectDiffuse` on the reference GPU (RTX 5070).

### M8 is done when

- [ ] M8.1 pipeline audit report (`m8_1_pipeline_audit.md`) is complete and all verified correctness/sync bugs (including `C-GA-1` / `C-T-6`) are fixed.
- [ ] M8.2 ImGui audit report (`m8_2_imgui_audit.md`) is complete, every control is verified end-to-end, and `C-NS-1` GPU gates pass.
- [ ] M8.3 composable toggles (`Direct`, `Indirect Bounce 1`, `Radiance Cache`, `Denoiser`) and demo-analog presets (`Engine Target`, `RTXPT`, `RTXDI`, `RTXGI`) switch cleanly at frame boundaries with zero validation errors and zero GPU cost for disabled stages.
- [ ] M8.4 SHaRC multi-bounce cache works as a runtime toggle beside NRC on pure Vulkan 1.4 BDA + Slang compute, with convergence and cost recorded.
- [ ] M8.5 target lighting stack (DLSS-RR + Streaming RIS or ReSTIR DI Temporal-only `Max History 4–8` / `Basic` / Spatial OFF + Raw 1-Bounce PT + NRC/SHaRC) meets spec §2.3 budgets and beats NVIDIA's reference demos in matched-config FPS on the RTX 5070 (with numbers recorded for Sponza and Bistro).
- [ ] M8.6 verification passes across all runners (`ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST`, `ENGINE_GUIDE_CAPTURE`, `ENGINE_GI_CHECK`, `ENGINE_STABILITY_CHECK`, `ENGINE_DLSS_CHECK`, `ctest`), validation (core + sync + best practices + GPU-AV) is clean, and RTX 5070 + RTX 2060 Super tables are recorded (or 2060 Super pending hardware availability noted in `considerations.md`).

---

## 5. Disposition of prior `spec/fixesM1-6/` items

| Prior item | Disposition |
|---|---|
| `I1–I10`, `I13`, `I14`, `I15`, `I16` (landed fixes) | Retained in the tree as the baseline M8 builds on; history stays in [spec/fixesM1-6](../fixesM1-6/README.md). |
| `I16` (open ReSTIR boiling/popping hacks) | **Closed / superseded** by the target architecture shift away from ReSTIR GI and spatial/long-history ReSTIR DI. |
| `I12` (many-light & glossy re-check) | Folded into **M8.5** on Bistro under the new target stack and demo-analog presets. |
| `I11` (RTX 2060 Super floor tier) | Folded into **M8.6**. |
| `nvidia_settings_layout.md` (`C-NS-1`, `C-NS-6`) | Audited and closed in **M8.2** and **M8.3**. |

---

## 6. References

- `reference/RTXPT-main`: `Docs/Design.md`, `Docs/intro-sample.md`, `Rtxpt/Sharc/`, `Rtxpt/Shaders/IntroSample/` (`RTXDIStreamingRISLightingPass.hlsl`, `IntroReSTIRDI_*.hlsl`, `IntroPathTracer.hlsl`, `SHaRC*.hlsl`), `Rtxpt/Shaders/PathTracer/` (`PathTracer.hlsli`, `PathTracerSHaRC.hlsli`, `BSDFs/`).
- `reference/RTXDI`: `Samples/FullSample/`, `Doc/RestirGI.md`, `Doc/NoiseAndBias.md`, `ThirdParty/_deps/src/rtxdi` (3.2).
- `reference/RTXGI`: `Samples/IndirectDiffuse/`, `Samples/Pathtracer/`, `Docs/NrcGuide.md`, `Docs/SharcGuide.md`, `ThirdParty/_deps/src/nrc` (0.16), `ThirdParty/_deps/src/sharc`.
- `reference/RTXMG-main`, `reference/vk_lod_clusters`, `reference/vk_partitioned_tlas-main`: Mega Geometry CLAS / PTLAS.
- `reference/vk_denoise_dlssrr`, `reference/vk_gltf_renderer`: DLSS-RR guides & glTF PBR.
