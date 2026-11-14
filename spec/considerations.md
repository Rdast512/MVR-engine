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

- **C-E1-1 · Deferred · `lodBias` is 0.** It is `log2(render / display)`, and render equals display until H1 sets a render resolution. **Exit:** H1.
- **C-E1-2 · Deferred · jitter is off by default.** Halton jitter is built, but the display shimmers by half a pixel until DLSS-RR resolves it. `ENGINE_JITTER=1` turns it on. **Exit:** H1.

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
- **C-F3-3 · Deferred · permutation sampling is off.** It raised the frame-to-frame change from 0.8% to 20.5% of the signal. Turning it back on also needs `SetSpatioTemporalResamplingParameters` every frame, because RTXDI 3.1's `SetFrameIndex` rehashes only the temporal pass's random number. **Exit:** H1 decides with DLSS-RR in the loop.
- **C-F3-4 · Deferred · no BRDF candidates (`numBrdfSamples` 0).** No BRDF ray can hit a light until emissive triangles exist. **Exit:** M7, with emissive triangles and `Power_RIS`.
- **C-F3-5 · Deferred · no presampling, no emissive triangles, no environment light.** **Exit:** M7 for presampling and emissive triangles; the environment light is unscheduled.
- **C-F3-6 · Unverified · stability under motion.** Convergence and stability were measured on a static camera only. **Exit:** a moving-camera and moving-light check, for example within M3's "Sponza + sun" goal.
- **C-F3-7 · Unverified · full-screen cost.** No lit full-screen scene exists yet. On Sponza without lights, before the no-light skip, the passes cost 0.05 + 0.25 ms at 1280×720 on the RTX 5070. A lit frame costs more, since it adds light evaluation and shadow rays. The RTX 2060 Super has not been measured. **Exit:** timings on Sponza + sun on both GPUs.
- **C-F3-8 · Limitation · no permanent convergence regression check.** The F3 comparison against `Shadowed lights` used temporary instrumentation in the guide capture. **Exit:** a permanent check in the guide capture, needed again for `Power_RIS` in M7.
- **C-F3-9 · Limitation · the `ReSTIR DI initial` GPU timer includes the TLAS wait.** It is the first span of the ray command buffer, so any wait for the async TLAS build lands in it. **Exit:** a separate wait span, if the number ever matters.

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
| C-F3-R1 | M3 · F3 | `BestPractices-AMD-ClearAttachment-ClearImage-color`: with no lights, `directRadiance` was cleared with `vkCmdClearColorImage` | 2026-11-14: `rtxdi_shade` writes zeros itself when no light is loaded (a uniform early-out), so the image needs no transfer usage |
