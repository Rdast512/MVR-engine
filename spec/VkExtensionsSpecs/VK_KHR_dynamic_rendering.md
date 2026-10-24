# VK_KHR_dynamic_rendering — Implementation Spec

**Status in this repo: PROMOTED TO CORE (Vulkan 1.3), ENABLED via `VkPhysicalDeviceVulkan13Features::dynamicRendering = true`. CONSUMED in `vk_renderer.cpp` (`beginRendering` / `endRendering`), `vk_pipeline.cpp` (`PipelineRenderingCreateInfo`), and ImGui (`UseDynamicRendering`). NOT listed as a device extension. Primary modern render-pass path.**

Official sources:

- [VK_KHR_dynamic_rendering](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_dynamic_rendering.html)
- [Proposal](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_KHR_dynamic_rendering.adoc)
- [VkPhysicalDeviceVulkan13Features](https://docs.vulkan.org/spec/latest/chapters/features.html#VkPhysicalDeviceVulkan13Features) (`dynamicRendering`)
- [vkCmdBeginRendering](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBeginRendering.html) / [vkCmdEndRendering](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdEndRendering.html)
- [VkPipelineRenderingCreateInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineRenderingCreateInfo.html)

> Note: Promoted to **Vulkan 1.3 core**. Enable only through `VkPhysicalDeviceVulkan13Features`. **Never** add `VK_KHR_dynamic_rendering` to `requiredDeviceExtension`. Prefer core `beginRendering` / `endRendering` (and maintenance10 `endRendering2KHR` when migrating ends).

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` | **Absent by design** (1.3 promotion list in comment) |
| Feature enable | `src/core/vk_device.cpp` | `Vulkan13Features{ .dynamicRendering = true }` |
| Local-read (1.4 sibling) | same feature chain | `Vulkan14Features{ .dynamicRenderingLocalRead = true }` |
| Main frame path | `src/render/vk_renderer.cpp` | **CONSUMED** — `beginRendering` / `endRendering` |
| Graphics pipeline | `src/render/vk_pipeline.cpp` | `PipelineRenderingCreateInfo`, `renderPass = nullptr` |
| ImGui | `src/core/vk_engine.cpp` | `UseDynamicRendering = true` + `PipelineRenderingCreateInfo` |
| Classic render passes | main path | **Not used** for the forward MSAA frame |

### Already wired

```cpp
// src/core/vk_device.cpp
// Vulkan13Features
{.synchronization2 = true,
 .dynamicRendering = true,
 .maintenance4 = true},
// Vulkan14Features
{.dynamicRenderingLocalRead = true, /* ... */},

// src/render/vk_renderer.cpp — recordCommandBuffer
commandBuffers[currentFrame].beginRendering(renderingInfo);
// ... draws + ImGui ...
commandBuffers[currentFrame].endRendering();

// src/render/vk_pipeline.cpp
vk::PipelineRenderingCreateInfo pipelineRenderingCreateInfo{
    .colorAttachmentCount = 1,
    .pColorAttachmentFormats = &swapChainImageFormat,
    .depthAttachmentFormat = depthFormat};
// GraphicsPipelineCreateInfo: .pNext = &pipelineRenderingCreateInfo, .renderPass = nullptr

// src/core/vk_engine.cpp — ImGui
init_info.UseDynamicRendering = true;
init_info.PipelineInfoMain.RenderPass = VK_NULL_HANDLE;
init_info.PipelineInfoMain.PipelineRenderingCreateInfo = imguiPipelineRenderingInfo;
```

### Partially / not wired yet

- `endRendering2KHR` + `RenderingEndInfoKHR` (see [VK_KHR_maintenance10.md](./VK_KHR_maintenance10.md))
- `RenderingAttachmentFlagsInfoKHR` (sRGB resolve TF control, input-attachment feedback)
- `dynamicRenderingLocalRead` **consumption** (local read / input-attachment feedback G-buffer) — feature on, no pass uses it yet
- `AttachmentLoadOp/StoreOp::eNone` for non-preserved attachments (see [VK_KHR_load_store_op_none.md](./VK_KHR_load_store_op_none.md))
- Suspend/resume multi-pass (`VK_RENDERING_SUSPENDING_BIT` / `RESUMING_BIT`)

---

## 2. What the extension provides

This is already the engine’s only main-path render model. `vk_renderer.cpp` records `beginRendering` / `endRendering` around mesh draws and ImGui; color is an MSAA attachment that **resolves into the swapchain** view. `vk_pipeline.cpp` chains `PipelineRenderingCreateInfo` and sets `renderPass = nullptr`. Layout transitions around the instance use Synchronization2 (`pipelineBarrier2`). `dynamicRenderingLocalRead` is enabled as a Vulkan 1.4 feature but is **not consumed** — the forward pass does not read attachments as input attachments.

Dynamic rendering removes `VkRenderPass` / `VkFramebuffer` objects from the primary graphics path. Attachment formats and load/store ops are specified at pipeline create (`PipelineRenderingCreateInfo`) and at command recording (`RenderingInfo` + `RenderingAttachmentInfo`).

| Feature | API | Engine relevance |
| --- | --- | --- |
| Begin/end rendering | `vkCmdBeginRendering`, `vkCmdEndRendering` | **Main path** in `vk_renderer.cpp` |
| Pipeline formats | `VkPipelineRenderingCreateInfo` (pNext of graphics pipeline) | `vk_pipeline.cpp` + ImGui |
| Attachments | `VkRenderingAttachmentInfo` | Color (MSAA + resolve) + depth |
| Nested secondary contents | `VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT` | Not used today |
| Suspend/resume | `SUSPENDING` / `RESUMING` bits | Multi-pass split recording (future) |
| Depth/stencil resolve | `resolveMode` on depth attachment | MSAA depth resolve if needed |
| Local read (1.4) | `dynamicRenderingLocalRead` + layout/input-attachment rules | Future G-buffer/feedback |

**Primary modern path for this engine.** Do not reintroduce full render-pass objects for the forward frame unless a specific legacy interop requires it.

---

## 3. Structures and commands

### 3.1 Feature (core 1.3)

```cpp
// VkPhysicalDeviceVulkan13Features
VkBool32 dynamicRendering;
```

**Already enabled.** Keep `true`.

Related (core 1.4, separate bit):

```cpp
// VkPhysicalDeviceVulkan14Features
VkBool32 dynamicRenderingLocalRead;
```

**Already enabled** in this repo; not yet consumed by a local-read pass.

### 3.2 Begin / end (core entry points)

```cpp
void vkCmdBeginRendering(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo);
void vkCmdEndRendering(VkCommandBuffer commandBuffer);
```

Vulkan-Hpp (as used today):

```cpp
cmd.beginRendering(renderingInfo);
// ...
cmd.endRendering();
```

Extensible end (maintenance10 — preferred next step when touching this path):

```cpp
cmd.endRendering2KHR(vk::RenderingEndInfoKHR{});
```

### 3.3 Rendering info and attachments

```cpp
typedef struct VkRenderingAttachmentInfo {
    VkStructureType          sType;
    const void*              pNext;
    VkImageView              imageView;
    VkImageLayout            imageLayout;
    VkResolveModeFlagBits    resolveMode;
    VkImageView              resolveImageView;
    VkImageLayout            resolveImageLayout;
    VkAttachmentLoadOp       loadOp;
    VkAttachmentStoreOp      storeOp;
    VkClearValue             clearValue;
} VkRenderingAttachmentInfo;

typedef struct VkRenderingInfo {
    VkStructureType                     sType;
    const void*                         pNext;
    VkRenderingFlags                    flags;
    VkRect2D                            renderArea;
    uint32_t                            layerCount;
    uint32_t                            viewMask;
    uint32_t                            colorAttachmentCount;
    const VkRenderingAttachmentInfo*    pColorAttachments;
    const VkRenderingAttachmentInfo*    pDepthAttachment;
    const VkRenderingAttachmentInfo*    pStencilAttachment;
} VkRenderingInfo;
```

Current engine usage (forward MSAA → swapchain resolve):

| Attachment | imageView | resolve | loadOp | storeOp |
| --- | --- | --- | --- | --- |
| Color | MSAA color (`resourceManager.colorImageView`) | Average → swapchain view | `eClear` | `eStore` |
| Depth | depth image view | none | `eClear` | `eDontCare` |

Notes:

- Color `storeOp = eStore` on the **MSAA** target is often unnecessary if only the resolve is consumed; prefer `eDontCare` or `eNone` when the MSAA contents are not read later (see load/store none doc).
- Depth `eDontCare` is appropriate when depth is not sampled after the pass.

### 3.4 Pipeline rendering create info

```cpp
typedef struct VkPipelineRenderingCreateInfo {
    VkStructureType    sType;
    const void*        pNext;
    uint32_t           viewMask;
    uint32_t           colorAttachmentCount;
    const VkFormat*    pColorAttachmentFormats;
    VkFormat           depthAttachmentFormat;
    VkFormat           stencilAttachmentFormat;
} VkPipelineRenderingCreateInfo;
```

Rules:

- Formats must be compatible with the attachments used at `beginRendering` (validation enforces).
- `renderPass` must be `VK_NULL_HANDLE` for pure dynamic-rendering pipelines.
- ImGui uses the KHR-tagged struct name in C API (`VkPipelineRenderingCreateInfoKHR`) — same layout as core.

### 3.5 Layout requirements (with synchronization2)

Attachments must be in a legal layout at begin:

| Usage | Typical layout |
| --- | --- |
| Color write | `eColorAttachmentOptimal` |
| Depth write | `eDepthStencilAttachmentOptimal` / `eDepthAttachmentOptimal` |
| Resolve target | `eColorAttachmentOptimal` (as used today for swapchain) |

This engine transitions the swapchain image to `eColorAttachmentOptimal` before `beginRendering` via `transitionImageLayout` → `pipelineBarrier2` (sync2). After `endRendering`, it transitions to `ePresentSrcKHR`.

---

## Limits

No dedicated property struct for `Vulkan13Features::dynamicRendering`. Attachment counts and extents remain **core** limits — query `VkPhysicalDeviceLimits` / `HardwareCapabilities::properties2`. Do not invent extra numeric minima for this feature.

| Limit | Source | Engine implication |
| --- | --- | --- |
| `maxColorAttachments` | core | `colorAttachmentCount` in `RenderingInfo` / `PipelineRenderingCreateInfo` must be ≤ this. Current forward pass uses **1**. |
| `maxFramebufferWidth` / `maxFramebufferHeight` / `maxFramebufferLayers` | core | `renderArea` and `layerCount` must stay inside these. Swapchain extent is the current render area. |
| Sample-count bitmasks (`framebufferColorSampleCounts`, `framebufferDepthSampleCounts`, …) | core | MSAA color + resolve-to-swapchain must be a legal sample count. |
| `maxMultiviewViewCount` | core / Vulkan 11 multiview | Only if `viewMask` / layered multiview is used. Current path: `layerCount = 1`, `viewMask` unused. |

Official: [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html).

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.cpp` | Keep `dynamicRendering = true` (1.3) and `dynamicRenderingLocalRead = true` (1.4) |
| `src/core/vk_device.hpp` | Do **not** list `VK_KHR_dynamic_rendering` |

### 4.2 Primary consumer — frame recording — DONE (baseline)

**File:** `src/render/vk_renderer.cpp` → `recordCommandBuffer`

Flow today:

1. Transition swapchain → color attachment  
2. Build `RenderingAttachmentInfo` color (MSAA + resolve) + depth  
3. `beginRendering`  
4. Bind pipeline, heaps, draw indexed  
5. ImGui draw data (same rendering instance)  
6. `endRendering`  
7. Transition swapchain → present  

**Keep this as the only main-path render model.**

### 4.3 Pipeline create — DONE

**File:** `src/render/vk_pipeline.cpp`

- `PipelineRenderingCreateInfo` on `pNext`
- `renderPass = nullptr`
- Dynamic viewport/scissor set at record time

When adding pipelines (shadow, post, UI), always pair formats with the recording attachments; never invent a dummy render pass for the main path.

### 4.4 ImGui — DONE

**File:** `src/core/vk_engine.cpp`

- `UseDynamicRendering = true`
- `RenderPass = VK_NULL_HANDLE`
- Formats match swapchain color + depth (note MSAA: ImGui init uses `device->msaaSamples`)

ImGui draws **inside** the same `beginRendering`/`endRendering` scope after main geometry.

### 4.5 Maintenance10 upgrades (next polish)

Documented fully in [VK_KHR_maintenance10.md](./VK_KHR_maintenance10.md):

- `endRendering()` → `endRendering2KHR`
- Optional sRGB resolve transfer-function flags on the color attachment pNext
- Input-attachment feedback flags only when local-read feedback exists

### 4.6 Local read (future multi-pass)

When implementing G-buffer / deferred feedback under dynamic rendering:

1. Keep `dynamicRenderingLocalRead = true` (already on).  
2. Image usage: `eInputAttachment` (+ color/depth as needed).  
3. Use local-read layouts / barriers per 1.4 rules.  
4. If writing and reading the same attachment as feedback, set maintenance10 `INPUT_ATTACHMENT_FEEDBACK` + concurrent access control flags.  
5. Do **not** reintroduce classic subpass dependencies unless interop forces it.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep dynamic rendering as sole main path | `vk_renderer.cpp`, `vk_pipeline.cpp`, `vk_engine.cpp` | Already correct modern baseline |
| P1 | `endRendering2KHR` | `vk_renderer.cpp` | Extensible end (maintenance10 already enabled) |
| P1 | Tighten MSAA color `storeOp` (`DontCare` / `None`) | `vk_renderer.cpp` | Avoid useless MSAA stores when only resolve is needed |
| P2 | sRGB resolve TF control (maintenance10 props) | `vk_renderer.cpp` | Lighting-correct MSAA resolve |
| P3 | Local-read multi-pass | new pass helpers | Only when deferred/feedback is designed |
| P3 | Suspend/resume or secondary contents | renderer | Only if splitting recording |

### Implementation recipe

1. **Keep the consumed path.** `Vulkan13Features::dynamicRendering = true` (never list the KHR extension string). `vk_renderer.cpp` already does swapchain → `eColorAttachmentOptimal`, builds MSAA color + resolve + depth `RenderingAttachmentInfo`, `beginRendering`, draws, ImGui, `endRendering`, then present layout. Do not reintroduce `VkRenderPass` / `VkFramebuffer` for this pass.
2. **Keep Synchronization2 around the instance.** Layout transitions already use `pipelineBarrier2` / `AccessFlags2`. New attachments must follow the same pattern ([VK_KHR_synchronization2.md](./VK_KHR_synchronization2.md)).
3. **Do not consume local-read on this pass.** `dynamicRenderingLocalRead` is already true (Vulkan 1.4) but unused. Leave the forward MSAA→swapchain resolve as a write-only color attachment until a G-buffer pass is designed ([VK_KHR_dynamic_rendering_local_read.md](./VK_KHR_dynamic_rendering_local_read.md)).
4. **Polish the existing instance (P1).** Switch `endRendering()` to `endRendering2KHR` (maintenance10 already enabled). Consider `storeOp = eDontCare` / `eNone` on the MSAA color when only the resolve is consumed.
5. **New pipelines** (shadow, post, UI): pair `PipelineRenderingCreateInfo` formats with the recording attachments; keep viewport/scissor dynamic as today. EDS1/2 are **core 1.3** — do not list those extensions. EDS3 remains a separate extension with limited use.
6. **Leave FSR / advanced blend / ASTC HDR off this pass.** FSR is enabled and `HardwareCapabilities::fragmentShadingRate` is cached but unused. Advanced blend properties are cached; `advancedBlendCoherentOperations` is **explicitly false**. ASTC HDR is **false** (GPU-gated).

---

## 6. Concrete integration sketch (current shape)

```cpp
// src/render/vk_renderer.cpp — illustrative of existing path + small improvements
vk::RenderingAttachmentInfo colorAttachmentInfo = {
    .imageView = resourceManager.colorImageView,
    .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
    .resolveMode = vk::ResolveModeFlagBits::eAverage,
    .resolveImageView = swapChain.swapChainImageViews[imageIndex],
    .resolveImageLayout = vk::ImageLayout::eColorAttachmentOptimal,
    .loadOp = vk::AttachmentLoadOp::eClear,
    // Prefer DontCare/None if MSAA surface is not read after resolve:
    .storeOp = vk::AttachmentStoreOp::eDontCare,
    .clearValue = clearColor
};

vk::RenderingAttachmentInfo depthAttachmentInfo = {
    .imageView = resourceManager.depthImageView,
    .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
    .loadOp = vk::AttachmentLoadOp::eClear,
    .storeOp = vk::AttachmentStoreOp::eDontCare,
    .clearValue = clearDepth
};

vk::RenderingInfo renderingInfo = {
    .renderArea = {{0, 0}, swapChain.swapChainExtent},
    .layerCount = 1,
    .colorAttachmentCount = 1,
    .pColorAttachments = &colorAttachmentInfo,
    .pDepthAttachment = &depthAttachmentInfo
};

cmd.beginRendering(renderingInfo);
// draws + ImGui
cmd.endRendering2KHR(vk::RenderingEndInfoKHR{}); // preferred once maintenance10 path is adopted
```

---

## 7. Validation / test checklist

1. Device creates with `dynamicRendering = true` without listing the KHR extension.  
2. No validation error: pipeline `renderPass` null + `PipelineRenderingCreateInfo` formats match begin attachments.  
3. Frame path: begin/end paired; no nested classic `beginRenderPass`.  
4. MSAA resolve: `resolveMode == AVERAGE`, resolve view is single-sample swapchain image.  
5. ImGui: `UseDynamicRendering`, draws only while inside dynamic rendering instance.  
6. Barriers: swapchain in color-attachment layout at begin; present layout after end.  
7. If local-read added: feature on; usage bits; no illegal self-dependency without feedback flags.  
8. If `endRendering2KHR` used: maintenance10 enabled (already required in this repo).

---

## 8. Shader interaction

**Indirect only.** Dynamic rendering removes `VkRenderPass` / subpass indices from pipeline create. Fragment shader input attachments / subpass self-dependencies are replaced by other mechanisms (e.g. local read). This engine FS is a simple color sample path — compatible with dynamic rendering as already used.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Vulkan 1.3 `synchronization2` | Barriers around the pass use `pipelineBarrier2` / stage-access 2 masks |
| Vulkan 1.4 `dynamicRenderingLocalRead` | Required for input-attachment style local read under dynamic rendering |
| `VK_KHR_maintenance10` | `endRendering2KHR`, attachment flags, sRGB resolve control |
| `VK_KHR_load_store_op_none` (core 1.3) | `NONE` load/store for unused attachments |
| Depth stencil resolve properties | Cached as `HardwareCapabilities::depthStencilResolve` if MSAA depth resolve is added |
| Descriptor heaps / push data | Bound inside the rendering instance (current path) |
| Classic `VkRenderPass` | Obsolete for main path; do not rebuild forward pass as objects |

---

## 10. Non-goals / do not do

- Do **not** add `VK_KHR_dynamic_rendering` to the device extension list on this 1.3+/1.4 target.  
- Do **not** create render pass / framebuffer objects for the main forward path.  
- Do **not** draw ImGui outside `beginRendering`/`endRendering` while `UseDynamicRendering` is true.  
- Do **not** enable local-read feedback flags without `dynamicRenderingLocalRead` and correct image usage.  
- Do **not** treat KHR-suffixed type names in ImGui as requiring the device extension — they alias core under 1.3+.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| How enabled? | **Core 1.3** — `Vulkan13Features::dynamicRendering = true` |
| Consumed? | **Yes** — `vk_renderer.cpp`, `vk_pipeline.cpp`, ImGui in `vk_engine.cpp` |
| Device extension? | **No** (correct) |
| Still valid? | **Yes** — **primary modern rendering path** |
| Sibling features | `dynamicRenderingLocalRead` (1.4, enabled, unused); maintenance10 end/flags (enabled, unused) |
| Highest-value next steps | `endRendering2KHR`, tighter store ops, optional sRGB resolve control |
