# VK_KHR_maintenance4 — Implementation Spec

**Status in this repo: PROMOTED TO CORE (Vulkan 1.3), ENABLED via `VkPhysicalDeviceVulkan13Features::maintenance4 = true`. Properties cached as `HardwareCapabilities::maintenance4`. VMA created with `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT`. NOT listed as a device extension.**

Official sources:

- [VK_KHR_maintenance4](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_maintenance4.html)
- [Proposal](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_KHR_maintenance4.adoc)
- [VkPhysicalDeviceVulkan13Features](https://docs.vulkan.org/spec/latest/chapters/features.html#VkPhysicalDeviceVulkan13Features) (`maintenance4`)
- [VkPhysicalDeviceMaintenance4Properties](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMaintenance4Properties.html)
- [vkGetDeviceBufferMemoryRequirements](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetDeviceBufferMemoryRequirements.html)
- [vkGetDeviceImageMemoryRequirements](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetDeviceImageMemoryRequirements.html)

> Note: Promoted to **Vulkan 1.3 core**. Enable only through `VkPhysicalDeviceVulkan13Features` (or rely on always-on 1.3 feature support). **Never** add `VK_KHR_maintenance4` to `requiredDeviceExtension`.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | **Absent by design** (promoted; comment documents 1.3 set) |
| Feature enable | `src/core/vk_device.cpp` → `createLogicalDevice()` | `Vulkan13Features{ .maintenance4 = true }` |
| Property query | `src/core/vk_device.cpp` → properties chain | `vk::PhysicalDeviceMaintenance4Properties` queried |
| Capability cache | `src/core/types.hpp` → `HardwareCapabilities::maintenance4` | Cached (`maxBufferSize`) |
| VMA | `src/core/vk_allocator.hpp` | `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT` set |
| API usage | resource / shader paths | Indirect via VMA; no direct `getDevice*MemoryRequirements` call sites |

### Already wired

```cpp
// src/core/vk_device.cpp (feature chain)
// vk::PhysicalDeviceVulkan13Features
{.synchronization2 = true,
 .textureCompressionASTC_HDR = false,
 .dynamicRendering = true,
 .maintenance4 = true},

// src/core/vk_device.cpp (properties chain)
capabilities.maintenance4 =
    propertiesChain.get<vk::PhysicalDeviceMaintenance4Properties>();

// src/core/types.hpp
vk::PhysicalDeviceMaintenance4Properties maintenance4;

// src/core/vk_allocator.hpp
allocatorInfo.flags = /* ... */ | VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT | /* ... */;
allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_4;
```

### Not wired yet (optional consumption)

- Explicit buffer size gating against `maintenance4.maxBufferSize`
- Pre-create memory requirement queries via `getDeviceBufferMemoryRequirements` / `getDeviceImageMemoryRequirements` / `getDeviceImageSparseMemoryRequirements`
- SPIR-V `LocalSizeId` / workgroup size specialization using maintenance4 relaxations (if compute paths grow)

---

## 2. What the extension provides

Maintenance4 is a **cumulative maintenance bag**: small, independent fixes promoted wholesale into Vulkan 1.3 (`VkPhysicalDeviceVulkan13Features::maintenance4`). It is **not a replacement** for earlier maintenance (1.1 maintenance1–3 / `HardwareCapabilities::maintenance3`); later bags (5–10) add more fixes on top. Enable the core 1.3 bit only — do not re-list `VK_KHR_maintenance4`.

| Feature | New API / bit | Engine relevance |
| --- | --- | --- |
| Max buffer size property | `maxBufferSize` | Hard upper bound for buffer creates (beyond `maxMemoryAllocationSize` / heap) |
| Memory requirements without objects | `vkGetDeviceBufferMemoryRequirements`, `vkGetDeviceImageMemoryRequirements`, `vkGetDeviceImageSparseMemoryRequirements` | Size/align planning before `vkCreateBuffer` / `vkCreateImage`; VMA uses this path when MAINTENANCE4 bit is set |
| Local size id (SPIR-V) | `LocalSizeId` / related workgroup sizing | Compute specialization without recompiling fixed local size |
| Pipeline create consistency | binding / push-constant / interface clarifications | Safer pipeline create validation |
| Image/buffer create relaxations | various VU relaxations | Fewer artificial create-time restrictions |

**Still valid modern API.** Prefer the device-level memory-requirements entry points over inventing throwaway buffers/images solely for `vkGetBufferMemoryRequirements`.

---

## 3. Structures and commands

### 3.1 Feature (enable at device create — core 1.3)

```cpp
// Inside VkPhysicalDeviceVulkan13Features (core)
typedef struct VkPhysicalDeviceVulkan13Features {
    // ...
    VkBool32 maintenance4;
    // ...
} VkPhysicalDeviceVulkan13Features;
```

**Already enabled** as `maintenance4 = true`. Keep forced on for this engine (required behavior for VMA maintenance4 flag).

### 3.2 Properties

```cpp
typedef struct VkPhysicalDeviceMaintenance4Properties {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES
    void*           pNext;
    VkDeviceSize    maxBufferSize;
} VkPhysicalDeviceMaintenance4Properties;
```

| Property | Meaning |
| --- | --- |
| `maxBufferSize` | Maximum size for any `VkBuffer` create on this device |

**Already cached** as `HardwareCapabilities::maintenance4`.

### 3.3 Device-level memory requirements (preferred modern path)

```cpp
void vkGetDeviceBufferMemoryRequirements(
    VkDevice                                    device,
    const VkDeviceBufferMemoryRequirements*     pInfo,
    VkMemoryRequirements2*                      pMemoryRequirements);

void vkGetDeviceImageMemoryRequirements(
    VkDevice                                    device,
    const VkDeviceImageMemoryRequirements*      pInfo,
    VkMemoryRequirements2*                      pMemoryRequirements);

void vkGetDeviceImageSparseMemoryRequirements(
    VkDevice                                    device,
    const VkDeviceImageMemoryRequirements*      pInfo,
    uint32_t*                                   pSparseMemoryRequirementCount,
    VkSparseImageMemoryRequirements2*           pSparseMemoryRequirements);
```

Vulkan-Hpp pattern:

```cpp
vk::BufferCreateInfo bufferCi{ /* size, usage, ... */ };
vk::DeviceBufferMemoryRequirements info{ .pCreateInfo = &bufferCi };
auto reqs = device.getBufferMemoryRequirements(info); // or getDeviceBufferMemoryRequirements
```

Valid usage highlights:

- Create-info must be a legal create (usage bits, size ≤ `maxBufferSize`, etc.)
- Does **not** create a buffer/image; safe for probing multiple size/usage combinations
- Prefer over temporary create → query → destroy when choosing memory types / sizes

Related (older, still valid but object-bound):

- `vkGetBufferMemoryRequirements2` / `vkGetImageMemoryRequirements2` (1.1+; need live objects)

### 3.4 Interaction with VMA

`VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT` tells VMA it may use maintenance4-style APIs and assumptions (including device buffer/image requirements without temporary objects where beneficial). This project sets the bit and `vulkanApiVersion = VK_API_VERSION_1_4`, which is correct for core 1.3+ maintenance4.

Do **not** remove the bit while `maintenance4 = true` remains required.

---

## Limits

See [Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html) and [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html#limits-required).

| Limit | Required (spec) | This repo | Notes |
| --- | --- | --- | --- |
| `maxBufferSize` | min **2^30** bytes (same magnitude as maintenance3 `maxMemoryAllocationSize`) | Cached `HardwareCapabilities::maintenance4` via `VkPhysicalDeviceMaintenance4Properties` | Gates `vkCreateBuffer` size |

`LocalSizeId` is a SPIR-V workgroup-size relaxation from this bag, not a numeric device limit. Do not invent additional minima.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep **out** of `requiredDeviceExtension` (comment already lists Maintenance4 under 1.3 promotions) |
| `src/core/vk_device.cpp` | Keep `Vulkan13Features::maintenance4 = true` + property query |
| `src/core/types.hpp` | Keep `HardwareCapabilities::maintenance4` |
| `src/core/vk_allocator.hpp` | Keep `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT` |

### 4.2 Buffer size gating (recommended hardening)

**Files:** `src/util/vk_utils.cpp` (`createBuffer`), `src/core/vk_resource_manager.cpp`, `src/core/vk_allocator.hpp` helpers

Before large buffer creates (vertex/index/staging/UBO heaps):

```cpp
const auto maxBuf = device.getCapabilities().maintenance4.maxBufferSize;
if (size > maxBuf) {
    throw std::runtime_error("buffer size exceeds maintenance4.maxBufferSize");
}
```

`maxBufferSize` is often very large on desktop; the check is still the correct portable gate.

### 4.3 Pre-create memory planning (optional)

When adding allocators, sparse textures, or size-class caches that need memory type bits **before** object create:

```cpp
// Prefer device-level query (maintenance4 / core 1.3)
vk::DeviceBufferMemoryRequirements bufReqInfo{ .pCreateInfo = &bufferCreateInfo };
vk::MemoryRequirements2 memReqs =
    device.getBufferMemoryRequirements(bufReqInfo);
// use memReqs.memoryRequirements.size / alignment / memoryTypeBits
```

Same for images with `DeviceImageMemoryRequirements` + `getImageMemoryRequirements`.

Today VMA owns most of this; only call directly for custom non-VMA paths.

### 4.4 Compute local size (future)

If compute shaders use specialization of workgroup size via `LocalSizeId`, document the SPIR-V requirement and specialization-constant mapping in the shader pipeline. No current compute main path depends on this.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep core enablement + VMA bit | `vk_device.*`, `types.hpp`, `vk_allocator.hpp` | Already correct |
| P1 | Optional log of `maxBufferSize` at init | `vk_device.cpp` | Debuggability next to other capability dumps |
| P2 | Assert large buffer creates against `maxBufferSize` | `vk_utils.cpp` / resource paths | Fail fast on invalid sizes |
| P3 | Direct `getDevice*MemoryRequirements` for custom allocators | future allocator code | Only if bypassing VMA |

### Implementation recipe

1. Confirm `src/core/types.hpp` → `HardwareCapabilities` caches `maintenance3`, `maintenance4`, `maintenance5`, `maintenance6`, `maintenance7`, `maintenance9`, and `maintenance10`. There is **no `maintenance8` properties field**.
2. Confirm `src/core/vk_device.cpp` property chain includes `PhysicalDeviceMaintenance3Properties`–`Maintenance7PropertiesKHR`, `Maintenance9PropertiesKHR`, and `Maintenance10PropertiesKHR` (skip 8). Feature bits: `Vulkan13Features::maintenance4 = true`, `Vulkan14Features::maintenance5`/`maintenance6`, plus `Maintenance7/8/10FeaturesKHR`; `maintenance9` is currently forced **false** (VVL #12449) while its properties are still queried.
3. Confirm `src/core/vk_allocator.hpp` already sets `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT` and `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT` (no VMA bits for 6–10). Keep the MAINTENANCE4 bit aligned with `maintenance4 = true`.
4. Log `maintenance4.maxBufferSize` at device init next to other capability dumps.
5. Gate large `vkCreateBuffer` / VMA buffer creates against `maxBufferSize`; use `getDevice*MemoryRequirements` only on custom (non-VMA) paths.

---

## 6. Concrete integration sketch

```cpp
// Hardening only — placement near createBuffer / VMA allocate
const VkDeviceSize maxBufferSize =
    /* HardwareCapabilities */.maintenance4.maxBufferSize;

if (bufferInfo.size > maxBufferSize) {
    throw std::runtime_error(
        std::format("VkBuffer size {} exceeds maxBufferSize {}",
                    bufferInfo.size, maxBufferSize));
}
```

Do **not** invent a second feature path or re-enable `VK_KHR_maintenance4` as an extension name.

---

## 7. Validation / test checklist

1. Device creates with `maintenance4 = true` in the 1.3 feature struct (no extension string).
2. Capability dump shows `maintenance4.maxBufferSize`.
3. VMA allocator create succeeds with `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT`.
4. Validation layers: no “extension not enabled” for maintenance4 entry points.
5. If size gate added: oversize buffer create fails with a clear error before Vulkan create.

---

## 8. Shader interaction

**Limited / toolchain.** Maintenance4 relaxations include SPIR-V `LocalSizeId` style workgroup sizing (specialize compute/mesh local size). No new shading stages. Current `mesh.slang` uses literal `numthreads(32,1,1)`.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Vulkan 1.3 core | Promotion home; feature bit lives in `Vulkan13Features` |
| VMA | `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT` requires maintenance4 available |
| `VK_KHR_maintenance5` / core 1.4 maintenance5 | Further buffer/image create and binding helpers; already enabled via `Vulkan14Features` in this repo |
| `vkGet*MemoryRequirements2` (1.1) | Object-bound alternative; device-level queries preferred when create-info is known first |
| Buffer device address (1.2) | Large BDA buffers still bounded by `maxBufferSize` |

---

## 10. Non-goals / do not do

- Do **not** add `VK_KHR_maintenance4` to `requiredDeviceExtension`.
- Do **not** set `maintenance4 = false` while VMA’s MAINTENANCE4 bit remains set.
- Do **not** document this as requiring a KHR device extension on Vulkan 1.3+ targets.
- Do **not** create temporary buffers solely for memory requirements if device-level queries suffice.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| How is Maintenance4 enabled? | **Core 1.3** — `VkPhysicalDeviceVulkan13Features::maintenance4 = true` |
| Device extension listed? | **No** (correct) |
| Properties? | **Yes** — `HardwareCapabilities::maintenance4` (`maxBufferSize`) |
| Consumed? | **Indirectly** via VMA MAINTENANCE4 bit; no direct `getDevice*MemoryRequirements` call sites |
| Still valid? | **Yes** — preferred modern memory-requirements and buffer-limit path |
| Highest-value next steps | Log/gate `maxBufferSize`; keep VMA bit aligned with feature |
