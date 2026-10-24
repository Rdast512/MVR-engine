# VK_KHR_maintenance8 — Implementation Spec

**Status in this repo: ENABLED (device extension + feature), NOT CONSUMED — `transitionImageLayout` does not set QFOT `USE_ALL_STAGES`; callers pass `QUEUE_FAMILY_IGNORED`. No pipeline cache / AccessFlags3 / depth↔color copy / 3D blit.**

Official sources:

- [VK_KHR_maintenance8](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_maintenance8.html)
- [Proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_maintenance8.html)
- [VkPhysicalDeviceMaintenance8FeaturesKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMaintenance8FeaturesKHR.html)
- [VkMemoryBarrierAccessFlags3KHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryBarrierAccessFlags3KHR.html)

> Note: There is **no** dedicated property struct for maintenance8. Feature enable is the only device-create hook.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::KHRMaintenance8ExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → `createLogicalDevice()` feature chain | `vk::PhysicalDeviceMaintenance8FeaturesKHR{ .maintenance8 = true }` |
| Property query | `src/core/vk_device.cpp` → properties chain | **N/A** — no `PhysicalDeviceMaintenance8PropertiesKHR` |
| Capability cache | `src/core/types.hpp` | **None** |
| API usage | barriers / copies / pipeline cache / blits | **None** — `transitionImageLayout` never sets `eQueueFamilyOwnershipTransferUseAllStagesKHR`. Callers pass `QUEUE_FAMILY_IGNORED`. No pipeline cache, AccessFlags3, depth↔color copy, or 3D blit. |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::KHRMaintenance8ExtensionName,

// src/core/vk_device.cpp (feature chain)
// vk::PhysicalDeviceMaintenance8FeaturesKHR
{.maintenance8 = true},

// src/util/vk_utils.cpp — DependencyInfo has no QFOT USE_ALL_STAGES flag
```

### Not wired (no existing call site — do not invent)

- `DependencyFlagBits::eQueueFamilyOwnershipTransferUseAllStagesKHR` on `transitionImageLayout`
- Depth/stencil ↔ matching color image copies
- `VK_PIPELINE_CACHE_CREATE_INTERNALLY_SYNCHRONIZED_MERGE_BIT_KHR` (no pipeline cache)
- `Offset` image operand / OpSRem/OpSMod — SPIR-V only
- `VkMemoryBarrierAccessFlags3KHR` (no access3 bit in use)
- Relaxed 3D → non-3D blit layer rules (mipgen is 2D same-image)

---

## 2. What the extension provides

Maintenance8 is a **cumulative maintenance bag**: small, independent host/SPIR-V/sync fixes. It is still a **device extension** in this repo (`vk::KHRMaintenance8ExtensionName` + `Maintenance8FeaturesKHR`) — feature-only, no property struct. It is **not a replacement** for earlier maintenance (4–7 limits/properties still apply); later bags (9–10) add more on top.

| Feature | New API / bit | Engine relevance |
| --- | --- | --- |
| Depth/stencil ↔ color matching copies | copy format rules relaxed | Debug viz / packing depth into color buffers |
| Internally synchronized pipeline cache merge | `PIPELINE_CACHE_CREATE_INTERNALLY_SYNCHRONIZED_MERGE_BIT_KHR` | Multi-threaded `mergePipelineCaches` without external lock |
| QFOT uses all stages | `DEPENDENCY_QUEUE_FAMILY_OWNERSHIP_TRANSFER_USE_ALL_STAGES_BIT_KHR` | Correct QFOT when stage masks would otherwise be incomplete |
| `Offset` image operand | SPIR-V capability / operand | Dynamic texel offsets in sampling |
| OpSRem / OpSMod for negatives | SPIR-V behavior | Well-defined signed remainder/modulo |
| Access flags 3 | `VkMemoryBarrierAccessFlags3KHR` | Extra 64 access-flag bits on sync2 barriers |
| 3D blit layer relax | blit region rules | Blitting 3D slices to 2D / array images more freely |

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
// Provided by VK_KHR_maintenance8
typedef struct VkPhysicalDeviceMaintenance8FeaturesKHR {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR
    void*           pNext;
    VkBool32        maintenance8; // master switch
} VkPhysicalDeviceMaintenance8FeaturesKHR;
```

**Already enabled.** Keep `maintenance8 = true` whenever the extension name is listed.

### 3.2 Access flags 3 (pNext of sync2 barriers)

```cpp
// Provided by VK_KHR_maintenance8
typedef struct VkMemoryBarrierAccessFlags3KHR {
    VkStructureType    sType; // VK_STRUCTURE_TYPE_MEMORY_BARRIER_ACCESS_FLAGS_3_KHR
    const void*        pNext;
    VkAccessFlags3KHR  srcAccessMask3;
    VkAccessFlags3KHR  dstAccessMask3;
} VkMemoryBarrierAccessFlags3KHR;
```

Valid hosts (pNext chain):

- `VkMemoryBarrier2`
- `VkBufferMemoryBarrier2`
- `VkImageMemoryBarrier2`
- (and related range-barrier infos where the extension allows)

Vulkan-Hpp sketch:

```cpp
vk::MemoryBarrierAccessFlags3KHR access3{
    .srcAccessMask3 = {},
    .dstAccessMask3 = {}
};

vk::ImageMemoryBarrier2 barrier{
    .pNext = &access3,
    .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
    .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
    // ...
};
```

Use only when a new `VkAccessFlagBits3KHR` bit is actually required; do not chain empty access3 on every barrier.

### 3.3 Pipeline cache create flag

| Flag | Meaning |
| --- | --- |
| `VK_PIPELINE_CACHE_CREATE_INTERNALLY_SYNCHRONIZED_MERGE_BIT_KHR` | Implementation synchronizes merges into this cache; concurrent `vkMergePipelineCaches` with this cache as `dstCache` is safe |

```cpp
vk::PipelineCacheCreateInfo cacheInfo{
    .flags = vk::PipelineCacheCreateFlagBits::eInternallySynchronizedMergeKHR
};
```

### 3.4 QFOT dependency flag

| Flag | Meaning |
| --- | --- |
| `VK_DEPENDENCY_QUEUE_FAMILY_OWNERSHIP_TRANSFER_USE_ALL_STAGES_BIT_KHR` | On ownership-transfer dependencies, treat stage masks as covering all stages needed for the transfer |

```cpp
vk::ImageMemoryBarrier2 barrier{
    // ...
    // on the dependency / barrier flags field where DependencyFlags apply:
};
// For VkDependencyInfo:
vk::DependencyInfo dep{
    .dependencyFlags =
        vk::DependencyFlagBits::eQueueFamilyOwnershipTransferUseAllStagesKHR,
    // ...
};
```

### 3.5 Depth/stencil ↔ color copies

When `maintenance8` is enabled, copies between depth/stencil images and “matching” color formats are allowed (size/component compatibility per the extension). Use existing `vkCmdCopyImage` / `copyImage2` — no new command.

### 3.6 SPIR-V / blit behavior (no new host structs)

- Image operands may use `Offset` as an alternative to `ConstOffset` where the extension permits.
- `OpSRem` / `OpSMod` follow SPIR-V rules for negative operands (well-defined).
- Blits from 3D images to other image types have loosened layer restrictions.

---

## Limits

See [Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html) and [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html#limits-required).

**No dedicated limits struct.** Maintenance8 is feature-only in this repo: there is no `VkPhysicalDeviceMaintenance8Properties` on the properties chain and **no** `HardwareCapabilities::maintenance8` field. Behavioral only (Offset image operand on more ops; QFOT refinements via `USE_ALL_STAGES`; AccessFlags3; depth/stencil ↔ color copies; etc.), not numeric min/max.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep `vk::KHRMaintenance8ExtensionName` |
| `src/core/vk_device.cpp` | Keep `{.maintenance8 = true}` |

No property cache needed.

### 4.2 Pipeline cache merge synchronization

**Files:** pipeline cache create path under `src/render/vk_pipeline.cpp` (or wherever caches are created).

If multi-threaded pipeline compilation merges into a shared cache:

```cpp
vk::PipelineCacheCreateInfo info{
    .flags = vk::PipelineCacheCreateFlagBits::eInternallySynchronizedMergeKHR,
    .initialDataSize = blob.size(),
    .pInitialData = blob.data()
};
```

If merges are single-threaded today, this flag is optional safety for future parallelism.

### 4.3 Queue family ownership transfers

**Files:**

- `src/core/vk_resource_manager.cpp`
- `src/core/texture_manager.cpp`
- any exclusive-sharing image/buffer handoff between graphics / transfer / compute

`transitionImageLayout` already accepts `srcQueueFamily` / `dstQueueFamily`. When both are not `QUEUE_FAMILY_IGNORED` and they differ, the helper now sets `eQueueFamilyOwnershipTransferUseAllStagesKHR` on the `DependencyInfo`. Concurrent paths (current callers) stay unchanged.

### 4.4 AccessFlags3

**Files:** same barrier helpers as above.

Only introduce `MemoryBarrierAccessFlags3KHR` when a specific access3 bit is required by a future extension interaction. Do not rewrite existing sync2 barriers preemptively.

### 4.5 Depth/stencil ↔ color copies

**Files:** debug/visualization helpers (future), `vk_resource_manager.cpp` copy utilities.

Useful for packing depth into a color image for ImGui/debug display without a full-screen shader. Gate on maintenance8 being required (already is).

### 4.6 Shader compiler path

OpSRem/OpSMod and `Offset` operands are SPIR-V / glslang / slang concerns. No C++ engine change unless the engine authors raw SPIR-V. Document as compile-time benefit of enabling the feature.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep enablement as-is | `vk_device.*` | Already correct |
| P1 | QFOT `USE_ALL_STAGES` on multi-queue transfers | `vk_utils.cpp` | **Done** on existing transition helper |
| P2 | Internally synchronized pipeline cache | pipeline cache create | Future multi-threaded compile |
| P2 | Depth↔color copy helper (if debug viz needs it) | resource manager | Avoid custom shader for depth preview |
| P3 | AccessFlags3 | barrier helpers | Only when a concrete access3 bit is needed |
| P3 | Rely on SPIR-V Offset / SRem behavior | shaders | No host code |

### Implementation recipe

1. Confirm `src/core/types.hpp` → `HardwareCapabilities` caches `maintenance3`, `maintenance4`, `maintenance5`, `maintenance6`, `maintenance7`, `maintenance9`, and `maintenance10`. There is **no `maintenance8` properties field** — do not add one.
2. Confirm `src/core/vk_device.cpp` feature chain sets `Maintenance8FeaturesKHR{ .maintenance8 = true }` and that the **property** chain skips maintenance8. Keep `vk::KHRMaintenance8ExtensionName` in `requiredDeviceExtension`.
3. Confirm `src/core/vk_allocator.hpp` already sets `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT` and `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT` (no VMA bit for maintenance8).
4. On remaining exclusive QFOT barriers, set `eQueueFamilyOwnershipTransferUseAllStagesKHR` (pair with maintenance9 buffer/linear QFOT skip).
5. Optionally set `eInternallySynchronizedMergeKHR` on pipeline caches; introduce `MemoryBarrierAccessFlags3KHR` only when a concrete access3 bit is required.

---

## 6. Concrete integration sketch (QFOT)

```cpp
// When releasing an exclusive image from transfer → graphics:
vk::ImageMemoryBarrier2 release{
    .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
    .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
    .dstStageMask = vk::PipelineStageFlagBits2::eNone,
    .dstAccessMask = vk::AccessFlagBits2::eNone,
    .oldLayout = vk::ImageLayout::eTransferDstOptimal,
    .newLayout = vk::ImageLayout::eTransferDstOptimal,
    .srcQueueFamilyIndex = transferFamily,
    .dstQueueFamilyIndex = graphicsFamily,
    .image = image,
    .subresourceRange = range
};

vk::DependencyInfo dep{
    .dependencyFlags =
        vk::DependencyFlagBits::eQueueFamilyOwnershipTransferUseAllStagesKHR,
    .imageMemoryBarrierCount = 1,
    .pImageMemoryBarriers = &release
};

cmd.pipelineBarrier2(dep);
```

Mirror acquire on the graphics queue with the same dependency flag if needed.

---

## 7. Validation / test checklist

1. Device creates with `VK_KHR_maintenance8` + `maintenance8 = true`.
2. Multi-queue exclusive image/buffer transfers validate clean with `USE_ALL_STAGES` set.
3. Pipeline cache created with `INTERNALLY_SYNCHRONIZED_MERGE` still loads/stores correctly.
4. Depth→color copy of a “matching” format validates when used.
5. No empty `MemoryBarrierAccessFlags3KHR` chained on every barrier (avoid noise).
6. Shader path: negative OpSRem/OpSMod and dynamic Offset sampling compile and run if used.

---

## 8. Shader interaction

**Indirect only.** No new shader stages or builtins. Shaders are unaffected in source form; this feature changes host-side pipeline/render-pass/resource/sync setup that must stay compatible with the stages already bound (vertex/mesh/fragment/compute/RT). Ensure push constants / heap binds / attachments still match the active shader I/O when using this path.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Vulkan 1.1+ | Extension dependency baseline |
| Vulkan 1.3 `synchronization2` | Host for AccessFlags3 and modern barriers (enabled) |
| Multi-queue Device (graphics/transfer/compute) | QFOT consumers |
| Pipeline caches | Internally synchronized merge |
| `VK_KHR_maintenance9` | Further QFOT relaxations (buffers/linear may skip QFOT) |
| SPIR-V toolchain | Offset operand, OpSRem/OpSMod |

---

## 10. Non-goals / do not do

- Do not invent a `HardwareCapabilities::maintenance8` property field — none exists.
- Do not chain AccessFlags3 on barriers without a real access3 bit.
- Do not force depth↔color copies into the hot path; keep as optional utility.
- Do not remove external pipeline-cache locking until the create flag is set **and** merge concurrency is validated.
- Do not treat SPIR-V behavior changes as requiring renderer rewrites.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is Maintenance8 enabled? | **Yes** — extension + feature; no properties |
| Is it used? | **No** — QFOT `USE_ALL_STAGES` not set; no cache / AccessFlags3 / depth-copy sites |
| Highest-value next steps | None until exclusive QFOT callers, a pipeline cache, or a concrete access3 bit exist |
| Secondary steps | Depth↔color copy utility; AccessFlags3 when needed |
