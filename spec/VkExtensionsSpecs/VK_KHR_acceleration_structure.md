# VK_KHR_acceleration_structure — Implementation Spec

**Status in this repo: ENABLED (device extension + feature + properties cached), NOT CONSUMED by any build/trace/query path.**

Official sources:

- [VK_KHR_acceleration_structure](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_acceleration_structure.html)
- [Proposal](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_KHR_acceleration_structure.adoc)
- [VkPhysicalDeviceAccelerationStructureFeaturesKHR](https://docs.vulkan.org/spec/latest/chapters/features.html)
- [VkPhysicalDeviceAccelerationStructurePropertiesKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceAccelerationStructurePropertiesKHR.html)

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::KHRAccelerationStructureExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → `createLogicalDevice()` feature chain | `accelerationStructure = true` only |
| Property query | `src/core/vk_device.cpp` → properties chain | Queried and assigned |
| Capability cache | `src/core/types.hpp` → `HardwareCapabilities::accelerationStructure` | Cached |
| API usage | renderer / pipelines / descriptors | **None** — no `createAccelerationStructure`, no `cmdBuildAccelerationStructures`, no AS descriptors |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::KHRDeferredHostOperationsExtensionName, // required by KHR_acceleration_structure
vk::KHRAccelerationStructureExtensionName,

// src/core/vk_device.cpp (feature chain)
// vk::PhysicalDeviceAccelerationStructureFeaturesKHR
{.accelerationStructure = true},

// src/core/vk_device.cpp (properties chain)
capabilities.accelerationStructure =
    propertiesChain.get<vk::PhysicalDeviceAccelerationStructurePropertiesKHR>();

// src/core/types.hpp
vk::PhysicalDeviceAccelerationStructurePropertiesKHR accelerationStructure;
```

Also required (already true in this project):

- Vulkan 1.2 `bufferDeviceAddress = true` in `PhysicalDeviceVulkan12Features`
- `VK_KHR_deferred_host_operations` listed

### Not wired yet

- AS object create/destroy and buffer backing (`vkCreateAccelerationStructureKHR`, etc.)
- Build-size queries and scratch buffers
- Device builds (`vkCmdBuildAccelerationStructuresKHR` / Indirect)
- Host builds + deferred ops
- Copy / serialize / compact
- `vkGetAccelerationStructureDeviceAddressKHR`
- Descriptor type `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR` (or heap equivalent)
- Geometry/instance buffer upload paths

---

## 2. What the extension provides

Acceleration structures (AS) are the BVH-like objects consumed by ray tracing pipelines and ray queries. This is the **portable KHR baseline** of the Vulkan RT stack: every multi-vendor path (`VK_KHR_ray_tracing_pipeline`, `VK_KHR_ray_query`, opacity micromaps) sits on BLAS/TLAS built here. The extension is **not** promoted into core Vulkan 1.4; this repo lists it as a required device extension, enables only the master `accelerationStructure` feature, and lists `VK_KHR_deferred_host_operations` solely as this extension’s spec dependency.

NVIDIA `VK_NV_cluster_acceleration_structure` / `VK_NV_partitioned_acceleration_structure` are vendor-only add-ons, not replacements. They are currently **required** device extensions here (a portability hazard). KHR AS remains the path that must work first. Enablement is complete (`HardwareCapabilities::accelerationStructure` is cached); **nothing consumes** create/build/address/descriptor APIs yet.

| Feature | New API / type | Engine relevance |
| --- | --- | --- |
| Bottom-level AS (BLAS) | `VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR` | Per-mesh geometry BVH |
| Top-level AS (TLAS) | `VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR` | Instance hierarchy for scenes |
| Build sizes | `vkGetAccelerationStructureBuildSizesKHR` | AS + scratch buffer sizing |
| Device build | `vkCmdBuildAccelerationStructuresKHR` | Runtime GPU builds |
| Indirect build | `vkCmdBuildAccelerationStructuresIndirectKHR` | GPU-driven builds (feature bit) |
| Copy / compact | `vkCmdCopyAccelerationStructureKHR` | Compaction after build |
| Device address | `vkGetAccelerationStructureDeviceAddressKHR` | Instance refs, shader binding |
| Host commands | `vkBuildAccelerationStructuresKHR`, etc. | Offline bake (optional feature) |
| AS descriptor | `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR` | Bind TLAS to shaders |

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
typedef struct VkPhysicalDeviceAccelerationStructureFeaturesKHR {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR
    void*           pNext;
    VkBool32        accelerationStructure;
    VkBool32        accelerationStructureCaptureReplay;
    VkBool32        accelerationStructureIndirectBuild;
    VkBool32        accelerationStructureHostCommands;
    VkBool32        descriptorBindingAccelerationStructureUpdateAfterBind;
} VkPhysicalDeviceAccelerationStructureFeaturesKHR;
```

| Field | This repo | Notes |
| --- | --- | --- |
| `accelerationStructure` | **true** | Master switch — required |
| `accelerationStructureCaptureReplay` | false (default) | Capture/replay tooling only |
| `accelerationStructureIndirectBuild` | false (default) | Enable when GPU-driven builds needed |
| `accelerationStructureHostCommands` | false (default) | Enable when CPU builds needed |
| `descriptorBindingAccelerationStructureUpdateAfterBind` | false (default) | UAB for AS descriptors |

**Already enabled:** master bit only. Turn on additional bits when a consumer requires them.

### 3.2 Properties (query after physical device pick)

```cpp
typedef struct VkPhysicalDeviceAccelerationStructurePropertiesKHR {
    VkStructureType sType;
    void*           pNext;
    uint64_t        maxGeometryCount;
    uint64_t        maxInstanceCount;
    uint64_t        maxPrimitiveCount;
    uint32_t        maxPerStageDescriptorAccelerationStructures;
    uint32_t        maxPerStageDescriptorUpdateAfterBindAccelerationStructures;
    uint32_t        maxDescriptorSetAccelerationStructures;
    uint32_t        maxDescriptorSetUpdateAfterBindAccelerationStructures;
    uint32_t        minAccelerationStructureScratchOffsetAlignment;
} VkPhysicalDeviceAccelerationStructurePropertiesKHR;
```

**Already cached** as `HardwareCapabilities::accelerationStructure`.

Use `minAccelerationStructureScratchOffsetAlignment` when placing scratch buffers.

### 3.3 Core commands

```cpp
VkResult vkCreateAccelerationStructureKHR(VkDevice device,
    const VkAccelerationStructureCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkAccelerationStructureKHR* pAccelerationStructure);

void vkDestroyAccelerationStructureKHR(VkDevice device,
    VkAccelerationStructureKHR accelerationStructure,
    const VkAllocationCallbacks* pAllocator);

void vkGetAccelerationStructureBuildSizesKHR(VkDevice device,
    VkAccelerationStructureBuildTypeKHR buildType,
    const VkAccelerationStructureBuildGeometryInfoKHR* pBuildInfo,
    const uint32_t* pMaxPrimitiveCounts,
    VkAccelerationStructureBuildSizesInfoKHR* pSizeInfo);

void vkCmdBuildAccelerationStructuresKHR(VkCommandBuffer commandBuffer,
    uint32_t infoCount,
    const VkAccelerationStructureBuildGeometryInfoKHR* pInfos,
    const VkAccelerationStructureBuildRangeInfoKHR* const* ppBuildRangeInfos);

void vkCmdBuildAccelerationStructuresIndirectKHR(VkCommandBuffer commandBuffer,
    uint32_t infoCount,
    const VkAccelerationStructureBuildGeometryInfoKHR* pInfos,
    const VkDeviceAddress* pIndirectDeviceAddresses,
    const uint32_t* pIndirectStrides,
    const uint32_t* const* ppMaxPrimitiveCounts);

void vkCmdCopyAccelerationStructureKHR(VkCommandBuffer commandBuffer,
    const VkCopyAccelerationStructureInfoKHR* pInfo);

void vkCmdWriteAccelerationStructuresPropertiesKHR(VkCommandBuffer commandBuffer,
    uint32_t accelerationStructureCount,
    const VkAccelerationStructureKHR* pAccelerationStructures,
    VkQueryType queryType,
    VkQueryPool queryPool,
    uint32_t firstQuery);

VkDeviceAddress vkGetAccelerationStructureDeviceAddressKHR(VkDevice device,
    const VkAccelerationStructureDeviceAddressInfoKHR* pInfo);

// Host variants (require accelerationStructureHostCommands):
// vkBuildAccelerationStructuresKHR, vkCopyAccelerationStructureKHR,
// vkWriteAccelerationStructuresPropertiesKHR, vkCopyAccelerationStructureToMemoryKHR,
// vkCopyMemoryToAccelerationStructureKHR — optional deferredOperation parameter
```

### 3.4 Key create / build structs (host-side setup)

```cpp
typedef struct VkAccelerationStructureCreateInfoKHR {
    VkStructureType                     sType;
    const void*                         pNext;
    VkAccelerationStructureCreateFlagsKHR createFlags;
    VkBuffer                            buffer;
    VkDeviceSize                        offset;
    VkDeviceSize                        size;
    VkAccelerationStructureTypeKHR      type; // TOP_LEVEL / BOTTOM_LEVEL / GENERIC
    VkDeviceAddress                     deviceAddress; // capture/replay
} VkAccelerationStructureCreateInfoKHR;

typedef struct VkAccelerationStructureBuildGeometryInfoKHR {
    VkStructureType                     sType;
    const void*                         pNext;
    VkAccelerationStructureTypeKHR      type;
    VkBuildAccelerationStructureFlagsKHR flags;
    VkBuildAccelerationStructureModeKHR mode; // BUILD or UPDATE
    VkAccelerationStructureKHR          srcAccelerationStructure;
    VkAccelerationStructureKHR          dstAccelerationStructure;
    uint32_t                            geometryCount;
    const VkAccelerationStructureGeometryKHR* pGeometries;
    const VkAccelerationStructureGeometryKHR* const* ppGeometries;
    VkDeviceOrHostAddressKHR            scratchData;
} VkAccelerationStructureBuildGeometryInfoKHR;
```

Buffer usages required for AS storage / scratch / geometry:

- `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR`
- `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR` (vertex/index/instance inputs)
- `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT` and/or shader device address flags as required by BDA
- Scratch: typically storage + device address; respect scratch offset alignment property

---

## Limits

Cached as `HardwareCapabilities::accelerationStructure` (`src/core/types.hpp`). Source: [Vulkan Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html#limits-minmax).

| Property | Required limit | Notes |
| --- | --- | --- |
| `maxGeometryCount` | min **2^24 − 1** (must not exceed 2^32−1) | 64-bit property; create-time geometry counts are **32-bit** |
| `maxInstanceCount` | min **2^24 − 1** | 64-bit property; create-time instance counts are **32-bit** |
| `maxPrimitiveCount` | min **2^29 − 1** | 64-bit property; create-time primitive counts are **32-bit** |
| `maxPerStageDescriptorAccelerationStructures` | min **16** | Per-stage AS descriptors |
| `maxDescriptorSetAccelerationStructures` | min **16** | Per-set AS descriptors |
| `minAccelerationStructureScratchOffsetAlignment` | **max 256**, power of two | Scratch `deviceAddress` / offset must be a multiple of this |

UAB variants (`maxPerStageDescriptorUpdateAfterBindAccelerationStructures`, `maxDescriptorSetUpdateAfterBindAccelerationStructures`) exist on the same struct; this repo does not enable AS update-after-bind.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE (master feature)

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep extension name + deferred host ops dependency |
| `src/core/vk_device.cpp` | Keep feature + property assignment; enable extra feature bits when needed |
| `src/core/types.hpp` | Keep `HardwareCapabilities::accelerationStructure` |

Optional hardening:

1. Log key AS limits at init (`maxPrimitiveCount`, `minAccelerationStructureScratchOffsetAlignment`).
2. Assert `bufferDeviceAddress` remains enabled whenever AS is required.

### 4.2 New AS resource module (primary work)

**Recommended new files** (do not dump all of this into `vk_renderer.cpp`):

| File | Role |
| --- | --- |
| `src/core/vk_acceleration_structure.hpp/.cpp` (or under `src/render/`) | BLAS/TLAS create, build sizes, build, compact, destroy |
| `src/core/vk_resource_manager.cpp` | Buffer allocation helpers with AS usage bits + BDA |
| Scene/mesh load path | Upload triangle/AABB geometry and instance buffers |

### 4.3 Descriptor binding

**File:** `src/core/vk_descriptors.cpp` (descriptor heaps path)

- Bind TLAS with `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR` via `VkWriteDescriptorSetAccelerationStructureKHR` on the write pNext chain (legacy sets), **or** the heap/descriptor-buffer equivalent once RT descriptors are defined.
- Today heaps/legacy paths have **no** AS descriptor type wiring.

### 4.4 Command recording

**File:** `src/render/vk_renderer.cpp` (or dedicated RT pass module)

- AS **builds** go in transfer/graphics/compute-capable queues with proper barriers (`VK_ACCESS_2_ACCELERATION_STRUCTURE_*_BIT_KHR`, `VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR`).
- Current renderer is **raster dynamic rendering only** — introduce a separate RT prep / trace path rather than overloading the MSAA forward pass.

### 4.5 Pipelines

**File:** `src/render/vk_pipeline.cpp`

- No change required for AS alone; graphics pipelines stay as-is.
- Ray tracing pipelines / ray-query shaders depend on AS but live under their own extensions (`VK_KHR_ray_tracing_pipeline`, `VK_KHR_ray_query`).

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep enablement as-is | `vk_device.*`, `types.hpp` | Already correct for master feature |
| P1 | AS buffer + object helpers (create/destroy, device address) | new AS module, resource manager | Foundation |
| P1 | `getAccelerationStructureBuildSizes` + scratch alloc | new AS module | Correct sizing / alignment |
| P1 | Device BLAS build for static meshes | AS module + mesh load | First real consumer |
| P2 | TLAS build/update from instance buffer | AS module + scene | Scene-level tracing |
| P2 | Compaction path (`COPY_MODE_COMPACT`) | AS module | Memory quality |
| P2 | TLAS descriptor write | `vk_descriptors.cpp` | Shader visibility |
| P3 | Indirect builds (enable feature bit) | device feature + AS module | GPU-driven scenes |
| P3 | Host builds + deferred ops | AS module + deferred host ops | Offline bake |

### Implementation recipe

1. Keep KHR AS + deferred host ops required; keep `HardwareCapabilities::accelerationStructure` as the only portable AS cache. Do not treat NV cluster/partitioned as the baseline.
2. Query build sizes with `getAccelerationStructureBuildSizesKHR` (`eDevice`) from geometry + primitive counts (counts must stay within the 32-bit create-time range and ≤ cached max* limits).
3. Allocate the AS buffer with `ACCELERATION_STRUCTURE_STORAGE` + `SHADER_DEVICE_ADDRESS` (STORAGE + BDA), size ≥ `sizes.accelerationStructureSize`.
4. `createAccelerationStructureKHR` on that buffer (type BLAS or TLAS).
5. Allocate scratch (`STORAGE` + BDA) of `sizes.buildScratchSize`, offset/address aligned to `capabilities.accelerationStructure.minAccelerationStructureScratchOffsetAlignment`.
6. Record `cmdBuildAccelerationStructuresKHR`.
7. Barrier: `ACCELERATION_STRUCTURE_BUILD` write → later AS read (TLAS build, `traceRays`, or `rayQuery`).
8. `getAccelerationStructureDeviceAddressKHR` and/or write `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR` (or the heap equivalent). Still **not consumed** anywhere in this repo.

---

## 6. Concrete integration sketch (device BLAS build)

Illustrative:

```cpp
// 1) Query sizes
vk::AccelerationStructureBuildGeometryInfoKHR buildInfo{
    .type  = vk::AccelerationStructureTypeKHR::eBottomLevel,
    .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
    .mode  = vk::BuildAccelerationStructureModeKHR::eBuild,
    .geometryCount = 1,
    .pGeometries = &geometry
};

vk::AccelerationStructureBuildSizesInfoKHR sizes =
    device.getAccelerationStructureBuildSizesKHR(
        vk::AccelerationStructureBuildTypeKHR::eDevice,
        buildInfo,
        {primitiveCount});

// 2) Alloc AS buffer (storage + BDA) sized to sizes.accelerationStructureSize
// 3) Create AS object on that buffer
vk::AccelerationStructureCreateInfoKHR createInfo{
    .buffer = *asBuffer,
    .offset = 0,
    .size   = sizes.accelerationStructureSize,
    .type   = vk::AccelerationStructureTypeKHR::eBottomLevel
};
vk::raii::AccelerationStructureKHR blas(device, createInfo);

// 4) Scratch buffer: sizes.buildScratchSize, align to
//    capabilities.accelerationStructure.minAccelerationStructureScratchOffsetAlignment

// 5) Record build
buildInfo.dstAccelerationStructure = *blas;
buildInfo.scratchData.deviceAddress = scratchAddress;
cmd.buildAccelerationStructuresKHR(buildInfo, buildRanges);

// 6) Barrier before TLAS build or trace/query use
```

---

## 7. Validation / test checklist

1. Device creates with AS extension + `accelerationStructure = true` (already).
2. Capability dump shows non-zero limits and scratch alignment.
3. Geometry/instance buffers have correct usage flags + device addresses.
4. Build sizes query before create; AS buffer ≥ `accelerationStructureSize`.
5. Scratch offset respects `minAccelerationStructureScratchOffsetAlignment`.
6. No validation errors on `cmdBuildAccelerationStructuresKHR`.
7. Post-build barrier before TLAS or ray use.
8. Destroy AS before underlying buffer; no use-after-destroy in descriptors.

---

## 8. Shader interaction

**Indirect but required — shaders do not build AS; they only reference them.** Host builds BLAS/TLAS; shaders consume TLAS via [VK_KHR_ray_query](./VK_KHR_ray_query.md) (`rayQueryInitialize`) or [VK_KHR_ray_tracing_pipeline](./VK_KHR_ray_tracing_pipeline.md) (`traceRayEXT`). Acceleration structures appear as a descriptor type or heap resource write; engine shaders will need TLAS binding plus BDA for host-side build inputs.

### Engine status

AS extension **enabled**, no builds, no shader AS declarations. First shader work blocked on BLAS/TLAS create + descriptor/heap write path.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| `VK_KHR_deferred_host_operations` | Spec dependency (listed) |
| Vulkan 1.2 `bufferDeviceAddress` | Required for AS buffers/addresses (enabled) |
| `VK_KHR_ray_tracing_pipeline` | Consumes TLAS via SBT + `traceRays` |
| `VK_KHR_ray_query` | Consumes TLAS via `rayQuery*` in any stage |
| `VK_KHR_ray_tracing_maintenance1` | Extra copy/indirect helpers on top of AS |
| `VK_EXT_opacity_micromap` | Micromaps attached to AS geometry |
| `VK_NV_cluster_acceleration_structure` / `VK_NV_partitioned_acceleration_structure` | Optional NVIDIA AS alternatives/extensions |
| Descriptor heaps (`vk_descriptors.cpp`) | Must learn AS descriptor type |

---

## 10. Non-goals / do not do

- Do not call AS APIs from the current MSAA forward pass without a dedicated RT/scene path.
- Do not enable host/indirect/capture-replay feature bits until a real consumer needs them.
- Do not store AS without device-addressable buffers.
- Do not skip build-size queries or scratch alignment.
- Do not treat NVIDIA cluster/partitioned AS as the baseline — KHR AS is the portable path.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is acceleration structure enabled? | **Yes** — extension + `accelerationStructure` feature + properties cached |
| Is it used? | **No** — zero build/bind/trace consumption |
| Highest-value next steps | AS module: sizes → create → device BLAS → TLAS → descriptor |
| Secondary steps | Compaction; optional host/indirect features when needed |
