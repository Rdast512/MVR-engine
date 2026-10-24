# VK_KHR_swapchain — Implementation Spec

**Status in this repo: ENABLED (required device extension), FULLY CONSUMED by swapchain + present path.**

Official sources:

- [VK_KHR_swapchain](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_swapchain.html)
- [VkSwapchainCreateInfoKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainCreateInfoKHR.html)
- [vkQueuePresentKHR](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html)
- [vkAcquireNextImageKHR](https://docs.vulkan.org/refpages/latest/refpages/source/vkAcquireNextImageKHR.html)

> Note: There is **no** feature struct for core `VK_KHR_swapchain`. Surface/instance WSI + device extension enablement is sufficient.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::KHRSwapchainExtensionName` present (first KHR entry) |
| Feature enable | `createLogicalDevice()` | **N/A** — no feature struct |
| Instance / surface | `src/core/vk_device.cpp` | SDL surface extensions + `KHRGetSurfaceCapabilities2ExtensionName`, `KHRSurfaceMaintenance1ExtensionName`, etc. |
| Swapchain create | `src/core/vk_swapchain.cpp` | Full create, image views, recreate |
| Present / acquire | `src/render/vk_renderer.cpp` → `drawFrame()` | `acquireNextImage` + `presentKHR` |
| Sync objects | `src/core/vk_resource_manager.cpp` | `presentCompleteSemaphore`, `renderFinishedSemaphore`, `inFlightFences` |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::KHRSwapchainExtensionName,

// src/core/vk_swapchain.cpp — createSwapChain()
vk::SwapchainCreateInfoKHR swapChainCreateInfo{ /* surface, format, extent, presentMode, ... */ };
swapChain = vk::raii::SwapchainKHR(vkDevice, swapChainCreateInfo);
swapChainImages = swapChain.getImages();
// + createImageViews(), recreateSwapChain()

// src/render/vk_renderer.cpp — drawFrame()
auto acquired = swapChainKHR.acquireNextImage(UINT64_MAX, presentSemaphore, nullptr);
// ... submit ...
presentQueue.presentKHR(presentInfoKHR);
```

### Not wired yet (extensions layered on top, not base swapchain gaps)

- Present fence from `VK_KHR_swapchain_maintenance1` (TODO in `vk_swapchain.cpp`)
- Present IDs (`VK_KHR_present_id2`)
- Present timing (`VK_EXT_present_timing`)
- Per-present mode switch / scaling create info (maintenance1)

Base swapchain lifecycle is complete for the current engine.

---

## 2. What the extension provides

Without `VK_KHR_swapchain` there is no `VkSwapchainKHR`, acquire, or present — this is the required WSI device extension, not a Vulkan 1.x promotion. Surface formats, capabilities, and present modes come from instance-side `KHR_surface` (this repo queries via `getSurfaceCapabilities2KHR`).

It does **not** guarantee MAILBOX, IMMEDIATE, or FIFO_RELAXED, a specific image count, or an extent other than what the surface reports. FIFO is the only present mode that is always available. Core `maxFramebufferWidth` / `maxFramebufferHeight` still apply to swapchain extent in practice.

| Feature | API | Engine use |
| --- | --- | --- |
| Swapchain object | `vkCreateSwapchainKHR` / destroy | `SwapChain::createSwapChain` |
| Images | `vkGetSwapchainImagesKHR` | Color targets for present |
| Acquire | `vkAcquireNextImageKHR` | Frame start in `drawFrame` |
| Present | `vkQueuePresentKHR` | Frame end on present queue |
| Surface queries | formats, capabilities, present modes | `chooseSwapSurfaceFormat`, `chooseSwapPresentMode`, `chooseSwapExtent` |
| Out-of-date handling | `VK_ERROR_OUT_OF_DATE_KHR` / `VK_SUBOPTIMAL_KHR` | `recreateSwapChain` + resource rebuild |

---

## 3. Structures and commands

### 3.1 Create info (core)

```cpp
typedef struct VkSwapchainCreateInfoKHR {
    VkStructureType               sType;
    const void*                   pNext;
    VkSwapchainCreateFlagsKHR     flags;
    VkSurfaceKHR                  surface;
    uint32_t                      minImageCount;
    VkFormat                      imageFormat;
    VkColorSpaceKHR               imageColorSpace;
    VkExtent2D                    imageExtent;
    uint32_t                      imageArrayLayers;
    VkImageUsageFlags             imageUsage;
    VkSharingMode                 imageSharingMode;
    uint32_t                      queueFamilyIndexCount;
    const uint32_t*               pQueueFamilyIndices;
    VkSurfaceTransformFlagBitsKHR preTransform;
    VkCompositeAlphaFlagBitsKHR   compositeAlpha;
    VkPresentModeKHR              presentMode;
    VkBool32                      clipped;
    VkSwapchainKHR                oldSwapchain;
} VkSwapchainCreateInfoKHR;
```

This repo already fills these fields. pNext currently carries `VkSwapchainPresentModesCreateInfoKHR` (from swapchain maintenance1).

### 3.2 Present info

```cpp
typedef struct VkPresentInfoKHR {
    VkStructureType          sType;
    const void*              pNext;
    uint32_t                 waitSemaphoreCount;
    const VkSemaphore*       pWaitSemaphores;
    uint32_t                 swapchainCount;
    const VkSwapchainKHR*    pSwapchains;
    const uint32_t*          pImageIndices;
    VkResult*                pResults;
} VkPresentInfoKHR;
```

Current use: one wait semaphore (render finished per image), one swapchain, one index. Future pNext: present fence, present id2, present timings.

### 3.3 Commands (Vulkan-Hpp)

```cpp
// Acquire
auto acquired = swapChain.acquireNextImage(timeout, semaphore, fence);

// Present
presentQueue.presentKHR(presentInfo);

// Images
auto images = swapChain.getImages();
```

## Limits

No feature or property struct. Surface limits come from `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` (this repo: `getSurfaceCapabilities2KHR` → `.surfaceCapabilities`). Official required-limits chapter: [limits](https://docs.vulkan.org/spec/latest/chapters/limits.html).

| Item | Source | Notes |
| --- | --- | --- |
| `minImageCount` / `maxImageCount` | surface capabilities | `maxImageCount == 0` means unlimited |
| `currentExtent`, `minImageExtent`, `maxImageExtent` | same | Clamp window size into min/max |
| `maxImageArrayLayers` | same | Create-info `imageArrayLayers` |
| `supportedTransforms` | same | `preTransform` must be supported |
| `supportedCompositeAlpha` | same | `compositeAlpha` must be supported |
| `supportedUsageFlags` | same | `imageUsage` must be a subset |
| Present modes | `vkGetPhysicalDeviceSurfacePresentModesKHR` | **FIFO always**. MAILBOX / IMMEDIATE / FIFO_RELAXED optional |
| `maxFramebufferWidth` / `maxFramebufferHeight` | core `VkPhysicalDeviceLimits` | Still apply to swapchain extent in practice |
| Engine image count | `vk_swapchain.cpp` | `max(3, minImageCount)`, then ≤ `maxImageCount` if `max ≠ 0` |

Do not invent other numeric minima.

---

## 4. Where to implement in this codebase

### 4.1 Core path — DONE

| File | Role |
| --- | --- |
| `src/core/vk_device.hpp` / `.cpp` | Extension enable, surface, queues (`presentQueue`) |
| `src/core/vk_swapchain.cpp` / `.hpp` | Create, views, recreate, format/mode/extent selection |
| `src/render/vk_renderer.cpp` | Acquire → record → submit → present; out-of-date recreate |
| `src/core/vk_resource_manager.cpp` | MSAA color/depth sized to swapchain; frame sync objects |

### 4.2 Current present-mode policy

```cpp
// vk_swapchain.cpp
// Prefers FIFO_LATEST_READY (from VK_KHR_present_mode_fifo_latest_ready), else FIFO.
```

FIFO is always available on KHR swapchain; mailbox/immediate are not selected today.

### 4.3 Recreate path

```cpp
// Out-of-date on acquire or present:
swapChain.recreateSwapChain();
rebuildSwapchainResources(); // color/depth MSAA targets
```

`recreateSwapChain` waits idle, destroys views/swapchain, recreates. Image count may change — resource manager must track that (already does via `setSwapChainImageCount`).

### 4.4 Future extensions (do not re-implement base swapchain)

See sibling specs:

- `VK_KHR_swapchain_maintenance1.md` — present fence, scaling, release images
- `VK_KHR_present_mode_fifo_latest_ready.md` — present mode choice (partially done)
- `VK_KHR_present_id2.md` / `VK_EXT_present_timing.md` — frame pacing IDs and timing

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep base swapchain as-is | `vk_swapchain.*`, `vk_renderer.cpp` | Already correct and required |
| P1 | Present fence via maintenance1 | present path | TODO already noted in `vk_swapchain.cpp` |
| P2 | Present id2 + timing stack | present path | Frame pacing / metrics |
| P3 | Optional mailbox / user present-mode setting | `chooseSwapPresentMode` | Only if product needs uncapped FPS modes |

### Implementation recipe

1. Instance: SDL surface + `KHR_surface`; also `KHR_get_surface_capabilities2` and `KHR_surface_maintenance1` (`vk_device.cpp`).
2. Device: keep `vk::KHRSwapchainExtensionName` required. No feature struct.
3. `SwapChain::createSwapChain` (`src/core/vk_swapchain.cpp`): capabilities2; `minImageCount = max(3, surface.minImageCount)` then clamp to `maxImageCount` if `max ≠ 0`; `chooseSwapPresentMode` prefers `eFifoLatestReady` else `eFifo`.
4. Recreate: `recreateSwapChain` — `waitIdle`, destroy views/swapchain, create + views. Image count may change (`setSwapChainImageCount`).
5. Frame: `src/render/vk_renderer.cpp` `drawFrame` — wait fence, `acquireNextImage`, submit2, `presentKHR`. Out-of-date / suboptimal → recreate + `rebuildSwapchainResources`.
6. Layer siblings on create/present `pNext`. present_id2 + calibrated_timestamps are present_timing deps only. present_timing features are in the device chain but all **false**. global_priority: query enabled, not used.

---

## 6. Concrete integration sketch (current baseline)

```cpp
// src/render/vk_renderer.cpp — structural shape already present
deviceRef.waitForFences(fence, vk::True, UINT64_MAX);
auto [result, imageIndex] = swapChainKHR.acquireNextImage(
    UINT64_MAX, presentSemaphore, nullptr);

if (result == vk::Result::eErrorOutOfDateKHR) {
    swapChain.recreateSwapChain();
    rebuildSwapchainResources();
    return;
}

deviceRef.resetFences(fence);
// record + submit2 with wait=presentSemaphore, signal=renderFinished[imageIndex]

vk::PresentInfoKHR presentInfo{
    .waitSemaphoreCount = 1,
    .pWaitSemaphores = &*renderSemaphore,
    .swapchainCount = 1,
    .pSwapchains = &*swapChainKHR,
    .pImageIndices = &imageIndex
};
presentQueue.presentKHR(presentInfo);
```

Any new WSI extension should **chain into** `PresentInfoKHR::pNext` or `SwapchainCreateInfoKHR::pNext` rather than replacing this skeleton.

---

## 7. Validation / test checklist

1. Device creates with `VK_KHR_swapchain` required.
2. Surface formats include preferred `B8G8R8A8Srgb` + `SrgbNonlinear` (fallback to first format works).
3. Present mode falls back to FIFO if latest-ready unavailable.
4. Resize: out-of-date acquire/present recreates swapchain and MSAA targets without crash.
5. Validation: no present-after-destroy; wait semaphores valid; image index in range.
6. Multi-queue: concurrent sharing mode when graphics ≠ present family indices.
7. Min image count ≥ 3 clamped by surface max.

---

## 8. Shader interaction

**None (direct).** This extension is host / queue / memory / presentation / resource plumbing only. It does not add shader stages, builtins, storage classes, or SPIR-V capabilities. Shaders continue to use whatever programmable stages the active pipeline or shader objects declare; this feature only changes how the host prepares or submits work those shaders run on.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Instance surface extensions (SDL + KHR surface) | Required to create `VkSurfaceKHR` |
| `VK_KHR_get_surface_capabilities2` | Used with maintenance1 present-mode compatibility |
| `VK_KHR_surface_maintenance1` | Instance-side companion for swapchain maintenance1 |
| Present queue family | `Device::presentQueue` / `presentIndex` |
| MSAA resolve into swapchain | `vk_renderer.cpp` dynamic rendering resolve |
| Sibling WSI extensions | Layer on create/present pNext chains |

---

## 10. Non-goals / do not do

- Do not reimplement swapchain creation in the renderer — keep ownership in `SwapChain`.
- Do not switch to exclusive full-screen APIs unless a dedicated platform path is designed.
- Do not drop FIFO fallback — it is the portable baseline.
- Do not ignore `eSuboptimalKHR` forever; treat like out-of-date for recreate.
- Do not invent a feature bit for core swapchain.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is KHR_swapchain enabled? | **Yes** — required device extension |
| Is it used? | **Yes** — full create/acquire/present/recreate path |
| Highest-value next steps | Layer maintenance1 present fence; then present_id2 / present_timing |
| Secondary steps | Optional present-mode UX; scaling on resize |
