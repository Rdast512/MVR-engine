# VK_EXT_ray_tracing_invocation_reorder — Implementation Spec

**Status in this repo: ENABLED (device extension + feature), NOT CONSUMED — no shader reorder hints or property cache.**

Official sources:

- [VK_EXT_ray_tracing_invocation_reorder](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_ray_tracing_invocation_reorder.html)
- [Proposal](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_EXT_ray_tracing_invocation_reorder.adoc)
- [VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT](https://docs.vulkan.org/spec/latest/chapters/features.html)
- GLSL: `GL_EXT_shader_invocation_reorder` (and related) — confirm exact extension strings in shader toolchain docs

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::EXTRayTracingInvocationReorderExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → feature chain | `rayTracingInvocationReorder = true` |
| Property query | `src/core/vk_device.cpp` | **No** dedicated properties type in the project properties chain |
| Capability cache | `src/core/types.hpp` | **No** reorder properties field |
| API usage | RT shaders | **None** — no RT shader path at all |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::EXTRayTracingInvocationReorderExtensionName,

// src/core/vk_device.cpp
// vk::PhysicalDeviceRayTracingInvocationReorderFeaturesEXT
{.rayTracingInvocationReorder = true},
```

### Not wired yet

- Shader-side reorder hints / `hitObject` style APIs as defined by the extension
- Optional properties query if the Vulkan header provides `VkPhysicalDeviceRayTracingInvocationReorderPropertiesEXT` (or successor) — **not** cached today
- Any consumption in raygen/closest-hit paths

---

## 2. What the extension provides

Hints and mechanisms for **reordering ray tracing invocations** so coherent waves hit similar materials/geometry, improving cache and divergence behavior on supporting GPUs. This is a portable EXT **quality hint** on top of KHR ray tracing pipelines (typically raygen), not a new traversal API and not promoted into core. This repo requires the extension and sets `rayTracingInvocationReorder = true` only — **no properties cached** in `HardwareCapabilities`.

It is **not consumed** (no RT shaders exist). Do not block the KHR AS + RT pipeline MVP on reorder. It is not a vendor AS alternative; NVIDIA cluster/partitioned extensions are a separate portability concern.

| Feature | Mechanism | Engine relevance |
| --- | --- | --- |
| Feature bit | `rayTracingInvocationReorder` | Master enable for reorder SPIR-V |
| Shader hints | Reorder / hit-object instructions (see GLSL EXT) | Secondary rays, stochastic lighting |
| Implementation quality | Driver may no-op safely if unused in shaders | Zero cost until shaders opt in |

This is an **optimization** extension, not required to trace rays.

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
typedef struct VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_EXT
    void*           pNext;
    VkBool32        rayTracingInvocationReorder;
} VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT;
```

**Already enabled:** `rayTracingInvocationReorder = true`.

### 3.2 Properties

Some implementations expose reorder-related properties (e.g. support level enums). This repo:

- Does **not** query a reorder properties struct in `getProperties2` chain
- Does **not** store anything under `HardwareCapabilities`

When implementing shaders, consult the exact header version in the project for:

- Whether a properties struct exists
- Enum values for reorder support quality

If present and useful for gating shader variants, add query + cache similar to other RT properties.

### 3.3 Host commands

**None** beyond feature enablement. All behavior is shader ISA + driver scheduling.

### 3.4 Shader usage (conceptual)

Shaders opt into reorder via the corresponding GLSL/SPIR-V extension (hit object + reorder invocations pattern). Exact entry points depend on toolchain; follow Khronos GLSL extension docs used by your compiler.

Typical flow in raygen:

1. Trace or intersect into a hit object representation (as defined by the EXT).
2. Hint reorder by material / coherence key.
3. Resume shading after reorder for better wave coherence.

---

## Limits

No numeric Required Limits for this extension. The spec’s properties (`VkPhysicalDeviceRayTracingInvocationReorderPropertiesEXT`: `rayTracingInvocationReorderReorderingHint`, plus `maxShaderBindingTableRecordIndex` on recent headers) are a **hint to the implementation for hit/miss coherence**, not min/max rows like AS primitive counts.

This repo enables the **feature only**. `vk_device.cpp` does not query the properties struct; `HardwareCapabilities` has no reorder field (unlike `rayTracingPipeline` / `accelerationStructure`). Do not invent numeric limits. If shader variants later need the hint enum, query and cache then.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE for feature

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep extension name |
| `src/core/vk_device.cpp` | Keep feature true |
| `src/core/types.hpp` | Optional: add properties if header + gating need them |

### 4.2 Real consumption

Only after `VK_KHR_ray_tracing_pipeline` (or advanced ray-query) shaders exist:

| Location | Role |
| --- | --- |
| Raygen / shading SPIR-V | Reorder hints |
| Shader compile flags | Enable reorder GLSL extension |
| `src/render/` RT module | No host API changes expected |

**Not relevant** to current raster-only `vk_renderer.cpp` / `vk_pipeline.cpp` graphics PSOs.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep feature enabled | `vk_device.cpp` | Harmless until shaders use it |
| P1 | Working RT pipeline + secondary rays | RT stack | Need divergence before reorder pays off |
| P2 | Introduce reorder in hottest raygen loops | shaders | Performance |
| P2 | Optional properties cache + shader variant gate | `types.hpp`, `vk_device.cpp` | If support level varies |

### Implementation recipe

1. Keep `rayTracingInvocationReorder = true`. No host commands; no `HardwareCapabilities` field required for MVP.
2. Implement portable KHR AS + `VK_KHR_ray_tracing_pipeline` (enabled, **not consumed**) and ship secondary rays first — reorder only pays off with divergence.
3. Opt in from raygen via `GL_EXT_shader_invocation_reorder` / hit-object + reorder SPIR-V. Do not emit those ops unless the feature is enabled.
4. Treat reorder as scheduling: results must match without it. Measure (Nsight / RGP) before keeping hints.
5. Optional: query `PhysicalDeviceRayTracingInvocationReorderPropertiesEXT` and cache if the hint enum should gate shader variants.

---

## 6. Concrete integration sketch

Host side remains a no-op beyond enablement:

```cpp
// Already:
// PhysicalDeviceRayTracingInvocationReorderFeaturesEXT { .rayTracingInvocationReorder = true }
```

Shader side (illustrative — match your GLSL EXT version):

```glsl
#extension GL_EXT_shader_invocation_reorder : require
// hitObjectNV/EXT + reorderThreadExecution / equivalent per extension revision
// Use after a coherence-breaking trace batch
```

Prefer measuring with vendor tools (Nsight, RGP) before and after.

---

## 7. Validation / test checklist

1. Device creates with extension + feature (already).
2. SPIR-V with reorder ops only when feature enabled.
3. Functional correctness identical with/without reorder hints (reorder is scheduling).
4. Performance: secondary-ray heavy scenes should improve or not regress on target NVIDIA/other supporting GPUs.
5. If properties added later: capability dump shows support level.

---

## 8. Shader interaction

**Direct — SPIR-V ops to reorder RT invocations for better coherence.** Feature `rayTracingInvocationReorder` unlocks hit-object + reorder intrinsics so raygen can defer shading until similar hits are grouped (trace/record hit object, reorder by key, invoke hit shading).

Host only enables the feature and provides normal RT pipeline + SBT (or advanced ray-query paths). Do not emit reorder SPIR-V until the feature is confirmed at device create.

### Engine status

Feature **enabled**, **no shaders**. Only relevant after [VK_KHR_ray_tracing_pipeline](./VK_KHR_ray_tracing_pipeline.md) content exists.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| `VK_KHR_ray_tracing_pipeline` | Primary consumer of reorder in raygen |
| `VK_KHR_ray_query` | Less common; reorder is aimed at RT invocations |
| `VK_KHR_acceleration_structure` | Underlying scene structure |
| No descriptor/host changes | Pure shader + feature bit |

---

## 10. Non-goals / do not do

- Do not block RT MVP on reorder implementation.
- Do not call non-existent host reorder commands.
- Do not assume properties exist without checking headers used by the project.
- Do not enable in graphics fragment shaders expecting RT reorder semantics without RT stages.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is invocation reorder enabled? | **Yes** — extension + `rayTracingInvocationReorder` |
| Is it used? | **No** |
| Highest-value next steps | Ship RT pipeline; then add shader reorder hints on secondary rays |
| Secondary steps | Optional properties query for support-level gating |
