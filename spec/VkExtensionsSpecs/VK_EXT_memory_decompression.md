# VK_EXT_memory_decompression — Implementation Spec

**Status in this repo: ENABLED (device extension + feature + properties cached), NOT CONSUMED — no `vkCmdDecompressMemory*` usage.**

Official sources:

- [VK_EXT_memory_decompression](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_memory_decompression.html)
- [VkPhysicalDeviceMemoryDecompressionFeaturesEXT](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMemoryDecompressionFeaturesEXT.html)
- [VkPhysicalDeviceMemoryDecompressionPropertiesEXT](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMemoryDecompressionPropertiesEXT.html)

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::EXTMemoryDecompressionExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → feature chain | `memoryDecompression = true` |
| Property query | `src/core/vk_device.cpp` → properties chain | `vk::PhysicalDeviceMemoryDecompressionPropertiesEXT` queried |
| Capability cache | `src/core/types.hpp` → `HardwareCapabilities::memoryDecompression` | Cached |
| API usage | texture / streaming | **None** |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::EXTMemoryDecompressionExtensionName,

// src/core/vk_device.cpp (feature chain)
// vk::PhysicalDeviceMemoryDecompressionFeaturesEXT
{.memoryDecompression = true},

capabilities.memoryDecompression =
    propertiesChain.get<vk::PhysicalDeviceMemoryDecompressionPropertiesEXT>();

// src/core/types.hpp
vk::PhysicalDeviceMemoryDecompressionPropertiesEXT memoryDecompression;
```

### Not wired yet

- `vkCmdDecompressMemoryEXT`
- `vkCmdDecompressMemoryIndirectCountEXT` (if used)
- Compressed asset pipeline producing supported encodings
- Queue selection for decompression commands

---

## 2. What the extension provides

GPU decompression of buffers from compressed source memory into destination memory using implementation-supported compression formats (reported in properties). Useful for streaming assets without CPU inflate.

CPU inflate of streamed blobs burns host time and extra staging copies. This remains a **device extension** (not promoted).

It does **not** guarantee any particular codec. `decompressionMethods` may be empty on some devices — never assume GDeflate or any other bit without querying `HardwareCapabilities::memoryDecompression`.

| Feature | New API / bit | Engine relevance |
| --- | --- | --- |
| Feature | `memoryDecompression` | Master switch (enabled) |
| Properties | supported decompression methods / max sizes | Gate asset formats |
| Commands | `vkCmdDecompressMemoryEXT`, multi/indirect variants | Streaming path |

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
typedef struct VkPhysicalDeviceMemoryDecompressionFeaturesEXT {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_DECOMPRESSION_FEATURES_EXT
    void*           pNext;
    VkBool32        memoryDecompression;
} VkPhysicalDeviceMemoryDecompressionFeaturesEXT;
```

**Already enabled.**

### 3.2 Properties (cached)

```cpp
typedef struct VkPhysicalDeviceMemoryDecompressionPropertiesEXT {
    VkStructureType                sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_DECOMPRESSION_PROPERTIES_EXT
    void*                          pNext;
    VkMemoryDecompressionMethodFlagsEXT decompressionMethods; // bitmask of supported methods
    uint64_t                       maxDecompressionIndirectCount;
} VkPhysicalDeviceMemoryDecompressionPropertiesEXT;
```

**Already cached** as `HardwareCapabilities::memoryDecompression`. Inspect `decompressionMethods` before choosing an on-disk format.

### 3.3 Commands

```cpp
typedef struct VkDecompressMemoryRegionEXT {
    VkDeviceAddress srcAddress;
    VkDeviceAddress dstAddress;
    VkDeviceSize    compressedSize;
    VkDeviceSize    decompressedSize;
    VkMemoryDecompressionMethodFlagsEXT decompressionMethod;
} VkDecompressMemoryRegionEXT;

void vkCmdDecompressMemoryEXT(
    VkCommandBuffer                    commandBuffer,
    uint32_t                           regionCount,
    const VkDecompressMemoryRegionEXT* pRegions);

// Indirect form (when supported by feature/revision):
// vkCmdDecompressMemoryIndirectCountEXT(...)
```

Addresses require buffers created with appropriate usage and BDA. Synchronize with transfer/compute access masks per queue.

---

## Limits

Properties are **implementation-dependent**. Query `capabilities.memoryDecompression` (`HardwareCapabilities` in `src/core/types.hpp`; filled in `src/core/vk_device.cpp`). There are no portable Required Limits minima to invent ([Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html)).

| Field | Meaning | Required minimum |
| --- | --- | --- |
| `decompressionMethods` | Bitmask of supported `VkMemoryDecompressionMethodFlagsEXT` | Implementation-dependent — query `capabilities.memoryDecompression` |
| `maxDecompressionIndirectCount` | Max count for the indirect decompress command | Implementation-dependent — query `capabilities.memoryDecompression` |

Gate asset formats on the bitmask. Indirect count must be ≤ `maxDecompressionIndirectCount`.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep extension name |
| `src/core/vk_device.cpp` | Keep feature + property cache |
| `src/core/types.hpp` | Keep `memoryDecompression` |

### 4.2 Primary consumer — streaming / texture upload — NOT STARTED

**Files:**

- `src/core/texture_manager.cpp` — currently CPU-side load + staging upload
- `src/core/vk_resource_manager.cpp` — transfer submissions
- Transfer queue: `Device::transferQueue` / `getTransferQueue` patterns in device

Flow sketch:

1. Ship assets compressed with a method in `decompressionMethods`.
2. Upload compressed blob to device-local or host-visible buffer (BDA).
3. `vkCmdDecompressMemoryEXT` into final buffer/image staging buffer.
4. Optional: copy decompressed buffer into image.

### 4.3 Format gate

At init, log:

```cpp
const auto& md = capabilities.memoryDecompression;
// md.decompressionMethods, md.maxDecompressionIndirectCount
```

Refuse asset packs that require unsupported methods.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep enablement as-is | `vk_device.*` | Already correct |
| P1 | Log supported methods | `vk_device.cpp` | Decide toolchain format |
| P2 | Prototype single-buffer decompress on transfer queue | resource manager | Prove path |
| P2 | Integrate into texture streaming | `texture_manager.cpp` | Product value |
| P3 | Indirect multi-decompress | GPU-driven IO | Batch streaming |

### Implementation recipe

1. Keep the extension, `{.memoryDecompression = true}`, and the `HardwareCapabilities::memoryDecompression` cache (`src/core/types.hpp`, `vk_device.cpp`).
2. At init, log `decompressionMethods` and `maxDecompressionIndirectCount` from `capabilities.memoryDecompression`. Refuse packs whose method bit is unset.
3. Prototype `vkCmdDecompressMemoryEXT` on the transfer queue in `vk_resource_manager.cpp`. Src/dst need BDA — VMA already has `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` (with `EXT_MEMORY_BUDGET`, `MAINTENANCE4`, `MAINTENANCE5`).
4. Integrate into `texture_manager.cpp` after the compressed blob is on-device, then copy the decompressed buffer into the image (existing `copyBufferToImage` or a later `*2` / host-image-copy path).
5. Only then consider `vkCmdDecompressMemoryIndirectCountEXT`, gated on `maxDecompressionIndirectCount`.

---

## 6. Concrete integration sketch

```cpp
vk::DecompressMemoryRegionEXT region{
    .srcAddress = compressedAddr,
    .dstAddress = decompressedAddr,
    .compressedSize = compSize,
    .decompressedSize = rawSize,
    .decompressionMethod = vk::MemoryDecompressionMethodFlagBitsEXT::eGdeflate1_0, // example; query first
};
transferCmd.decompressMemoryEXT(region);
// barrier then copyBufferToImage / consume SSBO
```

Always verify the method bit against `capabilities.memoryDecompression.decompressionMethods`.

---

## 7. Validation / test checklist

1. Device creates with extension + feature true.
2. Properties show at least one decompression method on target GPUs.
3. Decompress known vector: dst matches golden decompressed bytes.
4. Size fields match codec requirements; no OOB addresses.
5. Barriers correct between decompress and subsequent reads.
6. Indirect count ≤ `maxDecompressionIndirectCount` when used.

---

## 8. Shader interaction

**None (direct).** This extension is host / queue / memory / presentation / resource plumbing only. It does not add shader stages, builtins, storage classes, or SPIR-V capabilities. Shaders continue to use whatever programmable stages the active pipeline or shader objects declare; this feature only changes how the host prepares or submits work those shaders run on.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Buffer device address | Source/dest addressed by BDA (enabled) |
| Transfer/compute queues | Where decompress is submitted |
| `VK_KHR_copy_memory_indirect` | Sibling for indirect copies; different purpose |
| Texture manager staging | Natural integration point |

---

## 10. Non-goals / do not do

- Do not assume a specific compression method without checking properties.
- Do not decompress on the CPU path and GPU path for the same asset without a clear ownership model.
- Do not skip size validation — wrong `decompressedSize` is undefined/invalid.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is Memory Decompression enabled? | **Yes** — extension, feature, properties cached |
| Is it used? | **No** |
| Highest-value next steps | Log methods; prototype transfer-queue decompress for streaming textures |
| Secondary steps | Indirect multi-region; full asset pipeline |
