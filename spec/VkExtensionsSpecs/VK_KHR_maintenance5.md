# VK_KHR_maintenance5 — Implementation Spec

**Status in this repo: PROMOTED TO CORE (Vulkan 1.4), CONSUMED (inline SPIR-V + core Flags2 + VMA + property log). No index bind / render-area / layout queries — no existing call sites.**

Official sources:

- [VK_KHR_maintenance5](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_maintenance5.html)
- [Proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_maintenance5.html)
- [VkPhysicalDeviceVulkan14Features](https://docs.vulkan.org/spec/latest/chapters/features.html) (`maintenance5`)
- [VkPhysicalDeviceMaintenance5Properties](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMaintenance5Properties.html)

> **Promoted to core:** Functionality is core Vulkan 1.4 with the KHR suffix omitted. KHR type/enum/command names remain aliases. Prefer core / unprefixed names (or Vulkan-Hpp without `KHR`) in new code.  
> **Do not** list `VK_KHR_maintenance5` in `requiredDeviceExtension` — it is covered by `VkPhysicalDeviceVulkan14Features`.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | **Must not be listed** — promoted to 1.4; documented in the promoted-extensions note |
| Feature enable | `src/core/vk_device.cpp` → `createLogicalDevice()` feature chain | `vk::PhysicalDeviceVulkan14Features{ .maintenance5 = true }` |
| Property query | `src/core/vk_device.cpp` → properties chain | `vk::PhysicalDeviceMaintenance5Properties` queried |
| Capability cache | `src/core/types.hpp` → `HardwareCapabilities::maintenance5` | Cached |
| VMA | `src/core/vk_allocator.hpp` | `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT` set |
| API usage | pipeline / renderer | **Consumed on existing paths** — inline SPIR-V on mesh pipeline stages; core `PipelineCreateFlags2CreateInfo` + `eDescriptorHeapEXT`. No `bindIndexBuffer2` (mesh/DGC, no index bind). No render-area / layout queries. |

### Already wired

```cpp
// src/core/vk_device.cpp (feature chain) — vk::PhysicalDeviceVulkan14Features
{.globalPriorityQuery = true,
 .dynamicRenderingLocalRead = true,
 .maintenance5 = true,
 .maintenance6 = true,
 .hostImageCopy = true},

// src/core/vk_device.cpp (properties chain)
capabilities.maintenance5 =
    propertiesChain.get<vk::PhysicalDeviceMaintenance5Properties>();

// src/core/types.hpp
vk::PhysicalDeviceMaintenance5Properties maintenance5;

// src/core/vk_allocator.hpp
allocatorInfo.flags = /* ... */ | VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT;

// src/render/vk_pipeline.cpp — Flags2 create-info for descriptor-heap path
vk::PipelineCreateFlags2CreateInfo pipelineFlags2CreateInfo{
    .pNext = &pipelineRenderingCreateInfo,
    .flags = vk::PipelineCreateFlagBits2::eDescriptorHeapEXT
};

// src/render/vk_pipeline.cpp — inline SPIR-V (no VkShaderModule)
vk::ShaderModuleCreateInfo shaderModuleInfo{ .codeSize = spirv.size(), .pCode = ... };
vk::PipelineShaderStageCreateInfo mesh{ .pNext = &shaderModuleInfo, .module = {}, .pName = "meshMain" };
```

### Intentionally not wired (no existing call site)

- `vkCmdBindIndexBuffer2` — mesh/DGC path; no index buffer bind
- `vkGetRenderingAreaGranularity` — would add render-area policy
- `vkGetDeviceImageSubresourceLayout` / `vkGetImageSubresourceLayout2` — no linear/host layout planning
- `BufferUsageFlags2CreateInfo` — 32-bit usage bits suffice
- Property-driven raster branches and new packed formats — no content/path need

---

## 2. What maintenance5 provides

Maintenance5 is a **cumulative maintenance bag**: small, independent fixes. None alone justifies a dedicated extension. All are core when `maintenance5` is enabled under Vulkan 1.4 (`Vulkan14Features`). It is **not a replacement** for maintenance4 (1.3 / `maxBufferSize` / device-level memory requirements) or earlier 1.1 maintenance; later bags (6–10) add more on top. Do not re-list `VK_KHR_maintenance5`.

| Feature | New API / bit / property | Engine relevance |
| --- | --- | --- |
| Index buffer range | `vkCmdBindIndexBuffer2` + size | Tighter robustness for index subranges |
| Dynamic rendering area granularity | `vkGetRenderingAreaGranularity` | Tile-friendly `renderArea` for `beginRendering` |
| Image subresource layout without image object | `vkGetDeviceImageSubresourceLayout` | Host layout / staging without dummy images |
| Image subresource layout 2 | `vkGetImageSubresourceLayout2` | Extensible layout query on existing images |
| Pipeline create flags 2 | `VkPipelineCreateFlags2CreateInfo` | **Already used** for `eDescriptorHeapEXT` |
| Buffer usage flags 2 | `VkBufferUsageFlags2CreateInfo` | Future buffer/view usage subsets |
| Inline SPIR-V on stage create | `VkShaderModuleCreateInfo` pNext of stage; `module = VK_NULL_HANDLE` | Deprecate separate shader modules — candidate |
| Remaining array layers | `VK_REMAINING_ARRAY_LAYERS` on `VkImageSubresourceLayers` | Copies / blits / transitions |
| Whole size on vertex bind 2 | `VK_WHOLE_SIZE` in `bindVertexBuffers2` sizes | Dynamic vertex binding helpers |
| Formats | `A1B5G5R5_UNORM_PACK16`, `A8_UNORM` | Packed / alpha-only content |
| Non-strict lines | `nonStrictSinglePixelWideLinesUseParallelogram`, `nonStrictWideLinesUseParallelogram` | Debug/line rendering correctness |
| Early fragment sample order | `earlyFragmentMultisampleCoverageAfterSampleCounting`, `earlyFragmentSampleMaskTestBeforeSampleCounting` | MSAA / early tests documentation |
| Polygon mode point size | `polygonModePointSize` | Wire/point rasterization |
| Depth/stencil swizzle one | `depthStencilSwizzleOneSupport` | Component mapping on D/S views |
| Default PointSize 1.0 | behavioral | Points without writing `PointSize` |
| Device-lost propagation | stronger guarantees | Error handling |
| Format query flexibility | unknown formats allowed in queries | Simpler format probing |
| Cross image-type copies | clarified | 1D/2D/3D copy flexibility |
| Buffer view usage subset | via usage flags 2 | Emulation / flexible views |

---

## 3. Structures and commands

### 3.1 Feature (enable at device create) — CORE via Vulkan 1.4

```cpp
// Prefer Vulkan14Features (this repo). Standalone Maintenance5Features is the
// pre-promotion / alias form — do not re-enable as a device extension.
typedef struct VkPhysicalDeviceVulkan14Features {
    // ...
    VkBool32 maintenance5;
    // ...
} VkPhysicalDeviceVulkan14Features;
```

**Already enabled** in this project: `maintenance5 = true` inside `PhysicalDeviceVulkan14Features`.

### 3.2 Properties (query after physical device pick)

```cpp
typedef struct VkPhysicalDeviceMaintenance5Properties {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_PROPERTIES
    void*           pNext;
    VkBool32        earlyFragmentMultisampleCoverageAfterSampleCounting;
    VkBool32        earlyFragmentSampleMaskTestBeforeSampleCounting;
    VkBool32        depthStencilSwizzleOneSupport;
    VkBool32        polygonModePointSize;
    VkBool32        nonStrictSinglePixelWideLinesUseParallelogram;
    VkBool32        nonStrictWideLinesUseParallelogram;
} VkPhysicalDeviceMaintenance5Properties;
```

| Property | Meaning |
| --- | --- |
| `earlyFragmentMultisampleCoverageAfterSampleCounting` | Multisample coverage after sample counting under EarlyFragmentTests |
| `earlyFragmentSampleMaskTestBeforeSampleCounting` | Sample mask test before sample counting under EarlyFragmentTests |
| `depthStencilSwizzleOneSupport` | `VK_COMPONENT_SWIZZLE_ONE` is defined for depth/stencil |
| `polygonModePointSize` | `PointSize` affects `VK_POLYGON_MODE_POINT` rasterization |
| `nonStrictSinglePixelWideLinesUseParallelogram` | Algorithm for width-1.0 non-strict lines |
| `nonStrictWideLinesUseParallelogram` | Algorithm for wide non-strict lines |

**Already cached** as `HardwareCapabilities::maintenance5`.

### 3.3 Key commands (prefer core names)

```cpp
// Index bind with size (robustness over subrange)
void vkCmdBindIndexBuffer2(
    VkCommandBuffer commandBuffer,
    VkBuffer        buffer,
    VkDeviceSize    offset,
    VkDeviceSize    size,
    VkIndexType     indexType);

// Dynamic rendering preferred render-area granularity
void vkGetRenderingAreaGranularity(
    VkDevice                        device,
    const VkRenderingAreaInfo*      pRenderingAreaInfo,
    VkExtent2D*                     pGranularity);

// Subresource layout without creating an image
void vkGetDeviceImageSubresourceLayout(
    VkDevice                              device,
    const VkDeviceImageSubresourceInfo*   pInfo,
    VkSubresourceLayout2*                 pLayout);

// Extensible subresource layout for an existing image
void vkGetImageSubresourceLayout2(
    VkDevice                    device,
    VkImage                     image,
    const VkImageSubresource2*  pSubresource,
    VkSubresourceLayout2*       pLayout);
```

Vulkan-Hpp (core / preferred):

```cpp
commandBuffer.bindIndexBuffer2(buffer, offset, size, vk::IndexType::eUint32);
device.getRenderingAreaGranularity(renderingAreaInfo);
device.getDeviceImageSubresourceLayout(info);
device.getImageSubresourceLayout2(image, subresource);
```

Aliases `*KHR` remain valid.

### 3.4 Pipeline create flags 2

```cpp
typedef struct VkPipelineCreateFlags2CreateInfo {
    VkStructureType           sType; // VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO
    const void*               pNext;
    VkPipelineCreateFlags2    flags;
} VkPipelineCreateFlags2CreateInfo;
```

Chain on `VkGraphicsPipelineCreateInfo` / compute / RT pipeline create infos. This engine already chains it when using descriptor heaps (`eDescriptorHeapEXT`).

### 3.5 Buffer usage flags 2

```cpp
typedef struct VkBufferUsageFlags2CreateInfo {
    VkStructureType        sType; // VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO
    const void*            pNext;
    VkBufferUsageFlags2    usage;
} VkBufferUsageFlags2CreateInfo;
```

Chain on buffer create, buffer-view create, and related external/descriptor-buffer infos when 32-bit usage bits are insufficient or a usage subset is required.

### 3.6 Inline shader module (deprecate separate modules)

```cpp
vk::ShaderModuleCreateInfo smci{
    .codeSize = spirvBytes,
    .pCode    = spirvWords
};

vk::PipelineShaderStageCreateInfo stage{
    .pNext  = &smci,
    .stage  = vk::ShaderStageFlagBits::eVertex,
    .module = nullptr, // VK_NULL_HANDLE
    .pName  = "vertMain"
};
```

Shader modules are **not removed**; inlining avoids a transient object and an extra copy. Recommended for new pipeline paths.

### 3.7 Formats

| Format | Notes |
| --- | --- |
| `VK_FORMAT_A1B5G5R5_UNORM_PACK16` | Packed 1555 (A in MSBs of the pack layout as specified) |
| `VK_FORMAT_A8_UNORM` | Single-channel alpha |

KHR-suffixed format names are aliases.

---

## Limits

See [Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html) and [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html#limits-required).

`VkPhysicalDeviceMaintenance5Properties` fields (`earlyFragmentMultisampleCoverageAfterSampleCounting`, `earlyFragmentSampleMaskTestBeforeSampleCounting`, `depthStencilSwizzleOneSupport`, `polygonModePointSize`, `nonStrictSinglePixelWideLinesUseParallelogram`, `nonStrictWideLinesUseParallelogram`) are **behavior queries**, not numeric min/max. Cached as `HardwareCapabilities::maintenance5`.

The same bag also introduces `VkBufferUsageFlags2CreateInfo` / `VkPipelineCreateFlags2CreateInfo` (already **partial** in this engine: pipeline Flags2 for descriptor heaps). Those are flag-space expansions, not Required Limits numbers.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep maintenance5 **out** of `requiredDeviceExtension` |
| `src/core/vk_device.cpp` | Keep `Vulkan14Features::maintenance5 = true` + properties chain |
| `src/core/types.hpp` | Keep `HardwareCapabilities::maintenance5` |
| `src/core/vk_allocator.hpp` | Keep `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT` |

Optional hardening:

1. Log maintenance5 properties at device init next to other capability dumps.
2. Prefer unprefixed Vulkan-Hpp types in new code (`PhysicalDeviceMaintenance5Properties`, not `*KHR`).

### 4.2 Pipeline path — DONE

**File:** `src/render/vk_pipeline.cpp`

Core Flags2 names + inline SPIR-V on mesh + fragment stages. `ResourceManager::createShaderModule` removed (only call site migrated).

### 4.3 Index bind — not applicable

No index buffer. Mesh draw is DGC `DRAW_MESH_TASKS`. Do not reintroduce `indexBuffer` to use `bindIndexBuffer2`.

### 4.4 Dynamic rendering area granularity

**File:** `src/render/vk_renderer.cpp` (where `RenderingInfo::renderArea` is set)

```cpp
vk::RenderingAreaInfo areaInfo{
    .viewMask              = 0,
    .colorAttachmentCount  = 1,
    .pColorAttachmentFormats = &swapChainImageFormat,
    .depthAttachmentFormat = depthFormat,
    .stencilAttachmentFormat = vk::Format::eUndefined
};
vk::Extent2D gran = device.getRenderingAreaGranularity(areaInfo);
// Align renderArea.offset/extent to gran when beneficial for TBDR
```

Dynamic rendering is already the engine’s path (`beginRendering` / `endRendering`); this is an optimization/hint, not a correctness requirement on all GPUs.

### 4.5 Subresource layout queries

**Files:** `src/core/texture_manager.cpp`, `src/core/vk_resource_manager.cpp` (if host-visible linear images or staging layout math is added)

Use `getDeviceImageSubresourceLayout` when planning linear/host images before creation; use `getImageSubresourceLayout2` for extensible queries on existing images.

### 4.6 Constants already useful without new call sites

- `VK_REMAINING_ARRAY_LAYERS` on `ImageSubresourceLayers` in copies / mip generation
- `VK_WHOLE_SIZE` when adopting `bindVertexBuffers2` with explicit sizes

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep feature + properties + VMA bit | `vk_device.*`, `types.hpp`, `vk_allocator.hpp` | Already correct; do not add extension string |
| P1 | Prefer core Flags2 type names in pipeline | `vk_pipeline.cpp` | **Done** |
| P1 | Inline SPIR-V on pipeline stages | `vk_pipeline.cpp` | **Done** — dropped `createShaderModule` |
| P2 | `bindIndexBuffer2` with real range size | `vk_renderer.cpp` | **Skip** — no index bind (mesh/DGC) |
| P2 | Optional `getRenderingAreaGranularity` align | `vk_renderer.cpp` | **Skip** — would add render-area policy |
| P3 | Log maintenance5 properties | `vk_device.cpp` | **Done** |
| P3 | Layout queries / usage flags 2 / new formats | resource / texture paths | **Skip** — no existing path needs them |

### Implementation recipe

1. Confirm `src/core/types.hpp` → `HardwareCapabilities` caches `maintenance3`, `maintenance4`, `maintenance5`, `maintenance6`, `maintenance7`, `maintenance9`, and `maintenance10`. There is **no `maintenance8` properties field**.
2. Confirm `src/core/vk_device.cpp` property chain includes `PhysicalDeviceMaintenance3Properties`–`Maintenance7PropertiesKHR`, `Maintenance9PropertiesKHR`, and `Maintenance10PropertiesKHR` (skip 8). Feature bits: `Vulkan13Features::maintenance4`, `Vulkan14Features::maintenance5`/`maintenance6`, plus `Maintenance7/8/10FeaturesKHR`; `maintenance9` is currently forced **false** (VVL #12449) while its properties are still queried.
3. Confirm `src/core/vk_allocator.hpp` already sets `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT` and `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT`. Keep the MAINTENANCE5 bit aligned with `maintenance5 = true`.
4. Prefer core (unprefixed) Flags2 type names in `vk_pipeline.cpp`; keep the existing `eDescriptorHeapEXT` Flags2 path.
5. Next consume: inline SPIR-V on pipeline stages; optional `bindIndexBuffer2`, `getRenderingAreaGranularity`, and property-driven raster branches.

---

## 6. Concrete integration sketch (pipeline inline SPIR-V)

```cpp
const auto spirv = readFile(shaderPath);
// SPIR-V is uint32_t-aligned; codeSize is byte size.
vk::ShaderModuleCreateInfo smci{
    .codeSize = spirv.size(),
    .pCode    = reinterpret_cast<const uint32_t*>(spirv.data())
};

vk::PipelineShaderStageCreateInfo vert{
    .pNext  = &smci,
    .stage  = vk::ShaderStageFlagBits::eVertex,
    .module = nullptr,
    .pName  = "vertMain"
};
vk::PipelineShaderStageCreateInfo frag{
    .pNext  = &smci, // same module create info OK if one combined module
    .stage  = vk::ShaderStageFlagBits::eFragment,
    .module = nullptr,
    .pName  = "fragMain"
};

// Keep Flags2 for descriptor-heap pipelines:
vk::PipelineCreateFlags2CreateInfo flags2{
    .pNext = &pipelineRenderingCreateInfo,
    .flags = vk::PipelineCreateFlagBits2::eDescriptorHeapEXT
};

vk::GraphicsPipelineCreateInfo pipelineInfo{
    .pNext      = useDescriptorHeaps ? static_cast<const void*>(&flags2)
                                     : static_cast<const void*>(&pipelineRenderingCreateInfo),
    .stageCount = 2,
    .pStages    = stages.data(),
    // ...
};
```

Ensure `spirv` (and `smci`) outlive the `createGraphicsPipelines` call.

---

## 7. Validation / test checklist

1. Device creates with Vulkan 1.4 and `maintenance5 = true` (no `VK_KHR_maintenance5` in extension list).
2. Capability dump shows all six maintenance5 properties.
3. VMA allocator creates successfully with `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT`.
4. Descriptor-heap pipelines still create with Flags2 + `eDescriptorHeapEXT`.
5. After inline SPIR-V: no validation errors; no leaked shader modules for that path.
6. If `bindIndexBuffer2` is used: size covers drawn index range; validation clean on robustness-enabled devices.
7. If render-area granularity is applied: `renderArea` still covers the intended pixels; no scissor/viewport mismatch.

---

## 8. Shader interaction

**Indirect — pipeline creation convenience for SPIR-V.** Inline `VkShaderModuleCreateInfo` on stage create avoids separate `VkShaderModule` objects; shader content is identical SPIR-V from Slang. Extended pipeline create flags (descriptor-heap flag2) are used when creating heap-model pipelines.

### Engine status

Consumed on the mesh pipeline: inline SPIR-V chained on both stages; no standalone `VkShaderModule`.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Vulkan 1.4 core | Promotion host; enable via `Vulkan14Features` only |
| Vulkan 1.3 `dynamicRendering` | Required baseline for rendering-area granularity relevance |
| `VK_EXT_descriptor_heap` | Uses pipeline create flags 2 (`eDescriptorHeapEXT`) — **already wired** |
| VMA | Maintenance5 allocator bit already set |
| Maintenance6–10 | Separate bags; do not conflate enablement |
| Shader object (`VK_EXT_shader_object`) | Alternative to classic pipelines; still benefits from m5 properties |

---

## 10. Non-goals / do not do

- **Do not** add `VK_KHR_maintenance5` to `requiredDeviceExtension`.
- Do not require a second enable path via `PhysicalDeviceMaintenance5FeaturesKHR` when `Vulkan14Features` is already chained.
- Do not remove the Flags2 path used for descriptor heaps.
- Do not force new formats (`A8` / `A1B5G5R5`) without content need.
- Do not document this as still-required device extension status — it is **PROMOTED TO CORE**.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| How is Maintenance5 enabled? | **Core Vulkan 1.4** — `Vulkan14Features::maintenance5 = true` |
| Properties cached? | **Yes** — `HardwareCapabilities::maintenance5` |
| Is it used? | **Yes on existing paths** — VMA bit, core Flags2 + `eDescriptorHeapEXT`, inline SPIR-V on mesh pipeline, properties logged. No index bind / area / layout (no call sites). |
| Highest-value next steps | None unless a vertex/index or linear-image path is added |
| Extension string? | **Never list** in `requiredDeviceExtension` |
