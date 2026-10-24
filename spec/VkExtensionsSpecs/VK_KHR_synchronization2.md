# VK_KHR_synchronization2 — Implementation Spec

**Status in this repo: PROMOTED TO CORE (Vulkan 1.3), ENABLED via `VkPhysicalDeviceVulkan13Features::synchronization2 = true`. CONSUMED via `pipelineBarrier2` in `vk_utils.cpp` and `vk_resource_manager.cpp`, and via `SubmitInfo2` / `queue.submit2` in `vk_renderer.cpp` and `vk_resource_manager.cpp`. NOT listed as a device extension.**

Official sources:

- [VK_KHR_synchronization2](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_synchronization2.html)
- [Proposal](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_KHR_synchronization2.adoc)
- [VkPhysicalDeviceVulkan13Features](https://docs.vulkan.org/spec/latest/chapters/features.html#VkPhysicalDeviceVulkan13Features) (`synchronization2`)
- [vkCmdPipelineBarrier2](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdPipelineBarrier2.html)
- [VkDependencyInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkDependencyInfo.html)
- [vkQueueSubmit2](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit2.html)

> Note: Promoted to **Vulkan 1.3 core**. Enable only through `VkPhysicalDeviceVulkan13Features`. **Never** add `VK_KHR_synchronization2` to `requiredDeviceExtension`. Prefer `pipelineBarrier2` / `SubmitInfo2` over legacy `pipelineBarrier` / `SubmitInfo` as the primary path.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` | **Absent by design** |
| Feature enable | `src/core/vk_device.cpp` | `Vulkan13Features{ .synchronization2 = true }` |
| Image transitions | `src/util/vk_utils.cpp` → `transitionImageLayout` | **CONSUMED** — `ImageMemoryBarrier2` + `pipelineBarrier2` |
| Mipgen barriers | `src/core/vk_resource_manager.cpp` → `generateMipmaps` | **CONSUMED** — `pipelineBarrier2` |
| Frame submit | `src/render/vk_renderer.cpp` → `drawFrame` | **CONSUMED** — `SubmitInfo2` + `submit2` + `SemaphoreSubmitInfo` stage masks |
| Transfer copy submit | `src/core/vk_resource_manager.cpp` → `copyBuffer` | **CONSUMED** — `SubmitInfo2` + `submit2` |
| Legacy submit residual | `ResourceManager::endCommandBuffer` | Still uses `vk::SubmitInfo` + `queue.submit` |
| Legacy barrier residual | `src/core/texture_manager.cpp` mip path | Still uses 1.0 `pipelineBarrier` + `ImageMemoryBarrier` |

### Already wired

```cpp
// src/core/vk_device.cpp
// Vulkan13Features
{.synchronization2 = true, .dynamicRendering = true, .maintenance4 = true},

// src/util/vk_utils.cpp
vk::ImageMemoryBarrier2 barrier{ /* stage/access 2 masks, layouts, image */ };
vk::DependencyInfo dependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
commandBuffer->pipelineBarrier2(dependencyInfo);

// src/render/vk_renderer.cpp
vk::SemaphoreSubmitInfo waitSemaphoreInfo = {
    .semaphore = presentSemaphore,
    .stageMask = vk::PipelineStageFlagBits2::eTopOfPipe};
const vk::SubmitInfo2 submitInfo{ /* wait / cmd / signal */ };
graphicsQueue.submit2(submitInfo, fence);

// src/core/vk_resource_manager.cpp — generateMipmaps + copyBuffer submit2
graphicsCmd.pipelineBarrier2(depInfoToSrc);
// ...
transferQueue.submit2(submitInfo, nullptr);
```

### Not fully migrated yet

- `texture_manager.cpp` mipmap generation still on **old** `pipelineBarrier`
- `endCommandBuffer` still on **old** `SubmitInfo`
- No `MemoryBarrier2` / `BufferMemoryBarrier2` helpers beyond image barriers (add when buffer-stage precision is needed)
- No `setEvent2` / `waitEvents2` / `writeTimestamp2` usage yet

---

## 2. What the extension provides

Synchronization2 is the engine’s preferred barrier and submit model and is already consumed on the main path. Image layout transitions (`transitionImageLayout` in `vk_utils.cpp`), mipgen in `vk_resource_manager.cpp`, and frame submit in `vk_renderer.cpp` use `pipelineBarrier2` / `SubmitInfo2`. Remaining work is migrating leftover 1.0 call sites, not a new enablement.

Dynamic rendering depends on these 64-bit stage/access masks: the swapchain is transitioned to `eColorAttachmentOptimal` before `beginRendering` and to `ePresentSrcKHR` after `endRendering` (MSAA color resolves into the swapchain inside that instance). New passes (local-read, FSR attachments, extra color targets) must use the same `*2` types.

Synchronization2 unifies pipeline stages and access masks into 64-bit flag types, embeds stages **on each barrier**, and extends queue submit with per-semaphore stage masks.

| Feature | API | Engine relevance |
| --- | --- | --- |
| Dependency batch | `VkDependencyInfo` | Single structure for memory / buffer / image barriers |
| Image barrier 2 | `VkImageMemoryBarrier2` | **Primary** image layout transitions |
| Buffer barrier 2 | `VkBufferMemoryBarrier2` | Buffer ownership / access (future multi-queue precision) |
| Global memory barrier 2 | `VkMemoryBarrier2` | Coarse full-memory deps |
| Pipeline barrier 2 | `vkCmdPipelineBarrier2` | **Preferred** over `vkCmdPipelineBarrier` |
| Submit 2 | `vkQueueSubmit2` + `VkSubmitInfo2` | **Preferred** graphics/transfer submits |
| Semaphore submit info | `VkSemaphoreSubmitInfo` (stage mask per wait/signal) | Frame wait/signal in `vk_renderer.cpp` |
| Command buffer submit info | `VkCommandBufferSubmitInfo` | Used with SubmitInfo2 |
| Events / timestamps 2 | `setEvent2`, `waitEvents2`, `writeTimestamp2` | Profiling / fine sync (Tracy path separate) |

**Still valid.** Prefer sync2 for all new code. Migrate remaining 1.0 barrier/submit call sites.

---

## 3. Structures and commands

### 3.1 Feature (core 1.3)

```cpp
// VkPhysicalDeviceVulkan13Features
VkBool32 synchronization2;
```

**Already enabled** as `true`. Keep forced on.

### 3.2 DependencyInfo + PipelineBarrier2

```cpp
typedef struct VkDependencyInfo {
    VkStructureType                     sType;
    const void*                         pNext;
    VkDependencyFlags                   dependencyFlags;
    uint32_t                            memoryBarrierCount;
    const VkMemoryBarrier2*             pMemoryBarriers;
    uint32_t                            bufferMemoryBarrierCount;
    const VkBufferMemoryBarrier2*       pBufferMemoryBarriers;
    uint32_t                            imageMemoryBarrierCount;
    const VkImageMemoryBarrier2*        pImageMemoryBarriers;
} VkDependencyInfo;

void vkCmdPipelineBarrier2(VkCommandBuffer commandBuffer, const VkDependencyInfo* pDependencyInfo);
```

### 3.3 ImageMemoryBarrier2 (primary pattern in this engine)

```cpp
typedef struct VkImageMemoryBarrier2 {
    VkStructureType            sType;
    const void*                pNext;
    VkPipelineStageFlags2      srcStageMask;
    VkAccessFlags2             srcAccessMask;
    VkPipelineStageFlags2      dstStageMask;
    VkAccessFlags2             dstAccessMask;
    VkImageLayout              oldLayout;
    VkImageLayout              newLayout;
    uint32_t                   srcQueueFamilyIndex;
    uint32_t                   dstQueueFamilyIndex;
    VkImage                    image;
    VkImageSubresourceRange    subresourceRange;
} VkImageMemoryBarrier2;
```

Key difference vs 1.0: **stage masks live on the barrier**, not as separate `pipelineBarrier` arguments. Access masks are `VkAccessFlags2`.

Common mappings already encoded in `transitionImageLayout`:

| Transition | src stage / access | dst stage / access |
| --- | --- | --- |
| Undefined → TransferDst | TopOfPipe / None | Transfer / TransferWrite |
| TransferDst → ShaderReadOnly | Transfer / TransferWrite | FragmentShader / ShaderRead |
| Undefined → DepthStencilAttachment | TopOfPipe / None | EarlyFragmentTests / DS read|write |
| Undefined → ColorAttachment | TopOfPipe / None | ColorAttachmentOutput / ColorAttachmentWrite |
| ColorAttachment → Present | ColorAttachmentOutput / ColorWrite | BottomOfPipe / None (via overrides in renderer) |

### 3.4 SubmitInfo2

```cpp
typedef struct VkSubmitInfo2 {
    VkStructureType                     sType;
    const void*                         pNext;
    VkSubmitFlags                       flags;
    uint32_t                            waitSemaphoreInfoCount;
    const VkSemaphoreSubmitInfo*        pWaitSemaphoreInfos;
    uint32_t                            commandBufferInfoCount;
    const VkCommandBufferSubmitInfo*    pCommandBufferInfos;
    uint32_t                            signalSemaphoreInfoCount;
    const VkSemaphoreSubmitInfo*        pSignalSemaphoreInfos;
} VkSubmitInfo2;

void vkQueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence);
```

Vulkan-Hpp (as used):

```cpp
graphicsQueue.submit2(submitInfo, fence);
```

---

## Limits

No numeric Required Limits for `Vulkan13Features::synchronization2`. The change is width and placement of masks, not new counts. Do not invent numeric minima.

| Topic | Fact |
| --- | --- |
| Dedicated property struct | None |
| Stage / access types | `VkPipelineStageFlags2` / `VkAccessFlags2` are **64-bit** |
| Replaces | 32-bit `VkPipelineStageFlags` / `VkAccessFlags` |
| Extra bits | Acceleration-structure build, copy, host, and other stages/accesses that did not fit in 32 bits |

Stay inside existing core stage/queue capabilities. Official: [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html).

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

Keep `synchronization2 = true` in `Vulkan13Features`. Do not list the KHR extension.

### 4.2 Shared image transition helper — DONE

**File:** `src/util/vk_utils.cpp` → `transitionImageLayout`

This is the **canonical** sync2 layout helper. Renderer and resource paths should call it (or copy its barrier pattern) instead of open-coding 1.0 barriers.

Optional improvements:

- Expand default transition table (e.g. `ShaderReadOnly` → `TransferSrc`, general → present).
- Support buffer barriers in a sibling helper when multi-queue buffer ownership is refined.

### 4.3 Resource manager mipgen — DONE (sync2)

**File:** `src/core/vk_resource_manager.cpp` → `generateMipmaps`

Uses `ImageMemoryBarrier2` + `pipelineBarrier2` correctly between blits.

### 4.4 Texture manager mipgen — MIGRATE

**File:** `src/core/texture_manager.cpp`

Still uses:

```cpp
commandBuffer.pipelineBarrier(/* 1.0 stages */, ..., barrier /* ImageMemoryBarrier */);
```

**Action:** rewrite to the same `ImageMemoryBarrier2` + `DependencyInfo` + `pipelineBarrier2` pattern as `ResourceManager::generateMipmaps` / `transitionImageLayout`. Prefer calling the shared helper where layouts match.

### 4.5 Queue submits

| Site | Status | Action |
| --- | --- | --- |
| `vk_renderer.cpp` `drawFrame` | SubmitInfo2 | Keep |
| `vk_resource_manager.cpp` `copyBuffer` | SubmitInfo2 | Keep |
| `vk_resource_manager.cpp` `endCommandBuffer` | SubmitInfo v1 | Migrate to SubmitInfo2 + `submit2` |

Sketch for `endCommandBuffer`:

```cpp
vk::CommandBufferSubmitInfo cbInfo{.commandBuffer = *commandBuffer};
vk::SubmitInfo2 submitInfo{.commandBufferInfoCount = 1, .pCommandBufferInfos = &cbInfo};
queue.submit2(submitInfo, nullptr);
queue.waitIdle();
```

### 4.6 New code policy

Any new barrier or submit **must** use sync2 types (`*2` / `Flags2` / `SubmitInfo2`) unless a third-party API forces 1.0 (then isolate and document).

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep `synchronization2 = true` | `vk_device.cpp` | Required for current main path |
| P1 | Migrate `texture_manager.cpp` barriers to `pipelineBarrier2` | `texture_manager.cpp` | Eliminate last major 1.0 barrier consumer |
| P1 | Migrate `endCommandBuffer` to `submit2` | `vk_resource_manager.cpp` | Uniform submit path |
| P2 | Expand `transitionImageLayout` cases | `vk_utils.cpp` | Fewer one-off barriers |
| P3 | BufferMemoryBarrier2 for QFOT | multi-queue upload paths | When ownership transfers need precision |

### Implementation recipe

1. **Keep the consumed main path.** `synchronization2 = true` in `Vulkan13Features`. `vk_renderer.cpp` already pairs dynamic rendering with sync2: `transitionImageLayout` → `beginRendering` (MSAA color resolve to swapchain) → `endRendering` → present barrier → `submit2`.
2. **Migrate residuals (P1).** Rewrite `texture_manager.cpp` mip barriers to `ImageMemoryBarrier2` + `pipelineBarrier2`. Convert `ResourceManager::endCommandBuffer` from `SubmitInfo` to `SubmitInfo2` / `submit2`.
3. **New code policy.** Any new barrier or submit uses `*2` types. Do not mix 1.0 stage arguments with `AccessFlags2`.
4. **Future consumers stay sync2.** When local-read (`dynamicRenderingLocalRead` is on, unused), FSR (`HardwareCapabilities::fragmentShadingRate`, unused), or extra attachments appear, their layout transitions go through the same helper. Advanced blend coherent ops stay **explicitly false**; ASTC HDR stays **false** — neither adds sync primitives.
5. **EDS is orthogonal.** EDS1/2 are core 1.3 dynamic state; EDS3 is still an extension with limited use. They do not change barrier types.

---

## 6. Concrete integration sketch (legacy → sync2)

```cpp
// BEFORE (texture_manager-style 1.0)
vk::ImageMemoryBarrier barrier{
    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
    .dstAccessMask = vk::AccessFlagBits::eTransferRead,
    .oldLayout = vk::ImageLayout::eTransferDstOptimal,
    .newLayout = vk::ImageLayout::eTransferSrcOptimal,
    .image = image,
    .subresourceRange = range};
commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                              vk::PipelineStageFlagBits::eTransfer, {},
                              nullptr, nullptr, barrier);

// AFTER (preferred)
vk::ImageMemoryBarrier2 barrier2{
    .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
    .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
    .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
    .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    .oldLayout = vk::ImageLayout::eTransferDstOptimal,
    .newLayout = vk::ImageLayout::eTransferSrcOptimal,
    .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
    .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
    .image = image,
    .subresourceRange = range};
vk::DependencyInfo dep{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier2};
commandBuffer.pipelineBarrier2(dep);
```

---

## 7. Validation / test checklist

1. Device creates with `synchronization2 = true` without KHR extension string.  
2. Main frame: no validation errors on `submit2` wait/signal stage masks.  
3. `transitionImageLayout` covers all transitions used by the renderer.  
4. After texture_manager migration: zero `pipelineBarrier(` (1.0) left in engine paths (except intentional third-party).  
5. Multi-queue: queue family indices not left as ignored when a real QFOT is required.  
6. Access masks use `AccessFlagBits2` (including `eNone` where appropriate).  
7. Dynamic rendering color/depth transitions match attachment layouts at `beginRendering`.

---

## 8. Shader interaction

**Indirect — barriers and stage masks must name the correct shader stages.** Use `VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT`, `FRAGMENT_SHADER`, `MESH_SHADER_BIT_EXT`, `TASK_SHADER_BIT_EXT`, `RAY_TRACING_SHADER_BIT_KHR`, `COMPUTE_SHADER`, plus access masks for shader read/write and heap read bits when applicable. Wrong stage masks cause stale samples or hazards. No SPIR-V changes.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Dynamic rendering | Layout transitions around `beginRendering`/`endRendering` |
| Copy commands / blits | Transfer stage/access pairs between copies and shader reads |
| Maintenance9 QFOT relaxations | May reduce required buffer/linear ownership transfers when that path is consumed |
| Maintenance10 depth copy-on-queue | Transfer/compute copies still need correct stage masks |
| Tracy / timestamps | Prefer timestamp2 APIs if adopting sync2 query writes |
| Legacy 1.0 barrier API | Obsolete as primary path; migrate residuals |

---

## 10. Non-goals / do not do

- Do **not** add `VK_KHR_synchronization2` to `requiredDeviceExtension`.  
- Do **not** mix stage masks on 1.0 `pipelineBarrier` with `AccessFlags2` types.  
- Do **not** use `VK_PIPELINE_STAGE_ALL_COMMANDS_BIT` as a permanent substitute for precise stages in hot paths.  
- Do **not** signal/wait binary semaphores in SubmitInfo2 without correct `stageMask`.  
- Do **not** reintroduce 1.0 barriers in new code.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| How enabled? | **Core 1.3** — `Vulkan13Features::synchronization2 = true` |
| Consumed? | **Yes** — `pipelineBarrier2` (utils, resource manager), `submit2` (renderer, copyBuffer) |
| Residuals? | `texture_manager.cpp` 1.0 barriers; `endCommandBuffer` 1.0 submit |
| Device extension? | **No** (correct) |
| Still valid? | **Yes** — preferred synchronization and submit model |
| Highest-value next steps | Finish migration of texture_manager + endCommandBuffer to sync2 |
