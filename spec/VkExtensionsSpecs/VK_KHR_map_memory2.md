# VK_KHR_map_memory2 — Implementation Spec

**Status in this repo: PROMOTED TO CORE (project targets Vulkan 1.4; treat as core-available — see version note). NOT listed as a device extension. Engine maps memory through VMA (`vmaMapMemory` / `vmaUnmapMemory`); no direct `vkMapMemory2` / `vkUnmapMemory2` call sites. Still valid extensible mapping API.**

Official sources:

- [VK_KHR_map_memory2](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_map_memory2.html)
- [vkMapMemory2](https://docs.vulkan.org/refpages/latest/refpages/source/vkMapMemory2.html) / [vkUnmapMemory2](https://docs.vulkan.org/refpages/latest/refpages/source/vkUnmapMemory2.html)
- [VkMemoryMapInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryMapInfo.html)
- [VkMemoryUnmapInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryUnmapInfo.html)

### Version note

Khronos history places **map_memory2** promotion with the **Vulkan 1.4** core set in many references; this repository’s `vk_device.hpp` comment also lists it among 1.3 promotions. For **this project** (`vulkanApiVersion = VK_API_VERSION_1_4`, required API 1.4 features already enabled):

- Treat `vkMapMemory2` / `vkUnmapMemory2` as **core-available**.
- **Never** add `VK_KHR_map_memory2` to `requiredDeviceExtension`.
- Do not gate on a separate device extension string when running at 1.4.

If a future build ever targeted only 1.3 without the KHR extension, re-verify promotion/extension availability for that exact apiVersion.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` | **Absent by design** (promoted; mentioned in 1.3 list comment) |
| Explicit feature bit | `Vulkan13Features` / `Vulkan14Features` | **None** required for base map2 entry points on core targets |
| Direct API usage | engine | **None** — no `mapMemory2` / `unmapMemory2` |
| VMA mapping | `vk_resource_manager.cpp`, `texture_manager.cpp`, `vk_descriptors.cpp`, `vk_camera.cpp`, `object_storage.cpp` | **`vmaMapMemory` / `vmaUnmapMemory`** |
| VMA API version | `src/core/vk_allocator.hpp` | `VK_API_VERSION_1_4` — VMA may use map2 internally when beneficial |

### Already wired (indirect)

```cpp
// Typical staging upload pattern (resource manager / textures)
void* data = nullptr;
vmaMapMemory(allocator.allocator, stagingBufferMemory, &data);
// memcpy ...
vmaUnmapMemory(allocator.allocator, stagingBufferMemory);
```

### Not wired yet

- Direct `vkMapMemory2` with `VkMemoryMapInfo` pNext chains
- `VK_MEMORY_UNMAP_RESERVE_BIT_EXT`-style reserved unmap behaviors if ever needed via pNext
- Non-VMA raw device memory mapping (engine standardizes on VMA)

---

## 2. What the extension provides

Map memory 2 replaces the fixed-argument `vkMapMemory` / `vkUnmapMemory` with **info structs** that can grow via `pNext` (flags, future map modes, reserved unmap, etc.).

Classic `vkMapMemory` cannot grow via `pNext`. On this project treat map-memory2 as **core-available** (Vulkan 1.3/1.4; never list `VK_KHR_map_memory2`).

It does **not** add new numeric limits and does **not** relax host-visibility or alignment. Mapped ranges must still honor core `minMemoryMapAlignment` and, when non-coherent, `nonCoherentAtomSize`. Mapping still requires `HOST_VISIBLE` memory.

| Feature | API | Engine relevance |
| --- | --- | --- |
| Extensible map | `vkMapMemory2` + `VkMemoryMapInfo` | Preferred if mapping outside VMA |
| Extensible unmap | `vkUnmapMemory2` + `VkMemoryUnmapInfo` | Pair with map2 |
| Classic map | `vkMapMemory` / `vkUnmapMemory` | Still legal; VMA abstracts both |

**Still valid.** For application code that already uses VMA, continuing to call `vmaMapMemory` is correct; VMA selects underlying entry points based on allocator flags and `vulkanApiVersion`.

---

## 3. Structures and commands

### 3.1 Map / unmap 2

```cpp
typedef struct VkMemoryMapInfo {
    VkStructureType    sType; // VK_STRUCTURE_TYPE_MEMORY_MAP_INFO
    const void*        pNext;
    VkMemoryMapFlags   flags;
    VkDeviceMemory     memory;
    VkDeviceSize       offset;
    VkDeviceSize       size;
} VkMemoryMapInfo;

typedef struct VkMemoryUnmapInfo {
    VkStructureType    sType; // VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO
    const void*        pNext;
    VkMemoryUnmapFlags flags;
    VkDeviceMemory     memory;
} VkMemoryUnmapInfo;

VkResult vkMapMemory2(
    VkDevice                   device,
    const VkMemoryMapInfo*     pMemoryMapInfo,
    void**                     ppData);

VkResult vkUnmapMemory2(
    VkDevice                   device,
    const VkMemoryUnmapInfo*   pMemoryUnmapInfo);
```

Vulkan-Hpp sketch:

```cpp
vk::MemoryMapInfo mapInfo{
    .flags = {},
    .memory = deviceMemory,
    .offset = 0,
    .size = vk::WholeSize};
void* ptr = device.mapMemory2(mapInfo);

vk::MemoryUnmapInfo unmapInfo{.memory = deviceMemory};
device.unmapMemory2(unmapInfo);
```

### 3.2 Relationship to classic map

| API | Extensible? | Typical use |
| --- | --- | --- |
| `vkMapMemory` / `vkUnmapMemory` | No | Legacy / simple |
| `vkMapMemory2` / `vkUnmapMemory2` | Yes (`pNext`) | New direct maps |
| `vmaMapMemory` / `vmaUnmapMemory` | Via VMA | **This engine’s standard** |

---

## Limits

No new numeric limits from this promotion ([Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html)). Respect the existing core limits on mapped host-visible ranges:

| Core limit | Constraint | Engine implication |
| --- | --- | --- |
| `minMemoryMapAlignment` | Power of two | Map `offset` must be a multiple; VMA handles this for engine allocations |
| `nonCoherentAtomSize` | Power of two | Flush/invalidate ranges on non-coherent host-visible memory must be multiples |

Both live in `VkPhysicalDeviceLimits` (`HardwareCapabilities::properties2.properties.limits`). Commands: `vkMapMemory2` / `vkUnmapMemory2` with `VkMemoryMapInfo` / `VkMemoryUnmapInfo`.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE (implicit core)

Do not list the extension. Ensure API version remains 1.4 so core map2 is available to the loader/VMA.

### 4.2 Prefer VMA for engine allocations — DONE

**Files using VMA map today:**

| File | Role |
| --- | --- |
| `src/core/vk_resource_manager.cpp` | Staging vertex/index, UBO maps |
| `src/core/texture_manager.cpp` | Staging texture upload |
| `src/core/vk_descriptors.cpp` | Descriptor heap host maps |
| `src/scene/vk_camera.cpp` | Camera UBO unmap on teardown |
| `src/core/object_storage.cpp` | Per-object UBO maps |

**Policy:** keep mapping through VMA unless a raw `VkDeviceMemory` path is intentionally non-VMA.

### 4.3 Direct map2 (only if needed)

Use `mapMemory2` when:

- Mapping non-VMA memory  
- Needing a map/unmap pNext chain not exposed by VMA  
- Writing a diagnostic tool that must avoid VMA  

Place such helpers next to allocator code (`vk_allocator.hpp` / utils), not scattered in renderers.

### 4.4 Host-visible allocation flags

Ensure host-visible allocations still set VMA host-access flags (already done in `createBuffer` for host-visible paths). Map2 does not remove the need for `HOST_VISIBLE` memory.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep VMA + API 1.4 | `vk_allocator.hpp` | Correct indirect path |
| P1 | No mandatory migration | — | VMA map is sufficient |
| P2 | Optional thin wrapper `mapMemory2` for non-VMA | util/allocator | Only if raw memory appears |
| P3 | Document any pNext map flags if vendors require | future | Spec extensions on map info |

### Implementation recipe

1. Keep `vulkanApiVersion = VK_API_VERSION_1_4` in `src/core/vk_allocator.hpp`. Do **not** add `VK_KHR_map_memory2` to `requiredDeviceExtension`. VMA already has `EXT_MEMORY_BUDGET`, `MAINTENANCE4`, `MAINTENANCE5`.
2. Keep mapping through `vmaMapMemory` / `vmaUnmapMemory` in `vk_resource_manager.cpp` (vertex/meshlet/UBO staging), `texture_manager.cpp` (texture staging), `vk_descriptors.cpp`, `vk_camera.cpp`, `object_storage.cpp`. VMA may call map2 internally at 1.4.
3. Do not migrate those call sites to raw `mapMemory2` unless a non-VMA `VkDeviceMemory` or a map-info `pNext` is required.
4. If adding a raw helper, place it next to `vk_allocator.hpp` and still honor `minMemoryMapAlignment` / `nonCoherentAtomSize`.
5. For some image uploads, prefer core 1.4 host image copy (`HardwareCapabilities::hostImageCopy`) over stage + map.

---

## 6. Concrete integration sketch (non-VMA)

```cpp
// Only for raw VkDeviceMemory — prefer VMA in this engine
vk::MemoryMapInfo mapInfo{
    .memory = mem,
    .offset = offset,
    .size = size};
void* data = (*device).mapMemory2(mapInfo);
// ... write ...
(*device).unmapMemory2(vk::MemoryUnmapInfo{.memory = mem});
```

Staging upload via VMA (current preferred style):

```cpp
void* data = nullptr;
vmaMapMemory(allocator, allocation, &data);
std::memcpy(data, src, bytes);
vmaUnmapMemory(allocator, allocation);
```

---

## 7. Validation / test checklist

1. No `VK_KHR_map_memory2` in the device extension list.  
2. Staging uploads and UBO updates succeed under validation.  
3. If calling `mapMemory2` directly: memory is host-visible; range within bound size; not double-mapped.  
4. VMA allocator create with `VK_API_VERSION_1_4` succeeds.  
5. Unmap pairs with map (no leak of persistent maps unless intentionally persistent-mapped via VMA flags).

---

## 8. Shader interaction

**None (direct).** This extension is host / queue / memory / presentation / resource plumbing only. It does not add shader stages, builtins, storage classes, or SPIR-V capabilities. Shaders continue to use whatever programmable stages the active pipeline or shader objects declare; this feature only changes how the host prepares or submits work those shaders run on.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| VMA | Primary consumer of mapping; may use map2 under the hood at 1.4 |
| Maintenance4 / buffer size limits | Mapped buffer sizes still constrained by create limits |
| Host image copy (1.4) | Alternative for some image uploads without staging maps |
| Buffer device address | Device addresses are not host maps; separate path |
| Coherent vs non-coherent memory | Flushes/invalidates still required when non-coherent |

---

## 10. Non-goals / do not do

- Do **not** add `VK_KHR_map_memory2` to `requiredDeviceExtension` on the 1.4 target.  
- Do **not** force a rewrite of all `vmaMapMemory` call sites to raw `mapMemory2`.  
- Do **not** map memory that is not host-visible.  
- Do **not** mix unmap APIs incorrectly (map with map2, unmap with 1.0 unmap on the same design — pick one stack; VMA owns pairing when using VMA).

---

## 11. Summary

| Question | Answer |
| --- | --- |
| How available? | **Core on this 1.4-targeted project** (see version note) |
| Device extension? | **No** (correct) |
| Direct consumption? | **No** — VMA map/unmap throughout |
| Still valid? | **Yes** — preferred extensible map API for raw memory |
| Highest-value next steps | Keep VMA; use `mapMemory2` only for non-VMA or pNext needs |
