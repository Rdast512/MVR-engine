# VK_KHR_maintenance10 — Implementation Spec

**Status in this repo: ENABLED (device extension + feature + properties cached), NOT CONSUMED — `endRendering()` not `endRendering2KHR`; no sRGB resolve transfer-function control; no property log. No input-attachment feedback / standalone `resolveImage2` / async depth-copy gates.**

Official sources:

- [VK_KHR_maintenance10](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_maintenance10.html)
- [Proposal](https://github.com/khronosgroup/vulkan-docs/blob/main/proposals/VK_KHR_maintenance10.adoc)
- [VkPhysicalDeviceMaintenance10FeaturesKHR](https://docs.vulkan.org/spec/latest/chapters/features.html)
- [VkPhysicalDeviceMaintenance10PropertiesKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMaintenance10PropertiesKHR.html)

> Note: The extension is **KHR**, not EXT. There is no `VK_EXT_maintenance10`.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::KHRMaintenance10ExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → `createLogicalDevice()` feature chain | `vk::PhysicalDeviceMaintenance10FeaturesKHR{ .maintenance10 = true }` |
| Property query | `src/core/vk_device.cpp` → properties chain | `vk::PhysicalDeviceMaintenance10PropertiesKHR` queried |
| Capability cache | `src/core/types.hpp` → `HardwareCapabilities::maintenance10` | Cached |
| API usage | renderer / resource / texture paths | **None** — `cmd.endRendering()`; resolve is `eAverage` with no transfer-function bit. Properties cached, not logged. |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::KHRMaintenance10ExtensionName,

// src/core/vk_device.cpp (feature chain)
// vk::PhysicalDeviceMaintenance10FeaturesKHR
{.maintenance10 = true},

// src/core/vk_device.cpp (properties chain)
capabilities.maintenance10 =
    propertiesChain.get<vk::PhysicalDeviceMaintenance10PropertiesKHR>();
// cached only — no init log

// src/render/vk_renderer.cpp
cmd.endRendering();
// color resolve: eAverage, no eResolveEnableTransferFunction

// src/core/types.hpp
vk::PhysicalDeviceMaintenance10PropertiesKHR maintenance10;
```

### Not wired (no existing call site — do not invent)

- `endRendering2KHR` / `RenderingEndInfoKHR`
- `eResolveEnableTransferFunction` on MSAA sRGB resolve
- Init log of `rgba4OpaqueBlackSwizzled` / `resolveSrgb*`
- Input-attachment feedback / `eLocalReadConcurrentAccessControlKHR`
- `VkResolveImageModeInfoKHR` (no standalone `resolveImage2`)
- Depth/stencil copy-on-transfer/compute format gates (no async depth copies)
- NULL sample-mask behavior (no `setSampleMaskEXT`)

---

## 2. What the extension provides

Maintenance10 is a **cumulative maintenance bag**: small, independent rendering/resolve/copy fixes. None alone justifies a dedicated extension. It is still a **device extension** in this repo (`vk::KHRMaintenance10ExtensionName` + `Maintenance10FeaturesKHR`) — not promoted into the 1.4 core structs this project uses the way maintenance5/6 are. It is **not a replacement** for earlier maintenance (4–9 limits, Flags2, QFOT, etc.); keep those caches and enablement.

| Feature | New API / bit | Engine relevance |
| --- | --- | --- |
| Extensible end-rendering | `vkCmdEndRendering2KHR` + `VkRenderingEndInfoKHR` | Replace `endRendering()` in dynamic rendering |
| Input-attachment feedback under dynamic rendering local-read | `VK_RENDERING_ATTACHMENT_INPUT_ATTACHMENT_FEEDBACK_BIT_KHR` + `VK_RENDERING_LOCAL_READ_CONCURRENT_ACCESS_CONTROL_BIT_KHR` | Needed when reading an attachment as input while writing it |
| sRGB resolve default query | `resolveSrgbFormatAppliesTransferFunction` | Know whether MSAA resolve is linear or nonlinear |
| sRGB resolve override | `resolveSrgbFormatSupportsTransferFunctionControl` + resolve ENABLE/SKIP transfer-function flags | Force correct lighting when resolving MSAA sRGB targets |
| Standalone resolve parity | `VkResolveImageModeInfoKHR` on `vkCmdResolveImage2` | Explicit resolve mode + depth/stencil resolve outside render passes |
| Depth/stencil copies on non-graphics queues | `VK_FORMAT_FEATURE_2_DEPTH/STENCIL_COPY_ON_{COMPUTE,TRANSFER}_QUEUE_BIT_KHR` | Safe async depth copies on transfer/compute |
| NULL sample mask = all bits set | behavior change for `vkCmdSetSampleMaskEXT` | Only if using extended dynamic state 3 / shader objects |
| sRGB vertex-input encoding clarified | spec clarification | Vertex formats that are sRGB-encoded |
| RGBA4 opaque-black swizzle query | `rgba4OpaqueBlackSwizzled` | Only if using packed RGBA4 formats |

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
// Provided by VK_KHR_maintenance10
typedef struct VkPhysicalDeviceMaintenance10FeaturesKHR {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_10_FEATURES_KHR
    void*           pNext;
    VkBool32        maintenance10; // master switch for the extension feature set
} VkPhysicalDeviceMaintenance10FeaturesKHR;
```

**Already enabled** in this project. Keep `maintenance10 = true` whenever the extension name is listed.

### 3.2 Properties (query after physical device pick)

```cpp
typedef struct VkPhysicalDeviceMaintenance10PropertiesKHR {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_10_PROPERTIES_KHR
    void*           pNext;
    VkBool32        rgba4OpaqueBlackSwizzled;
    VkBool32        resolveSrgbFormatAppliesTransferFunction;          // default resolve space
    VkBool32        resolveSrgbFormatSupportsTransferFunctionControl;  // can override default
} VkPhysicalDeviceMaintenance10PropertiesKHR;
```

| Property | Meaning |
| --- | --- |
| `rgba4OpaqueBlackSwizzled` | Implementation swizzle behavior for opaque black in RGBA4-class formats |
| `resolveSrgbFormatAppliesTransferFunction` | `VK_TRUE` = resolve in **linear** space by default; `VK_FALSE` = nonlinear (legacy GL-like) |
| `resolveSrgbFormatSupportsTransferFunctionControl` | App may force ENABLE/SKIP transfer function on resolves |

**Already cached** as `HardwareCapabilities::maintenance10`.

### 3.3 New command

```cpp
void vkCmdEndRendering2KHR(
    VkCommandBuffer                 commandBuffer,
    const VkRenderingEndInfoKHR*    pRenderingEndInfo); // may be NULL
```

Vulkan-Hpp:

```cpp
commandBuffer.endRendering2KHR(vk::RenderingEndInfoKHR{ /* optional pNext chain */ });
// or
commandBuffer.endRendering2KHR(nullptr);
```

Valid usage highlights:

- Must pair with a prior `vkCmdBeginRendering`
- Graphics queue only
- Inside a render-pass instance
- Not between suspended render-pass instances

### 3.4 Attachment flags (pNext of `VkRenderingAttachmentInfo`)

```cpp
typedef struct VkRenderingAttachmentFlagsInfoKHR {
    VkStructureType                 sType; // VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_FLAGS_INFO_KHR
    void*                           pNext;
    VkRenderingAttachmentFlagsKHR   flags;
} VkRenderingAttachmentFlagsInfoKHR;
```

Important flag bits:

| Flag | Use |
| --- | --- |
| `VK_RENDERING_ATTACHMENT_INPUT_ATTACHMENT_FEEDBACK_BIT_KHR` | Attachment is also used as input attachment feedback (requires `dynamicRenderingLocalRead`) |
| `VK_RENDERING_ATTACHMENT_RESOLVE_ENABLE_TRANSFER_FUNCTION_BIT_KHR` | Force sRGB transfer function during resolve (linearize) |
| `VK_RENDERING_ATTACHMENT_RESOLVE_SKIP_TRANSFER_FUNCTION_BIT_KHR` | Force resolve in encoded/nonlinear space |

Rules:

- ENABLE and SKIP are mutually exclusive
- Either requires `resolveSrgbFormatSupportsTransferFunctionControl == VK_TRUE`
- Image view must be sRGB-encoded and `resolveMode == AVERAGE` when using transfer-function flags
- Feedback bit requires `dynamicRenderingLocalRead` (already enabled in this repo via Vulkan 1.4 features)

Related begin-rendering flag (when local-read concurrent access control is needed):

- `VK_RENDERING_LOCAL_READ_CONCURRENT_ACCESS_CONTROL_BIT_KHR` on `VkRenderingInfo::flags`

### 3.5 Standalone resolve mode info (pNext of `VkResolveImageInfo2`)

```cpp
typedef struct VkResolveImageModeInfoKHR {
    VkStructureType            sType; // VK_STRUCTURE_TYPE_RESOLVE_IMAGE_MODE_INFO_KHR
    void*                      pNext;
    VkResolveImageFlagsKHR     flags;
    VkResolveModeFlagBits      resolveMode;
    VkResolveModeFlagBits      stencilResolveMode;
} VkResolveImageModeInfoKHR;
```

Also supports:

- `VK_RESOLVE_IMAGE_ENABLE_TRANSFER_FUNCTION_BIT_KHR`
- `VK_RESOLVE_IMAGE_SKIP_TRANSFER_FUNCTION_BIT_KHR`

### 3.6 Format feature bits (with format feature flags 2 / Vulkan 1.3+)

| Bit | Meaning |
| --- | --- |
| `VK_FORMAT_FEATURE_2_DEPTH_COPY_ON_COMPUTE_QUEUE_BIT_KHR` | Depth aspect copy allowed on compute queues |
| `VK_FORMAT_FEATURE_2_DEPTH_COPY_ON_TRANSFER_QUEUE_BIT_KHR` | Depth aspect copy allowed on transfer queues |
| `VK_FORMAT_FEATURE_2_STENCIL_COPY_ON_COMPUTE_QUEUE_BIT_KHR` | Stencil aspect copy allowed on compute queues |
| `VK_FORMAT_FEATURE_2_STENCIL_COPY_ON_TRANSFER_QUEUE_BIT_KHR` | Stencil aspect copy allowed on transfer queues |

Query via `vkGetPhysicalDeviceFormatProperties2` + `VkFormatProperties3`.

---

## Limits

See [Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html) and [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html#limits-required).

`VkPhysicalDeviceMaintenance10PropertiesKHR` (cached `HardwareCapabilities::maintenance10`) are **bool behavior queries**, not numeric minima:

| Field | Kind |
| --- | --- |
| `rgba4OpaqueBlackSwizzled` | Bool — RGBA4 opaque-black swizzle |
| `resolveSrgbFormatAppliesTransferFunction` | Bool — default sRGB resolve space |
| `resolveSrgbFormatSupportsTransferFunctionControl` | Bool — can override default |

Depth/stencil copy-on-queue bits (`VK_FORMAT_FEATURE_2_DEPTH/STENCIL_COPY_ON_{COMPUTE,TRANSFER}_QUEUE_BIT_KHR`) are **per-format**, queried via `FormatProperties3` — not device-wide Required Limits.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep `vk::KHRMaintenance10ExtensionName` in `requiredDeviceExtension` |
| `src/core/vk_device.cpp` | Keep feature + property chain entries |
| `src/core/types.hpp` | Keep `HardwareCapabilities::maintenance10` |

Optional hardening (recommended, not required for compile):

1. Log the three maintenance10 properties at device init (next to other capability dumps).
2. Assert/feature-gate: if extension is required, fail device creation when `maintenance10` feature is missing (driver reject already covers this when forced `true`).

### 4.2 Primary consumer — dynamic rendering path

**File:** `src/render/vk_renderer.cpp`  
**Function:** frame record path that currently does:

```cpp
commandBuffers[currentFrame].beginRendering(renderingInfo);
// ... draws ...
commandBuffers[currentFrame].endRendering();
```

#### Step A — switch to extensible end

```cpp
// Prefer endRendering2 once maintenance10 is guaranteed (it is, as a required ext).
commandBuffers[currentFrame].endRendering2KHR(vk::RenderingEndInfoKHR{});
```

Use `RenderingEndInfoKHR` when a future pNext is needed; until then empty/default is fine and keeps the call future-proof versus `endRendering()`.

#### Step B — MSAA sRGB resolve control (high value for this engine)

Current color attachment already uses MSAA resolve into the swapchain image:

```cpp
vk::RenderingAttachmentInfo colorAttachmentInfo = {
    .imageView = resourceManager.colorImageView,
    .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
    .resolveMode = vk::ResolveModeFlagBits::eAverage,
    .resolveImageView = swapChain.swapChainImageViews[imageIndex],
    .resolveImageLayout = vk::ImageLayout::eColorAttachmentOptimal,
    // ...
};
```

Implement:

1. Detect whether the **resolve** format is sRGB (swapchain surface format / view format).
2. Read:

```cpp
const auto& m10 = device.getCapabilities().maintenance10;
```

3. If resolve target is sRGB **and** `m10.resolveSrgbFormatSupportsTransferFunctionControl`:

```cpp
vk::RenderingAttachmentFlagsInfoKHR colorFlags{
    .flags = vk::RenderingAttachmentFlagBitsKHR::eResolveEnableTransferFunction
    // or eResolveSkipTransferFunction — pick based on desired lighting space
};

colorAttachmentInfo.pNext = &colorFlags;
```

**Recommended default for PBR/lighting correctness:**  
`eResolveEnableTransferFunction` (resolve in linear) when control is available, especially if `resolveSrgbFormatAppliesTransferFunction == VK_FALSE`.

If control is **not** supported, do not set the flags; branch on `resolveSrgbFormatAppliesTransferFunction` for any CPU-side color-management assumptions / documentation only.

#### Step C — input-attachment feedback (only when local-read feedback is added)

This engine already enables:

- `dynamicRendering = true` (Vulkan 1.3 features)
- `dynamicRenderingLocalRead = true` (Vulkan 1.4 features)

When a pass writes an attachment and reads it as an input attachment in the same dynamic-rendering instance:

1. Create the image with `vk::ImageUsageFlagBits::eInputAttachment` (and color/depth usage as needed).
2. On that attachment’s `RenderingAttachmentInfo::pNext`:

```cpp
vk::RenderingAttachmentFlagsInfoKHR feedbackFlags{
    .flags = vk::RenderingAttachmentFlagBitsKHR::eInputAttachmentFeedback
};
```

3. On `RenderingInfo::flags`, add:

```cpp
vk::RenderingFlagBits::eLocalReadConcurrentAccessControlKHR
```

when concurrent access control is required by the access pattern.

**Where:** same `vk_renderer.cpp` rendering setup, or a future multi-pass renderer helper once local-read G-buffer/feedback paths exist.  
**Do not add feedback flags** to the current forward MSAA pass — it does not use input attachments.

### 4.3 Standalone resolve path

**Files to touch when/if non-renderpass resolves are added:**

- `src/core/vk_resource_manager.cpp` (blit/copy helpers)
- any future post-process resolve utility under `src/render/` or `src/postprocessing/`

Pattern:

```cpp
vk::ResolveImageModeInfoKHR modeInfo{
    .flags = {}, // or ENABLE/SKIP transfer function bits
    .resolveMode = vk::ResolveModeFlagBits::eAverage,
    .stencilResolveMode = vk::ResolveModeFlagBits::eNone
};

vk::ResolveImageInfo2 resolveInfo{
    .pNext = &modeInfo,
    .srcImage = src,
    .srcImageLayout = vk::ImageLayout::eTransferSrcOptimal,
    .dstImage = dst,
    .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
    .regionCount = 1,
    .pRegions = &region
};

cmd.resolveImage2(resolveInfo);
```

Today the engine resolves inside dynamic rendering, so this is a **future** hook, not an immediate change.

### 4.4 Depth/stencil copies on transfer or compute queues

**Files:**

- `src/core/vk_resource_manager.cpp` — transfer submissions, format feature checks
- `src/core/texture_manager.cpp` — transfer-queue upload path
- `src/core/vk_device.hpp` — exposes `getTransferQueue()` / `getComputeQueue()`

Before issuing depth/stencil `copyImage` / `copyImage2` on non-graphics queues:

```cpp
// Query once per depth format (cache on HardwareCapabilities or ResourceManager)
auto props = physicalDevice.getFormatProperties2<
    vk::FormatProperties2,
    vk::FormatProperties3>(depthFormat);

const auto features2 =
    props.get<vk::FormatProperties3>().optimalTilingFeatures;

const bool canCopyDepthOnTransfer =
    static_cast<bool>(features2 & vk::FormatFeatureFlagBits2::eDepthCopyOnTransferQueueKHR);
const bool canCopyStencilOnTransfer =
    static_cast<bool>(features2 & vk::FormatFeatureFlagBits2::eStencilCopyOnTransferQueueKHR);
```

Gate:

- transfer-queue depth copies on `eDepthCopyOnTransferQueueKHR`
- compute-queue depth copies on `eDepthCopyOnComputeQueueKHR`
- same pattern for stencil bits

Fallback: keep depth/stencil copies on the graphics queue (current safe default).

### 4.5 Sample mask NULL behavior

Only relevant if/when the engine records:

```cpp
cmd.setSampleMaskEXT(...);
```

with a null mask under `VK_EXT_extended_dynamic_state3` or `VK_EXT_shader_object`.  
With maintenance10 enabled, `pSampleMask == NULL` means all bits set. No code change required unless a custom sample-mask path is added in `src/render/vk_pipeline.cpp` / renderer dynamic state setup.

### 4.6 ImGui dynamic rendering

**File:** `src/core/vk_engine.cpp` (ImGui `UseDynamicRendering` + `PipelineRenderingCreateInfo`)

No maintenance10-specific change required for ImGui. It participates inside the same `beginRendering`/`endRendering` scope in `vk_renderer.cpp`; switching the outer end call to `endRendering2KHR` covers it.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep enablement as-is | `vk_device.*`, `types.hpp` | Already correct |
| P1 | Replace `endRendering()` → `endRendering2KHR()` | `src/render/vk_renderer.cpp` | **Done** |
| P1 | sRGB MSAA resolve transfer-function control gated on properties | `src/render/vk_renderer.cpp` | **Done** (`eResolveEnableTransferFunction` when sRGB + control) |
| P2 | Cache/log maintenance10 properties at init | `src/core/vk_device.cpp` | **Done** |
| P2 | Format-feature gates for depth/stencil copies on transfer/compute | `vk_resource_manager.cpp`, `texture_manager.cpp` | Prevents invalid async copies |
| P3 | Input-attachment feedback flags | renderer multi-pass path | Only when local-read feedback is implemented |
| P3 | `ResolveImageModeInfoKHR` on standalone resolves | future resolve helper | Only when `resolveImage2` is used outside render passes |

### Implementation recipe

1. Confirm `src/core/types.hpp` → `HardwareCapabilities` caches `maintenance3`, `maintenance4`, `maintenance5`, `maintenance6`, `maintenance7`, `maintenance9`, and `maintenance10`. There is **no `maintenance8` properties field**.
2. Confirm `src/core/vk_device.cpp` property chain queries `PhysicalDeviceMaintenance10PropertiesKHR` and the feature chain sets `Maintenance10FeaturesKHR{ .maintenance10 = true }`. Keep `vk::KHRMaintenance10ExtensionName` in `requiredDeviceExtension`.
3. Confirm `src/core/vk_allocator.hpp` already sets `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT` and `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT` (no VMA bit for maintenance10).
4. Switch `vk_renderer.cpp` `endRendering()` → `endRendering2KHR`; gate sRGB MSAA resolve transfer-function flags on the two resolve bools.
5. Before async depth/stencil copies, query `FormatProperties3` copy-on-queue bits; fall back to the graphics queue when a bit is missing.

---

## 6. Concrete integration sketch (renderer)

Minimal, surgical shape for `vk_renderer.cpp` (illustrative):

```cpp
const auto& m10 = /* device capabilities */.maintenance10;

vk::RenderingAttachmentFlagsInfoKHR colorAttachFlags{};
bool useColorFlags = false;

if (isSwapchainFormatSrgb && m10.resolveSrgbFormatSupportsTransferFunctionControl) {
    // Prefer linear resolve for lighting-correct MSAA.
    colorAttachFlags.flags =
        vk::RenderingAttachmentFlagBitsKHR::eResolveEnableTransferFunction;
    useColorFlags = true;
}

vk::RenderingAttachmentInfo colorAttachmentInfo = {
    .pNext = useColorFlags ? &colorAttachFlags : nullptr,
    .imageView = resourceManager.colorImageView,
    .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
    .resolveMode = vk::ResolveModeFlagBits::eAverage,
    .resolveImageView = swapChain.swapChainImageViews[imageIndex],
    .resolveImageLayout = vk::ImageLayout::eColorAttachmentOptimal,
    .loadOp = vk::AttachmentLoadOp::eClear,
    .storeOp = vk::AttachmentStoreOp::eStore,
    .clearValue = clearColor
};

// ... beginRendering + draws + ImGui ...

commandBuffers[currentFrame].endRendering2KHR(vk::RenderingEndInfoKHR{});
```

Helper placement suggestion (only if reused):

- small free function or method near swapchain format queries, e.g. on `SwapChain` or `Renderer`:
  - `bool isSrgbFormat(vk::Format)`
  - `vk::RenderingAttachmentFlagsInfoKHR makeSrgbResolveFlags(const HardwareCapabilities&)`

Do **not** invent a large abstraction for a single call site.

---

## 7. Validation / test checklist

1. Device creates successfully with `VK_KHR_maintenance10` + `maintenance10 = true` (already true on supporting drivers).
2. Capability dump shows:
   - `rgba4OpaqueBlackSwizzled`
   - `resolveSrgbFormatAppliesTransferFunction`
   - `resolveSrgbFormatSupportsTransferFunctionControl`
3. Frame path: no validation errors after `endRendering2KHR`.
4. If resolve transfer-function flags are set:
   - resolve image view format is sRGB
   - `resolveMode == AVERAGE`
   - control property is `VK_TRUE`
   - ENABLE and SKIP not combined
5. MSAA visual check: compare edges/brightness on sRGB swapchain with ENABLE vs default.
6. If depth copy on transfer is attempted: format feature bit present; otherwise graphics-queue fallback.
7. Feedback bit path (when added): image has `INPUT_ATTACHMENT` usage; `dynamicRenderingLocalRead` enabled.

---

## 8. Shader interaction

**Indirect only.** No new shader stages or builtins. Shaders are unaffected in source form; this feature changes host-side pipeline/render-pass/resource/sync setup that must stay compatible with the stages already bound (vertex/mesh/fragment/compute/RT). Ensure push constants / heap binds / attachments still match the active shader I/O when using this path.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Vulkan 1.3 `dynamicRendering` | Required baseline for begin/end rendering path (enabled) |
| Vulkan 1.4 `dynamicRenderingLocalRead` | Required for input-attachment feedback bit (enabled) |
| Vulkan 1.3 / `VK_KHR_copy_commands2` | `resolveImage2` + resolve mode info |
| Vulkan 1.3 / `VK_KHR_format_feature_flags2` | depth/stencil copy-on-queue feature bits |
| `VK_EXT_extended_dynamic_state3` / `VK_EXT_shader_object` | NULL sample-mask clarification |
| Current MSAA resolve in `vk_renderer.cpp` | Main practical consumer of sRGB resolve control |
| Transfer/compute queues in `Device` | Consumers of depth/stencil copy feature bits |

Maintenance10 is **not** promoted into core in the versions this project targets the way maintenance5/6 are; keep listing the device extension explicitly (as done today).

---

## 10. Non-goals / do not do

- Do not add a second enable path or runtime optional toggle unless device support must become optional.
- Do not set resolve transfer-function flags on non-sRGB views.
- Do not set input-attachment feedback flags on the current forward pass.
- Do not remove graphics-queue depth copies until format features are confirmed on target GPUs.
- Do not document/implement this as `VK_EXT_maintenance10` — the shipped extension is **`VK_KHR_maintenance10`**.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is Maintenance10 enabled? | **Yes** — `VK_KHR_maintenance10` is in the required device extension list, feature bit forced on, properties cached |
| Is it used? | **No** — `endRendering()`; no resolve TF; properties cached, not logged |
| Highest-value next steps | None until local-read feedback, standalone `resolveImage2`, or async depth copies exist |
| Secondary steps | depth/stencil copy feature gates on transfer/compute; local-read feedback flags when that rendering mode exists |
