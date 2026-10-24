# VK_EXT_pageable_device_local_memory — Implementation Spec

**Status in this repo: ENABLED (device extension + feature), NOT CONSUMED — no `vkSetDeviceMemoryPriorityEXT` usage yet.**

Official sources:

- [VK_EXT_pageable_device_local_memory](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_pageable_device_local_memory.html)
- [VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT.html)
- [vkSetDeviceMemoryPriorityEXT](https://docs.vulkan.org/refpages/latest/refpages/source/vkSetDeviceMemoryPriorityEXT.html)

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::EXTPageableDeviceLocalMemoryExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → feature chain | `pageableDeviceLocalMemory = true` |
| Property query | — | **None** (feature + command only) |
| Capability cache | `src/core/types.hpp` | **No** field |
| API usage | allocator / resources | **None** |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::EXTPageableDeviceLocalMemoryExtensionName,

// src/core/vk_device.cpp (feature chain)
// vk::PhysicalDevicePageableDeviceLocalMemoryFeaturesEXT
{.pageableDeviceLocalMemory = true},
```

### Not wired yet

- `vkSetDeviceMemoryPriorityEXT` on `VkDeviceMemory` handles
- Policy for adjusting priority when resources become hot/cold
- VMA integration if exposing underlying `VkDeviceMemory` for priority updates
- Coordination with allocate-time `VK_EXT_memory_priority`

---

## 2. What the extension provides

Allows device-local memory to be treated as **pageable** by the OS/driver under oversubscription, and exposes **dynamic** priority updates on existing `VkDeviceMemory` objects so the app can influence what stays resident.

Allocate-time `VK_EXT_memory_priority` cannot mark pages cold after a stream-out. This remains a **device extension** (not promoted). Enabling `pageableDeviceLocalMemory` lets the OS treat `DEVICE_LOCAL` as pageable under oversubscription.

It does **not** add numeric limits and does **not** guarantee that priority `1.0` keeps pages resident. Paging behavior is OS/driver-defined.

| Feature | New API / bit | Engine relevance |
| --- | --- | --- |
| Feature | `pageableDeviceLocalMemory` | Enables pageable behavior + command |
| Command | `vkSetDeviceMemoryPriorityEXT` | Runtime priority 0.0–1.0 |

Differs from `VK_EXT_memory_priority` (fixed at allocate via pNext).

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
typedef struct VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT
    void*           pNext;
    VkBool32        pageableDeviceLocalMemory;
} VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT;
```

**Already enabled.**

### 3.2 Command

```cpp
void vkSetDeviceMemoryPriorityEXT(
    VkDevice       device,
    VkDeviceMemory memory,
    float          priority); // 0.0 .. 1.0 inclusive
```

Vulkan-Hpp:

```cpp
device.setDeviceMemoryPriorityEXT(memory, 0.25f);
```

Call when residency importance changes (e.g. texture streamed in/out, level unload). Safe to call multiple times; last value wins.

---

## Limits

No property struct. This extension does **not** add new numeric Required Limits ([Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html)).

| Item | Constraint |
| --- | --- |
| `vkSetDeviceMemoryPriorityEXT` `priority` | `float` in **[0.0, 1.0]** inclusive (same range as allocate-time memory priority) |
| Outside `[0.0, 1.0]` | Validation error |
| Scope | Per `VkDeviceMemory` object, not per VMA suballocation |
| `DEVICE_LOCAL` | OS **may page** it when this feature is enabled |

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep extension name |
| `src/core/vk_device.cpp` | Keep feature true |

### 4.2 Memory handle access — prerequisite

**Files:** `src/core/vk_allocator.hpp`, VMA allocation info paths

VMA owns `VkDeviceMemory`. To set priority:

```cpp
VmaAllocationInfo info{};
vmaGetAllocationInfo(allocator, allocation, &info);
if (info.deviceMemory != VK_NULL_HANDLE) {
    device.setDeviceMemoryPriorityEXT(info.deviceMemory, priority);
}
```

Caveat: multiple allocations may share one `VkDeviceMemory` (suballocation). Priority is **per device memory object**, not per VMA allocation — prefer dedicated allocations for resources that need independent priority (VMA dedicated flag already used for descriptor heaps).

### 4.3 Policy consumers — NOT STARTED

| Consumer | File | Behavior |
| --- | --- | --- |
| Textures | `texture_manager.cpp` | Raise priority on bind/use; lower on eviction |
| Render targets | resource manager | Keep high while in frame graph |
| Staging | upload paths | Low priority |

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep enablement as-is | `vk_device.*` | Already correct |
| P1 | Helper `setAllocationPriority(VmaAllocation, float)` | `vk_allocator.hpp` | Single call site |
| P1 | High priority for heaps + main RTs | descriptors / resources | Protect critical memory |
| P2 | Texture hot/cold adjustments | `texture_manager.cpp` | Streaming wins |
| P2 | Pair with allocate-time memory priority | both priority EXTs | Consistent policy |
| P3 | Budget-driven demotion | with memory budget | When usage ≈ budget |

### Implementation recipe

1. Keep `vk::EXTPageableDeviceLocalMemoryExtensionName` and `{.pageableDeviceLocalMemory = true}` (`src/core/vk_device.hpp`, `vk_device.cpp`). No `HardwareCapabilities` field — there is no property struct.
2. VMA with `VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT` in `src/core/vk_allocator.hpp` already applies allocation `priority` (and may call `vkSetDeviceMemoryPriorityEXT`). That covers **initial** dynamic priority; keep the flag next to `EXT_MEMORY_BUDGET`, `MAINTENANCE4`, `MAINTENANCE5`.
3. Add a helper `setAllocationPriority(VmaAllocation, float)` via `vmaGetAllocationInfo` → `device.setDeviceMemoryPriorityEXT` for later hot/cold changes. Clamp to `[0.0, 1.0]`.
4. Prefer dedicated allocations when independent priority is required (descriptor heaps already use dedicated). Suballocs share one `VkDeviceMemory`.
5. `texture_manager.cpp`: raise priority on bind/use, lower on eviction. Attachments stay high via existing `allocInfo.priority = 1.0f` in `vk_resource_manager.cpp`.
6. Combine with `vmaGetHeapBudgets` (`VK_EXT_memory_budget`) to demote when usage approaches budget.

---

## 6. Concrete integration sketch

```cpp
void setAllocationMemoryPriority(const vk::raii::Device& device, VmaAllocator allocator,
                                 VmaAllocation allocation, float priority) {
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(allocator, allocation, &info);
    if (info.deviceMemory) {
        device.setDeviceMemoryPriorityEXT(info.deviceMemory, std::clamp(priority, 0.f, 1.f));
    }
}

// After creating MSAA color image:
setAllocationMemoryPriority(device, allocator, colorAlloc, 1.0f);

// When a texture is unevicted from a far cascade:
setAllocationMemoryPriority(device, allocator, texAlloc, 0.2f);
```

---

## 7. Validation / test checklist

1. Device creates with extension + feature true.
2. `setDeviceMemoryPriorityEXT` with 0.0 / 0.5 / 1.0 succeeds.
3. Out-of-range priority fails validation.
4. Shared `VkDeviceMemory` suballocs: changing priority affects all suballocs — document/test.
5. No crashes when called on host-visible non-device-local memory (behavior may be no-op or valid per spec — only use on intended device-local pages).

---

## 8. Shader interaction

**None (direct).** This extension is host / queue / memory / presentation / resource plumbing only. It does not add shader stages, builtins, storage classes, or SPIR-V capabilities. Shaders continue to use whatever programmable stages the active pipeline or shader objects declare; this feature only changes how the host prepares or submits work those shaders run on.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| `VK_EXT_memory_priority` | Allocate-time initial priority |
| `VK_EXT_memory_budget` | When to demote pages |
| VMA dedicated vs suballoc | Priority granularity |
| OS memory manager | Actual paging behavior is implementation-defined |

---

## 10. Non-goals / do not do

- Do not assume setting priority 1.0 prevents all paging.
- Do not call priority updates every frame for every allocation without need (driver cost).
- Do not set independent priorities on suballocations that share one `VkDeviceMemory` expecting isolation.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is Pageable Device Local Memory enabled? | **Yes** — extension + `pageableDeviceLocalMemory = true` |
| Is it used? | **No** — no `vkSetDeviceMemoryPriorityEXT` |
| Highest-value next steps | Allocator helper + protect heaps/RTs |
| Secondary steps | Texture hot/cold; budget-driven demotion |
