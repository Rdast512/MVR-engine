# VK_KHR_ray_tracing_pipeline — Implementation Spec

**Status in this repo: ENABLED (device extension + features + properties cached), NOT CONSUMED — no RT pipelines, SBT, or `traceRays`.**

Official sources:

- [VK_KHR_ray_tracing_pipeline](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_ray_tracing_pipeline.html)
- [Proposal](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_KHR_ray_tracing_pipeline.adoc)
- [VkPhysicalDeviceRayTracingPipelineFeaturesKHR](https://docs.vulkan.org/spec/latest/chapters/features.html)
- [VkPhysicalDeviceRayTracingPipelinePropertiesKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceRayTracingPipelinePropertiesKHR.html)

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::KHRRayTracingPipelineExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → feature chain | `rayTracingPipeline`, `rayTracingPipelineTraceRaysIndirect`, `rayTraversalPrimitiveCulling` all **true** |
| Property query | `src/core/vk_device.cpp` → properties chain | Queried and assigned |
| Capability cache | `src/core/types.hpp` → `HardwareCapabilities::rayTracingPipeline` | Cached |
| API usage | `src/render/vk_pipeline.cpp`, `vk_renderer.cpp` | **None** — graphics pipelines + raster dynamic rendering only |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::KHRRayTracingPipelineExtensionName,

// src/core/vk_device.cpp (feature chain)
// vk::PhysicalDeviceRayTracingPipelineFeaturesKHR
{.rayTracingPipeline = true,
 .rayTracingPipelineTraceRaysIndirect = true,
 .rayTraversalPrimitiveCulling = true},

// src/core/vk_device.cpp (properties)
capabilities.rayTracingPipeline =
    propertiesChain.get<vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();

// src/core/types.hpp
vk::PhysicalDeviceRayTracingPipelinePropertiesKHR rayTracingPipeline;
```

Depends on AS stack already listed (`VK_KHR_acceleration_structure`, deferred host ops, BDA).

### Not wired yet

- `vkCreateRayTracingPipelinesKHR`
- Shader groups (raygen / miss / hit / callable) and SPIR-V RT stages
- Shader Binding Table (SBT) buffers + handle queries
- `vkCmdTraceRaysKHR` / `vkCmdTraceRaysIndirectKHR`
- Stack size queries / `vkCmdSetRayTracingPipelineStackSizeKHR`
- RT pipeline layout / descriptor set for TLAS + output images
- Any path in `vk_pipeline.cpp` or `vk_renderer.cpp` for RT

---

## 2. What the extension provides

Dedicated ray tracing **pipelines** with a shader binding table and hardware `traceRays` dispatch (as opposed to inline `rayQuery` in raster/compute stages). This is the portable KHR “full RT” half of the stack: it consumes KHR acceleration structures (TLAS) and is **not** promoted into core Vulkan 1.4. This repo requires the device extension, enables `rayTracingPipeline` + indirect + primitive-culling features, and caches `HardwareCapabilities::rayTracingPipeline`.

It is **enabled but not consumed** — no RT pipelines, SBT, or `traceRays` exist. Prefer KHR AS + this pipeline (or ray query) over NVIDIA cluster/partitioned AS. `VK_KHR_ray_tracing_maintenance1` adds Indirect2 on top; invocation reorder is an optional shader-quality hint after this path works.

| Feature | New API | Engine relevance |
| --- | --- | --- |
| RT pipeline create | `vkCreateRayTracingPipelinesKHR` | Full RT / hybrid path |
| Shader group handles | `vkGetRayTracingShaderGroupHandlesKHR` | SBT population |
| Trace rays | `vkCmdTraceRaysKHR` | Primary RT dispatch |
| Indirect trace | `vkCmdTraceRaysIndirectKHR` | GPU-driven dispatch (feature enabled) |
| Stack size | `vkGetRayTracingShaderGroupStackSizeKHR` + `vkCmdSetRayTracingPipelineStackSizeKHR` | Recursion / callable safety |
| Primitive culling | `rayTraversalPrimitiveCulling` | SPIR-V culling ops in intersection/any-hit |

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
typedef struct VkPhysicalDeviceRayTracingPipelineFeaturesKHR {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR
    void*           pNext;
    VkBool32        rayTracingPipeline;
    VkBool32        rayTracingPipelineShaderGroupHandleCaptureReplay;
    VkBool32        rayTracingPipelineShaderGroupHandleCaptureReplayMixed;
    VkBool32        rayTracingPipelineTraceRaysIndirect;
    VkBool32        rayTraversalPrimitiveCulling;
} VkPhysicalDeviceRayTracingPipelineFeaturesKHR;
```

| Field | This repo |
| --- | --- |
| `rayTracingPipeline` | **true** |
| `rayTracingPipelineShaderGroupHandleCaptureReplay` | false (default) |
| `rayTracingPipelineShaderGroupHandleCaptureReplayMixed` | false (default) |
| `rayTracingPipelineTraceRaysIndirect` | **true** |
| `rayTraversalPrimitiveCulling` | **true** |

### 3.2 Properties (cached)

```cpp
typedef struct VkPhysicalDeviceRayTracingPipelinePropertiesKHR {
    VkStructureType sType;
    void*           pNext;
    uint32_t        shaderGroupHandleSize;
    uint32_t        maxRayRecursionDepth;
    uint32_t        maxShaderGroupStride;
    uint32_t        shaderGroupBaseAlignment;
    uint32_t        shaderGroupHandleCaptureReplaySize;
    uint32_t        maxRayDispatchInvocationCount;
    uint32_t        shaderGroupHandleAlignment;
    uint32_t        maxRayHitAttributeSize;
} VkPhysicalDeviceRayTracingPipelinePropertiesKHR;
```

**Already cached** as `HardwareCapabilities::rayTracingPipeline`.

SBT layout **must** use:

- `shaderGroupHandleSize` / `shaderGroupHandleAlignment`
- `shaderGroupBaseAlignment` for region base offsets
- strides ≤ `maxShaderGroupStride` and multiple of handle alignment

### 3.3 Commands

```cpp
VkResult vkCreateRayTracingPipelinesKHR(
    VkDevice device,
    VkDeferredOperationKHR deferredOperation,
    VkPipelineCache pipelineCache,
    uint32_t createInfoCount,
    const VkRayTracingPipelineCreateInfoKHR* pCreateInfos,
    const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines);

VkResult vkGetRayTracingShaderGroupHandlesKHR(
    VkDevice device,
    VkPipeline pipeline,
    uint32_t firstGroup,
    uint32_t groupCount,
    size_t dataSize,
    void* pData);

VkResult vkGetRayTracingCaptureReplayShaderGroupHandlesKHR(...); // capture/replay only

void vkCmdTraceRaysKHR(
    VkCommandBuffer commandBuffer,
    const VkStridedDeviceAddressRegionKHR* pRaygenShaderBindingTable,
    const VkStridedDeviceAddressRegionKHR* pMissShaderBindingTable,
    const VkStridedDeviceAddressRegionKHR* pHitShaderBindingTable,
    const VkStridedDeviceAddressRegionKHR* pCallableShaderBindingTable,
    uint32_t width, uint32_t height, uint32_t depth);

void vkCmdTraceRaysIndirectKHR(
    VkCommandBuffer commandBuffer,
    const VkStridedDeviceAddressRegionKHR* pRaygenShaderBindingTable,
    const VkStridedDeviceAddressRegionKHR* pMissShaderBindingTable,
    const VkStridedDeviceAddressRegionKHR* pHitShaderBindingTable,
    const VkStridedDeviceAddressRegionKHR* pCallableShaderBindingTable,
    VkDeviceAddress indirectDeviceAddress); // VkTraceRaysIndirectCommandKHR

VkDeviceSize vkGetRayTracingShaderGroupStackSizeKHR(
    VkDevice device,
    VkPipeline pipeline,
    uint32_t group,
    VkShaderGroupShaderKHR groupShader);

void vkCmdSetRayTracingPipelineStackSizeKHR(
    VkCommandBuffer commandBuffer,
    uint32_t pipelineStackSize);
```

### 3.4 Pipeline create info (conceptual)

```cpp
typedef struct VkRayTracingPipelineCreateInfoKHR {
    VkStructureType sType;
    const void* pNext;
    VkPipelineCreateFlags flags;
    uint32_t stageCount;
    const VkPipelineShaderStageCreateInfo* pStages;
    uint32_t groupCount;
    const VkRayTracingShaderGroupCreateInfoKHR* pGroups;
    uint32_t maxPipelineRayRecursionDepth;
    const VkPipelineLibraryCreateInfoKHR* pLibraryInfo;
    const VkRayTracingPipelineInterfaceCreateInfoKHR* pLibraryInterface;
    const VkPipelineDynamicStateCreateInfo* pDynamicState;
    VkPipelineLayout layout;
    VkPipeline basePipelineHandle;
    int32_t basePipelineIndex;
} VkRayTracingPipelineCreateInfoKHR;
```

Shader stages: `VK_SHADER_STAGE_RAYGEN_BIT_KHR`, `ANY_HIT`, `CLOSEST_HIT`, `MISS`, `INTERSECTION`, `CALLABLE`.

---

## Limits

Cached as `HardwareCapabilities::rayTracingPipeline`. Source: [Vulkan Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html#limits-minmax).

| Property | Required limit | SBT / dispatch rule |
| --- | --- | --- |
| `shaderGroupHandleSize` | **exact 32** bytes | Size of each group handle from `getRayTracingShaderGroupHandlesKHR` |
| `maxRayRecursionDepth` | min **1** | `maxPipelineRayRecursionDepth` must be ≤ this |
| `maxShaderGroupStride` | min **4096** | Region `stride` ≤ this |
| `shaderGroupBaseAlignment` | max **256** | Each SBT region **base** must be aligned to this |
| `shaderGroupHandleAlignment` | max **32**, power of two | Each handle (and stride) aligned to this |
| `maxRayDispatchInvocationCount` | min **2^30** | `width × height × depth` of a trace dispatch |
| `maxRayHitAttributeSize` | min **32** | Hit-attribute payload bytes |
| `shaderGroupHandleCaptureReplaySize` | exact `shaderGroupHandleSize` | Only when capture/replay handles are used (not enabled here) |

SBT layout: region base aligned to `shaderGroupBaseAlignment`; each handle aligned to `shaderGroupHandleAlignment`; stride ≤ `maxShaderGroupStride` and a multiple of handle alignment. Always read the cached property values — do not hardcode except the spec’s exact 32-byte handle size as a sanity check.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep `vk::KHRRayTracingPipelineExtensionName` |
| `src/core/vk_device.cpp` | Keep features + property cache |
| `src/core/types.hpp` | Keep `rayTracingPipeline` properties |

Log at init: `shaderGroupHandleSize`, alignments, `maxRayRecursionDepth`.

### 4.2 Pipeline creation

**File:** `src/render/vk_pipeline.cpp` / `vk_pipeline.hpp` today = **graphics only**.

Options:

1. Extend with RT pipeline factory methods, or
2. New `src/render/vk_rt_pipeline.cpp` dedicated module (preferred for clarity)

Do not overload mesh/graphics PSO code paths blindly — RT uses different create info and no vertex input / raster state.

### 4.3 SBT + dispatch

| Concern | Location |
| --- | --- |
| SBT buffer alloc (BDA + shader binding table usage) | resource manager / RT module |
| Handle copy into SBT | after `createRayTracingPipelinesKHR` |
| `cmdTraceRaysKHR` | new RT pass — **not** current `vk_renderer.cpp` forward pass |
| TLAS descriptor | `vk_descriptors.cpp` + AS module |

### 4.4 Shaders

SPIR-V must be compiled with ray tracing stages and linked against AS/ray payload interfaces. No ray-gen/miss/hit assets exist in-tree yet for consumption.

### 4.5 Renderer integration

**File:** `src/render/vk_renderer.cpp`

- Remains raster-only until a hybrid or pure-RT frame graph is designed.
- Suggested: separate record function (e.g. `recordRayTracingPass`) that binds RT pipeline, descriptors, SBT regions, then `traceRaysKHR`.
- Output: storage image → later blit/tonemap into swapchain; do not assume dynamic rendering attachments work like raster color attachments for raygen.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep enablement | `vk_device.*`, `types.hpp` | Already correct |
| P0 | Working BLAS/TLAS | AS module (`VK_KHR_acceleration_structure`) | Hard dependency |
| P1 | RT pipeline + shader groups | new RT pipeline module | First pipeline object |
| P1 | SBT from group handles using cached properties | RT module + resources | Required for dispatch |
| P1 | `cmdTraceRaysKHR` smoke pass (primary rays → storage image) | renderer RT pass | First visual |
| P2 | Recursion / shadow rays; stack size calculation | RT module | Correctness under recursion |
| P2 | `cmdTraceRaysIndirectKHR` | RT pass | Uses already-enabled feature |
| P3 | Libraries / callable shaders / capture-replay | as needed | Advanced |

### Implementation recipe

1. Keep `HardwareCapabilities::rayTracingPipeline` cached. Do not start this path until KHR BLAS/TLAS exist (`VK_KHR_acceleration_structure` is enabled but **not consumed**).
2. Create an RT pipeline (`createRayTracingPipelinesKHR`) with shader groups; `maxPipelineRayRecursionDepth` ≤ `rayTracingPipeline.maxRayRecursionDepth`.
3. `getRayTracingShaderGroupHandlesKHR` — `dataSize` = `shaderGroupHandleSize × groupCount` (handle size is exact **32**).
4. Pack the SBT: each region base aligned to `shaderGroupBaseAlignment`; each handle aligned to `shaderGroupHandleAlignment`; stride ≤ `maxShaderGroupStride` and a multiple of handle alignment.
5. Bind RT pipeline + TLAS descriptor (or heap write) + output storage image.
6. `cmdTraceRaysKHR` (smoke: primary rays → storage image). Optional later: `cmdTraceRaysIndirectKHR` (already-enabled feature) or maintenance1 `traceRaysIndirect2`.
7. Query/set stack size if recursion or callables exceed the default.

---

## 6. Concrete integration sketch (trace rays)

```cpp
const auto& rtp = device.getCapabilities().rayTracingPipeline;
const uint32_t handleSize = rtp.shaderGroupHandleSize;
const uint32_t handleAlign = rtp.shaderGroupHandleAlignment;
const uint32_t baseAlign = rtp.shaderGroupBaseAlignment;

// After createRayTracingPipelinesKHR:
std::vector<uint8_t> handles(handleSize * groupCount);
device.getRayTracingShaderGroupHandlesKHR(
    *rtPipeline, 0, groupCount, handles.size(), handles.data());

// Pack into SBT buffer with correct base/stride alignment (raygen often stride = aligned handle size)

vk::StridedDeviceAddressRegionKHR raygen{raygenBase, raygenStride, raygenSize};
vk::StridedDeviceAddressRegionKHR miss{missBase, missStride, missSize};
vk::StridedDeviceAddressRegionKHR hit{hitBase, hitStride, hitSize};
vk::StridedDeviceAddressRegionKHR callable{}; // empty if unused

cmd.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, *rtPipeline);
// bind descriptor sets / push constants (TLAS, output image, camera)
cmd.traceRaysKHR(raygen, miss, hit, callable, width, height, 1);
```

Indirect (feature already on):

```cpp
// Buffer contains VkTraceRaysIndirectCommandKHR { width, height, depth }
cmd.traceRaysIndirectKHR(raygen, miss, hit, callable, indirectBufferAddress);
```

---

## 7. Validation / test checklist

1. Device creates with RT pipeline features as forced today.
2. `maxPipelineRayRecursionDepth` ≤ `properties.maxRayRecursionDepth`.
3. SBT bases aligned to `shaderGroupBaseAlignment`; strides multiples of `shaderGroupHandleAlignment`.
4. Handles size = `shaderGroupHandleSize * groupCount`.
5. TLAS built and barred before first `traceRays`.
6. No validation errors on `createRayTracingPipelinesKHR` / `traceRaysKHR`.
7. Indirect path only uses buffer with device address and correct struct layout.
8. Stack size set if recursion/callables require more than default.

---

## 8. Shader interaction

**Direct — introduces dedicated ray-tracing shader stages and the shader binding table (SBT).** Unlike ray query, tracing is launched with `vkCmdTraceRays*KHR` and shaders are grouped into an RT pipeline.

### Shader stages

| Stage | Role | Typical Slang / GLSL |
| --- | --- | --- |
| Ray generation | Launch primary/secondary rays | `[shader("raygeneration")]` / `rayGenerationEXT` |
| Intersection | Custom primitive hit test | `intersection` |
| Any-hit | Accept/ignore candidates (alpha test) | `anyhit` |
| Closest-hit | Shading at committed hit | `closesthit` |
| Miss | Environment / no hit | `miss` |
| Callable | Procedural subroutines via `executeCallable` | `callable` |

### Shader to host contract

1. **TLAS** descriptor (or heap handle) visible to raygen/hit/miss as configured.
2. **Payload** and **attributes** locations must match across stages (`rayPayloadEXT`, hit attributes).
3. **SBT** records: `vkGetRayTracingShaderGroupHandlesKHR` into a buffer; strides/alignments from `rayTracingPipeline` properties.
4. Recursion depth / stack: `maxRayRecursionDepth` + stack size queries.
5. `rayTraversalPrimitiveCulling` enables SPIR-V skip/cull ops when feature true (enabled).

### Example shape (GLSL-style)

```glsl
layout(set=0, binding=0, rgba8) uniform image2D image;
layout(set=0, binding=1) uniform accelerationStructureEXT as;
layout(location=0) rayPayloadEXT float payload;

void main() {
    vec3 origin = /* NDC from gl_LaunchIDEXT */;
    vec3 dir = vec3(0,0,-1);
    traceRayEXT(as, 0, 0xff, 0, 1, 0, origin, 0.0, dir, 100.0, 0);
    imageStore(image, ivec2(gl_LaunchIDEXT.xy), vec4(0, payload, 0, 1));
}
```

### Engine status

Features + properties **cached**; **no** RT shader sources, SBT, or `traceRays` yet. Prefer hybrid [VK_KHR_ray_query](./VK_KHR_ray_query.md) for first lighting experiments if avoiding full SBT complexity.

Cross-refs: [VK_KHR_acceleration_structure](./VK_KHR_acceleration_structure.md), [VK_EXT_ray_tracing_invocation_reorder](./VK_EXT_ray_tracing_invocation_reorder.md), [VK_EXT_opacity_micromap](./VK_EXT_opacity_micromap.md).

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| `VK_KHR_acceleration_structure` | TLAS + geometry for traversal |
| `VK_KHR_deferred_host_operations` | Optional deferred pipeline create |
| `VK_KHR_ray_query` | Alternative to full RT pipelines (inline queries) |
| `VK_KHR_ray_tracing_maintenance1` | `traceRaysIndirect2` and other maintenance |
| `VK_EXT_ray_tracing_invocation_reorder` | Reorder hints in RT shaders |
| `bufferDeviceAddress` | SBT and AS addresses |
| `vk_pipeline.cpp` | Graphics only today — isolate RT create |
| `vk_renderer.cpp` | Raster only today — add separate RT pass |

---

## 10. Non-goals / do not do

- Do not implement RT by hacking graphics `vkCreateGraphicsPipelines` paths.
- Do not hardcode SBT handle size (always use properties).
- Do not enable capture-replay handle features unless tooling needs them.
- Do not call `traceRays` without a complete SBT for used stages.
- Do not assume ray-query-only shaders need this extension (they need `VK_KHR_ray_query` instead).

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is ray tracing pipeline enabled? | **Yes** — extension + 3 feature bits + properties cached |
| Is it used? | **No** — no RT PSO, SBT, or `traceRays` |
| Highest-value next steps | AS foundation → RT pipeline + SBT → `traceRaysKHR` smoke pass |
| Secondary steps | Indirect dispatch; stack size; hybrid lighting |
