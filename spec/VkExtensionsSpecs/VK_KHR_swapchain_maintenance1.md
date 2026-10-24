# VK_KHR_swapchain_maintenance1 — Implementation Spec

**Status in this repo: ENABLED (device extension + feature), PARTIALLY CONSUMED (present-mode create info + surface compatibility query; present fence / scaling / release / deferred alloc not yet).**

Official sources:

- [VK_KHR_swapchain_maintenance1](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_swapchain_maintenance1.html)
- [Proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_swapchain_maintenance1.html)
- [VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR.html)
- [VkSwapchainPresentFenceInfoKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainPresentFenceInfoKHR.html)
- [VkSwapchainPresentModesCreateInfoKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainPresentModesCreateInfoKHR.html)

**Dependencies:** `VK_KHR_swapchain` + `VK_KHR_surface_maintenance1` (+ properties2 / Vulkan 1.1). Instance already lists `KHRSurfaceMaintenance1ExtensionName` and `KHRGetSurfaceCapabilities2ExtensionName` in `vk_device.cpp`.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` | `vk::KHRSwapchainMaintenance1ExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` | `{.swapchainMaintenance1 = true}` |
| Instance companions | `src/core/vk_device.cpp` | `KHRSurfaceMaintenance1`, `KHRGetSurfaceCapabilities2` enabled |
| Present modes on create | `src/core/vk_swapchain.cpp` | `SwapchainPresentModesCreateInfoKHR` on create pNext |
| Compatible modes query | `src/core/vk_swapchain.cpp` | Two-step `getSurfaceCapabilities2KHR` + `SurfacePresentModeCompatibilityKHR` |
| Present fence | present path | **Not wired** — TODO: `// TODO use swapchain maintenance1 special fence so i can remove other present fences` |
| Scaling create info | swapchain create | **Not wired** |
| Release swapchain images | — | **Not wired** |
| Deferred memory allocation | create flags | **Not wired** |
| Per-present mode change | `PresentInfo` pNext | **Not wired** |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::KHRSwapchainMaintenance1ExtensionName,

// src/core/vk_device.cpp
// vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR
{.swapchainMaintenance1 = true},

// src/core/vk_swapchain.cpp
vk::SurfacePresentModeKHR surfacePresentModeKHR{.presentMode = chosenPresentMode};
// getSurfaceCapabilities2KHR<SurfaceCapabilities2KHR, SurfacePresentModeCompatibilityKHR>
// fill compatibleModes, then:
vk::SwapchainPresentModesCreateInfoKHR modesInfo{
    .presentModeCount = static_cast<uint32_t>(compatibleModes.size()),
    .pPresentModes = compatibleModes.data()
};
// swapChainCreateInfo.pNext = &modesInfo;
```

### Not wired yet

- `VkSwapchainPresentFenceInfoKHR` on `vkQueuePresentKHR`
- `VkSwapchainPresentScalingCreateInfoKHR` on create
- `vkReleaseSwapchainImagesKHR` for discarded acquired images
- `VK_SWAPCHAIN_CREATE_DEFERRED_MEMORY_ALLOCATION_BIT_KHR`
- `VkSwapchainPresentModeInfoKHR` for per-present mode switches

---

## 2. What the extension provides

KHR_swapchain leaves present-semaphore lifetime, resize-without-recreate, and multi-mode create underspecified. Maintenance1 adds present fences, scaling, present-modes create info, per-present mode switch, deferred alloc, and release-without-present. Device extension + `swapchainMaintenance1` — not a core promotion.

Enabling the feature does **not** add a present fence, change image count, or guarantee scaling / mode compatibility on every surface. Those require extra create/present structs and surface_capabilities2 queries.

| Feature | New API / bit | Engine relevance |
| --- | --- | --- |
| Present fence | `VkSwapchainPresentFenceInfoKHR` | Destroy wait semaphores / swapchain when present resources are done; simplifies sync |
| Multi present-mode create | `VkSwapchainPresentModesCreateInfoKHR` | Allow mode changes without full recreate (**already used**) |
| Per-present mode switch | `VkSwapchainPresentModeInfoKHR` | Switch FIFO ↔ latest-ready without recreate |
| Present scaling | `VkSwapchainPresentScalingCreateInfoKHR` | Resize without `OUT_OF_DATE` when dimensions differ |
| Deferred memory alloc | `SWAPCHAIN_CREATE_DEFERRED_MEMORY_ALLOCATION_BIT_KHR` | Faster startup / lower idle footprint |
| Release acquired images | `vkReleaseSwapchainImagesKHR` | Drop acquired images without presenting (e.g. after skip frame) |

---

## 3. Structures and commands

### 3.1 Feature

```cpp
// Provided by VK_KHR_swapchain_maintenance1
typedef struct VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR {
    VkStructureType sType;
    void*           pNext;
    VkBool32        swapchainMaintenance1;
} VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR;
```

**Already enabled.**

### 3.2 Present modes create info (already used)

```cpp
typedef struct VkSwapchainPresentModesCreateInfoKHR {
    VkStructureType    sType;
    const void*        pNext;
    uint32_t           presentModeCount;
    const VkPresentModeKHR* pPresentModes;
} VkSwapchainPresentModesCreateInfoKHR;
```

**Validation rule (already documented in code):** `pPresentModes` must be a **subset** of modes returned by `VkSurfacePresentModeCompatibilityKHR` for the **chosen** present mode — not the full surface present-mode list.

### 3.3 Present fence info (primary missing piece)

```cpp
// Provided by VK_KHR_swapchain_maintenance1
typedef struct VkSwapchainPresentFenceInfoKHR {
    VkStructureType sType; // VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR
    const void*     pNext;
    uint32_t        swapchainCount;
    const VkFence*  pFences; // one per swapchain; may be VK_NULL_HANDLE
} VkSwapchainPresentFenceInfoKHR;
```

Semantics:

- Signaled when presentation-related queue ops and PE reference-taking for that present are done (not necessarily when the image is shown).
- Application may destroy **wait semaphores** used in that present after this fence signals.
- Application may destroy the **swapchain** after all past present fences for it have signaled.
- Fences for the same swapchain on the same queue signal in present order.

### 3.4 Scaling create info

```cpp
typedef struct VkSwapchainPresentScalingCreateInfoKHR {
    VkStructureType           sType;
    const void*               pNext;
    VkPresentScalingFlagsKHR  scalingBehavior;
    VkPresentGravityFlagsKHR  presentGravityX;
    VkPresentGravityFlagsKHR  presentGravityY;
} VkSwapchainPresentScalingCreateInfoKHR;
```

Chain on `VkSwapchainCreateInfoKHR`. Query supported scaling via surface capabilities2 companion structs from surface maintenance1.

### 3.5 Per-present mode info

```cpp
typedef struct VkSwapchainPresentModeInfoKHR {
    VkStructureType       sType;
    const void*           pNext;
    uint32_t              swapchainCount;
    const VkPresentModeKHR* pPresentModes;
} VkSwapchainPresentModeInfoKHR;
```

Chain on `VkPresentInfoKHR`. Mode must have been listed in the create-time present modes list.

### 3.6 Release images

```cpp
void vkReleaseSwapchainImagesKHR(
    VkDevice device,
    const VkReleaseSwapchainImagesInfoKHR* pReleaseInfo);
```

```cpp
typedef struct VkReleaseSwapchainImagesInfoKHR {
    VkStructureType sType;
    const void*     pNext;
    VkSwapchainKHR  swapchain;
    uint32_t        imageIndexCount;
    const uint32_t* pImageIndices;
} VkReleaseSwapchainImagesInfoKHR;
```

### 3.7 Deferred allocation flag

`VK_SWAPCHAIN_CREATE_DEFERRED_MEMORY_ALLOCATION_BIT_KHR` on `VkSwapchainCreateInfoKHR::flags` — image memory may allocate on first acquire.

## Limits

Feature: `swapchainMaintenance1` (already `true`).

No dedicated numeric Required Limits table. Limits come from surface capabilities2 plus scaling / present-mode compatibility queries, not a property struct. Official required-limits chapter: [limits](https://docs.vulkan.org/spec/latest/chapters/limits.html).

| Query | What it bounds |
| --- | --- |
| `vkGetPhysicalDeviceSurfaceCapabilities2KHR` | Same surface caps as KHR_swapchain (`minImageCount`, extents, usage, …) |
| `VkSurfacePresentModeCompatibilityKHR` | Compatible present-mode set for the **chosen** mode |
| surface_maintenance1 scaling / present-mode companions | Supported `scalingBehavior` / gravity flags |

Query those; do not invent minima. Engine image count remains `max(3, minImageCount)` clamped to `maxImageCount` if `max ≠ 0`.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

Keep extension + `{.swapchainMaintenance1 = true}` and instance surface maintenance1 / capabilities2.

### 4.2 Present modes create path — DONE

Keep the two-step compatibility query and `SwapchainPresentModesCreateInfoKHR` in `vk_swapchain.cpp`.

### 4.3 Present fence — P0 (matches existing TODO)

**Files:**

- `src/core/vk_swapchain.cpp` (TODO comment line 7)
- `src/render/vk_renderer.cpp` → `drawFrame()` present
- `src/core/vk_resource_manager.cpp` / `.hpp` — fence pool for presents

Sketch:

1. Allocate a ring of present fences (one per swapchain image or per frame-in-flight; must not reuse a fence until it has signaled for its prior present).
2. On present:

```cpp
vk::SwapchainPresentFenceInfoKHR presentFenceInfo{
    .swapchainCount = 1,
    .pFences = &*presentFences[imageIndex]
};

vk::PresentInfoKHR presentInfo{
    .pNext = &presentFenceInfo,
    .waitSemaphoreCount = 1,
    .pWaitSemaphores = &*renderSemaphore,
    .swapchainCount = 1,
    .pSwapchains = &*swapChainKHR,
    .pImageIndices = &imageIndex
};
```

3. Wait/reset that fence before reusing associated present wait semaphore or before destroying the swapchain on recreate.
4. Evaluate whether `inFlightFences` (submit fences) can be simplified once present completion is tracked separately — do **not** remove submit fences until acquire/submit ordering is still safe.

### 4.4 Recreate with present fences

On `recreateSwapChain`:

- Wait for all outstanding present fences for the old swapchain (or `device.waitIdle()` as today).
- Then destroy views/swapchain.

`waitIdle` already works; present fences enable finer-grained teardown later.

### 4.5 Scaling (resize robustness)

**File:** `vk_swapchain.cpp` create path.

If surface maintenance1 reports supported scaling flags, chain `SwapchainPresentScalingCreateInfoKHR` to reduce `OUT_OF_DATE` on window size mismatch. Still handle out-of-date for other causes.

### 4.6 Release images / deferred alloc / per-present mode

- **Release:** if a frame aborts after acquire without present (rare today — early return only on out-of-date acquire before record). Useful if skip-render paths appear.
- **Deferred alloc:** optional startup optimization; ensure first acquire cost is acceptable.
- **Per-present mode:** switch between `eFifo` and `eFifoLatestReady` without recreate when both are in the compatible create list.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep modes create + compatibility query | `vk_swapchain.cpp` | Already correct and required by validation |
| P1 | Present fence on `presentKHR` | `vk_renderer.cpp`, resource manager, swapchain | Explicit TODO; safer semaphore/swapchain lifetime |
| P2 | Wait present fences on recreate | swapchain recreate | Finer than full idle (optional) |
| P2 | Scaling create info if surface supports | `vk_swapchain.cpp` | Fewer resize out-of-date failures |
| P3 | Per-present mode switch | present path | Dynamic VSync-style policy |
| P3 | Release images / deferred alloc | swapchain + renderer | Niche until skip-frame / startup paths need them |

### Implementation recipe

1. Instance companions already on: `KHR_surface`, `KHR_get_surface_capabilities2`, `KHR_surface_maintenance1` (`vk_device.cpp`).
2. Keep device extension + `{.swapchainMaintenance1 = true}`.
3. Keep `src/core/vk_swapchain.cpp` create: present mode prefers `eFifoLatestReady` else `eFifo`; two-step compatibility query; `SwapchainPresentModesCreateInfoKHR` (subset only). Image count `max(3, min)` clamped.
4. Recreate still `waitIdle` then destroy/create (`recreateSwapChain`).
5. Next: `SwapchainPresentFenceInfoKHR` on `presentKHR` in `src/render/vk_renderer.cpp`; wait those fences on recreate.
6. Then scaling create info, then per-present mode / release / deferred alloc as needed.
7. present_id2 + calibrated_timestamps are present_timing deps only. present_timing features are in the chain but all **false**. global_priority: query enabled, not used.

---

## 6. Concrete integration sketch (present fence)

```cpp
// resource_manager: std::vector<vk::raii::Fence> presentFences; // size = swapchain image count

// before reuse:
deviceRef.waitForFences(*presentFences[imageIndex], vk::True, UINT64_MAX);
deviceRef.resetFences(*presentFences[imageIndex]);

vk::SwapchainPresentFenceInfoKHR fenceInfo{
    .swapchainCount = 1,
    .pFences = &*presentFences[imageIndex]
};

vk::PresentInfoKHR presentInfo{
    .pNext = &fenceInfo,
    .waitSemaphoreCount = 1,
    .pWaitSemaphores = &*renderSemaphore,
    .swapchainCount = 1,
    .pSwapchains = &*swapChainKHR,
    .pImageIndices = &imageIndex
};

presentQueue.presentKHR(presentInfo);
```

Chain additional pNext (present id2, present timings) from `fenceInfo.pNext` or a dedicated chain head when those features are enabled.

---

## 7. Validation / test checklist

1. Device creates with `swapchainMaintenance1 = true`.
2. Swapchain create: present modes list is subset of compatibility query for the chosen mode (already).
3. Present with fence: no validation errors; fence signals in present order.
4. Recreate: no destroy-while-present-in-flight.
5. Semaphore reuse only after corresponding present fence (if relying on that guarantee).
6. Scaling path (if added): window resize stress without continuous out-of-date.
7. Per-present mode (if added): mode is in create-time list; validation clean.

---

## 8. Shader interaction

**None (direct).** This extension is host / queue / memory / presentation / resource plumbing only. It does not add shader stages, builtins, storage classes, or SPIR-V capabilities. Shaders continue to use whatever programmable stages the active pipeline or shader objects declare; this feature only changes how the host prepares or submits work those shaders run on.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| `VK_KHR_swapchain` | Base WSI |
| `VK_KHR_surface_maintenance1` | Instance; scaling + surface queries |
| `VK_KHR_get_surface_capabilities2` | Compatibility / scaling queries |
| `VK_KHR_present_mode_fifo_latest_ready` | Modes included in compatible list |
| `VK_KHR_present_id2` / `VK_EXT_present_timing` | Additional present pNext chain peers |
| Submit `inFlightFences` | Still required for CB/UBO reuse; present fence is not a full replacement |

---

## 10. Non-goals / do not do

- Do not pass the full `getSurfacePresentModesKHR` list into `SwapchainPresentModesCreateInfoKHR` — use compatibility query (already fixed).
- Do not remove submit fences solely because present fences exist.
- Do not enable deferred allocation without measuring first-acquire hitch.
- Do not switch present mode to a mode not listed at create time.
- Do not ignore fence ordering when recycling present fences per image.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is swapchain_maintenance1 enabled? | **Yes** — extension + feature |
| Is it used? | **Partially** — present-mode create info + compatibility query only |
| Highest-value next step | Present fence integration (existing TODO) |
| Secondary steps | Scaling; per-present mode; release/deferred alloc as needed |
