# VK_KHR_ray_query — Implementation Spec

**Status in this repo: ENABLED (device extension + feature), NOT CONSUMED — no `rayQueryEXT` SPIR-V usage or AS-bound query shaders.**

Official sources:

- [VK_KHR_ray_query](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_ray_query.html)
- [Proposal](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_KHR_ray_query.adoc)
- [VkPhysicalDeviceRayQueryFeaturesKHR](https://docs.vulkan.org/spec/latest/chapters/features.html)
- GLSL: `GL_EXT_ray_query` / SPIR-V: `RayQueryKHR` capability

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::KHRRayQueryExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → feature chain | `rayQuery = true` |
| Property query | N/A | **No** dedicated properties struct |
| Capability cache | `src/core/types.hpp` | **No** ray-query-specific fields |
| API usage | shaders / pipelines / renderer | **None** — no host commands; no SPIR-V ray query |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::KHRRayQueryExtensionName,

// src/core/vk_device.cpp
// vk::PhysicalDeviceRayQueryFeaturesKHR
{.rayQuery = true},
```

Depends on `VK_KHR_acceleration_structure` (listed) + BDA.

### Not wired yet

- Shaders using `rayQueryEXT` / SPIR-V `OpRayQuery*`
- Pipeline/shader stages that declare RayQuery capability
- TLAS bound for query (AS module + descriptors)
- Hybrid effects (shadows, reflections, AO) in raster or compute

---

## 2. What the extension provides

**Inline** ray traversal from any supported shader stage without a ray tracing pipeline or SBT. Ray query is the portable hybrid half of the KHR RT stack: same KHR acceleration structures as the RT pipeline, but no SBT and no dedicated raygen/hit stages. It is **not** promoted into core Vulkan 1.4. This repo requires the extension and sets `rayQuery = true`; there is **no** property struct and **no** `HardwareCapabilities` field.

Enabled but **not consumed** — no `rayQueryEXT` shaders, no TLAS bind. First lighting experiments can use this (fragment/compute + TLAS) without standing up a full RT pipeline. It is not a substitute for KHR AS, and it does not need NVIDIA cluster/partitioned AS.

| Feature | Mechanism | Engine relevance |
| --- | --- | --- |
| Ray query object | `rayQueryEXT` (GLSL) / SPIR-V ray query instructions | Shadows, AO, SSR assist in raster/compute |
| Proceed / confirm hits | `rayQueryProceedEXT`, candidate/committed hits | Any-hit style logic in-shader |
| No new host commands | Feature + SPIR-V only | Simpler than full RT pipeline for hybrid |

Compare to `VK_KHR_ray_tracing_pipeline`: ray query is **not** `traceRays`; it runs inside vertex/fragment/compute/mesh/etc. with an AS descriptor.

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
typedef struct VkPhysicalDeviceRayQueryFeaturesKHR {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR
    void*           pNext;
    VkBool32        rayQuery;
} VkPhysicalDeviceRayQueryFeaturesKHR;
```

**Already enabled:** `rayQuery = true`.

### 3.2 Properties

None for this extension.

### 3.3 Host commands

**None.** Enablement is device feature + acceleration structure objects + shader code.

### 3.4 Shader-side API (GLSL sketch)

```glsl
#extension GL_EXT_ray_query : require

layout(set = X, binding = Y) uniform accelerationStructureEXT topLevelAS;

void main() {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, topLevelAS,
        gl_RayFlagsOpaqueEXT,   // or other flags
        cullMask,
        origin, tMin,
        direction, tMax);

    while (rayQueryProceedEXT(rq)) {
        // optional: examine candidate intersections
    }

    if (rayQueryGetIntersectionTypeEXT(rq, true) ==
        gl_RayQueryCommittedIntersectionTriangleEXT) {
        // hit
    } else {
        // miss
    }
}
```

SPIR-V requires `RayQueryKHR` capability and AS decorations; compile with a toolchain that supports `GL_EXT_ray_query`.

### 3.5 Descriptor

Same AS descriptor binding as RT pipelines:

- Type: `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`
- Write via `VkWriteDescriptorSetAccelerationStructureKHR`

---

## Limits

No dedicated property struct and no Required Limits rows for this extension. Ray query **consumes KHR AS limits and descriptor counts**:

- `HardwareCapabilities::accelerationStructure` — `maxGeometryCount` / `maxInstanceCount` / `maxPrimitiveCount`, `maxPerStageDescriptorAccelerationStructures` (min **16**), `maxDescriptorSetAccelerationStructures` (min **16**), scratch alignment for the TLAS you query.
- Any stage that uses the `rayQuery` feature still needs an AS **descriptor** or a descriptor-heap AS write. There is nothing extra to cache in `types.hpp`.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep `vk::KHRRayQueryExtensionName` |
| `src/core/vk_device.cpp` | Keep `rayQuery = true` |
| `src/core/types.hpp` | No property field needed |

### 4.2 Prerequisites (other specs)

1. **BLAS/TLAS** — `VK_KHR_acceleration_structure` module (does not exist yet).
2. **Descriptors** — `src/core/vk_descriptors.cpp` must bind TLAS for the shader set/heap layout.

### 4.3 Shaders + pipelines

| Path | Files |
| --- | --- |
| Fragment/compute hybrid effects | New shader assets + load path used by materials |
| Pipeline creation | `src/render/vk_pipeline.cpp` — graphics/compute PSOs that use query-enabled SPIR-V |
| Frame binding | `src/render/vk_renderer.cpp` or material pass — bind TLAS before draws/dispatches that query |

**Do not** require `vkCreateRayTracingPipelinesKHR` for pure ray-query hybrid techniques.

### 4.4 Preferred first consumer for this engine

Given raster dynamic rendering is the only path today, the natural first use is:

- **Shadow rays** or **AO** from a deferred/forward fragment or compute pass
- TLAS of opaque scene geometry
- Output to a screen-space buffer, composite later

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep `rayQuery = true` | `vk_device.cpp` | Already correct |
| P0 | AS + TLAS descriptor | AS module, `vk_descriptors.cpp` | Required binding |
| P1 | First ray-query shader (e.g. shadow visibility) | shaders + materials | Prove stack |
| P1 | Bind TLAS for that pass | renderer / descriptors | Host/shader connect |
| P2 | Any-hit alpha / flags policy | shaders + AS geometry flags | Correct foliage etc. |
| P3 | Share TLAS with full RT pipeline path | RT module | One scene structure |

### Implementation recipe

1. Keep `rayQuery = true`. Do not add a fake properties field to `HardwareCapabilities`.
2. Implement portable KHR BLAS/TLAS first (`VK_KHR_acceleration_structure` is enabled, **not consumed**). Gate geometry/instance counts with `HardwareCapabilities::accelerationStructure`.
3. Bind the TLAS: `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR` or a descriptor-heap AS write. Any stage with the rayQuery feature still needs that binding.
4. Compile a fragment or compute shader with `GL_EXT_ray_query` / SPIR-V `RayQueryKHR`; keep a graphics/compute PSO (no RT pipeline / SBT).
5. Barrier AS build write → shader `ACCELERATION_STRUCTURE_READ` at the querying stage.
6. Optional: share the same TLAS with `VK_KHR_ray_tracing_pipeline` later.

---

## 6. Concrete integration sketch

Host (after TLAS exists):

```cpp
// Descriptor write (legacy set style; adapt for heaps)
vk::WriteDescriptorSetAccelerationStructureKHR asInfo{
    .accelerationStructureCount = 1,
    .pAccelerationStructures = &*tlas
};

vk::WriteDescriptorSet write{
    .pNext = &asInfo,
    .dstSet = set,
    .dstBinding = kTlasBinding,
    .descriptorCount = 1,
    .descriptorType = vk::DescriptorType::eAccelerationStructureKHR
};
device.updateDescriptorSets(write, {});
```

Shader module must be built with ray query support; pipeline create stays graphics/compute.

Barriers: AS must be ready for `VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR` at shader stages that issue queries.

---

## 7. Validation / test checklist

1. Device creates with `rayQuery = true` (already).
2. SPIR-V validates with `RayQueryKHR` when extension/feature enabled.
3. TLAS descriptor type/layout matches shader.
4. No `traceRays` required for query-only passes.
5. Traversal flags / cull mask match geometry instance masks.
6. Performance: limit query count per pixel; opaque flags when alpha not needed.

---

## 8. Shader interaction

**Direct — inline ray traversal inside ordinary stages (no RT pipeline / SBT).** Feature `rayQuery` + SPIR-V `RayQueryKHR` capability. Shaders call initialize, proceed loop, then inspect candidate/committed hits.

### Where it can run

Any stage allowed by the implementation — commonly **fragment**, **compute**, and also **vertex / mesh** for specialized effects. Hybrid path: raster (or mesh) primary visibility + ray query for shadows/AO/reflections.

### Conceptual shader flow

```glsl
rayQueryEXT rq;
rayQueryInitializeEXT(rq, tlas, rayFlags, cullMask, origin, tMin, direction, tMax);
while (rayQueryProceedEXT(rq)) {
    if (rayQueryGetIntersectionTypeEXT(rq, false) == /* candidate triangle */) {
        rayQueryConfirmIntersectionEXT(rq);
    }
}
if (rayQueryGetIntersectionTypeEXT(rq, true) != /* none */) {
    // use committed hit T, barycentrics, instance custom index
}
```

Slang exposes analogous ray-query intrinsics when targeting SPIR-V with ray query enabled.

### Resource requirements

- TLAS as acceleration-structure descriptor (set/binding or descriptor heap AS write).
- Geometry for AS still built via [VK_KHR_acceleration_structure](./VK_KHR_acceleration_structure.md); shaders only **query**.
- Ray flags and cull masks must match AS instance / geometry flags.

### Engine status

Feature **enabled**. No engine shader uses ray query yet. First consumer would likely be classic or mesh **fragment** after TLAS exists — no SBT required.

Compare: [VK_KHR_ray_tracing_pipeline](./VK_KHR_ray_tracing_pipeline.md).

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| `VK_KHR_acceleration_structure` | Required |
| `VK_KHR_ray_tracing_pipeline` | Optional peer — full RT vs inline query |
| `VK_KHR_ray_tracing_maintenance1` | Maintenance for broader RT stack; not required for basic query |
| `VK_EXT_opacity_micromap` | Micromaps can affect query hits if attached to AS |
| Raster pipelines in `vk_pipeline.cpp` | Can consume ray query SPIR-V |
| No host commands | All complexity is shader + AS + descriptors |

---

## 10. Non-goals / do not do

- Do not invent host entry points for ray query.
- Do not add fake properties to `HardwareCapabilities` for this extension.
- Do not require SBT / raygen stages for query-only effects.
- Do not enable ray query shaders before a valid TLAS exists (validation / GPU hang risk).

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is ray query enabled? | **Yes** — extension + `rayQuery` feature |
| Is it used? | **No** — no SPIR-V / bind consumption |
| Highest-value next steps | TLAS + descriptor + first hybrid shadow/AO query shader |
| Secondary steps | Share AS with RT pipeline path; alpha/micromap policy |
