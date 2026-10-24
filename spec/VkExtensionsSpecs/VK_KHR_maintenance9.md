# VK_KHR_maintenance9 — Implementation Spec

**Status in this repo: ENABLED (device extension + properties; feature **false** for VVL #12449), PARTIALLY CONSUMED (per-family QFOT mask log). `PhysicalDeviceMaintenance9PropertiesKHR` cached, not logged. No buffer/linear QFOT to drop (concurrent). No query pools / vertex input / 3D-array-compatible / asymmetric events.**

Official sources:

- [VK_KHR_maintenance9](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_maintenance9.html)
- [Proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_maintenance9.html)
- [VkPhysicalDeviceMaintenance9FeaturesKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMaintenance9FeaturesKHR.html)
- [VkPhysicalDeviceMaintenance9PropertiesKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMaintenance9PropertiesKHR.html)
- [VkQueueFamilyOwnershipTransferPropertiesKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkQueueFamilyOwnershipTransferPropertiesKHR.html)

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::KHRMaintenance9ExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → `createLogicalDevice()` feature chain | `vk::PhysicalDeviceMaintenance9FeaturesKHR{ .maintenance9 = false }` — VVL #12449 |
| Property query | `src/core/vk_device.cpp` → properties chain | `vk::PhysicalDeviceMaintenance9PropertiesKHR` queried |
| Capability cache | `src/core/types.hpp` → `HardwareCapabilities::maintenance9` | Cached (`image2DViewOf3DSparse`, `defaultVertexAttributeValue`) |
| Queue-family ownership props | `getQueueFamilyProperties2` + `QueueFamilyOwnershipTransferPropertiesKHR` | **Queried and logged** per family (`optimalImageTransferToQueueFamilies`) |
| API usage | query pools / events / barriers / device create | **Partial** — QFOT mask logged. Maintenance9 properties cached, not logged. No exclusive buffer/linear QFOT (concurrent sharing). |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::KHRMaintenance9ExtensionName,

// src/core/vk_device.cpp (feature chain)
// vk::PhysicalDeviceMaintenance9FeaturesKHR
{.maintenance9 = false}, // VVL #12449

// src/core/vk_device.cpp (properties chain)
capabilities.maintenance9 =
    propertiesChain.get<vk::PhysicalDeviceMaintenance9PropertiesKHR>();
// cached only — no init log of image2DViewOf3DSparse / defaultVertexAttributeValue
// per-family QueueFamilyOwnershipTransferPropertiesKHR is logged

// src/core/types.hpp
vk::PhysicalDeviceMaintenance9PropertiesKHR maintenance9;
```

### Not wired (no existing call site — do not invent)

- Devices with zero queues
- Asymmetric `setEvent2`
- `VK_QUERY_POOL_CREATE_RESET_BIT_KHR` (no query pools)
- Integer bitwise bitwidth relaxation (shader path)
- Sparse 2D-view-of-3D / `image2DViewOf3DSparse` gating
- Unbound vertex-attribute asserts (mesh path, no vertex input)
- 3D `2D_ARRAY_COMPATIBLE` slice barriers
- Dropping buffer/linear QFOT — none exist (concurrent sharing)

---

## 2. What the extension provides

Maintenance9 is a **cumulative maintenance bag**: small, independent device/query/QFOT/shader fixes. It is still a **device extension** in this repo (`vk::KHRMaintenance9ExtensionName` + properties cache). It is **not a replacement** for earlier maintenance (including maintenance8 QFOT `USE_ALL_STAGES`); later maintenance10 adds more on top. The extension string and property query stay; the feature bit is currently forced **false** for VVL #12449 (see §5 recipe).

| Feature | New API / bit | Engine relevance |
| --- | --- | --- |
| Device with no queues | create device with empty queue create infos | Offline pipeline cache bake tools |
| Asymmetric setEvent2 | `VK_DEPENDENCY_ASYMMETRIC_EVENT_BIT_KHR` | `setEvent2` without full dependency (setEvent-style) |
| Query pool create-reset | `VK_QUERY_POOL_CREATE_RESET_BIT_KHR` | Skip first `cmdResetQueryPool` |
| Integer bitwise any bitwidth | SPIR-V relaxation | Shader convenience |
| Sparse 2D view of 3D | `image2DViewOf3DSparse` property | Only with `VK_EXT_image_2d_view_of_3d` + sparse |
| Default vertex attribute value | `defaultVertexAttributeValue` enum property | Document unbound attribute behavior |
| 3D barrier slice scoping | behavioral for `VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT` | Correct partial-slice barriers on 3D images |
| QFOT not required for buffers/linear | behavioral + queue-family property for optimal | Fewer barriers on multi-queue uploads |

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
// Provided by VK_KHR_maintenance9
typedef struct VkPhysicalDeviceMaintenance9FeaturesKHR {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_9_FEATURES_KHR
    void*           pNext;
    VkBool32        maintenance9;
} VkPhysicalDeviceMaintenance9FeaturesKHR;
```

**Listed, feature false.** Keep the extension name. Leave `.maintenance9 = false` until VVL #12449 is fixed.

### 3.2 Properties

```cpp
// Provided by VK_KHR_maintenance9
typedef struct VkPhysicalDeviceMaintenance9PropertiesKHR {
    VkStructureType                  sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_9_PROPERTIES_KHR
    void*                            pNext;
    VkBool32                         image2DViewOf3DSparse;
    VkDefaultVertexAttributeValueKHR defaultVertexAttributeValue;
} VkPhysicalDeviceMaintenance9PropertiesKHR;
```

| Property | Meaning |
| --- | --- |
| `image2DViewOf3DSparse` | If 2D-view-of-3D feature is enabled, whether sparse 3D slices can bind to 2D views |
| `defaultVertexAttributeValue` | What unbound vertex attributes return: zero-vector or (0,0,0,1) style default per enum |

**Already cached** as `HardwareCapabilities::maintenance9`.

### 3.3 Queue family ownership transfer properties

```cpp
// Provided by VK_KHR_maintenance9
typedef struct VkQueueFamilyOwnershipTransferPropertiesKHR {
    VkStructureType sType; // VK_STRUCTURE_TYPE_QUEUE_FAMILY_OWNERSHIP_TRANSFER_PROPERTIES_KHR
    void*           pNext;
    uint32_t        optimalImageTransferQueueFamilyCount;
    // ... (see latest refpage for full member set; chain on VkQueueFamilyProperties2)
} VkQueueFamilyOwnershipTransferPropertiesKHR;
```

Query via `vkGetPhysicalDeviceQueueFamilyProperties2` with this struct in the pNext chain. Use to know whether exclusive optimal images still need explicit QFOT between specific families. **Buffers and linear images no longer require QFOT** when maintenance9 is enabled.

### 3.4 Query pool create flag

```cpp
vk::QueryPoolCreateInfo info{
    .flags = vk::QueryPoolCreateFlagBits::eResetKHR,
    .queryType = vk::QueryType::eTimestamp,
    .queryCount = N
};
// Queries start in the reset state — no initial cmdResetQueryPool required.
```

### 3.5 Asymmetric event dependency

| Flag | Use |
| --- | --- |
| `VK_DEPENDENCY_ASYMMETRIC_EVENT_BIT_KHR` | Allow `vkCmdSetEvent2` without providing a full dependency beyond the event src stage mask |

```cpp
vk::DependencyInfo dep{
    .dependencyFlags = vk::DependencyFlagBits::eAsymmetricEventKHR,
    // ...
};
cmd.setEvent2(event, dep);
```

### 3.6 3D image barriers with 2D_ARRAY_COMPATIBLE

When `maintenance9` is enabled, image memory barriers / layout transitions on 3D images created with `VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT` affect **only** the slices in the provided `VkImageSubresourceRange`, not the entire image. Use precise `baseArrayLayer` / `layerCount` (slice) ranges.

---

## Limits

See [Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html) and [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html#limits-required).

Properties cached as `HardwareCapabilities::maintenance9` (`VkPhysicalDeviceMaintenance9PropertiesKHR`):

| Field | Kind |
| --- | --- |
| `image2DViewOf3DSparse` | Bool — sparse 2D view of 3D (with `VK_EXT_image_2d_view_of_3d`) |
| `defaultVertexAttributeValue` | Enum behavior query (unbound attribute default) |

`VkQueueFamilyOwnershipTransferPropertiesKHR` is queried per family at device create and logged; it is **not** stored on `HardwareCapabilities`. Do not invent Required Limits numbers.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE (core enablement)

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep `vk::KHRMaintenance9ExtensionName` |
| `src/core/vk_device.cpp` | Keep feature + property chain |
| `src/core/types.hpp` | Keep `HardwareCapabilities::maintenance9` |

Optional:

1. Log `maintenance9.image2DViewOf3DSparse` and `defaultVertexAttributeValue` at init — **done**.
2. Query `QueueFamilyOwnershipTransferPropertiesKHR` per queue family — **done** (log only; no exclusive optimal QFOT to gate).

### 4.2 Skip unnecessary QFOT for buffers / linear images — high value

**Files:**

- `src/core/vk_resource_manager.cpp`
- `src/core/texture_manager.cpp`
- any transfer → graphics handoff for staging buffers / linear images

Buffers and images already use `SharingMode::eConcurrent` and `QUEUE_FAMILY_IGNORED`. There is no exclusive buffer/linear QFOT pair to remove. Do not invent exclusive uploads just to skip QFOT.

### 4.3 Query pools with create-reset

**Files:** wherever timestamp / occlusion / pipeline-statistics pools are created (Tracy integration, GPU timers).

```cpp
vk::QueryPoolCreateInfo{
    .flags = vk::QueryPoolCreateFlagBits::eResetKHR,
    // ...
};
```

Removes a mandatory reset before first use. Keep per-frame resets as needed for reuse.

### 4.4 Default vertex attribute value

**Files:** mesh/vertex input setup, validation of optional attributes.

Read `capabilities.maintenance9.defaultVertexAttributeValue` and document/assert shader expectations for unbound locations (e.g. missing COLOR0). No runtime branch required unless shaders depend on a specific default.

### 4.5 Asymmetric events / zero-queue device / sparse 2D-of-3D

- **Asymmetric events:** only if a future CPU-GPU event path wants setEvent-style `setEvent2`.
- **Zero-queue device:** tooling/offline cache bake only — not the interactive engine device.
- **Sparse 2D-of-3D:** only if `VK_EXT_image_2d_view_of_3d` + sparse volumes are adopted; gate on `image2DViewOf3DSparse`.

### 4.6 3D + 2D_ARRAY_COMPATIBLE barriers

**Files:** resource manager / volume texture helpers.

When using 3D images with 2D array compatibility, barrier only the active slices:

```cpp
vk::ImageSubresourceRange range{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = sliceIndex, // slice
    .layerCount = 1
};
```

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep enablement as-is | `vk_device.*`, `types.hpp` | Already correct |
| P1 | Drop buffer/linear QFOT under maintenance9 | resource/texture multi-queue paths | N/A — concurrent, no QFOT |
| P1 | Log maintenance9 properties | `vk_device.cpp` | **Done** |
| P2 | `QUERY_POOL_CREATE_RESET_BIT_KHR` | query pool create sites | No query pools |
| P2 | Query `QueueFamilyOwnershipTransferPropertiesKHR` | device init | **Done** (logged) |
| P3 | Unbound vertex attribute documentation/asserts | mesh/pipeline input | Correctness with defaultVertexAttributeValue |
| P3 | Slice-scoped 3D barriers | volume texture path | Only when 2D_ARRAY_COMPATIBLE 3D is used |

### Implementation recipe

1. Confirm `src/core/types.hpp` → `HardwareCapabilities` caches `maintenance3`, `maintenance4`, `maintenance5`, `maintenance6`, `maintenance7`, `maintenance9`, and `maintenance10`. There is **no `maintenance8` properties field**.
2. Confirm `src/core/vk_device.cpp` property chain still queries `PhysicalDeviceMaintenance9PropertiesKHR` into `capabilities.maintenance9`. Feature chain currently sets `{.maintenance9 = false}` (VVL #12449 / SDK ≤1.4.357); keep `vk::KHRMaintenance9ExtensionName` listed. Re-enable the feature when VVL skips `VK_QUEUE_FAMILY_IGNORED` before `ValidateImageInQueue`.
3. Confirm `src/core/vk_allocator.hpp` already sets `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT` and `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT` (no VMA bit for maintenance9).
4. After the feature can be true: drop buffer/linear QFOT; keep optimal-image QFOT until `QueueFamilyOwnershipTransferPropertiesKHR` is queried.
5. Log `image2DViewOf3DSparse` and `defaultVertexAttributeValue`; optional `QUERY_POOL_CREATE_RESET_BIT_KHR` on new query pools.

---

## 6. Concrete integration sketch (drop buffer QFOT)

```cpp
// Before (exclusive buffer transfer → graphics): release + acquire QFOT pair.
// After maintenance9: only access/stage barrier; keep sharing mode exclusive.

vk::BufferMemoryBarrier2 afterUpload{
    .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
    .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
    .dstStageMask = vk::PipelineStageFlagBits2::eVertexAttributeInput,
    .dstAccessMask = vk::AccessFlagBits2::eVertexAttributeRead,
    .srcQueueFamilyIndex = vk::QueueFamilyIgnored, // no QFOT for buffers under maintenance9
    .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
    .buffer = vertexBuffer,
    .offset = 0,
    .size = vk::WholeSize
};

cmd.pipelineBarrier2(vk::DependencyInfo{
    .bufferMemoryBarrierCount = 1,
    .pBufferMemoryBarriers = &afterUpload
});
```

Keep QFOT for **optimal** images until ownership-transfer properties prove otherwise. Pair with maintenance8 `USE_ALL_STAGES` when QFOT remains.

---

## 7. Validation / test checklist

1. Device creates with `VK_KHR_maintenance9` + `maintenance9 = true`.
2. Capability dump shows `image2DViewOf3DSparse` and `defaultVertexAttributeValue`.
3. Buffer multi-queue upload without QFOT validates clean.
4. Linear image multi-queue path without QFOT validates clean.
5. Query pool with `eResetKHR` has defined results on first use without prior reset.
6. Optimal exclusive images: either QFOT still present or ownership property confirms skip.
7. 3D `2D_ARRAY_COMPATIBLE` partial-slice barrier does not clear unrelated slices (when used).

---

## 8. Shader interaction

**Indirect only.** No new shader stages or builtins. Shaders are unaffected in source form; this feature changes host-side pipeline/render-pass/resource/sync setup that must stay compatible with the stages already bound (vertex/mesh/fragment/compute/RT). Ensure push constants / heap binds / attachments still match the active shader I/O when using this path.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Vulkan 1.1 / properties2 | Extension dependency |
| Multi-queue Device | Primary consumer of QFOT relaxations |
| `VK_KHR_maintenance8` | Complements remaining optimal-image QFOT with stage-mask flag |
| `VK_EXT_image_2d_view_of_3d` | Consumer of `image2DViewOf3dSparse` |
| Query pools / Tracy | Create-reset flag |
| Synchronization2 | Events / barriers |

---

## 10. Non-goals / do not do

- Do not create the interactive engine device with zero queues.
- Do not drop QFOT for **optimal** images without checking ownership-transfer properties.
- Do not assume unbound vertex attributes are always (0,0,0,1) — read `defaultVertexAttributeValue`.
- Do not enable sparse 2D-of-3D paths without the sparse property.
- Do not rewrite all event code to asymmetric unless there is a concrete need.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is Maintenance9 enabled? | **Extension listed, properties cached, feature false** (VVL #12449) |
| Is it used? | **Partial** — QFOT-mask log only; maintenance9 properties cached, not logged |
| Highest-value next steps | Re-enable `.maintenance9 = true` when VVL #12449 is fixed |
| Secondary steps | Ownership-transfer property query for optimal images; slice-scoped 3D barriers |
