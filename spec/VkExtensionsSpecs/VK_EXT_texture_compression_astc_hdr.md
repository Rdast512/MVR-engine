# VK_EXT_texture_compression_astc_hdr — Implementation Spec

**Status in this repo: PROMOTED TO CORE (Vulkan 1.3) as optional format feature `VkPhysicalDeviceVulkan13Features::textureCompressionASTC_HDR`. Currently forced `false` (comment: RTX 2060 Super not supported). NOT listed as a device extension. Still valid optional feature — enable only when the GPU supports it; do not force true on all targets.**

Official sources:

- [VK_EXT_texture_compression_astc_hdr](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_texture_compression_astc_hdr.html)
- [VkPhysicalDeviceVulkan13Features](https://docs.vulkan.org/spec/latest/chapters/features.html#VkPhysicalDeviceVulkan13Features) (`textureCompressionASTC_HDR`)
- ASTC HDR format enumerants under [VkFormat](https://docs.vulkan.org/refpages/latest/refpages/source/VkFormat.html) (`VK_FORMAT_ASTC_*_SFLOAT_BLOCK`)

> Note: Promoted to **Vulkan 1.3 core** as a **feature bit**, not an always-on requirement. **Never** add `VK_EXT_texture_compression_astc_hdr` to `requiredDeviceExtension`. Gate asset paths on the feature and format support.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` | **Absent by design** (comment lists under 1.3 promotions) |
| Feature enable | `src/core/vk_device.cpp` | `textureCompressionASTC_HDR = false` // rtx 2060 super not supported |
| Format / asset usage | texture loaders | No ASTC HDR path required today |
| Runtime query before enable | feature chain | Forced false rather than queried-then-set |

### Already wired

```cpp
// src/core/vk_device.cpp — Vulkan13Features
{.synchronization2 = true,
 .textureCompressionASTC_HDR = false, // rtx 2060 super not supported
 .dynamicRendering = true,
 .maintenance4 = true},
```

### Not wired yet

- Query physical device for `textureCompressionASTC_HDR` support and enable only if present  
- Optional asset pipeline for `ASTC_*_SFLOAT_BLOCK` formats  
- Fallback chain: ASTC HDR → ASTC LDR / BC / uncompressed  

---

## 2. What the feature provides

ASTC HDR is a **Vulkan 1.3 optional format feature**, not an always-on baseline. This repo forces `textureCompressionASTC_HDR = false` because the current target GPU class (RTX 2060 Super) does not support it. Forcing `true` would fail device creation. The bit does **not** imply LDR ASTC (`textureCompressionASTC_LDR` / core format features).

The renderer already uses dynamic rendering + Synchronization2 and does not care about compressed format families until a loader selects one. There is no ASTC HDR asset path today.

Enables **ASTC floating-point (HDR)** compressed texture formats for sampling/filtering when supported by the implementation.

| Item | Detail |
| --- | --- |
| Feature bit | `VkPhysicalDeviceVulkan13Features::textureCompressionASTC_HDR` |
| Formats | `VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK`, `5x4`, `5x5`, … block sizes (SFLOAT variants) |
| Use case | HDR compressed albedo/emissive/lightmaps on mobile/tile GPUs that support ASTC HDR |
| Desktop reality | Many NVIDIA consumer GPUs (including RTX 20-series class) **lack** this feature — matches current `false` default |

**Still valid optional format feature.** Not a required engine baseline on the current primary GPU target.

---

## 3. Structures and commands

### 3.1 Feature (core 1.3)

```cpp
// VkPhysicalDeviceVulkan13Features
VkBool32 textureCompressionASTC_HDR;
```

| Value | Meaning |
| --- | --- |
| `VK_TRUE` | Device supports ASTC HDR compressed formats (subject to format feature queries) |
| `VK_FALSE` | Do not create images with ASTC HDR formats |

No additional commands — uses standard image create / sampler / descriptor paths with ASTC HDR formats.

### 3.2 Enablement gate (recommended pattern)

```cpp
// Query support before forcing the bit on
auto features = physicalDevice.getFeatures2<
    vk::PhysicalDeviceFeatures2,
    vk::PhysicalDeviceVulkan13Features>();

const bool astcHdr =
    features.get<vk::PhysicalDeviceVulkan13Features>().textureCompressionASTC_HDR == vk::True;

// In create chain:
// .textureCompressionASTC_HDR = astcHdr  // never hard-require true on all SKUs
```

Also verify per-format:

```cpp
auto props = physicalDevice.getFormatProperties2(vk::Format::eAstc4x4SfloatBlock);
const bool canSample =
    static_cast<bool>(props.formatProperties.optimalTilingFeatures &
                      vk::FormatFeatureFlagBits::eSampledImage);
```

### 3.3 No device extension string

On 1.3+, do not enable `VK_EXT_texture_compression_astc_hdr` as an extension name; the feature bit is sufficient when true.

---

## Limits

No extra numeric size limits beyond ordinary image/format support. Do not invent block-count minima. Feature `textureCompressionASTC_HDR` is currently **false** (GPU-gated).

| Topic | Fact | Engine note |
| --- | --- | --- |
| Feature | `Vulkan13Features::textureCompressionASTC_HDR` | Forced **false** (RTX 2060 Super class) |
| Dedicated Required Limits | None | Query format features instead |
| Format features | `vkGetPhysicalDeviceFormatProperties2` for each `ASTC_*_SFLOAT_BLOCK` before create | Sampled / filter / blit bits as needed |
| LDR ASTC | Separate (`textureCompressionASTC_LDR` / core format features) | This bit does **not** enable LDR |

Official: [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html).

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — intentional false

**File:** `src/core/vk_device.cpp`

Current choice is correct for a required-true-or-fail feature chain: forcing `true` would **reject device creation** on unsupported GPUs (e.g. RTX 2060 Super).

Options going forward:

| Strategy | When |
| --- | --- |
| Keep `false` always | No ASTC HDR assets planned |
| Query + set if supported | Optional HDR compressed textures; never hard-require |
| Dual asset packs | Ship BC/ASTC LDR/uncompressed fallbacks when feature is false |

### 4.2 Capability cache (optional)

**File:** `src/core/types.hpp` / property dumps

Cache a bool `textureCompressionASTC_HDR` on `HardwareCapabilities` (from the enabled feature or a pre-create query) so texture loading can branch without re-querying.

### 4.3 Texture loading

**Files:** `src/core/texture_manager.cpp`, asset import tools

Only select ASTC HDR formats when:

1. Feature enabled on the device, and  
2. Format features include sampled (and filter if needed), and  
3. Asset exists in that encoding  

Otherwise load BC7 / ASTC LDR / RGBA16F / RGBA8 as appropriate for the platform.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep `false` unless assets need HDR ASTC | `vk_device.cpp` | Avoid device create failure on current GPUs |
| P1 | Optional query-and-enable without requiring | `vk_device.cpp` | Opt-in on supporting hardware |
| P2 | Cache capability for loaders | `types.hpp` | Clean branching |
| P3 | Asset fallback matrix | texture pipeline | Production packaging |

### Implementation recipe

1. **Keep `false` on the current device-create chain** unless the product ships ASTC HDR assets and queries support first. Never add `VK_EXT_texture_compression_astc_hdr` to `requiredDeviceExtension`.
2. **Optional later: query-then-enable.** Read `Vulkan13Features::textureCompressionASTC_HDR` from the physical device and set the create-chain bit only if true. Cache a bool for loaders.
3. **Loaders must double-gate.** Feature bit **and** per-format features. Fallback to BC / ASTC LDR / uncompressed when false.
4. **No renderer change for this feature.** `vk_renderer.cpp` `beginRendering` / MSAA resolve into the swapchain / `endRendering` and sync2 barriers stay as they are. Local-read is enabled unused; FSR (`HardwareCapabilities::fragmentShadingRate`) is enabled unused; advanced blend properties are cached with `advancedBlendCoherentOperations` **explicitly false**; EDS1/2 are core 1.3 and EDS3 is still an extension with limited use — none of these interact with ASTC HDR.

---

## 6. Concrete integration sketch (optional enable)

```cpp
// Pre-create query
const auto queried = physicalDevice.getFeatures2<
    vk::PhysicalDeviceFeatures2,
    vk::PhysicalDeviceVulkan13Features>();
const bool supportAstcHdr =
    queried.get<vk::PhysicalDeviceVulkan13Features>().textureCompressionASTC_HDR == vk::True;

// Device create chain — optional, never "required true"
vk::PhysicalDeviceVulkan13Features vulkan13{
    .synchronization2 = true,
    .textureCompressionASTC_HDR = supportAstcHdr, // false on RTX 2060 Super class
    .dynamicRendering = true,
    .maintenance4 = true};
```

Do **not** put ASTC HDR in a “must support or abort” list unless the product exclusively targets hardware that has it.

---

## 7. Validation / test checklist

1. Device creates with `textureCompressionASTC_HDR = false` on unsupported GPUs.  
2. Forcing `true` on unsupported GPU fails device creation (expected).  
3. If enabled: create sampled image with `eAstc4x4SfloatBlock` (or chosen block size) validates.  
4. Loader never picks ASTC HDR formats when feature is false.  
5. No `VK_EXT_texture_compression_astc_hdr` extension string in the device list.

---

## 8. Shader interaction

**Indirect — format support for sampled images.** When ASTC HDR formats are used, fragment/mesh shaders sample them like any `Texture2D` (`DescriptorHandle` path unchanged). Feature currently **false** (GPU-gated); do not ship ASTC HDR assets until enabled.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Vulkan 1.3 core | Feature bit home |
| Format feature queries | Confirm sampled/filter/blit per ASTC HDR format |
| Texture manager / asset pipeline | Must honor feature gate |
| Other compression (BC, ETC2, ASTC LDR) | Fallbacks when HDR ASTC unavailable |
| Maintenance / copy paths | Compressed format copy/blit restrictions still apply |

---

## 10. Non-goals / do not do

- Do **not** force `textureCompressionASTC_HDR = true` on all developer machines.  
- Do **not** list `VK_EXT_texture_compression_astc_hdr` as a device extension on 1.3+.  
- Do **not** assume desktop NVIDIA always supports ASTC HDR.  
- Do **not** create ASTC HDR images without checking both the feature bit and format features.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| How exposed? | **Core 1.3** feature `textureCompressionASTC_HDR` |
| Current value? | **`false`** (RTX 2060 Super class not supported) |
| Device extension? | **No** (correct) |
| Consumed? | **No** ASTC HDR asset path required |
| Still valid? | **Yes** — optional format feature when GPU supports it |
| Highest-value next steps | Keep gated; optional query-and-enable; never hard-require |
