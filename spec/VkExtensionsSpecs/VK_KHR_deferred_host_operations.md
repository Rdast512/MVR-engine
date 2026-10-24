# VK_KHR_deferred_host_operations — Implementation Spec

**Status in this repo: ENABLED (device extension), NOT CONSUMED by host AS / deferred-op paths yet.**

Official sources:

- [VK_KHR_deferred_host_operations](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_deferred_host_operations.html)
- [Proposal](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_KHR_deferred_host_operations.adoc)
- [vkCreateDeferredOperationKHR](https://docs.vulkan.org/spec/latest/chapters/deferredhostoperations.html)

> Note: This extension has **no** feature or property struct. Listing the extension name on the device is the enablement.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::KHRDeferredHostOperationsExtensionName` present |
| Feature enable | N/A | No feature struct |
| Property query | N/A | No properties struct |
| Capability cache | `src/core/types.hpp` → `HardwareCapabilities` | **None** (nothing to cache) |
| API usage | renderer / AS / host-build paths | **None** — dependency-only today |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::KHRDeferredHostOperationsExtensionName, // required by KHR_acceleration_structure
```

### Not wired yet

- `vkCreateDeferredOperationKHR` / `vkDestroyDeferredOperationKHR`
- `vkGetDeferredOperationMaxConcurrencyKHR`
- `vkGetDeferredOperationResultKHR`
- `vkDeferredOperationJoinKHR`
- Host-side AS or micromap builds that pass a deferred-operation handle

---

## 2. What the extension provides

Deferred host operations let long-running **host** work (typically acceleration-structure or micromap builds/copies) be split across threads and polled/joined asynchronously. This is a portable KHR dependency of `VK_KHR_acceleration_structure` (and of host opacity-micromap builds). It is **not** promoted into core and has **no** feature or property struct — listing the extension name on the device is the entire enablement.

This repo lists it in `requiredDeviceExtension` **only as the AS dependency**. There is nothing to cache in `HardwareCapabilities`, and no host AS/micromap path consumes `vkCreateDeferredOperationKHR` yet. GPU `vkCmdBuild*` paths must not use deferred ops.

| Feature | New API | Engine relevance |
| --- | --- | --- |
| Deferred op object | `VkDeferredOperationKHR` + create/destroy | Required handle for host AS/micromap builds |
| Concurrency query | `vkGetDeferredOperationMaxConcurrencyKHR` | How many worker threads can usefully join |
| Result poll | `vkGetDeferredOperationResultKHR` | `VK_SUCCESS` / `VK_NOT_READY` / build error |
| Join workers | `vkDeferredOperationJoinKHR` | Thread pool participation in a deferred op |

This engine currently has **no** host AS builds. The extension is listed because **`VK_KHR_acceleration_structure` depends on it**.

---

## 3. Structures and commands

### 3.1 Feature / properties

None. No `VkPhysicalDevice*Features` / `*Properties` for this extension.

### 3.2 Commands

```cpp
VkResult vkCreateDeferredOperationKHR(
    VkDevice                     device,
    const VkAllocationCallbacks* pAllocator,
    VkDeferredOperationKHR*      pDeferredOperation);

void vkDestroyDeferredOperationKHR(
    VkDevice                     device,
    VkDeferredOperationKHR       operation,
    const VkAllocationCallbacks* pAllocator);

uint32_t vkGetDeferredOperationMaxConcurrencyKHR(
    VkDevice               device,
    VkDeferredOperationKHR operation);

VkResult vkGetDeferredOperationResultKHR(
    VkDevice               device,
    VkDeferredOperationKHR operation);

VkResult vkDeferredOperationJoinKHR(
    VkDevice               device,
    VkDeferredOperationKHR operation);
```

Vulkan-Hpp shape:

```cpp
vk::raii::DeferredOperationKHR deferredOp(device);
// or non-RAII:
auto deferredOp = device.createDeferredOperationKHR();

uint32_t maxThreads = device.getDeferredOperationMaxConcurrencyKHR(*deferredOp);
// Worker threads:
device.deferredOperationJoinKHR(*deferredOp);
// Until:
auto result = device.getDeferredOperationResultKHR(*deferredOp);
// result == vk::Result::eSuccess or eNotReady (or error)
```

Valid usage highlights:

- A deferred operation is **single-use** for one host command that accepts `VkDeferredOperationKHR`
- Join may return `VK_THREAD_DONE_KHR` / `VK_THREAD_IDLE_KHR` (implementation-defined scheduling)
- Destroy only after the operation completes (`VK_SUCCESS` or error, not `VK_NOT_READY`)

### 3.3 How AS commands use it

Host variants of AS build/copy take an optional deferred-op handle, e.g.:

```cpp
// Conceptual — when implementing host builds:
vkBuildAccelerationStructuresKHR(
    device,
    deferredOperation, // VK_NULL_HANDLE = blocking; non-null = deferred
    infoCount,
    pInfos,
    ppBuildRangeInfos);
```

Same pattern applies to micromap host builds under `VK_EXT_opacity_micromap`.

---

## Limits

No limits struct and no Required Limits rows. Behavior is join/result codes plus an implementation-dependent concurrency query:

| API | Returns / notes |
| --- | --- |
| `vkCreateDeferredOperationKHR` | Creates a single-use `VkDeferredOperationKHR` |
| `vkDeferredOperationJoinKHR` | May return `VK_THREAD_IDLE_KHR`, `VK_THREAD_DONE_KHR`, `VK_OPERATION_DEFERRED_KHR`, `VK_OPERATION_NOT_DEFERRED_KHR`, or `VK_SUCCESS` |
| `vkGetDeferredOperationResultKHR` | `VK_SUCCESS`, `VK_NOT_READY`, or a build/copy error |
| `vkGetDeferredOperationMaxConcurrencyKHR` | How many threads can usefully join **this** op |

Max concurrent deferred operations on a device is **implementation-dependent** (not a Required Limits row). Query concurrency per operation; do not invent a `HardwareCapabilities` field.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep `vk::KHRDeferredHostOperationsExtensionName` in `requiredDeviceExtension` |
| `src/core/vk_device.cpp` | No feature/property work required |
| `src/core/types.hpp` | No cache field required |

### 4.2 Future consumer — host AS / micromap build helpers

**No current consumer.** When host builds are added, preferred locations:

| File (proposed) | Role |
| --- | --- |
| New helper under `src/core/` or `src/render/` (e.g. `vk_acceleration_structure.cpp`) | Create deferred op, dispatch host build, join pool, destroy |
| Existing thread/job infrastructure (if any) | Workers call `deferredOperationJoinKHR` |
| `src/core/vk_device.hpp` | Device/queues already available; deferred ops are device-scoped |

### 4.3 Do not put this in the frame renderer

**File:** `src/render/vk_renderer.cpp`  
Renderer remains raster dynamic rendering only. Deferred host ops belong in **asset / scene bake / streaming** code paths, not per-frame record.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep extension listed | `vk_device.hpp` | Hard dependency of acceleration structure |
| P1 | RAII helper for deferred ops | new AS utility | Only when first host AS/micromap build lands |
| P2 | Thread-pool join loop using max concurrency | job system / AS utility | Parallel host builds |
| P3 | Optional: log max concurrency once at first use | AS utility | Debuggability |

### Implementation recipe

1. Keep `vk::KHRDeferredHostOperationsExtensionName` listed — required by KHR AS. No feature bit, no `types.hpp` cache.
2. Prefer device builds (`cmdBuildAccelerationStructuresKHR`) for runtime. Only introduce deferred ops with the first **host** AS or micromap build.
3. `createDeferredOperationKHR`, pass the handle into the host build/copy (null handle = blocking).
4. `getDeferredOperationMaxConcurrencyKHR`; join from up to that many workers via `deferredOperationJoinKHR` (expect `THREAD_IDLE` / `THREAD_DONE` / `SUCCESS`).
5. Poll `getDeferredOperationResultKHR` until not `VK_NOT_READY`; destroy only after `VK_SUCCESS` or a real error.
6. Do not call any of this from `vk_renderer.cpp`.

---

## 6. Concrete integration sketch (host AS build)

Illustrative only — no such path exists yet:

```cpp
// When implementing host BLAS build:
vk::raii::DeferredOperationKHR deferred(device.handle());

// Kick host build with deferred handle (API from VK_KHR_acceleration_structure)
device.buildAccelerationStructuresKHR(
    *deferred,
    buildInfos,
    buildRangeInfos);

const uint32_t n = device.getDeferredOperationMaxConcurrencyKHR(*deferred);
// Spawn up to n workers:
//   while (device.deferredOperationJoinKHR(*deferred) == vk::Result::eThreadIdleKHR) { ... }

vk::Result r;
do {
    r = device.getDeferredOperationResultKHR(*deferred);
} while (r == vk::Result::eNotReady);

if (r != vk::Result::eSuccess) {
    // handle build failure
}
// deferred RAII destroys when complete
```

Prefer **device builds** (`vkCmdBuildAccelerationStructuresKHR`) for runtime; use deferred host ops for offline/import bake on CPU when useful.

---

## 7. Validation / test checklist

1. Device creates with `VK_KHR_deferred_host_operations` listed (already required).
2. When host builds exist: create → build with deferred handle → join → result `VK_SUCCESS`.
3. No destroy while result is `VK_NOT_READY`.
4. Max concurrency ≥ 1 on supporting drivers; join from 0..N threads without validation errors.
5. `VK_NULL_HANDLE` deferred op still performs a blocking host build correctly (baseline path).

---

## 8. Shader interaction

**None (direct).** Async host-side AS builds/compiles. Shaders only see completed acceleration structures after builds finish and are synchronized for RT stages.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| `VK_KHR_acceleration_structure` | Spec dependency; host AS commands accept deferred ops |
| `VK_EXT_opacity_micromap` | Host micromap builds can use deferred ops |
| `bufferDeviceAddress` (Vulkan 1.2 features) | Required for AS; already enabled in this repo |
| Device builds (`vkCmdBuild*`) | Do **not** use deferred ops; GPU timeline only |

---

## 10. Non-goals / do not do

- Do not invent a feature bit or properties cache for this extension.
- Do not call deferred-op APIs from the raster frame path in `vk_renderer.cpp`.
- Do not require deferred ops for GPU (`vkCmd*`) AS builds.
- Do not treat this extension as optional while `VK_KHR_acceleration_structure` remains required.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is deferred host operations enabled? | **Yes** — extension name in `requiredDeviceExtension` |
| Is it used? | **No** — dependency-only until host AS/micromap builds exist |
| Highest-value next steps | Keep listed; add deferred-op helper with first host build |
| Secondary steps | Thread-pool join using `getDeferredOperationMaxConcurrencyKHR` |
