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
- **C-M0-7 · Limitation · the Debug `gltf_ktx2` build aborts in libktx.** On `SheenCloth` it fails `pos == private->_levelIndex[level].byteOffset + baseOffset` (`KTXSoftware/lib/writer2.c:563`). UBSan also reports calls through the wrong function-pointer type, `ktxTexture2_Destroy` (`main.cpp:303`) and `ktxTexture2_WriteToNamedFile` (`main.cpp:414`), plus one in `writer2.c:244`. Release converts the same model correctly. Seen 2026-11-15 on HEAD `b1c5843af`; the cause has not been investigated. **Exit:** the Debug tool converts the glTF samples cleanly under the sanitizers.
- **C-M0-5 · Limitation · per-frame CPU work is O(entities) (audit AU4, optional, was M2).** `FrameData::write` rewrites every node instance and object row every frame and computes each entity's model matrix twice (`writeNodeInstances` and the entity loop). **Exit:** AU4's GPU scene-update pass fed by a CPU delta list, at the latest with M8's loading work.

## M1 · visibility buffer and reverse Z

- **C-M1-1 · Warning · `BestPractices-NVIDIA-CreateImage-Depth32Format` (0xf00e92a8).** Reverse Z needs float depth for its precision, so the message is accepted. A code comment in `RenderTargets::findDepthFormat` records why. **Exit:** none planned.

## M2 · C4 PTLAS

- **C-C4-3 · Workaround · the OMM ray flag's SPIR-V capability is declared by hand.** Slang 2026.20 emits `RAY_FLAG_FORCE_OMM_2_STATE` (`0x400`) without `OpCapability RayTracingOpacityMicromapKHR`, and spirv-val rejects the module. `requireOmm2StateRayFlag()` in `shaders/raytracing/trace.slang` declares the extension and capability through `spirv_asm`; `traceVisibility` and `traceClosest` call it, and every new ray (the G1 SER trace) must too. **Exit:** a Slang version that emits the capability. Re-check with spirv-val on every Slang upgrade.
- **C-C4-4 · Limitation · a non-empty PTLAS update costs at least ~0.11 ms.** With every instance moving it is slower than the full build it replaced: 0.12 ms against 0.101 ms on Sponza, 0.18 ms against 0.150 ms on the side-64 field (RTX 5070). Still frames cost 0.015–0.019 ms. The global partition, other build flags and other grid sizes did not lower it (plan C4, Checked). It runs on async compute and overlaps the frame. **Exit:** re-measure on Bistro (M9, engine spec Q10) and the 2060 Super. Try the sample's near/far mode (global partition for far partitions only) if the update shows up in the frame.
- **C-C4-5 · Limitation · partitions are fixed per layout.** An instance keeps the grid cell of its position at the last layout change. One that travels far widens its partition's bounds, which may cost trace time; nothing measures it yet. **Exit:** reassign the partition when an instance leaves its cell, if Bistro or a gameplay scene shows a trace cost.

## M3 · E1 guides


## M3 · F1 RTXDI host context

- **C-F1-1 · Limitation · reservoir contents are undefined after a resize.** Correctness relies on the resize arriving as a camera cut, which resets history so the shade pass skips reuse for that frame. **Exit:** none needed while resize keeps resetting history. Recheck if the history-reset rules change.

## M3 · F2 RTXDI bridge

- **C-F2-2 · Workaround · RTXDI's implicit `uint`→`bool` warning (E30081) is disabled for each whole compile unit.** Units that include the bridge compile with `RTXDI_SLANG_FLAGS` (`-warnings-disable 30081`). Slang 2026.20 ignores `#pragma warning` scoping once a unit imports a module, so the warning cannot be limited to RTXDI's headers. RTXDI 3.2 still has the conversion: 8 warnings in `TemporalResampling.hlsli`, `SpatialResampling.hlsli` and `Utils/Math.hlsli` (2026-11-15). **Exit:** a Slang version that scopes the pragma, or an RTXDI version without the conversion.
- **C-F2-3 · Limitation · the `DirectionalLight` sample asset cannot serve as a shadow-acne test.** Its spheres are wound inside out on single-sided materials, so its `Shadowed lights` view is black by design. **Exit:** use another asset for acne checks.

## M3 · F3 ReSTIR DI passes

- **C-F3-1 · Workaround · bounds-checked reads in the bridge.** RTXDI reads its buffers as structured buffers under robust access, where out-of-range reads return zeros:
  - the fused pairwise path loads the reservoir of an off-screen back-projected pixel;
  - it loads light 0 for an empty result, even when no lights exist.

  Through BDA both reads lost the device. The bridge's reservoir accessor and `RAB_LoadLightInfo` now check bounds. RTXDI 3.2 (2026-11-15) dropped the fused path and guards the empty result in `RTXDI_TargetPdfHelper`, but still assumes robust buffers, so the checks stay. **Exit:** none expected. Any new RTXDI bridge (ReSTIR GI in G5, PT) must do the same.
- **C-F3-3 · Unverified · permutation sampling is on by default and off in the convergence check.** With 3.1's fused pass it raised the frame-to-frame change from 0.8% to 20.5% of the signal. With 3.2's separate passes it is worse: on `LightsPunctualLamp` temporal-only reuse goes from 4.29% to 43.4% per-frame error and from +0.07% to −1.94% bias (2026-11-15). The library and the RTXDI sample keep it on, to decorrelate frames for denoisers, so since 2026-11-15 the engine default does too (`RtxdiContext::defaultSettings`); `convergedSettings` keeps it off. 3.2's `SetFrameIndex` rehashes the random number the temporal pass reads, so no extra setter call is needed. Since E3 the sunlit `Sponza` courtyard lights enough of the frame for an A/B. **Exit:** an A/B through DLSS-RR on that view decides whether the engine keeps NVIDIA's default.
- **C-F3-4 · Deferred · no BRDF candidates (`numBrdfSamples` 0).** No BRDF ray can hit a light until emissive triangles exist. **Exit:** M7, with emissive triangles and `Power_RIS`.
- **C-F3-5 · Deferred · no presampling, no emissive triangles, no environment light.** **Exit:** M7 for presampling and emissive triangles; the environment light is unscheduled.
- **C-F3-7 · Unverified · full-screen cost on the 2060 Super.** On the RTX 5070 the sunlit default scene costs 0.14 ms (initial) + 0.10 ms (shade) at the 853×480 render size, mean of a 6000-frame bench (2026-11-14). Since RTXDI 3.2: 0.15 ms (initial) + 0.07 ms (temporal) + 0.04 ms (spatial + shade) (2026-11-15). The RTX 2060 Super has not been measured. **Exit:** the same bench on the 2060 Super (with C-M0-1).
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

- **C-M4-1 · Unverified · lighting fades in at the screen edges.** Turning so that geometry enters from a screen edge, or moving backwards, shows the lighting visibly appearing in the newly revealed border. Likely cause, not yet measured: those pixels have no history. ReSTIR DI's temporal back-projection lands off screen, so they start from the initial candidates plus spatial neighbors (`disocclusionBoostSamples` at the library default), and DLSS-RR has no accumulated history there either, so they converge over several frames. Since RTXDI 3.2 (2026-11-15) the temporal pass's fallback sampling is on: a pixel whose history does not match reuses last frame's reservoir at the same pixel, which upstream aims at screen-edge disocclusions. It is not measured on a moving camera yet. **Exit:** a scripted pan and backward dolly that measures how many frames the border band needs to come within a set tolerance of its converged value; then tune `disocclusionBoostSamples`, the initial candidate counts and `maxHistoryLength`, and add a guard band (rendering a margin beyond the viewport) if tuning is not enough.
- **C-M4-2 · Deferred · the default settings of RTXDI, DLSS-RR and RTXGI are not tuned.**
  - Since 2026-11-15 every default is NVIDIA's own, for a cost comparison with their samples ([rtxdi_3_2_update.md](./features/rtxdi_3_2_update.md) §6.4).
  - RTXDI: the RTXDI sample's `Medium` preset (`RtxdiContext::defaultSettings`), with uniform local light sampling until ReGIR (M7). It fails the convergence check, which holds `convergedSettings` (C-R32-1).
  - DLSS-RR: Quality mode and the `Default` preset (the driver picks), as `vk_denoise_dlssrr`. The RTXDI sample renders at native resolution, the engine's DLAA. Checked only for still-camera stability (C-E3-2).
  - CLAS, cluster BLAS and PTLAS: fast trace and the minimum position truncation, as `vk_lod_clusters` and `vk_partitioned_tlas`.
  - How to tune each setting, and the stability of every variant measured on the sunlit courtyard on 2026-11-15: [docs/settings.md](../docs/settings.md).
  - RTXGI: no pass exists yet; NRC comes with G3 and ReSTIR GI with G5, so the panel shows no settings.

  **Exit:** tune RTXDI and DLSS-RR on sunlit `Sponza` with the pose-repeat measurement of C-F3-6 plus C-M4-1's border measurement, and record the chosen defaults and their numbers in the PBR plan. RTXGI's defaults are tuned with G3 and G5.

## M4 · RTXDI 3.2 update

Stamped M4, the current milestone; [rtxdi_3_2_update.md](./features/rtxdi_3_2_update.md).

- **C-R32-1 · Workaround · the NVIDIA defaults fail the convergence check, so the check holds its own settings.** Since 2026-11-15 the defaults are the RTXDI sample's `Medium` preset: 2 `Basic` spatial samples every frame and permutation sampling on (C-F3-3). On `LightsPunctualLamp` they read bias −1.34%, tile p90 26.4%, per-frame error 54.9%, frame-to-frame change 68.2%. Each of the two raises the tile error past the 3% bound on its own: per-frame spatial reuse feeds neighbors back through the temporal history (p90 49.6% `Basic`, 21.7% Pairwise), and permutation sampling alone reaches 23.6%. The guide capture therefore runs the convergence shot under `RtxdiContext::convergedSettings`: spatial reuse on disocclusions only (8 boost samples below a history of 20) with Pairwise MIS, as 3.1's fused pass did, which reads −0.13% and 2.87%. The lamp is a hard case, a thin glossy stand under 5 lights inside a shade. The NVIDIA defaults cost 0.11 ms more on the sunlit Sponza bench (rtxdi_3_2_update.md §6.4). **Exit:** run the RTXDI sample on the same scene to see whether its raw output blotches the same way, A/B both settings through DLSS-RR on sunlit `Sponza` (C-F3-3), and re-measure on a many-light scene (M7 emissive triangles, Bistro). Then pick one default that passes the check, or bound the check to what DLSS-RR resolves.
- **C-R32-2 · Limitation · temporal reuse has no Pairwise MIS.** 3.1's fused pass applied Pairwise MIS to its temporal sample too; 3.2's temporal pass offers `Off`, `Basic` and `Raytraced` only. On `LightsPunctualLamp` the frame-to-frame change rose from 0.76% to 1.50% and the per-frame error from 4.20% to 4.75%. The bias stays small (−0.13% against +0.13%). `Raytraced` measured no better in the converged settings (−0.29%, p90 3.13%) at one more ray per pixel; the NVIDIA defaults use it with the visibility shortcut (+0.01 ms on the bench). **Exit:** accept it after C-F3-3's A/B through DLSS-RR, or an RTXDI release with pairwise temporal reuse.

- **C-R32-3 · Limitation · the default BRDF candidate is a wasted ray until emissive triangles.** NVIDIA's `Medium` preset takes 1 BRDF candidate per pixel. Until M7 no geometry is a light, so `RAB_TraceRayForLocalLight` never names one: the ray costs 0.04 ms on the sunlit Sponza bench and adds nothing. It stays on for parity with NVIDIA's sample; `convergedSettings` and the reference set it to 0. **Exit:** M7's emissive triangles make the BRDF candidate find lights.

- **C-R32-4 · Limitation · final visibility reuse leaks light past hard shadow edges.** It is on in NVIDIA's defaults. A reservoir's stored visibility is reused for up to 4 frames and from up to 16 render px away. Under the delta sun that lights the building's shadow edge up to ~16 px into the shadow and fades thin shadows such as Sponza's poles (2026-11-15, courtyard pose of docs/settings.md §3.4). Turning it off with permutation sampling also off and Pairwise spatial reuse gave the most stable DLSS-RR output measured: 0.9% of pixels flickering, against 7.5% for NVIDIA's defaults. **Exit:** re-measure once indirect light (M4) and soft lights (M7) exist, then decide whether the engine keeps NVIDIA's default (C-M4-2).
- **C-R32-5 · Unverified · DLSS-RR flickers more on a noise-free ReSTIR input.** On the courtyard pose the `convergedSettings` raw output is bit-stable, yet RR's output varies 2.8% (1.6% with jitter off). With spatial reuse every frame, the raw output changes 13–19% per frame and RR's output varies 0.9–1.1%. Without spatial reuse each frame, 4.7–42% of lit pixels flicker by more than 10%, depending on the other settings. RR appears tuned to ReSTIR-style noise. **Exit:** confirm with NVIDIA's RR guidance or the RTXDI sample, and judge RTXDI defaults on the `Lit` view (docs/settings.md §3.4).

## M4 · G1 SER pipeline

- **C-G1-1 · Workaround · the closest-hit reads `ClusterIDNV` through `spirv_asm`.** Slang 2026.22 has no `ClusterID()` intrinsic for SPIR-V hit shaders, and `HitObject::GetClusterID` is NV-only (no EXT target). `writeClusterPayload` in `shaders/raytracing/ser.slang` loads the builtin directly, declaring `SPV_NV_cluster_acceleration_structure` itself. Probed 2026-11-16, `spirv-val` clean. **Exit:** a Slang intrinsic for the hit-shader cluster id, or `GetClusterID` on the EXT path, which would also drop the closest-hit invoke.
- **C-G1-2 · Workaround · no subgroup operations after `ReorderThread`.** On driver 621.01 (RTX 5070), wave ops after a reorder in raygen go wrong: a reorder followed by `WaveActiveCountBits` + `WaveIsFirstLane` counted 13,098,546 of 13,102,080 launches, and the same counters after `HitObject::TraceRay` + `ReorderThread` lost the device. The SPIR-V is in the right order (`OpReorderThreadWithHintEXT`, then `OpGroupNonUniformBallot`); per-lane atomics after the reorder are exact. `rt_ser_check.slang` counts with one atomic per lane. Every SER raygen must avoid subgroup ops after the reorder (G2, G3 update paths). Not yet ruled out: descriptor heaps (every bisect variant used a heap pipeline). The Vulkan beta driver branch (597.33, 2026-10-22: descriptor-heap stability and untyped-pointer fixes) is separate from the Game Ready branch (621.01) and may carry fixes 621.01 lacks; none of its notes names SER or subgroups. **Exit:** first rerun the failing variant as a non-heap pipeline (push-constant layout); if heaps are involved, test the Vulkan beta driver. Recheck on each driver update, both branches, with the recipe in spec/m4/g1_ser_rt_pipeline.md §7; report to NVIDIA with a standalone repro.

## M4 · G2 indirect pass

- **C-G2-1 · Deferred · floor-tier indirect at full resolution.** The plan's floor tier traces the bounce at half resolution on a checkerboard; G2 traces every pixel (RTXDI GI context created with checkerboard off). No tier switch exists yet, and the 2060 Super is not in the dev machine. **Exit:** measure the `Indirect` scope on the 2060 Super; if over budget, enable `CheckerboardMode` in the GI context and trace the active field only.
- **C-G2-2 · Open · GPU-AV loses the device on the indirect pass.** On driver 621.01 (RTX 5070, SDK 1.4.363 layers), GPU-AV with BDA bounds instrumentation (`gpuav_buffer_address_oob`) loses the device on the first frame that records `indirect.slang` ("Failed to wait for fence", nvlddmkm event 153). Bisect, 2026-11-16, by `.spv` swap:
  - clean: shader instrumentation off; BDA OOB check off; the indirect pass skipped (G1's SER check stays GPU-AV clean);
  - clean: the same shading code with `traceClosest` (ray query) instead of `traceShaded`, which also gives full GPU-AV coverage of the decode, material and light loads with no reports;
  - clean: `traceShaded` followed by one BDA load;
  - device lost: `traceShaded` followed by `decodeHit` alone; with reorder off (no `ReorderThread`); with GPU-AV safe mode on (which skips failing accesses, so a real out-of-bounds access is ruled out); with GPU-AV's check functions inlined; with an `-O2` shader.

  The engine runs it clean without GPU-AV, and the SER check proves its hits bit-identical to `traceClosest`. Likely the same driver area as C-G1-2: code after `HitObject` trace + invoke in raygen. Until fixed, the GPU-AV pass for SER raygens is the `traceClosest` variant. **Exit:** GPU-AV runs `indirect.slang` clean on a driver update (both branches); report with C-G1-2.

## M4 · G3 NRC

- **C-G3-1 · Deferred · SHaRC code is still in the tree.** NRC became the only radiance cache on 2026-11-14 (NVIDIA-only target). Still present:
  - `Device` picks NRC when its extensions exist and otherwise falls back to SHaRC, enabling SHaRC's 64-bit atomic features (`vk_device.cpp` `kSharcFeatures`, `GiCache::Sharc` in `types.hpp`). NRC's extensions are therefore still optional at device creation.
  - `shaders/gi/sharc.slang`, `tests/sharc_test.cpp`, `tests/shaders/sharc_roundtrip.slang`, the `sharc` fetch in `ThirdParty/CMakeLists.txt` and its Slang include directory.

  **Exit:** delete them, and make NRC's extensions required at device creation, no later than G3.
- **C-G3-2 · Unverified · NRC on the RTX 2060 Super.** The pinned SDK names no minimum GPU but can return `UnsupportedHardware`. The floor tier's 0.6 ms cache budget was set for SHaRC (engine spec §2.3, R4). **Exit:** NRC runs on the 2060 Super and the cache row is measured there in G3.

## M6 · D alpha on rays

- **C-D-1 · Limitation · alpha-tested geometry is opaque to rays.** No opacity micromaps exist yet and every ray instance is `FORCE_OPAQUE` (`shaders/passes/rt_instances.slang`), so rays see alpha-tested cards whole. Sponza's plants cast rectangular shadows, and leaf pixels that pass the raster alpha test are shadowed by the transparent parts of neighboring cards. Under jitter the alpha edges flip between frames, so the plants flicker more than the rest of the frame. **Exit:** D1–D4: per-cluster OMMs baked with the OMM SDK, with alpha-tested primitives dropping `FORCE_OPAQUE`.

## Tooling and validation environment

- **C-T-3 · Limitation · the motion test does not support jitter.** With `ENGINE_JITTER=1` it fails 7 of 13: the static, entity and teleport scenarios compare albedo pixel by pixel between frames, which jitter changes by design. The motion vectors themselves stay within 0.06 px. The test defaults jitter off; jittered runs of it are for validation coverage only.
- **C-T-1 · Limitation · GPU-AV changes results.** Under GPU-assisted validation the motion test's "camera yaw, sky in view" fails with 0.87 px and 2 outlier pixels, giving 12 of 13. This was checked on 2026-11-13: it comes from GPU-AV, not the engine. GPU-AV also makes GPU timings meaningless. vkconfig can force it even with `VK_LOADER_LAYERS_DISABLE`, so set `VK_KHRONOS_VALIDATION_GPUAV_ENABLE=false` explicitly for pass/fail and timing runs.
- **C-T-4 · Warning · third-party performance warnings.** With performance warnings on (`VK_KHRONOS_VALIDATION_REPORT_FLAGS=error,warn,perf`) the guide capture reports `BestPractices-vkBindBufferMemory-small-dedicated-allocation` (10), `-vkBindImageMemory-small-dedicated-allocation` (4), `BestPractices-NVIDIA-AllocateMemory-ReuseAllocations` (8), `BestPractices-NVIDIA-BindMemory-NoPriority` (10) and `BestPractices-NVIDIA-CreatePipelineLayout-SeparateSampler` (1). Every object involved is unnamed, and the engine names its objects before binding memory, so they come from ImGui's Vulkan backend (one allocation per buffer; a 737,280 B image matches a 512×360 RGBA font atlas) and NGX. Seen 2026-11-15. **Exit:** none planned; recheck after ImGui or DLSS SDK upgrades.
- **C-T-2 · Warning · "legacy command" notices.** `WARNING-legacy-buffer-commands` and similar notices suggest `VK_KHR_device_address_commands` replacements for `vkCmdDispatchIndirect`, `vkCmdUpdateBuffer` and `vkCreateAccelerationStructureKHR`. They were seen with the vkconfig-forced validation settings on 2026-11-14, not with the engine's own validation environment. Not scheduled. **Exit:** adopt the extension, once it is available on the target drivers.

---

## Resolved

| ID | Stamp | Problem | Fixed |
|---|---|---|---|
| C-F2-1 | M3 · F2 | `RAB_LoadLightInfo(i, true)` returned this frame's row, so temporal reuse saw a moving light at its new position | 2026-11-15: `GpuFrameRoot.prevWorldLights` carries the previous slot's world rows (0 on a history reset); the next frame's light pass waits for the ray passes at the compute stage, and a growing slot waits for the last submitted ray passes (PBR plan F2) |
| C-DGC-1 | Pre-M3 · DGC | `DgcPreprocess_*` (128 KiB) took dedicated memory blocks (`BestPractices-vkBindBufferMemory-small-dedicated-allocation`) | 2026-11-15: suballocated through `vmaCreateBufferWithAlignment` at a 4 KiB start, which keeps every region page-aligned (the reason for the dedicated block); `createBuffer` takes a `minAlignment`. Guide capture, motion test and cull check unchanged |
| C-M0-2 | M0 · AU10 | swapchain recreation had two paths: every resize event rebuilt at once, the renderer's out-of-date path skipped ImGui, and no old swapchain was passed | 2026-11-15: resize events and out-of-date / suboptimal results only call `Renderer::requestSwapchainRecreate`; `Engine::recreateSwapchain` runs once before the frame is built, recreates from the old swapchain and updates ImGui. The motion test's resize recreates once (13 of 13) |
| C-M0-3 | M0 · AU8 | `ThirdPartyImpl` was linked into `engine_core` and `engine_util`; coverage defaulted to ON; a stale `libengine_postprocessing.dll` sat in the build folders | 2026-11-15: whole-archive in `engine_util` only, which auto-exports VMA, stb and tinygltf (the other DLLs export none); the separate C compile of `tiny_gltf_v3.c` was dropped, since `implementations.cpp` already compiles it; `ENGINE_ENABLE_COVERAGE` defaults to OFF (existing build caches keep their value); both stale DLLs deleted |
| C-M0-6 | M0 | docs described M0 placeholders (`tlas` 0, `exposure` as pre-exposure, V1's missing A4, §16.1 "linkage tests only") | 2026-11-15: PBR plan §3 and §4.2, `types.slang` / `types.hpp`, visibility buffer V1 and engine spec §16.1 corrected |
| C-F3-8 | M3 · F3 | no permanent convergence regression check | 2026-11-15: the guide capture compares 64 frames of `Lighting only` on `LightsPunctualLamp` with `Shadowed lights` and logs `Convergence check: passed` or `FAILED` (\|bias\| ≤ 0.5%, tile p90 ≤ 3%). It caught 3.2's spatial-reuse regression (C-R32-1) |
| C-F3-2 | M3 · F3 | RTXDI 3.1's fused `Basic` path darkened by 35%, so Pairwise MIS replaced it | 2026-11-15: RTXDI 3.2 removed the fused path. Spatial reuse keeps Pairwise on measurement (`Basic` tile p90 6.28% against 2.87%); temporal reuse uses `Basic`, since 3.2 has no pairwise temporal reuse (C-R32-2) |
| C-M0-4 | M0 · AU6 | the per-slot color-space rules were duplicated: `tools/gltf_ktx2` kept its own `isSrgbSlot` beside the loader's | 2026-11-15: `gltfTextureSlotColorSpace` and `TextureColorSpace` live in the dependency-free `src/core/texture_color_space.hpp`. The loader (core slots, extension slots and the image-use dry run) and the tool both use it. The `assets_loader.cpp` split of AU6 stays with M8 |
| C-H1-5 | M3 · H1 | DLSS-RR was checked only on a sparsely lit frame (no lights in `Sponza`) | 2026-11-14: E3's sun lights the courtyard. At Quality on a still camera the Lit output's temporal deviation is 0.35% of the mean (0.2% of lit pixels above 10%); captured frames during a yaw show no ghosting. Needs a look on screen by the user |
| C-F3-6 | M3 · F3 | stability measured on a static camera only | 2026-11-14: a camera yawing ±8.6° (60-frame period) and a sun swinging ±5.7° in azimuth, both on the sunlit courtyard at Quality: frames at repeated poses differ by 2.4% and 1.5% of the mean. Jitter off gives the same 2.4% under motion, so it is RR's and ReSTIR's history, not jitter |
| C-C4-1 | M2 · C4 | the TLAS was a full per-slot build, and partitioned AS was optional at device creation | 2026-11-14: partitioned AS is required; one PTLAS is updated in place from GPU-written instance writes |
| C-C4-2 | M2 · C4 | no ray set the OMM 2-state flag, which PTLAS instances cannot carry | 2026-11-14: `traceVisibility` and `traceClosest` set `RAY_FLAG_FORCE_OMM_2_STATE` (see C-C4-3) |
| C-H1-7 | M3 · H1 | below DLAA, the debug views and Lit without lights (unlit `Sponza`) boiled: jittered at render resolution and scaled up with nothing resolving the jitter | 2026-11-14: `Camera::updateCameraData` jitters only frames DLSS-RR evaluates (`Renderer::isDlssPresented()`); `ENGINE_JITTER=1` forces it for the culling checks |
| C-H1-4 | M3 · H1 | DLSS-RR used its own auto exposure | 2026-11-14: H2's exposure image is passed as `pInExposureTexture`, and the feature is created without `AutoExposure` |
| C-E1-1 | M3 · E1 | `lodBias` was 0 while render equalled display | 2026-11-14: `Camera` sets `log2(render / display)` from DLSS-RR's render resolution (H1) |
| C-E1-2 | M3 · E1 | jitter was off by default | 2026-11-14: on by default, resolved by DLSS-RR (H1). `ENGINE_JITTER=0` turns it off; the motion test and the guide capture default to off, since they compare frames pixel by pixel |
| C-F3-R1 | M3 · F3 | `BestPractices-AMD-ClearAttachment-ClearImage-color`: with no lights, `directRadiance` was cleared with `vkCmdClearColorImage` | 2026-11-14: `rtxdi_shade` writes zeros itself when no light is loaded (a uniform early-out), so the image needs no transfer usage |
