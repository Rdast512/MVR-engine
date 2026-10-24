# VK_NV_cluster_acceleration_structure — Implementation Spec

**Status in this repo: ENABLED (device extension + feature + properties cached), NOT CONSUMED — optional NVIDIA path, not baseline RT.**

Official sources:

- [VK_NV_cluster_acceleration_structure](https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_cluster_acceleration_structure.html)
- NVIDIA / Khronos extension documentation for cluster AS build APIs
- [VkPhysicalDeviceClusterAccelerationStructureFeaturesNV](https://docs.vulkan.org/spec/latest/chapters/features.html) (as present in headers)
- Properties: `VkPhysicalDeviceClusterAccelerationStructurePropertiesNV`

> Vendor extension: **NVIDIA-only**. Portable baseline remains `VK_KHR_acceleration_structure`.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::NVClusterAccelerationStructureExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → feature chain | `clusterAccelerationStructure = true` |
| Property query | `src/core/vk_device.cpp` → properties chain | Queried and assigned |
| Capability cache | `src/core/types.hpp` → `HardwareCapabilities::clusterAccelerationStructure` | Cached |
| API usage | AS / renderer | **None** |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::NVClusterAccelerationStructureExtensionName,

// src/core/vk_device.cpp (feature)
// vk::PhysicalDeviceClusterAccelerationStructureFeaturesNV
{.clusterAccelerationStructure = true},

// src/core/vk_device.cpp (properties)
capabilities.clusterAccelerationStructure =
    propertiesChain.get<vk::PhysicalDeviceClusterAccelerationStructurePropertiesNV>();

// src/core/types.hpp
vk::PhysicalDeviceClusterAccelerationStructurePropertiesNV clusterAccelerationStructure;
```

### Not wired yet

- Cluster AS create/build/update command paths (`vkCmdBuildClusterAccelerationStructure*`-class APIs per header)
- Content pipeline producing cluster geometry
- Gating when device is non-NVIDIA (today extension is **required**, so device create fails without support)
- Integration with KHR TLAS / ray query / RT pipeline

---

## 2. What the extension provides

Cluster acceleration structures target **large worlds / highly instantiated or clustered geometry** with NVIDIA-specific BVH clustering (finer-grained or streaming-friendly structures compared to a single monolithic BLAS strategy). This is **vendor-only** — not portable, not promoted, and **not** a replacement for `VK_KHR_acceleration_structure`. The portable baseline remains KHR BLAS/TLAS + ray query / RT pipeline.

This repo caches `HardwareCapabilities::clusterAccelerationStructure` and currently lists `vk::NVClusterAccelerationStructureExtensionName` as a **required** device extension (portability hazard: device create fails on non-NVIDIA). Feature is on; **not consumed**. Optionalize this extension before treating cluster AS as a product path.

| Feature | Role | Engine relevance |
| --- | --- | --- |
| Cluster AS feature bit | Master enable | Optional high-end path |
| Properties | Driver limits for cluster builds | Sizing / caps |
| Build commands | NVIDIA cluster AS build API family | Streaming / nanite-like geometry |

Exact command names and structs evolve with header versions — implement against the Vulkan-Hpp headers this repo compiles with.

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
typedef struct VkPhysicalDeviceClusterAccelerationStructureFeaturesNV {
    VkStructureType sType;
    void*           pNext;
    VkBool32        clusterAccelerationStructure;
} VkPhysicalDeviceClusterAccelerationStructureFeaturesNV;
```

**Already enabled:** `clusterAccelerationStructure = true`.

### 3.2 Properties (cached)

```cpp
// Shape is header-defined; cached as:
// HardwareCapabilities::clusterAccelerationStructure
// VkPhysicalDeviceClusterAccelerationStructurePropertiesNV
```

Read fields from the project’s Vulkan headers when implementing (max cluster sizes, alignments, etc.).

### 3.3 Commands

Use official / NVIDIA docs and local headers for:

- Cluster AS input buffer layouts
- Build/move/instantiate command buffer entry points
- Interaction with device addresses and scratch memory

Do **not** invent commands; match `vulkan_raii.hpp` / `vulkan.hpp` used by the engine.

---

## Limits

Vendor extension. Do **not** invent portable Required Limits for this path.

Properties are **implementation-dependent**. Read `HardwareCapabilities::clusterAccelerationStructure` and the local `vulkan.hpp` fields on `VkPhysicalDeviceClusterAccelerationStructurePropertiesNV` (typical names: `maxVerticesPerCluster`, `maxTrianglesPerCluster`, `clusterScratchByteAlignment`, `clusterByteAlignment`, `clusterTemplateByteAlignment`, `clusterBottomLevelByteAlignment`, `clusterTemplateBoundsByteAlignment`, `maxClusterGeometryIndex`). Use those values for sizing and alignment; do not hardcode.

KHR AS limits (`HardwareCapabilities::accelerationStructure`) still apply to any KHR BLAS/TLAS you keep as the default path.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE (aggressive)

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Currently **required** — consider making optional later for non-NVIDIA |
| `src/core/vk_device.cpp` | Feature + properties already set |
| `src/core/types.hpp` | Properties cached |

**Product note:** Requiring NV cluster AS means logical device creation only succeeds on supporting NVIDIA drivers. For multi-vendor portability, move to an optional extension list gated by `isExtensionSupported` while keeping KHR AS required.

### 4.2 Implementation home

| Module | Role |
| --- | --- |
| Future AS module | Branch: KHR BLAS vs NV cluster path behind vendor gate |
| Content streaming | Cluster generation offline or at load |
| `vk_renderer.cpp` | No direct cluster AS calls in raster path |

Ray query / RT pipeline still need a **KHR-compatible** TLAS interface or an NV-documented way to trace against cluster structures — verify with extension rules before designing the scene graph.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Ship portable KHR AS + RT/ray query | AS / RT modules | Baseline |
| P1 | Consider optionalizing NV extension | `vk_device.hpp/.cpp` | Multi-vendor device create |
| P2 | Cluster AS builder behind `#`/runtime NVIDIA gate | AS module | Large-world optimization |
| P3 | Streaming updates for clusters | AS + scene | Open-world target |

### Implementation recipe

1. Ship portable KHR AS + ray query / RT pipeline first. Those are enabled and **not consumed**; cluster AS is not the baseline.
2. Treat the current **required** listing of `NVClusterAccelerationStructure` as a portability hazard — move it to an optional list gated by `isExtensionSupported` so non-NVIDIA devices can create.
3. Read `device.getCapabilities().clusterAccelerationStructure` (and local `vulkan.hpp` fields) for cluster sizes and alignments. Do not invent Required Limits.
4. Behind an NVIDIA runtime gate: record cluster AS build/move/instantiate commands from the headers this repo compiles with; keep KHR BLAS/TLAS as the default.
5. Barrier before trace/query. Confirm with the extension whether cluster structures are visible to KHR `traceRays` / `rayQuery` or need an NV-documented TLAS hook.
6. Do not call NV entry points when the extension is not enabled.

---

## 6. Concrete integration sketch

```cpp
const auto& clusterProps = device.getCapabilities().clusterAccelerationStructure;
// Log / gate build parameters from clusterProps.*

if (useNvClusterPath) {
    // Record NVIDIA cluster AS builds using header APIs
} else {
    // KHR BLAS/TLAS path (default)
}
```

Prefer feature detection + optional extension listing rather than compile-only switches.

---

## 7. Validation / test checklist

1. Device creates on target NVIDIA GPU with cluster feature (already required).
2. Capability dump shows cluster properties.
3. Cluster builds respect property limits and alignments.
4. Trace/query results match KHR path for equivalent content (where comparable).
5. Non-NVIDIA: either optional skip or documented hard requirement.

---

## 8. Shader interaction

**Indirect / vendor RT geometry.** Cluster AS changes how geometry is built for traversal; ray-query / RT pipeline shaders still use TLAS traces/queries. Prefer KHR AS + ray query/pipeline for portable shaders first.

### Engine status

Enabled as vendor extension, not consumed.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| `VK_KHR_acceleration_structure` | Portable baseline; still required in this repo |
| `VK_NV_partitioned_acceleration_structure` | Related NVIDIA large-scene AS tech |
| `VK_KHR_ray_tracing_pipeline` / `ray_query` | Consumers — confirm cluster AS visibility rules |
| BDA / deferred host ops | Same memory/address model assumptions |

---

## 10. Non-goals / do not do

- Do not treat cluster AS as the only AS implementation.
- Do not block engine bring-up of KHR RT on NV cluster work.
- Do not document cluster AS as cross-vendor.
- Do not call NV entry points without the extension enabled (validation error).

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is NV cluster AS enabled? | **Yes** — extension + feature + properties cached |
| Is it used? | **No** |
| Highest-value next steps | Portable KHR AS first; optionalize NV for multi-vendor; then cluster builder |
| Secondary steps | Streaming cluster updates for large worlds |
