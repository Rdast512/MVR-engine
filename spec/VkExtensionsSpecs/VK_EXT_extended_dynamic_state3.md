# VK_EXT_extended_dynamic_state3 — Implementation Spec

**Status in this repo: ENABLED (device extension listed). Feature chain enables EDS1 `ExtendedDynamicStateFeaturesEXT{ .extendedDynamicState = true }`, not `ExtendedDynamicState3FeaturesEXT`. NOT CONSUMED — no EDS3 `vkCmdSet*EXT`; renderer uses core 1.0 `setViewport`/`setScissor` only.**

Official sources:

- [VK_EXT_extended_dynamic_state3](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_extended_dynamic_state3.html)
- [VkPhysicalDeviceExtendedDynamicState3FeaturesEXT](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.html)
- [VkPhysicalDeviceExtendedDynamicState3PropertiesEXT](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceExtendedDynamicState3PropertiesEXT.html)

Related (do not confuse):

- `VK_EXT_extended_dynamic_state` (EDS1) — **promoted to Vulkan 1.3**; repo still chains `ExtendedDynamicStateFeaturesEXT{ .extendedDynamicState = true }`
- `VK_EXT_extended_dynamic_state2` (EDS2) — promoted to 1.3
- Comment in `vk_device.hpp`: EDS1/EDS2 must **not** be listed as device extensions under 1.3+; EDS3 remains an extension

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::EXTExtendedDynamicState3ExtensionName` present |
| EDS1 feature (related) | `src/core/vk_device.cpp` → feature chain | `vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT{ .extendedDynamicState = true }` |
| EDS3 feature struct | feature chain | **Not** present as `PhysicalDeviceExtendedDynamicState3FeaturesEXT` in current chain |
| EDS3 properties | properties chain / `types.hpp` | **Not** cached |
| API usage | `vk_pipeline.cpp`, `vk_renderer.cpp` | Dynamic viewport + scissor only (`eViewport`, `eScissor`); no `vkCmdSet*EXT` EDS3 calls |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::EXTExtendedDynamicState3ExtensionName,

// src/core/vk_device.cpp (feature chain) — EDS1 feature struct, not EDS3
// vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT
{.extendedDynamicState = true},
```

### Not wired yet

- `VkPhysicalDeviceExtendedDynamicState3FeaturesEXT` bits selectively enabled for needed states
- Optional `VkPhysicalDeviceExtendedDynamicState3PropertiesEXT` (e.g. dynamic depth clamp range support)
- Pipeline `pDynamicState` expansion beyond viewport/scissor
- Recording `vkCmdSetPolygonModeEXT`, `vkCmdSetColorBlendEnableEXT`, rasterization samples, etc.
- Full dynamic-state set required by `VK_EXT_shader_object`

---

## 2. What the extension provides

EDS3 is still an **extension**. This repo lists `VK_EXT_extended_dynamic_state3` but does not chain `PhysicalDeviceExtendedDynamicState3FeaturesEXT`; each `vkCmdSet*EXT` is gated by its own feature bit. The renderer only uses core viewport/scissor inside the dynamic-rendering / Synchronization2 frame (`beginRendering`, MSAA resolve into the swapchain, `endRendering`).

EDS1/2 are already core 1.3 — do not list those extensions. Consume EDS3 only for states that are not core (polygon mode, raster samples, color blend enable/equation, advanced blend, …) and only after enabling the matching bits.

A large third wave of **pipeline state as dynamic state**: tessellation, polygon mode, rasterization samples, depth clamp enable, color blend enables/equations/masks, coverage, advanced blend, sample locations interactions, and many more — each gated by its own feature bit inside `VkPhysicalDeviceExtendedDynamicState3FeaturesEXT`.

| Feature group | Example commands | Engine relevance |
| --- | --- | --- |
| Raster | `vkCmdSetPolygonModeEXT`, `vkCmdSetRasterizationSamplesEXT` | Debug wireframe; MSAA toggles |
| Depth/stencil extras | `vkCmdSetDepthClampEnableEXT`, … | Specialty passes |
| Color blend | `vkCmdSetColorBlendEnableEXT`, `vkCmdSetColorBlendEquationEXT`, `vkCmdSetColorWriteMaskEXT` | Material variety without pipelines |
| Multisample | alpha-to-coverage, sample mask (see maintenance10 NULL mask note) | MSAA policies |
| Advanced blend | `vkCmdSetColorBlendAdvancedEXT` | With blend operation advanced |
| Logic ops / FSR-related extras | various | Only if used |

---

## 3. Structures and commands

### 3.1 EDS3 feature (should be used when consuming EDS3)

```cpp
typedef struct VkPhysicalDeviceExtendedDynamicState3FeaturesEXT {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT
    void*           pNext;
    VkBool32        extendedDynamicState3TessellationDomainOrigin;
    VkBool32        extendedDynamicState3DepthClampEnable;
    VkBool32        extendedDynamicState3PolygonMode;
    VkBool32        extendedDynamicState3RasterizationSamples;
    VkBool32        extendedDynamicState3SampleMask;
    VkBool32        extendedDynamicState3AlphaToCoverageEnable;
    VkBool32        extendedDynamicState3AlphaToOneEnable;
    VkBool32        extendedDynamicState3LogicOpEnable;
    VkBool32        extendedDynamicState3ColorBlendEnable;
    VkBool32        extendedDynamicState3ColorBlendEquation;
    VkBool32        extendedDynamicState3ColorWriteMask;
    VkBool32        extendedDynamicState3RasterizationStream;
    VkBool32        extendedDynamicState3ConservativeRasterizationMode;
    // ... many more bits (line rasterization, depth clip, color write enable,
    // sample locations, provoking vertex, line stipple, depth clip control,
    // color blend advanced, etc.) per full header
} VkPhysicalDeviceExtendedDynamicState3FeaturesEXT;
```

**Project gap:** extension name is listed, but the EDS3 feature structure is not currently filled in the device create chain. Before calling EDS3 commands, add the struct and set only needed bits to `true` (and verify support).

### 3.2 Properties (optional query)

```cpp
typedef struct VkPhysicalDeviceExtendedDynamicState3PropertiesEXT {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_PROPERTIES_EXT
    void*           pNext;
    VkBool32        dynamicPrimitiveTopologyUnrestricted;
    // additional property fields per revision
} VkPhysicalDeviceExtendedDynamicState3PropertiesEXT;
```

Not cached today.

### 3.3 Representative commands

```cpp
void vkCmdSetPolygonModeEXT(VkCommandBuffer commandBuffer, VkPolygonMode polygonMode);
void vkCmdSetRasterizationSamplesEXT(VkCommandBuffer commandBuffer, VkSampleCountFlagBits rasterizationSamples);
void vkCmdSetSampleMaskEXT(VkCommandBuffer commandBuffer, VkSampleCountFlagBits samples, const VkSampleMask* pSampleMask);
void vkCmdSetAlphaToCoverageEnableEXT(VkCommandBuffer commandBuffer, VkBool32 alphaToCoverageEnable);
void vkCmdSetColorBlendEnableEXT(VkCommandBuffer commandBuffer, uint32_t firstAttachment,
                                 uint32_t attachmentCount, const VkBool32* pColorBlendEnables);
void vkCmdSetColorBlendEquationEXT(VkCommandBuffer commandBuffer, uint32_t firstAttachment,
                                   uint32_t attachmentCount, const VkColorBlendEquationEXT* pColorBlendEquations);
void vkCmdSetColorWriteMaskEXT(VkCommandBuffer commandBuffer, uint32_t firstAttachment,
                               uint32_t attachmentCount, const VkColorComponentFlags* pColorWriteMasks);
// ... plus many more Set*EXT entry points
```

### 3.4 Relationship to current feature chain

| Struct in chain today | Meaning |
| --- | --- |
| `PhysicalDeviceExtendedDynamicStateFeaturesEXT` | EDS1 (`extendedDynamicState`) — core 1.3 equivalent largely redundant but currently forced true |
| EDS3 extension name | Listed in `requiredDeviceExtension` |
| `PhysicalDeviceExtendedDynamicState3FeaturesEXT` | **Missing** — add when consuming |

---

## Limits

No numeric Required Limits table for EDS3. Gating is per-feature-bit, not a new count. Do not invent numeric minima.

| Topic | Fact | Engine note |
| --- | --- | --- |
| Required Limits row | None typical | Each `vkCmdSet*EXT` is gated by its feature bit |
| Feature bits | Each command requires its `extendedDynamicState3*` bit | Bits **not fully chained** in this repo |
| Properties | `VkPhysicalDeviceExtendedDynamicState3PropertiesEXT` (e.g. `dynamicPrimitiveTopologyUnrestricted`) | **Not cached** |
| Inherited counts | Viewport/attachment counts still core (`maxViewports`, `maxColorAttachments`) | Current pass: 1 color attachment |

Official: [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html).

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — PARTIAL

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep `vk::EXTExtendedDynamicState3ExtensionName` |
| `src/core/vk_device.cpp` | **Add** EDS3 features (selective) + optional properties when implementing |
| `src/core/types.hpp` | Optional: cache EDS3 properties |

### 4.2 Pipeline dynamic state array

**File:** `src/render/vk_pipeline.cpp`

Today:

```cpp
std::vector dynamicStates = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
```

Extend with EDS3 dynamic state enums for each promoted state (e.g. `ePolygonModeEXT`, `eColorBlendEnableEXT`, …) **and** record matching `cmd.set*EXT` before draws.

### 4.3 Renderer record path

**File:** `src/render/vk_renderer.cpp`

Already sets viewport/scissor dynamically. Add EDS3 setters when those states leave the static pipeline info.

### 4.4 Shader object prerequisite

Full `VK_EXT_shader_object` adoption depends on covering nearly all state dynamically — EDS3 is a major part of that surface (see shader object spec).

### 4.5 Maintenance10 interaction

With `VK_KHR_maintenance10`, `vkCmdSetSampleMaskEXT` with `pSampleMask == NULL` means all bits set. Only relevant once sample-mask dynamic state is used.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep extension listed | `vk_device.hpp` | Already required |
| P1 | When needed: chain EDS3 features with **only** used bits | `vk_device.cpp` | Spec requires feature bits for commands |
| P1 | Expand `dynamicStates` + matching set cmds | `vk_pipeline.cpp`, `vk_renderer.cpp` | Actual consumption |
| P2 | Wireframe debug via dynamic polygon mode | renderer debug | Low-hanging fruit |
| P2 | Dynamic blend enables for materials | pipeline/renderer | Fewer pipelines |
| P3 | Full state set for shader objects | shader object migration | Hard dependency |

### Implementation recipe

1. **Treat listing as enablement, not consumption.** Extension is in `requiredDeviceExtension`. Before any `set*EXT`, add `PhysicalDeviceExtendedDynamicState3FeaturesEXT` and set **only** the bits you will call (query support).
2. **Prefer core EDS1/2 first.** Cull, depth test/write, topology, viewport-with-count are core 1.3. Use those without EDS3.
3. **First real EDS3 consumer (when needed):** dynamic polygon mode (debug wireframe) or color-blend enable/equation. Add the enums to `vk_pipeline.cpp` `dynamicStates` and the matching `set*EXT` in `vk_renderer.cpp` after `beginRendering` / `bindPipeline`.
4. **Stay inside the existing frame.** Dynamic rendering + sync2 are consumed; local-read is enabled (Vulkan 1.4) but unused. Do not invent a second render-pass model for EDS3. MSAA resolve into the swapchain stays as today.
5. **Do not enable advanced-blend EDS3 setters** while `advancedBlendCoherentOperations` is **explicitly false** and no pass uses advanced ops. FSR (`HardwareCapabilities::fragmentShadingRate`) is enabled unused; ASTC HDR is **false** (GPU-gated). Both are unrelated.

---

## 6. Concrete integration sketch

```cpp
// Device create — only enable bits you will call:
vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{
    .extendedDynamicState3PolygonMode = true,
    .extendedDynamicState3ColorBlendEnable = true,
    .extendedDynamicState3ColorBlendEquation = true,
    .extendedDynamicState3ColorWriteMask = true,
    // ...
};
// chain eds3 into feature pNext

// Pipeline:
std::vector dynamicStates = {
    vk::DynamicState::eViewport,
    vk::DynamicState::eScissor,
    vk::DynamicState::ePolygonModeEXT,
    vk::DynamicState::eColorBlendEnableEXT,
    // ...
};

// Record:
cmd.setViewport(0, viewport);
cmd.setScissor(0, scissor);
cmd.setPolygonModeEXT(vk::PolygonMode::eFill);
vk::Bool32 blendEnable = false;
cmd.setColorBlendEnableEXT(0, 1, &blendEnable);
```

---

## 7. Validation / test checklist

1. Device creates with EDS3 extension; if EDS3 features are chained, only supported bits are true.
2. Every dynamic state enum in the pipeline has a matching set command before draw.
3. Removing a state from static pipeline info without adding dynamic set fails validation (use as test).
4. Viewport/scissor path still works (current baseline).
5. Sample mask NULL behavior checked if used with maintenance10.
6. No accidental reliance on EDS3 commands without feature bits.

---

## 8. Shader interaction

**Indirect.** Extra dynamic pipeline state. Important surface when using [VK_EXT_shader_object](./VK_EXT_shader_object.md). Does not change SPIR-V; host must set state before draws that execute the bound shaders.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Vulkan 1.3 core dynamic state | Viewport, scissor, depth test, etc. already core |
| EDS1 feature struct in chain | Redundant with 1.3 but currently present |
| `VK_EXT_shader_object` | Needs wide dynamic state |
| `VK_EXT_blend_operation_advanced` | Dynamic advanced blend setters |
| `VK_KHR_maintenance10` | NULL sample mask clarification |
| Graphics pipelines | `pDynamicState` must list each dynamic enum |

---

## 10. Non-goals / do not do

- Do not enable **all** EDS3 feature bits blindly — query support and enable what you call.
- Do not list EDS1/EDS2 as device extensions (already correctly omitted per `vk_device.hpp` comments).
- Do not assume listing `VK_EXT_extended_dynamic_state3` alone enables commands — feature bits matter.
- Do not document EDS3 as “fully wired” until the EDS3 feature struct and setters exist.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is EDS3 enabled? | **Extension listed**; **EDS3 feature struct not in chain**; EDS1 feature struct is enabled separately |
| Is it used? | **No** — core 1.0 `setViewport`/`setScissor` only; no EDS3 setters |
| Highest-value next steps | Add selective EDS3 features + dynamic states when reducing pipeline count or adopting shader objects |
| Secondary steps | Property cache; wireframe debug; dynamic blend |
