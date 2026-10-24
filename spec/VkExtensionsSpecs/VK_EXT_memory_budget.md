# VK_EXT_memory_budget — Implementation Spec

**Status in this repo: ENABLED (device extension), PARTIALLY CONSUMED via VMA (`VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT`). No direct app-level budget UI/logic.**

Official sources:

- [VK_EXT_memory_budget](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_memory_budget.html)
- [VkPhysicalDeviceMemoryBudgetPropertiesEXT](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMemoryBudgetPropertiesEXT.html)

> Note: There is **no feature struct**. Enabling the extension allows chaining budget properties onto memory property queries.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::EXTMemoryBudgetExtensionName` present |
| Feature enable | — | **N/A** (no feature bit) |
| Property query (engine) | `src/core/vk_device.cpp` | **Not** explicitly chained in `HardwareCapabilities` |
| VMA integration | `src/core/vk_allocator.hpp` | `VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT` set |
| API usage | engine code | **Indirect** — VMA may call `vkGetPhysicalDeviceMemoryProperties2` with budget chain |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::EXTMemoryBudgetExtensionName,

// src/core/vk_allocator.hpp
allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT |
    VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT | VMA_ALLOCATOR_CREATE_KHR_BIND_MEMORY2_BIT |
    VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT | VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT |
    VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT;
```

### Not wired yet

- Explicit `VkPhysicalDeviceMemoryBudgetPropertiesEXT` query cached on `HardwareCapabilities`
- Engine-facing budget telemetry (HUD, logs, allocation refusal policies)
- Direct use of `vmaGetHeapBudgets` results in gameplay/engine systems (if desired)

---

## 2. What the extension provides

Per-heap **budget** and **usage** counters so applications (and allocators like VMA) can avoid oversubscribing device memory.

Without a live per-heap budget, the app only sees static `VkPhysicalDeviceMemoryProperties` heap sizes and can allocate until the driver fails or the OS pages. This remains a **device extension** (not promoted; **no feature bit**).

It does **not** make `heapBudget` a hard Vulkan limit. Values are guidance, may be `0` on unused heaps, and change over time — re-query (VMA does this). Staying under budget does not guarantee the next allocation succeeds.

| Feature | New API / bit | Engine relevance |
| --- | --- | --- |
| Budget properties | `VkPhysicalDeviceMemoryBudgetPropertiesEXT` via `vkGetPhysicalDeviceMemoryProperties2` | VMA heap budgets; optional engine telemetry |
| No feature bit | Extension enable only | List in `requiredDeviceExtension` |

---

## 3. Structures and commands

### 3.1 Feature

None.

### 3.2 Properties (query with memory properties 2)

```cpp
typedef struct VkPhysicalDeviceMemoryBudgetPropertiesEXT {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT
    void*           pNext;
    VkDeviceSize    heapBudget[VK_MAX_MEMORY_HEAPS]; // bytes app should stay under
    VkDeviceSize    heapUsage[VK_MAX_MEMORY_HEAPS];  // current usage estimate
} VkPhysicalDeviceMemoryBudgetPropertiesEXT;
```

Usage pattern:

```cpp
vk::StructureChain<vk::PhysicalDeviceMemoryProperties2,
                   vk::PhysicalDeviceMemoryBudgetPropertiesEXT>
    chain = physicalDevice.getMemoryProperties2<
        vk::PhysicalDeviceMemoryProperties2,
        vk::PhysicalDeviceMemoryBudgetPropertiesEXT>();

const auto& budget = chain.get<vk::PhysicalDeviceMemoryBudgetPropertiesEXT>();
// budget.heapBudget[i], budget.heapUsage[i]
```

Values can change over time; re-query periodically (VMA does this when the budget flag is set).

### 3.3 Commands

No new commands. Uses existing `vkGetPhysicalDeviceMemoryProperties2`.

---

## Limits

No feature bit and no Required Limits table entry. Query `VkPhysicalDeviceMemoryBudgetPropertiesEXT` by chaining it onto [`vkGetPhysicalDeviceMemoryProperties2`](https://docs.vulkan.org/spec/latest/chapters/memory.html) (`pNext`). Official limits chapter: [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html).

| Field | Constraint |
| --- | --- |
| `heapBudget[VK_MAX_MEMORY_HEAPS]` | **Guidance**, not a hard Vulkan limit. Changes over time — re-query (VMA does this when `VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT` is set). |
| `heapUsage[VK_MAX_MEMORY_HEAPS]` | Current usage estimate; same volatility. |
| Unused heaps | Budget and usage **may be `0`**. |

Do not cache a one-shot query at device create for later allocation decisions.

---

## 4. Where to implement in this codebase

### 4.1 Extension list — DONE

**File:** `src/core/vk_device.hpp`

### 4.2 VMA — PARTIAL CONSUMPTION — DONE

**File:** `src/core/vk_allocator.hpp`

The budget bit lets VMA track heap budgets and make better allocation decisions / defragmentation choices. This is the primary practical consumer.

### 4.3 Optional engine telemetry — NOT DONE

**Possible files:**

- `src/core/vk_device.cpp` — periodic or init-time log of heap budget/usage
- Debug UI / ImGui overlay via `src/core/vk_engine.cpp`
- Fail-soft allocation policies near resource create sites (`texture_manager.cpp`, `vk_resource_manager.cpp`)

VMA API sketch:

```cpp
uint32_t heapCount = /* from memory properties */;
std::vector<VmaBudget> budgets(heapCount);
vmaGetHeapBudgets(allocator, budgets.data());
// budgets[i].budget, budgets[i].usage, stats
```

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep extension + VMA flag | `vk_device.hpp`, `vk_allocator.hpp` | Already correct |
| P1 | Debug log of VMA heap budgets | allocator or device init | Visibility under memory pressure |
| P2 | Optional HUD / Tracy counters | engine debug | Runtime awareness |
| P3 | Soft allocation limits | resource managers | Prevent OOM thrash |

### Implementation recipe

1. Keep `vk::EXTMemoryBudgetExtensionName` in `requiredDeviceExtension` (`src/core/vk_device.hpp`). There is no feature struct.
2. Keep `VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT` in `src/core/vk_allocator.hpp` (already set with `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT`, `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT`, `VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT`, BDA, dedicated, bind-memory2). VMA re-queries `vkGetPhysicalDeviceMemoryProperties2` with the budget pNext.
3. Do **not** add a `HardwareCapabilities` budget field unless you want init-time telemetry — budgets go stale. Prefer `vmaGetHeapBudgets` at the point of use.
4. Optional debug: log `vmaGetHeapBudgets` from allocator/device init; optionally surface usage/budget in a HUD.
5. Soft-fail or demote in `vk_resource_manager.cpp` / `texture_manager.cpp` only after measuring. Do not treat budget as a create-time VU.

---

## 6. Concrete integration sketch (telemetry)

```cpp
void logMemoryBudgets(VmaAllocator allocator, uint32_t heapCount) {
    std::vector<VmaBudget> budgets(heapCount);
    vmaGetHeapBudgets(allocator, budgets.data());
    for (uint32_t i = 0; i < heapCount; ++i) {
        log_info(std::format("Heap {}: usage={} / budget={}",
            i, budgets[i].usage, budgets[i].budget), "MemoryBudget");
    }
}
```

Do not cache budget as a one-shot at device create if using for decisions — refresh.

---

## 7. Validation / test checklist

1. Device creates with `VK_EXT_memory_budget` enabled.
2. VMA allocator creates successfully with budget bit.
3. `vmaGetHeapBudgets` returns non-zero budgets on real GPUs.
4. Allocations under pressure still behave (may fail with OOM — ensure errors are handled).
5. No validation errors when chaining budget properties to memory properties2.

---

## 8. Shader interaction

**None (direct).** This extension is host / queue / memory / presentation / resource plumbing only. It does not add shader stages, builtins, storage classes, or SPIR-V capabilities. Shaders continue to use whatever programmable stages the active pipeline or shader objects declare; this feature only changes how the host prepares or submits work those shaders run on.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| `VK_KHR_get_physical_device_properties2` / Vulkan 1.1+ | Property query chain (core for this project) |
| VMA | Primary consumer via `VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT` |
| `VK_EXT_memory_priority` | Priority hints under contention (sibling, not consumed) |
| `VK_EXT_pageable_device_local_memory` | OS pageable heaps; budget still meaningful |

---

## 10. Non-goals / do not do

- Do not invent a feature bit for this extension.
- Do not treat `heapBudget` as a hard Vulkan limit — it is a guidance signal.
- Do not remove the VMA budget flag without a reason; it is low-cost enablement.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is Memory Budget enabled? | **Yes** — extension listed; no feature struct |
| Is it used? | **Partially** — VMA budget tracking only |
| Highest-value next steps | Expose/log `vmaGetHeapBudgets` for debugging |
| Secondary steps | HUD / soft allocation policies |
