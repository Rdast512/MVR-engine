# Feature: `VK_EXT_present_timing`: present feedback, display metrics and paced FIFO

**Date:** 2026-11-26 · **Status:** planned · **Milestone:** M8 tooling and display path. P1 lands before [M8.5](../../m8/m8_5_perf_and_nvidia_benchmark.md) P6, the head-to-head FPS comparison, because it supplies that comparison's frame-interval numbers. P2 lands after [M8.2](../../m8/README.md), so its one new control is born audited.
**Fixes:** the extension is required at device creation but nothing uses it, and the feature comment at `src/core/vk_device.cpp:162` says so. This leaves no display-side frame timing: `fps` / `frameMs` in `src/core/vk_engine.cpp:271-272` come from the CPU loop, and the unfocused window is paced by `SDL_Delay` (`vk_engine.cpp:399-406`).
**Touches:**
- `src/core/vk_device.{hpp,cpp}`: features;
- `src/core/vk_swapchain.{hpp,cpp}`: caps, flags, queue size, time domain;
- `src/render/vk_renderer.{hpp,cpp}`: `PendingPresent`, `finishPreviousFrame`;
- new `src/render/vk_present_timing.{hpp,cpp}`;
- `src/checks/engine_bench.cpp`: present line;
- engine UI stats, and `SettingsRegistry` for P2;
- [VkExtensionsSpecs/VK_EXT_present_timing.md](../../VkExtensionsSpecs/VK_EXT_present_timing.md): corrected per §2.

**Reference:** no NVIDIA sample uses the extension yet. `reference/vk_minimal_latest/src/vk_framework.h:1480-1545` (`FramePacer`, `getMonitorsMinRefreshRate`) is the CPU-sleep pacer that this replaces, and its comment at :1537 waits for exactly this extension. The API facts in §2 are checked against the installed SDK 1.4.363.1 (`vulkan_core.h`, `vulkan_raii.hpp`, `share/vulkan/registry/validusage.json`), the [proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_present_timing.html) and the refpages for `VkPresentTimingInfoEXT`, `VkPresentTimingsInfoEXT`, `vkGetPastPresentationTimingEXT`, `VkSwapchainTimingPropertiesEXT`, `vkSetSwapchainPresentTimingQueueSizeEXT` and `vkQueuePresentKHR`.

---

## 0. Why, with M8 in mind

| M8 need | What present timing gives |
|---|---|
| **M8.5 P6** records FPS "mean and 1% low" against RTXPT / RTXDI / RTXGI, and says our `[Bench]` numbers alone are not enough | Per-present `QUEUE_OPERATIONS_END` and `IMAGE_FIRST_PIXEL_OUT` times from the presentation engine. These give the same interval definitions a PresentMon or Nsight Systems capture of the demos uses, instead of CPU-loop time |
| **M8.5** judges CPU/GPU overlap. The present is deferred by one CPU frame (`PendingPresent`, issued in `finishPreviousFrame` after the next frame's commands, `vk_renderer.cpp:330-335`), which keeps C-G3-6's "no submit while NRC/NGX CUDA runs" | **Frame latency** (frame start → first pixel out) and **present latency** (present call → first pixel out). They make the cost of that deferral measurable. Today it is invisible |
| `FIFO_LATEST_READY`, the default, can replace queued images | **Replaced presents**: records whose later stages report time 0. That is GPU work rendered and never shown |
| **M8.3**: a disabled stage costs 0 ms | `Off` chains nothing and drains nothing. `Feedback` is host-only: no GPU work, no submit, no barrier |
| **M8.6** runners stay byte-repeatable on a fixed clock | Present timing never feeds simulation time or GPU constants. Automated runs force `IMMEDIATE` (`vk_engine.cpp:108`), where target times are illegal (VUID-12235), so they run `Feedback` only |
| Interactive smoothness on the 60–144 Hz FRR monitors of both dev PCs | **P2 paced FIFO:** a relative present target at a whole number of refresh cycles. It gives a stable cadence (30 instead of alternating 60/30 at ~45 fps) without the 1 ms Windows sleep fudge `FramePacer` needs |

Not a goal: input-latency reduction beyond what pacing gives (no Reflex: NVIDIA-only SDK, not integrated), frame generation, and presenting earlier than the frame graph allows.

---

## 1. What the drivers expose

RTX 5070, driver 621.40, `vulkaninfo` on 2026-11-26, windowed SDL surface:

| Query | Value |
|---|---|
| `VkPhysicalDevicePresentTimingFeaturesEXT` | `presentTiming`, `presentAtAbsoluteTime`, `presentAtRelativeTime` all `true` |
| `VkPhysicalDevicePresentId2FeaturesKHR::presentId2` | `true` |
| `VkPresentTimingSurfaceCapabilitiesEXT` | `presentTimingSupported = true`, **`presentAtAbsoluteTimeSupported = false`**, `presentAtRelativeTimeSupported = true` |
| `presentStageQueries` | **`QUEUE_OPERATIONS_END` and `IMAGE_FIRST_PIXEL_OUT` only**. No `REQUEST_DEQUEUED`, no `IMAGE_FIRST_PIXEL_VISIBLE` |
| `VkSurfaceCapabilitiesPresentId2KHR::presentId2Supported` | `true` |
| Implementation | `VK_LAYER_NV_present` (NVIDIA Presentation Layer, 1.4.357) |

**Consequences for the design:**
- Absolute target times cannot be used on this surface, so pacing is built on **relative** times.
- The proposal's pacing recipe (§4.4: target = last dequeued time + n × IPD) needs `REQUEST_DEQUEUED`, which is not offered. `IMAGE_FIRST_PIXEL_OUT` is the display-side stage here.
- Every surface-level value is queried per swapchain, never assumed: caps can change between windowed and fullscreen, across monitors, and over remote desktop.

The **RTX 2060 Super** (floor) is unverified. P0 runs the same `vulkaninfo` query there before any feature becomes required (C-PT-1).

---

## 2. API facts the design depends on

Each fact was checked in the SDK 1.4.363.1 headers and `validusage.json`. The vendored [VkExtensionsSpecs page](../../VkExtensionsSpecs/VK_EXT_present_timing.md) still has placeholder structs ("see current refpage for exact fields") and misses the time-domain-id rule, so P0 replaces its §3 with this list.

1. **Structs (spec version 3):**
   - `VkPresentTimingInfoEXT {flags, targetTime, timeDomainId, presentStageQueries, targetTimeDomainPresentStage}`;
   - `VkPastPresentationTimingEXT {presentId, targetTime, presentStageCount, pPresentStages, timeDomain, timeDomainId, reportComplete}`;
   - `VkSwapchainTimingPropertiesEXT {refreshDuration, refreshInterval}` (ns);
   - `VkPastPresentationTimingPropertiesEXT {timingPropertiesCounter, timeDomainsCounter, presentationTimingCount, pPresentationTimings}`.
2. **`timeDomainId` is always validated.** VUID-VkPresentTimingInfoEXT-timeDomainId-12400 says it must be an id returned by `vkGetSwapchainTimeDomainPropertiesEXT`, with no `targetTime != 0` condition. A feedback-only present with `timeDomainId = 0` is therefore invalid unless 0 is a returned id. The swapchain queries its domains once after creation and every time `timeDomainsCounter` changes.
3. **Swapchain flags:**
   - `ePresentTimingEXT` needs one of the three features enabled (VUID-VkSwapchainCreateInfoKHR-presentTiming-12232). Every swapchain in a present carrying `VkPresentTimingsInfoEXT` must have it (VUID-12234).
   - `ePresentId2KHR` is required for a `VkPresentId2KHR` (VUID-10820). Non-zero IDs need the `presentId2` feature (VUID-VkPresentInfoKHR-pNext-10821) and `presentId2Supported` (VUID-10822), and must strictly increase per swapchain (VUID-10819). Gaps are allowed.
4. **Target times:**
   - non-zero `targetTime` only in `FIFO`, `FIFO_RELAXED` or `FIFO_LATEST_READY` (VUID-VkPresentTimingsInfoEXT-pSwapchains-12235);
   - relative targets need the `presentAtRelativeTime` feature and surface support (VUID-12237), and absolute targets the same for absolute (VUID-12236);
   - relative means "the minimum duration the previous image stays visible". `PRESENT_AT_NEAREST_REFRESH_CYCLE` lets the image land on the refresh cycle closest to the target and absorbs rounding error.
5. **Queue:**
   - only presents with `presentStageQueries != 0` take a slot. `vkGetPastPresentationTimingEXT` frees one slot per returned record with `reportComplete = VK_TRUE`;
   - a present that needs a slot when none is free, or before `vkSetSwapchainPresentTimingQueueSizeEXT` was ever called, returns `VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT`. The queue size is per swapchain, so **every recreated swapchain must set it again**;
   - on that error the wait semaphores still execute and the image is released but not shown (`vkQueuePresentKHR` lists it with OUT_OF_DATE and SURFACE_LOST). The image cannot be presented again without a new acquire;
   - `vk::raii::Queue::presentKHR` treats only `eSuccess`, `eSuboptimalKHR` and (with `VULKAN_HPP_HANDLE_ERROR_OUT_OF_DATE_AS_SUCCESS`, `src/pch.h:43`) `eErrorOutOfDateKHR` as success. QUEUE_FULL arrives as a thrown `vk::SystemError`;
   - growing the queue is legal at any time. Shrinking it below the outstanding count returns `VK_NOT_READY`.
6. **Drain:**
   - each output element's `presentStageCount` must be at least the number of stages requested by the earliest present not yet completed (VUID-vkGetPastPresentationTimingEXT-flags-12231, in-order mode);
   - `VK_INCOMPLETE` means more records are waiting;
   - a stage the request never reached reports time 0. This happens when `FIFO_LATEST_READY` replaces the image, or on platform events.
7. **Properties:**
   - `vkGetSwapchainTimingPropertiesEXT` returns `VK_NOT_READY` until at least one image has been presented, and again after a change;
   - `refreshDuration == 0` means unknown;
   - `refreshInterval == refreshDuration` means FRR, `UINT64_MAX` means VRR (`refreshDuration` is then the minimum), and 0 means the dynamics are unknown;
   - changes are announced only by the counters in each drain result.
8. **Calibration:** `vkGetCalibratedTimestampsKHR` with `VkCalibratedTimestampInfoKHR{timeDomain = eSwapchainLocalEXT}` chained to `VkSwapchainCalibratedTimestampInfoEXT{swapchain, presentStage = 0, timeDomainId}` maps the swapchain clock. A `ePresentStageLocalEXT` domain needs exactly one stage bit (VUID-12228). Paired in the same call with `eQueryPerformanceCounter`, this gives host time.
9. **vulkan.hpp entry points:**
   - `vk::raii::SwapchainKHR::setPresentTimingQueueSizeEXT(uint32_t)` returns `Result` (success or `eNotReady`);
   - `getTimingPropertiesEXT()` returns `ResultValue<pair<props, counter>>`;
   - `getTimeDomainPropertiesEXT(props*, counter*)` takes pointers, so the code does the count-then-fill pattern itself;
   - `vk::raii::Device::getPastPresentationTimingEXT(info*, props*)` takes pointers.

---

## 3. Design

### 3.1 Device: `vk_device.cpp`

- Add `vk::PhysicalDevicePresentId2FeaturesKHR` to `FeatureChain`.
- Add to `kRequiredFeatures`:
  - `PresentId2FeaturesKHR::presentId2`;
  - `PresentTimingFeaturesEXT::presentTiming`;
  - `PresentTimingFeaturesEXT::presentAtRelativeTime`.
- Delete the `:162` comment.
- `presentAtAbsoluteTime` stays off: no surface on the reference GPU supports it, and nothing uses it (YAGNI). It is enabled when a surface reports support and a use exists.
- Device creation fails loudly like every other required feature (`failStep`), after P0 confirms the 2060 Super has them.
- At init, log whether `getCalibrateableTimeDomainsKHR()` contains `eQueryPerformanceCounter`. Without it, latency metrics are off and intervals still work.

### 3.2 Swapchain: `vk_swapchain.cpp`

`createSwapChain` already queries a `SurfaceCapabilities2KHR` chain with the chosen present mode (`SurfacePresentModeKHR` in `surfaceInfo2.pNext`). Extend that chain with:
- `vk::PresentTimingSurfaceCapabilitiesEXT`;
- `vk::SurfaceCapabilitiesPresentId2KHR`.

The caps then belong to the mode actually used. The raw dispatcher re-query at `:104-108` fills the same chain.

New public state on `SwapChain`, in the style of the class (public handles, no getters):

```cpp
struct PresentTimingCaps
{
    bool isSupported = false;           // presentTimingSupported && presentId2Supported
    bool isRelativeTargetSupported = false;
    vk::PresentStageFlagsEXT stages{};  // surface presentStageQueries
};
PresentTimingCaps presentTimingCaps;
vk::PresentModeKHR presentMode = vk::PresentModeKHR::eFifo; // chosen mode; VUID-12235 gate
uint32_t generation = 0;                                     // bumped per createSwapChain
```

When `isSupported`:
1. Set `flags = ePresentTimingEXT | ePresentId2KHR` on `SwapchainCreateInfoKHR`.
2. After creation, `setPresentTimingQueueSizeEXT(initial)` with `initial = 2 × image count` (proposal issue 5.2).
3. Query time domains (count, then fill). Pick `eSwapchainLocalEXT` if offered, otherwise `ePresentStageLocalEXT`, which every implementation supports. Store the id and domain in `PresentTimingFeedback` (§3.4).
4. Log caps, stages, domains and the queue size once per swapchain (`SwapChain` category).

When not supported (for example a remote session), no flag is set, presents carry no timing chain, and the UI shows "present timing unsupported on this surface". This is a per-surface soft gate, not an engine failure.

`recreateSwapChain` keeps its order (wait idle, retire, create from old). Records still outstanding on the retired swapchain are abandoned. `PresentTimingFeedback::onSwapchainCreated(generation)` resets the slot count, the domain, the cached properties and the pacing baseline. The refresh rate may have changed after a monitor move.

### 3.3 Present: `vk_renderer.cpp`

The present stays where it is (`finishPreviousFrame`, after the next frame's commands are recorded). Moving it would undo C-G3-6. The changes:

- `PendingPresent` gains `uint64_t presentId`, set to the `frameNumber` of the frame it shows. That number already increases strictly, and the out-of-date path skips presenting, which only leaves a legal gap. The ID then joins present records directly to the CPU stats ring and the GPU timer slot of the same frame.
- `drawFrame` stores, per present ID, the host time of the frame start (`SDL_GetPerformanceCounter`, QPC on Windows) in a ring sized to the timing queue.
- In `finishPreviousFrame`, before `presentKHR`:
  1. `feedback.drain(swapChain)` (§3.4) frees slots;
  2. `feedback.buildPresent(present.presentId, mode)` returns the `PresentId2KHR` → `PresentTimingsInfoEXT` chain, or nothing in `Off` / unsupported. The chain hangs off `PresentInfoKHR.pNext`;
  3. record the host present-call time for that ID.
- `VkPresentTimingInfoEXT` per present:
  - `presentStageQueries = caps.stages ∩ {QUEUE_OPERATIONS_END, IMAGE_FIRST_PIXEL_OUT, IMAGE_FIRST_PIXEL_VISIBLE}`. On the 5070 this is two stages;
  - `timeDomainId` = the queried id (fact 2);
  - `targetTime = 0` and `flags = 0` in `Feedback`. P2 sets them in `Paced`;
  - `targetTimeDomainPresentStage = 0`: it only matters for a non-zero target in a stage-local domain, and in that case P2 sets `IMAGE_FIRST_PIXEL_OUT` (VUID-12238).
- **Slot accounting, so QUEUE_FULL cannot happen:**
  - `outstanding` = presents sent with queries − complete records drained;
  - when `outstanding == capacity`, grow the queue ×2 up to `kMaxTimingQueue` (64) and log a warning;
  - at the cap, send this present without `presentStageQueries` (no slot) and count it in `unmeasuredPresents`.
  - A thrown QUEUE_FULL is then a bookkeeping bug. It propagates, so the bug fails loudly.
- No GPU work, no submit and no barrier is added. The drain is a host call between recording and presenting, so the "one submit while the SDKs run" rule holds.

### 3.4 `PresentTimingFeedback`: new `src/render/vk_present_timing.{hpp,cpp}`

Owned by `Renderer` and used only on the render thread: the swapchain is externally synchronized for `setPresentTimingQueueSizeEXT`. It is a plain class with fixed storage and no per-frame allocation.

- Storage: `capacity` × `VkPastPresentationTimingEXT`, and `capacity × kMaxStages` × `VkPresentStageTimeEXT`. Each element's `presentStageCount` is reset to the requested stage count before every call (VUID-12231).
- `drain()` runs `getPastPresentationTimingEXT` with `flags = 0` (in order, complete records only) and loops while `VK_INCOMPLETE`. For each record:
  - `reportComplete` frees a slot;
  - a stage time of 0 marks the present as **replaced** (`FIFO_LATEST_READY`) or dropped;
  - a record in another domain or domain id than the calibrated one (surface moved, proposal issue 5.4) still counts for intervals against its predecessor in the same domain, and is left out of latency.
  - The two counters are compared against the cached values. On a change, the code re-queries `getTimingPropertiesEXT` (and treats `eNotReady` as "unknown, ask again after the next present") or the time domains, recalibrates, and sends `TracyMessageL("Display timing changed")`.
- Calibration: one `getCalibratedTimestampsKHR` pairing the swapchain domain with QPC. It runs at swapchain creation, on a domain change and once per second against drift. The returned `maxDeviation` is kept and shown.
- **Derived metrics**, a rolling window of `kStatsWindow` (512) displayed presents plus whole-run totals for the bench:
  - `presentIntervalMs`: Δ `QUEUE_OPERATIONS_END`. This is present throughput, and valid under `IMMEDIATE`;
  - `displayIntervalMs`: Δ `IMAGE_FIRST_PIXEL_OUT` between displayed presents. This is what the user sees;
  - `presentLatencyMs`: present call → `IMAGE_FIRST_PIXEL_OUT`;
  - `frameLatencyMs`: frame start → `IMAGE_FIRST_PIXEL_OUT`. It includes the one-frame present deferral;
  - counters: displayed, replaced, unmeasured, late (P2);
  - refresh: `refreshDuration`, and the FRR / VRR / unknown classification from fact 7.
  - Mean and 1% low come from the interval distribution: 1% low FPS = 1000 / p99 interval, the definition PresentMon-based tools use. Percentiles use `nth_element` over a copy of the window, computed only when the UI or bench reads them.
- Tracy:
  - `ZoneScopedN("PresentTimingDrain")`;
  - plots `Present interval ms`, `Display interval ms`, `Present latency ms` and `Frame latency ms` (number format), plus `Replaced presents` per drain;
  - `TracyMessageL` on property or domain change and on queue growth.

### 3.5 Where the numbers show up

- **Engine UI stats:** next to the CPU-loop FPS, a line such as `Display 144.0 Hz FRR · 6.94 ms (1% low 7.61) · latency 9.8 ms · replaced 0`, or `present timing: unsupported` / `waiting for first present`. The CPU-loop `fps` stays, because it is a different quantity.
- **Bench** (`engine_bench.cpp`, beside the existing `CPU frame:` line), from whole-run totals over the measured frames only:
  ```
    present: 2000 records (0 unmeasured, 0 replaced), present interval mean 3.912 ms (1% low 231.4 fps),
             display interval mean ... ms, present latency mean ... ms p99 ... ms, frame latency mean ... ms, refresh 6.944 ms FRR
  ```
  Under `IMMEDIATE` the display interval may be meaningless: the window is composed, and the record is kept as reported. M8.5 P6 quotes the **present interval** for the throughput comparison with the demos, and the frame latency as an extra column.

### 3.6 P2 · paced FIFO

A `SettingsRegistry` entry `Present pacing`: `Off` | `Feedback` (default) | `Paced`. It sits next to the M8.3 toggles, with their key style, and is reachable through `ENGINE_CONFIG` for completeness. Changing it resets no lighting or DLSS-RR history, since nothing in the image depends on it.

- **Mode:**
  - `Paced` selects `FIFO` and recreates the swapchain through the existing `requestSwapchainRecreate`. Mode changes are rare user actions, and the recreate path already exists;
  - `FIFO`, not `FIFO_LATEST_READY`: a paced queue needs back pressure, and `FIFO_LATEST_READY` may still replace waiting images, which costs rendered frames. P2 measures whether `FIFO_LATEST_READY` with relative targets behaves identically on 621.40 (replaced-present counter). If it does, the recreate is dropped;
  - `Paced` is unavailable (the combo entry is disabled with the reason) when the surface lacks `presentAtRelativeTimeSupported`, or when `isUncapped` (automated runs, `IMMEDIATE`, VUID-12235).
- **Target per present:** `flags = PRESENT_AT_RELATIVE_TIME | PRESENT_AT_NEAREST_REFRESH_CYCLE`, `targetTime = k × refreshDuration` for FRR:
  - `k` starts at `ceil(frameCost / refreshDuration)`. `frameCost` is the larger of the GPU total (`gpuTimer`, already collected per slot) and the CPU `drawFrame` mean;
  - **raise** `k` after `kLateRaise` (2) late presents in the last 30. Late means `displayInterval > target + refreshDuration / 2`;
  - **lower** `k` after `kEarlyLower` (120) consecutive presents whose queue slack (`IMAGE_FIRST_PIXEL_OUT − QUEUE_OPERATIONS_END`) exceeds one refresh, while `frameCost < (k − 1) × refreshDuration × 0.9`. This replaces the proposal's `REQUEST_DEQUEUED` early estimate (issue 5.7), which this surface does not offer;
  - the constants are starting points, tuned by measurement and logged when they change.
- **VRR** (`refreshInterval == UINT64_MAX`): no target unless a frame cap is set. VRR already absorbs variance, and the target then equals the cap interval, always ≥ `refreshDuration`. **Unknown** (`refreshDuration == 0`): `targetTime = 0` until the properties arrive. Never spin on them.
- **Unfocused window:** under `Paced`, the 30 Hz cap becomes `targetTime = max(k × refreshDuration, 33.3 ms)`, rounded up to whole refreshes, and the `SDL_Delay` branch in `vk_engine.cpp:399-406` is skipped. FIFO back pressure throttles the CPU through the acquire and fence waits. In `Off` / `Feedback` the sleep stays.
- No baseline before the first displayed record: the first presents after a (re)create carry `targetTime = 0` (proposal §4.4).

### 3.7 Deferred · P3 display-time simulation step

The camera step (`vk_engine.cpp:339`) uses CPU loop `dt`. Under `Paced`, the predicted display time (last `IMAGE_FIRST_PIXEL_OUT` + k × refresh) could drive `dt`, removing the judder that a varying CPU `dt` causes on a steady display cadence. This waits until P2 numbers show the CPU `dt` variance is visible (C-PT-3). Checks keep their fixed clock in any case.

---

## 4. Steps and gates

| Step | Change | Gate |
|---|---|---|
| **P0** | `vulkaninfo` on the RTX 2060 Super (features, surface caps, stages). Log the 5070 swapchain time domains with a throwaway query. Correct the VkExtensionsSpecs page (§2) and its README row | Both GPUs report `presentTiming`, `presentAtRelativeTime`, `presentId2`. Otherwise the missing bit becomes optional, recorded in C-PT-1 |
| **P1a** | §3.1 device features, §3.2 caps / flags / queue size / time domain | Device and swapchain create, resize and recreate validation-clean (core + sync + best practices + NVIDIA) |
| **P1b** | §3.3 present chain + §3.4 drain in `Feedback` | No validation output over 10k frames including 20 resizes and a monitor move. `unmeasuredPresents == 0` at the default queue size under FIFO_LATEST_READY and IMMEDIATE. IDs strictly increasing |
| **P1c** | §3.5 UI, Tracy, bench line | FIFO on a 144 Hz FRR monitor: display interval mean within 0.5% of `refreshDuration` on a light scene. Bench `Off` vs `Feedback`: GPU total within ±5% noise, CPU `drawFrame` Δ < 0.02 ms (release build, vkconfig off, P0 clocks). All runners unchanged: `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST`, `ENGINE_GUIDE_CAPTURE`, `ENGINE_GI_CHECK`, temporal checks byte-identical |
| **P2** | §3.6 paced FIFO, after M8.2 | Bistro café at ~45 fps on 60 Hz: display interval p99 / p1 ≤ 1.05 at a steady 33.3 ms (unpaced: alternating 16.7 / 33.3). Late count 0 over 2,000 presents on a still camera. Unfocused window: CPU frame rate ≤ 31 Hz with no `SDL_Delay` |
| **P3** | §3.7, only when measured necessary | Decided by user after P2 |

---

## 5. Validation and correctness checklist

- [ ] The swapchain flag is set only when `presentTimingSupported && presentId2Supported` for the chosen mode's caps (VUID-12232, 10820, 10822).
- [ ] `timeDomainId` is always an id returned for **this** swapchain. It is re-queried after recreate and on a `timeDomainsCounter` change (VUID-12400).
- [ ] The queue size is set on every new swapchain before its first present with queries.
- [ ] `presentStageCount` is reset on every output element before each drain (VUID-12231).
- [ ] Non-zero `targetTime` only in FIFO modes with relative support (VUID-12235, 12237). Never under `isUncapped`.
- [ ] `VK_NOT_READY` from properties and `refreshDuration == 0` are handled without blocking.
- [ ] Records with zero stage times and records from a different domain are handled as in §3.4.
- [ ] No pNext chain on the present in `Off` or on unsupported surfaces. Present-chain storage lives until `presentKHR` returns (locals in `finishPreviousFrame`).
- [ ] Present ID gaps from out-of-date frames are legal, and IDs never repeat across swapchain generations (`frameNumber` is global).
- [ ] Tracy zone and plots present. No Tracy memory tracking: fixed-size storage (vulkan-dev §9).

---

## 6. Rejected alternatives

- **Absolute targets** (proposal §4.4): not supported by the reference surface.
- **CPU sleep pacer** (`FramePacer`): quantized by the Windows timer, it needs a 1 ms fudge and guesses the refresh from the monitor list. Present timing gives the swapchain's real refresh and real display times.
- **Partial or out-of-order results:** in-order complete records are enough for one swapchain and per-frame drains. The out-of-order VUID (12230) needs more bookkeeping for no gain.
- **Present-mode switching per present through `VkSwapchainPresentModeInfoKHR`** (swapchain_maintenance1) instead of recreating for `Paced`: workable, since the compatible-mode list is already passed. It is deferred, because the toggle is rare and recreate is the tested path. It can be revisited together with the maintenance1 present-fence TODO (`vk_swapchain.cpp:14`).
- **A sync wait on present results:** the proposal deliberately has none (issue 5.3). The engine polls once per present.

---

## 7. Considerations entries

Added to [considerations.md](../../considerations.md) under `M8 · PT present timing`: C-PT-1 (2060 Super unverified), C-PT-2 (no absolute targets and no `REQUEST_DEQUEUED` / `FIRST_PIXEL_VISIBLE` on the 5070 windowed surface), C-PT-3 (P3 deferred).
