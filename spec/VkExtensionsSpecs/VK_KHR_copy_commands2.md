# VK_KHR_copy_commands2 — Implementation Spec

**Status in this repo: PROMOTED TO CORE (Vulkan 1.3). Implicitly available on the project’s Vulkan 1.3+/1.4 device — no feature bit in `VkPhysicalDeviceVulkan13Features` for the base `*2` copy commands. NOT listed as a device extension. Engine still uses Vulkan 1.0 `copyBuffer` / `copyBufferToImage` / `blitImage` in places; `*2` is the preferred extensible form for new code and migration.**

Official sources:

- [VK_KHR_copy_commands2](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_copy_commands2.html)
- [Proposal](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_KHR_copy_commands2.adoc)
- [vkCmdCopyBuffer2](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyBuffer2.html)
- [vkCmdCopyImage2](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyImage2.html)
- [vkCmdCopyBufferToImage2](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyBufferToImage2.html)
- [vkCmdBlitImage2](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBlitImage2.html)
- [vkCmdResolveImage2](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdResolveImage2.html)

> Note: Promoted to **Vulkan 1.3 core**. **Never** add `VK_KHR_copy_commands2` to `requiredDeviceExtension`. There is **no** `Vulkan13Features` switch required for the base copy-commands2 entry points when running core 1.3+.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` | **Absent by design** (comment lists KHR_copy_commands2 under 1.3 promotions) |
| Feature bit | `Vulkan13Features` | **None required** for base `*2` commands |
| Buffer copy | `src/core/vk_resource_manager.cpp` → `copyBuffer` | Uses **1.0** `copyBuffer` + `BufferCopy` |
| Buffer→image | `vk_resource_manager.cpp`, `texture_manager.cpp` | Uses **1.0** `copyBufferToImage` |
| Blit / mipgen | `vk_resource_manager.cpp`, `texture_manager.cpp` | Uses **1.0** `blitImage` + `ImageBlit` |
| Resolve | main path | MSAA resolve inside **dynamic rendering** (not `resolveImage` / `resolveImage2`) |
| `*2` API usage | engine paths | **Not yet** as primary |

### Already available (core)

Core 1.3+ device + `VK_API_VERSION_1_4` target means `cmdCopyBuffer2`, `cmdCopyImage2`, `cmdCopyBufferToImage2`, `cmdCopyImageToBuffer2`, `cmdBlitImage2`, `cmdResolveImage2` are available without enabling a device extension.

### Not migrated yet

- All transfer helpers still call 1.0 copy/blit entry points  
- No `CopyBufferInfo2` / `BlitImageInfo2` / `ResolveImageInfo2` construction  
- Standalone `resolveImage2` + maintenance10 `ResolveImageModeInfoKHR` not used (resolve is in-render-pass)

---

## 2. What the extension provides

Copy commands 2 wrap the classic copy/blit/resolve operations in **info structs** that support `pNext` chaining (future extensions, maintenance10 resolve mode info, etc.) without new entry points for every flag.

The 1.0 copy/blit/resolve commands cannot accept `pNext`. This is **Vulkan 1.3 core always** — no `Vulkan13Features` bit, never list `VK_KHR_copy_commands2`.

It does **not** change validity of offsets or row pitches. `optimalBufferCopyOffsetAlignment` and `optimalBufferCopyRowPitchAlignment` are **performance** hints (powers of two), not validation requirements. 1.0 commands remain legal.

| Command | Info struct | Engine relevance |
| --- | --- | --- |
| `vkCmdCopyBuffer2` | `VkCopyBufferInfo2` + `VkBufferCopy2` | Staging → device vertex/index/UBO |
| `vkCmdCopyImage2` | `VkCopyImageInfo2` + `VkImageCopy2` | Image copies / depth async copies |
| `vkCmdCopyBufferToImage2` | `VkCopyBufferToImageInfo2` + `VkBufferImageCopy2` | Texture upload |
| `vkCmdCopyImageToBuffer2` | `VkCopyImageToBufferInfo2` | Readback |
| `vkCmdBlitImage2` | `VkBlitImageInfo2` + `VkImageBlit2` | Mipmap generation |
| `vkCmdResolveImage2` | `VkResolveImageInfo2` + `VkImageResolve2` | Standalone MSAA resolve; pNext for maintenance10 |

**Still valid.** Prefer `*2` for all new transfer code and when chaining extension structs. 1.0 commands remain legal but are not extensible.

---

## 3. Structures and commands

### 3.1 No feature bit (base)

Unlike synchronization2 / dynamicRendering / maintenance4, copy_commands2 does **not** add a boolean to `VkPhysicalDeviceVulkan13Features`. Presence is implied by Vulkan 1.3+ core.

### 3.2 Copy buffer 2

```cpp
typedef struct VkBufferCopy2 {
    VkStructureType    sType;
    const void*        pNext;
    VkDeviceSize       srcOffset;
    VkDeviceSize       dstOffset;
    VkDeviceSize       size;
} VkBufferCopy2;

typedef struct VkCopyBufferInfo2 {
    VkStructureType        sType;
    const void*            pNext;
    VkBuffer               srcBuffer;
    VkBuffer               dstBuffer;
    uint32_t               regionCount;
    const VkBufferCopy2*   pRegions;
} VkCopyBufferInfo2;

void vkCmdCopyBuffer2(VkCommandBuffer commandBuffer, const VkCopyBufferInfo2* pCopyBufferInfo);
```

Vulkan-Hpp:

```cpp
vk::BufferCopy2 region{.srcOffset = 0, .dstOffset = 0, .size = size};
vk::CopyBufferInfo2 info{
    .srcBuffer = *src,
    .dstBuffer = *dst,
    .regionCount = 1,
    .pRegions = &region};
cmd.copyBuffer2(info);
```

### 3.3 Copy buffer to image 2

```cpp
void vkCmdCopyBufferToImage2(
    VkCommandBuffer commandBuffer,
    const VkCopyBufferToImageInfo2* pCopyBufferToImageInfo);
```

Pattern for texture upload:

```cpp
vk::BufferImageCopy2 region{
    .bufferOffset = 0,
    .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
    .imageOffset = {0, 0, 0},
    .imageExtent = {width, height, 1}};
vk::CopyBufferToImageInfo2 info{
    .srcBuffer = *buffer,
    .dstImage = *image,
    .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
    .regionCount = 1,
    .pRegions = &region};
cmd.copyBufferToImage2(info);
```

### 3.4 Blit image 2 (mipgen)

```cpp
void vkCmdBlitImage2(VkCommandBuffer commandBuffer, const VkBlitImageInfo2* pBlitImageInfo);
```

```cpp
vk::ImageBlit2 blit{};
blit.srcSubresource = {vk::ImageAspectFlagBits::eColor, srcMip, 0, 1};
blit.srcOffsets = {vk::Offset3D{0, 0, 0}, vk::Offset3D{srcW, srcH, 1}};
blit.dstSubresource = {vk::ImageAspectFlagBits::eColor, dstMip, 0, 1};
blit.dstOffsets = {vk::Offset3D{0, 0, 0}, vk::Offset3D{dstW, dstH, 1}};

vk::BlitImageInfo2 info{
    .srcImage = *image,
    .srcImageLayout = vk::ImageLayout::eTransferSrcOptimal,
    .dstImage = *image,
    .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
    .regionCount = 1,
    .pRegions = &blit,
    .filter = vk::Filter::eLinear};
cmd.blitImage2(info);
```

### 3.5 Resolve image 2 + maintenance10

```cpp
void vkCmdResolveImage2(VkCommandBuffer commandBuffer, const VkResolveImageInfo2* pResolveImageInfo);
```

When resolving **outside** dynamic rendering, chain `VkResolveImageModeInfoKHR` (maintenance10) for resolve mode and sRGB transfer-function control. Today the engine resolves inside `beginRendering` via `resolveMode` on `RenderingAttachmentInfo`, so this is a future hook.

---

## Limits

No dedicated property struct and no new Required Limits numbers ([Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html)). Related **core** limits (performance, not validity):

| Core limit | Constraint | Role |
| --- | --- | --- |
| `optimalBufferCopyOffsetAlignment` | Power of two | Performance hint for buffer copy offsets |
| `optimalBufferCopyRowPitchAlignment` | Power of two | Performance hint for buffer row pitch |

Commands: `vkCmdCopyBuffer2`, `vkCmdCopyImage2`, `vkCmdCopyBufferToImage2`, `vkCmdCopyImageToBuffer2`, `vkCmdBlitImage2`, `vkCmdResolveImage2`.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE (implicit)

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep **out** of extension list |
| Feature chain | No bit to set for base `*2` commands |

### 4.2 Buffer upload path — MIGRATE

**File:** `src/core/vk_resource_manager.cpp` → `copyBuffer`

```cpp
// Current
transferCommandBuffer[0].copyBuffer(srcBuffer, dstBuffer, vk::BufferCopy(0, 0, size));

// Preferred
vk::BufferCopy2 region{.srcOffset = 0, .dstOffset = 0, .size = size};
vk::CopyBufferInfo2 info{
    .srcBuffer = *srcBuffer,
    .dstBuffer = *dstBuffer,
    .regionCount = 1,
    .pRegions = &region};
transferCommandBuffer[0].copyBuffer2(info);
```

Submit can stay `SubmitInfo2` (already sync2).

### 4.3 Texture upload — MIGRATE

**Files:**

- `src/core/vk_resource_manager.cpp` → `copyBufferToImage`
- `src/core/texture_manager.cpp` → `copyBufferToImage`

Switch to `copyBufferToImage2` + `BufferImageCopy2`.

### 4.4 Mipmap generation — MIGRATE

**Files:**

- `src/core/vk_resource_manager.cpp` → `generateMipmaps` (`blitImage` → `blitImage2`)
- `src/core/texture_manager.cpp` → mip loop (`blitImage` → `blitImage2`)

Pair with sync2 barriers (resource manager already sync2; texture_manager still on 1.0 barriers — migrate both together).

### 4.5 Standalone resolve — FUTURE

Only if a post path resolves MSAA outside dynamic rendering. Use `resolveImage2` + optional maintenance10 mode info. Prefer keeping resolve inside dynamic rendering for the swapchain forward path.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Do not list as device extension | `vk_device.hpp` | Already correct |
| P1 | `copyBuffer` → `copyBuffer2` | `vk_resource_manager.cpp` | Single obvious call site; extensible |
| P1 | `copyBufferToImage` → `*2` | resource + texture managers | Texture upload path |
| P2 | `blitImage` → `blitImage2` in mipgen | both mipgen paths | Consistency + pNext |
| P3 | `resolveImage2` helper | future post/resolve utility | Only if needed outside rendering |

### Implementation recipe

1. Keep the extension **out** of `requiredDeviceExtension` (1.3 core). No feature bit. VMA flags in `src/core/vk_allocator.hpp` (`EXT_MEMORY_BUDGET`, `MAINTENANCE4`, `MAINTENANCE5`) are unrelated to copy2 but stay as-is.
2. Migrate `ResourceManager::copyBuffer` in `src/core/vk_resource_manager.cpp` from `copyBuffer` + `BufferCopy` to `copyBuffer2` + `CopyBufferInfo2` / `BufferCopy2`. Submit already uses `SubmitInfo2`.
3. Migrate `copyBufferToImage` in both `vk_resource_manager.cpp` and `texture_manager.cpp` to `copyBufferToImage2` + `BufferImageCopy2`.
4. Migrate mipgen `blitImage` loops in both files to `blitImage2`. Pair texture-manager 1.0 barriers with sync2 when touching that path.
5. Leave MSAA resolve inside dynamic rendering. Add `resolveImage2` only for out-of-pass resolve.
6. Optionally align staging row pitches to `optimalBufferCopyRowPitchAlignment` for performance (not a VU).

---

## 6. Concrete integration sketch

```cpp
void ResourceManager::copyBuffer(vk::raii::Buffer& srcBuffer, vk::raii::Buffer& dstBuffer,
                                 vk::DeviceSize size)
{
    transferCommandBuffer[0].begin(
        vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    const vk::BufferCopy2 region{.srcOffset = 0, .dstOffset = 0, .size = size};
    const vk::CopyBufferInfo2 copyInfo{
        .srcBuffer = *srcBuffer,
        .dstBuffer = *dstBuffer,
        .regionCount = 1,
        .pRegions = &region};
    transferCommandBuffer[0].copyBuffer2(copyInfo);

    transferCommandBuffer[0].end();
    vk::CommandBufferSubmitInfo commandBufferInfo{.commandBuffer = *transferCommandBuffer[0]};
    const vk::SubmitInfo2 submitInfo{.commandBufferInfoCount = 1,
                                     .pCommandBufferInfos = &commandBufferInfo};
    transferQueue.submit2(submitInfo, nullptr);
    transferQueue.waitIdle();
}
```

---

## 7. Validation / test checklist

1. Device does **not** list `VK_KHR_copy_commands2`.  
2. After migration, validation accepts `copyBuffer2` / `blitImage2` without “feature not enabled”.  
3. Vertex/index upload still correct after `copyBuffer2`.  
4. Texture upload + mipgen produce identical images to 1.0 path.  
5. Layouts remain `TransferSrc`/`TransferDst` as required around blits/copies (sync2 barriers).  
6. If `resolveImage2` + maintenance10 flags: resolve mode and sRGB rules satisfied.

---

## 8. Shader interaction

**None (direct).** This extension is host / queue / memory / presentation / resource plumbing only. It does not add shader stages, builtins, storage classes, or SPIR-V capabilities. Shaders continue to use whatever programmable stages the active pipeline or shader objects declare; this feature only changes how the host prepares or submits work those shaders run on.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Vulkan 1.3 core | Promotion home; no separate feature bit for base commands |
| Synchronization2 | Barriers around copies/blits must use correct Transfer stages |
| Maintenance10 | `ResolveImageModeInfoKHR` pNext on `ResolveImageInfo2` |
| Dynamic rendering | Preferred MSAA resolve path today; reduces need for standalone resolve |
| Indirect copy (`VK_KHR_copy_memory_indirect`) | Separate device extension; not a replacement for these `*2` cmds |
| Host image copy (1.4) | Alternative for some uploads; orthogonal to GPU `*2` copies |

---

## 10. Non-goals / do not do

- Do **not** add `VK_KHR_copy_commands2` to `requiredDeviceExtension`.  
- Do **not** invent a fake feature bit in the 1.3 struct for base copy2.  
- Do **not** block on full migration before landing other work — 1.0 copies remain valid; migrate incrementally.  
- Do **not** replace in-render MSAA resolve with `resolveImage2` without a clear post-process reason.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| How enabled? | **Implicit core 1.3+** (no feature bit for base `*2` commands) |
| Device extension? | **No** (correct) |
| Consumed as `*2`? | **Not yet** — engine still uses 1.0 copy/blit entry points |
| Still valid? | **Yes** — preferred extensible transfer API |
| Highest-value next steps | Migrate `copyBuffer` / `copyBufferToImage` / mip `blitImage` to `*2` |
