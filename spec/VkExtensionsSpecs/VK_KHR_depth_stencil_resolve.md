# VK_KHR_depth_stencil_resolve — Implementation Spec

**Status in this repo: PROMOTED TO CORE (Vulkan 1.2), ALWAYS AVAILABLE at 1.2+; properties cached as `depthStencilResolve`. Still valid for MSAA depth/stencil resolve.**

Official sources:

- [VK_KHR_depth_stencil_resolve](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_depth_stencil_resolve.html) (historical extension name)
- [VkPhysicalDeviceDepthStencilResolveProperties](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDepthStencilResolveProperties.html)
- [VkSubpassDescriptionDepthStencilResolve](https://docs.vulkan.org/refpages/latest/refpages/source/VkSubpassDescriptionDepthStencilResolve.html) (render-pass path)
- Dynamic rendering: `VkRenderingAttachmentInfo::resolveMode` / `resolveImageView` for depth

> **PROMOTED TO CORE — do not list as a device extension.** At Vulkan 1.2+, depth/stencil resolve support is core; query properties only. This project targets Vulkan 1.4; do **not** put `VK_KHR_depth_stencil_resolve` in `requiredDeviceExtension`.

> **No feature bit to flip** for the promotion itself — unlike BDA or drawIndirectCount. Behavior is gated by **properties** (`supportedDepthResolveModes`, `supportedStencilResolveModes`, independent resolve support).

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` | **Must not list** — promoted (1.2 comment block) |
| Feature enable | — | **N/A** (always available 1.2+; mode support via properties) |
| Property query | `src/core/vk_device.cpp` → properties chain | `vk::PhysicalDeviceDepthStencilResolveProperties` queried |
| Capability cache | `src/core/types.hpp` → `HardwareCapabilities::depthStencilResolve` | Cached |
| Color MSAA resolve | `src/render/vk_renderer.cpp` | Color attachment uses `resolveMode = eAverage` |
| Depth MSAA resolve | same renderer path | Depth attachment **has no resolve** today (`resolveMode` default / no resolve view) |

### Already wired

```cpp
// src/core/vk_device.cpp
capabilities.depthStencilResolve =
    propertiesChain.get<vk::PhysicalDeviceDepthStencilResolveProperties>();

// src/core/types.hpp
vk::PhysicalDeviceDepthStencilResolveProperties depthStencilResolve;
```

### Current rendering attachments (partial consumption)

```cpp
// src/render/vk_renderer.cpp — color resolves to swapchain; depth does not resolve
vk::RenderingAttachmentInfo colorAttachmentInfo = {
    .resolveMode = vk::ResolveModeFlagBits::eAverage,
    .resolveImageView = swapChain.swapChainImageViews[imageIndex],
    // ...
};
vk::RenderingAttachmentInfo depthAttachmentInfo = {
    .imageView = resourceManager.depthImageView,
    .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
    .loadOp = vk::AttachmentLoadOp::eClear,
    .storeOp = vk::AttachmentStoreOp::eDontCare,
    // no resolveMode / resolveImageView for depth
};
```

---

## 2. What core provides

MSAA **depth and stencil** resolve into single-sample attachments, with implementation-defined supported modes (sample-zero, average, min, max) and optional independent depth vs stencil modes.

Vulkan 1.0/1.1 could resolve MSAA **color** inside a render pass, but depth/stencil resolve was not a portable core path. This extension is **promoted to Vulkan 1.2 core** with **no feature bit** — support is **property-driven**. This repo already caches `HardwareCapabilities::depthStencilResolve`.

This does **not** make `AVERAGE` required (or even legal) for depth. `SAMPLE_ZERO` is the portable mode. Independent depth vs stencil resolve is **optional**. Color resolve (`eAverage` in `vk_renderer.cpp`) is a separate attachment and is not a substitute for depth resolve.


| Capability | Mechanism | Engine relevance |
| --- | --- | --- |
| Supported depth resolve modes | `supportedDepthResolveModes` | Which modes are legal for depth |
| Supported stencil resolve modes | `supportedStencilResolveModes` | Which modes are legal for stencil |
| Independent resolve | `independentResolve` / `independentResolveNone` | Depth mode ≠ stencil mode |
| Dynamic rendering | `RenderingAttachmentInfo` resolve fields on depth attachment | Current engine path |
| Render passes | `VkSubpassDescriptionDepthStencilResolve` | Legacy RP path (engine uses dynamic rendering) |

**Still valid** for any MSAA depth prepass, deferred edges, or effects that need a resolved single-sample depth. Color resolve alone is not a substitute.

Related later extensions (`VK_KHR_maintenance10` resolve transfer-function control, standalone `ResolveImageModeInfoKHR`) build on resolve concepts but do **not** obsolete depth/stencil resolve properties.

---

## 3. Structures and commands

### 3.1 Properties (query — already done)

```cpp
typedef struct VkPhysicalDeviceDepthStencilResolveProperties {
    VkStructureType       sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES
    void*                 pNext;
    VkResolveModeFlags    supportedDepthResolveModes;
    VkResolveModeFlags    supportedStencilResolveModes;
    VkBool32              independentResolveNone;
    VkBool32              independentResolve;
} VkPhysicalDeviceDepthStencilResolveProperties;
```

| Field | Meaning |
| --- | --- |
| `supportedDepthResolveModes` | Bitmask of allowed depth resolve modes |
| `supportedStencilResolveModes` | Bitmask of allowed stencil resolve modes |
| `independentResolveNone` | Can resolve depth XOR stencil with the other NONE |
| `independentResolve` | Can use different non-NONE modes for depth vs stencil |

**Already cached** as `HardwareCapabilities::depthStencilResolve`.

### 3.2 Dynamic rendering depth resolve

```cpp
vk::RenderingAttachmentInfo depthAttachmentInfo = {
    .imageView = msaaDepthView,
    .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
    .resolveMode = vk::ResolveModeFlagBits::eSampleZero, // pick from supportedDepthResolveModes
    .resolveImageView = singleSampleDepthView,
    .resolveImageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
    .loadOp = vk::AttachmentLoadOp::eClear,
    .storeOp = vk::AttachmentStoreOp::eDontCare, // MSAA depth often discarded after resolve
    .clearValue = clearDepth
};
```

Common mode choice for depth: `SAMPLE_ZERO` or `MIN`/`MAX` depending on algorithm; **average** is often unsupported or inappropriate for depth.

### 3.3 No dedicated “enable” feature struct

Unlike 1.2 optional features, this promotion is property-driven. Do not invent a `depthStencilResolve = true` feature enable in `Vulkan12Features` — that bit does not exist as an app toggle in the same way.

---

## Limits

Properties are boolean/bitmask fields, **not** numeric Required Limits rows. Query `HardwareCapabilities::depthStencilResolve`.

| Limit / property | Spec-required (when feature supported) | Constraint | Engine implication |
| --- | --- | --- | --- |
| `supportedDepthResolveModes` | implementation-dependent bitmask | **`AVERAGE` is not required for depth** | Prefer `SAMPLE_ZERO`; never assume `eAverage` |
| `supportedStencilResolveModes` | implementation-dependent bitmask | `SAMPLE_ZERO` is the portable mode | Same selection rule for stencil |
| `independentResolveNone` | optional (`VkBool32`) | may be false | Only resolve depth XOR stencil with the other NONE if true |
| `independentResolve` | optional (`VkBool32`) | may be false | Differing non-NONE modes only if true |
| Numeric minima | **none** | — | No Required Limits row for these properties |

Sources: [limits](https://docs.vulkan.org/spec/latest/chapters/limits.html), [Required Limits](https://docs.vulkan.org/refpages/latest/refpages/source/Required_Limits.html).

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.cpp` / `types.hpp` | Keep property query + cache |
| `src/core/vk_device.hpp` | Keep extension string **out** |

### 4.2 MSAA depth resolve — when needed

**Files:** `src/render/vk_renderer.cpp`, `src/core/vk_resource_manager.cpp` (single-sample depth image if not present)

Today depth is MSAA attachment with `storeOp = eDontCare` and **no** resolve — fine for forward shading that only needs MSAA depth test during the pass.

Add resolve when a later pass needs single-sample depth (SSR, soft particles, history, editor pick, etc.):

1. Create single-sample depth image matching extent/format.
2. Read `capabilities.depthStencilResolve.supportedDepthResolveModes`.
3. Pick a supported mode (prefer `eSampleZero` if available for general Z).
4. Set depth attachment resolve fields in `beginRendering`.

### 4.3 Interaction with maintenance10

sRGB transfer-function resolve flags apply to **color** resolves. Depth/stencil mode selection remains this properties surface. Standalone `resolveImage2` + `ResolveImageModeInfoKHR` can carry depth/stencil modes outside render passes — see `VK_KHR_maintenance10.md`.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep properties cached; never list extension | `vk_device.*`, `types.hpp` | Always-on 1.2+ baseline |
| P1 | Log supported depth/stencil resolve modes at init | `vk_device.cpp` | Debuggability |
| P2 | Add single-sample depth + resolve when a consumer exists | renderer + resource manager | Actual MSAA depth resolve |
| P3 | Gate mode choice on properties | renderer | Portability |

### Implementation recipe

1. **Enable** — no feature bit and no extension string. Keep `VK_KHR_depth_stencil_resolve` out of `requiredDeviceExtension` (`vk_device.hpp` 1.2 promoted block).
2. **Query** — keep `vk::PhysicalDeviceDepthStencilResolveProperties` on the properties chain in `vk_device.cpp`.
3. **Cache** — keep `capabilities.depthStencilResolve` (`types.hpp`). Log `supportedDepthResolveModes` / `supportedStencilResolveModes` / independent bits at init.
4. **Gate** — if a later pass needs single-sample depth, pick a mode from the cached mask (`eSampleZero` first; never `eAverage` unless the bit is set). Honor `independentResolve` / `independentResolveNone` before mixing depth vs stencil modes.
5. **Record** — today `vk_renderer.cpp` sets color `resolveMode = eAverage` and leaves depth without resolve. When a consumer exists, create a 1x depth image in `vk_resource_manager.cpp` and fill `RenderingAttachmentInfo` resolve fields at `beginRendering`.

---

## 6. Integration sketch (depth resolve)

```cpp
const auto& dsr = device.getCapabilities().depthStencilResolve;
const auto depthMode = (dsr.supportedDepthResolveModes & vk::ResolveModeFlagBits::eSampleZero)
    ? vk::ResolveModeFlagBits::eSampleZero
    : /* first supported bit */ vk::ResolveModeFlagBits::eNone;

vk::RenderingAttachmentInfo depthAttachmentInfo = {
    .imageView = resourceManager.depthImageView, // MSAA
    .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
    .resolveMode = depthMode,
    .resolveImageView = resourceManager.depthResolveImageView, // 1x
    .resolveImageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
    .loadOp = vk::AttachmentLoadOp::eClear,
    .storeOp = vk::AttachmentStoreOp::eDontCare,
    .clearValue = clearDepth
};
```

If no post-pass needs resolved depth, **leave as today** (no resolve) — not every MSAA depth attachment must resolve.

---

## 7. Validation / test checklist

1. Properties query succeeds; modes masks non-empty on real GPUs for at least sample-zero or min/max.
2. No `VK_KHR_depth_stencil_resolve` in device extension list.
3. Color MSAA resolve still works (`eAverage` on color).
4. If depth resolve enabled: mode ∈ supported mask; resolve image is 1x samples; formats compatible.
5. Independent depth/stencil: only set differing modes if `independentResolve` allows.

---

## 8. Shader interaction

**Indirect.** Multisample depth/stencil resolve is fixed-function. FS may run at sample/pixel rate depending on sample shading; resolve mode does not add builtins.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Vulkan 1.2+ core | Always available; properties core |
| Vulkan 1.3 dynamic rendering | Engine’s resolve attachment path |
| MSAA color resolve | Already used; depth optional |
| `VK_KHR_maintenance10` | Extra resolve controls; not a replacement |
| Depth format selection | Resolve image must match compatible format |

---

## 10. Non-goals / do not do

- Do **not** list as a device extension on 1.2+.
- Do not assume `eAverage` is valid for depth — check properties.
- Do not force depth resolve when the resolved image is unused (wasted bandwidth).
- Do not confuse color resolve transfer-function flags with depth resolve modes.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Promotion | **Vulkan 1.2 core** (always available at 1.2+) |
| Enable path | Properties only — no app feature bit to toggle |
| Consumed? | Properties cached; **color** resolve used; **depth** resolve not yet |
| Still modern? | **Yes** — MSAA depth/stencil resolve |
| Device extension string? | **Never** on this 1.4 target |
