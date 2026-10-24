# VK_KHR_copy_memory_indirect — Implementation Spec

**Status in this repo: ENABLED (device extension + both feature bits), NOT CONSUMED — uploads still use core `copyBuffer` / `copyBufferToImage`. Properties not queried. No `copyMemoryIndirectKHR` / `copyMemoryToImageIndirectKHR`.**

Official sources:

- [VK_KHR_copy_memory_indirect](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_copy_memory_indirect.html)
- [VkPhysicalDeviceCopyMemoryIndirectFeaturesKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceCopyMemoryIndirectFeaturesKHR.html)
- [vkCmdCopyMemoryIndirectKHR](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyMemoryIndirectKHR.html)
- [vkCmdCopyMemoryToImageIndirectKHR](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyMemoryToImageIndirectKHR.html)

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::KHRCopyMemoryIndirectExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → feature chain | `indirectMemoryCopy = true`, `indirectMemoryToImageCopy = true` |
| Property query | `src/core/vk_device.cpp` → properties chain | **Missing** — `vk::PhysicalDeviceCopyMemoryIndirectPropertiesKHR` not chained |
| Capability cache | `src/core/types.hpp` | **Missing** — no `HardwareCapabilities::copyMemoryIndirect` |
| API usage | `src/core/vk_resource_manager.cpp` | **None** — `copyBuffer` uses `vk::CommandBuffer::copyBuffer`; `copyBufferToImage` uses `copyBufferToImage`. STB textures stay on host image copy. |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::KHRCopyMemoryIndirectExtensionName,

// src/core/vk_device.cpp (feature chain)
// vk::PhysicalDeviceCopyMemoryIndirectFeaturesKHR
{.indirectMemoryCopy = true, .indirectMemoryToImageCopy = true},
```

### Not wired

- `vkCmdCopyMemoryIndirectKHR` / `vk::CopyMemoryIndirectInfoKHR`
- `vkCmdCopyMemoryToImageIndirectKHR` / `vk::CopyMemoryToImageIndirectInfoKHR`
- Host-filled BDA command buffer
- `supportedQueues` query / log / queue pick
- Tracy plots for indirect copies
- GPU/compute-filled copy-command lists
- Texture page streaming / replacing host image copy
- Pairing with `VK_EXT_memory_decompression`

---

## 2. What the extension provides

**Indirect** device-memory copies: the GPU (or host) fills a buffer of copy commands executed with a single cmd. Enables GPU-driven upload/relocation without CPU rewrite of copy regions.

CPU-built copy region lists do not scale for GPU-driven streaming. This remains a **device extension** (not promoted). This repo enables both `indirectMemoryCopy` and `indirectMemoryToImageCopy` at device create, but does not issue the cmds.

It does **not** replace `VK_KHR_copy_commands2` (immediate `*2` copies) and does not make a compute-filled command buffer visible without barriers. Address alignment of the indirect command buffer is the app’s responsibility.

| Feature | New API / bit | Engine relevance |
| --- | --- | --- |
| Memory→memory indirect | `indirectMemoryCopy` + `vkCmdCopyMemoryIndirectKHR` | Streaming, compaction |
| Memory→image indirect | `indirectMemoryToImageCopy` + `vkCmdCopyMemoryToImageIndirectKHR` | Texture page streaming |
| Address-based regions | Commands carry device addresses | Fits BDA-centric design |

Distinct from `VK_KHR_device_address_commands` direct `vkCmdCopyMemoryKHR` (immediate, not indirect buffer).

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
typedef struct VkPhysicalDeviceCopyMemoryIndirectFeaturesKHR {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_FEATURES_KHR
    void*           pNext;
    VkBool32        indirectMemoryCopy;
    VkBool32        indirectMemoryToImageCopy;
} VkPhysicalDeviceCopyMemoryIndirectFeaturesKHR;
```

**Already enabled** both bits.

### 3.2 Command structures (device-memory layout)

```cpp
typedef struct VkCopyMemoryIndirectCommandKHR {
    VkDeviceAddress srcAddress;
    VkDeviceAddress dstAddress;
    VkDeviceSize    size;
} VkCopyMemoryIndirectCommandKHR;

typedef struct VkCopyMemoryToImageIndirectCommandKHR {
    VkDeviceAddress            srcAddress;
    uint32_t                   bufferRowLength;
    uint32_t                   bufferImageHeight;
    VkImageSubresourceLayers   imageSubresource;
    VkOffset3D                 imageOffset;
    VkExtent3D                 imageExtent;
} VkCopyMemoryToImageIndirectCommandKHR;
```

### 3.3 Commands (SDK 1.4.357 / current KHR)

```cpp
void vkCmdCopyMemoryIndirectKHR(
    VkCommandBuffer                     commandBuffer,
    const VkCopyMemoryIndirectInfoKHR*  pCopyMemoryIndirectInfo);

void vkCmdCopyMemoryToImageIndirectKHR(
    VkCommandBuffer                            commandBuffer,
    const VkCopyMemoryToImageIndirectInfoKHR*  pCopyMemoryToImageIndirectInfo);
```

`VkCopyMemoryIndirectInfoKHR` carries `srcCopyFlags` / `dstCopyFlags` (`VkAddressCopyFlagsKHR`) plus `copyAddressRange` (`VkStridedDeviceAddressRangeKHR`: address, size, stride). Use `stride = sizeof(command)` unless packing denser with validated stride rules.

### 3.4 Properties

```cpp
typedef struct VkPhysicalDeviceCopyMemoryIndirectPropertiesKHR {
    VkStructureType  sType;
    void*            pNext;
    VkQueueFlags     supportedQueues; // queue types that may record the cmds
} VkPhysicalDeviceCopyMemoryIndirectPropertiesKHR;
```

Not cached. Command pool / submit queue must match one of these bits once queried.

---

## Limits

Gate on the two feature bits, `supportedQueues`, and 4-byte alignment of the copy-command address. No Required Limits numbers to invent ([Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html)).

| Gate | Meaning |
| --- | --- |
| `indirectMemoryCopy` | Enables `vkCmdCopyMemoryIndirectKHR` — **feature true, cmd unused** |
| `indirectMemoryToImageCopy` | Enables `vkCmdCopyMemoryToImageIndirectKHR` — **feature true, cmd unused** |
| `supportedQueues` | Which queue types may record the cmds — **not queried** |
| Command-buffer address | BDA; 4-byte aligned |
| `stride` | Usually `sizeof(command)`; must satisfy validated stride rules |

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — extension + features only

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep extension name |
| `src/core/vk_device.cpp` | Keep both feature bits true; **add** property query + log `supportedQueues` |
| `src/core/types.hpp` | **Add** `HardwareCapabilities::copyMemoryIndirect` |

### 4.2 Primary consumers — not started

| Use case | File(s) | State |
| --- | --- | --- |
| Geometry staging → device-local | `vk_resource_manager.cpp` `copyBuffer` | Core `copyBuffer` only |
| Buffer → image helper | `vk_resource_manager.cpp` `copyBufferToImage` | Core `copyBufferToImage`; no current caller (STB uses host image copy) |
| Compute-built copy lists | future compute pass | Not started |

Requires BDA on the indirect command buffer and on src/dst resources (staging uploads already request `eShaderDeviceAddress`).

### 4.3 Queue notes

Submit on a queue that supports transfer. Prefer `transferQueue` when dedicated. Ensure ownership/layout for images before memory→image indirect copies.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep enablement as-is | `vk_device.*` | Extension + both feature bits already true |
| P0 | Cache `supportedQueues` | `types.hpp`, `vk_device.cpp` | Queue pick needs the property |
| P1 | Memory→memory indirect | `vk_resource_manager.cpp` `copyBuffer` | Geometry staging still uses core copy |
| P1 | Memory→image helper | `copyBufferToImage` | Helper unused by STB path |
| P2 | Compute fills command buffer | compute path | Not started |
| P3 | Combine with memory decompression | streaming stack | Not started |

---

## 6. Concrete integration sketch

```cpp
vk::CopyMemoryIndirectCommandKHR cmd{
    .srcAddress = srcAddr, .dstAddress = dstAddr, .size = bytes};
// host-write into BDA command buffer, then:
transferCmd.copyMemoryIndirectKHR(vk::CopyMemoryIndirectInfoKHR{
    .srcCopyFlags = {},
    .dstCopyFlags = vk::AddressCopyFlagBitsKHR::eDeviceLocal,
    .copyCount = 1,
    .copyAddressRange = {.address = cmdAddr, .size = sizeof(cmd), .stride = sizeof(cmd)},
});
```

Image path fills `CopyMemoryToImageIndirectCommandKHR` then `copyMemoryToImageIndirectKHR` with destination in `TRANSFER_DST`.

---

## 7. Validation / test checklist

1. Device creates with extension + both feature bits true.
2. Indirect command buffer usage flags / address valid.
3. `copyCount` and `stride` satisfy alignment rules.
4. Src/dst ranges non-overlapping unless permitted; sizes match.
5. Image layouts and subresources valid for memory→image.
6. Compare results against equivalent direct `copyBuffer` / `copyBufferToImage`.

---

## 8. Shader interaction

**Indirect.** GPU may execute copies from device-addressed command buffers. Compute shaders can generate copy commands; graphics/RT shaders only observe results after sync. No new graphics stages.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Buffer device address | Indirect cmd buffer + copy endpoints |
| `VK_KHR_device_address_commands` | Direct address copies unused. `MemoryRangeBarrierKHR` would sync a future host-filled command list + staging src |
| `VK_EXT_memory_decompression` | Decompress then indirect copy |
| Transfer queue in `Device` | Preferred submit path |
| Synchronization2 | Barriers around indirect copies |

---

## 10. Non-goals / do not do

- Do not enable only one feature bit if both paths are planned — both already true.
- Do not issue indirect copies without ensuring the command buffer contents are visible to the queue (barriers / submits).
- Do not replace trivial single-region CPU-known copies with indirect unless batching pays off.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is Copy Memory Indirect enabled? | **Yes** — extension + both feature bits. Properties not queried |
| Is it used? | **No** — geometry `copyBuffer` and `copyBufferToImage` stay on core cmds |
| Not done | Property cache; host-filled cmds; compute-generated lists; streaming; decompression pairing |
