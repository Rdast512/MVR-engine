# VK_KHR_fragment_shading_rate — Implementation Spec

**Status in this repo: ENABLED (device extension + all three feature bits + properties cached), NOT CONSUMED — no VRS pipeline state, dynamic rate, or attachment rate image.**

Official sources:

- [VK_KHR_fragment_shading_rate](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_fragment_shading_rate.html)
- [Proposal](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_KHR_fragment_shading_rate.adoc)
- [VkPhysicalDeviceFragmentShadingRateFeaturesKHR](https://docs.vulkan.org/spec/latest/chapters/features.html)
- [VkPhysicalDeviceFragmentShadingRatePropertiesKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFragmentShadingRatePropertiesKHR.html)
- [vkCmdSetFragmentShadingRateKHR](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdSetFragmentShadingRateKHR.html)

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::KHRFragmentShadingRateExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → feature chain | `pipelineFragmentShadingRate`, `primitiveFragmentShadingRate`, `attachmentFragmentShadingRate` all **true** |
| Property query | `src/core/vk_device.cpp` → properties chain | Queried and assigned |
| Capability cache | `src/core/types.hpp` → `HardwareCapabilities::fragmentShadingRate` | Cached |
| API usage | `vk_pipeline.cpp`, `vk_renderer.cpp` | **None** — no VRS state, no rate attachment, no `cmdSetFragmentShadingRateKHR` |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::KHRFragmentShadingRateExtensionName,

// src/core/vk_device.cpp (feature)
// vk::PhysicalDeviceFragmentShadingRateFeaturesKHR
{.pipelineFragmentShadingRate = true,
 .primitiveFragmentShadingRate = true,
 .attachmentFragmentShadingRate = true},

// src/core/vk_device.cpp (properties)
capabilities.fragmentShadingRate =
    propertiesChain.get<vk::PhysicalDeviceFragmentShadingRatePropertiesKHR>();

// src/core/types.hpp
vk::PhysicalDeviceFragmentShadingRatePropertiesKHR fragmentShadingRate;
```

### Not wired yet

- `vkGetPhysicalDeviceFragmentShadingRatesKHR` enumeration of supported rates (optional but recommended)
- Pipeline fragment shading rate state (`VkPipelineFragmentShadingRateStateCreateInfoKHR`)
- Dynamic state `VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR` + `vkCmdSetFragmentShadingRateKHR`
- Primitive shading rate from vertex/geometry/mesh shaders
- Attachment-based VRS image + `VkRenderingFragmentShadingRateAttachmentInfoKHR` (dynamic rendering) or render-pass rate attachment
- Combiner ops between pipeline / primitive / attachment rates

---

## 2. What the extension provides

Variable-rate shading is **enabled but not consumed**. The three feature bits are forced true and `HardwareCapabilities::fragmentShadingRate` caches the property struct. The forward path in `vk_renderer.cpp` still shades 1×1: `beginRendering` with MSAA color **resolve into the swapchain**, no `setFragmentShadingRateKHR`, no rate attachment.

Consuming FSR starts with pipeline/dynamic 2×2 (clamp to cached `maxFragmentSize`), then optional attachment or primitive rates. Combiner ops and the attachment path require their respective feature bits (already true here). Keep ImGui at 1×1.

Variable Rate Shading (VRS): shade at coarser rates than 1×1 per pixel region to save fill cost.

| Mode | Feature bit | Control |
| --- | --- | --- |
| Pipeline | `pipelineFragmentShadingRate` | Static or dynamic pipeline rate + combiners |
| Primitive | `primitiveFragmentShadingRate` | Per-primitive rate from shaders |
| Attachment | `attachmentFragmentShadingRate` | Screen-space rate image |

All three bits are already forced **true** in this project.

| API | Role |
| --- | --- |
| `vkCmdSetFragmentShadingRateKHR` | Dynamic pipeline rate + combiners |
| `vkGetPhysicalDeviceFragmentShadingRatesKHR` | List supported fragment sizes / sample counts |
| Rate attachment info | Bind density/rate image for attachment VRS |

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
typedef struct VkPhysicalDeviceFragmentShadingRateFeaturesKHR {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR
    void*           pNext;
    VkBool32        pipelineFragmentShadingRate;
    VkBool32        primitiveFragmentShadingRate;
    VkBool32        attachmentFragmentShadingRate;
} VkPhysicalDeviceFragmentShadingRateFeaturesKHR;
```

| Field | This repo |
| --- | --- |
| `pipelineFragmentShadingRate` | **true** |
| `primitiveFragmentShadingRate` | **true** |
| `attachmentFragmentShadingRate` | **true** |

### 3.2 Properties (cached)

```cpp
typedef struct VkPhysicalDeviceFragmentShadingRatePropertiesKHR {
    VkStructureType sType;
    void*           pNext;
    VkExtent2D      minFragmentSize;
    VkExtent2D      maxFragmentSize;
    uint32_t        maxFragmentSizeAspectRatio;
    uint32_t        maxFragmentShadingRateCoverageSamples;
    VkSampleCountFlagBits maxFragmentShadingRateRasterizationSamples;
    VkBool32        fragmentShadingRateWithShaderDepthStencilWrites;
    VkBool32        fragmentShadingRateWithSampleMask;
    VkBool32        fragmentShadingRateWithShaderSampleMask;
    VkBool32        fragmentShadingRateWithConservativeRasterization;
    VkBool32        fragmentShadingRateWithFragmentShaderInterlock;
    VkBool32        fragmentShadingRateWithCustomSampleLocations;
    VkBool32        fragmentShadingRateStrictMultiplyCombiner;
    // ... additional fields may exist in newer headers (layered, non-trivial combiners, etc.)
} VkPhysicalDeviceFragmentShadingRatePropertiesKHR;
```

**Already cached** as `HardwareCapabilities::fragmentShadingRate`.

Use `minFragmentSize` / `maxFragmentSize` to clamp requested rates (e.g. 2×2, 4×4).

### 3.3 Commands

```cpp
void vkCmdSetFragmentShadingRateKHR(
    VkCommandBuffer commandBuffer,
    const VkExtent2D* pFragmentSize, // e.g. {2, 2}
    const VkFragmentShadingRateCombinerOpKHR combinerOps[2]);

VkResult vkGetPhysicalDeviceFragmentShadingRatesKHR(
    VkPhysicalDevice physicalDevice,
    uint32_t* pFragmentShadingRateCount,
    VkPhysicalDeviceFragmentShadingRateKHR* pFragmentShadingRates);
```

### 3.4 Pipeline state (non-dynamic)

```cpp
typedef struct VkPipelineFragmentShadingRateStateCreateInfoKHR {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR
    const void*     pNext;
    VkExtent2D      fragmentSize;
    VkFragmentShadingRateCombinerOpKHR combinerOps[2];
} VkPipelineFragmentShadingRateStateCreateInfoKHR;
```

pNext of `VkGraphicsPipelineCreateInfo`.

Dynamic rendering attachment rate (pNext of `VkRenderingInfo`):

```cpp
typedef struct VkRenderingFragmentShadingRateAttachmentInfoKHR {
    VkStructureType sType; // VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR
    const void*     pNext;
    VkImageView     imageView;
    VkImageLayout   imageLayout;
    VkExtent2D      shadingRateAttachmentTexelSize;
} VkRenderingFragmentShadingRateAttachmentInfoKHR;
```

Combiner ops (typical): `KEEP`, `REPLACE`, `MIN`, `MAX`, `MUL` — final rate combines pipeline × primitive × attachment per property rules.

---

## Limits

Properties live in the cached `HardwareCapabilities::fragmentShadingRate` (`VkPhysicalDeviceFragmentShadingRatePropertiesKHR`). Spec-required minima when `pipelineFragmentShadingRate` is supported:

| Limit | Required minimum | Notes |
| --- | --- | --- |
| `maxFragmentSize` | **(2, 2)** | Clamp requested rates to `[minFragmentSize, maxFragmentSize]` |
| `maxFragmentSizeAspectRatio` | **2** | Width/height ratio of a fragment size |

Other fields are **implementation-dependent** — read the cached struct; do not invent values:

- min/max attachment texel size
- combiner / `fragmentShadingRateStrictMultiplyCombiner`
- sample interactions (`maxFragmentShadingRateCoverageSamples`, `maxFragmentShadingRateRasterizationSamples`, with-sample-mask / conservative-raster / interlock flags)

Combiner ops and the attachment path need the matching feature bits (`pipelineFragmentShadingRate` / `primitiveFragmentShadingRate` / `attachmentFragmentShadingRate`) — all **true** in this repo.

Official: [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html).

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep extension name |
| `src/core/vk_device.cpp` | Keep three features + property cache |
| `src/core/types.hpp` | Keep `fragmentShadingRate` |

Recommended: once at init, call `getPhysicalDeviceFragmentShadingRatesKHR` and log supported rates (store in capabilities if useful).

### 4.2 Pipeline setup

**File:** `src/render/vk_pipeline.cpp`

- Add optional pNext `PipelineFragmentShadingRateStateCreateInfoKHR` for graphics PSOs, **or**
- Prefer **dynamic state** `eFragmentShadingRateKHR` so the renderer can change rates per pass without PSO spam.

### 4.3 Frame recording (primary consumer)

**File:** `src/render/vk_renderer.cpp`

Current path: `beginRendering` → draws → `endRendering` (maintenance10 may move to `endRendering2KHR`). No VRS yet.

#### Pipeline VRS (simplest win)

```cpp
const auto& fsr = device.getCapabilities().fragmentShadingRate;
vk::Extent2D rate{2, 2}; // clamp to fsr.minFragmentSize .. maxFragmentSize

vk::FragmentShadingRateCombinerOpKHR ops[2] = {
    vk::FragmentShadingRateCombinerOpKHR::eKeep,    // combine pipeline with primitive
    vk::FragmentShadingRateCombinerOpKHR::eKeep     // combine result with attachment
};

commandBuffers[currentFrame].setFragmentShadingRateKHR(rate, ops);
```

Requires the bound graphics pipeline to enable dynamic fragment shading rate state (or static state matching).

#### Attachment VRS (screen-space)

1. Create rate image (`VK_FORMAT_R8_UINT` typical) with `VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR`.
2. Texel size from properties / supported rates (`shadingRateAttachmentTexelSize`).
3. Fill rate image (compute or CPU) with encoded rates.
4. Chain `RenderingFragmentShadingRateAttachmentInfoKHR` on `RenderingInfo::pNext`.
5. Set combiners so attachment contributes (`REPLACE` / `MIN` as desired).

### 4.4 Primitive rate

Mesh/vertex shaders write primitive shading rate (SPIR-V `PrimitiveShadingRateKHR`) when `primitiveFragmentShadingRate` is true. Only after mesh/vertex paths declare the capability — coordinate with `VK_EXT_mesh_shader` usage if rates come from mesh shaders.

### 4.5 Descriptors / resources

**Files:** `src/core/vk_resource_manager.cpp`, rate-image helper

- Rate attachment is an image view, not a descriptor for the shading-rate attachment itself (it's a rendering attachment info).
- Shaders reading/writing a *custom* rate buffer for compute generation use normal storage images.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep enablement | `vk_device.*`, `types.hpp` | Already complete |
| P1 | Enumerate supported rates at init | `vk_device.cpp` | Know legal sizes |
| P1 | Dynamic `setFragmentShadingRateKHR` on forward pass | `vk_pipeline.cpp`, `vk_renderer.cpp` | Easiest quality/perf knob |
| P2 | Clamp helpers using `fragmentShadingRate` properties | shared util | Avoid invalid rates |
| P2 | Optional attachment VRS for peripheral pixels | resource manager + renderer | Foveated / edge savings |
| P3 | Primitive rates from mesh shaders | mesh shaders + combiners | Content-driven VRS |

### Implementation recipe

1. **Enablement is done.** Extension listed; three features true; properties cached on `HardwareCapabilities::fragmentShadingRate`. Do not re-query at draw time except for optional `getPhysicalDeviceFragmentShadingRatesKHR` logging.
2. **Frame model stays dynamic rendering + sync2.** Add VRS inside the existing `beginRendering` / MSAA resolve / `endRendering` instance. Prefer dynamic `eFragmentShadingRateKHR` + `setFragmentShadingRateKHR` so ImGui can force 1×1 in the same instance.
3. **First consumer: pipeline/dynamic 2×2.** Clamp to `capabilities.fragmentShadingRate.maxFragmentSize` (minimum guaranteed **(2, 2)**). Combiners `KEEP`/`KEEP` until primitive or attachment rates exist.
4. **Check MSAA interaction** against cached `maxFragmentShadingRateRasterizationSamples` before combining VRS with the engine’s MSAA resolve-to-swapchain.
5. **Do not block on unused siblings.** `dynamicRenderingLocalRead` is on but unused. Advanced blend properties cached; `advancedBlendCoherentOperations` **explicitly false**. ASTC HDR **false** (GPU-gated). EDS1/2 are core 1.3; EDS3 is still an extension with limited use — FSR dynamic state is its own enum, not EDS3.

---

## 6. Concrete integration sketch (renderer)

```cpp
// Pipeline create (once): include dynamic state
// vk::DynamicState::eFragmentShadingRateKHR

// Per-frame or per-pass in vk_renderer.cpp:
const auto& fsrProps = /* capabilities */.fragmentShadingRate;

vk::Extent2D fragmentSize{1, 1};
if (enableVrs2x2) {
    fragmentSize.width  = std::clamp(2u, fsrProps.minFragmentSize.width,  fsrProps.maxFragmentSize.width);
    fragmentSize.height = std::clamp(2u, fsrProps.minFragmentSize.height, fsrProps.maxFragmentSize.height);
}

vk::FragmentShadingRateCombinerOpKHR combiners[2] = {
    vk::FragmentShadingRateCombinerOpKHR::eKeep,
    vk::FragmentShadingRateCombinerOpKHR::eKeep
};

cmd.setFragmentShadingRateKHR(fragmentSize, combiners);

cmd.beginRendering(renderingInfo);
// ... draws ...
cmd.endRendering(); // or endRendering2KHR under maintenance10
```

For MSAA: check `maxFragmentShadingRateRasterizationSamples` and related property bools before combining VRS with the engine’s MSAA forward path.

---

## 7. Validation / test checklist

1. Device creates with all three FSR features (already).
2. Capability dump: min/max fragment size, coverage samples, MSAA interaction flags.
3. `getPhysicalDeviceFragmentShadingRatesKHR` list non-empty on supporting GPUs.
4. Dynamic rate: pipeline declares dynamic state; no validation error on `setFragmentShadingRateKHR`.
5. Visual: 2×2 rate softens fine detail; disable for UI/ImGui pass if needed (rate 1×1).
6. Attachment path: image usage bits, layout, texel size match properties.
7. Combiners: document chosen ops; verify against `fragmentShadingRateStrictMultiplyCombiner` if using `MUL`.

---

## 8. Shader interaction

**Direct — shaders and/or fixed function control per-fragment shading rate (VRS).** Rates can come from pipeline/dynamic state, a shading-rate attachment image, or **primitive rates** written by vertex/geometry/mesh shaders (`PrimitiveShadingRateKHR`) when `primitiveFragmentShadingRate` is true (mesh needs optional `primitiveFragmentShadingRateMeshShader`).

Fragment shaders still run and may execute at coarser rate (multiple pixels per invocation). Avoid assuming 1:1 pixel-to-invocation for derivatives when rate is not 1x1.

### Engine status

Extension **enabled**, **not consumed**. No engine shader writes primitive rates. Coordinate with [VK_EXT_mesh_shader](./VK_EXT_mesh_shader.md) if mesh-driven rates are desired later.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Dynamic rendering | Rate attachment via `RenderingFragmentShadingRateAttachmentInfoKHR` |
| MSAA in `vk_renderer.cpp` | Sample count vs `maxFragmentShadingRateRasterizationSamples` |
| `VK_EXT_mesh_shader` | Possible primitive rate source |
| `VK_EXT_extended_dynamic_state3` / shader object | Dynamic state availability patterns |
| ImGui pass | Prefer 1×1 rate for UI sharpness |
| maintenance10 | Orthogonal; still use same begin/end rendering scope |

---

## 10. Non-goals / do not do

- Do not force coarse rates on UI / text / debug lines.
- Do not request fragment sizes outside min/max or unsupported enumerate list.
- Do not enable attachment VRS without the rate image usage bit and correct layout.
- Do not assume all three feature bits exist on every GPU if extension ever becomes optional — today they are required true at create.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is fragment shading rate enabled? | **Yes** — extension + pipeline/primitive/attachment features + properties cached |
| Is it used? | **No** — pure raster without VRS state |
| Highest-value next steps | Dynamic `setFragmentShadingRateKHR` + rate enumeration; keep ImGui at 1×1 |
| Secondary steps | Attachment VRS; mesh primitive rates; MSAA interaction validation |
