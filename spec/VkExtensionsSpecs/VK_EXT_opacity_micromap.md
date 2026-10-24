# VK_EXT_opacity_micromap — Implementation Spec

**Status in this repo: ENABLED (device extension + `micromap` feature), PARTIALLY QUERIED (properties in chain, NOT stored in `HardwareCapabilities`), NOT CONSUMED by AS builds.**

Official sources:

- [VK_EXT_opacity_micromap](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_opacity_micromap.html)
- [Proposal](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_EXT_opacity_micromap.adoc)
- [VkPhysicalDeviceOpacityMicromapFeaturesEXT](https://docs.vulkan.org/spec/latest/chapters/features.html)
- [VkPhysicalDeviceOpacityMicromapPropertiesEXT](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceOpacityMicromapPropertiesEXT.html)

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::EXTOpacityMicromapExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → feature chain | `micromap = true` only |
| Property query | `src/core/vk_device.cpp` → properties chain | `vk::PhysicalDeviceOpacityMicromapPropertiesEXT` **in chain** (~line 417) |
| Capability cache | `src/core/types.hpp` → `HardwareCapabilities` | **NOT stored** — no `opacityMicromap` field; get() never assigned |
| API usage | AS / micromap builds | **None** |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::EXTOpacityMicromapExtensionName,

// src/core/vk_device.cpp (feature)
// vk::PhysicalDeviceOpacityMicromapFeaturesEXT
{.micromap = true},

// src/core/vk_device.cpp (properties chain includes type — but no assignment)
vk::PhysicalDeviceOpacityMicromapPropertiesEXT, // in getProperties2<> chain
// NOTE: no capabilities.opacityMicromap = propertiesChain.get<...>();
```

### Not wired yet

- Cache `PhysicalDeviceOpacityMicromapPropertiesEXT` in `HardwareCapabilities`
- Optional feature bits: `micromapCaptureReplay`, `micromapHostCommands`, `micromapArrayFeatures` (names per header)
- `vkCreateMicromapEXT` / destroy / build sizes / `vkCmdBuildMicromapsEXT`
- Attach micromap to `VkAccelerationStructureGeometryTrianglesDataKHR` via pNext (`VkAccelerationStructureTrianglesOpacityMicromapEXT`)
- Host micromap builds with deferred ops

---

## 2. What the extension provides

Opacity micromaps encode sub-triangle opacity (e.g. alpha-tested foliage) so ray traversal can **skip or accept** hits without full any-hit shading in many cases. This is a portable EXT on top of KHR acceleration structure (triangle geometry `pNext`), consumed automatically by KHR ray query / RT pipeline traversal once attached. It is **not** promoted into core Vulkan 1.4. Host micromap builds can use `VK_KHR_deferred_host_operations`.

This repo requires the extension and sets `micromap = true`. `PhysicalDeviceOpacityMicromapPropertiesEXT` is **in the properties chain but not assigned** — there is no `HardwareCapabilities` field. Not consumed: no micromap objects, no BLAS attachment. Do not chase micromaps before portable KHR BLAS/TLAS work.

| Feature | New API | Engine relevance |
| --- | --- | --- |
| Micromap objects | `VkMicromapEXT` create/build/destroy | Alpha-tested geometry acceleration |
| Build sizes / device build | `vkGetMicromapBuildSizesEXT`, `vkCmdBuildMicromapsEXT` | Same pattern as AS |
| Link to triangle geometry | `VkAccelerationStructureTrianglesOpacityMicromapEXT` | BLAS references micromap |
| Host builds | optional feature + deferred ops | Offline bake |

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
typedef struct VkPhysicalDeviceOpacityMicromapFeaturesEXT {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT
    void*           pNext;
    VkBool32        micromap;
    VkBool32        micromapCaptureReplay;
    VkBool32        micromapHostCommands;
    VkBool32        micromapArrayFeatures;
} VkPhysicalDeviceOpacityMicromapFeaturesEXT;
```

| Field | This repo |
| --- | --- |
| `micromap` | **true** |
| Others | default **false** until needed |

### 3.2 Properties (queried in chain, not cached)

```cpp
typedef struct VkPhysicalDeviceOpacityMicromapPropertiesEXT {
    VkStructureType sType;
    void*           pNext;
    uint32_t        maxOpacity2StateSubdivisionLevel;
    uint32_t        maxOpacity4StateSubdivisionLevel;
} VkPhysicalDeviceOpacityMicromapPropertiesEXT;
```

**Gap to fix when implementing:** add to `HardwareCapabilities` and assign in `vk_device.cpp` next to other property copies.

### 3.3 Representative commands

```cpp
VkResult vkCreateMicromapEXT(VkDevice device,
    const VkMicromapCreateInfoEXT* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkMicromapEXT* pMicromap);

void vkDestroyMicromapEXT(...);

void vkGetMicromapBuildSizesEXT(VkDevice device,
    VkAccelerationStructureBuildTypeKHR buildType,
    const VkMicromapBuildInfoEXT* pBuildInfo,
    VkMicromapBuildSizesInfoEXT* pSizeInfo);

void vkCmdBuildMicromapsEXT(VkCommandBuffer commandBuffer,
    uint32_t infoCount,
    const VkMicromapBuildInfoEXT* pInfos);

// Plus copy/write-property/host variants analogous to AS — see official refpages
```

### 3.4 Attach to AS geometry

```cpp
typedef struct VkAccelerationStructureTrianglesOpacityMicromapEXT {
    VkStructureType sType; // VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT
    void*           pNext;
    VkOpacityMicromapFormatEXT indexType; // format/index typing per spec
    VkDeviceOrHostAddressConstKHR indexBuffer;
    VkDeviceSize    indexStride;
    uint32_t        baseTriangle;
    uint32_t        usageCountsCount;
    const VkMicromapUsageEXT* pUsageCounts;
    const VkMicromapUsageEXT* const* ppUsageCounts;
    VkMicromapEXT   micromap;
} VkAccelerationStructureTrianglesOpacityMicromapEXT;
```

Chain onto triangle geometry pNext when building BLAS for alpha-tested meshes.

---

## Limits

Source: [Vulkan Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html#limits-minmax).

| Property | Required limit | This repo |
| --- | --- | --- |
| `maxOpacity2StateSubdivisionLevel` | min **3** | On `VkPhysicalDeviceOpacityMicromapPropertiesEXT` — **queried, not cached** |
| `maxOpacity4StateSubdivisionLevel` | min **3** | Same EXT struct — **queried, not cached** |
| `maxMicromapTriangles` | min **2^27** (HTML may show `227`) | Required Limits row on the KHR properties struct; not a field of the EXT struct this repo queries |

`src/core/vk_device.cpp` includes `vk::PhysicalDeviceOpacityMicromapPropertiesEXT` in `getProperties2` but never assigns it. `HardwareCapabilities` has no `opacityMicromap` member (unlike `accelerationStructure` / `rayTracingPipeline`). **Cache the queried EXT properties before consuming** micromap builds so subdivision levels are gated. If a later header exposes `maxMicromapTriangles` on the struct you query, cache that too.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — partial

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep extension name |
| `src/core/vk_device.cpp` | Keep `micromap = true`; **assign properties into capabilities** |
| `src/core/types.hpp` | **Add** `vk::PhysicalDeviceOpacityMicromapPropertiesEXT opacityMicromap` (or similar name) |

### 4.2 Build path

Micromaps are **not** independent of AS:

1. Build micromap(s) for sub-triangle opacity data.
2. Build BLAS with geometry pNext referencing micromap.
3. TLAS instances of those BLAS as usual.
4. Ray query / RT pipeline traversal benefits automatically when flags/geometry are set correctly.

| Module | Role |
| --- | --- |
| Future AS module | Own micromap create/build next to BLAS |
| Mesh/material import | Generate opacity subdivision data for cutouts |
| `vk_renderer.cpp` | No direct micromap calls in raster pass |

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep extension + `micromap` feature | `vk_device.*` | Already required |
| P1 | Cache opacity micromap properties | `types.hpp`, `vk_device.cpp` | Close the enablement gap |
| P0–P1 | Working BLAS/TLAS without micromaps | AS module | Baseline RT |
| P2 | Micromap build + BLAS attachment for cutout foliage | AS + content pipeline | Real value of extension |
| P3 | Host micromap builds / array features | device features + AS | Advanced content bake |

### Implementation recipe

1. Keep `micromap = true`. Close the cache gap: add `HardwareCapabilities::opacityMicromap` and `propertiesChain.get<PhysicalDeviceOpacityMicromapPropertiesEXT>()` next to the other RT property copies.
2. Ship portable KHR BLAS/TLAS **without** micromaps first (AS is enabled, **not consumed**).
3. Gate 2-state / 4-state `subdivisionLevel` with the cached max*SubdivisionLevel (min 3). Keep triangle counts ≤ `maxMicromapTriangles` (min **2^27**).
4. `getMicromapBuildSizesEXT` → alloc micromap buffer → `createMicromapEXT` → `cmdBuildMicromapsEXT` → barrier micromap write → AS build read.
5. Chain `VkAccelerationStructureTrianglesOpacityMicromapEXT` onto triangle geometry `pNext` and rebuild BLAS.
6. Optional host builds use deferred host ops (listed as the AS dependency). Destroy AS that reference a micromap before destroying the micromap.

---

## 6. Concrete integration sketch

```cpp
// types.hpp
vk::PhysicalDeviceOpacityMicromapPropertiesEXT opacityMicromap;

// vk_device.cpp after propertiesChain get:
capabilities.opacityMicromap =
    propertiesChain.get<vk::PhysicalDeviceOpacityMicromapPropertiesEXT>();

// Build order (later):
// cmd.buildMicromapsEXT(...);
// barrier micromap write → AS build read
// geometry.pNext = &trianglesOpacityMicromap; // includes micromap handle
// cmd.buildAccelerationStructuresKHR(...);
```

---

## 7. Validation / test checklist

1. Device creates with `micromap = true` (already).
2. After cache fix: subdivision level limits visible in capability dump.
3. Micromap buffer usages / build sizes respected.
4. BLAS build with micromap pNext validates; without orphan micromap handles.
5. Ray hits on cutout geometry match CPU reference for opacity states.
6. Destroy order: AS using micromap before micromap destroy (per valid usage).

---

## 8. Shader interaction

**Mostly data-plane + any-hit / ray-query behavior.** Opacity micromaps attach compressed opacity to AS geometry so traversal can skip or accept hits without full any-hit shading. Any-hit may run less often; closest-hit / ray-query committed hits see final accepted results. Coordinate micromap build (host) with any-hit alpha logic so opacity is not double-evaluated incorrectly.

### Engine status

Extension **enabled**, not consumed. No micromap builds or RT shaders yet.

Cross-refs: [VK_KHR_acceleration_structure](./VK_KHR_acceleration_structure.md), [VK_KHR_ray_query](./VK_KHR_ray_query.md), [VK_KHR_ray_tracing_pipeline](./VK_KHR_ray_tracing_pipeline.md).

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| `VK_KHR_acceleration_structure` | Micromaps attach to triangle geometry |
| `VK_KHR_deferred_host_operations` | Host micromap builds |
| `VK_KHR_ray_query` / `VK_KHR_ray_tracing_pipeline` | Consumers of micromap-accelerated AS |
| Properties chain bug/gap | Queried type not assigned to `HardwareCapabilities` |

---

## 10. Non-goals / do not do

- Do not claim properties are cached until `types.hpp` stores them.
- Do not enable micromaps on every BLAS — only alpha-tested content that benefits.
- Do not skip baseline AS implementation to chase micromaps first.
- Do not enable host/capture-replay micromap features without consumers.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is opacity micromap enabled? | **Yes** — extension + `micromap` feature |
| Are properties cached? | **No** — in property chain only, not in `HardwareCapabilities` |
| Is it used? | **No** |
| Highest-value next steps | Cache properties; after AS works, micromap build + BLAS pNext for cutouts |
