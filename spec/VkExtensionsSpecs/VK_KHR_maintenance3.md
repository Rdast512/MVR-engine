# VK_KHR_maintenance3 — Implementation Spec

**Status in this repo: PROMOTED TO CORE (Vulkan 1.1), ALWAYS AVAILABLE at 1.1+; properties cached as `maintenance3`. Still valid limits.**

Official sources:

- [VK_KHR_maintenance3](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_maintenance3.html) (historical extension name)
- [VkPhysicalDeviceMaintenance3Properties](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMaintenance3Properties.html)
- [vkGetDescriptorSetLayoutSupport](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetDescriptorSetLayoutSupport.html)

> **PROMOTED TO CORE — do not list as a device extension.** At Vulkan 1.1+, maintenance3 is core. This project targets Vulkan 1.4; do **not** put `VK_KHR_maintenance3` in `requiredDeviceExtension` (already documented under “Promoted to 1.1” in `vk_device.hpp`).

> **No feature bit** — always available at 1.1+. Query **properties** only.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` | **Must not list** — promoted (Maintenance1–3 note) |
| Feature enable | — | **N/A** (always available 1.1+) |
| Property query | `src/core/vk_device.cpp` → properties chain | `vk::PhysicalDeviceMaintenance3Properties` queried |
| Capability cache | `src/core/types.hpp` → `HardwareCapabilities::maintenance3` | Cached |
| Layout support query | app code | Optional API available; use when building large set layouts |

### Already wired

```cpp
// src/core/vk_device.cpp
capabilities.maintenance3 =
    propertiesChain.get<vk::PhysicalDeviceMaintenance3Properties>();

// src/core/types.hpp
vk::PhysicalDeviceMaintenance3Properties maintenance3;
```

---

## 2. What core provides

Maintenance3 adds **implementation limits** and a query to test whether a descriptor set layout is supported, avoiding hard failures on oversized layouts.

Vulkan 1.0 provided no portable per-set descriptor ceiling and no way to test a layout before `vkCreateDescriptorSetLayout`. Maintenance3 supplies those limits plus `vkGetDescriptorSetLayoutSupport`. It is **promoted to Vulkan 1.1 core** — always on at 1.1+, **no feature bit**, properties only.

This does **not** replace later maintenance (4+), descriptor indexing, or heaps. It does **not** need a device-extension string on this 1.4 target. Ignoring the two limits still fails create/allocate on large sets and huge allocations.


| Item | Meaning | Engine relevance |
| --- | --- | --- |
| `maxPerSetDescriptors` | Max descriptors in a single set | Legacy set / large bindless layouts |
| `maxMemoryAllocationSize` | Max size of a single memory allocation | Huge buffers/images; VMA/device memory planning |
| `vkGetDescriptorSetLayoutSupport` | Query layout support before create | Safe layout construction |

**Still valid limits** — not obsolete. Later maintenance extensions are **cumulative**; they do not remove these limits or replace the need to respect them.

This is mostly **baseline plumbing + limit queries**: little continuous app action beyond caching limits and optionally probing layouts.

---

## 3. Structures and commands

### 3.1 Properties (already cached)

```cpp
typedef struct VkPhysicalDeviceMaintenance3Properties {
    VkStructureType    sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES
    void*              pNext;
    uint32_t           maxPerSetDescriptors;
    VkDeviceSize       maxMemoryAllocationSize;
} VkPhysicalDeviceMaintenance3Properties;
```

| Property | Use |
| --- | --- |
| `maxPerSetDescriptors` | Cap bindings/descriptors when designing set layouts (legacy path) |
| `maxMemoryAllocationSize` | Reject or split allocations above limit; guide streaming heaps |

### 3.2 Layout support query

```cpp
void vkGetDescriptorSetLayoutSupport(
    VkDevice                                    device,
    const VkDescriptorSetLayoutCreateInfo*      pCreateInfo,
    VkDescriptorSetLayoutSupport*               pSupport);
```

`pSupport->supported` indicates whether the layout can be created on this device.

Vulkan-Hpp: `device.getDescriptorSetLayoutSupport(...)`.

---

## Limits

Properties live in `VkPhysicalDeviceMaintenance3Properties`, already cached as `HardwareCapabilities::maintenance3`. Spec minima apply whenever the device is Vulkan 1.1+ (always on here).

| Limit / property | Spec-required (when feature supported) | Constraint | Engine implication |
| --- | --- | --- | --- |
| `maxPerSetDescriptors` | min **1024** | implementation-dependent above floor — query `maintenance3` | Cap legacy set layouts; probe with `getDescriptorSetLayoutSupport` |
| `maxMemoryAllocationSize` | min **2^30** bytes (1 GiB) | implementation-dependent above floor | Reject or split allocs above the queried value; VMA planning |
| `maxMemoryAllocationCount` (core) | min **4096** | `properties2.limits` | Separate from allocation *size* |
| `maxBoundDescriptorSets` (core) | min **4** | `properties2.limits` | Complementary set-count cap |
| `maxBufferSize` (maintenance4) | min **2^30** | `HardwareCapabilities::maintenance4` | Buffer *create* size vs allocation size — both apply |

Sources: [limits](https://docs.vulkan.org/spec/latest/chapters/limits.html), [Required Limits](https://docs.vulkan.org/refpages/latest/refpages/source/Required_Limits.html).

---

## 4. Where to use in this codebase

### 4.1 Device bootstrap — DONE

Keep property chain entry and `HardwareCapabilities::maintenance3`.

### 4.2 Consumers

| Consumer | How |
| --- | --- |
| Descriptor set layouts (legacy) | Ensure total descriptors ≤ `maxPerSetDescriptors`; probe with `getDescriptorSetLayoutSupport` for aggressive bindless sets |
| Large buffers / images | Compare size to `maxMemoryAllocationSize` before allocate |
| Descriptor heaps | Heap path is less set-bound, but allocation size limit still applies to heap buffers |

**Files:** `src/core/vk_descriptors.*`, `src/core/vk_resource_manager.cpp`, `src/core/vk_allocator.hpp` / VMA paths.

---

## 5. Recommended implementation order

| Priority | Change | Why |
| --- | --- | --- |
| P0 | Keep cache; never list extension | Correct 1.1+ posture |
| P1 | Log `maxPerSetDescriptors` / `maxMemoryAllocationSize` at init | Debuggability |
| P2 | Guard huge allocations against `maxMemoryAllocationSize` | Hard failure avoidance |
| P2 | Layout support query for large legacy sets | Safer bindless fallback |

### Implementation recipe

1. **Enable** — no feature bit and no extension string. Keep `VK_KHR_maintenance3` out of `requiredDeviceExtension` (`vk_device.hpp` “Promoted to 1.1”).
2. **Query** — keep `vk::PhysicalDeviceMaintenance3Properties` on the `getProperties2` chain in `vk_device.cpp`.
3. **Cache** — keep `capabilities.maintenance3 = propertiesChain.get<vk::PhysicalDeviceMaintenance3Properties>()` into `HardwareCapabilities::maintenance3` (`types.hpp`). Optionally log `maxPerSetDescriptors` / `maxMemoryAllocationSize` at init.
4. **Gate** — before large `vmaCreateBuffer` / image allocs (`vk_allocator.hpp`, `vk_resource_manager.cpp`), compare size to `device.getCapabilities().maintenance3.maxMemoryAllocationSize`. For aggressive legacy layouts (`vk_descriptors.*`), call `device.getDescriptorSetLayoutSupport` and stay ≤ `maxPerSetDescriptors`.
5. **Record** — no extra command-buffer work. Layout and alloc gating are host-side only.

---

## 6. Integration sketch

```cpp
const auto& m3 = device.getCapabilities().maintenance3;

if (requestedAllocSize > m3.maxMemoryAllocationSize) {
    // split, sparse, or error
}

vk::DescriptorSetLayoutSupport support = device.getDescriptorSetLayoutSupport(layoutInfo);
if (!support.supported) {
    // reduce array sizes / split sets
}
```

---

## 7. Validation / test checklist

1. Properties populated; no extension string in device list.
2. Pathological oversized set layout either fails support query or create — handle gracefully.
3. Allocation near `maxMemoryAllocationSize` behaves per driver (no silent overflow assumptions).

---

## 8. Shader interaction

**Indirect.** Raised descriptor limits / property queries. Set-based shaders with large arrays must stay within maintenance3 / per-set limits. Heap path is less sensitive to classic per-set limits.

## 9. Dependencies and interactions

| Sibling | Interaction |
| --- | --- |
| Maintenance1/2 (1.1) | Earlier cumulative fixes — see `Vulkan_1_1_maintenance_baseline.md` |
| Maintenance4+ | Later limits/features; **do not remove** maintenance3 queries |
| Descriptor indexing | Large sets still bounded by `maxPerSetDescriptors` and indexing properties |
| VMA | Allocation size planning |

---

## 10. Non-goals / do not do

- Do **not** re-list as a device extension on 1.1+.
- Do not assume later maintenance replaces these two limits.
- Do not skip limit checks for “heap-only” engines — memory size limit still matters.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Promotion | **Vulkan 1.1 core** (always available at 1.1+) |
| Enable path | Properties only |
| Consumed? | **Cached**; optional layout/alloc gating recommended |
| Still modern? | **Yes** — still-valid limits |
| Device extension string? | **Never** on this 1.4 target |
