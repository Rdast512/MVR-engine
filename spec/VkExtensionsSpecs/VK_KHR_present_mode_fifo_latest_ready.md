# VK_KHR_present_mode_fifo_latest_ready — Implementation Spec

**Status in this repo: ENABLED (device extension + feature), PARTIALLY CONSUMED (`chooseSwapPresentMode` prefers `eFifoLatestReady`, else FIFO).**

Official sources:

- [VK_KHR_present_mode_fifo_latest_ready](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_present_mode_fifo_latest_ready.html)
- [Proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_present_mode_fifo_latest_ready.html)
- [VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR.html)

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` | `vk::KHRPresentModeFifoLatestReadyExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` | `{.presentModeFifoLatestReady = true}` |
| Present mode selection | `src/core/vk_swapchain.cpp` → `chooseSwapPresentMode` | Prefers `vk::PresentModeKHR::eFifoLatestReady`, falls back to `eFifo` |
| Swapchain create | `createSwapChain` | Uses chosen mode; also lists compatible modes via maintenance1 |
| Present path | `vk_renderer.cpp` | No mode-specific logic beyond swapchain present mode |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::KHRPresentModeFifoLatestReadyExtensionName,

// src/core/vk_device.cpp
// vk::PhysicalDevicePresentModeFifoLatestReadyFeaturesKHR
{.presentModeFifoLatestReady = true},

// src/core/vk_swapchain.cpp
vk::PresentModeKHR SwapChain::chooseSwapPresentMode(
    const std::vector<vk::PresentModeKHR> &availablePresentModes) {
    for (const auto &availablePresentMode : availablePresentModes) {
        if (availablePresentMode == vk::PresentModeKHR::eFifoLatestReady) {
            return availablePresentMode;
        }
    }
    return vk::PresentModeKHR::eFifo;
}
```

### Not wired yet

- Runtime user toggle between FIFO and FIFO_LATEST_READY (would use maintenance1 per-present mode switch or recreate)
- Explicit interaction with `VK_EXT_present_timing` target present times (extension text notes usefulness with time-based present APIs)
- Logging / telemetry of which mode was chosen beyond existing log in create

---

## 2. What the extension provides

Classic FIFO queues every present until shown (higher latency when the app is faster than refresh). MAILBOX/IMMEDIATE are optional and not the portable vsync path. `FIFO_LATEST_READY` is a tear-free, vblank-aligned mode that dequeues until the latest ready image. Device extension + `presentModeFifoLatestReady` — not a core promotion.

The feature bit does **not** guarantee the mode exists: it must also appear in the surface present-mode list. It is still vsync-like and does **not** uncap like IMMEDIATE.

| Feature | New API / bit | Engine relevance |
| --- | --- | --- |
| New present mode | `VK_PRESENT_MODE_FIFO_LATEST_READY_KHR` | Tear-free; at vblank dequeues until latest ready image (unlike mailbox which can release continuously) |
| Feature bit | `presentModeFifoLatestReady` | Must be enabled to use the mode |

**Behavior vs FIFO:** still tear-free and vblank-aligned, but discards older queued presents when a newer image is ready by vblank — lower latency than pure FIFO when the app produces frames faster than refresh, without mailbox’s continuous release model.

**Behavior vs MAILBOX:** processing of present requests happens during vblank; a single vblank may release multiple swapchain images at once.

---

## 3. Structures and commands

### 3.1 Feature

```cpp
// Provided by VK_KHR_present_mode_fifo_latest_ready
typedef struct VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR {
    VkStructureType sType;
    void*           pNext;
    VkBool32        presentModeFifoLatestReady;
} VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR;
```

**Already enabled.**

### 3.2 Present mode enum

| Enum | Vulkan-Hpp |
| --- | --- |
| `VK_PRESENT_MODE_FIFO_LATEST_READY_KHR` | `vk::PresentModeKHR::eFifoLatestReady` |

No new commands. Mode appears in:

- `vkGetPhysicalDeviceSurfacePresentModesKHR`
- `VkSwapchainCreateInfoKHR::presentMode`
- maintenance1 compatible present-mode lists

## Limits

Feature: `presentModeFifoLatestReady` (already `true`).

No property struct. No numeric Required Limits table. Official required-limits chapter: [limits](https://docs.vulkan.org/spec/latest/chapters/limits.html).

The mode is **optional** even if the extension is listed — it must appear in `vkGetPhysicalDeviceSurfacePresentModesKHR`. FIFO remains the portable fallback (always available via KHR_swapchain). Still vsync-like; do not treat it as IMMEDIATE.

---

## 4. Where to implement in this codebase

### 4.1 Enablement + selection — DONE

Keep feature/extension and `chooseSwapPresentMode` preference order:

1. `eFifoLatestReady` if advertised
2. else `eFifo` (always available)

### 4.2 Interaction with swapchain maintenance1

`createSwapChain` already:

1. Chooses present mode (may be latest-ready).
2. Queries **compatible** modes for that choice via `SurfacePresentModeCompatibilityKHR`.
3. Passes the compatible set in `SwapchainPresentModesCreateInfoKHR`.

If the app later switches modes at present time, only modes in that compatible set are valid — confirm FIFO remains compatible with latest-ready on target drivers if dual-mode switching is desired.

### 4.3 Present timing (future)

When `VK_EXT_present_timing` is fully enabled, FIFO_LATEST_READY pairs well with target present times (extension description: useful with time-based present APIs). No extra host struct for this extension alone.

### 4.4 Optional settings

Expose engine setting:

- Prefer latest-ready (default on)
- Force classic FIFO (minimum latency variance / simpler pacing)

Implementation: branch in `chooseSwapPresentMode` on settings; optional maintenance1 present-mode change without recreate.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep preference + fallback | `vk_swapchain.cpp` | Already correct |
| P1 | Log chosen mode prominently (already partial) | create path | Confirm driver support in the field |
| P2 | Settings toggle FIFO vs latest-ready | settings + chooseSwapPresentMode | User control |
| P3 | Combine with present timing target times | present path | Frame pacing once EXT_present_timing is on |

### Implementation recipe

1. Keep extension + `{.presentModeFifoLatestReady = true}`.
2. Keep `chooseSwapPresentMode` in `src/core/vk_swapchain.cpp`: prefer `eFifoLatestReady`, else `eFifo`. Never require latest-ready.
3. Create/recreate still clamps image count `max(3, minImageCount)` and uses maintenance1 compatible-mode list. Instance: `KHR_surface`, `KHR_get_surface_capabilities2`, `KHR_surface_maintenance1`.
4. Acquire/present stay in `src/render/vk_renderer.cpp` — no mode-specific present logic.
5. Optional later: settings toggle; maintenance1 per-present switch; pair with present_timing **after** those features are enabled (today all **false**).
6. present_id2 + calibrated_timestamps remain present_timing deps. global_priority: query enabled, not used.

---

## 6. Concrete integration sketch (optional setting)

```cpp
vk::PresentModeKHR SwapChain::chooseSwapPresentMode(
    const std::vector<vk::PresentModeKHR>& available) const {
    const bool preferLatestReady = /* engine settings */ true;

    if (preferLatestReady) {
        for (const auto mode : available) {
            if (mode == vk::PresentModeKHR::eFifoLatestReady)
                return mode;
        }
    }
    return vk::PresentModeKHR::eFifo;
}
```

Do not select mailbox/immediate unless product requirements change.

---

## 7. Validation / test checklist

1. Device creates with `presentModeFifoLatestReady = true`.
2. On supporting drivers, log shows `FifoLatestReady` chosen.
3. On non-supporting present-mode lists, clean fallback to `Fifo` (feature still enabled at device create — mode simply absent from surface list).
4. Resize/recreate still works under latest-ready.
5. Visual: no tearing; latency subjectively ≤ FIFO when GPU-bound frame production is bursty.
6. maintenance1 compatibility list includes the chosen mode (and any modes you intend to switch to).

---

## 8. Shader interaction

**None (direct).** This extension is host / queue / memory / presentation / resource plumbing only. It does not add shader stages, builtins, storage classes, or SPIR-V capabilities. Shaders continue to use whatever programmable stages the active pipeline or shader objects declare; this feature only changes how the host prepares or submits work those shaders run on.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| `VK_KHR_swapchain` | Required |
| `VK_KHR_swapchain_maintenance1` | Compatible mode list at create; optional per-present switch |
| `VK_EXT_present_timing` | Complementary for time-based pacing |
| Surface present mode query | Must advertise mode for selection |

---

## 10. Non-goals / do not do

- Do not require latest-ready to be present — always keep FIFO fallback.
- Do not treat latest-ready as mailbox for pacing math without measuring.
- Do not remove the feature enable when the mode is preferred; both extension + feature + surface advertisement are required.
- Do not invent extra enums beyond `eFifoLatestReady`.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is the extension enabled? | **Yes** — extension + feature |
| Is it used? | **Partially** — preferred present mode with FIFO fallback |
| Highest-value next steps | Settings toggle; pair with present timing when enabled |
| Secondary steps | Per-present mode switch via maintenance1 |
