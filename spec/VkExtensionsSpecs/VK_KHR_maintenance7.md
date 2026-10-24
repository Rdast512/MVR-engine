# VK_KHR_maintenance7 — Implementation Spec

**Status in this repo: ENABLED (device extension + feature + properties cached), NOT CONSUMED — no property log and no dynamic-buffer / FSR / dual-aspect / CONTENTS_INLINE / layered-API use.**

Official sources:

- [VK_KHR_maintenance7](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_maintenance7.html)
- [Proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_maintenance7.html)
- [VkPhysicalDeviceMaintenance7FeaturesKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMaintenance7FeaturesKHR.html)
- [VkPhysicalDeviceMaintenance7PropertiesKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMaintenance7PropertiesKHR.html)

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::KHRMaintenance7ExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → `createLogicalDevice()` feature chain | `vk::PhysicalDeviceMaintenance7FeaturesKHR{ .maintenance7 = true }` |
| Property query | `src/core/vk_device.cpp` → properties chain | `vk::PhysicalDeviceMaintenance7PropertiesKHR` queried |
| Capability cache | `src/core/types.hpp` → `HardwareCapabilities::maintenance7` | Cached |
| API usage | descriptors / FSR / dynamic rendering / queries | **None** — properties cached, not logged. No layout asserts. No FSR attachment sizing. No dual-aspect D/S. No `CONTENTS_INLINE`. No layered-API list query. |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::KHRMaintenance7ExtensionName,

// src/core/vk_device.cpp (feature chain)
// vk::PhysicalDeviceMaintenance7FeaturesKHR
{.maintenance7 = true},

// src/core/vk_device.cpp (properties chain)
capabilities.maintenance7 =
    propertiesChain.get<vk::PhysicalDeviceMaintenance7PropertiesKHR>();
// cached only — no init log

// src/core/types.hpp
vk::PhysicalDeviceMaintenance7PropertiesKHR maintenance7;
```

### Not wired (no existing call site — do not invent)

- Dynamic-buffer totals on pipeline layouts (`maxDescriptorSetTotal*BuffersDynamic`)
- Dual-aspect depth/stencil access (`separateDepthStencilAttachmentAccess`)
- FSR attachment size (`robustFragmentShadingRateAttachmentAccess`)
- Layered-API property list query
- `VK_RENDERING_CONTENTS_INLINE_BIT_KHR` / secondary-CB hybrid contents
- 32-bit vs 64-bit query result checks (no query pools)

---

## 2. What the extension provides

Maintenance7 is a **cumulative maintenance bag**: small, independent fixes. None alone justifies a dedicated extension. It is still a **device extension** in this repo (`vk::KHRMaintenance7ExtensionName` + `Maintenance7FeaturesKHR`) — not promoted into the 1.4 core structs this project uses. It is **not a replacement** for maintenance4–6 (or 1.1 maintenance3 limits); later bags (8–10) add more on top.

| Feature | New API / bit | Engine relevance |
| --- | --- | --- |
| Separate depth/stencil attachment access property | `separateDepthStencilAttachmentAccess` | Safe sample-one-aspect while writing the other without RMW on sibling aspect |
| Robust FSR attachment size | `robustFragmentShadingRateAttachmentAccess` | Allow undersized FSR attachments (out-of-bounds behavior) |
| Total dynamic buffer limits | `maxDescriptorSetTotal*BuffersDynamic` (+ update-after-bind variants) | Correct pipeline-layout budgeting beyond per-set limits |
| Layered API properties | `VkPhysicalDeviceLayeredApiPropertiesListKHR` | Diagnose real GPU under Venus/Mesa layered stacks |
| Inline + secondary contents flags | `VK_RENDERING_CONTENTS_INLINE_BIT_KHR`, `VK_SUBPASS_CONTENTS_INLINE_AND_SECONDARY_COMMAND_BUFFERS_KHR` | Mixed inline + secondary recording in one render pass instance |
| 32-bit query overflow | behavioral guarantee | 32-bit timestamp/query results match low 32 bits of 64-bit |

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
// Provided by VK_KHR_maintenance7
typedef struct VkPhysicalDeviceMaintenance7FeaturesKHR {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_FEATURES_KHR
    void*           pNext;
    VkBool32        maintenance7; // master switch for the extension feature set
} VkPhysicalDeviceMaintenance7FeaturesKHR;
```

**Already enabled** in this project. Keep `maintenance7 = true` whenever the extension name is listed.

### 3.2 Properties (query after physical device pick)

```cpp
// Provided by VK_KHR_maintenance7
typedef struct VkPhysicalDeviceMaintenance7PropertiesKHR {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_PROPERTIES_KHR
    void*           pNext;
    VkBool32        robustFragmentShadingRateAttachmentAccess;
    VkBool32        separateDepthStencilAttachmentAccess;
    uint32_t        maxDescriptorSetTotalUniformBuffersDynamic;
    uint32_t        maxDescriptorSetTotalStorageBuffersDynamic;
    uint32_t        maxDescriptorSetTotalBuffersDynamic;
    uint32_t        maxDescriptorSetUpdateAfterBindTotalUniformBuffersDynamic;
    uint32_t        maxDescriptorSetUpdateAfterBindTotalStorageBuffersDynamic;
    uint32_t        maxDescriptorSetUpdateAfterBindTotalBuffersDynamic;
} VkPhysicalDeviceMaintenance7PropertiesKHR;
```

| Property | Meaning |
| --- | --- |
| `robustFragmentShadingRateAttachmentAccess` | `VK_TRUE` = FSR attachment scaled size may be smaller than render area (OOB rules apply) |
| `separateDepthStencilAttachmentAccess` | `VK_TRUE` = writing one depth/stencil aspect does not force RMW on the sibling aspect |
| `maxDescriptorSetTotalUniformBuffersDynamic` | Max total dynamic UBO descriptors in a pipeline layout (non-UAB layouts) |
| `maxDescriptorSetTotalStorageBuffersDynamic` | Max total dynamic SSBO descriptors in a pipeline layout (non-UAB layouts) |
| `maxDescriptorSetTotalBuffersDynamic` | Max total dynamic UBO+SSBO descriptors combined |
| `maxDescriptorSetUpdateAfterBindTotal*` | Same family of limits including UAB layouts (often unused / may be zero) |

**Already cached** as `HardwareCapabilities::maintenance7`.

### 3.3 Layered API properties (optional diagnostic query)

```cpp
// Provided by VK_KHR_maintenance7
typedef struct VkPhysicalDeviceLayeredApiPropertiesListKHR {
    VkStructureType                         sType;
    void*                                   pNext;
    uint32_t                                layeredApiCount;
    VkPhysicalDeviceLayeredApiPropertiesKHR* pLayeredApis; // may be NULL for count query
} VkPhysicalDeviceLayeredApiPropertiesListKHR;
```

Chain onto `VkPhysicalDeviceProperties2` for a two-step count-then-fill query. Useful only for logging under layered drivers (e.g. Venus).

### 3.4 Promoted rendering / subpass contents flags

| Flag | Use |
| --- | --- |
| `VK_RENDERING_CONTENTS_INLINE_BIT_KHR` | Dynamic rendering may mix inline draws with secondary command buffers |
| `VK_SUBPASS_CONTENTS_INLINE_AND_SECONDARY_COMMAND_BUFFERS_KHR` | Same idea for legacy render passes |

Vulkan-Hpp:

```cpp
vk::RenderingFlagBits::eContentsInlineKHR
// legacy:
vk::SubpassContents::eInlineAndSecondaryCommandBuffersKHR
```

### 3.5 Behavioral: 32-bit queries

When `maintenance7` is enabled, for unsigned integer queries the 32-bit result **must** equal the low 32 bits of the equivalent 64-bit result. No new command — just a correctness guarantee for timestamp/occlusion query paths that use 32-bit storage.

---

## Limits

See [Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html) and [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html#limits-required).

Properties are cached as `HardwareCapabilities::maintenance7` (`VkPhysicalDeviceMaintenance7PropertiesKHR`). Fields already listed in this spec:

| Field | Kind |
| --- | --- |
| `robustFragmentShadingRateAttachmentAccess` | Bool behavior query |
| `separateDepthStencilAttachmentAccess` | Bool behavior query |
| `maxDescriptorSetTotalUniformBuffersDynamic` | Numeric layout total |
| `maxDescriptorSetTotalStorageBuffersDynamic` | Numeric layout total |
| `maxDescriptorSetTotalBuffersDynamic` | Numeric combined total |
| `maxDescriptorSetUpdateAfterBindTotalUniformBuffersDynamic` | Numeric UAB variant (often unused / may be zero) |
| `maxDescriptorSetUpdateAfterBindTotalStorageBuffersDynamic` | Numeric UAB variant |
| `maxDescriptorSetUpdateAfterBindTotalBuffersDynamic` | Numeric UAB variant |

Do **not** invent Required Limits minima — consume the cached values when budgeting dynamic buffers or gating FSR / dual-aspect D/S access.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep `vk::KHRMaintenance7ExtensionName` in `requiredDeviceExtension` |
| `src/core/vk_device.cpp` | Keep feature + property chain entries |
| `src/core/types.hpp` | Keep `HardwareCapabilities::maintenance7` |

Optional hardening:

1. Log maintenance7 properties at device init — **done**.
2. Assert pipeline-layout dynamic-buffer counts against the total limits when layouts grow — no such layouts today.

### 4.2 Descriptor / pipeline layout budgeting — primary consumer

**Files:**

- `src/core/vk_descriptors.cpp` / `vk_descriptors.hpp`
- any pipeline-layout builder under `src/render/`

When building layouts that use `UNIFORM_BUFFER_DYNAMIC` / `STORAGE_BUFFER_DYNAMIC`:

```cpp
const auto& m7 = device.getCapabilities().maintenance7;

// Sum dynamic UBO/SSBO bindings across all set layouts in the pipeline layout.
assert(totalDynamicUbos <= m7.maxDescriptorSetTotalUniformBuffersDynamic);
assert(totalDynamicSsbos <= m7.maxDescriptorSetTotalStorageBuffersDynamic);
assert(totalDynamicUbos + totalDynamicSsbos <= m7.maxDescriptorSetTotalBuffersDynamic);
```

Today the engine may not hit these limits; wire the check when multi-set dynamic layouts expand.

### 4.3 Depth/stencil aspect independence

**Files:**

- `src/render/vk_renderer.cpp` (dynamic rendering attachments)
- depth/stencil resource setup in `src/core/vk_resource_manager.cpp`

If a pass writes depth and samples stencil (or vice versa) from the same image:

1. Read `capabilities.maintenance7.separateDepthStencilAttachmentAccess`.
2. If `VK_FALSE`, avoid concurrent sibling-aspect access or accept RMW / use separate images.
3. If `VK_TRUE`, proceed with single-aspect barriers and layouts as designed.

Current forward path does not need this until depth+stencil dual-use appears.

### 4.4 Fragment shading rate attachments

**File:** FSR setup path (fragment shading rate is already feature-enabled in `vk_device.cpp`).

When creating FSR attachments:

```cpp
const auto& m7 = device.getCapabilities().maintenance7;
if (!m7.robustFragmentShadingRateAttachmentAccess) {
    // Ensure FSR attachment scaled size covers the full render area.
}
```

### 4.5 Inline + secondary dynamic rendering

**File:** `src/render/vk_renderer.cpp`

Only when multi-threaded secondary command buffer recording is added:

```cpp
vk::RenderingInfo renderingInfo{
    .flags = vk::RenderingFlagBits::eContentsInlineKHR,
    // ...
};
```

Do not set this flag on the current single-CB forward path.

### 4.6 Layered API diagnostics

**File:** `src/core/vk_device.cpp` (init logging only)

Optional two-step query of `PhysicalDeviceLayeredApiPropertiesListKHR` for debug dumps. Not required for correctness.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep enablement as-is | `vk_device.*`, `types.hpp` | Already correct |
| P1 | Log maintenance7 properties at init | `src/core/vk_device.cpp` | **Done** |
| P2 | Assert dynamic-buffer totals against limits | `vk_descriptors.cpp` / layout builders | Prevent silent validation failures as layouts grow |
| P2 | Gate FSR attachment sizing on robustness property | FSR setup path | Avoid invalid undersized FSR attachments |
| P3 | Branch dual-aspect depth/stencil access | renderer / resource manager | Only when sibling-aspect sampling is implemented |
| P3 | Inline+secondary contents flags | multi-threaded record path | Only when secondary CBs enter dynamic rendering |

### Implementation recipe

1. Confirm `src/core/types.hpp` → `HardwareCapabilities` caches `maintenance3`, `maintenance4`, `maintenance5`, `maintenance6`, `maintenance7`, `maintenance9`, and `maintenance10`. There is **no `maintenance8` properties field**.
2. Confirm `src/core/vk_device.cpp` property chain includes `PhysicalDeviceMaintenance7PropertiesKHR` and the feature chain sets `Maintenance7FeaturesKHR{ .maintenance7 = true }`. Keep `vk::KHRMaintenance7ExtensionName` in `requiredDeviceExtension`. `maintenance9` is currently forced **false** (VVL #12449) while its properties are still queried.
3. Confirm `src/core/vk_allocator.hpp` already sets `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT` and `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT` (no VMA bit for maintenance7).
4. Log the cached maintenance7 fields at device init.
5. When layouts grow, assert dynamic UBO/SSBO totals against `HardwareCapabilities::maintenance7`; gate FSR attachment size on `robustFragmentShadingRateAttachmentAccess`.

---

## 6. Concrete integration sketch (descriptor limits)

```cpp
// Illustrative guard near pipeline layout creation
const auto& m7 = /* device capabilities */.maintenance7;

const uint32_t dynUbo  = countDynamicUbos(setLayouts);
const uint32_t dynSsbo = countDynamicSsbos(setLayouts);

if (dynUbo > m7.maxDescriptorSetTotalUniformBuffersDynamic ||
    dynSsbo > m7.maxDescriptorSetTotalStorageBuffersDynamic ||
    (dynUbo + dynSsbo) > m7.maxDescriptorSetTotalBuffersDynamic) {
    throw std::runtime_error("pipeline layout exceeds maintenance7 dynamic buffer totals");
}
```

Do **not** invent a large abstraction for a single assert site.

---

## 7. Validation / test checklist

1. Device creates successfully with `VK_KHR_maintenance7` + `maintenance7 = true`.
2. Capability dump shows `separateDepthStencilAttachmentAccess`, `robustFragmentShadingRateAttachmentAccess`, and the six dynamic-buffer totals.
3. Pipeline layouts with dynamic UBOs/SSBOs stay under total limits (validation layer clean).
4. If FSR attachments are undersized: only when `robustFragmentShadingRateAttachmentAccess == VK_TRUE`.
5. If dual-aspect depth/stencil is used: no unexpected sibling-aspect writes when separate-access is true.
6. 32-bit vs 64-bit query results agree on the low 32 bits for timestamps/occlusion when both are used.
7. Layered-API list query (if added) does not crash when `layeredApiCount == 0`.

---

## 8. Shader interaction

**Indirect only.** No new shader stages or builtins. Shaders are unaffected in source form; this feature changes host-side pipeline/render-pass/resource/sync setup that must stay compatible with the stages already bound (vertex/mesh/fragment/compute/RT). Ensure push constants / heap binds / attachments still match the active shader I/O when using this path.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Vulkan 1.1+ | Extension dependency baseline (repo targets 1.4) |
| Descriptor indexing / dynamic UBO-SSBO layouts | Consumers of total dynamic buffer limits |
| `VK_KHR_fragment_shading_rate` | Consumer of robust FSR attachment property |
| Dynamic rendering (Vulkan 1.3) | Consumer of `CONTENTS_INLINE` bit |
| Query pools / timestamps | Benefit from 32-bit overflow guarantee |
| Layered drivers (Venus, etc.) | Layered API properties diagnostics |

Maintenance7 is **not** promoted into core in the versions this project targets; keep listing the device extension explicitly (as done today).

---

## 10. Non-goals / do not do

- Do not add a runtime optional toggle unless device support must become optional.
- Do not set `CONTENTS_INLINE` on the current single-command-buffer frame path.
- Do not assume sibling depth/stencil aspects are independent without reading `separateDepthStencilAttachmentAccess`.
- Do not treat update-after-bind total dynamic limits as always useful — they may be zero with no practical effect.
- Do not implement layered-API queries as required path; diagnostics only.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is Maintenance7 enabled? | **Yes** — extension listed, feature forced on, properties cached |
| Is it used? | **No** — properties cached, not logged; no layout / FSR / dual-aspect / query sites |
| Highest-value next steps | None until dynamic-buffer layouts, FSR attachments, or dual-aspect D/S exist |
| Secondary steps | Dual-aspect depth/stencil branching; inline+secondary flags when multi-threaded recording lands |
