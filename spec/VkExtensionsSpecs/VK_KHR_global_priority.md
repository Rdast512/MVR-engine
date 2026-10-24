# VK_KHR_global_priority — Implementation Spec

**Status in this repo: PROMOTED TO CORE (Vulkan 1.4), ENABLED via Vulkan14Features::globalPriorityQuery, NOT CONSUMED**

Official sources:

- [VK_KHR_global_priority](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_global_priority.html)
- [Proposal / feature materials](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_global_priority.html)
- [VkPhysicalDeviceVulkan14Features](https://docs.vulkan.org/spec/latest/chapters/features.html) (`globalPriorityQuery`)
- [VkDeviceQueueGlobalPriorityCreateInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkDeviceQueueGlobalPriorityCreateInfo.html)
- [VkQueueFamilyGlobalPriorityProperties](https://docs.vulkan.org/refpages/latest/refpages/source/VkQueueFamilyGlobalPriorityProperties.html)

> **Promoted to core:** Queue global priority create/query functionality is available as core Vulkan 1.4 APIs (KHR names remain aliases). Prefer core / unprefixed names.  
> **Do not** list `VK_KHR_global_priority` in `requiredDeviceExtension`.  
> Note: `src/core/vk_device.hpp` comments historically group some promotions under 1.3; **this feature is enabled via `Vulkan14Features::globalPriorityQuery`** in `vk_device.cpp` — treat 1.4 as the enablement source of truth for this repo.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | **Must not be listed** — promoted; covered by 1.4 features |
| Feature enable | `src/core/vk_device.cpp` → feature chain | `vk::PhysicalDeviceVulkan14Features{ .globalPriorityQuery = true }` |
| Property query | per-queue-family properties | **Not cached** on `HardwareCapabilities` (optional: query `QueueFamilyGlobalPriorityProperties` when implementing) |
| Capability cache | `src/core/types.hpp` | **No** dedicated global-priority field |
| API usage | device queue create | **None** — `DeviceQueueCreateInfo` uses only local `pQueuePriorities` (float), no global priority pNext |

### Already wired

```cpp
// src/core/vk_device.cpp — vk::PhysicalDeviceVulkan14Features
{.globalPriorityQuery = true,
 .dynamicRenderingLocalRead = true,
 .maintenance5 = true,
 .maintenance6 = true,
 .hostImageCopy = true},
```

### Queue create today (no global priority)

```cpp
// src/core/vk_device.cpp — createLogicalDevice()
std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos;
queueCreateInfos.push_back(
    {.queueFamilyIndex = graphicsIndex, .queueCount = 1, .pQueuePriorities = &queuePriority});
// present / transfer / compute similarly — no DeviceQueueGlobalPriorityCreateInfo
```

### Not wired yet

- Per-queue-family global priority property query
- `VkDeviceQueueGlobalPriorityCreateInfo` on graphics / transfer / compute / present queues
- Policy for `LOW` / `MEDIUM` / `HIGH` / `REALTIME` selection
- Handling of denied realtime priorities (fallback)

---

## 2. What global priority provides

Vulkan’s per-queue `pQueuePriorities` floats only order work **within** an application. Global priority expresses intent **across** applications/processes to the system compositor/scheduler (where supported). Promoted to **Vulkan 1.4** (`globalPriorityQuery`); do **not** list `VK_KHR_global_priority`.

Enabling the query does **not** change scheduling — queues still need `DeviceQueueGlobalPriorityCreateInfo`. This repo queries only. `REALTIME` is not guaranteed and may need OS privileges.

| Piece | Role |
| --- | --- |
| `globalPriorityQuery` feature | Allows querying which global priorities a queue family supports |
| `VkQueueFamilyGlobalPriorityProperties` | Lists supported `VkQueueGlobalPriority` values for a family |
| `VkDeviceQueueGlobalPriorityCreateInfo` | Requests a global priority at queue creation (pNext of `VkDeviceQueueCreateInfo`) |
| Priority enum | `LOW`, `MEDIUM` (default), `HIGH`, `REALTIME` (core names; `*_EXT`/`*_KHR` aliases exist) |

Engines that care about background asset streaming vs interactive frame production can map:

- Graphics / present → `MEDIUM` or `HIGH`
- Async transfer / background compute → `LOW`
- Time-critical audio/VR-style work → `REALTIME` only when supported and justified

This project currently creates queues without global priority — implementations use the default global priority behavior.

---

## 3. Structures and commands

### 3.1 Feature — CORE via Vulkan 1.4

```cpp
// Inside VkPhysicalDeviceVulkan14Features
VkBool32 globalPriorityQuery;
```

**Already enabled:** `globalPriorityQuery = true`.

Enabling the query feature does **not** by itself change queue scheduling; consumption requires create-info pNext (and optionally reading family properties first).

### 3.2 Queue-family properties

```cpp
typedef struct VkQueueFamilyGlobalPriorityProperties {
    VkStructureType           sType; // VK_STRUCTURE_TYPE_QUEUE_FAMILY_GLOBAL_PRIORITY_PROPERTIES
    void*                     pNext;
    uint32_t                  priorityCount;
    VkQueueGlobalPriority     priorities[VK_MAX_GLOBAL_PRIORITY_SIZE];
} VkQueueFamilyGlobalPriorityProperties;
```

Query by chaining into `vkGetPhysicalDeviceQueueFamilyProperties2` for each family of interest (graphics, transfer, compute).

### 3.3 Queue create info

```cpp
typedef struct VkDeviceQueueGlobalPriorityCreateInfo {
    VkStructureType          sType; // VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO
    const void*              pNext;
    VkQueueGlobalPriority    globalPriority;
} VkDeviceQueueGlobalPriorityCreateInfo;
```

Vulkan-Hpp sketch:

```cpp
vk::DeviceQueueGlobalPriorityCreateInfo gp{
    .globalPriority = vk::QueueGlobalPriority::eMedium
};
vk::DeviceQueueCreateInfo qci{
    .pNext            = &gp,
    .queueFamilyIndex = graphicsIndex,
    .queueCount       = 1,
    .pQueuePriorities = &queuePriority
};
```

If an unsupported priority is requested, device creation can fail — always intersect with queried family priorities, especially for `eRealtime`.

## Limits

Feature: `globalPriorityQuery` (already `true`). Official required-limits chapter: [limits](https://docs.vulkan.org/spec/latest/chapters/limits.html).

| Limit / query | Min / notes |
| --- | --- |
| `discreteQueuePriorities` (core) | **≥ 2** — intra-app float `pQueuePriorities` levels, not the global enum |
| Global priority levels | `LOW` / `MEDIUM` / `HIGH` / `REALTIME` |
| `VkQueueFamilyGlobalPriorityProperties` | `priorityCount` + `priorities[]` — query per family; do **not** assume `REALTIME` |

`REALTIME` may be missing or require privileges. No other numeric Required Limits for the global-priority enum set.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — feature DONE, queues NOT CONSUMED

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep global priority **out** of `requiredDeviceExtension` |
| `src/core/vk_device.cpp` | Keep `globalPriorityQuery = true`; **add** optional create-info + queries when consuming |
| `src/core/types.hpp` | Optional: cache per-family supported priorities if multi-place policy is needed |

### 4.2 Implementation site — `createLogicalDevice()`

**File:** `src/core/vk_device.cpp`  
**Region:** construction of `queueCreateInfos` for graphics / present / transfer / compute

Steps:

1. For each unique queue family index used, query `QueueFamilyGlobalPriorityProperties` via `getQueueFamilyProperties2` structure chain (the file already uses property chains for queue families in places — extend similarly).
2. Choose priorities:

| Queue role | Suggested default | Notes |
| --- | --- | --- |
| Graphics + present | `eMedium` or `eHigh` | Interactive frames |
| Dedicated transfer | `eLow` | Background uploads |
| Dedicated compute | `eMedium` / `eLow` | Depends on whether compute is frame-critical |
| Realtime | **only if** listed in family props and product requirements demand it | May require privileges on some OSes |

3. Chain `DeviceQueueGlobalPriorityCreateInfo` on each `DeviceQueueCreateInfo`.
4. Keep float `pQueuePriorities` as today for intra-app ordering.

### 4.3 Failure handling

```cpp
// Prefer:
const auto supported = /* priorities[] for this family */;
vk::QueueGlobalPriority chosen = pickSupported(desired, supported);
// Never request eRealtime unless present in supported and explicitly configured.
```

If device creation must remain portable, default to `eMedium` when uncertain.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep `globalPriorityQuery = true`; do not list extension | `vk_device.*` | Already correct |
| P1 | Query per-family global priority props for families you create | `vk_device.cpp` | Safe selection |
| P1 | Chain create info: graphics/present MEDIUM/HIGH, transfer LOW | `vk_device.cpp` | First real consumption |
| P2 | Engine setting / config for priority policy | settings / device | Product control without recompiles |
| P3 | Cache supported priorities on `HardwareCapabilities` | `types.hpp` | Only if reused outside device init |

### Implementation recipe

1. Keep `globalPriorityQuery = true`. Never add the extension string.
2. Do not change the swapchain/present path. Create still prefers FIFO_LATEST_READY else FIFO, image count `max(3, min)` clamped (`src/core/vk_swapchain.cpp`); acquire/present in `src/render/vk_renderer.cpp`.
3. When consuming: query `QueueFamilyGlobalPriorityProperties` per used family; pick a listed level; chain `DeviceQueueGlobalPriorityCreateInfo`. Default `MEDIUM` if uncertain. Never request `REALTIME` unless listed and required.
4. Suggested map: graphics/present `MEDIUM`/`HIGH`, transfer `LOW`.
5. present_timing features stay **false**. present_id2 + calibrated_timestamps remain unused deps. Instance WSI: `KHR_surface`, `KHR_get_surface_capabilities2`, `KHR_surface_maintenance1`.

---

## 6. Concrete integration sketch

```cpp
// Per family used in queueCreateInfos:
using QFamilyChain = vk::StructureChain<
    vk::QueueFamilyProperties2,
    vk::QueueFamilyGlobalPriorityProperties>;

auto families = physicalDevice.getQueueFamilyProperties2<QFamilyChain>();

auto pick = [](const vk::QueueFamilyGlobalPriorityProperties& props,
               vk::QueueGlobalPriority desired) {
    const auto* b = props.priorities.data();
    const auto* e = b + props.priorityCount;
    if (std::find(b, e, desired) != e)
        return desired;
    return vk::QueueGlobalPriority::eMedium; // safe default if supported; else first entry
};

vk::DeviceQueueGlobalPriorityCreateInfo graphicsGp{
    .globalPriority = pick(
        families[graphicsIndex].get<vk::QueueFamilyGlobalPriorityProperties>(),
        vk::QueueGlobalPriority::eHigh)
};

float queuePriority = 1.0f;
vk::DeviceQueueCreateInfo graphicsQci{
    .pNext            = &graphicsGp,
    .queueFamilyIndex = graphicsIndex,
    .queueCount       = 1,
    .pQueuePriorities = &queuePriority
};
// Repeat for transfer with eLow, etc. Lifetime: gp structs must outlive device create.
```

Adjust to the exact Vulkan-Hpp overloads present in the project’s SDK.

---

## 7. Validation / test checklist

1. Device creates with `globalPriorityQuery = true` and **without** the extension string in the list.
2. Without create-info pNext: behavior remains as today (still valid).
3. With create-info: only priorities present in family properties are requested.
4. Transfer-low / graphics-high policy does not break present or validation layers.
5. Realtime path (if ever added): fails gracefully when unsupported or OS-denied.
6. Multi-family devices: each family queried independently (do not assume same set).

---

## 8. Shader interaction

**None (direct).** This extension is host / queue / memory / presentation / resource plumbing only. It does not add shader stages, builtins, storage classes, or SPIR-V capabilities. Shaders continue to use whatever programmable stages the active pipeline or shader objects declare; this feature only changes how the host prepares or submits work those shaders run on.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Vulkan 1.4 `globalPriorityQuery` | Feature enablement for querying supported priorities |
| Queue family discovery in `Device` | Must run before priority selection |
| Async transfer / compute queues | Best place to apply `LOW` vs interactive `HIGH` |
| OS / driver privileges | Realtime may be restricted outside the Vulkan API |

---

## 10. Non-goals / do not do

- **Do not** add `VK_KHR_global_priority` to `requiredDeviceExtension`.
- Do not request `REALTIME` by default.
- Do not confuse float `pQueuePriorities` with global priority — both can be set; they solve different problems.
- Do not assume enablement alone changes scheduling without create-info consumption.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| How is global priority enabled? | **Core Vulkan 1.4** — `Vulkan14Features::globalPriorityQuery = true` |
| Properties cached? | **No** app-level cache yet (query at use) |
| Is it used? | **No** — queues created without `DeviceQueueGlobalPriorityCreateInfo` |
| Highest-value next steps | Query family props; set HIGH/MEDIUM for graphics, LOW for transfer |
| Extension string? | **Never list** in `requiredDeviceExtension` |
