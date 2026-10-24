# VK_KHR_present_id2 — Implementation Spec

**Status in this repo: ENABLED (device extension listed as dependency for EXT_present_timing), NOT CONSUMED — feature struct not in device create chain; no present IDs on present.**

Official sources:

- [VK_KHR_present_id2](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_present_id2.html)
- [Proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_present_id2.html)
- [VkPhysicalDevicePresentId2FeaturesKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDevicePresentId2FeaturesKHR.html)
- [VkPresentId2KHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkPresentId2KHR.html)

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` | `vk::KHRPresentId2ExtensionName` present; comment: required by `EXT_present_timing` |
| Feature enable | `src/core/vk_device.cpp` feature chain | **Missing** — no `PhysicalDevicePresentId2FeaturesKHR` entry |
| Surface capability | `getSurfaceCapabilities2KHR` + `SurfaceCapabilitiesPresentId2KHR` | **Not queried** |
| Swapchain create flag | `VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR` | **Not set** |
| Present path | `vk_renderer.cpp` | Plain `PresentInfoKHR` — no `PresentId2KHR` pNext |
| Consumption | — | **None** |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::KHRPresentId2ExtensionName, // required by EXT_present_timing
```

### Not wired yet

- `vk::PhysicalDevicePresentId2FeaturesKHR{ .presentId2 = true }` in feature chain
- Surface capability query `VkSurfaceCapabilitiesPresentId2KHR`
- `VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR` on swapchain create
- Monotonic `uint64_t` present IDs in `VkPresentId2KHR` on each present
- Any wait-for-present / timing correlation using those IDs

> **Important:** Listing the extension without enabling `presentId2` is insufficient to use present IDs. For `VK_EXT_present_timing`, enable present_id2 feature **before** (or together with) present timing features.

---

## 2. What the extension provides

Presents have no portable identity unless the app assigns one. present_id2 attaches monotonically increasing 64-bit IDs so timing / wait APIs can correlate frames. This repo lists the extension only as a `VK_EXT_present_timing` dependency — `presentId2` is **not** in the device feature chain.

Listing the extension does **not** enable IDs, set the swapchain create flag, or imply a minimum ID count. Do not chain `PresentId2KHR` until the feature, surface capability, and create flag are all on.

| Feature | New API / bit | Engine relevance |
| --- | --- | --- |
| Present identifiers | `VkPresentId2KHR` on `PresentInfoKHR` | Correlate presents with timing feedback / wait APIs |
| Feature bit | `presentId2` | Master switch |
| Surface capability | `VkSurfaceCapabilitiesPresentId2KHR` | Whether the surface supports present id2 |
| Swapchain create flag | `VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR` | Opt swapchain into present id2 |

IDs share namespace with legacy `VK_KHR_present_id` (monotonically increasing non-zero IDs per swapchain).

---

## 3. Structures and commands

### 3.1 Feature (must enable to use)

```cpp
// Provided by VK_KHR_present_id2
typedef struct VkPhysicalDevicePresentId2FeaturesKHR {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR
    void*           pNext;
    VkBool32        presentId2;
} VkPhysicalDevicePresentId2FeaturesKHR;
```

Vulkan-Hpp device create:

```cpp
vk::PhysicalDevicePresentId2FeaturesKHR{
    .presentId2 = true
}
```

### 3.2 Surface capabilities

```cpp
typedef struct VkSurfaceCapabilitiesPresentId2KHR {
    VkStructureType sType;
    void*           pNext;
    VkBool32        presentId2Supported;
} VkSurfaceCapabilitiesPresentId2KHR;
```

Chain onto `VkSurfaceCapabilities2KHR` via `getSurfaceCapabilities2KHR`. Only use present IDs when supported.

### 3.3 Swapchain create flag

`VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR` / `vk::SwapchainCreateFlagBitsKHR::ePresentId2`

### 3.4 Present ID struct

```cpp
// Provided by VK_KHR_present_id2
typedef struct VkPresentId2KHR {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR
    const void*     pNext;
    uint32_t        swapchainCount;
    const uint64_t* pPresentIds; // non-zero = ID; must be > any previous non-zero ID for that swapchain
} VkPresentId2KHR;
```

```cpp
uint64_t presentId = ++nextPresentId; // per swapchain, strictly increasing when non-zero

vk::PresentId2KHR idInfo{
    .swapchainCount = 1,
    .pPresentIds = &presentId
};

presentInfo.pNext = &idInfo;
```

Zero means “no id” for that swapchain entry.

## Limits

No dedicated feature in this repo’s chain (dependency only). No numeric Required Limits table. Official required-limits chapter: [limits](https://docs.vulkan.org/spec/latest/chapters/limits.html).

Present IDs are 64-bit, monotonically increasing, per swapchain handle. Zero means “no id”. There is **no** spec-required minimum ID count.

Surface gate: `VkSurfaceCapabilitiesPresentId2KHR::presentId2Supported` via capabilities2 — query before using IDs.

---

## 4. Where to implement in this codebase

### 4.1 Feature chain — REQUIRED before use

**File:** `src/core/vk_device.cpp` → `createLogicalDevice()` `StructureChain`

Add `vk::PhysicalDevicePresentId2FeaturesKHR` and set `{.presentId2 = true}`.  
Today the chain has present timing **disabled** (`{}`); when enabling present timing, enable present_id2 as well.

### 4.2 Surface capability check

**File:** `src/core/vk_swapchain.cpp` (alongside existing capabilities2 query)

```cpp
auto caps = physicalDevice.getSurfaceCapabilities2KHR<
    vk::SurfaceCapabilities2KHR,
    vk::SurfaceCapabilitiesPresentId2KHR>(surfaceInfo2);

const bool presentId2Ok =
    caps.get<vk::SurfaceCapabilitiesPresentId2KHR>().presentId2Supported;
```

Gate create flag and present pNext on this.

### 4.3 Swapchain create flag

**File:** `src/core/vk_swapchain.cpp`

```cpp
if (presentId2Ok) {
    swapChainCreateInfo.flags |= vk::SwapchainCreateFlagBitsKHR::ePresentId2;
}
```

### 4.4 Assign IDs at present

**Files:**

- `src/render/vk_renderer.cpp` → `drawFrame`
- optional counter on `SwapChain` (`uint64_t nextPresentId = 0`)

```cpp
const uint64_t id = ++swapChain.nextPresentId;

vk::PresentId2KHR presentId2{
    .swapchainCount = 1,
    .pPresentIds = &id
};
// chain with present fence / present timings pNext as needed
```

### 4.5 Consumers

- `VK_EXT_present_timing` past presentation timing entries reference present IDs.
- Any future wait-for-present API that accepts present id2 values.

Without consumers, enabling IDs is still useful as plumbing for timing work.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep extension name listed | `vk_device.hpp` | Dependency of present_timing |
| P1 | Add feature to device chain when turning on present timing | `vk_device.cpp` | Required for valid use |
| P1 | Query surface presentId2Supported | `vk_swapchain.cpp` | Gate create flag |
| P2 | Create flag + monotonic IDs on present | swapchain + renderer | Actual consumption |
| P3 | Wire IDs into present_timing feedback | timing integration | End-to-end frame pacing |

### Implementation recipe

1. Keep `KHRPresentId2ExtensionName` listed (required by EXT_present_timing). Do **not** chain `PresentId2KHR` while the feature is missing.
2. Instance already has `KHR_surface` + `KHR_get_surface_capabilities2` (+ `KHR_surface_maintenance1` for sibling WSI).
3. When enabling present_timing: add `{.presentId2 = true}` **together with** present_timing features (today those are all **false**).
4. `src/core/vk_swapchain.cpp`: query `presentId2Supported`; set `ePresentId2` only if supported. Keep FIFO_LATEST_READY else FIFO and image-count `max(3, min)` clamp.
5. `src/render/vk_renderer.cpp`: assign `++nextPresentId` on `presentKHR` `pNext`; IDs are per swapchain handle (recreate may restart at 1).
6. calibrated_timestamps is the other present_timing dep (listed, unused). global_priority: query enabled, not used.

---

## 6. Concrete integration sketch

```cpp
// Device feature chain (when enabling the stack):
// vk::PhysicalDevicePresentId2FeaturesKHR { .presentId2 = true }

// Present:
uint64_t id = ++nextPresentId;
vk::PresentId2KHR ids{ .swapchainCount = 1, .pPresentIds = &id };

vk::PresentInfoKHR presentInfo{
    .pNext = &ids,
    .waitSemaphoreCount = 1,
    .pWaitSemaphores = &*renderSemaphore,
    .swapchainCount = 1,
    .pSwapchains = &*swapChainKHR,
    .pImageIndices = &imageIndex
};
```

Reset or continue ID counter across recreate as preferred; if continuing, still ensure strictly increasing per new swapchain object rules — **per swapchain handle**, so recreate may restart at 1.

---

## 7. Validation / test checklist

1. Extension listed; feature true only when surface supports and app intends use.
2. Create without `ePresentId2` if surface capability false.
3. Non-zero IDs strictly increase per swapchain; zero allowed for “no id”.
4. Present with `PresentId2KHR` validates when feature + create flag set.
5. No present IDs if feature left false (current state) — avoid chaining structs illegally.
6. Integration with present_timing: IDs match past timing records.

---

## 8. Shader interaction

**None (direct).** This extension is host / queue / memory / presentation / resource plumbing only. It does not add shader stages, builtins, storage classes, or SPIR-V capabilities. Shaders continue to use whatever programmable stages the active pipeline or shader objects declare; this feature only changes how the host prepares or submits work those shaders run on.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| `VK_KHR_swapchain` | Base present |
| `VK_KHR_get_surface_capabilities2` | Capability query |
| `VK_EXT_present_timing` | Requires present_id2; primary consumer |
| `VK_KHR_swapchain_maintenance1` present fence | Peer pNext on same `PresentInfoKHR` |

---

## 10. Non-goals / do not do

- Do not chain `PresentId2KHR` without feature + create flag + surface support.
- Do not reuse non-zero IDs or go backwards.
- Do not confuse with old `VK_KHR_present_id` type names — this repo targets **present_id2**.
- Do not enable feature “for free” on surfaces that report unsupported without a soft fallback path.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is present_id2 listed? | **Yes** — as EXT_present_timing dependency |
| Is the feature enabled? | **No** — not in feature chain |
| Is it used? | **No** |
| Highest-value next steps | Enable feature + surface query + create flag + monotonic IDs when bringing up present_timing |
