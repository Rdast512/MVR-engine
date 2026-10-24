# Host Image Copy — Vulkan 1.4 Core Feature

**Status in this repo: VULKAN 1.4 CORE (optional feature). ENABLED via `VkPhysicalDeviceVulkan14Features::hostImageCopy`. Layout lists cached. CONSUMED for STB base-level uploads (`texture_manager.cpp`); KTX still GPU.**

Historical extension name: `VK_EXT_host_image_copy`. Promoted into Vulkan 1.4 with the `EXT` suffix omitted. EXT type/enum/command names remain aliases of the core API. This is **not** a device extension on this target.

Official sources:

- [Vulkan 1.4](https://docs.vulkan.org/refpages/latest/refpages/source/VK_VERSION_1_4.html) (promotion + optional-support rule)
- [VkPhysicalDeviceVulkan14Features](https://docs.vulkan.org/spec/latest/chapters/features.html#VkPhysicalDeviceVulkan14Features) (`hostImageCopy`)
- [VkPhysicalDeviceHostImageCopyProperties](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceHostImageCopyProperties.html)
- [vkCopyMemoryToImage](https://docs.vulkan.org/refpages/latest/refpages/source/vkCopyMemoryToImage.html) / [vkCopyImageToMemory](https://docs.vulkan.org/refpages/latest/refpages/source/vkCopyImageToMemory.html) / [vkCopyImageToImage](https://docs.vulkan.org/refpages/latest/refpages/source/vkCopyImageToImage.html)
- [vkTransitionImageLayout](https://docs.vulkan.org/refpages/latest/refpages/source/vkTransitionImageLayout.html)
- [Host image copies (spec)](https://docs.vulkan.org/spec/latest/chapters/copies.html#copies-host-image-copies)
- [Proposal (historical EXT)](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_host_image_copy.html)
- [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html)

> **Enablement:** `Vulkan14Features::hostImageCopy = true` only.  
> **Do not** list `VK_EXT_host_image_copy` in `requiredDeviceExtension`.  
> **Prefer** core / unprefixed names (`vkCopyMemoryToImage`, `vk::CopyMemoryToImageInfo`, `eHostTransfer`). EXT aliases are valid but not preferred in new code.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | **Must not be listed** — 1.4 promotion note already says so |
| Feature enable | `src/core/vk_device.cpp` → `Vulkan14Features` | `hostImageCopy = true` |
| Dedicated property query | `src/core/vk_device.cpp` → properties chain | `vk::PhysicalDeviceHostImageCopyPropertiesEXT` (core alias) |
| Folded 1.4 properties | same chain → `Vulkan14Properties` | Same fields exist on `capabilities.vulkan14` (`copySrcLayoutCount`, UUID, `identicalMemoryTypeRequirements`) |
| Capability cache | `src/core/types.hpp` → `HardwareCapabilities::hostImageCopy` | Cached (type still `*EXT`) |
| Layout list fill | `src/core/vk_device.cpp` + `HardwareCapabilities` vectors | **Filled** — two-step query, logged at device init |
| Format / dest layout | `texture_manager.cpp` → `stbHostCopyDstLayout` | **Required** — missing `HOST_IMAGE_TRANSFER` / dest layouts throws |
| API usage | `texture_manager.cpp` STB path | **Consumed** — host copy only; no staging fallback |
| Host layout transition | `device.transitionImageLayout` in STB path | **Consumed** — `UNDEFINED` → queried dest layout |

### Already wired

```cpp
// src/core/vk_device.cpp — vk::PhysicalDeviceVulkan14Features
{.globalPriorityQuery = true,
 .dynamicRenderingLocalRead = true,
 .maintenance5 = true,
 .maintenance6 = true,
 .hostImageCopy = true},

// src/core/vk_device.cpp (properties)
capabilities.hostImageCopy =
    propertiesChain.get<vk::PhysicalDeviceHostImageCopyPropertiesEXT>();

// src/core/types.hpp
vk::PhysicalDeviceHostImageCopyPropertiesEXT hostImageCopy;
```

### Not wired yet

- `vkCopyImageToMemory` / `vkCopyImageToImage` (readback / host image-to-image)
- KTX / multi-mip host upload
- Prefer core property type name in `types.hpp` (still `*EXT` alias)

---

## 2. What host image copy provides

Host image copy is **core Vulkan 1.4**, not a device extension. It lets the CPU copy between host pointers and images, copy image-to-image, and change image layouts, **without recording or submitting a command buffer**. The destination (or source) image must be created with `VK_IMAGE_USAGE_HOST_TRANSFER_BIT`.

It exists to drop the staging-buffer hop (`host → VkBuffer → vkCmdCopyBufferToImage`). That hop doubles peak memory during mass asset load and forces the streaming path to allocate Vulkan buffers.

**Optional in 1.4.** Unlike most 1.4 promotions, `hostImageCopy` is **not** required on every 1.4 implementation. From [VK_VERSION_1_4](https://docs.vulkan.org/refpages/latest/refpages/source/VK_VERSION_1_4.html):

- If `VK_EXT_host_image_copy` is not advertised, support is optional in 1.4.
- A 1.4 implementation with a `VK_QUEUE_GRAPHICS_BIT` queue **must** support **either** `hostImageCopy` **or** an additional queue that supports `VK_QUEUE_TRANSFER_BIT`.
- Every graphics/compute queue must also advertise `VK_QUEUE_TRANSFER_BIT`.

This engine already forces `hostImageCopy = true` at device create, so a 1.4 device without the feature will fail create. STB uploads have **no** staging fallback: missing dest layouts or `HOST_IMAGE_TRANSFER` throws.

| Feature | Core API | Engine relevance |
| --- | --- | --- |
| Memory → image | `vkCopyMemoryToImage` | **STB upload path** |
| Image → memory | `vkCopyImageToMemory` | Readback without a transfer-queue round trip |
| Image → image (host) | `vkCopyImageToImage` | Host-side image copies when both images allow it |
| Host layout transition | `vkTransitionImageLayout` | `UNDEFINED` → host-copyable dest layout with no command buffer |
| Copyable layouts | properties / `Vulkan14Properties` | Legal `srcImageLayout` / `dstImageLayout` values — **must query** |
| Identical memory types | `identicalMemoryTypeRequirements` | Whether `HOST_TRANSFER` changes the image’s memory-type mask |
| Device-perf query | `VkHostImageCopyDevicePerformanceQuery` | Whether `HOST_TRANSFER` hurts sampled-image device access |
| Format feature | `VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT` | Which formats accept host copies |

**Not a free win everywhere.** Optimal images are often swizzled. The CPU must undo that swizzle, so a host copy can be slower than `memcpy` + GPU `copyBufferToImage`, and `HOST_TRANSFER` **may** force a simpler (less compressed) device layout. Measure. The proposal’s baseline: do **not** host-copy every texture unless a platform recommends it.

Host copy does **not**:

- Replace GPU mip generation (unless every mip is uploaded from the host)
- Make every optimal image host-copyable
- Relax tiling / format / usage rules
- Require or replace VMA allocation

---

## 3. Structures and commands

All of the following are **`VK_VERSION_1_4`**. EXT names are aliases.

### 3.1 Feature — `VkPhysicalDeviceVulkan14Features`

```c
// inside VkPhysicalDeviceVulkan14Features
VkBool32 hostImageCopy;
```

Standalone `VkPhysicalDeviceHostImageCopyFeatures` exists but is redundant here: the 1.4 feature struct already carries the bit. **Already enabled.**

Do not chain `VkPhysicalDeviceHostImageCopyFeatures` a second time.

### 3.2 Properties

Query via `VkPhysicalDeviceHostImageCopyProperties` **or** the same fields on `VkPhysicalDeviceVulkan14Properties`. This repo caches the dedicated struct (currently the `*EXT` alias).

```c
// Provided by VK_VERSION_1_4
typedef struct VkPhysicalDeviceHostImageCopyProperties {
    VkStructureType    sType;
    void*              pNext;
    uint32_t           copySrcLayoutCount;
    VkImageLayout*     pCopySrcLayouts;
    uint32_t           copyDstLayoutCount;
    VkImageLayout*     pCopyDstLayouts;
    uint8_t            optimalTilingLayoutUUID[VK_UUID_SIZE];
    VkBool32           identicalMemoryTypeRequirements;
} VkPhysicalDeviceHostImageCopyProperties;
```

| Field | Meaning |
| --- | --- |
| `copySrcLayoutCount` / `pCopySrcLayouts` | Legal layouts for `vkCopyImageToMemory` / `vkCopyImageToImage` **source** |
| `copyDstLayoutCount` / `pCopyDstLayouts` | Legal layouts for `vkCopyMemoryToImage` / `vkCopyImageToImage` **destination** |
| `optimalTilingLayoutUUID` | UUID of the implementation’s optimal-tiling swizzle; compare when using `VK_HOST_IMAGE_COPY_MEMCPY_BIT` |
| `identicalMemoryTypeRequirements` | `VK_TRUE` ⇒ adding `HOST_TRANSFER` does **not** change the image’s memory-type requirements |

Two-step query (same pattern as most counted Vulkan lists):

1. Call with `pCopySrcLayouts = pCopyDstLayouts = NULL` → fill counts.
2. Allocate arrays, set counts, query again → fill layouts.

The implementation **must** include `VK_IMAGE_LAYOUT_GENERAL` in **both** lists. If `unifiedImageLayouts` is supported, every layout interchangeable with `GENERAL` is included as well. Do **not** assume `TRANSFER_DST_OPTIMAL` / `SHADER_READ_ONLY_OPTIMAL` are present — they often are, but only the queried list is valid.

The current `propertiesChain.get<...>()` stores counts/UUID/flags only. Fill the layout arrays with an explicit follow-up before gating uploads.

### 3.3 Host copy commands

```c
// Provided by VK_VERSION_1_4
VkResult vkCopyMemoryToImage(VkDevice device, const VkCopyMemoryToImageInfo* pCopyMemoryToImageInfo);
VkResult vkCopyImageToMemory(VkDevice device, const VkCopyImageToMemoryInfo*  pCopyImageToMemoryInfo);
VkResult vkCopyImageToImage(VkDevice device, const VkCopyImageToImageInfo*   pCopyImageToImageInfo);
VkResult vkTransitionImageLayout(VkDevice device, uint32_t transitionCount,
                                 const VkHostImageLayoutTransitionInfo* pTransitions);
```

Vulkan-Hpp (core names):

```cpp
device.copyMemoryToImage(copyInfo);
device.copyImageToMemory(copyInfo);
device.copyImageToImage(copyInfo);
device.transitionImageLayout(transitions); // HOST command — not vk_utils::transitionImageLayout
```

Host copies are functionally the `vkCmdCopyBufferToImage2` / `vkCmdCopyImageToBuffer2` / `vkCmdCopyImage2` counterparts, executed on the host, reading/writing a host pointer instead of a `VkBuffer`. Image memory is accessed as if **coherent**.

Name collision: `src/util/vk_utils.cpp` already exports `transitionImageLayout(...)` as a GPU `pipelineBarrier2` helper. Keep that name. Call the host entry point as `device.transitionImageLayout(...)`.

### 3.4 Info / region structs

```c
typedef struct VkCopyMemoryToImageInfo {
    VkStructureType               sType;
    const void*                   pNext;
    VkHostImageCopyFlags          flags;
    VkImage                       dstImage;
    VkImageLayout                 dstImageLayout;   // must be in pCopyDstLayouts
    uint32_t                      regionCount;
    const VkMemoryToImageCopy*    pRegions;
} VkCopyMemoryToImageInfo;

typedef struct VkMemoryToImageCopy {
    VkStructureType             sType;
    const void*                 pNext;
    const void*                 pHostPointer;
    uint32_t                    memoryRowLength;    // 0 = tightly packed
    uint32_t                    memoryImageHeight;  // 0 = tightly packed
    VkImageSubresourceLayers    imageSubresource;
    VkOffset3D                  imageOffset;
    VkExtent3D                  imageExtent;
} VkMemoryToImageCopy;
```

`VkCopyImageToMemoryInfo` / `VkImageToMemoryCopy` are the inverse (`void* pHostPointer` is the destination). `VkCopyImageToImageInfo` reuses `VkImageCopy2` regions.

`dstImageLayout` / `srcImageLayout` **must** be a layout returned in the matching property list.

### 3.5 Host layout transition

```c
typedef struct VkHostImageLayoutTransitionInfo {
    VkStructureType            sType;
    const void*                pNext;
    VkImage                    image;
    VkImageLayout              oldLayout;
    VkImageLayout              newLayout;
    VkImageSubresourceRange    subresourceRange;
} VkHostImageLayoutTransitionInfo;
```

Allowed `oldLayout` (proposal + spec):

- `VK_IMAGE_LAYOUT_UNDEFINED`
- `VK_IMAGE_LAYOUT_PREINITIALIZED`
- any layout in `pCopySrcLayouts`

Allowed `newLayout`:

- any layout in `pCopyDstLayouts` (always includes `GENERAL`)

Host transitions do **not** perform queue-family ownership transfers: the physical layout of a host-transfer image does not vary between families for these layouts.

### 3.6 Usage, format feature, flags

| Symbol | Role |
| --- | --- |
| `VK_IMAGE_USAGE_HOST_TRANSFER_BIT` | Required on any image used by host copy or host transition. Vulkan-Hpp: `eHostTransfer`. EXT alias: `*_HOST_TRANSFER_BIT_EXT`. |
| `VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT` | Format supports host copies. Required for all sampleable color formats except DRM modifiers / Android hardware buffers. |
| `VK_HOST_IMAGE_COPY_MEMCPY_BIT` | Skip CPU swizzle: host bytes already match the device tiling. `memoryRowLength` and `memoryImageHeight` **must** be 0. Size via `VkSubresourceHostMemcpySize` chained to `vkGetImageSubresourceLayout2`. Compare `optimalTilingLayoutUUID` across devices/caches. |

Default path for this engine: **do not** set `MEMCPY`. STB pixels are tightly packed linear RGBA, so the implementation must swizzle into optimal tiling.

### 3.7 Device-performance query

Chain `VkHostImageCopyDevicePerformanceQuery` onto `vkGetPhysicalDeviceImageFormatProperties2`:

| Field | Meaning |
| --- | --- |
| `optimalDeviceAccess` | `HOST_TRANSFER` does not degrade later device access vs an image created without it |
| `identicalMemoryLayout` | Physical layout is the same (or close enough that perf is equivalent) |

Block-compressed formats **must** report `optimalDeviceAccess = VK_TRUE`. Uncompressed sampled textures may not. This repo always host-copies STB images; the query is logged so a `false` `optimalDeviceAccess` can be measured against upload savings.

Proposal issue 3.1: `HOST_TRANSFER` makes `GENERAL` as efficient as `OPTIMAL` for that image. Host transitions therefore use **`GENERAL` only** — do not host-transition to `TRANSFER_DST_OPTIMAL` / `SHADER_READ_ONLY_OPTIMAL`. GPU mipgen still uses a GPU `GENERAL` → `TRANSFER_DST` barrier, then `generateMipmaps` to `SHADER_READ_ONLY_OPTIMAL` for the descriptor write.

### 3.8 Synchronization (spec)

These commands do **not** wait for in-flight GPU work. The application **must** guarantee the subresource is idle.

| Direction | Extra barrier |
| --- | --- |
| Host → image (`vkCopyMemoryToImage`) or host layout transition, then use on device | None. Queue submission already makes host writes visible to the device. |
| Device wrote the image, then host reads it (`vkCopyImageToMemory`) or host-transitions it | Device barrier with `VK_PIPELINE_STAGE_HOST_BIT` + `VK_ACCESS_HOST_READ_BIT` **must** have completed first. |

For a fresh STB upload (`UNDEFINED` → host copy → sample), no GPU barrier is required around the host copy itself.

---

## 4. Limits

Properties live in `HardwareCapabilities::hostImageCopy` (`VkPhysicalDeviceHostImageCopyPropertiesEXT` today) and are duplicated on `HardwareCapabilities::vulkan14`. Feature: `hostImageCopy` in `Vulkan14Features` (already `true`). Official: [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html), [VK_VERSION_1_4](https://docs.vulkan.org/refpages/latest/refpages/source/VK_VERSION_1_4.html).

| Field / rule | Constraint |
| --- | --- |
| `hostImageCopy` feature | **Optional** in Vulkan 1.4. Required only when the impl has a graphics queue and **no** extra transfer-only queue. This repo requires the feature at device create. |
| `copySrcLayoutCount` / `pCopySrcLayouts` | Implementation-dependent list. **Must query.** `GENERAL` is always present. |
| `copyDstLayoutCount` / `pCopyDstLayouts` | Same. `GENERAL` is always present. |
| `identicalMemoryTypeRequirements` | Implementation-dependent. If `VK_FALSE`, re-query memory types when adding `HOST_TRANSFER`. |
| `optimalTilingLayoutUUID` | Compare before reusing `MEMCPY` dumps across devices/drivers. |
| Host pointer alignment | **None** (proposal issue 2, resolved). Do not apply `optimalBufferCopyOffsetAlignment` as a VU. |
| Offset / extent granularity | 1 pixel (uncompressed) / 1 texel block (compressed). Same edge-of-image rule as GPU copies. |
| `memoryRowLength` / `memoryImageHeight` | Same packing as `VkBufferImageCopy2`. Both 0 if `MEMCPY` is set. |
| Format feature | Sampleable color formats must support `HOST_IMAGE_TRANSFER` (except external modifiers / AHB). Still verify the format you actually create. |
| `optimalDeviceAccess` | Required `VK_TRUE` for block-compressed formats; otherwise query. |
| `maxImageDimension2D` (1.4) | 8192 — host copies still cannot exceed the image’s create extent / this limit. |

Related 1.4 limits (`maxImageDimension*`, `bufferImageGranularity`, …) apply to the image object itself, not uniquely to host copy.

---

## 5. Where to implement in this codebase

### 5.1 Device bootstrap — DONE (keep)

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep host image copy **out** of `requiredDeviceExtension` |
| `src/core/vk_device.cpp` | Keep `hostImageCopy = true` + property query |
| `src/core/types.hpp` | Keep `HardwareCapabilities::hostImageCopy` |

### 5.2 Property completion — next

**File:** `src/core/vk_device.cpp`

After the existing chain get, two-step query the layout arrays and log:

- `copySrcLayoutCount` / `copyDstLayoutCount`
- whether `GENERAL` is present (must be)
- `identicalMemoryTypeRequirements`
- `optimalTilingLayoutUUID`

Store the filled arrays somewhere the texture path can test (`std::vector<vk::ImageLayout>` on `HardwareCapabilities`, or a small helper). The raw property struct cannot own the arrays if they are stack temporaries.

### 5.3 Texture upload — primary consumer

**File:** `src/core/texture_manager.cpp` (STB path, ~lines 198–282)

Current STB path (host copy only):

1. Require `HOST_IMAGE_TRANSFER` on `R8G8B8A8_SRGB` and `GENERAL` in dest layouts; log `HostImageCopyDevicePerformanceQuery`
2. `stbi_load` → host pixels
3. Optimal image: `eHostTransfer | eTransferSrc | eTransferDst | eSampled`
4. Host `transitionImageLayout` `eUndefined` → `eGeneral`
5. `copyMemoryToImage` from the decoded pointer, then `stbi_image_free`
6. GPU `GENERAL` → `TRANSFER_DST`, then `generateMipmaps` to `SHADER_READ_ONLY_OPTIMAL`

**KTX path:** still the GPU loader (`ktxVulkanTexture_*`).

### 5.4 Decision heuristic

None. STB always host-copies. Missing format feature or dest layouts throw.

### 5.5 Image create usage

```cpp
usage |= vk::ImageUsageFlagBits::eHostTransfer; // VK_IMAGE_USAGE_HOST_TRANSFER_BIT
```

If `identicalMemoryTypeRequirements` is false, do not reuse a memory-type decision computed without `eHostTransfer`.

### 5.6 Readback / tools

Future capture / thumbnail / CPU processing can use `copyImageToMemory` after a `HOST_READ` barrier. Not needed for the first consumer.

---

## 6. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep feature + properties; never list the extension | `vk_device.*`, `types.hpp` | Already correct |
| P1 | Two-step layout-list query + log counts / UUID / identical-mem | `vk_device.cpp` | Cannot pick a dest layout without this |
| P1 | STB base-level host upload (required; no staging) | `texture_manager.cpp` | **Done** |
| P2 | Host layout-transition helper (do not overload `vk_utils::transitionImageLayout`) | new small helper or `texture_manager` | Shared by loaders |
| P3 | KTX / multi-mip host upload | texture loaders | Only if profiling justifies |
| P3 | Prefer core property type `vk::PhysicalDeviceHostImageCopyProperties` in `types.hpp` | `types.hpp` | Alias cleanup |

### Implementation recipe

1. Keep `hostImageCopy = true` and the `HardwareCapabilities::hostImageCopy` cache. Never list `VK_EXT_host_image_copy`. VMA is unchanged (`vk_allocator.hpp` already has budget / maintenance4 / maintenance5).
2. Two-step query `pCopySrcLayouts` / `pCopyDstLayouts`. Log counts and `identicalMemoryTypeRequirements`. Treat `eGeneral` as the guaranteed dest layout.
3. STB always host-copies: `eHostTransfer`, host-transition `eUndefined` → queried dest layout, `copyMemoryToImage`, then GPU `generateMipmaps`.
4. Do not host-copy swapchain images or transient attachments.
5. Do not apply buffer-copy alignment VUs to `pHostPointer`. Tightly packed (`memoryRowLength = memoryImageHeight = 0`) is correct for STB RGBA8.

---

## 7. Integration sketch (STB base level)

```cpp
const auto& hic = deviceWrapper.getCapabilities().hostImageCopy;
// 1) dest layout must be in the queried pCopyDstLayouts (GENERAL is always legal).
// 2) create image with eHostTransfer | eSampled | eTransferSrc (GPU mips).

const vk::ImageLayout hostDstLayout = vk::ImageLayout::eGeneral;

vk::HostImageLayoutTransitionInfo hostTransition{
    .image            = *asset.textureImage,
    .oldLayout        = vk::ImageLayout::eUndefined,
    .newLayout        = hostDstLayout,
    .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
};
device.transitionImageLayout({hostTransition});

vk::MemoryToImageCopy region{
    .pHostPointer     = pixels,
    .memoryRowLength  = 0,
    .memoryImageHeight = 0,
    .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
    .imageOffset      = {0, 0, 0},
    .imageExtent      = {static_cast<uint32_t>(texWidth),
                         static_cast<uint32_t>(texHeight), 1},
};
vk::CopyMemoryToImageInfo copyInfo{
    .flags          = {},
    .dstImage       = *asset.textureImage,
    .dstImageLayout = hostDstLayout,
    .regionCount    = 1,
    .pRegions       = &region,
};
device.copyMemoryToImage(copyInfo);
stbi_image_free(const_cast<stbi_uc*>(pixels));

// Then existing generateMipmaps (GPU) and/or a host/GPU transition of remaining
// mips to the sampled layout. Do not call vk_utils::transitionImageLayout here
// unless you are recording a command buffer.
```

Exact Vulkan-Hpp field order must match the SDK in this repo (1.4.357). Treat the sketch as structural.

---

## 8. Validation / test checklist

1. Device creates with `hostImageCopy = true` and **without** `VK_EXT_host_image_copy` in `requiredDeviceExtension`.
2. Two-step property query: counts match filled arrays; both lists contain `GENERAL`.
3. Host upload image has `eHostTransfer`; `dstImageLayout` is from `pCopyDstLayouts`.
4. No command buffer is required for the host copy + host transition sequence.
5. Visual check of STB textures after GPU mipgen.
6. Missing `HOST_IMAGE_TRANSFER` or dest layouts throws (no staging path).
7. `pHostPointer` stays valid for the duration of `copyMemoryToImage` (free STB pixels **after** the call).
8. No use of host copy on swapchain or transient images.
9. If `identicalMemoryTypeRequirements` is false, images with `eHostTransfer` still bind successfully via VMA.
10. Validation layers: no VU on layout, usage, or in-flight access.

---

## 9. Shader interaction

**None.** Host copies write image memory. After the image is in a sampled layout (or `GENERAL` when that is the sampled layout), fragment/mesh shaders bind the existing descriptor-heap sampled-image handle. No new SPIR-V capability, builtin, or sampler op.

---

## 10. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Vulkan 1.4 | Promotion host. Feature lives in `Vulkan14Features`; properties also fold into `Vulkan14Properties`. |
| `maintenance5` | `vkGetImageSubresourceLayout2` / `vkGetDeviceImageSubresourceLayout` + `VkSubresourceHostMemcpySize` for `MEMCPY` size. Not needed for the default (swizzled) STB path. |
| Transfer / graphics single-time commands | Used for GPU mipgen after host base-level copy |
| VMA | Still allocates image memory. Host copy does not replace allocation and does not require a host-visible memory type for the image itself (the driver maps internally). |
| Mipmap generation | Remains GPU unless every mip is host-uploaded. Keep `eTransferSrc` when GPU blit-mipgen follows a host base-level upload. |
| `vk_utils::transitionImageLayout` | GPU barrier helper. Do not reuse for host transitions. |
| Synchronization2 | Only needed when the GPU has already written the image and the host then reads it. |

---

## 11. Non-goals / do not do

- **Do not** add `VK_EXT_host_image_copy` to `requiredDeviceExtension`.
- Do not reintroduce a staging + `copyBufferToImage` STB path.
- Do not assume `TRANSFER_*` / `SHADER_READ_ONLY_OPTIMAL` are host-copyable without checking the queried lists.
- Do not assume every optimal image stays as fast to sample after adding `eHostTransfer` — query `optimalDeviceAccess`.
- Do not host-copy swapchain images or transient attachments.
- Do not treat `hostImageCopy` as a required 1.4 bit in portable code outside this engine’s device-create policy.
- Do not use `VK_HOST_IMAGE_COPY_MEMCPY_BIT` for STB/linear pixels.

---

## 12. Summary

| Question | Answer |
| --- | --- |
| What is this? | **Vulkan 1.4 core feature**, historically `VK_EXT_host_image_copy` |
| How is it enabled? | `Vulkan14Features::hostImageCopy = true` only |
| Optional in 1.4? | **Yes** — required only if there is a graphics queue and no extra transfer queue |
| Properties cached? | **Yes** — counts, UUID, flags, and layout arrays |
| Is it used? | **Yes** — STB always host-copies; KTX still GPU |
| Highest-value next steps | KTX host path only if profiling justifies; core property type alias cleanup |
| Extension string? | **Never list** in `requiredDeviceExtension` |
