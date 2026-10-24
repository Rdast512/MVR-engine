# VK_EXT_memory_priority — Implementation Spec

**Status in this repo: ENABLED (device extension + feature), PARTIALLY CONSUMED via VMA `VmaAllocationCreateInfo::priority` + `VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT`. No raw `VkMemoryPriorityAllocateInfoEXT` pNext on engine `vkAllocateMemory` calls.**

Official sources:

- [VK_EXT_memory_priority](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_memory_priority.html)
- [VkPhysicalDeviceMemoryPriorityFeaturesEXT](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMemoryPriorityFeaturesEXT.html)
- [VkMemoryPriorityAllocateInfoEXT](https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryPriorityAllocateInfoEXT.html)

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::EXTMemoryPriorityExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → feature chain | `memoryPriority = true` |
| Property query | — | **None** (no property struct) |
| Capability cache | `src/core/types.hpp` | **No** field |
| API usage | VMA / allocate paths | **Partial** — `VmaAllocationCreateInfo::priority` on resource/texture/util allocs |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::EXTMemoryPriorityExtensionName,

// src/core/vk_device.cpp (feature chain)
// vk::PhysicalDeviceMemoryPriorityFeaturesEXT
{.memoryPriority = true},

// src/core/vk_allocator.hpp
VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT
// + resolveAllocationCreateInfo() default priority when caller left 0
```

Callers already set `allocInfo.priority` in `texture_manager.cpp`, `vk_resource_manager.cpp`, and `vk_utils.cpp`.

### Not wired yet

- Raw `VkMemoryPriorityAllocateInfoEXT` on non-VMA `vkAllocateMemory` (none expected)
- Dynamic `vkSetDeviceMemoryPriorityEXT` after allocate (see pageable EXT)
- Telemetry pairing with `VK_EXT_memory_budget` to demote cold resources

---

## 2. What the extension provides

At allocation time, assign a **priority** in `[0.0, 1.0]` so the implementation can prefer which allocations stay in fast memory under pressure (especially with oversubscription / pageable heaps).

The problem is undifferentiated allocations: when `DEVICE_LOCAL` is oversubscribed, the implementation has no hint which images are working-set vs staging. This remains a **device extension** (not promoted).

Priority is a **hint only**. `1.0` does not guarantee residency, and setting every allocation to `1.0` defeats the policy. Values outside `[0.0, 1.0]` fail validation.

| Feature | New API / bit | Engine relevance |
| --- | --- | --- |
| Feature | `memoryPriority` | Master switch (enabled) |
| Allocate pNext | `VkMemoryPriorityAllocateInfoEXT` | Per-allocation priority |
| Default | 0.5 if not specified | Current implicit behavior |

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
typedef struct VkPhysicalDeviceMemoryPriorityFeaturesEXT {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT
    void*           pNext;
    VkBool32        memoryPriority;
} VkPhysicalDeviceMemoryPriorityFeaturesEXT;
```

**Already enabled.**

### 3.2 Allocate info

```cpp
typedef struct VkMemoryPriorityAllocateInfoEXT {
    VkStructureType sType; // VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT
    const void*     pNext;
    float           priority; // 0.0 .. 1.0 inclusive
} VkMemoryPriorityAllocateInfoEXT;
```

Chain onto `VkMemoryAllocateInfo::pNext` (or the equivalent VMA mechanism for custom memory allocate pNext).

### 3.3 Commands

No new commands. Uses `vkAllocateMemory` pNext chain only.

> Related but separate: `VK_EXT_pageable_device_local_memory` adds `vkSetDeviceMemoryPriorityEXT` for **dynamic** priority changes after allocate. This extension is **static at allocate time**.

---

## Limits

No property struct and no Required Limits numbers. Behavioral limit only ([Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html) has no dedicated entry).

| Item | Constraint |
| --- | --- |
| `VkMemoryPriorityAllocateInfoEXT::priority` | `float` in **[0.0, 1.0]** inclusive |
| Omitted pNext | Default **0.5** |
| Outside `[0.0, 1.0]` | Validation error |
| Semantics | Hint — does **not** guarantee residency |

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep extension name |
| `src/core/vk_device.cpp` | Keep `{.memoryPriority = true}` |

### 4.2 Allocation path — NOT STARTED

**Files:**

- `src/core/vk_allocator.hpp` — central VMA allocate entry points (`alocateBuffer`, `alocateImage`)
- Call sites: `src/core/vk_descriptors.cpp` (heaps), texture/resource managers

VMA support options:

1. `VmaAllocationCreateInfo::priority` (VMA maps this to the extension when enabled — preferred if VMA version supports it).
2. Or custom memory allocate callbacks / pNext injection if using raw `vkAllocateMemory`.

Suggested priority policy:

| Resource | Priority |
| --- | --- |
| Swapchain-sized color/depth / main G-buffer | 1.0 |
| Descriptor heaps, UBOs, AS scratch | 0.8–1.0 |
| Resident textures (hot) | 0.5–0.8 |
| Streaming / rarely used textures | 0.2–0.4 |
| Host staging | 0.0–0.3 |

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep enablement + VMA priority bit | `vk_device.*`, `vk_allocator.hpp` | Already correct |
| P1 | Keep / refine caller priorities | `texture_manager.cpp`, `vk_resource_manager.cpp` | Already consumed |
| P2 | Combine with budget telemetry | memory budget path | Decide when to demote |
| P3 | Dynamic priority via pageable EXT | pageable path | Runtime reprioritize |

### Implementation recipe

1. Keep `vk::EXTMemoryPriorityExtensionName` and `{.memoryPriority = true}` (`src/core/vk_device.hpp`, `vk_device.cpp`).
2. Keep `VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT` in `src/core/vk_allocator.hpp` (already set with `EXT_MEMORY_BUDGET`, `MAINTENANCE4`, `MAINTENANCE5`). VMA maps `VmaAllocationCreateInfo::priority` to allocate-time `VkMemoryPriorityAllocateInfoEXT` (and may call `vkSetDeviceMemoryPriorityEXT` when pageable is enabled).
3. Keep `resolveAllocationCreateInfo` defaults when the caller left priority at `0`: images `1.0`, host-visible `0.25`, device-local buffers `0.75`.
4. Callers already pass priorities — refine, do not flatten to `1.0`: `texture_manager.cpp` (staging `0.25`, device buffers `0.75`, sampled images `0.9`); `vk_resource_manager.cpp` (staging `0.25`, color/depth attachments `1.0`, other images `0.9`).
5. For runtime hot/cold changes, use `VK_EXT_pageable_device_local_memory` on dedicated `VkDeviceMemory` (VMA suballocs share one memory object). No `HardwareCapabilities` field — there is no property struct.

---

## 6. Concrete integration sketch (VMA)

```cpp
VmaAllocationCreateInfo allocInfo{};
allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
// When VMA is built with memory priority support:
allocInfo.priority = 1.0f; // high-priority render target

// Or explicit pNext if allocating memory manually:
vk::MemoryPriorityAllocateInfoEXT prio{
    .priority = 1.0f,
};
vk::MemoryAllocateInfo mai{
    .pNext = &prio,
    // ...
};
```

Start with high priority for MSAA color/depth and descriptor heaps; leave default (0.5) elsewhere until measured.

---

## 7. Validation / test checklist

1. Device creates with extension + `memoryPriority = true`.
2. Allocations with priority 0.0 and 1.0 succeed.
3. Priority outside [0,1] fails validation.
4. Under memory pressure (if testable), high-priority working set stays stable longer than low-priority (implementation-dependent).
5. No regressions when priority omitted (default 0.5).

---

## 8. Shader interaction

**None (direct).** This extension is host / queue / memory / presentation / resource plumbing only. It does not add shader stages, builtins, storage classes, or SPIR-V capabilities. Shaders continue to use whatever programmable stages the active pipeline or shader objects declare; this feature only changes how the host prepares or submits work those shaders run on.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| `VK_EXT_memory_budget` | Know when heaps are under pressure |
| `VK_EXT_pageable_device_local_memory` | Dynamic `vkSetDeviceMemoryPriorityEXT` after allocate |
| VMA | Best injection point for engine-wide policy |
| Dedicated allocations | Priority still applies to dedicated device memory |

---

## 10. Non-goals / do not do

- Do not assume priority guarantees residency — it is a hint.
- Do not set every allocation to 1.0 (defeats the purpose).
- Do not confuse allocate-time priority with pageable dynamic priority API.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is Memory Priority enabled? | **Yes** — extension + `memoryPriority = true` |
| Is it used? | **Partially** — VMA `priority` on allocs; no raw allocate pNext |
| Highest-value next steps | Keep policy table; do not flatten every alloc to 1.0 |
| Secondary steps | Pair with budget telemetry + pageable dynamic priority |
