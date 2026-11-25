# M4 · G3: Neural Radiance Cache

**Started:** 2026-11-17 · **Status:** built 2026-11-17; 2060 Super numbers open (C-G3-2) · **Milestone:** M4
**Implements:** engine spec §11.4; plan G3 ([pbr_implementation_plan.md](../pbr_implementation_plan.md))
**Depends on:** [G1](./g1_ser_rt_pipeline.md) (update paths are shaded rays), [G2](./g2_indirect_pass.md) (query pass)
**Used by:** [G5](./g5_restir_gi.md)
**Considerations:** C-G3-1 (SHaRC removed, resolved), C-G3-2 (NRC on the 2060 Super), C-G3-3 (SDK leaks objects), C-G3-4 (no loss on Vulkan), C-G3-5 (`requestReset` ineffective), C-G3-6 (intermittent device loss)
**Reference:** NVIDIA RTXGI `Samples/IndirectDiffuse` (`reference/RTXGI`, commit 04372d7f, 2026-11-12): `Shaders/NrcIndirectDiffuse.hlsli`, `Pathtracer.cpp` (per-frame flow, settings), `Samples/Shared/Nrc/NrcIntegration.cpp` (Vulkan host); `reference/NrcGuide.md`

SDK: NVIDIA RTX Neural Radiance Cache 0.16 (Vulkan host DLL + HLSL headers), pinned in `ThirdParty/CMakeLists.txt`; prebuilt DLLs (NRC + CUDA runtime) copied next to the executable. `NrcLibrary` binds the DLL's MSVC-mangled exports at runtime (the toolchain cannot link its import library).

The cache supplies the radiance of bounces 2…∞ at the indirect pass's hit X₂. G2 stores Lo(X₂) = emission + direct light; G3 adds `prefixThroughput × cachedRadiance` to that reservoir radiance, so G5 resamples full multi-bounce samples.

---

## 1. Decisions

| Decision | Rule | Why |
|---|---|---|
| Library lifetime | the engine loads `NrcLibrary` before `Device` and keeps it until after the renderer; `nrc::vulkan::Initialize` once after device creation, `Shutdown` at teardown | the DLL lists the device extensions, so it must be loaded first; `Initialize` is process-global |
| Device | NRC's instance and device extension lists join NGX's through `SdkExtensions` (missing → device creation throws). `uniformBufferStandardLayout` (NRC's feature list), `shaderInt16` and `shaderFloat16` become required features (NRC's HLSL packs with `f32tof16` / `f16tof32`, which Slang lowers to the `Float16` and `Int16` capabilities). No `VK_NV_cooperative_vector`: NRC 0.16 asks for `VK_NVX_binary_import`, `VK_NVX_image_view_handle`, memory budget and BDA only (its kernels are CUDA binaries) | required, no fallback (plan G3); the scope line naming cooperative vectors predates the SDK check |
| SHaRC | removed: `GiCache`, the SHaRC features, `gi/sharc.slang`, `sharc_test`, its fetch and include dir (C-G3-1 exit) | NRC is the only cache (engine spec §11.4) |
| Memory | engine-allocated SDK buffers (`enableGPUMemoryAllocation = false`): `GetBuffersAllocationInfo` sizes them, `GetBufferUsageFlags` + storage + device address set the usage, VMA suballocates them named `Nrc<debugName>`; only buffers whose size changed are reallocated before `Configure`, and they are passed to it as `nrc::vulkan::Buffers` | SDK-allocated buffers (the sample's choice) were unnamed dedicated blocks without a memory priority, and each `Configure` / `Destroy` leaked them (C-G3-3) |
| Context settings | frame = render extent; training = `ComputeIdealTrainingDimensions(frame, 0)`; `maxPathVertices` 8, 1 spp, `learnIrradiance` on, `smallestResolvableFeatureSize` 0.01: the sample's values. **`includeDirectLighting` off** | the sample caches direct light; engine spec §11.3 samples direct light at X₂ explicitly (G2), so the cache holds the indirect part only and the query ends after direct light |
| Scene bounds | world AABB of every ray instance, computed with the RT instance templates (`RayTracingScene::sceneBounds`) | "anything a ray can hit must be inside"; the ray instances are exactly that |
| Configure | at startup, when the render extent outgrows the configured frame (grow-only: `frameDimensions` = max of the render extent and the previous frame; device idle already), and when the bounds change: `waitIdle`, then `Configure`. Used training dimensions come from the render extent, not the configured frame. No `requestReset`, no reset button: NRC 0.16 keeps the weights through it (C-G3-5) | `Configure` may reallocate the buffers frames in flight use; a layout change is a load. Every `Configure` reloads the network and leaks SDK objects (C-G3-3), so a shrinking resize must not trigger one |
| Frame settings | the sample's: 1 training iteration per frame (`usedTrainingDimensions = ComputeIdealTrainingDimensions(frame, 1)`), `maxExpectedAverageRadianceValue` 1 (sun 10 lx), both heuristic thresholds 0.01, `proportionPrimarySegmentsToTrainOn` 0.02, `skipDeltaVertices` on; training toggle and iteration count in the UI | NVIDIA defaults (perf comparison against the sample) |
| Constants | `GpuNrcConstants` per frame slot through `GpuFrameRoot.cacheConstants`: `NrcConstants` + the six buffer addresses and row counts | one root pointer |
| Bridge | `gi/nrc_bridge.slang`: `NRC_USE_CUSTOM_BUFFER_ACCESSORS` with bounds-checked BDA buffers (`CheckedBuffer<T>`), scalar layout (equals the SDK's tightly packed structured-buffer layout; sizes checked in `tests/layout_test.cpp`) | vendor headers stay untouched; BDA bridges bounds-check (RTXDI bridge lesson) |
| Counter atomics | NRC's own `NrcIncrementCounter`: one atomic per wave (`WaveActiveCountBits` / `WavePrefixCountBits`). The SER version mapped the wave intrinsics to single-lane values (C-G1-2); the compute passes need no mapping | the SDK's code, unedited |
| Delta lobes | `isDeltaLobe` false everywhere | the BRDF clamps α ≥ 10⁻³ (`kMinAlpha`); no material is a mirror |
| Update pass | `passes/nrc_update.slang` (`NRC_UPDATE`), compute `nrcUpdateMain`, one thread per training path over the used training dimensions (`GpuNrcConstants.trainingLaunch`, width \| height << 16). Each thread picks a jittered G-buffer pixel (the sample's `SelectSparseGBufferPixel`), submits the primary surface as vertex 0 (segment = camera distance), adds emission + direct light at every vertex (`directLightAt`, as G2 at X₂), samples the full BRDF and traces up to 8 vertices with `traceClosest` | the sample's training path; ray queries while SER is shelved (C-G1-3) |
| Bounce loop | plain loop, ends at the path's end | no reorder, so no uniform-control-flow constraint (the SER version ran a fixed trip count with NOP hit objects) |
| Query pass | G2's `indirect.slang` compiled with `NRC_QUERY`. X₂ is the path's first NRC vertex; the pass's copy of the constants turns the termination heuristic off, so the query sits at X₂ (`TerminateAfterDirectLighting`). Every launch writes its query path info, also with no query | one ray per pixel (spec §11.3); with the sample's 0.01 threshold the query lands on the first hit almost always anyway. The resolve reads every pixel |
| Train | `QueryAndTrain(cmd, nullptr)` in the ray command buffer after the query pass | engine spec §11.4 frame order; the loss readback fails on Vulkan (C-G3-4) |
| NRC commands and descriptor heaps | an empty legacy descriptor set is bound before each NRC call and both heaps are rebound after (shared with DLSS: `DriverDescriptorHeap`). Since 2026-11-18 `BeginFrame` and `QueryAndTrain` (and DLSS-RR) each record into a command buffer of their own inside the ray submit (`Renderer::recordIsolated`), which binds both heaps before and after the call | NRC launches CUDA kernels through `VK_NVX_binary_import` like NGX (C-H1-1) |
| NRC barriers | a global `ALL_COMMANDS` memory barrier before and after each NRC call | its kernels run as CU launches with no documented stage or access |
| Resolve | `passes/nrc_resolve.slang` (compute): `reservoir.radiance += ao · max(prefixThroughput · NrcUnpackQueryRadiance(queryRadiance), 0)` at the pixel's GI reservoir (an untrained network predicts negative radiance); `ao` = the primary's material AO (`DiffuseAlbedo.a`) | NRC guide step 7 (custom resolve); engine spec §11.6: material AO applies only to the cache term |
| Cache debug view | `NRC cache` (28): the query pass queries at the primary surface instead of X₂ and the resolve is skipped; the debug view shows `prefixThroughput · cachedRadiance` | the SDK's `DirectCacheView` (cache at vertex 0), through the custom path |
| End of frame | `EndFrame(graphicsQueue)` right after the ray submit | the queue that ran every NRC command |
| Submit after NRC | no queue operation and no NRC / NGX call while a frame's NRC kernels or the DLSS-RR after them run. The ray commands from the frame's first NRC / NGX segment on and the present pass share one `submit2` call (the pure-Vulkan segments before it go with the frame commands ahead of `AcquireImage`, MG-P7; NRC frames since 2026-11-25); the frame's present is deferred (`Renderer::pendingPresent`). `Renderer::finishPreviousFrame` waits on the host for `FrameSync::rayTimeline` = the last ray submit with NRC commands, then presents. It runs after the next frame's frame commands are recorded and before its ray commands, and before a swapchain or DLSS rebuild | a submit, present or SDK recording call issued during that work loses the device, sooner at larger render extents; no GPU-side barrier or semaphore wait prevents it (C-G3-6). Frame commands still overlap the previous frame's tail. Costs 11% frame time uncapped at 1280×720 (2.87 → 3.20 ms vs the older pre-submit wait, which loses the device from 1280×720 render) |
| Timers | `NrcUpdate`, `NrcTrain`, `NrcResolve` scopes and Tracy zones | budget row "radiance cache update / train" (spec §2.3) |

---

## 2. Files

| File | Change |
|---|---|
| `src/render/nrc_library.{hpp,cpp}` | unchanged bindings; `requiredExtensions()` for `SdkExtensions` |
| `src/render/vk_nrc.{hpp,cpp}` | `NrcCache`: context, configure, frame settings, per-slot constants, update dispatch, record, end frame |
| `src/render/vk_driver_heap.{hpp,cpp}` | `DriverDescriptorHeap`: the empty legacy set of C-H1-1, shared by DLSS and NRC |
| `src/core/vk_device.{hpp,cpp}`, `src/core/types.hpp` | SHaRC and `GiCache` removed; `uniformBufferStandardLayout`, `shaderInt16`, `shaderFloat16` required; `mergeSdkExtensions` |
| `src/core/vk_engine.{hpp,cpp}` | loads `NrcLibrary` before the device |
| `src/render/vk_rt_scene.{hpp,cpp}` | `getSceneBounds()` from the instance templates |
| `shaders/core/binding.slang`, `shaders/lighting/rtxdi_bridge.slang` | `CheckedBuffer<T>` (was `RtxdiReservoirBuffer<T>`) |
| `shaders/gi/nrc_bridge.slang`, `shaders/gi/nrc_constants.slang` | NRC include with BDA accessors, `GpuNrcConstants` mirror |
| `shaders/lighting/path_shading.slang` | `directLightAt` and `shadeRayHit`, shared by the indirect and update raygens |
| `shaders/passes/nrc_update.slang`, `nrc_resolve.slang`, `indirect.slang` | update raygen; resolve; query mode |
| `shaders/passes/debug_view.slang`, `debug_view.hpp`, `types.slang` | `NRC cache` view (28) |
| `src/render/vk_renderer.{hpp,cpp}`, `vk_gpu_timer.hpp`, `engine_ui.cpp` | record order, timers, "Indirect (M4)" NRC controls |
| `tests/layout_test.cpp` | `GpuNrcConstants`, NRC buffer element sizes |
| removed | `shaders/gi/sharc.slang`, `tests/sharc_test.cpp`, `tests/shaders/sharc_roundtrip.slang`, SHaRC fetch |

---

## 3. Verification

| Check | Result |
|---|---|
| Training loss falls after load | the loss cannot be read on Vulkan (C-G3-4). Replaced by the cache view's convergence at the courtyard pose: mean relative difference to the last warm-up view (frame 399) 0.297 at frame 0, 0.072 at frame 319, 0.012 at frame 389 |
| `NRC cache` view shows cached radiance at the primary | smooth radiance with the shadow structure of the scene and sunlit bounce (probe captures, 2026-11-17). With `includeDirectLighting` off, sunlit pixels read 0.064 in the cache against 0.55 direct light; with it on, 0.73: the cache holds the indirect part only, so nothing is counted twice |
| Interior lit only by the sun is not black | `Indirect samples`, gallery pose, 64-frame mean: 90.1% black pixels without the cache term, 0% with it (+113% radiance). Courtyard: the cache term adds 25% |
| Validation: core + sync + best practices, `ENGINE_RT_CHECK=1 ENGINE_GUIDE_CAPTURE=1` (debug) | clean apart from known messages: `BestPractices-SpirvDeprecated_WorkgroupSize` ×3 (C-H1-2) and `VUID-vkDestroyDevice-device-05137` ×2 (the SDK's leaked objects, C-G3-3). Ray check 270 of 270, SER check 0 mismatches (reorder off and on), normal checks 2 of 2, `Convergence check: passed` (bias −0.096%, tile p90 2.58%: the added `NRC cache` guide view shifts RTXDI's RNG) |
| SPIR-V | no `OpHitObject*` / `OpReorderThread*` in `indirect` and `nrc_update` (GLCompute entry points, ray queries); NRC's wave counter: `OpGroupNonUniformBallot` + one `OpAtomicIAdd` per counter site |
| Training with the compute update pass | `NRC cache` view at frame 400, gallery: mean 0.000345, 409,384 of 409,440 texels lit; with training off 0.000020 (untrained), 48,699 lit |
| Long uncapped runs | before the submit wait, every release bench run of ≥700 frames lost the device (C-G3-6); with it, 0 of 6 runs of 2000 frames. Since 2026-11-18 (deferred present): 0 of 3 runs of 3000 frames each at 1920×1080 and 2560×1369 windows (render 1280×720 and 1707×913). SDK calls isolated (2026-11-18): the user reports the crash fixed; 0 of 3 runs of 3000 frames at 2560×1369 with the host wait skipped |
| GPU-AV | clean since the compute passes (2026-11-17, debug, 240 frames, `Indirect` 4.79 ms under instrumentation): only `VUID-vkDestroyDevice-device-05137` ×2 (C-G3-3) |

## 4. Measurements

Release build, no layers (`VK_KHRONOS_VALIDATION_GPUAV_ENABLE=false`, no validation lines in the log), 853×480 render → 1280×720 (DLSS-RR Quality), RTX 5070, driver 621.01. Gallery pose (startup view), `ENGINE_BENCH_FRAMES=2000`, mean of 3 runs (lowest 120-frame window in parentheses).

| Scope | Compute + ray query (2026-11-17, current) | SER pipeline, reorder off (before the shelving) |
|---|---|---|
| `NrcUpdate` (BeginFrame + 197×111 training paths) | 0.017 (0.016) | 0.181 (0.175) |
| `Indirect` (query pass) | 0.576 (0.544) | 0.528 (0.475) |
| `NrcTrain` (`QueryAndTrain`, 1 iteration) | 0.434 (0.418) | 0.406 (0.384) |
| `NrcResolve` | 0.034 (0.031) | 0.034 (0.032) |
| GPU total | 2.80 | 2.84 |

The cache costs 0.49 ms on the 5070 (update + train + resolve). The SER update pass traced all 7 bounces on every lane (finished lanes with NOP hit objects) and counted with one atomic per lane; the compute pass ends each path at its last vertex. The ray-query indirect pass costs ~0.05 ms more than the SER one (mean +9%, best window +15%: above the ±5% noise), so the update and query passes together are 0.12 ms cheaper.

Still to do: the cache row of spec §2.3 measured on the 2060 Super (C-G3-2, spec R4).

---|---|
| `NrcUpdate` (BeginFrame + 197×111 training paths) | 0.181 (0.175) |
| `Indirect` (query pass) | 0.528 (0.475) |
| `NrcTrain` (`QueryAndTrain`, 1 iteration) | 0.406 (0.384) |
| `NrcResolve` | 0.034 (0.032) |
| GPU total | 2.84 |

The cache costs 0.62 ms on the 5070 (update + train + resolve); the query pass is 0.08–0.1 ms above G2's `Indirect` (0.42–0.43 ms). One courtyard run with reorder on (before the submit wait): update 0.217, indirect 0.575, train 0.368, resolve 0.051 ms.

Still to do: the cache row of spec §2.3 measured on the 2060 Super (C-G3-2, spec R4), and reorder on/off at fixed poses once a pose hook exists (the probe hooks were removed).

---

## 5. Log

| Date | Change |
|---|---|
| 2026-11-16 | created from plan G3 |
| 2026-11-17 | design (§1) from the RTXGI IndirectDiffuse sample; cooperative vectors dropped from the device scope (not requested by NRC 0.16) |
| 2026-11-17 | built: SHaRC removed, NRC required; loss readback and cache reset dropped (C-G3-4, C-G3-5); negative predictions clamped; `shaderInt16` / `shaderFloat16` required; host wait before a submit that follows NRC work (C-G3-6); verified (§3), measured on the 5070 (§4) |
| 2026-11-17 | SER shelved (C-G1-3): update pass and query pass are compute shaders with `traceClosest`; NRC wave counter restored; `GpuNrcConstants.pad0` became `trainingLaunch`; GPU-AV clean; measurements redone (§4) |
| 2026-11-17 | vkconfig triage: SDK buffers allocated by the engine, grow-only frame configure (a resize no longer reconfigures: teardown leak 92 → 51 objects after the motion test's resize); motion test 13 of 13, guide capture + ray check unchanged, 2 × 3000 uncapped frames without device loss (GPU total 2.70 / 2.75 ms) |
| 2026-11-18 | device loss at large render extents (user report: full-screen resize; reproduced at 853×480 → 1280×720+ render, within seconds): C-G3-6 workaround rebuilt as one ray + present submit, deferred present and a host wait before the next frame's ray recording; motion test 13 of 13, guide capture + ray check unchanged, GPU-AV clean |
| 2026-11-18 | NRC `BeginFrame`, `QueryAndTrain` and DLSS-RR each in a command buffer of their own within the ray submit, heaps bound before and after (C-G3-6); the user reports the crash fixed, the host wait stays. Validation (core + sync + best practices) unchanged on the guide capture and the GI check (4 of 4). Cost of the isolation (release, no layers, alternating rounds of 1500 and 6000 frames): none measurable at 1280×720 (GPU total 3.28–3.35 ms and DLSS-RR 1.11–1.13 ms with and without it, frame time 3.89 → 3.82 ms); at a 2560×1369 window GPU total 10.78 → 10.83 ms (+0.5%), the DLSS-RR scope 3.61 → 3.69 ms (+2%), frame time 11.37 → 11.47 ms (+0.9%, inside the ±0.4 ms spread between runs); the other scopes unchanged |
