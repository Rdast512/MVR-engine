# VK_NV_partitioned_acceleration_structure — Implementation Spec

**Status in this repo: ENABLED (device extension + feature + properties cached), NOT CONSUMED — optional NVIDIA path, not baseline RT.**

Official sources:

- [VK_NV_partitioned_acceleration_structure](https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_partitioned_acceleration_structure.html)
- NVIDIA / Khronos documentation for partitioned AS
- Features: `VkPhysicalDevicePartitionedAccelerationStructureFeaturesNV`
- Properties: `VkPhysicalDevicePartitionedAccelerationStructurePropertiesNV`

> Vendor extension: **NVIDIA-only**. Portable baseline remains `VK_KHR_acceleration_structure`.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::NVPartitionedAccelerationStructureExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → feature chain | `partitionedAccelerationStructure = true` |
| Property query | `src/core/vk_device.cpp` → properties chain | Queried and assigned |
| Capability cache | `src/core/types.hpp` → `HardwareCapabilities::partitionedAccelerationStructure` | Cached |
| API usage | AS / renderer | **None** |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::NVPartitionedAccelerationStructureExtensionName,

// src/core/vk_device.cpp (feature)
// vk::PhysicalDevicePartitionedAccelerationStructureFeaturesNV
{.partitionedAccelerationStructure = true},

// src/core/vk_device.cpp (properties)
capabilities.partitionedAccelerationStructure =
    propertiesChain.get<vk::PhysicalDevicePartitionedAccelerationStructurePropertiesNV>();

// src/core/types.hpp
vk::PhysicalDevicePartitionedAccelerationStructurePropertiesNV partitionedAccelerationStructure;
```

### Not wired yet

- Partitioned AS build/update APIs (per NVIDIA header)
- Partition/instance management for world streaming
- Optional extension gating for non-NVIDIA devices
- Hook into ray query / RT pipeline scene representation

---

## 2. What the extension provides

Partitioned acceleration structures split large top-level or world structures into **partitions** that can be updated or streamed independently — aimed at large open worlds where a single monolithic TLAS rebuild is too expensive. This is **vendor-only** NVIDIA, not promoted, and **not** a substitute for portable `VK_KHR_acceleration_structure` TLAS rebuild/refit.

This repo caches `HardwareCapabilities::partitionedAccelerationStructure` and currently lists `vk::NVPartitionedAccelerationStructureExtensionName` as a **required** device extension (same portability hazard as cluster AS). Feature is on; **not consumed**. Optionalize before depending on it; keep a KHR TLAS path for tools, multi-vendor, and small scenes.

| Feature | Role | Engine relevance |
| --- | --- | --- |
| Partitioned AS feature | Master enable | Optional large-world path |
| Properties | Partition limits / caps | Design of streaming tiles |
| Build/update commands | NVIDIA partitioned AS API family | Partial world updates |

Exact structs/commands: follow the Vulkan headers vendored/used by this engine.

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
typedef struct VkPhysicalDevicePartitionedAccelerationStructureFeaturesNV {
    VkStructureType sType;
    void*           pNext;
    VkBool32        partitionedAccelerationStructure;
} VkPhysicalDevicePartitionedAccelerationStructureFeaturesNV;
```

**Already enabled:** `partitionedAccelerationStructure = true`.

### 3.2 Properties (cached)

```cpp
// HardwareCapabilities::partitionedAccelerationStructure
// VkPhysicalDevicePartitionedAccelerationStructurePropertiesNV
// Inspect header for maxPartitionCount and related limits
```

### 3.3 Commands

Implement against local headers:

- Create/build/update partitioned structures
- Partition description buffers / flags
- Device address and scratch requirements

Cross-check valid usage with KHR AS memory model (BDA, barriers).

---

## Limits

Vendor extension. Do **not** invent portable Required Limits.

Properties are **implementation-dependent**. Read `HardwareCapabilities::partitionedAccelerationStructure` and the local `vulkan.hpp` fields on `VkPhysicalDevicePartitionedAccelerationStructurePropertiesNV`. The struct’s primary field is `maxPartitionCount`. The spec Required Limits table lists a minimum for that property; **always query the cached value** rather than hardcoding. KHR AS instance/primitive limits still apply to the portable TLAS fallback.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE (aggressive)

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Listed as **required** (multi-vendor concern — same as cluster AS) |
| `src/core/vk_device.cpp` | Feature + properties assigned |
| `src/core/types.hpp` | Properties field present |

### 4.2 Implementation home

| Module | Role |
| --- | --- |
| Future AS / scene streaming module | Partitioned TLAS-like structure per world tile |
| Default path | KHR TLAS rebuild/update |
| `vk_renderer.cpp` | Raster only today — no AS |

Prefer a single scene interface:

```text
ITopLevelStructure → KhrTlas | NvPartitionedAs
```

so ray query / RT pipeline code does not fork everywhere.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | KHR TLAS with refit/rebuild | AS module | Portable |
| P1 | Optionalize NV partitioned extension | `vk_device.*` | Non-NVIDIA device create |
| P2 | Partitioned AS for world tiles | AS + streaming | Large worlds |
| P3 | Balance vs cluster AS (NV) | AS module | Prefer one NV strategy first |

### Implementation recipe

1. Implement KHR TLAS rebuild/refit first (`VK_KHR_acceleration_structure` is enabled, **not consumed**). Partitioned AS is not the portable world structure.
2. Optionalize `NVPartitionedAccelerationStructure` (today it is **required** — device create fails off NVIDIA). Same hazard as cluster AS.
3. Read `device.getCapabilities().partitionedAccelerationStructure.maxPartitionCount` (and any other local `vulkan.hpp` fields). Do not hardcode a Required Limits number.
4. Stream only dirty partitions; record NVIDIA partitioned build/update commands from the headers this repo compiles with.
5. Barrier before `traceRays` / `rayQuery`. Keep a single scene interface (`KhrTlas | NvPartitionedAs`) so shaders do not fork.
6. Prefer one NVIDIA strategy first if both cluster and partitioned AS are in play.

---

## 6. Concrete integration sketch

```cpp
const auto& partProps = device.getCapabilities().partitionedAccelerationStructure;
// Gate partition counts and buffer sizes from partProps

// Streaming tick:
// 1) Update only dirty partitions' instance data
// 2) Record NVIDIA partitioned AS update commands
// 3) Barrier before traceRays / rayQuery
```

Keep KHR TLAS path for tools, multi-vendor, and small scenes.

---

## 7. Validation / test checklist

1. Device creates with partitioned feature on supporting NVIDIA GPU.
2. Properties cached and logged.
3. Partition counts ≤ property limits.
4. Partial updates do not leave stale partitions addressable incorrectly.
5. Trace/query after barrier sees new partition data.
6. Fallback KHR path still works when NV path disabled.

---

## 8. Shader interaction

**Indirect / vendor RT.** Partitioned AS is a build/update organization feature. Portable raygen/hit/ray-query shaders remain KHR-style.

### Engine status

Enabled, not consumed.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| `VK_KHR_acceleration_structure` | Baseline TLAS/BLAS |
| `VK_NV_cluster_acceleration_structure` | Complementary NV geometry clustering |
| RT pipeline / ray query | Consumers of the world structure |
| Scene streaming | Primary motivation for partitions |

---

## 10. Non-goals / do not do

- Do not make partitioned AS the only supported world structure.
- Do not require it for small demo scenes.
- Do not assume AMD/Intel support.
- Do not implement before a correct KHR TLAS update story exists (harder to debug).

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is NV partitioned AS enabled? | **Yes** — extension + feature + properties cached |
| Is it used? | **No** |
| Highest-value next steps | KHR TLAS first; optionalize NV; then partitioned streaming |
| Secondary steps | Co-design with NV cluster AS for end-to-end NVIDIA large-world path |
