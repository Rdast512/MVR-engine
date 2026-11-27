# Feature: Nsight Graphics GPU markers (`VK_EXT_debug_utils` command-buffer labels)

**Date:** 2026-11-27 · **Status:** planned · **Milestone:** M8 tooling, before [M8.5](../../m8/m8_5_perf_and_nvidia_benchmark.md) P5/P6, so its GPU Trace captures of the engine and of NVIDIA's demos read the same way.
**Fixes:** the engine has no GPU ranges in Nsight. `VK_EXT_debug_utils` is always enabled at the instance (`src/core/vk_device.cpp:415`, "object names and labels for RenderDoc / Nsight"), and objects, queues and command buffers are named (`src/util/debug.hpp`, `vk_device.cpp:864-867`, `vk_frame_sync.cpp:90-120`). But no `vkCmdBeginDebugUtilsLabelEXT` is recorded anywhere in `src/`. GPU Trace and the Frame Debugger therefore show a flat list of unlabeled dispatches. The only GPU ranges are Tracy's (`TracyVkNamedZone`), which Nsight does not read, and those cannot span the SDK segments (`vk_renderer.cpp:750`: "a Tracy zone cannot span segments").
**Touches:**
- `src/util/debug.{hpp,cpp}`: label helpers;
- `src/render/vk_gpu_timer.hpp`: the shared pass names;
- `src/render/vk_renderer.{hpp,cpp}`: frame, ray-segment and present labels, the segment label stack, event markers;
- `src/render/vk_rt_scene.cpp`, `vk_rtxdi.cpp`: async compute labels;
- `src/core/vk_mega_geometry.cpp`, `vk_asset_buffers.cpp`: load-time labels (P3).

**Reference:**
- `reference/vk_minimal_latest/src/debug_util.h:63-84`: `utils::DebugUtil::ScopedCmdLabel`, an RAII begin/end pair that is a no-op when the function pointer is null, plus `DBG_VK_SCOPE(cmd)` labeling with `__FUNCTION__`.
- `reference/vk_gltf_renderer/src/renderer.cpp:1449,3030` and `dlss.cpp:871`: `NVVK_DBG_SCOPE(cmd); // Helps to debug in NSight`, one scope per recording function.
- `reference/vk_denoise_dlssrr/dlss_rr/src/dlssrr_wrapper.cpp:352`: a named label around the DLSS-RR SDK call.
- `reference/RTXPT-main/Rtxpt/GPUSort/GPUSort.cpp:222`: `RAII_SCOPE(beginMarker, endMarker)` per pass. Its `AdvancedSample.cpp:190` removed the top-level "MainRendering" marker because "added hierarchy reduces readability". That is why this spec caps depth at 2 (§2.3).
- VUIDs checked against SDK 1.4.363.1 `share/vulkan/registry/validusage.json`:
  - `VUID-vkCmdEndDebugUtilsLabelEXT-commandBuffer-01912`: an outstanding begin must exist on the queue the command buffer is submitted to;
  - `-01913`: secondary command buffers must balance within themselves;
  - `VUID-vkQueueEndDebugUtilsLabelEXT-None-01911`.
- Nsight Graphics [User Guide](https://docs.nvidia.com/nsight-graphics/UserGuide/):
  - GPU Trace collects user markers and shows them on the queue they executed on;
  - for Vulkan it accepts `VK_EXT_debug_utils` or `VK_EXT_debug_marker`, plus NVTX;
  - Multi-Pass Metrics needs user markers and matches frames by them, so their order should stay consistent across frames;
  - names set through `vkSetDebugUtilsObjectNameEXT` are also captured.

  See also [Configuring Your Application](https://docs.nvidia.com/nsight-graphics/UserGuide/configure-application.html) and the [Nsight Systems guide](https://docs.nvidia.com/nsight-systems/UserGuide/index.html) (debug-util labels on CPU and GPU).

---

## 0. Why, with M8 in mind

| Need | What labels give |
|---|---|
| **M8.5 P5/P6** compares the engine against RTXPT / RTXDI / RTXGI in GPU Trace. Those demos label their passes (`beginMarker`) | The engine's passes appear as named ranges on the graphics and compute queues, next to the demos' ranges, with per-range SM / RT-core / L2 throughput |
| GPU Trace **Multi-Pass Metrics** refuses an application without markers | Consistent markers make that mode available |
| **Async compute overlap** (PTLAS build + light prep against the raster phase, F4 `RayStartWait`) is read today from Tracy timestamps only | Labels on the compute queue show the overlap directly in GPU Trace's queue timeline |
| **SDK segments** (NRC `QueryAndTrain`, NGX DLSS-RR) run in their own command buffers (`recordIsolated`, `vk_renderer.cpp:806`) | Each segment carries its own label and keeps the parent group (§2.4), so CUDA-interop gaps can be attributed |
| **M8.3** zero-cost stages: a skipped stage must leave no work | A skipped stage records no label, so an empty frame region in GPU Trace confirms the skip without reading timer scopes |
| Frame Debugger event list | Every dispatch, draw and DGC execute sits under a named scope instead of in a flat list |

Not a goal:
- Nsight Aftermath crash checkpoints;
- the Nsight Perf SDK in-app HUD;
- NVTX CPU ranges (Tracy covers the CPU);
- shader-source correlation (Slang debug info).

§6 lists them as follow-ups.

---

## 1. Facts this design relies on

1. The extension is on in every build, with or without validation (`vk_device.cpp:415`). The device dispatcher has `vkCmdBeginDebugUtilsLabelEXT`, `vkCmdEndDebugUtilsLabelEXT` and `vkCmdInsertDebugUtilsLabelEXT` whenever the instance extension is enabled. A null pointer (e.g. a loader without it) makes every helper a no-op, as `setDebugNameHandle` does (`src/util/debug.cpp:35-37`).
2. Command-buffer labels may legally span primary command buffers on one queue (`-01912`). This engine still **balances every label within one command buffer**:
   - frame work splits into a pre-acquire and a post-acquire submit (MG-P7, `preSdkRaySegmentCount`);
   - SDK segments are separate command buffers;
   - GPU Trace and the Frame Debugger show per-command-buffer trees.

   A span across those boundaries would nest wrongly whenever the split moves (NRC or DLSS-RR toggled off, §3.1 of M8.3).
3. Labels are host-side driver calls with no GPU work. Their cost is CPU record time, about 40–60 begin/end pairs per frame (§3). It is gated by ABBA in §5.
4. Present is deferred by one CPU frame (`PendingPresent`, `finishPreviousFrame`). Queue-level labels around a frame's submits would therefore interleave with the previous frame's present. Hence P3 and the open question in §7.

---

## 2. Design

### 2.1 Helpers (`src/util/debug.hpp`)

```cpp
// RAII command-buffer label; no-op without VK_EXT_debug_utils
class ScopedCmdLabel
{
public:
    ScopedCmdLabel(const vk::raii::CommandBuffer& cmd, const char* name, LabelColor color) noexcept;
    ~ScopedCmdLabel();
    ScopedCmdLabel(const ScopedCmdLabel&) = delete;
    ScopedCmdLabel& operator=(const ScopedCmdLabel&) = delete;

private:
    const vk::raii::CommandBuffer* cmd = nullptr;
};

// instant marker (history reset, NRC reset): vkCmdInsertDebugUtilsLabelEXT
void insertCmdLabel(const vk::raii::CommandBuffer& cmd, const char* name, LabelColor color) noexcept;
```

- `name` is `const char*` (null-terminated, string literal or `gpuTimerScopeName`), so no per-frame allocation.
- `LabelColor` is an `enum class` with one value per category in §2.2, mapped to one `constexpr` RGBA table. Nsight and RenderDoc color ranges by it.
- Move-only is not needed: labels live on the stack of the recording function, as in `vk_minimal_latest`.

### 2.2 Names and colors: one source

- **Leaf names reuse `gpuTimerScopeName(GpuTimerScope)`** (`vk_gpu_timer.hpp:73`). Bench keys, Tracy plots, the UI timing table and Nsight then show one name per pass. A render-side helper `ScopedCmdLabel gpuLabel(cmd, GpuTimerScope)` in `vk_gpu_timer.hpp` picks the name and the category color.
- **Exception:** Streaming RIS is timed under `RtxdiShade` (M8.3 §3.2) but labeled **`Streaming RIS`**. A label names the work that ran; the timer scope keeps its bench key.
- **Group names** are new literals: `Raster early`, `Raster late`, `Direct lighting`, `Indirect lighting`, `Reconstruction`, `Async compute`.

| Category (`LabelColor`) | Scopes |
|---|---|
| `Raster` | cull, DGC preprocess, mesh draws, Hi-Z, classify, resolve |
| `Direct` | ReSTIR DI initial / temporal / shade, Streaming RIS |
| `Indirect` | Indirect, ReSTIR GI temporal / shade |
| `Cache` | NRC update / train / resolve, SHaRC update / resolve |
| `Composite` | Combine, Glass, Accumulation, Post, Debug view |
| `Sdk` | NGX DLSS-RR and NRC SDK segments |
| `Compute` | PTLAS build, light preparation |
| `Event` | instant markers (§2.5) |

### 2.3 Hierarchy (depth ≤ 2)

Graphics queue:

| Command buffer | Level 1 | Level 2 (leaf) |
|---|---|---|
| `GraphicsCommandBuffer_i` (`recordFrameCommands`) | `Raster early` | `Early cull`, `Early DGC preprocess`, `Early mesh draw`, `Early alpha draw`, `Early Hi-Z build` |
| | `Raster late` | the same five `Late …` scopes |
| | `Material classify` | — |
| | `Vis-buffer resolve` | — |
| Ray segments (`recordRayCommands`) | `Direct lighting` | `ReSTIR DI initial`, `ReSTIR DI temporal`, `ReSTIR DI shade` **or** `Streaming RIS` |
| | `Indirect lighting` | `NRC update`, `SHaRC update`, `SHaRC resolve`, `Indirect`, `NRC train`, `NRC resolve`, `ReSTIR GI temporal`, `ReSTIR GI shade` |
| | `Reconstruction` | `Combine`, `Glass`, `DLSS-RR`, `Accumulation`, `Post` |
| | `Debug view` | — |
| | `Self-tests` (frame hook) | — |
| `PresentCommandBuffer_i` | `Present` | — |
| | `UI` | — |

Compute queue (`submitComputeCommands`, `vk_renderer.cpp:851`):

| Level 1 | Level 2 |
|---|---|
| `Async compute` | `PTLAS build` (`rtScene.recordBuild`), `Light preparation` (`light_world` → `light_pdf_mips` → `rtxdi_presample`) |

Rules:
- **Skipped work records no label.** Examples: `Direct == Off`, `Indirect == Off`, NRC paused, no glass, DLSS-RR off. The `gpuTimer.end` calls for skipped scopes stay as they are (M8.3 §3.1).
- A group whose every leaf is skipped records nothing.
- No `RayStartWait` label. It is a wait span between submits, not recorded work; GPU Trace shows the gap itself.
- Labels sit next to the existing `ZoneScopedN` / `TracyVkNamedZone` scopes, in the same C++ scope, so CPU, Tracy GPU and Nsight ranges line up.

### 2.4 Labels across SDK segments (`recordIsolated`)

`Indirect lighting` (NRC train) and `Reconstruction` (DLSS-RR) contain an SDK call that ends the current ray segment and starts two new command buffers (`vk_renderer.cpp:806-818`). An RAII label in the enclosing C++ scope would then end on a different command buffer than it began.

- `Renderer` keeps a fixed-capacity **segment label stack**: `std::array<const char*, 2>` of the open level-1 / level-2 names and their colors.
- `pushRayLabel(cmd, name, color)` / `popRayLabel(cmd)` replace RAII inside `recordRayCommands` and its callees.
- `recordIsolated`:
  1. ends every open label on the old segment;
  2. labels the SDK segment `"<group> · <leaf> (SDK)"`, e.g. `Indirect lighting · NRC train (SDK)` or `Reconstruction · DLSS-RR (SDK)`;
  3. re-begins the stack, bottom to top, on the new segment.

  Nsight then shows the group as adjacent ranges of the same name with the SDK range between them.
- `beginRaySegment` / `cmd.end()` of a ray segment `assert` that the stack depth matches what was pushed, which catches unbalanced labels in debug builds. Release builds rely on the validation run in §5.
- The frame and present command buffers never split, so they use `ScopedCmdLabel` only.

### 2.5 Event markers

`vkCmdInsertDebugUtilsLabelEXT` instant markers are emitted at the top of `recordFrameCommands` (after `gpuTimer.begin`) for events `drawFrame` decided this frame. Each mirrors an existing `TracyMessageL`:

| Marker | Source |
|---|---|
| `History reset` | `vk_renderer.cpp:406` |
| `Indirect history reset` | `vk_renderer.cpp:248` |
| `NRC network diverged` | `vk_renderer.cpp:278` |
| `RTXDI light PDF grown` | `vk_rtxdi.cpp:370` |
| `Direct mode changed` | `isDiModeChanged` (M8.3 §5.1) |

`Renderer` collects them in a fixed `std::array<const char*, 8>` plus a count, cleared per frame. No strings are formatted; the log keeps the detailed text. `Swapchain recreated` has no command buffer at its call site and stays Tracy-only.

### 2.6 Object names (audit only)

GPU Trace and crash dumps already read `vkSetDebugUtilsObjectNameEXT` names. P1 audits that every object Nsight shows in a pass's resource list is named:
- shader objects;
- lazy targets (`RawIndirectWeight`, `GiStagnancy_*`, created via `createTarget` with names);
- DI / GI reservoirs;
- NRC / SHaRC buffers;
- the DGC state command buffers (`vk_dgc.cpp:143`).

Unnamed objects found are fixed in place. No new naming API.

---

## 3. Phases

| Phase | Scope | Done when |
|---|---|---|
| **P1 · Frame passes** | §2.1 helpers, §2.2 names/colors, §2.3 graphics-queue hierarchy, §2.4 segment stack, §2.6 audit | GPU Trace on the RTX 5070 shows the §2.3 tree for `pipeline=targetRis`, `pipeline=rtxdi` and `frame.directMode=off frame.indirectBounce=off`; validation clean (§5) |
| **P2 · Async compute + events** | §2.3 compute-queue labels, §2.5 event markers | GPU Trace shows `Async compute` overlapping `Raster early/late`; a scripted `ENGINE_INDIRECT_MODE=cycle` run shows `Indirect history reset` markers at each switch |
| **P3 · Load time + queue labels** | Labels around model upload / CLAS / BLAS / OMM builds (`vk_mega_geometry.cpp`, `vk_asset_buffers.cpp`, one `Model load <name>` per model, name built once per load); optional `vkQueueBeginDebugUtilsLabelEXT` frame labels after §7 Q1 is answered | A capture of scene load shows one range per model with its build passes |

Per frame P1+P2 adds about 25 groups/leaves on the graphics queue, 3 on the compute queue and ≤ 8 inserts. No GPU work, no barriers, no allocations.

---

## 4. Constraints carried from the engine

- **Zero GPU cost, measured:** labels add no dispatch, barrier or descriptor write. The claim is gated on ABBA GPU total (§5), not assumed.
- **No new CPU waits, fences or `waitIdle`** (C-G3-6 / BUG-B4 submit structure unchanged). `preSdkRaySegmentCount` and `needsPostTlasWait` keep their values; labels never open a segment.
- **Release builds keep labels.** Profiling runs on release builds with layers off (vulkan-dev §6.1), and that is where Nsight is used. No build flag. If §5 shows a CPU cost above noise, add a runtime `ENGINE_GPU_LABELS=0` switch; do not strip at compile time.
- **Consistent order per configuration**, for Multi-Pass Metrics: the label sequence depends only on toggles and scene state, never on frame-varying data (no frame number in names).
- Tracy stays mandatory for new passes. A new pass adds a timer scope, a Tracy zone and a label together.

---

## 5. Verification

| Gate | How | Pass |
|---|---|---|
| CPU unit tests | `ctest`, `windows-clang-release` and `-checks` | unchanged pass count |
| Validation | Core + Synchronization + Best Practices + NVIDIA (`REPORT_FLAGS=error,warn,perf`): the 5 `pipeline=*` presets, `frame.directMode=off`, `frame.indirectBounce=off`, `frame.denoiser=off frame.aa=accumulation`, and the 200-frame live toggle cycle from M8.3 §8.4 | 0 new messages, in particular none of `VUID-vkCmdEndDebugUtilsLabelEXT-commandBuffer-01912` |
| Balance | debug build, the same runs | no §2.4 stack assertion |
| Nsight GPU Trace | RTX 5070, driver 621.40, release build, layers off, Sponza gallery, `pipeline=targetRis` and `pipeline=rtxdi` | §2.3 tree on the graphics and compute queues; SDK ranges named; Multi-Pass Metrics capture completes |
| Nsight Frame Debugger | same build | every dispatch / draw / DGC execute sits under a §2.3 label |
| GPU cost | ABBA, `ENGINE_BENCH_FRAMES=600`, 2560×1440 Quality, Sponza, labels on vs a build with the helpers compiled as no-ops | GPU total within ±5% noise ([engine bench mode](../../../docs/settings.md)) |
| CPU cost | Tracy `RecordFrameCommands` + ray/present recording zones, same ABBA | delta reported in this spec; above noise → add the §4 runtime switch |
| Checks | `ENGINE_GUIDE_CAPTURE=1 ENGINE_RT_CHECK=1`, `ENGINE_GI_CHECK=1`, `ENGINE_STABILITY_CHECK=1` | byte-identical results (labels change no GPU work) |

---

## 6. Follow-ups (not in this spec)

- **Nsight Aftermath** (`VK_NV_device_diagnostic_checkpoints` / Aftermath SDK): crash-time pass attribution. It would reuse the §2.2 names as checkpoint markers.
- **Slang debug info** for the Nsight shader profiler's source view (`-g` / `NonSemantic.Shader.DebugInfo`): a build-time cost question, separate spec.
- **Nsight Perf SDK** in-app range profiler: would reuse the same label tree.

---

## 7. Open questions

1. **Queue frame labels (P3):** with the deferred present, should `Frame N` enclose frame N's submits only (present of N−1 lands inside it), or be skipped? GPU Trace already delimits frames by present. Decide after a P1 capture shows whether frame boundaries read correctly without it.
2. **NRC / NGX internal labels:** do the NRC 0.16 and NGX SDKs record their own labels inside their segments? If they do, the `(SDK)` range nests them; if not, the SDK range is opaque. Check in the P1 capture and note it here.
