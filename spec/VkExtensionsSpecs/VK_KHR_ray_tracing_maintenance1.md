# VK_KHR_ray_tracing_maintenance1 — Implementation Spec

**Status in this repo: ENABLED (device extension + features), NOT CONSUMED — no RT dispatch or AS copy paths use maintenance1 entry points.**

Official sources:

- [VK_KHR_ray_tracing_maintenance1](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_ray_tracing_maintenance1.html)
- [Proposal](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_KHR_ray_tracing_maintenance1.adoc)
- [VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR](https://docs.vulkan.org/spec/latest/chapters/features.html)
- [vkCmdTraceRaysIndirect2KHR](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdTraceRaysIndirect2KHR.html)

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::KHRRayTracingMaintenance1ExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → feature chain | `rayTracingMaintenance1 = true`, `rayTracingPipelineTraceRaysIndirect2 = true` |
| Property query | N/A | **No** dedicated properties struct in this enablement |
| Capability cache | `src/core/types.hpp` | **No** maintenance1-specific fields |
| API usage | RT / AS paths | **None** — no `traceRaysIndirect2`, no maintenance1 AS↔memory copies in engine code |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::KHRRayTracingMaintenance1ExtensionName,

// src/core/vk_device.cpp
// vk::PhysicalDeviceRayTracingMaintenance1FeaturesKHR
{.rayTracingMaintenance1 = true,
 .rayTracingPipelineTraceRaysIndirect2 = true},
```

### Not wired yet

- `vkCmdTraceRaysIndirect2KHR` + `VkTraceRaysIndirectCommand2KHR`
- Maintenance1 additions for AS/micromap ↔ memory copies and related RT cleanup (use when implementing serialization/streaming)
- Any consumption from `vk_renderer.cpp` / future RT module

---

## 2. What the extension provides

A maintenance bag for the RT stack: extra host/device commands on top of KHR acceleration structure and KHR ray tracing pipeline, not a third traversal model. It is **not** promoted into core Vulkan 1.4. This repo requires the extension and forces both `rayTracingMaintenance1` and `rayTracingPipelineTraceRaysIndirect2`. There is no dedicated property struct and nothing in `HardwareCapabilities`.

Enabled but **not consumed** — no `traceRaysIndirect2`, no maintenance1 AS↔memory copies. It is portable KHR, not a vendor substitute for NVIDIA cluster/partitioned AS, and it is useless until a working `traceRaysKHR` + SBT path exists.

Highest practical items for this engine:

| Feature | New API / bit | Engine relevance |
| --- | --- | --- |
| Indirect trace v2 | `vkCmdTraceRaysIndirect2KHR` | One device address to a richer indirect command (SBT regions + dimensions) |
| Master maintenance bit | `rayTracingMaintenance1` | Enables the extension feature set |
| RT/AS maintenance ops | Additional copy/query interactions defined by the extension | Streaming, capture, tooling |

`traceRaysIndirect2` is more self-contained than `traceRaysIndirect`: the indirect buffer can include SBT device addresses/strides, not only width/height/depth.

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
typedef struct VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR
    void*           pNext;
    VkBool32        rayTracingMaintenance1;
    VkBool32        rayTracingPipelineTraceRaysIndirect2;
} VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR;
```

| Field | This repo |
| --- | --- |
| `rayTracingMaintenance1` | **true** |
| `rayTracingPipelineTraceRaysIndirect2` | **true** |

Both already forced on at device create.

### 3.2 Properties

No dedicated properties struct is cached in this project (none required for basic enablement).

### 3.3 Indirect2 command

```cpp
void vkCmdTraceRaysIndirect2KHR(
    VkCommandBuffer commandBuffer,
    VkDeviceAddress indirectDeviceAddress);
```

Indirect buffer contents (`VkTraceRaysIndirectCommand2KHR`) include raygen/miss/hit/callable `VkStridedDeviceAddressRegionKHR` fields plus `width`, `height`, `depth` (see official refpage for exact layout).

Contrast:

| API | Indirect payload |
| --- | --- |
| `vkCmdTraceRaysIndirectKHR` | Dimensions only; SBT regions from command parameters |
| `vkCmdTraceRaysIndirect2KHR` | SBT regions **and** dimensions from device memory |

### 3.4 Other maintenance pieces

Use the official extension refpage when implementing:

- AS/micromap copy-to-memory / copy-from-memory interactions introduced for RT maintenance
- Any additional query or property write helpers listed there

Do not invent host APIs not present in the Vulkan headers used by this repo.

---

## Limits

No dedicated property struct and no Required Limits rows. Enablement is feature-only:

| Feature | This repo | Role |
| --- | --- | --- |
| `rayTracingMaintenance1` | **true** | Master maintenance set |
| `rayTracingPipelineTraceRaysIndirect2` | **true** (optional in the spec) | `vkCmdTraceRaysIndirect2KHR` |

Indirect2 buffers still obey `HardwareCapabilities::rayTracingPipeline` SBT alignments and `maxRayDispatchInvocationCount`. AS copy helpers obey `HardwareCapabilities::accelerationStructure` (sizes, scratch alignment). Do not add a synthetic cache field.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep extension name |
| `src/core/vk_device.cpp` | Keep both feature bits true |

### 4.2 Consumers (future)

| Consumer | File(s) |
| --- | --- |
| GPU-driven RT dispatch | Future RT pass (not in `vk_renderer.cpp` today) |
| Indirect2 buffer fill | Compute/DGC or CPU write to BDA buffer |
| AS streaming copies | AS module when serialization lands |

**Prerequisite:** `VK_KHR_ray_tracing_pipeline` `traceRays` path and SBT setup must exist before Indirect2 matters.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep features enabled | `vk_device.cpp` | Already matches intended RT stack |
| P1 | Baseline `traceRaysKHR` | RT module | Must work before indirect variants |
| P1 | Optional `traceRaysIndirectKHR` | RT module | Feature already on for pipeline ext |
| P2 | `traceRaysIndirect2KHR` for fully GPU-built SBT+dispatch | RT module | Uses maintenance1 indirect2 bit |
| P3 | AS memory copy helpers if streaming/save needed | AS module | Maintenance extras |

### Implementation recipe

1. Keep both feature bits. No `HardwareCapabilities` field is required.
2. Ship KHR AS + a direct `cmdTraceRaysKHR` / SBT path first (those extensions are enabled, **not consumed**).
3. Optional: `cmdTraceRaysIndirectKHR` (pipeline extension feature, already on).
4. Add `cmdTraceRaysIndirect2KHR`: GPU writes `VkTraceRaysIndirectCommand2KHR` (SBT regions + dimensions) to a BDA buffer; host only passes the address.
5. Respect cached RT pipeline SBT alignments and `maxRayDispatchInvocationCount` when filling that buffer.
6. Later: maintenance1 AS/micromap ↔ memory copies if streaming/serialization needs them.

---

## 6. Concrete integration sketch

```cpp
// After GPU writes VkTraceRaysIndirectCommand2KHR at indirectAddr:
cmd.traceRaysIndirect2KHR(indirectAddr);

// No SBT parameters on the command — they live in device memory.
// Ensure: RT pipeline bound, push constants/descriptors set, AS ready.
```

Validation focus: indirect buffer usage flags, device address, and struct alignment per spec.

---

## 7. Validation / test checklist

1. Device creates with both maintenance1 feature bits (already).
2. Do not call `traceRaysIndirect2KHR` without `rayTracingPipelineTraceRaysIndirect2`.
3. Indirect buffer readable by RT stage; address stable for the duration of the command.
4. Compare results vs direct `traceRaysKHR` for same SBT/dimensions.
5. No use of maintenance1 APIs before RT pipeline path exists (would be dead code).

---

## 8. Shader interaction

**Mostly host RT maintenance; minor shader-facing extras depending on bits used.** Improves RT pipeline/query usability without new primary stages. Target base [ray query](./VK_KHR_ray_query.md) / [RT pipeline](./VK_KHR_ray_tracing_pipeline.md) first.

### Engine status

Enabled, not consumed.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| `VK_KHR_ray_tracing_pipeline` | Base trace/SBT APIs |
| `VK_KHR_acceleration_structure` | AS objects |
| Pipeline feature `rayTracingPipelineTraceRaysIndirect` | Older indirect form |
| Maintenance1 `rayTracingPipelineTraceRaysIndirect2` | Newer indirect form |
| Device-generated commands / compute | Natural fillers of Indirect2 buffers |

---

## 10. Non-goals / do not do

- Do not implement Indirect2 before a working direct `traceRays` path.
- Do not add synthetic properties to `HardwareCapabilities` without a real query struct in use.
- Do not document this extension as a substitute for AS or RT pipeline enablement.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is ray tracing maintenance1 enabled? | **Yes** — extension + both feature bits |
| Is it used? | **No** |
| Highest-value next steps | After RT pipeline works, use `traceRaysIndirect2KHR` for GPU-driven dispatch |
| Secondary steps | AS memory copy maintenance when streaming needs it |
