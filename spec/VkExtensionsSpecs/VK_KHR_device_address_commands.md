# VK_KHR_device_address_commands — Implementation Spec

**Status in this repo: ENABLED (device extension + feature), NOT CONSUMED — DGC/copy paths use `MemoryBarrier2` / core `copyBuffer`; no `MemoryRangeBarrierKHR`, no `copyMemoryKHR` / bind*3.**

Official sources:

- [VK_KHR_device_address_commands](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_device_address_commands.html)
- [Proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_device_address_commands.html)
- [VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR.html)
- [VkMemoryRangeBarrierKHR](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#VkMemoryRangeBarrierKHR)

> Note: Extension name string is `VK_KHR_device_address_commands`. Requires buffer device address, synchronization2, and copy_commands2 (all satisfied by this project’s Vulkan 1.3/1.4 baseline).

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::KHRDeviceAddressCommandsExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → feature chain | `vk::PhysicalDeviceDeviceAddressCommandsFeaturesKHR{ .deviceAddressCommands = true }` |
| Property query | — | **None** (feature-only) |
| Capability cache | `src/core/types.hpp` | **No** dedicated field |
| API usage | resource / DGC barriers | **None** — `pipelineBarrier2` + `MemoryBarrier2` only |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::KHRDeviceAddressCommandsExtensionName,

// src/core/vk_device.cpp (feature chain)
// vk::PhysicalDeviceDeviceAddressCommandsFeaturesKHR
{.deviceAddressCommands = true},
```

### Not wired

- `VkMemoryRangeBarrierKHR` / `VkMemoryRangeBarriersInfoKHR`
- `makeMemoryRangeBarrier` / `pipelineBarrier2MemoryRanges` helpers
- Device log / Tracy plots for range barriers
- `vkCmdBindVertexBuffers3KHR` / `vkCmdBindIndexBuffer3KHR` — no classic VS/index draw path
- `vkCmdCopyMemoryKHR` / memory↔image / fill / update — copies still use core `copyBuffer` / `copyBufferToImage`
- Indirect `*2KHR` / `vkCmdDrawMeshTasksIndirect2EXT` — scene draws are DGC `DRAW_MESH_TASKS`
- `vkCreateAccelerationStructure2KHR` — no RT AS path yet

---

## 2. What the extension provides

Address-based replacements for core (and some extension) commands that still take `VkBuffer` + offset. Goal: pass **device addresses only** instead of buffer handle + address pairs throughout the engine.

| Feature | New API / bit | Engine relevance |
| --- | --- | --- |
| Address ranges | `VkDeviceAddressRangeKHR`, `VkStridedDeviceAddressRangeKHR` | Common building block |
| Address flags | `VkAddressCommandFlagsKHR` (protected, fully bound, usage hints) | `eFullyBound` on committed VMA ranges |
| Vertex/index bind | `vkCmdBindVertexBuffers3KHR`, `vkCmdBindIndexBuffer3KHR` | N/A — mesh shaders, no vertex input |
| Indirect | `vkCmdDrawIndirect2KHR`, `DrawIndexed*`, `DispatchIndirect2KHR`, count variants | N/A — DGC owns draws |
| Transfers | `vkCmdCopyMemoryKHR`, memory↔image, fill, update, query results | Distinct from indirect copies; not consumed |
| Sync | `VkMemoryRangeBarrierKHR` in dependency info | **Consumed** — barriers without buffer objects |
| AS create | `vkCreateAccelerationStructure2KHR` | Future RT |

These commands take `VkDeviceAddressRangeKHR` / `VkStridedDeviceAddressRangeKHR` instead of `VkBuffer` + offset. Shaders are unchanged; only how the host (or a GPU-written argument buffer) names memory changes.

There is no property struct in `HardwareCapabilities`. Enablement is the `deviceAddressCommands` feature only. Keep buffer objects alive for the allocation lifetime even when recording with addresses.

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
typedef struct VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_ADDRESS_COMMANDS_FEATURES_KHR
    void*           pNext;
    VkBool32        deviceAddressCommands;
} VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR;
```

**Already enabled.** Keep `deviceAddressCommands = true` whenever the extension is listed.

### 3.2 Core range types

```cpp
typedef struct VkDeviceAddressRangeKHR {
    VkDeviceAddress address;
    VkDeviceSize    size;
} VkDeviceAddressRangeKHR;

typedef struct VkStridedDeviceAddressRangeKHR {
    VkDeviceAddress address;
    VkDeviceSize    size;
    VkDeviceSize    stride;
} VkStridedDeviceAddressRangeKHR;
```

### 3.3 Address command flags (hints + protected)

```cpp
typedef enum VkAddressCommandFlagBitsKHR {
    VK_ADDRESS_COMMAND_PROTECTED_BIT_KHR                               = 0x00000001,
    VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR                             = 0x00000002,
    VK_ADDRESS_COMMAND_STORAGE_BUFFER_USAGE_BIT_KHR                    = 0x00000004,
    VK_ADDRESS_COMMAND_UNKNOWN_STORAGE_BUFFER_USAGE_BIT_KHR            = 0x00000008,
    VK_ADDRESS_COMMAND_TRANSFORM_FEEDBACK_BUFFER_USAGE_BIT_KHR         = 0x00000010,
    VK_ADDRESS_COMMAND_UNKNOWN_TRANSFORM_FEEDBACK_BUFFER_USAGE_BIT_KHR = 0x00000020,
} VkAddressCommandFlagBitsKHR;
```

Rules of thumb for this engine (no protected memory, no XFB today):

- Set `FULLY_BOUND` when the range is fully backed at access time (typical for committed VMA allocations).
- Set storage usage bit **or** unknown **or** neither (neither means “must not be SSBO usage”) consistently with buffer create flags.
- Never combine STORAGE with UNKNOWN_STORAGE (mutually exclusive).
- Current consumers are not created as SSBO, but VMA may pack them with STORAGE buffers. Default flags are `eFullyBound | eUnknownStorageBufferUsage` (VUID-VkMemoryRangeBarrierKHR-addressRange-13122).

### 3.4 Memory-range barriers (consumed)

```cpp
typedef struct VkMemoryRangeBarrierKHR {
    VkStructureType             sType;
    const void*                 pNext;
    VkPipelineStageFlags2       srcStageMask;
    VkAccessFlags2              srcAccessMask;
    VkPipelineStageFlags2       dstStageMask;
    VkAccessFlags2              dstAccessMask;
    uint32_t                    srcQueueFamilyIndex;
    uint32_t                    dstQueueFamilyIndex;
    VkDeviceAddressRangeKHR     addressRange;
    VkAddressCommandFlagsKHR    addressFlags;
} VkMemoryRangeBarrierKHR;

typedef struct VkMemoryRangeBarriersInfoKHR {
    VkStructureType                   sType;
    const void*                       pNext;
    uint32_t                          memoryRangeBarrierCount;
    const VkMemoryRangeBarrierKHR*    pMemoryRangeBarriers;
} VkMemoryRangeBarriersInfoKHR;
```

Chain `VkMemoryRangeBarriersInfoKHR` on `VkDependencyInfo::pNext`. Default queue-family fields are `0` — **must** set `VK_QUEUE_FAMILY_IGNORED` (buffers are concurrent). Stage/access pairing follows the same VUIDs as `VkMemoryBarrier2` (do not pair `eIndirectCommandRead` with `eTransfer` alone).

### 3.5 Other commands (not consumed)

```cpp
void vkCmdBindIndexBuffer3KHR(VkCommandBuffer, const VkBindIndexBuffer3InfoKHR*);
void vkCmdBindVertexBuffers3KHR(VkCommandBuffer, uint32_t firstBinding, uint32_t bindingCount,
                                const VkBindVertexBuffer3InfoKHR* pBindingInfos);
void vkCmdCopyMemoryKHR(VkCommandBuffer, const VkCopyDeviceMemoryInfoKHR*);
void vkCmdCopyMemoryToImageKHR(VkCommandBuffer, const VkCopyDeviceMemoryImageInfoKHR*);
void vkCmdFillMemoryKHR(VkCommandBuffer, const VkDeviceAddressRangeKHR*, VkAddressCommandFlagsKHR, uint32_t data);
```

Vulkan-Hpp naming follows the usual `commandBuffer.copyMemoryKHR(...)` pattern.

---

## Limits

Feature `deviceAddressCommands` only. **No property** in `HardwareCapabilities`.

Address-based draw / dispatch / mesh variants use the same 64-bit BDAs as [VK_KHR_buffer_device_address](./VK_KHR_buffer_device_address.md). Related command-count caps (for example `maxDrawIndirectCount` with `multiDrawIndirect`) are core limits, not this extension. Do not invent extra numbers. See [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html).

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep `vk::KHRDeviceAddressCommandsExtensionName` |
| `src/core/vk_device.cpp` | Keep `{.deviceAddressCommands = true}`; log `deviceAddressCommands: enabled` |

### 4.2 Geometry binds — N/A

**File:** `src/render/vk_renderer.cpp`

Scene draws are mesh + DGC `DRAW_MESH_TASKS`. There is no `bindVertexBuffers` / `bindIndexBuffer` / `drawIndexed`. Do not reintroduce a classic VS fallback to consume bind3.

### 4.3 Resource uploads / copies — sibling extension

**Files:** `src/core/vk_resource_manager.cpp`

`copyBuffer` / `copyBufferToImage` already use `copyMemoryIndirectKHR` / `copyMemoryToImageIndirectKHR`. Do not replace those with `copyMemoryKHR`. STB textures stay on host image copy.

### 4.4 Barriers — CONSUMED

**Files:** `src/util/vk_utils.*`, `src/core/vk_resource_manager.cpp`, `src/render/vk_dgc.cpp`

`VkMemoryRangeBarriersInfoKHR` on host-written BDA ranges. Image layout transitions stay on `VkImageMemoryBarrier2`.

### 4.5 Ray tracing (future)

Prefer `vkCreateAccelerationStructure2KHR` when AS backing is address-centric (host AS commands incompatible with address-created AS).

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep enablement as-is | `vk_device.*` | Already correct |
| P1 | Address-based vertex/index bind | — | **Skipped** — no VS/index path |
| P2 | Staging copies via `copyMemory*` | — | **Skipped** — `copy_memory_indirect` owns copies |
| P2 | Memory-range barriers | `vk_utils.*`, `vk_resource_manager.cpp`, `vk_dgc.cpp` | **Done** |
| P3 | Indirect `*2KHR` draws | — | DGC owns draws |
| P3 | AS create2 | RT path | When AS code is address-first |

### Implementation recipe

1. Keep the extension + `deviceAddressCommands = true`. No property field in `HardwareCapabilities`.
2. Do not add vertex/index binds. Mesh + DGC is the only scene draw path.
3. Do not replace `copyMemoryIndirectKHR` with `copyMemoryKHR`.
4. Replace global `VkMemoryBarrier2` on known BDA writes with `VkMemoryRangeBarrierKHR` (`eFullyBound`, `QUEUE_FAMILY_IGNORED`).
5. Keep buffer objects alive while addresses are in flight.

---

## 6. Concrete integration sketch (barriers)

```cpp
const vk::MemoryRangeBarrierKHR hostRanges[] = {
    makeMemoryRangeBarrier(vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite,
                           vk::PipelineStageFlagBits2::eCopyIndirectKHR,
                           vk::AccessFlagBits2::eIndirectCommandRead, commandListAddress, stride),
    makeMemoryRangeBarrier(vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite,
                           vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead,
                           srcAddress, size),
};
pipelineBarrier2MemoryRanges(cmd, hostRanges);
```

Keep buffer objects alive for allocation lifetime even when recording with addresses.

---

## 7. Validation / test checklist

1. Device creates with extension + `deviceAddressCommands = true`; log line present.
2. Geometry load: no VUID on `dstAccessMask` vs stage (split ranges: `eCopyIndirectKHR`+`eIndirectCommandRead` vs `eTransfer`+`eTransferRead`).
3. Flags match buffer usage (no STORAGE bit on INDIRECT/TRANSFER_SRC/PREPROCESS buffers).
4. Queue families are `IGNORED` (not default `0`).
5. DGC preprocess: sequence range visible to `eCommandPreprocessEXT`; preprocess range visible to execute when size ≠ 0.
6. Robustness is relative to the **range**, not only the parent buffer.

---

## 8. Shader interaction

**Indirect — host/sync setup for address-named memory.** No new SPIR-V stages. Compute that later fills argument buffers writes device addresses; mesh/FS/RT that execute as a result are unchanged. Argument buffers need `SHADER_DEVICE_ADDRESS`. Depends on [VK_KHR_buffer_device_address](./VK_KHR_buffer_device_address.md).

### Engine status

Enabled, not consumed. Draw/bind/direct-copy and memory-range barriers unused.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Vulkan 1.2 `bufferDeviceAddress` | Required (enabled) |
| Vulkan 1.3 `synchronization2` | Required — `pipelineBarrier2` + range info |
| Vulkan 1.3 copy_commands2 | Transfer command basis (core) |
| `VK_KHR_copy_memory_indirect` | Enabled, not consumed — uploads still use core copies |
| `VK_EXT_device_generated_commands` | Sequence / preprocess BDA ranges |
| `VK_EXT_mesh_shader` | Adds mesh indirect address variants (not used; DGC draws) |
| `VK_KHR_acceleration_structure` | AS create2 interaction (future) |

---

## 10. Non-goals / do not do

- Do not free/destroy buffers while addresses remain referenced by in-flight command buffers.
- Do not set protected address flags without protected memory.
- Do not assume host AS builds work with `CreateAccelerationStructure2KHR`.
- Do not reintroduce `bindVertexBuffers` / `drawIndexed` to consume bind3.
- Do not invent `copyMemoryIndirectKHR` or `copyMemoryKHR` for current single-region uploads.
- Do not invent `drawMeshTasksIndirect2EXT` alongside DGC.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is Device Address Commands enabled? | **Yes** — extension listed, `deviceAddressCommands = true` |
| Is it used? | **No** — DGC/copy use `MemoryBarrier2` / core `copyBuffer` |
| Why not bind3 / copyMemoryKHR? | Mesh+DGC only; copies still core `copyBuffer` |
| Remaining | Indirect `*2KHR`, AS create2 if those paths appear |
