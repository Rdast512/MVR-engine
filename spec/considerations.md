# Considerations: open problems and current workarounds

Every known unsolved problem, accepted limitation and workaround in the engine, each stamped with the milestone and plan step it belongs to (`M3 · F3` = milestone M3, [PBR plan](./pbr_implementation_plan.md) step F3). A new problem or workaround is added here when it is found. A solved one moves to **Resolved** with its date and fix.

Kinds:
- **Workaround:** code that works around a defect elsewhere, usually a library. It stays until that defect is fixed.
- **Limitation:** a known gap in what is built.
- **Deferred:** scheduled for a later step.
- **Unverified:** not yet measured or tested.
- **Warning:** a validation message that is not fixed.

---

## M0 · contracts and the audit's leftovers

Reviewed 2026-11-14 against the code, after M3's H2. The M0 work handed to M1–M3 is done: jitter (E1), `rayInstances` and `tlas` (C2, C4), `frameIndex`, and `exposure`, which stays 1 by design (C-H2-1). The work waiting for M4 and later is on schedule: `cacheConstants` (M4), the logged SER hint (M4), the cached OMM properties (M6), `GpuMaterialExtension` (M7), the logged decompression methods (M8), and exact list sizing (A10, post-1.0 LOD).

- **C-M0-1 · Unverified · M0's checks on the RTX 2060 Super.** Baseline per-stage timings and the culling image exist only for the RTX 5070 (engine spec §2.3, §16.2). The 2060 Super is not in the dev machine. **Exit:** both recorded on the 2060 Super; M0 closes then.
- **C-M0-2 · Limitation · swapchain recreation has two paths (audit AU10, optional, scheduled before M2).** Still as the audit found:
  - Every `SDL_EVENT_WINDOW_RESIZED` rebuilds at once (`Engine::recreateSwapchain`), several times during a window drag. Since H1 and H2 each rebuild also recreates the DLSS-RR feature, reallocates RTXDI's reservoirs (63 MiB at 1280×720) and snaps the exposure.
  - The renderer's out-of-date and suboptimal path (`Renderer::drawFrame`, after acquire and after present) recreates without `EngineUi::onSwapchainRecreated`, so ImGui's image count and format can go stale.
  - `SwapChain::recreateSwapChain` destroys the old swapchain before it creates the new one and passes no `oldSwapchain`.

  **Exit:** AU10: a resize sets a flag, and one recreate function runs once per frame, passes the old swapchain and updates ImGui.
- **C-M0-3 · Limitation · third-party code is compiled into two DLLs (audit AU8, optional, scheduled before M2).** `engine_core` and `engine_util` both link the static `ThirdPartyImpl`, so `stbi_*` is defined in `libengine_core.dll` and in `libengine_util.dll` (VMA only in util). The audit found five copies. `ENGINE_ENABLE_COVERAGE` still defaults to ON for Debug. A stale `libengine_postprocessing.dll` (2026-10-21) sits in the release build folder; no target builds it. **Exit:** link `ThirdPartyImpl` into `engine_util` only, default coverage to OFF, and delete the stale DLL.
- **C-M0-4 · Limitation · the per-slot color-space rules are duplicated (audit AU6, optional, scheduled before M2).** `tools/gltf_ktx2/main.cpp` keeps its own `isSrgbSlot` ("matches the engine loader") beside `assets_loader.cpp`'s rules. **Exit:** one shared rule set; the file split of AU6 stays with M8.
- **C-M0-5 · Limitation · per-frame CPU work is O(entities) (audit AU4, optional, was M2).** `FrameData::write` rewrites every node instance and object row every frame and computes each entity's model matrix twice (`writeNodeInstances` and the entity loop). **Exit:** AU4's GPU scene-update pass fed by a CPU delta list, at the latest with M8's loading work.
- **C-M0-6 · Limitation · docs still describe M0 placeholders.**
  - PBR plan §4.2: the frame root row says `tlas` (0), which is the PTLAS address since C4, and the struct listing calls `exposure` "H2 pre-exposure, fed to DLSS-RR".
  - `shaders/types.slang`: `GpuFrameRoot.exposure` is commented "pre-exposure, 1 until auto exposure"; auto exposure exists and the field stays 1 on purpose.
  - `spec/features/visibility_buffer.md` V1 still says A4 is not done and morph targets exist.
  - Engine spec §16.1: B3 "no pass calls it until RTXDI; not committed yet", and "DLSS, NRC: linkage tests only".

  **Exit:** correct them.

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
- **C-F3-3 · Deferred · permutation sampling is off.** It raised the frame-to-frame change from 0.8% to 20.5% of the signal. Turning it back on also needs `SetSpatioTemporalResamplingParameters` every frame, because RTXDI 3.1's `SetFrameIndex` rehashes only the temporal pass's random number. H1 did not decide it: no test scene lit enough of the frame. Since E3 the sunlit `Sponza` courtyard does. **Exit:** an A/B through DLSS-RR on that view.
- **C-F3-4 · Deferred · no BRDF candidates (`numBrdfSamples` 0).** No BRDF ray can hit a light until emissive triangles exist. **Exit:** M7, with emissive triangles and `Power_RIS`.
- **C-F3-5 · Deferred · no presampling, no emissive triangles, no environment light.** **Exit:** M7 for presampling and emissive triangles; the environment light is unscheduled.
- **C-F3-7 · Unverified · full-screen cost on the 2060 Super.** On the RTX 5070 the sunlit default scene costs 0.14 ms (initial) + 0.10 ms (shade) at the 853×480 render size, mean of a 6000-frame bench (2026-11-14). The RTX 2060 Super has not been measured. **Exit:** the same bench on the 2060 Super (with C-M0-1).
- **C-F3-8 · Limitation · no permanent convergence regression check.** The F3 comparison against `Shadowed lights` used temporary instrumentation in the guide capture. **Exit:** a permanent check in the guide capture, needed again for `Power_RIS` in M7.
- **C-F3-9 · Limitation · the `ReSTIR DI initial` GPU timer includes the TLAS wait.** It is the first span of the ray command buffer, so any wait for the async TLAS build lands in it. **Exit:** a separate wait span, if the number ever matters.

## M3 · H1 DLSS Ray Reconstruction

- **C-H1-1 · Workaround · NGX needs the driver's descriptor heap.** NGX's kernels address images by `VK_NVX_image_view_handle` handles, which index the driver-managed descriptor heap. While the engine's `VK_EXT_descriptor_heap` heap is bound, they index that heap instead. DLSS-RR then reads and writes whatever image sits in the engine slot with the same number: a guide image was overwritten every frame (motion test 6 of 13), and with the reserved range moved to the heap start the output stayed black. A new command buffer does not reset this. `Dlss::evaluate` binds an empty legacy descriptor set (`vkCmdBindDescriptorSets2` with a one-set pipeline layout) right before the evaluation, which returns the driver to its own heap. DXVK does the same (`DxvkContext::launchCuKernelNVX`, "no spec-legal way to bind it"). DXVK's bind without a layout crashes driver 621.01 inside `nvoglv64.dll`. The renderer rebinds both heaps afterwards. **Exit:** a spec-legal way to select the driver heap, or an NGX that does not use image view handles.
- **C-H1-2 · Warning · NGX's own validation messages.** These come from inside `NGX_VULKAN_EVALUATE_DLSSD_EXT` and pipeline creation, with no engine command in between:
  - `SYNC-HAZARD-WRITE-AFTER-WRITE` twice, on the first evaluation after each feature creation. NGX transitions two of its own images (`nv.ngx.dlssd.resource`) and then clears them without a barrier. NVIDIA's developer forum has a report of the same clear.
  - `BestPractices-SpirvDeprecated_WorkgroupSize` three times, from NGX's compute pipelines.

  The other per-frame hazard, NGX's fills against the previous evaluation's writes, is fixed by the engine's barrier before the evaluation. **Exit:** a DLSS SDK that fixes them.
- **C-H1-3 · Workaround · `--wrap=malloc` and MSVC-built static libraries.** The engine's modules link with `--wrap=malloc/calloc/realloc/free/aligned_alloc` to route allocations to mimalloc. `nvsdk_ngx_d.lib` is MSVC `/MD` code and calls `malloc` and `free` through `__imp_malloc` and `__imp_free`. lld redirects those to `__imp___wrap_*`, and its fallback local import left all 9 call sites with a zero displacement (crash in `NGXGetPathUsingQAI`). `src/util/mi_new_delete.cpp` defines the `__imp___wrap_*` slots, and that object is compiled without LTO, since bitcode slots did not resolve either. Checked with clang 22.1.8's lld. **Exit:** drop `--wrap`, or an lld that resolves the fallback.
- **C-H1-6 · Limitation · the debug views are not upscaled.** A debug view writes the display image at render resolution, and the DLSS-RR evaluation is skipped, so returning to Lit resets its history. **Exit:** none planned; debug views show render-resolution data on purpose.

## M3 · E3 engine sun

- **C-E3-1 · Limitation · the startup view gets no direct sun.** The startup camera looks at the origin from the +Z gallery, under the arcade roof and behind the curtains. A sweep of 18 sun directions lit at most 0.6% of that view, so Lit starts nearly black until M4's indirect light. The camera was left where it is, because the bench and the timing history in engine spec §2.3 use that pose. **Exit:** M4 (G6) lights the gallery by bounce; or a courtyard startup pose, with the bench history re-baselined.
- **C-E3-2 · Limitation · DLSS-RR quality depends on jitter and mode.** On the sunlit courtyard with a still camera, the temporal deviation of the Lit output is 0.35% of the mean at Quality and 0.31% at Performance, but 0.88% at DLAA (8 jitter phases), and 8.7% at Quality with the `Jitter` checkbox off: RR needs the jitter. **Exit:** none planned; the checkbox stays a debug control.

## M3 · H2 post

- **C-H2-1 · Limitation · the color is not pre-exposed.** `GpuFrameRoot.exposure` and DLSS-RR's `InPreExposure` stay 1, and the exposure reaches DLSS-RR only as the 1×1 exposure texture. NGX takes pre-exposure as a CPU float, which would need the GPU exposure read back a frame or more late. The RGBA16F `noisyColor` had enough range on the test scenes. **Exit:** a scene whose radiance nears the FP16 limits (65504 at the top), then pre-expose by a read-back value.
- **C-H2-2 · Limitation · DLSS-RR sees last frame's exposure.** The exposure pass runs after the evaluation, so the exposure texture DLSS-RR reads was written by the previous frame's post. While adapting, it differs by `1 − e^(−dt·rate)` of the gap, under 3% per frame at 60 Hz and the default rate. **Exit:** none planned unless RR shows exposure lag.
- **C-H2-3 · Limitation · auto exposure ignores black pixels.** Bin 0 holds pixels below 2^−16 and is not metered, so a mostly unlit frame is exposed for its lit part (on `Sponza` + `LightsPunctualLamp`, about 7% of the pixels). The EV range bounds it. **Exit:** a scene where dark areas should pull the exposure up, then meter them with a floor.

## M3 · D4 visibility rays

- **C-D4-1 · Deferred · `traceVisibility` lacks part of D4.** It was built early with F2, but D4's candidate debug counter, `pointOffset` for shadow origins and `minT` bias are still open (feature spec §6.1). **Exit:** D4.

## M4 · lighting defaults and screen-edge convergence (M3 features)

Reported by the user on 2026-11-14 after M3 closed; stamped M4, the current milestone.

- **C-M4-1 · Unverified · lighting fades in at the screen edges.** Turning so that geometry enters from a screen edge, or moving backwards, shows the lighting visibly appearing in the newly revealed border. Likely cause, not yet measured: those pixels have no history. ReSTIR DI's temporal back-projection lands off screen, so they start from the initial candidates plus spatial neighbors (`disocclusionBoostSamples` at the library default), and DLSS-RR has no accumulated history there either, so they converge over several frames. **Exit:** a scripted pan and backward dolly that measures how many frames the border band needs to come within a set tolerance of its converged value; then tune `disocclusionBoostSamples`, the initial candidate counts and `maxHistoryLength`, and add a guard band (rendering a margin beyond the viewport) if tuning is not enough.
- **C-M4-2 · Deferred · the default settings of RTXDI, DLSS-RR and RTXGI are not tuned.**
  - RTXDI: the library's defaults (`RtxdiContext::defaultSettings`), except Pairwise MIS (C-F3-2) and permutation sampling off (C-F3-3).
  - DLSS-RR: Quality mode and the `Default` preset (the driver picks), checked only for still-camera stability (C-E3-2).
  - RTXGI: no pass exists yet; NRC comes with G3 and ReSTIR GI with G5, so the panel shows no settings.

  **Exit:** tune RTXDI and DLSS-RR on sunlit `Sponza` with the pose-repeat measurement of C-F3-6 plus C-M4-1's border measurement, and record the chosen defaults and their numbers in the PBR plan. RTXGI's defaults are tuned with G3 and G5.

## M4 · G3 NRC

- **C-G3-1 · Deferred · SHaRC code is still in the tree.** NRC became the only radiance cache on 2026-11-14 (NVIDIA-only target). Still present:
  - `Device` picks NRC when its extensions exist and otherwise falls back to SHaRC, enabling SHaRC's 64-bit atomic features (`vk_device.cpp` `kSharcFeatures`, `GiCache::Sharc` in `types.hpp`). NRC's extensions are therefore still optional at device creation.
  - `shaders/gi/sharc.slang`, `tests/sharc_test.cpp`, `tests/shaders/sharc_roundtrip.slang`, the `sharc` fetch in `ThirdParty/CMakeLists.txt` and its Slang include directory.

  **Exit:** delete them, and make NRC's extensions required at device creation, no later than G3.
- **C-G3-2 · Unverified · NRC on the RTX 2060 Super.** The pinned SDK names no minimum GPU but can return `UnsupportedHardware`. The floor tier's 0.6 ms cache budget was set for SHaRC (engine spec §2.3, R4). **Exit:** NRC runs on the 2060 Super and the cache row is measured there in G3.

## Tooling and validation environment

- **C-T-3 · Limitation · the motion test does not support jitter.** With `ENGINE_JITTER=1` it fails 7 of 13: the static, entity and teleport scenarios compare albedo pixel by pixel between frames, which jitter changes by design. The motion vectors themselves stay within 0.06 px. The test defaults jitter off; jittered runs of it are for validation coverage only.
- **C-T-1 · Limitation · GPU-AV changes results.** Under GPU-assisted validation the motion test's "camera yaw, sky in view" fails with 0.87 px and 2 outlier pixels, giving 12 of 13. This was checked on 2026-11-13: it comes from GPU-AV, not the engine. GPU-AV also makes GPU timings meaningless. vkconfig can force it even with `VK_LOADER_LAYERS_DISABLE`, so set `VK_KHRONOS_VALIDATION_GPUAV_ENABLE=false` explicitly for pass/fail and timing runs.
- **C-T-2 · Warning · "legacy command" notices.** `WARNING-legacy-buffer-commands` and similar notices suggest `VK_KHR_device_address_commands` replacements for `vkCmdDispatchIndirect`, `vkCmdUpdateBuffer` and `vkCreateAccelerationStructureKHR`. They were seen with the vkconfig-forced validation settings on 2026-11-14, not with the engine's own validation environment. Not scheduled. **Exit:** adopt the extension, once it is available on the target drivers.

---

## Resolved

| ID | Stamp | Problem | Fixed |
|---|---|---|---|
| C-H1-5 | M3 · H1 | DLSS-RR was checked only on a sparsely lit frame (no lights in `Sponza`) | 2026-11-14: E3's sun lights the courtyard. At Quality on a still camera the Lit output's temporal deviation is 0.35% of the mean (0.2% of lit pixels above 10%); captured frames during a yaw show no ghosting. Needs a look on screen by the user |
| C-F3-6 | M3 · F3 | stability measured on a static camera only | 2026-11-14: a camera yawing ±8.6° (60-frame period) and a sun swinging ±5.7° in azimuth, both on the sunlit courtyard at Quality: frames at repeated poses differ by 2.4% and 1.5% of the mean. Jitter off gives the same 2.4% under motion, so it is RR's and ReSTIR's history, not jitter |
| C-C4-1 | M2 · C4 | the TLAS was a full per-slot build, and partitioned AS was optional at device creation | 2026-11-14: partitioned AS is required; one PTLAS is updated in place from GPU-written instance writes |
| C-C4-2 | M2 · C4 | no ray set the OMM 2-state flag, which PTLAS instances cannot carry | 2026-11-14: `traceVisibility` and `traceClosest` set `RAY_FLAG_FORCE_OMM_2_STATE` (see C-C4-3) |
| C-H1-7 | M3 · H1 | below DLAA, the debug views and Lit without lights (unlit `Sponza`) boiled: jittered at render resolution and scaled up with nothing resolving the jitter | 2026-11-14: `Camera::updateCameraData` jitters only frames DLSS-RR evaluates (`Renderer::isDlssPresented()`); `ENGINE_JITTER=1` forces it for the culling checks |
| C-H1-4 | M3 · H1 | DLSS-RR used its own auto exposure | 2026-11-14: H2's exposure image is passed as `pInExposureTexture`, and the feature is created without `AutoExposure` |
| C-E1-1 | M3 · E1 | `lodBias` was 0 while render equalled display | 2026-11-14: `Camera` sets `log2(render / display)` from DLSS-RR's render resolution (H1) |
| C-E1-2 | M3 · E1 | jitter was off by default | 2026-11-14: on by default, resolved by DLSS-RR (H1). `ENGINE_JITTER=0` turns it off; the motion test and the guide capture default to off, since they compare frames pixel by pixel |
| C-F3-R1 | M3 · F3 | `BestPractices-AMD-ClearAttachment-ClearImage-color`: with no lights, `directRadiance` was cleared with `vkCmdClearColorImage` | 2026-11-14: `rtxdi_shade` writes zeros itself when no light is loaded (a uniform early-out), so the image needs no transfer usage |
