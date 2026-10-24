# VK_EXT_blend_operation_advanced — Implementation Spec

**Status in this repo: ENABLED (device extension + properties cached), FEATURE PARTIALLY CONFIGURED — `advancedBlendCoherentOperations = false` (explicitly disabled). NOT used for advanced blend ops in pipelines.**

Official sources:

- [VK_EXT_blend_operation_advanced](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_blend_operation_advanced.html)
- [VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT.html)
- [VkPhysicalDeviceBlendOperationAdvancedPropertiesEXT](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceBlendOperationAdvancedPropertiesEXT.html)
- [VkPipelineColorBlendAdvancedStateCreateInfoEXT](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineColorBlendAdvancedStateCreateInfoEXT.html)

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::EXTBlendOperationAdvancedExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → feature chain | `advancedBlendCoherentOperations = **false**` (explicit) |
| Property query | `src/core/vk_device.cpp` → properties chain | `vk::PhysicalDeviceBlendOperationAdvancedPropertiesEXT` queried |
| Capability cache | `src/core/types.hpp` → `HardwareCapabilities::blendOperationAdvanced` | Cached |
| API usage | `vk_pipeline.cpp` color blend | **None** — `blendEnable = false`, standard blend state only |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::EXTBlendOperationAdvancedExtensionName,

// src/core/vk_device.cpp (feature chain)
// vk::PhysicalDeviceBlendOperationAdvancedFeaturesEXT
{.advancedBlendCoherentOperations = false},

capabilities.blendOperationAdvanced =
    propertiesChain.get<vk::PhysicalDeviceBlendOperationAdvancedPropertiesEXT>();

// src/core/types.hpp
vk::PhysicalDeviceBlendOperationAdvancedPropertiesEXT blendOperationAdvanced;
```

### Not wired yet

- Advanced `VkBlendOp` values (e.g. overlay, colorburn, HSL ops) on attachments
- `VkPipelineColorBlendAdvancedStateCreateInfoEXT` pNext on color blend state
- Enabling coherent operations if overlapping advanced blends need ordering guarantees
- Any post-process/UI pass that needs Photoshop-style blend modes

---

## 2. What the extension provides

Advanced blend is listed and its properties are cached on `HardwareCapabilities::blendOperationAdvanced`, but the feature that would make overlapping advanced blends well-defined — `advancedBlendCoherentOperations` — is **explicitly false**. Do not document coherent ops as enabled. The forward pipeline keeps `blendEnable = false`.

The current frame (`beginRendering`, MSAA resolve into the swapchain, `endRendering`, Synchronization2 barriers) is an opaque mesh pass plus ImGui. Advanced ops belong on a future UI/compositing attachment, gated by `advancedBlendMaxColorAttachments` (often **1** on some GPUs).

Additional blend equations beyond core `ADD`/`SUBTRACT`/etc., including many Porter-Duff-adjacent and HSL blend modes. Optional **coherent** advanced blends for correct results when self-overlapping geometry blends advanced modes.

| Feature | New API / bit | Engine relevance |
| --- | --- | --- |
| Advanced blend ops | Extended `VkBlendOp` enums | Special effects / 2D compositing |
| Advanced state | `VkPipelineColorBlendAdvancedStateCreateInfoEXT` | src/dst premul, overlap correl |
| Coherent feature | `advancedBlendCoherentOperations` | Currently **forced false** |
| Properties | max advanced attachments, independent blend, non-premul support | Gate usage |

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
typedef struct VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BLEND_OPERATION_ADVANCED_FEATURES_EXT
    void*           pNext;
    VkBool32        advancedBlendCoherentOperations;
} VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT;
```

**Current project choice:** `advancedBlendCoherentOperations = false`.

Only flip to `true` if a pass needs coherent advanced blending and the implementation supports it. Query support before forcing true if this ever becomes optional.

### 3.2 Properties (cached)

```cpp
typedef struct VkPhysicalDeviceBlendOperationAdvancedPropertiesEXT {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BLEND_OPERATION_ADVANCED_PROPERTIES_EXT
    void*           pNext;
    uint32_t        advancedBlendMaxColorAttachments;
    VkBool32        advancedBlendIndependentBlend;
    VkBool32        advancedBlendNonPremultipliedSrcColor;
    VkBool32        advancedBlendNonPremultipliedDstColor;
    VkBool32        advancedBlendCorrelatedOverlap;
    VkBool32        advancedBlendAllOperations;
} VkPhysicalDeviceBlendOperationAdvancedPropertiesEXT;
```

**Already cached** as `HardwareCapabilities::blendOperationAdvanced`.

### 3.3 Pipeline state

```cpp
typedef struct VkPipelineColorBlendAdvancedStateCreateInfoEXT {
    VkStructureType      sType; // VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_ADVANCED_STATE_CREATE_INFO_EXT
    const void*          pNext;
    VkBool32             srcPremultiplied;
    VkBool32             dstPremultiplied;
    VkBlendOverlapEXT    blendOverlap; // UNCORRELATED / DISJOINT / CONJOINT
} VkPipelineColorBlendAdvancedStateCreateInfoEXT;
```

Chain onto `VkPipelineColorBlendStateCreateInfo`. Set attachment `blendEnable = VK_TRUE` and `colorBlendOp` / `alphaBlendOp` to an advanced op. Respect property limits on attachment count and whether all operations are supported.

### 3.4 Commands

No new commands.

---

## Limits

No separate Required Limits table beyond the property struct already cached as `HardwareCapabilities::blendOperationAdvanced`. Do not invent extra numeric minima. `advancedBlendMaxColorAttachments` is the key numeric cap (often **1** on some GPUs).

| Field | Role |
| --- | --- |
| `advancedBlendMaxColorAttachments` | **Key numeric cap** — attachments that may use advanced blend |
| `advancedBlendIndependentBlend` | Per-attachment advanced ops vs one shared op |
| `advancedBlendNonPremultipliedSrcColor` | Non-premultiplied source allowed |
| `advancedBlendNonPremultipliedDstColor` | Non-premultiplied dest allowed |
| `advancedBlendCorrelatedOverlap` | `CONJOINT` / `DISJOINT` overlap modes |
| `advancedBlendAllOperations` | Whether the full advanced `VkBlendOp` set is supported |

`advancedBlendCoherentOperations` is a **feature bit**, not a property, and is **off** in this engine. Do not treat overlapping self-blended advanced ops as coherent.

Official: [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html).

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE (feature explicitly limited)

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep extension name |
| `src/core/vk_device.cpp` | Keep properties; keep coherent ops **false** until needed |
| `src/core/types.hpp` | Keep `blendOperationAdvanced` |

### 4.2 Pipeline color blend — NOT CONSUMED

**File:** `src/render/vk_pipeline.cpp`

Current state:

```cpp
colorBlendAttachment.blendEnable = vk::False;
// standard PipelineColorBlendStateCreateInfo, no advanced pNext
```

When needed (UI compositing, editor overlays, special materials):

1. Check `advancedBlendMaxColorAttachments` and `advancedBlendAllOperations`.
2. Enable blend on the attachment; pick advanced `VkBlendOp`.
3. Chain `PipelineColorBlendAdvancedStateCreateInfoEXT`.
4. If overlapping self-blend requires it, enable `advancedBlendCoherentOperations` at device create (requires support).

### 4.3 Dynamic state / shader objects (future)

EDS3 can set advanced blend state dynamically (`vkCmdSetColorBlendAdvancedEXT` when available) — only relevant if migrating state off pipelines.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep extension + property cache; coherent false | `vk_device.*` | Matches current non-use |
| P1 | Document property dump at init | `vk_device.cpp` | Know GPU coverage |
| P2 | First advanced blend only when a real pass needs it | `vk_pipeline.cpp` | Avoid dead code |
| P3 | Consider coherent ops for that pass | feature chain | Correctness for overlap |

### Implementation recipe

1. **Keep bootstrap as-is.** Extension listed; properties cached; `advancedBlendCoherentOperations = false`. Do not flip coherent ops “for completeness.”
2. **Do not add advanced blend to the opaque forward pass.** `vk_renderer.cpp` uses dynamic rendering + sync2, MSAA resolve into the swapchain, `blendEnable = false`. That remains correct.
3. **First consumer only when a pass needs Photoshop-style ops.** Check `advancedBlendMaxColorAttachments` (may be 1), optionally `advancedBlendAllOperations`, chain `PipelineColorBlendAdvancedStateCreateInfoEXT`, enable blend on that attachment only.
4. **If overlapping geometry requires it**, query support and then enable `advancedBlendCoherentOperations`. Until then, avoid self-overlapping advanced blends. Do not document coherent ops as enabled.
5. **Siblings.** Local-read enabled unused; FSR enabled unused (`HardwareCapabilities::fragmentShadingRate`); ASTC HDR **false** (GPU-gated); EDS1/2 core 1.3; EDS3 still an extension with limited use (dynamic advanced blend setter is EDS3 — only after that bit is chained).

---

## 6. Concrete integration sketch

```cpp
const auto& adv = device.capabilities.blendOperationAdvanced;
// assert attachmentCount <= adv.advancedBlendMaxColorAttachments

vk::PipelineColorBlendAdvancedStateCreateInfoEXT advancedState{
    .srcPremultiplied = true,
    .dstPremultiplied = true,
    .blendOverlap = vk::BlendOverlapEXT::eUncorrelated,
};

vk::PipelineColorBlendAttachmentState att{};
att.blendEnable = true;
att.colorBlendOp = vk::BlendOp::eOverlayEXT; // example advanced op
att.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha; // factors may be ignored for some ops
// ...

vk::PipelineColorBlendStateCreateInfo colorBlending{
    .pNext = &advancedState,
    .attachmentCount = 1,
    .pAttachments = &att,
};
```

Verify each advanced op against properties (`advancedBlendAllOperations` or documented subsets).

---

## 7. Validation / test checklist

1. Device creates with extension; feature coherent ops false as today.
2. Properties cached and sensible (`advancedBlendMaxColorAttachments ≥ 1` on supporting GPUs).
3. When advanced blend used: pNext present, blend enabled, op supported.
4. Premul flags match content encoding.
5. If coherent ops left false, avoid relying on coherent overlapping advanced blends.

---

## 8. Shader interaction

**Indirect — fragment shader outputs still write color attachments; advanced blend runs in the fixed-function blend unit.** FS does not call blend ops; pipeline color-blend advanced state selects the op. Dual-source modes may constrain FS outputs if ever used.

### Engine status

Extension listed; `advancedBlendCoherentOperations = false`; no FS changes until an advanced op is selected.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Standard color blend state | Advanced ops extend the same attachment state |
| `VK_EXT_extended_dynamic_state3` | Dynamic advanced blend setters |
| MSAA / resolve | Blend occurs before resolve; know target format |
| Premultiplied alpha pipeline | Align with `srcPremultiplied` / `dstPremultiplied` |

---

## 10. Non-goals / do not do

- Do not set `advancedBlendCoherentOperations = true` “for free” without a consumer and support check.
- Do not enable advanced blend on the main opaque forward pass without an art/tech reason (current `blendEnable = false` is correct for opaque).
- Do not assume all advanced ops exist when `advancedBlendAllOperations == VK_FALSE`.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is Advanced Blend enabled? | **Extension yes**; **coherent ops explicitly false**; properties cached |
| Is it used? | **No** advanced blend ops in pipelines |
| Highest-value next steps | Only implement when a pass needs advanced modes; dump properties |
| Secondary steps | Coherent ops if overlapping advanced blends appear |
