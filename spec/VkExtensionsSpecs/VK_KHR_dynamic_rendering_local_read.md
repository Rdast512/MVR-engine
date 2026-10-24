# VK_KHR_dynamic_rendering_local_read — Implementation Spec

**Status in this repo: PROMOTED TO CORE (Vulkan 1.4), ENABLED via Vulkan14Features::dynamicRenderingLocalRead, NOT CONSUMED**

Official sources:

- [VK_KHR_dynamic_rendering_local_read](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_dynamic_rendering_local_read.html)
- [Proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_dynamic_rendering_local_read.html)
- [VkPhysicalDeviceVulkan14Features](https://docs.vulkan.org/spec/latest/chapters/features.html) (`dynamicRenderingLocalRead`)
- [Dynamic rendering](https://docs.vulkan.org/spec/latest/chapters/renderpass.html) (local-read flags / input attachments)

> **Promoted to core:** Local-read dynamic rendering is core Vulkan 1.4. Prefer core / unprefixed flag and type names. KHR aliases remain valid.  
> **Do not** list `VK_KHR_dynamic_rendering_local_read` in `requiredDeviceExtension`.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | **Must not be listed** — promoted to 1.4 |
| Feature enable | `src/core/vk_device.cpp` → feature chain | `vk::PhysicalDeviceVulkan14Features{ .dynamicRenderingLocalRead = true }` |
| Property query | N/A | No dedicated local-read property struct required for basic enablement |
| Capability cache | `src/core/types.hpp` | No separate field (feature is forced on at device create) |
| API usage | `src/render/vk_renderer.cpp` | **Not consumed** — `beginRendering` / `endRendering` without local-read flags; no input-attachment G-buffer path |

### Already wired

```cpp
// src/core/vk_device.cpp — vk::PhysicalDeviceVulkan14Features
{.globalPriorityQuery = true,
 .dynamicRenderingLocalRead = true,
 .maintenance5 = true,
 .maintenance6 = true,
 .hostImageCopy = true},

// src/core/vk_device.cpp — Vulkan 1.3 dynamic rendering baseline
// vk::PhysicalDeviceVulkan13Features
{.dynamicRendering = true, /* ... */},
```

### Current rendering path (no local read)

```cpp
// src/render/vk_renderer.cpp
vk::RenderingInfo renderingInfo = {
    .renderArea = {...},
    .layerCount = 1,
    .colorAttachmentCount = 1,
    .pColorAttachments = &colorAttachmentInfo,
    .pDepthAttachment = &depthAttachmentInfo
};
commandBuffers[currentFrame].beginRendering(renderingInfo);
// draws — forward color + depth, MSAA resolve to swapchain
commandBuffers[currentFrame].endRendering();
```

No:

- `VK_RENDERING_CONTENTS_*` local-read related flags for input-attachment feedback paths
- Input attachment image usage on G-buffer targets
- `VK_RENDERING_ATTACHMENT_INPUT_ATTACHMENT_FEEDBACK_BIT` (from **maintenance10**) for write+read feedback
- Fragment shader `input_attachment_index` / local-read mappings for multipass within one dynamic rendering instance

### Not wired yet

- Multi-attachment deferred / G-buffer pass with input attachment reads inside one `beginRendering` scope
- Local-read specific `RenderingInfo` / attachment flags
- Interaction with maintenance10 feedback + concurrent access control bits
- Pipeline layout / fragment interfaces for input attachments under dynamic rendering

---

## 2. What dynamic rendering local read provides

Local-read is the 1.4 complement to the engine’s already-consumed dynamic rendering path. `vk_renderer.cpp` uses `beginRendering` / `endRendering` with MSAA color **resolve into the swapchain**; that pass never maps attachments as input attachments. The feature is forced on so a later G-buffer or feedback loop can stay inside one rendering instance instead of rebuilding classic subpasses.

Until that pass exists, treat local-read as infrastructure: keep the bit true, keep images without `eInputAttachment` unless they will actually be read, and do not set maintenance10 feedback flags on the forward resolve.

Classic render passes allow subpass self-dependencies and input attachments so a later subpass can read data written earlier in the same render pass instance. Dynamic rendering originally lacked an equivalent “read what we just wrote” path inside one `vkCmdBeginRendering` / `vkCmdEndRendering` instance.

**Local read** restores that capability for dynamic rendering:

| Capability | Role |
| --- | --- |
| Input attachments in dynamic rendering | Fragment shaders read color/depth attachments as input attachments without ending the rendering instance |
| Local-read mappings | Map attachment indices to input attachment indices for the dynamic pass |
| Enables multipass-style algorithms | Soft particles, deferred lighting tiles, various “read previous attachment” techniques without a full second render pass instance |

**Baseline still required:** `dynamicRendering` (Vulkan 1.3 features — already enabled).

---

## 3. Structures, flags, and interactions

### 3.1 Feature — CORE via Vulkan 1.4

```cpp
// Inside VkPhysicalDeviceVulkan14Features
VkBool32 dynamicRenderingLocalRead;
```

**Already enabled:** `dynamicRenderingLocalRead = true`.

### 3.2 Core commands / state (names simplified)

Exact struct names evolved through the EXT/KHR promotion; under Vulkan 1.4 prefer **core** enumerants and structs from the installed headers. Conceptually:

1. Begin a dynamic rendering instance with attachments that will also be read as input attachments (usage includes `INPUT_ATTACHMENT` as well as color/depth usage).
2. Provide local-read mapping / location information so the implementation knows which attachments are readable as inputs.
3. In the fragment shader, declare input attachments / use the local-read mechanism as specified for core 1.4.
4. Observe synchronization and layout rules for local-read attachments (general / feedback layouts as required by the path you use).

Vulkan-Hpp apps typically set flags on `vk::RenderingInfo` and chain any local-read mapping structs required by the SDK version.

### 3.3 Interaction with VK_KHR_maintenance10 (this repo)

Maintenance10 (still a **device extension** in this project) adds:

| Bit / flag | Purpose |
| --- | --- |
| `VK_RENDERING_ATTACHMENT_INPUT_ATTACHMENT_FEEDBACK_BIT_KHR` | Attachment is written and read as input feedback |
| `VK_RENDERING_LOCAL_READ_CONCURRENT_ACCESS_CONTROL_BIT_KHR` | Concurrent access control for local-read patterns |

These require **`dynamicRenderingLocalRead`** (already enabled). See [VK_KHR_maintenance10.md](./VK_KHR_maintenance10.md) for attachment-flags integration.

**Do not** set feedback / concurrent-access flags on the current forward MSAA pass — it does not use input attachments.

### 3.4 Image usage and layouts

For attachments that will be read as inputs inside the same dynamic rendering instance:

- Include `vk::ImageUsageFlagBits::eInputAttachment` (and color or depth usage).
- Follow local-read layout requirements for the attachment (often a general or feedback-capable layout depending on the exact algorithm and maintenance10 feedback bits).
- Ensure framebuffer-like attachment formats in `PipelineRenderingCreateInfo` match the local-read configuration.

---

## Limits

No dedicated numeric Required Limits row for `dynamicRenderingLocalRead` (`Vulkan14Features`). Constraints are inherited. Do not invent extra numeric minima.

| Topic | Constraint | Engine note |
| --- | --- | --- |
| Dedicated property struct | None required for basic enablement | No `HardwareCapabilities` field |
| Input attachment indices | Still interact with `maxPerStageDescriptorInputAttachments` (historical core limit) | Only when a fragment shader declares input attachments |
| Attachment count / extents | Same core limits as dynamic rendering (`maxColorAttachments`, framebuffer size, sample counts) | Current pass uses 1 color + depth |
| Feedback loops | Image usage must include `INPUT_ATTACHMENT`; when writing and reading the same attachment, set maintenance10 `INPUT_ATTACHMENT_FEEDBACK` (+ concurrent access control as required) | Not used on the forward MSAA resolve |

Official: [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html).

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep this feature **out** of `requiredDeviceExtension` |
| `src/core/vk_device.cpp` | Keep `dynamicRenderingLocalRead = true` next to other Vulkan14 features |
| `src/core/vk_device.cpp` | Keep `dynamicRendering = true` in Vulkan13 features |

### 4.2 Primary future consumer — multipass / G-buffer

**Files (when a deferred or multipass path is added):**

- `src/render/vk_renderer.cpp` — `beginRendering` / attachments / barriers
- `src/render/vk_pipeline.cpp` — pipeline rendering formats, fragment interfaces
- `src/core/vk_resource_manager.cpp` — G-buffer images with `eInputAttachment`
- Shaders under the engine shader tree — input attachment declarations

**Not the current path:** the existing forward pass resolves MSAA color to the swapchain and does not re-read color/depth as input attachments mid-pass.

### 4.3 Suggested first real consumer

A minimal educational path:

1. Two color attachments (e.g. albedo + normal) + depth.
2. Single `beginRendering` with both color attachments stored.
3. Draw G-buffer.
4. Draw lighting full-screen triangle reading attachments as inputs (**local read**), without `endRendering` between.
5. `endRendering` (or `endRendering2KHR` once maintenance10 is consumed).

Only then set maintenance10 feedback bits if write/read feedback on the same attachment is required.

### 4.4 Pipeline configuration

`PipelineRenderingCreateInfo` already supplies color/depth formats for dynamic rendering. Extend:

- `colorAttachmentCount` / formats for multiple G-buffer targets
- Fragment output locations matching those attachments
- Input attachment bindings / local-read mappings consistent with the rendering info

Descriptor heaps vs input attachments: input attachments are a **framebuffer-local** mechanism; they are complementary to bindless textures, not a replacement for the heap path’s sampled images.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep feature enabled; never list as device extension | `vk_device.*` | Already correct |
| P1 | Design G-buffer attachments with `eInputAttachment` | resource manager / renderer | Prerequisite images |
| P1 | Multipass draws inside one `beginRendering` | `vk_renderer.cpp` + shaders | Actual consumption |
| P2 | maintenance10 feedback flags if same-attachment RMW | `vk_renderer.cpp` | Needs local-read + m10 |
| P2 | Switch outer end to `endRendering2KHR` when adopting m10 | `vk_renderer.cpp` | See maintenance10 spec |
| P3 | Document shader local-read locations per pass graph | shaders / render graph | Maintainability |

### Implementation recipe

1. **Prerequisite is already done.** Dynamic rendering + Synchronization2 are consumed (`beginRendering` / `endRendering`, `pipelineBarrier2` around the swapchain). Keep `dynamicRenderingLocalRead = true` in `Vulkan14Features`; never list the KHR extension string.
2. **Do not touch the current forward pass.** MSAA color resolves into the swapchain; that attachment is not an input attachment. Adding feedback flags or `eInputAttachment` usage there is incorrect.
3. **First consumer = a new multipass inside one instance.** Create G-buffer images with `ColorAttachment | InputAttachment`, extend `PipelineRenderingCreateInfo` formats, draw G-buffer then lighting without `endRendering` between, using 1.4 local-read mappings / input-attachment declarations.
4. **Only then** set maintenance10 `INPUT_ATTACHMENT_FEEDBACK` / concurrent-access bits if the same attachment is written and read. Prefer `endRendering2KHR` when that path lands.
5. **Unrelated features stay off this work.** FSR (`HardwareCapabilities::fragmentShadingRate`) is enabled but unused. Advanced blend is cached with `advancedBlendCoherentOperations` **explicitly false**. ASTC HDR is **false** (GPU-gated). EDS1/2 are core 1.3; EDS3 is still an extension with limited use — none of these gate local-read.

---

## 6. Concrete integration sketch (future multipass)

```cpp
// Attachments created with ColorAttachment | InputAttachment (and depth as needed).

vk::RenderingAttachmentInfo gbuffer0 = { /* albedo view, load/store, no resolve */ };
vk::RenderingAttachmentInfo gbuffer1 = { /* normal view */ };
vk::RenderingAttachmentInfo depth    = { /* depth view */ };
std::array colors = { gbuffer0, gbuffer1 };

// When feedback is required (maintenance10), chain RenderingAttachmentFlagsInfoKHR
// with eInputAttachmentFeedback on the relevant attachments.

vk::RenderingInfo renderingInfo = {
    .flags = {}, // add local-read concurrent access control when required (m10)
    .renderArea = fullArea,
    .layerCount = 1,
    .colorAttachmentCount = 2,
    .pColorAttachments = colors.data(),
    .pDepthAttachment = &depth
};

cmd.beginRendering(renderingInfo);
// Pass A: write G-buffer
cmd.bindPipeline(..., gbufferPipeline);
// ... draws ...

// Pass B: read inputs, write lighting target (may be attachment 0 or a third color)
cmd.bindPipeline(..., lightingPipeline);
// ... full-screen draw reading input attachments ...

cmd.endRendering(); // or endRendering2KHR with maintenance10
```

Shader side must use the core 1.4 local-read / input attachment declarations that match the pipeline and attachment indices.

---

## 7. Validation / test checklist

1. Device creates with `dynamicRenderingLocalRead = true` and **without** the extension string.
2. Current forward path remains valid with the feature enabled but unused.
3. When multipass is added:
   - Images include `INPUT_ATTACHMENT` usage
   - Formats in `PipelineRenderingCreateInfo` match attachments
   - No validation errors on input attachment loads inside the rendering instance
4. If maintenance10 feedback bits are set:
   - Feature `dynamicRenderingLocalRead` remains enabled
   - Feedback only on attachments that are truly written and read as inputs
5. Performance: compare local-read deferred vs separate render-pass instances on target GPUs.

---

## 8. Shader interaction

**Direct when used — fragment stages can read current attachments via input-attachment style bindings without traditional subpasses.** Requires shader declarations matching local-read attachments/formats.

### Engine status

Enabled, not consumed. Current FS only writes color from texture sample; no local-read inputs in Slang sources.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Vulkan 1.3 `dynamicRendering` | Required baseline (**enabled**) |
| Vulkan 1.4 `dynamicRenderingLocalRead` | This feature (**enabled**, unused) |
| `VK_KHR_maintenance10` | Feedback + concurrent access control bits for local-read edge cases |
| MSAA resolve in current forward path | Orthogonal; local read is for multipass attachment reads |
| Descriptor heaps | Coexist; input attachments are not heap descriptors |
| Synchronization 2 | Barriers between draws inside the pass still follow attachment / local-read rules |

---

## 10. Non-goals / do not do

- **Do not** add `VK_KHR_dynamic_rendering_local_read` to `requiredDeviceExtension`.
- Do not add input-attachment feedback flags to the current single-pass forward renderer.
- Do not treat local read as required for simple forward rendering — it is optional infrastructure for multipass-in-one-instance techniques.
- Do not disable `dynamicRenderingLocalRead` solely because it is unused; keeping it on matches the engine’s “1.4 features forced” policy and avoids rework when multipass lands.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| How is local read enabled? | **Core Vulkan 1.4** — `Vulkan14Features::dynamicRenderingLocalRead = true` |
| Properties cached? | **N/A** (no dedicated cache field) |
| Is it used? | **No** — `beginRendering` without local-read multipass / input attachments |
| Highest-value next steps | Deferred/G-buffer multipass inside one dynamic rendering instance; then m10 feedback if needed |
| Extension string? | **Never list** in `requiredDeviceExtension` |
| Related doc | [VK_KHR_maintenance10.md](./VK_KHR_maintenance10.md) for feedback bits |
