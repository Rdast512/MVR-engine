# VK_KHR_bind_memory2 + VK_KHR_get_memory_requirements2 — Implementation Spec

**Status in this repo: PROMOTED TO CORE (Vulkan 1.1), ALWAYS AVAILABLE at 1.1+. Still valid modern memory path — required by VMA dedicated allocation / bind-memory2 flags and by many later extensions.**

Official sources:

- [VK_KHR_bind_memory2](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_bind_memory2.html) (historical)
- [VK_KHR_get_memory_requirements2](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_get_memory_requirements2.html) (historical)
- [vkBindBufferMemory2](https://docs.vulkan.org/refpages/latest/refpages/source/vkBindBufferMemory2.html) / [vkBindImageMemory2](https://docs.vulkan.org/refpages/latest/refpages/source/vkBindImageMemory2.html)
- [vkGetBufferMemoryRequirements2](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetBufferMemoryRequirements2.html) / [vkGetImageMemoryRequirements2](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetImageMemoryRequirements2.html)
- [VkMemoryDedicatedRequirements](https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryDedicatedRequirements.html) / [VkMemoryDedicatedAllocateInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryDedicatedAllocateInfo.html)

> **PROMOTED TO CORE — do not list as device extensions.** At Vulkan 1.1+, the `*2` memory requirement and bind entry points are core. This project targets Vulkan 1.4; do **not** put `VK_KHR_bind_memory2` or `VK_KHR_get_memory_requirements2` in `requiredDeviceExtension` (already noted under “Promoted to 1.1” in `vk_device.hpp`).

> Combined file: both promotions form one modern memory stack. Prefer `*2` APIs (and pNext chains) over 1.0-only `vkBindBufferMemory` / `vkGetBufferMemoryRequirements` when writing new code — though VMA usually owns the bind path.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` | **Must not list** — promoted |
| Feature enable | — | **N/A** (always available 1.1+) |
| VMA flags | `src/core/vk_allocator.hpp` | `VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT` + `VMA_ALLOCATOR_CREATE_KHR_BIND_MEMORY2_BIT` |
| VMA API version | same | `VK_API_VERSION_1_4` |
| Dedicated alloc usage | `vk_descriptors.cpp` heaps | `VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT` on heap buffers |
| Direct `bindMemory2` calls in app | mostly via VMA | Engine relies on VMA rather than raw bind |

### Already wired

```cpp
// src/core/vk_allocator.hpp
allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT |
    VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT |
    VMA_ALLOCATOR_CREATE_KHR_BIND_MEMORY2_BIT |
    VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT |
    VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT |
    VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT;
allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_4;
```

Dedicated memory example:

```cpp
// src/core/vk_descriptors.cpp — descriptor heap buffers
createBuffer(..., VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);
```

---

## 2. What core provides

Extensible **pNext** forms of memory requirements and bind operations, plus **dedicated allocation** interaction (prefer dedicated memory for certain images/buffers).

Vulkan 1.0 `vkGet*MemoryRequirements` / `vkBind*Memory` have no `pNext` chain, so dedicated-allocation hints and batched binds were impossible. Both extensions are **promoted to Vulkan 1.1 core** and are always available here; VMA consumes them via `VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT` and `VMA_ALLOCATOR_CREATE_KHR_BIND_MEMORY2_BIT`.

This does **not** add extra numeric limits or a feature bit. It does **not** replace core `VkMemoryRequirements` (size/alignment/type bits) or later maintenance4 device-level queries. New engine code should not go back to 1.0-only bind/req entry points.


| Capability | API | Engine relevance |
| --- | --- | --- |
| Requirements v2 | `vkGetBuffer/ImageMemoryRequirements2` | pNext: dedicated requirements, plane requirements, etc. |
| Bind v2 | `vkBindBuffer/ImageMemory2` | Batch binds; pNext: dedicated allocate info linkage |
| Dedicated allocation | `VkMemoryDedicatedRequirements` / `VkMemoryDedicatedAllocateInfo` | Heaps, large images, RT scratch, etc. |
| VMA integration | dedicated + bind memory2 create flags | **Active** in this repo |

**Still valid modern path.** Many later extensions and VMA features **require** the `*2` entry points. This is not obsolete 1.0 baggage.

**Baseline vs action:** always-on at 1.1+; app action is “use VMA/`*2`,” not “enable an extension.”

---

## 3. Structures and commands

### 3.1 Memory requirements 2

```cpp
typedef struct VkBufferMemoryRequirementsInfo2 {
    VkStructureType    sType;
    const void*        pNext;
    VkBuffer           buffer;
} VkBufferMemoryRequirementsInfo2;

typedef struct VkMemoryRequirements2 {
    VkStructureType         sType;
    void*                   pNext; // e.g. VkMemoryDedicatedRequirements
    VkMemoryRequirements    memoryRequirements;
} VkMemoryRequirements2;

void vkGetBufferMemoryRequirements2(VkDevice device,
    const VkBufferMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemoryRequirements);
// analogous: vkGetImageMemoryRequirements2
```

### 3.2 Bind memory 2

```cpp
typedef struct VkBindBufferMemoryInfo {
    VkStructureType    sType;
    const void*        pNext;
    VkBuffer           buffer;
    VkDeviceMemory     memory;
    VkDeviceSize       memoryOffset;
} VkBindBufferMemoryInfo;

VkResult vkBindBufferMemory2(VkDevice device, uint32_t bindInfoCount,
    const VkBindBufferMemoryInfo* pBindInfos);
// analogous: vkBindImageMemory2
```

### 3.3 Dedicated allocation (common pNext)

```cpp
typedef struct VkMemoryDedicatedRequirements {
    VkStructureType    sType;
    void*              pNext;
    VkBool32           prefersDedicatedAllocation;
    VkBool32           requiresDedicatedAllocation;
} VkMemoryDedicatedRequirements;

typedef struct VkMemoryDedicatedAllocateInfo {
    VkStructureType    sType;
    const void*        pNext;
    VkImage            image;  // XOR buffer
    VkBuffer           buffer;
} VkMemoryDedicatedAllocateInfo;
```

VMA’s dedicated and bind-memory2 flags wire these under the hood when `vulkanApiVersion` is set appropriately.

---

## Limits

There are **no extra limits** from bind_memory2 / get_memory_requirements2. Use **core** memory requirements plus dedicated-allocation pNext.

| Limit / property | Spec-required (when feature supported) | Constraint | Engine implication |
| --- | --- | --- | --- |
| `VkMemoryRequirements::{size,alignment,memoryTypeBits}` | implementation-dependent — query `*2` / VMA | alignment is a power of two | VMA already queries via `*2` |
| `prefersDedicatedAllocation` / `requiresDedicatedAllocation` | behavioral, per-resource | pNext on `VkMemoryRequirements2` | Heaps already force dedicated (`vk_descriptors.cpp`) |
| `maxMemoryAllocationCount` (core) | min **4096** | `properties2.limits` | Allocation *count* cap |
| `maxMemoryAllocationSize` (maintenance3) | min **2^30** bytes | `HardwareCapabilities::maintenance3` | Allocation *size* cap |
| `maxBufferSize` (maintenance4) | min **2^30** | `HardwareCapabilities::maintenance4` | Buffer create size cap |
| `minUniformBufferOffsetAlignment` / `minStorageBufferOffsetAlignment` | power of two | implementation-dependent — query `properties2.limits` | Do not hardcode |
| `nonCoherentAtomSize` | power of two | implementation-dependent — query | Host-visible flush granularity |

Sources: [limits](https://docs.vulkan.org/spec/latest/chapters/limits.html), [Required Limits](https://docs.vulkan.org/refpages/latest/refpages/source/Required_Limits.html).

---

## 4. Where this is used in the codebase

### 4.1 VMA — DONE (primary consumer)

| File | Role |
| --- | --- |
| `src/core/vk_allocator.hpp` | Enables dedicated + bind memory2 at allocator create |
| All `vmaCreateBuffer` / `vmaCreateImage` call sites | Implicit `*2` path |

### 4.2 Explicit dedicated heaps

| File | Role |
| --- | --- |
| `src/core/vk_descriptors.cpp` | Descriptor heap buffers use dedicated memory bit |

### 4.3 New code policy

- Prefer VMA for bind/alloc (already standard in engine).
- If raw Vulkan alloc/bind is ever needed: use `*2` + pNext, not 1.0-only entry points.
- Do **not** reintroduce extension strings for bind/requirements2.

---

## 5. Recommended implementation order

| Priority | Change | Why |
| --- | --- | --- |
| P0 | Keep VMA dedicated + bind memory2 flags | Required modern path |
| P0 | Never list as device extensions | Promoted to 1.1 |
| P1 | Use dedicated alloc for large/unique resources when beneficial | Heaps already do; AS/images may follow |
| P2 | Avoid new 1.0-only bindMemory call sites | Consistency + extension pNext |

### Implementation recipe

1. **Enable** — API 1.4 implies core `*2`. Never list `VK_KHR_bind_memory2` or `VK_KHR_get_memory_requirements2` in `requiredDeviceExtension` (`vk_device.hpp` 1.1 promoted block). Keep VMA flags in `vk_allocator.hpp`: `VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT` + `VMA_ALLOCATOR_CREATE_KHR_BIND_MEMORY2_BIT` with `vulkanApiVersion = VK_API_VERSION_1_4`.
2. **Query** — prefer VMA; if raw, use `getBufferMemoryRequirements2` / `getImageMemoryRequirements2` with `VkMemoryDedicatedRequirements` in pNext. Do not add a dedicated `HardwareCapabilities` field for this pair.
3. **Cache** — no extra property struct. Reuse `properties2.limits`, `maintenance3.maxMemoryAllocationSize`, and `maintenance4.maxBufferSize`.
4. **Gate** — keep `VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT` on descriptor-heap buffers (`vk_descriptors.cpp`). Respect `kMinDedicatedAllocationBytes` in `vk_allocator.hpp` for small buffers. New non-VMA paths must use `bindBufferMemory2` / `bindImageMemory2`.
5. **Record** — no command-buffer step. Bind happens at resource create (VMA), not in `recordCommandBuffer`.

---

## 6. Integration sketch (VMA — current pattern)

```cpp
VmaAllocatorCreateInfo allocatorInfo = {};
allocatorInfo.flags =
    VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT |
    VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT |
    VMA_ALLOCATOR_CREATE_KHR_BIND_MEMORY2_BIT |
    /* ... */;
allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_4;

// Per-allocation:
VmaAllocationCreateInfo allocInfo = {};
allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT; // when needed
```

Raw bind sketch (only if bypassing VMA):

```cpp
vk::BindBufferMemoryInfo bindInfo{
    .buffer = *buffer,
    .memory = *memory,
    .memoryOffset = offset
};
device.bindBufferMemory2(bindInfo);
```

---

## 7. Validation / test checklist

1. No `VK_KHR_bind_memory2` / `VK_KHR_get_memory_requirements2` in device extension list.
2. VMA creates successfully with dedicated + bind memory2 flags on 1.4.
3. Dedicated heap buffers allocate and map cleanly.
4. No validation errors preferring 1.0 bind when pNext dedicated info is required.

---

## 8. Shader interaction

**None (direct).** This extension is host / queue / memory / presentation / resource plumbing only. It does not add shader stages, builtins, storage classes, or SPIR-V capabilities. Shaders continue to use whatever programmable stages the active pipeline or shader objects declare; this feature only changes how the host prepares or submits work those shaders run on.

## 9. Dependencies and interactions

| Sibling | Interaction |
| --- | --- |
| Vulkan 1.1+ core | Promotion home |
| VMA | Primary engine consumer |
| Buffer device address | Separate feature; often combined on same buffers |
| Maintenance4/5 VMA flags | Additional VMA create bits already set |
| Acceleration structures / large images | Often want dedicated memory via this stack |

---

## 10. Non-goals / do not do

- Do **not** list either extension name on 1.1+ targets.
- Do **not** prefer exclusive 1.0 `vkBindBufferMemory` / `vkGetBufferMemoryRequirements` for new code when `*2` is core.
- Do not remove VMA bind-memory2 / dedicated flags without a replacement that still uses core `*2` APIs.
- Do not document these as optional “extensions to enable” in modern device lists.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Promotion | **Vulkan 1.1 core** (always available at 1.1+) |
| Enable path | API version; VMA flags for practical use |
| Consumed? | **Yes** — via VMA (+ dedicated heaps) |
| Still modern? | **Yes** — required by VMA and many extensions |
| Device extension strings? | **Never** on this 1.4 target |
