# Considerations: open problems and current workarounds

Every known unsolved problem, accepted limitation and workaround in the engine, each stamped with the milestone and plan step it belongs to (`M3 · F3` = milestone M3, [PBR plan](./pbr_implementation_plan.md) step F3). A new problem or workaround is added here when it is found. A solved one moves to **Resolved** with its date and fix.

Kinds:
- **Workaround:** code that works around a defect elsewhere, usually a library. It stays until that defect is fixed.
- **Limitation:** a known gap in what is built.
- **Deferred:** scheduled for a later step.
- **Unverified:** not yet measured or tested.
- **Warning:** a validation message that is not fixed.

---

## M1 · visibility buffer and reverse Z

- **C-M1-1 · Warning · `BestPractices-NVIDIA-CreateImage-Depth32Format` (0xf00e92a8).** Reverse Z needs float depth for its precision, so the message is accepted. A code comment in `RenderTargets::findDepthFormat` records why. **Exit:** none planned.

## Pre-M3 · DGC

- **C-DGC-1 · Warning · `BestPractices-vkBindBufferMemory-small-dedicated-allocation`.** `DgcPreprocess_0` and `DgcPreprocess_1` (128 KiB each) each get their own memory block instead of a suballocation. It is reported with performance warnings on (`REPORT_FLAGS` including `perf`). Seen 2026-11-14. **Exit:** suballocate the preprocess buffers.

## M2 · C4 PTLAS

- **C-C4-3 · Workaround · the OMM ray flag's SPIR-V capability is declared by hand.** Slang 2026.20 emits `RAY_FLAG_FORCE_OMM_2_STATE` (`0x400`) without `OpCapability RayTracingOpacityMicromapKHR`, and spirv-val rejects the module. `requireOmm2StateRayFlag()` in `shaders/raytracing/trace.slang` declares the extension and capability through `spirv_asm`; `traceVisibility` and `traceClosest` call it, and every new ray (the G1 SER trace) must too. **Exit:** a Slang version that emits the capability. Re-check with spirv-val on every Slang upgrade.
- **C-C4-4 · Limitation · a non-empty PTLAS update costs at least ~0.11 ms.** With every instance moving it is slower than the full build it replaced: 0.12 ms against 0.101 ms on Sponza, 0.18 ms against 0.150 ms on the side-64 field (RTX 5070). Still frames cost 0.015–0.019 ms. The global partition, other build flags and other grid sizes did not lower it (plan C4, Checked). It runs on async compute and overlaps the frame. **Exit:** re-measure on Bistro (M9, engine spec Q10) and the 2060 Super. Try the sample's near/far mode (global partition for far partitions only) if the update shows up in the frame.
- **C-C4-5 · Limitation · partitions are fixed per layout.** An instance keeps the grid cell of its position at the last layout change. One that travels far widens its partition's bounds, which may cost trace time; nothing measures it yet. **Exit:** reassign the partition when an instance leaves its cell, if Bistro or a gameplay scene shows a trace cost.

## M3 · E1 guides


## M3 · F1 RTXDI host context

- **C-F1-1 · Limitation · reservoir contents are undefined after a resize.** Correctness relies on the resize arriving as a camera cut, which resets history so the shade pass skips reuse for that frame. **Exit:** none needed while resize keeps resetting history. Recheck if the history-reset rules change.

## M3 · F2 RTXDI bridge

- **C-F2-1 · Limitation · no previous-frame light rows.** `RAB_LoadLightInfo(i, true)` returns this frame's row. Temporal reuse therefore evaluates a moving light at its new position, which biases reuse for moving lights. **Exit:** keep last frame's world light rows (E2 already writes one buffer per frame slot) and read them for `previousFrame`.
- **C-F2-2 · Workaround · RTXDI's implicit `uint`→`bool` warning (E30081) is disabled for each whole compile unit.** Units that include the bridge compile with `RTXDI_SLANG_FLAGS` (`-warnings-disable 30081`). Slang 2026.20 ignores `#pragma warning` scoping once a unit imports a module, so the warning cannot be limited to RTXDI's headers. **Exit:** a Slang version that scopes the pragma, or an RTXDI version without the conversion.
- **C-F2-3 · Limitation · the `DirectionalLight` sample asset cannot serve as a shadow-acne test.** Its spheres are wound inside out on single-sided materials, so its `Shadowed lights` view is black by design. **Exit:** use another asset for acne checks.

## M3 · F3 ReSTIR DI passes

- **C-F3-1 · Workaround · bounds-checked reads in the bridge.** RTXDI reads its buffers as structured buffers under robust access, where out-of-range reads return zeros:
  - the fused pairwise path loads the reservoir of an off-screen back-projected pixel;
  - it loads light 0 for an empty result, even when no lights exist.

  Through BDA both reads lost the device. The bridge's reservoir accessor and `RAB_LoadLightInfo` now check bounds. **Exit:** none expected. Any new RTXDI bridge (ReSTIR GI in G5, PT) must do the same.
- **C-F3-2 · Workaround · Pairwise MIS instead of the library default `Basic`.** RTXDI 3.1's fused `Basic` path skips naive neighbors (`discountNaiveSamples`) after marking them for its normalization. It measured 35% too dark on `LightsPunctualLamp`. **Exit:** an RTXDI release that fixes the fused `Basic` path. Re-measure on every RTXDI update.
- **C-F3-3 · Deferred · permutation sampling is off.** It raised the frame-to-frame change from 0.8% to 20.5% of the signal. Turning it back on also needs `SetSpatioTemporalResamplingParameters` every frame, because RTXDI 3.1's `SetFrameIndex` rehashes only the temporal pass's random number. H1 did not decide it: no test scene lights enough of the frame to judge it through DLSS-RR (C-H1-5). **Exit:** an A/B through DLSS-RR on a lit scene.
- **C-F3-4 · Deferred · no BRDF candidates (`numBrdfSamples` 0).** No BRDF ray can hit a light until emissive triangles exist. **Exit:** M7, with emissive triangles and `Power_RIS`.
- **C-F3-5 · Deferred · no presampling, no emissive triangles, no environment light.** **Exit:** M7 for presampling and emissive triangles; the environment light is unscheduled.
- **C-F3-6 · Unverified · stability under motion.** Convergence and stability were measured on a static camera only. **Exit:** a moving-camera and moving-light check, for example within M3's "Sponza + sun" goal.
- **C-F3-7 · Unverified · full-screen cost.** No lit full-screen scene exists yet. On Sponza without lights, before the no-light skip, the passes cost 0.05 + 0.25 ms at 1280×720 on the RTX 5070. A lit frame costs more, since it adds light evaluation and shadow rays. The RTX 2060 Super has not been measured. **Exit:** timings on Sponza + sun on both GPUs.
- **C-F3-8 · Limitation · no permanent convergence regression check.** The F3 comparison against `Shadowed lights` used temporary instrumentation in the guide capture. **Exit:** a permanent check in the guide capture, needed again for `Power_RIS` in M7.
- **C-F3-9 · Limitation · the `ReSTIR DI initial` GPU timer includes the TLAS wait.** It is the first span of the ray command buffer, so any wait for the async TLAS build lands in it. **Exit:** a separate wait span, if the number ever matters.

## M3 · H1 DLSS Ray Reconstruction

- **C-H1-1 · Workaround · NGX needs the driver's descriptor heap.** NGX's kernels address images by `VK_NVX_image_view_handle` handles, which index the driver-managed descriptor heap. While the engine's `VK_EXT_descriptor_heap` heap is bound, they index that heap instead. DLSS-RR then reads and writes whatever image sits in the engine slot with the same number: a guide image was overwritten every frame (motion test 6 of 13), and with the reserved range moved to the heap start the output stayed black. A new command buffer does not reset this. `Dlss::evaluate` binds an empty legacy descriptor set (`vkCmdBindDescriptorSets2` with a one-set pipeline layout) right before the evaluation, which returns the driver to its own heap. DXVK does the same (`DxvkContext::launchCuKernelNVX`, "no spec-legal way to bind it"). DXVK's bind without a layout crashes driver 621.01 inside `nvoglv64.dll`. The renderer rebinds both heaps afterwards. **Exit:** a spec-legal way to select the driver heap, or an NGX that does not use image view handles.
- **C-H1-2 · Warning · NGX's own validation messages.** These come from inside `NGX_VULKAN_EVALUATE_DLSSD_EXT` and pipeline creation, with no engine command in between:
  - `SYNC-HAZARD-WRITE-AFTER-WRITE` twice, on the first evaluation after each feature creation. NGX transitions two of its own images (`nv.ngx.dlssd.resource`) and then clears them without a barrier. NVIDIA's developer forum has a report of the same clear.
  - `BestPractices-SpirvDeprecated_WorkgroupSize` three times, from NGX's compute pipelines.

  The other per-frame hazard, NGX's fills against the previous evaluation's writes, is fixed by the engine's barrier before the evaluation. **Exit:** a DLSS SDK that fixes them.
- **C-H1-3 · Workaround · `--wrap=malloc` and MSVC-built static libraries.** The engine's modules link with `--wrap=malloc/calloc/realloc/free/aligned_alloc` to route allocations to mimalloc. `nvsdk_ngx_d.lib` is MSVC `/MD` code and calls `malloc` and `free` through `__imp_malloc` and `__imp_free`. lld redirects those to `__imp___wrap_*`, and its fallback local import left all 9 call sites with a zero displacement (crash in `NGXGetPathUsingQAI`). `src/util/mi_new_delete.cpp` defines the `__imp___wrap_*` slots, and that object is compiled without LTO, since bitcode slots did not resolve either. Checked with clang 22.1.8's lld. **Exit:** drop `--wrap`, or an lld that resolves the fallback.
- **C-H1-5 · Unverified · DLSS-RR is checked only on a sparsely lit frame.** `Sponza` has no lights, so Lit falls back to the unlit albedo there and DLSS-RR does not run. Even lit by the lamp below, the frame averages 1–1.6% of white after H2's exposure and tonemap, since the unlit 93% stays black. The `DirectionalLight` sample's sun points along −Z, horizontally, and lights no pixel of `Sponza`. The checks loaded `LightsPunctualLamp` beside it, which lights about 6% of the pixels. On that frame the jitter sign was measured (a still camera's frame-to-frame change is 0.000030 in tonemapped luminance, against 0.00064 with the opposite sign and 0.000025 with jitter off), and a yawing camera stays steadier with the chosen sign (0.00074 against 0.00109). The "stable under camera motion on Sponza" condition is therefore not verified on a lit Sponza, and no one has looked at it on screen yet. **Exit:** a lit Sponza (the M3 row's sun), checked on screen and by the same measurement.
- **C-H1-6 · Limitation · the debug views are not upscaled.** A debug view writes the display image at render resolution, and the DLSS-RR evaluation is skipped, so returning to Lit resets its history. **Exit:** none planned; debug views show render-resolution data on purpose.

## M3 · H2 post

- **C-H2-1 · Limitation · the color is not pre-exposed.** `GpuFrameRoot.exposure` and DLSS-RR's `InPreExposure` stay 1, and the exposure reaches DLSS-RR only as the 1×1 exposure texture. NGX takes pre-exposure as a CPU float, which would need the GPU exposure read back a frame or more late. The RGBA16F `noisyColor` had enough range on the test scenes. **Exit:** a scene whose radiance nears the FP16 limits (65504 at the top), then pre-expose by a read-back value.
- **C-H2-2 · Limitation · DLSS-RR sees last frame's exposure.** The exposure pass runs after the evaluation, so the exposure texture DLSS-RR reads was written by the previous frame's post. While adapting, it differs by `1 − e^(−dt·rate)` of the gap, under 3% per frame at 60 Hz and the default rate. **Exit:** none planned unless RR shows exposure lag.
- **C-H2-3 · Limitation · auto exposure ignores black pixels.** Bin 0 holds pixels below 2^−16 and is not metered, so a mostly unlit frame is exposed for its lit part (on `Sponza` + `LightsPunctualLamp`, about 7% of the pixels). The EV range bounds it. **Exit:** a scene where dark areas should pull the exposure up, then meter them with a floor.

## M3 · D4 visibility rays

- **C-D4-1 · Deferred · `traceVisibility` lacks part of D4.** It was built early with F2, but D4's candidate debug counter, `pointOffset` for shadow origins and `minT` bias are still open (feature spec §6.1). **Exit:** D4.

## M4 · G3 NRC

- **C-G3-1 · Deferred · SHaRC code is still in the tree.** NRC became the only radiance cache on 2026-11-14 (NVIDIA-only target). Still present:
  - `Device` picks NRC when its extensions exist and otherwise falls back to SHaRC, enabling SHaRC's 64-bit atomic features (`vk_device.cpp` `kSharcFeatures`, `GiCache::Sharc` in `types.hpp`). NRC's extensions are therefore still optional at device creation.
  - `shaders/gi/sharc.slang`, `tests/sharc_test.cpp`, `tests/shaders/sharc_roundtrip.slang`, the `sharc` fetch in `ThirdParty/CMakeLists.txt` and its Slang include directory.

  **Exit:** delete them, and make NRC's extensions required at device creation, no later than G3.
- **C-G3-2 · Unverified · NRC on the RTX 2060 Super.** The pinned SDK names no minimum GPU but can return `UnsupportedHardware`. The floor tier's 0.6 ms cache budget was set for SHaRC (engine spec §2.3, R4). **Exit:** NRC runs on the 2060 Super and the cache row is measured there in G3.

## Tooling and validation environment

- **C-T-1 · Limitation · GPU-AV changes results.** Under GPU-assisted validation the motion test's "camera yaw, sky in view" fails with 0.87 px and 2 outlier pixels, giving 12 of 13. This was checked on 2026-11-13: it comes from GPU-AV, not the engine. GPU-AV also makes GPU timings meaningless. vkconfig can force it even with `VK_LOADER_LAYERS_DISABLE`, so set `VK_KHRONOS_VALIDATION_GPUAV_ENABLE=false` explicitly for pass/fail and timing runs.
- **C-T-2 · Warning · "legacy command" notices.** `WARNING-legacy-buffer-commands` and similar notices suggest `VK_KHR_device_address_commands` replacements for `vkCmdDispatchIndirect`, `vkCmdUpdateBuffer` and `vkCreateAccelerationStructureKHR`. They were seen with the vkconfig-forced validation settings on 2026-11-14, not with the engine's own validation environment. Not scheduled. **Exit:** adopt the extension, once it is available on the target drivers.

---

## Resolved

| ID | Stamp | Problem | Fixed |
|---|---|---|---|
| C-C4-1 | M2 · C4 | the TLAS was a full per-slot build, and partitioned AS was optional at device creation | 2026-11-14: partitioned AS is required; one PTLAS is updated in place from GPU-written instance writes |
| C-C4-2 | M2 · C4 | no ray set the OMM 2-state flag, which PTLAS instances cannot carry | 2026-11-14: `traceVisibility` and `traceClosest` set `RAY_FLAG_FORCE_OMM_2_STATE` (see C-C4-3) |
| C-H1-7 | M3 · H1 | below DLAA, the debug views and Lit without lights (unlit `Sponza`) boiled: jittered at render resolution and scaled up with nothing resolving the jitter | 2026-11-14: `Camera::updateCameraData` jitters only frames DLSS-RR evaluates (`Renderer::isDlssPresented()`); `ENGINE_JITTER=1` forces it for the culling checks |
| C-H1-4 | M3 · H1 | DLSS-RR used its own auto exposure | 2026-11-14: H2's exposure image is passed as `pInExposureTexture`, and the feature is created without `AutoExposure` |
| C-E1-1 | M3 · E1 | `lodBias` was 0 while render equalled display | 2026-11-14: `Camera` sets `log2(render / display)` from DLSS-RR's render resolution (H1) |
| C-E1-2 | M3 · E1 | jitter was off by default | 2026-11-14: on by default, resolved by DLSS-RR (H1). `ENGINE_JITTER=0` turns it off; the motion test and the guide capture default to off, since they compare frames pixel by pixel |
| C-F3-R1 | M3 · F3 | `BestPractices-AMD-ClearAttachment-ClearImage-color`: with no lights, `directRadiance` was cleared with `vkCmdClearColorImage` | 2026-11-14: `rtxdi_shade` writes zeros itself when no light is loaded (a uniform early-out), so the image needs no transfer usage |
