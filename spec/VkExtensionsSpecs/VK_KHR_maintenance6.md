# VK_KHR_maintenance6 — Implementation Spec

**Status in this repo: PROMOTED TO CORE (Vulkan 1.4), ENABLED via Vulkan14Features::maintenance6, properties cached. NOT CONSUMED — no property log, no bindDescriptorSets2 / pushConstants2 / BindMemoryStatus / null index buffer.**

Official sources:

- [VK_KHR_maintenance6](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_maintenance6.html)
- [Proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_maintenance6.html)
- [VkPhysicalDeviceVulkan14Features](https://docs.vulkan.org/spec/latest/chapters/features.html) (`maintenance6`)
- [VkPhysicalDeviceMaintenance6Properties](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMaintenance6Properties.html)

> **Promoted to core:** Functionality is core Vulkan 1.4 with the KHR suffix omitted. Prefer core / unprefixed APIs (or Vulkan-Hpp without `KHR`).  
> **Do not** list `VK_KHR_maintenance6` in `requiredDeviceExtension`.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | **Must not be listed** — promoted to 1.4 |
| Feature enable | `src/core/vk_device.cpp` → feature chain | `vk::PhysicalDeviceVulkan14Features{ .maintenance6 = true }` |
| Property query | `src/core/vk_device.cpp` → properties chain | `vk::PhysicalDeviceMaintenance6Properties` queried |
| Capability cache | `src/core/types.hpp` → `HardwareCapabilities::maintenance6` | Cached |
| API usage | bind / descriptors / push constants / FSR | **None** — properties cached, not logged. No `bindDescriptorSets2` / `pushConstants2` (heaps + DGC `pushDataEXT` only). No `BindMemoryStatus` (VMA binds). No null index buffer (mesh/DGC). |

### Already wired

```cpp
// src/core/vk_device.cpp — vk::PhysicalDeviceVulkan14Features
{.maintenance6 = true, /* ... */},

// src/core/vk_device.cpp (properties)
capabilities.maintenance6 =
    propertiesChain.get<vk::PhysicalDeviceMaintenance6Properties>();
// cached only — no init log (unlike maintenance5)

// src/core/types.hpp
vk::PhysicalDeviceMaintenance6Properties maintenance6;
```

### Not wired (no existing call site — do not invent)

- `VkBindMemoryStatus` / per-binding result on bind-memory2 operations
- Null / `VK_NULL_HANDLE` index buffer binds
- `vkCmdBindDescriptorSets2`, `vkCmdPushConstants2`, push-descriptor / descriptor-buffer `*2` infos
- Property-driven FSR combiner / CIS layout / block-texel multi-layer views

---

## 2. What maintenance6 provides

Maintenance6 is a **cumulative maintenance bag**: small, independent bind/push/property fixes promoted into Vulkan 1.4 (`Vulkan14Features::maintenance6`). It is **not a replacement** for maintenance5 (Flags2 / buffer usage 2 / index bind 2) or earlier 1.1–1.3 maintenance; later bags (7–10) add more on top. Do not re-list `VK_KHR_maintenance6`.

| Feature | New API / property | Engine relevance |
| --- | --- | --- |
| Per-binding bind-memory status | `VkBindMemoryStatus` in bind buffer/image memory2 infos | Diagnose which bind failed in multi-bind batches |
| Null index buffer | Allow `VK_NULL_HANDLE` index buffer (with rules) | Mesh paths / non-indexed fallbacks; defensive unbind |
| Extensible descriptor bind | `vkCmdBindDescriptorSets2` + `VkBindDescriptorSetsInfo` | pNext-friendly binds; still relevant for **legacy sets** |
| Extensible push constants | `vkCmdPushConstants2` + `VkPushConstantsInfo` | pNext-friendly push constants |
| Extensible push descriptors / descriptor buffer offsets | `*2` command variants + info structs | When those binding models are used |
| Block texel view multi-layer | `blockTexelViewCompatibleMultipleLayers` | Compressed texture view rules |
| Combined image-sampler limit | `maxCombinedImageSamplerDescriptorCount` | Descriptor budgeting for CIS descriptors |
| FSR combiner clamp property | `fragmentShadingRateClampCombinerInputs` | Correct FSR combiner programming |

Descriptor **heaps** (`VK_EXT_descriptor_heap`) already supersede much classic `bindDescriptorSets` traffic in this engine when heaps are available. Maintenance6 remains important for:

1. Legacy set mode fallback (`DescriptorBindingMode::LegacySets`)
2. Extensible push-constant / future pNext chains
3. Bind-memory status and property queries

---

## 3. Structures and commands

### 3.1 Feature — CORE via Vulkan 1.4

```cpp
// Inside VkPhysicalDeviceVulkan14Features
VkBool32 maintenance6;
```

**Already enabled:** `maintenance6 = true`.

### 3.2 Properties

```cpp
typedef struct VkPhysicalDeviceMaintenance6Properties {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_PROPERTIES
    void*           pNext;
    VkBool32        blockTexelViewCompatibleMultipleLayers;
    uint32_t        maxCombinedImageSamplerDescriptorCount;
    VkBool32        fragmentShadingRateClampCombinerInputs;
} VkPhysicalDeviceMaintenance6Properties;
```

| Property | Meaning |
| --- | --- |
| `blockTexelViewCompatibleMultipleLayers` | Multi-layer views of block-texel-compatible images allowed under the property’s rules |
| `maxCombinedImageSamplerDescriptorCount` | Max descriptors contributing to a combined image sampler |
| `fragmentShadingRateClampCombinerInputs` | Whether FSR combiner inputs are clamped |

**Already cached** as `HardwareCapabilities::maintenance6`.

### 3.3 Bind memory status

```cpp
typedef struct VkBindMemoryStatus {
    VkStructureType sType; // VK_STRUCTURE_TYPE_BIND_MEMORY_STATUS
    const void*     pNext;
    VkResult*       pResult; // per-bind result written by the implementation
} VkBindMemoryStatus;
```

Chain onto each `VkBindBufferMemoryInfo` / `VkBindImageMemoryInfo` (or the Vulkan-Hpp equivalents) when using bind-memory2 multi-bind so a single failing entry does not leave the app guessing which element failed.

### 3.4 Extensible bind descriptor sets / push constants (core names)

```cpp
void vkCmdBindDescriptorSets2(
    VkCommandBuffer                 commandBuffer,
    const VkBindDescriptorSetsInfo* pBindDescriptorSetsInfo);

void vkCmdPushConstants2(
    VkCommandBuffer              commandBuffer,
    const VkPushConstantsInfo*   pPushConstantsInfo);
```

Vulkan-Hpp:

```cpp
cmd.bindDescriptorSets2(vk::BindDescriptorSetsInfo{ /* layout, firstSet, sets, dynamic offsets */ });
cmd.pushConstants2(vk::PushConstantsInfo{ /* layout, stageFlags, offset, size, pValues */ });
```

Similar `*2` entry points exist for push descriptors and descriptor-buffer offset binding. Prefer these when adding new code that may need pNext extensibility.

### 3.5 Null index buffer

With maintenance6, binding a null index buffer is allowed under the specification’s valid-usage rules (typically paired with non-indexed draws or before a later valid bind). Useful for explicit “no index buffer” state; not required for the current always-indexed mesh path.

---

## Limits

See [Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html) and [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html#limits-required).

| Property | Kind | This repo |
| --- | --- | --- |
| `maxCombinedImageSamplerDescriptorCount` | **Numeric** — YCbCr combined image sampler descriptor multiplicity | Cached `HardwareCapabilities::maintenance6` |
| `blockTexelViewCompatibleMultipleLayers` | Bool behavior query | Same cache |
| `fragmentShadingRateClampCombinerInputs` | Bool behavior query | Same cache |

`maxCombinedImageSamplerDescriptorCount` is the important numeric field. Read the implementation value; do not invent a Required Limits number here.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep maintenance6 **out** of `requiredDeviceExtension` |
| `src/core/vk_device.cpp` | Keep `maintenance6 = true` + property cache |
| `src/core/types.hpp` | Keep `HardwareCapabilities::maintenance6` |

### 4.2 Descriptor binding paths

**Files:**

- `src/core/vk_descriptors.*` — set layout / heap vs legacy mode
- `src/render/vk_renderer.cpp` — frame bind path
- `src/render/vk_pipeline.cpp` — layout / push ranges

**Current primary path (heaps):**

```cpp
commandBuffers[currentFrame].bindResourceHeapEXT(resourceHeapInfo);
commandBuffers[currentFrame].bindSamplerHeapEXT(samplerHeapInfo);
// pushDataEXT for per-object data
```

Heap binds are **not** replaced by `bindDescriptorSets2`. Maintenance6 matters when:

- `descriptorBindingMode == DescriptorBindingMode::LegacySets`
- Future code reintroduces classic sets alongside heaps
- Push constants need an extensible info struct (today heaps use `pushDataEXT` instead of classic push constants in the main loop)

There is **no** `bindDescriptorSets` / `pushConstants` call site today. Do not add `bindDescriptorSets2` just to consume maintenance6.

### 4.3 Bind-memory status (resource allocation)

**Files:** `src/core/vk_allocator.hpp` / VMA paths, any manual `bindBufferMemory2` / `bindImageMemory2`

If multi-info bind batches are added outside VMA, chain `BindMemoryStatus` per info. VMA-internal binds need no app-side status unless you bypass VMA.

### 4.4 Properties consumers

| Property | Consumer |
| --- | --- |
| `maxCombinedImageSamplerDescriptorCount` | Descriptor layout builders if CIS descriptors return |
| `fragmentShadingRateClampCombinerInputs` | Future FSR setup (`VK_KHR_fragment_shading_rate`) |
| `blockTexelViewCompatibleMultipleLayers` | Texture view creation for block-compressed arrays |

### 4.5 Null index buffer

Mesh + DGC only; no index bind. Do not add a null `bindIndexBuffer` just to consume maintenance6.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep enablement; never list as device extension | `vk_device.*`, `types.hpp` | Already correct |
| P1 | Use `bindDescriptorSets2` / `pushConstants2` in any **new** legacy-set code | descriptors / renderer | Future-proof pNext |
| P1 | Gate FSR combiner logic on `fragmentShadingRateClampCombinerInputs` when FSR lands | FSR setup path | Spec-correct combiners |
| P2 | Log maintenance6 properties at init | `vk_device.cpp` | Not done (cached only) |
| P2 | CIS count when building non-heap layouts | descriptors | Avoid over-large CIS layouts |
| P3 | `BindMemoryStatus` on multi-bind | manual bind helpers | Better bind diagnostics |
| P3 | Null index buffer | only if draw model needs it | Optional |

### Implementation recipe

1. Confirm `src/core/types.hpp` → `HardwareCapabilities` caches `maintenance3`, `maintenance4`, `maintenance5`, `maintenance6`, `maintenance7`, `maintenance9`, and `maintenance10`. There is **no `maintenance8` properties field**.
2. Confirm `src/core/vk_device.cpp` property chain includes `PhysicalDeviceMaintenance3Properties`–`Maintenance7PropertiesKHR`, `Maintenance9PropertiesKHR`, and `Maintenance10PropertiesKHR` (skip 8). Feature bits: `Vulkan13Features::maintenance4`, `Vulkan14Features::maintenance5`/`maintenance6 = true`, plus `Maintenance7/8/10FeaturesKHR`; `maintenance9` is currently forced **false** (VVL #12449) while its properties are still queried.
3. Confirm `src/core/vk_allocator.hpp` already sets `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT` and `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT` (no VMA bit for maintenance6).
4. Use `bindDescriptorSets2` / `pushConstants2` only on **new** legacy-set code; do not replace heap binds.
5. When FSR or CIS layouts land, consume `fragmentShadingRateClampCombinerInputs` and `maxCombinedImageSamplerDescriptorCount` from `HardwareCapabilities::maintenance6`.

---

## 6. Concrete integration sketch (legacy sets)

```cpp
// Only when not using descriptor heaps:
if (descriptorManager.descriptorBindingMode == DescriptorBindingMode::LegacySets) {
    vk::BindDescriptorSetsInfo bindInfo{
        .layout             = *pipeline.pipelineLayout,
        .firstSet           = 0,
        .descriptorSetCount = 1,
        .pDescriptorSets    = &*descriptorManager.descriptorSets[currentFrame]
    };
    commandBuffers[currentFrame].bindDescriptorSets2(bindInfo);
}
// Heap path unchanged: bindResourceHeapEXT / bindSamplerHeapEXT / pushDataEXT
```

---

## 7. Validation / test checklist

1. Device creates with `maintenance6 = true` and **without** `VK_KHR_maintenance6` in the extension list.
2. Capability dump shows the three maintenance6 properties.
3. Heap path: no forced migration to `bindDescriptorSets2`.
4. If legacy sets use `bindDescriptorSets2`: validation clean; sets match layout.
5. If FSR is enabled later: combiner inputs respect `fragmentShadingRateClampCombinerInputs`.
6. Multi-bind with status: each `pResult` inspected on failure.

---

## 8. Shader interaction

**Indirect only.** No new shader stages or builtins. Shaders are unaffected in source form; this feature changes host-side pipeline/render-pass/resource/sync setup that must stay compatible with the stages already bound (vertex/mesh/fragment/compute/RT). Ensure push constants / heap binds / attachments still match the active shader I/O when using this path.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Vulkan 1.4 | Promotion host |
| `VK_EXT_descriptor_heap` | Supersedes most set binds when supported (runtime-gated in this repo) |
| `VK_EXT_descriptor_buffer` | Related `*2` offset binds if used |
| `VK_KHR_fragment_shading_rate` | Consumes clamp property |
| Maintenance5 Flags2 / buffer usage 2 | Orthogonal flag-space expansion |
| VMA | Own bind path; status struct only for app-side bind2 |

---

## 10. Non-goals / do not do

- **Do not** add `VK_KHR_maintenance6` to `requiredDeviceExtension`.
- Do not replace heap binds with `bindDescriptorSets2`.
- Do not invent `bindDescriptorSets2` / `pushConstants2` / null index-buffer binds.
- Do not treat maintenance6 as a substitute for descriptor heaps or push data.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| How is Maintenance6 enabled? | **Core Vulkan 1.4** — `Vulkan14Features::maintenance6 = true` |
| Properties cached? | **Yes** — `HardwareCapabilities::maintenance6` |
| Is it used? | **No** — properties cached, not logged; no bind/push/`BindMemoryStatus` sites |
| Highest-value next steps | None until a legacy-set, FSR, CIS, or app-side bind2 path exists |
| Extension string? | **Never list** in `requiredDeviceExtension` |
