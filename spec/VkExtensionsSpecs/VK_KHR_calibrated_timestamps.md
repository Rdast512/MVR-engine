# VK_KHR_calibrated_timestamps — Implementation Spec

**Status in this repo: ENABLED (device extension listed as dependency for EXT_present_timing), NOT CONSUMED — no calibrateable domain queries or calibrated timestamp reads.**

Official sources:

- [VK_KHR_calibrated_timestamps](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_calibrated_timestamps.html)
- [Proposal (EXT heritage)](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_calibrated_timestamps.html)
- [VkCalibratedTimestampInfoKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkCalibratedTimestampInfoKHR.html)
- [vkGetCalibratedTimestampsKHR](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetCalibratedTimestampsKHR.html)
- [vkGetPhysicalDeviceCalibrateableTimeDomainsKHR](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPhysicalDeviceCalibrateableTimeDomainsKHR.html)

> Note: This extension has **no** feature struct. Enabling the device extension is enough to call the entry points. Present timing adds extra time domains and pNext on `VkCalibratedTimestampInfoKHR`.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` | `vk::KHRCalibratedTimestampsExtensionName` present; comment: required by `EXT_present_timing` |
| Feature enable | — | **N/A** — no feature structure |
| Domain query | — | **Not called** |
| Calibrated timestamp query | — | **Not called** |
| Present timing calibration pNext | — | Blocked on present_timing features (currently zero-init disabled) |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::KHRCalibratedTimestampsExtensionName, // required by EXT_present_timing
```

### Not wired yet

- `vkGetPhysicalDeviceCalibrateableTimeDomainsKHR` / `physicalDevice.getCalibrateableTimeDomainsKHR()`
- `vkGetCalibratedTimestampsKHR` / `device.getCalibratedTimestampsKHR(...)`
- Mapping host steady clock / device domain for Tracy or frame pacing
- `VkSwapchainCalibratedTimestampInfoEXT` when present_timing is active

---

## 2. What the extension provides

Host and GPU clocks are independent; correlating them needs one query that samples multiple time domains. calibrated_timestamps provides that and has **no** feature struct. This repo lists it only as a `VK_EXT_present_timing` dependency — domains and timestamps are not queried yet.

Enabling the extension does **not** pick a host domain, calibrate every frame, or enable present-stage / swapchain-local domains. Those extra domains need present_timing features, currently all **false**.

| Feature | API | Engine relevance |
| --- | --- | --- |
| List calibrateable domains | `vkGetPhysicalDeviceCalibrateableTimeDomainsKHR` | Discover host + device domains |
| Multi-domain timestamps | `vkGetCalibratedTimestampsKHR` | Quasi-simultaneous timestamps + max deviation |
| Time domain enum | `VkTimeDomainKHR` | e.g. device, clock monotonic, query performance counter (platform-dependent) |

Used standalone for CPU/GPU clock correlation, or as a **hard dependency** of `VK_EXT_present_timing` (swapchain-local / present-stage domains).

---

## 3. Structures and commands

### 3.1 Calibrated timestamp info

```cpp
// Provided by VK_KHR_calibrated_timestamps
typedef struct VkCalibratedTimestampInfoKHR {
    VkStructureType    sType; // VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR
    const void*        pNext;
    VkTimeDomainKHR    timeDomain;
} VkCalibratedTimestampInfoKHR;
```

With present_timing, pNext may include swapchain-scoped domain selection (`VkSwapchainCalibratedTimestampInfoEXT`).

### 3.2 Commands

```cpp
// Enumerate domains
std::vector<vk::TimeDomainKHR> domains =
    physicalDevice.getCalibrateableTimeDomainsKHR();

// Sample N domains together
std::array<vk::CalibratedTimestampInfoKHR, 2> infos = {
    vk::CalibratedTimestampInfoKHR{ .timeDomain = vk::TimeDomainKHR::eClockMonotonic },
    vk::CalibratedTimestampInfoKHR{ .timeDomain = vk::TimeDomainKHR::eDevice }
};
std::array<uint64_t, 2> timestamps{};
uint64_t maxDeviation = 0;
// Vulkan-Hpp:
// device.getCalibratedTimestampsKHR(infos, timestamps, &maxDeviation);
// (exact Hpp overload names may vary slightly by vulkan-hpp version)
```

`maxDeviation` is an upper bound on how far apart the samples may be — use it when correlating clocks.

### 3.3 Common domains (illustrative; query what the device actually supports)

| Domain | Typical use |
| --- | --- |
| `VK_TIME_DOMAIN_DEVICE_KHR` | GPU timestamp correlation |
| `VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR` | Host monotonic (POSIX) |
| `VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR` | Host QPC (Windows) |
| Present-timing domains | `VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT`, `VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT` (via EXT_present_timing) |

## Limits

No feature struct. No numeric Required Limits table. Official required-limits chapter: [limits](https://docs.vulkan.org/spec/latest/chapters/limits.html).

Query time domains via `vkGetPhysicalDeviceCalibrateableTimeDomainsKHR`. Do not invent domains.

Core `timestampPeriod` (`VkPhysicalDeviceLimits`) still applies to GPU / device-domain timestamp conversion.

`maxDeviation` from `vkGetCalibratedTimestampsKHR` is an upper bound, not a guaranteed error of zero.

---

## 4. Where to implement in this codebase

### 4.1 Extension list — DONE

Keep `vk::KHRCalibratedTimestampsExtensionName` while present_timing remains a planned/required dependency.

### 4.2 Init-time domain discovery

**File:** `src/core/vk_device.cpp` (after logical device / physical device selection)

```cpp
const auto domains = physicalDevice.getCalibrateableTimeDomainsKHR();
// log domains; cache preferred host domain for this platform
```

Optional: store preferred host + device domains on `Device` or `HardwareCapabilities`.

### 4.3 Correlation helper

**Suggested home:** `src/util/vk_utils.hpp` or a small `vk_time_calibration` helper used by:

- Tracy Vulkan calibration (if custom correlation is needed beyond Tracy’s path)
- Future present timing conversion between PE time and host time

```cpp
struct CalibratedSample {
    uint64_t hostTicks;
    uint64_t deviceTicks;
    uint64_t maxDeviation;
};
```

Call `getCalibratedTimestampsKHR` infrequently (init, after sleep, when clocks may drift) rather than every frame unless measuring.

### 4.4 Present timing integration

**Files:** swapchain + renderer (when EXT_present_timing features are turned on)

- Query swapchain time domain properties.
- Use calibrated timestamps with swapchain-local domains as defined by present_timing.
- Convert `actualPresentTime` / target times into host clocks for logging or pacing.

See `VK_EXT_present_timing.md`.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep extension listed | `vk_device.hpp` | present_timing dependency |
| P1 | Enumerate and log calibrateable domains at init | `vk_device.cpp` | Confirms driver support |
| P2 | Host↔device calibration helper | util / device | Reusable for profiling |
| P3 | Present-timing domain calibration | swapchain + present path | Only with present_timing features enabled |

### Implementation recipe

1. Keep `KHRCalibratedTimestampsExtensionName` listed (present_timing dep). No feature bit to flip.
2. Optional now: enumerate/log domains in `vk_device.cpp` after device pick. Instance already has `KHR_surface` + `KHR_get_surface_capabilities2`.
3. Do not call present-stage / swapchain-local domains until present_timing features are enabled (today **false**). present_id2 is the other required peer.
4. Swapchain create/present stay as-is: `src/core/vk_swapchain.cpp` prefers FIFO_LATEST_READY else FIFO, image count `max(3, min)` clamped; `src/render/vk_renderer.cpp` acquire/present.
5. When present_timing is on: calibrate with `VkSwapchainCalibratedTimestampInfoEXT` and convert PE times.
6. global_priority: query enabled, not used.

---

## 6. Concrete integration sketch

```cpp
// After device creation:
const auto domains = physicalDevice.getCalibrateableTimeDomainsKHR();
log_info(std::format("Calibrateable time domains: {}", domains.size()), "Device");

// Pick a host domain available on this OS:
vk::TimeDomainKHR hostDomain = vk::TimeDomainKHR::eClockMonotonic; // or QPC on Win
// verify hostDomain is in domains; else pick first non-device domain

std::vector<vk::CalibratedTimestampInfoKHR> infos = {
    { .timeDomain = hostDomain },
    { .timeDomain = vk::TimeDomainKHR::eDevice },
};
std::vector<uint64_t> ts(infos.size());
uint64_t maxDev = 0;
// device.getCalibratedTimestampsKHR(infos, ts, &maxDev);
```

Do not invent domains not returned by the query.

---

## 7. Validation / test checklist

1. Device creates with extension enabled.
2. Domain query succeeds; non-empty on supporting platforms.
3. Dual-domain sample returns finite timestamps and maxDeviation.
4. Re-calibration after long pause remains stable within maxDeviation budget.
5. With present_timing: swapchain-local domains only used when that extension’s features are enabled.
6. No calibrated timestamp calls if extension is ever made optional without a soft path.

---

## 8. Shader interaction

**None (direct).** This extension is host / queue / memory / presentation / resource plumbing only. It does not add shader stages, builtins, storage classes, or SPIR-V capabilities. Shaders continue to use whatever programmable stages the active pipeline or shader objects declare; this feature only changes how the host prepares or submits work those shaders run on.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Vulkan 1.1 / properties2 | Extension dependency baseline |
| `VK_EXT_present_timing` | Requires this extension; adds domains + pNext |
| Tracy / GPU timers | Optional consumer of host↔device correlation |
| Platform clocks | Domain availability differs (QPC vs monotonic) |

---

## 10. Non-goals / do not do

- Do not assume a feature bit exists — there is none.
- Do not hardcode host domain without checking calibrateable list.
- Do not sample every frame unless profiling proves the cost is acceptable.
- Do not use present-stage domains without EXT_present_timing fully enabled.
- Do not treat maxDeviation as zero error — it is a bound.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is calibrated_timestamps listed? | **Yes** — dependency of present_timing |
| Is it used? | **No** — no domain or timestamp queries yet |
| Highest-value next steps | Log domains; host↔device helper; then present_timing calibration |
| Secondary steps | Tracy/custom profiler correlation |
