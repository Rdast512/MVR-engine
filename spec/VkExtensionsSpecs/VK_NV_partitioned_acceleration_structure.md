# VK_NV_partitioned_acceleration_structure — Implementation Spec

**Status in this repo (2026-11-14): REQUIRED by the engine spec (§10.3), NOT CONSUMED yet.** The device enables it when present and caches its properties, but still treats it as optional; PBR plan C4 makes it required and replaces the per-slot full TLAS build with one PTLAS. Design: [ray_tracing_mega_geometry.md §3.6](../features/planned/ray_tracing_mega_geometry.md).

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
- Required-set entry at device creation (C4)
- Hook into ray query / RT pipeline scene representation

---

## 2. What the extension provides

Partitioned acceleration structures split large top-level or world structures into **partitions** that can be updated or streamed independently — aimed at large open worlds where a single monolithic TLAS rebuild is too expensive. It is NVIDIA-only and not promoted to core. The engine targets NVIDIA RTX only (engine spec §1.2), and the PTLAS is its TLAS.

This repo requires the extension and its `partitionedAccelerationStructure` feature at device creation (PBR C4) and caches `HardwareCapabilities::partitionedAccelerationStructure` (`maxPartitionCount` 16,777,215 on the RTX 5070). `RayTracingScene` builds and updates the engine's one PTLAS.

| Feature | Role | Engine relevance |
| --- | --- | --- |
| Partitioned AS feature | Master enable | the engine TLAS (required, C4) |
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
| `src/core/vk_device.cpp` | In `kRequiredDeviceExtensions` and `kRequiredFeatures` (C4); `maxPartitionCount` logged |
| `src/core/vk_device.cpp` | Feature + properties assigned |
| `src/core/types.hpp` | Properties field present |

### 4.2 Implementation home

| Module | Role |
| --- | --- |
| `src/render/vk_rt_scene.*` (`RayTracingScene`) | one PTLAS updated in place, its scratch and state rows, the per-slot write lists and op entries (C4, built) |
| `shaders/passes/rt_instances.slang` | `WRITE_INSTANCE` arguments for changed rows, appended to the op's `argCount` |
| `shaders/passes/rt_instances.slang` | writes the `WRITE_INSTANCE` ops (all rows after a layout change, changed rows otherwise) and the op count |
| `GpuFrameRoot.tlas` | the PTLAS address, traced as a raw address like the TLAS before it |

---

## 5. Recommended implementation order

PBR plan C4 and feature-spec step RT9 hold the order and the done-when. In short:

1. Move the extension into the required set.
2. Size one PTLAS from `vkGetPartitionedAccelerationStructuresBuildSizesNV` with the instance capacity, the grid's partition count and the global-partition capacity, within `maxPartitionCount` (VUID-10535).
3. Write ops on the GPU, build indirectly in place (src = dst, allowed by VUID-10549), after a wait on the previous frame's last ray pass.
4. Add the OMM 2-state ray flag to every ray: PTLAS has no 2-state instance flag.
5. Measure update and trace cost against the full TLAS build.

---

## 6. Concrete integration sketch

See [ray_tracing_mega_geometry.md §3.6](../features/planned/ray_tracing_mega_geometry.md) (API summary, engine mapping, frames in flight).

---

## 7. Validation / test checklist

1. Device creates with partitioned feature on supporting NVIDIA GPU.
2. Properties cached and logged.
3. Partition counts ≤ property limits.
4. Partial updates do not leave stale partitions addressable incorrectly.
5. Trace/query after barrier sees new partition data.
6. `ENGINE_RT_CHECK` matches the vis buffer on every frame, as with the full TLAS build.

---

## 8. Shader interaction

**Indirect / vendor RT.** Partitioned AS is a build/update organization feature. Portable raygen/hit/ray-query shaders remain KHR-style.

### Engine status

Required by the spec, not consumed yet (C4).

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| `VK_KHR_acceleration_structure` | AS objects, BLAS references in instances, the `accelerationStructure` limits |
| `VK_NV_cluster_acceleration_structure` | Complementary NV geometry clustering |
| RT pipeline / ray query | Consumers of the world structure |
| `VK_EXT_opacity_micromap` | no 2-state instance flag: rays set the 2-state ray flag instead |

---

## 10. Non-goals / do not do

- Do not keep a per-frame full TLAS build next to the PTLAS.
- Do not write ops on the CPU.
- Do not keep one PTLAS per frame slot: updates apply to their source (feature spec §3.6).

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is NV partitioned AS enabled? | **Yes**, required at device creation (C4) |
| Is it used? | **Yes**: one PTLAS updated in place every frame on async compute; a still frame writes no instances (C4, 2026-11-14) |
| Next step | Bistro measurements (M9, engine spec Q10); `UPDATE_INSTANCE` ops once BLAS change per frame (cluster LOD, post-1.0) |
