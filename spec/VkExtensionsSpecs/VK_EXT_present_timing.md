# VK_EXT_present_timing — Implementation Spec

**Status in this repo: EXTENSION LISTED but FEATURES NOT ENABLED (zero-init with comment “disabled: extension not enabled”), NOT CONSUMED.**

Official sources:

- [VK_EXT_present_timing](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_present_timing.html)
- [Proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_present_timing.html)
- [VkPhysicalDevicePresentTimingFeaturesEXT](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDevicePresentTimingFeaturesEXT.html)
- [vkGetPastPresentationTimingEXT](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPastPresentationTimingEXT.html)

**Hard dependencies (already listed in `requiredDeviceExtension`):**

- `VK_KHR_swapchain`
- `VK_KHR_present_id2`
- `VK_KHR_get_surface_capabilities2` (instance)
- `VK_KHR_calibrated_timestamps`

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` | `vk::EXTPresentTimingExtensionName` **present** |
| Feature chain slot | `src/core/vk_device.cpp` | `vk::PhysicalDevicePresentTimingFeaturesEXT` **in chain** |
| Feature values | same | **`{}` zero-init** — comment: `// vk::PhysicalDevicePresentTimingFeaturesEXT (disabled: extension not enabled)` |
| present_id2 feature | feature chain | **Not present / not enabled** (required peer) |
| Calibrated timestamps | extension only | Listed, unused |
| Swapchain create flag | `VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT` | **Not set** |
| Present path | `vk_renderer.cpp` | No `PresentTimingsInfoEXT` / IDs |
| Queries | timing properties / past timings | **None** |

### Already wired (partial / inconsistent)

```cpp
// src/core/vk_device.hpp — extension IS listed
vk::EXTPresentTimingExtensionName,
vk::KHRPresentId2ExtensionName,           // dependency
vk::KHRCalibratedTimestampsExtensionName, // dependency

// src/core/vk_device.cpp — feature struct present but all false
// vk::PhysicalDevicePresentTimingFeaturesEXT (disabled: extension not enabled)
{},
```

### Critical inconsistency to document

The **extension name is required** at device create, but **all present-timing feature bits are false**. That means:

- The extension may load, but present-timing **entry points and behaviors that require features must not be used**.
- Comment text says “extension not enabled” while the extension **is** in `requiredDeviceExtension` — treat features as the real gate; fix either the comment or the list when implementing.

### Not wired yet (full stack)

1. Enable `presentTiming` (+ absolute/relative time bits as needed).
2. Enable `presentId2` feature + surface/create/present ID plumbing.
3. Query `VkPresentTimingSurfaceCapabilitiesEXT`.
4. Create swapchain with `VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT`.
5. Set timing queue size; query timing / time-domain properties.
6. Pass `VkPresentTimingsInfoEXT` on present with present IDs.
7. Poll `vkGetPastPresentationTimingEXT` for feedback.
8. Optionally schedule target present times; calibrate domains.

---

## 2. What the extension provides

WSI has no standard way to read actual present times or request a target present time. present_timing adds past-timing queries and optional absolute/relative target times. In this repo the extension is **listed** but all feature bits are **false** — it is not enabled. `VK_KHR_present_id2` + `VK_KHR_calibrated_timestamps` are hard dependencies (listed; present_id2 feature still missing).

Listing the extension does **not** allow timing commands, set the create flag, or guarantee surface support. Do not call timing entry points while features remain zero-init.

| Feature | API | Engine relevance |
| --- | --- | --- |
| Past presentation timing | `vkGetPastPresentationTimingEXT` | Actual vs target present times, stages |
| Swapchain timing properties | `vkGetSwapchainTimingPropertiesEXT` | Refresh duration, FRR vs VRR |
| Time domains | `vkGetSwapchainTimeDomainPropertiesEXT` | Domains available for this swapchain |
| Queue size | `vkSetSwapchainPresentTimingQueueSizeEXT` | How many past results to retain |
| Present-time request | `VkPresentTimingsInfoEXT` / `VkPresentTimingInfoEXT` | Target absolute/relative present time |
| Surface capabilities | `VkPresentTimingSurfaceCapabilitiesEXT` | What the surface allows |
| Calibration hook | `VkSwapchainCalibratedTimestampInfoEXT` | With calibrated_timestamps |

Goal: smooth pacing, less stutter, correct motion when present time is known.

---

## 3. Structures and commands

### 3.1 Features (currently all false)

```cpp
// Provided by VK_EXT_present_timing
typedef struct VkPhysicalDevicePresentTimingFeaturesEXT {
    VkStructureType sType;
    void*           pNext;
    VkBool32        presentTiming;          // past presentation timing queries
    VkBool32        presentAtAbsoluteTime;  // absolute target present time
    VkBool32        presentAtRelativeTime;  // relative target present time
} VkPhysicalDevicePresentTimingFeaturesEXT;
```

Recommended enablement when bringing online:

```cpp
vk::PhysicalDevicePresentTimingFeaturesEXT{
    .presentTiming = true,
    .presentAtAbsoluteTime = true,  // if surface/device supports
    .presentAtRelativeTime = true   // if desired / supported
}
```

Query support first via `getFeatures2`; only force bits the GPU sets true, or keep them required and fail device create on unsupported hardware.

### 3.2 Surface capabilities (chain on capabilities2)

```cpp
typedef struct VkPresentTimingSurfaceCapabilitiesEXT {
    VkStructureType sType;
    void*           pNext;
    // members: presentTimingSupported, presentAtAbsoluteTimeSupported, etc.
    // (see current refpage for exact fields)
} VkPresentTimingSurfaceCapabilitiesEXT;
```

### 3.3 Swapchain create flag

`VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT` / `vk::SwapchainCreateFlagBitsKHR::ePresentTimingEXT`

Also set present_id2 create flag when using IDs.

### 3.4 Present timings info

```cpp
typedef struct VkPresentTimingsInfoEXT {
    VkStructureType              sType;
    const void*                  pNext;
    uint32_t                     swapchainCount;
    const VkPresentTimingInfoEXT* pTimingInfos;
} VkPresentTimingsInfoEXT;

typedef struct VkPresentTimingInfoEXT {
    VkStructureType              sType;
    const void*                  pNext;
    VkPresentTimingInfoFlagsEXT  flags;
    uint64_t                     targetTime; // 0 = no target / feedback only
    // ... stage masks / relative vs absolute per spec
} VkPresentTimingInfoEXT;
```

Chain on `VkPresentInfoKHR` together with `VkPresentId2KHR` (IDs required for correlation).

### 3.5 Key commands

| Command | Purpose |
| --- | --- |
| `vkSetSwapchainPresentTimingQueueSizeEXT` | Configure result queue depth |
| `vkGetSwapchainTimingPropertiesEXT` | refreshDuration, variable/fixed refresh |
| `vkGetSwapchainTimeDomainPropertiesEXT` | time domains for this swapchain |
| `vkGetPastPresentationTimingEXT` | drain past timing results |
| `vkGetCalibratedTimestampsKHR` + swapchain pNext | map PE time to host |

Results may return `VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT` if the app does not drain results.

### 3.6 Past presentation timing (conceptual)

```cpp
typedef struct VkPastPresentationTimingEXT {
    // presentId, actualPresentTime, earliestPresentTime, present margins, stage times, flags, ...
    // exact members: see refpage VkPastPresentationTimingEXT
} VkPastPresentationTimingEXT;
```

## Limits

Features currently **not enabled** (all `false`): `presentTiming`, `presentAtAbsoluteTime`, `presentAtRelativeTime`.

No dedicated numeric Required Limits table. Official required-limits chapter: [limits](https://docs.vulkan.org/spec/latest/chapters/limits.html). Surface capabilities via `VkPresentTimingSurfaceCapabilitiesEXT` (capabilities2):

| Field | Meaning |
| --- | --- |
| `presentTimingSupported` | Past-timing queries allowed for this surface |
| `presentAtAbsoluteTimeSupported` | Absolute target present time |
| `presentAtRelativeTimeSupported` | Relative target present time |
| `presentStageQueries` | Which present stages can be timestamped |

Query these; do not invent minima. `refreshDuration` from swapchain timing properties may be 0 until after first presents — not a Required Limit.

---

## 4. Where to implement in this codebase

### 4.1 Fix enablement policy — P0 decision

Pick one:

| Policy | Action |
| --- | --- |
| **A. Fully enable** (recommended when drivers ready) | Set present timing features true; enable present_id2 feature; implement create/present/query |
| **B. Fully optional** | Remove `EXTPresentTimingExtensionName` (and maybe deps) from required list until ready; keep features `{}` |

Current hybrid (listed + features false) is confusing for validation and for readers of `vk_device.cpp`.

### 4.2 Device create — when enabling (Policy A)

**File:** `src/core/vk_device.cpp`

```cpp
// vk::PhysicalDevicePresentTimingFeaturesEXT
{.presentTiming = true,
 .presentAtAbsoluteTime = true,
 .presentAtRelativeTime = false}, // example policy

// Also add:
// vk::PhysicalDevicePresentId2FeaturesKHR { .presentId2 = true }
```

### 4.3 Swapchain create

**File:** `src/core/vk_swapchain.cpp`

1. Query `PresentTimingSurfaceCapabilitiesEXT` (+ present id2 caps).
2. If supported, set create flags: present timing + present id2.
3. After create: `setSwapchainPresentTimingQueueSizeEXT` (e.g. 2× image count).
4. Query timing properties (refresh duration may be 0 until after first present — per issues section).

### 4.4 Present path

**File:** `src/render/vk_renderer.cpp`

```cpp
// Build pNext chain: PresentId2KHR → PresentTimingsInfoEXT → optional PresentFence
uint64_t id = ++nextPresentId;
vk::PresentId2KHR ids{ .swapchainCount = 1, .pPresentIds = &id };

vk::PresentTimingInfoEXT timing{
    .targetTime = 0 // feedback only until paced
};
vk::PresentTimingsInfoEXT timings{
    .swapchainCount = 1,
    .pTimingInfos = &timing
};
ids.pNext = &timings;

presentInfo.pNext = &ids;
```

Prefer `FIFO_LATEST_READY` when available (already preferred) for time-based present.

### 4.5 Feedback drain

After present or at frame start:

```cpp
// getPastPresentationTimingEXT → log / pace next targetTime
```

Use calibrated timestamps to convert PE times for HUD/Tracy if needed.

### 4.6 Suggested ownership

| Concern | Owner |
| --- | --- |
| Feature enable | `Device` |
| Timing queue size + properties | `SwapChain` |
| Present ID + timing info | `Renderer::drawFrame` |
| Pacing controller | small helper (optional) under `src/render/` or `src/util/` |

Do not bury pacing heuristics inside device init.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Decide Policy A vs B; fix comment / list consistency | `vk_device.*` | Clear enablement story |
| P1 | Enable `presentId2` + `presentTiming` features (if A) | `vk_device.cpp` | Legal use of entry points |
| P1 | Surface capability queries | `vk_swapchain.cpp` | Soft-fail if unsupported |
| P2 | Create flags + timing queue size | `vk_swapchain.cpp` | Swapchain participates in timing |
| P2 | Present IDs + empty/zero target timings | `vk_renderer.cpp` | Feedback path |
| P2 | Drain past presentation timings; log refreshDuration | swapchain/renderer | Metrics |
| P3 | Absolute/relative target present times | pacing helper | Actual stutter reduction |
| P3 | Calibrated timestamps for host mapping | util + device | Readable logs / Tracy |

### Implementation recipe

1. Policy: keep features **false** until Policy A. Do not call timing entry points. Comment vs list mismatch stays a P0 decision.
2. Instance already has `KHR_surface` + `KHR_get_surface_capabilities2` (+ `KHR_surface_maintenance1` for sibling WSI).
3. Keep listed deps: `KHR_present_id2`, `KHR_calibrated_timestamps`. present_id2 **feature** still absent.
4. Swapchain stays FIFO_LATEST_READY else FIFO, image count `max(3, min)` clamped (`src/core/vk_swapchain.cpp`). Acquire/present stay plain (`src/render/vk_renderer.cpp`).
5. When enabling: `presentId2 = true` + present_timing bits; query `PresentTimingSurfaceCapabilitiesEXT`; create flags; timing queue size; `PresentId2KHR` + `PresentTimingsInfoEXT` on present; drain past timings.
6. global_priority: query enabled, not used.

---

## 6. Concrete integration sketch (feedback-only first)

```cpp
// After present_timing + present_id2 features are true and swapchain created with flags:

// Once after create:
// device.setSwapchainPresentTimingQueueSizeEXT(*swapChain, queueSize, ...);

// Each frame present:
const uint64_t presentId = ++nextPresentId;
vk::PresentId2KHR presentIds{
    .swapchainCount = 1,
    .pPresentIds = &presentId
};

vk::PresentTimingInfoEXT timingInfo{
    .targetTime = 0 // request feedback only
};
vk::PresentTimingsInfoEXT timingsInfo{
    .swapchainCount = 1,
    .pTimingInfos = &timingInfo
};
presentIds.pNext = &timingsInfo;

vk::PresentInfoKHR presentInfo{
    .pNext = &presentIds,
    // wait semaphores, swapchain, indices ...
};

// Later / next frames:
// auto past = device.getPastPresentationTimingEXT(...);
// when refreshDuration becomes non-zero, start setting targetTime
```

Keep first milestone **feedback-only** before implementing a full pacing controller.

---

## 7. Validation / test checklist

1. **Current state:** device creates with extension listed and features false — no present-timing structs on present.
2. When enabling features: device create fails cleanly on unsupported GPUs if bits are forced true.
3. Surface caps: only set create flags when supported.
4. Present with timings + present id2: validation clean; IDs monotonic.
5. Drain past timings often enough to avoid `PRESENT_TIMING_QUEUE_FULL`.
6. `refreshDuration == 0` until enough presents — handle without spinning forever.
7. FRR vs VRR: IPD multiple of refresh for FRR; ≥ min refresh for VRR (per extension issues).
8. Interact with FIFO_LATEST_READY and maintenance1 present fence without pNext chain mistakes.
9. Resize/recreate: re-query timing properties; reset or continue present IDs per new swapchain.

---

## 8. Shader interaction

**None (direct).** This extension is host / queue / memory / presentation / resource plumbing only. It does not add shader stages, builtins, storage classes, or SPIR-V capabilities. Shaders continue to use whatever programmable stages the active pipeline or shader objects declare; this feature only changes how the host prepares or submits work those shaders run on.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| `VK_KHR_swapchain` | Base WSI |
| `VK_KHR_present_id2` | Required; IDs correlate timing records |
| `VK_KHR_calibrated_timestamps` | Required; map PE/host time |
| `VK_KHR_get_surface_capabilities2` | Surface timing caps |
| `VK_KHR_present_mode_fifo_latest_ready` | Preferred present mode for timed presents |
| `VK_KHR_swapchain_maintenance1` | Peer pNext (present fence); mode switching |
| Tracy | Optional display of present latency metrics |

---

## 10. Non-goals / do not do

- Do not call present-timing commands while features remain zero-init.
- Do not leave the “extension not enabled” comment once either the extension is removed or features are true — update docs/code together.
- Do not implement a complex PID/pacing controller before feedback logging works.
- Do not ignore queue-full errors.
- Do not assume fixed 16.67 ms refresh — always query.
- Do not enable only `presentTiming` without `presentId2` if the implementation path needs IDs (plan for both).

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is EXT_present_timing in the extension list? | **Yes** |
| Are features enabled? | **No** — all false with “disabled” comment |
| Is it used? | **No** |
| Dependencies listed? | present_id2 + calibrated_timestamps **yes**; present_id2 **feature** still missing |
| Highest-value next steps | Resolve list vs feature policy; enable present_id2 + presentTiming; surface caps; feedback-only present path |
| Secondary steps | Target present times; calibration; HUD/Tracy metrics |
