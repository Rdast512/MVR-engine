# VK_EXT_descriptor_heap — Implementation Spec

**Status in this repo: ENABLED (device extension + runtime-gated feature), PARTIALLY CONSUMED — heaps, writes, bind, and push data are live; full heap API surface still evolving.**

Official sources:

- [VK_EXT_descriptor_heap](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_heap.html)
- [Proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_descriptor_heap.html)
- [Descriptor heap guide](https://docs.vulkan.org/guide/latest/descriptor_heap.html)
- [VkPhysicalDeviceDescriptorHeapFeaturesEXT](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDescriptorHeapFeaturesEXT.html)
- [VkPhysicalDeviceDescriptorHeapPropertiesEXT](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDescriptorHeapPropertiesEXT.html)

> Note: `VK_KHR_shader_untyped_pointers` must be **supported** by the implementation for this extension; it does not need to be enabled when using only the legacy set/binding mapping interface. This engine enables both and is moving toward the untyped / push-data path.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::EXTDescriptorHeapExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → `createLogicalDevice()` feature chain | `descriptorHeap` set **only if** physical device reports support |
| Runtime mode | `src/core/vk_device.cpp` + `Device::descriptorBindingMode` | `DescriptorHeaps` or `LegacySets` |
| Property query | `src/core/vk_device.cpp` → properties chain | `vk::PhysicalDeviceDescriptorHeapPropertiesEXT` queried |
| Capability cache | `src/core/types.hpp` → `HardwareCapabilities::descriptorHeap` | Cached |
| API usage | `vk_descriptors.*`, `vk_pipeline.cpp`, `vk_renderer.cpp`, `texture_manager.cpp` | **Active** — heap buffers, writes, bind heaps, push data, pipeline flag |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::EXTDescriptorHeapExtensionName,

// src/core/vk_device.cpp (pre-device feature query)
const auto descriptorHeapFeatureQuery =
    physicalDevice.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceDescriptorHeapFeaturesEXT>();
const bool descriptorHeapFeatureSupported =
    descriptorHeapFeatureQuery.get<vk::PhysicalDeviceDescriptorHeapFeaturesEXT>().descriptorHeap == vk::True;
descriptorBindingMode =
    descriptorHeapFeatureSupported ? DescriptorBindingMode::DescriptorHeaps : DescriptorBindingMode::LegacySets;

// feature chain
// vk::PhysicalDeviceDescriptorHeapFeaturesEXT
{.descriptorHeap = descriptorHeapFeatureSupported},

// properties chain
capabilities.descriptorHeap =
    propertiesChain.get<vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();

// src/core/types.hpp
vk::PhysicalDeviceDescriptorHeapPropertiesEXT descriptorHeap;
enum class DescriptorBindingMode : uint8_t { LegacySets = 0, DescriptorHeaps = 1 };
```

### Actively consumed

- Heap buffer create with `eDescriptorHeapEXT` usage (`DescriptorManager::createHeapBuffers`)
- Host-mapped writes via `writeResourceDescriptorsEXT` / `writeSamplerDescriptorsEXT`
- Per-frame `bindResourceHeapEXT` / `bindSamplerHeapEXT` + `pushDataEXT` in `vk_renderer.cpp`
- Pipeline create with `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` when in heap mode (`vk_pipeline.cpp`)
- Texture `descriptorHeapIndex` stored on `TextureAsset` (`types.hpp`, `texture_manager.cpp`)

### Not fully wired yet / evolving

- Complete legacy set/binding → heap mapping path (`VkShaderDescriptorSetAndBindingMappingInfoEXT`) if keeping old SPIR-V
- Broader resource types in the resource heap (SSBO/UBO descriptors, AS, etc.) beyond sampled images + one sampler
- GPU-side descriptor heap updates + heap read barriers (`VK_ACCESS_2_*_HEAP_READ_BIT_EXT`)
- Capture/replay (`descriptorHeapCaptureReplay`) and opaque capture data
- Optional per-type packing via `vkGetPhysicalDeviceDescriptorSizeEXT`

---

## 2. What the extension provides

Explicit, DX12-like descriptor heaps: one **resource** heap and one **sampler** heap, stored in application-owned buffer memory. Descriptor sets and pipeline layouts can be eliminated; shaders look up descriptors by heap offset (typed set/binding mapping or untyped descriptor-heap model).

| Feature | New API / bit | Engine relevance |
| --- | --- | --- |
| Heap memory | `VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT` + BDA | `DescriptorManager` heap buffers |
| Write descriptors | `vkWriteResourceDescriptorsEXT`, `vkWriteSamplerDescriptorsEXT` | Texture + sampler init |
| Bind heaps | `vkCmdBindResourceHeapEXT`, `vkCmdBindSamplerHeapEXT` + `VkBindHeapInfoEXT` | Per-frame bind in renderer |
| Push data | `vkCmdPushDataEXT` + `VkPushDataInfoEXT` | Per-object handles / BDA in draw loop |
| Pipeline flag | `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` | Graphics pipeline create |
| Mapping (legacy SPIR-V) | `VkShaderDescriptorSetAndBindingMappingInfoEXT` | Transition path for set/binding shaders |
| Untyped model | `layout(descriptor_heap)` + untyped pointers | Preferred long-term shader model |
| Heap access bits | `VK_ACCESS_2_RESOURCE/SAMPLER_HEAP_READ_BIT_EXT` | Sync when GPU writes heaps |

Heaps are application-owned buffers with `DESCRIPTOR_HEAP` usage and a device address. The implementation reserves a trailing range in each bound heap that the app must not write. Shaders do not bind descriptor sets in this model: they resolve `DescriptorHandle` values from push data (`shaders/base/shader.slang`, `shaders/base/mesh.slang`) after the host binds one resource heap and one sampler heap.

Descriptor sizes, alignments, reserved ranges, and `maxPushDataSize` come from `HardwareCapabilities::descriptorHeap`. Do not hardcode slot strides — `DescriptorManager` already copies those properties in `vk_descriptors.cpp`.

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
typedef struct VkPhysicalDeviceDescriptorHeapFeaturesEXT {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT
    void*           pNext;
    VkBool32        descriptorHeap;
    VkBool32        descriptorHeapCaptureReplay; // optional tooling path
} VkPhysicalDeviceDescriptorHeapFeaturesEXT;
```

**Already enabled** only when supported. Do **not** force `descriptorHeap = true` on devices that lack the feature (this is the one extension that is intentionally runtime-gated).

### 3.2 Properties (query after physical device pick)

```cpp
typedef struct VkPhysicalDeviceDescriptorHeapPropertiesEXT {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT
    void*           pNext;
    // sizes (worst-case per category; tight sizes via vkGetPhysicalDeviceDescriptorSizeEXT)
    VkDeviceSize    samplerDescriptorSize;
    VkDeviceSize    bufferDescriptorSize;
    VkDeviceSize    imageDescriptorSize;
    // alignments
    VkDeviceSize    samplerDescriptorAlignment;
    VkDeviceSize    bufferDescriptorAlignment;
    VkDeviceSize    imageDescriptorAlignment;
    VkDeviceSize    resourceHeapAlignment;
    VkDeviceSize    samplerHeapAlignment;
    // heap limits + reserved driver ranges
    VkDeviceSize    maxResourceHeapSize;
    VkDeviceSize    maxSamplerHeapSize;
    VkDeviceSize    minResourceHeapReservedRange;
    VkDeviceSize    minSamplerHeapReservedRange;
    // plus additional limits (max push data size, etc.) per spec revision
} VkPhysicalDeviceDescriptorHeapPropertiesEXT;
```

**Already cached** as `HardwareCapabilities::descriptorHeap` and consumed in `DescriptorManager` for sizes, alignments, max heap sizes, and reserved ranges.

### 3.3 Bind heap

```cpp
typedef struct VkBindHeapInfoEXT {
    VkStructureType         sType; // VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT
    const void*             pNext;
    VkDeviceAddressRangeEXT heapRange;          // address + size of bound heap memory
    VkDeviceSize            reservedRangeOffset;
    VkDeviceSize            reservedRangeSize;  // driver-internal; app must not access
} VkBindHeapInfoEXT;

void vkCmdBindResourceHeapEXT(VkCommandBuffer commandBuffer, const VkBindHeapInfoEXT* pBindHeapInfo);
void vkCmdBindSamplerHeapEXT(VkCommandBuffer commandBuffer, const VkBindHeapInfoEXT* pBindHeapInfo);
```

### 3.4 Write descriptors (host)

```cpp
void vkWriteResourceDescriptorsEXT(
    VkDevice                                device,
    uint32_t                                descriptorInfoCount,
    const VkResourceDescriptorInfoEXT*      pDescriptorInfos,
    const VkHostAddressRangeEXT*            pDescriptors); // host memory / mapped heap

void vkWriteSamplerDescriptorsEXT(
    VkDevice                                device,
    uint32_t                                descriptorCount,
    const VkSamplerCreateInfo*              pCreateInfos,
    const VkHostAddressRangeEXT*            pDescriptors);
```

Resource writes take typed info (e.g. `VkImageDescriptorInfoEXT` with view create info + layout). Sampler writes take `VkSamplerCreateInfo` directly (no `VkSampler` object required for heap mode).

### 3.5 Push data

```cpp
typedef struct VkPushDataInfoEXT {
    VkStructureType              sType; // VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT
    const void*                  pNext;
    uint32_t                     offset;
    VkHostAddressRangeConstEXT   data; // host pointer + size
} VkPushDataInfoEXT;

void vkCmdPushDataEXT(VkCommandBuffer commandBuffer, const VkPushDataInfoEXT* pPushDataInfo);
```

Replaces push constants when using the descriptor-heap model; engine uses `PushData2` (object UBO BDA, camera BDA, texture/sampler indices).

### 3.6 Key usage / create flags

| Flag / bit | Meaning |
| --- | --- |
| `VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT` | Buffer is a descriptor heap |
| `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` | Pipeline created for heap binding model |
| `VK_ACCESS_2_RESOURCE_HEAP_READ_BIT_EXT` | Shader/heap read of resource heap |
| `VK_ACCESS_2_SAMPLER_HEAP_READ_BIT_EXT` | Shader/heap read of sampler heap |

---

## Limits

Cached in `HardwareCapabilities::descriptorHeap` (`VkPhysicalDeviceDescriptorHeapPropertiesEXT`). Spec Required Limits when `descriptorHeap` is supported ([Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html)):

| Property | Required limit |
| --- | --- |
| `maxPushDataSize` | min **256** |
| `samplerDescriptorSize` | max **32** |
| `imageDescriptorSize` | max **64** |
| `bufferDescriptorSize` | max **128** |
| `minResourceHeapReservedRange` | max 2^15 × max(image, buffer descriptor size) |
| `minSamplerHeapReservedRange` | max 96 × `samplerDescriptorSize` |
| `minSamplerHeapReservedRangeWithEmbedded` | max 2048 × `samplerDescriptorSize` |
| `maxSamplerHeapSize` / `maxResourceHeapSize` | large minima — see Required Limits; do not invent numbers |
| Sampler / image / buffer descriptor alignments | power of two; query (alignment ≤ corresponding size) |

Exactly **one** sampler heap and **one** resource heap may be bound. `DescriptorManager` already copies sizes, alignments, and reserved ranges from the cached struct — do not hardcode slot strides or reserved-range bytes.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE (with runtime gate)

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep `vk::EXTDescriptorHeapExtensionName`; `descriptorBindingMode` |
| `src/core/vk_device.cpp` | Keep feature query + conditional enable; keep property cache |
| `src/core/types.hpp` | Keep `HardwareCapabilities::descriptorHeap`, `DescriptorBindingMode` |

### 4.2 Primary consumer — DescriptorManager — ACTIVE

**Files:** `src/core/vk_descriptors.hpp`, `src/core/vk_descriptors.cpp`

Current responsibilities:

1. Cache sizes/alignments/reserved ranges from `capabilities.descriptorHeap`.
2. Create host-visible heap buffers (`eDescriptorHeapEXT` + BDA).
3. Fill `resourceHeapInfo` / `samplerHeapInfo` (`VkBindHeapInfoEXT`) with device addresses and reserved ranges at the end of each heap.
4. Write sampled-image descriptors into the resource heap; write default sampler into the sampler heap.
5. Assign `TextureAsset::descriptorHeapIndex` from byte offset / alignment.

### 4.3 Pipeline create — ACTIVE

**File:** `src/render/vk_pipeline.cpp`

- When `descriptorBindingMode == DescriptorHeaps`: zero set layouts, push-constant/push-data size range for layout sizing if needed, `VK_NULL_HANDLE` pipeline layout, `eDescriptorHeapEXT` pipeline create flags 2.
- When legacy: classic set layout + pipeline layout.

### 4.4 Frame record — ACTIVE

**File:** `src/render/vk_renderer.cpp`

```cpp
cmd.bindResourceHeapEXT(resourceHeapInfo);
cmd.bindSamplerHeapEXT(samplerHeapInfo);
// per object:
cmd.pushDataEXT(pushDataInfo);
cmd.drawIndexed(...);
```

### 4.5 Texture path — ACTIVE

**File:** `src/core/texture_manager.cpp`

- Returns / stores `descriptorHeapIndex` for loaded textures for use in push data.

### 4.6 Remaining work

| Item | File(s) |
| --- | --- |
| Grow heap write API for UBO/SSBO/AS descriptors as content expands | `vk_descriptors.cpp` |
| Shader path: untyped `layout(descriptor_heap)` vs mapping info | shader sources + `vk_pipeline.cpp` |
| Optional secondary-cmd inheritance heaps | future secondary CB path |
| Avoid rebinding heaps mid-CB (costly) | renderer policy |

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep runtime-gated enablement | `vk_device.*` | Correct on mixed GPU support |
| P0 | Keep property-driven heap layout | `vk_descriptors.cpp` | Portability (sizes/alignments/reserved) |
| P1 | Complete shader model (untyped or full mapping) | shaders + pipeline | Heaps only useful if shaders index correctly |
| P1 | Expand resource types in heap | `vk_descriptors.cpp` | Beyond one texture + one sampler |
| P2 | GPU heap updates + heap access barriers | descriptors / barriers | Dynamic content |
| P2 | Log key heap properties at init | `vk_device.cpp` | Debuggability |
| P3 | Capture/replay support | device features | Tooling only |

### Implementation recipe

1. Keep `HardwareCapabilities::descriptorHeap` cached in `src/core/types.hpp`. Runtime-gate the feature in `vk_device.cpp`; set `DescriptorBindingMode::DescriptorHeaps` only when `descriptorHeap == true`.
2. In `src/core/vk_descriptors.cpp`, size/align heaps from the cached properties (descriptor sizes, alignments, reserved ranges, max heap sizes). Create buffers with `eDescriptorHeapEXT | eShaderDeviceAddress`, map, `writeResourceDescriptorsEXT` / `writeSamplerDescriptorsEXT`, fill `BindHeapInfoEXT` with BDA + reserved range at the end. Store `TextureAsset::descriptorHeapIndex`.
3. In `src/render/vk_pipeline.cpp`, heap mode: zero set layouts, `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT`, push-data range matching `PushData2` / `MeshPushData`.
4. In `src/render/vk_renderer.cpp`, bind one resource heap and one sampler heap once per CB, then `pushDataEXT` per draw. Shaders (`shaders/base/shader.slang`, `shaders/base/mesh.slang`) consume BDA + `DescriptorHandle`.
5. Do not write the reserved range. Do not rebind heaps every draw. Keep `LegacySets` as the thin fallback when the feature query is false.

---

## 6. Concrete integration sketch (already close to production)

Illustrative shape matching current code:

```cpp
// Device: gate feature
const bool heapOk = /* query descriptorHeap feature */;
device.descriptorBindingMode =
    heapOk ? DescriptorBindingMode::DescriptorHeaps : DescriptorBindingMode::LegacySets;

// DescriptorManager: allocate heaps
createBuffer(maxResourceHeapSize,
    eStorageBuffer | eShaderDeviceAddress | eDescriptorHeapEXT,
    eHostVisible | eHostCoherent, resourceHeapBuffer, ...);
// map, writeImageDescriptor / writeSamplerDescriptorsEXT into mapped memory
// fill BindHeapInfoEXT with BDA + reserved range at end

// Renderer draw:
cmd.bindResourceHeapEXT(resourceHeapInfo);
cmd.bindSamplerHeapEXT(samplerHeapInfo);
cmd.pushDataEXT({.offset = 0, .data = {&pushData, sizeof(pushData)}});
cmd.drawIndexed(...);
```

Do **not** invent a second descriptor path abstraction until the legacy set path is retired; keep the `DescriptorBindingMode` branch thin.

---

## 7. Validation / test checklist

1. Device creates with extension listed; feature true only when supported; log mode (`DescriptorHeaps` vs `LegacySets`).
2. Heap property dump: descriptor sizes/alignments, max heap sizes, reserved ranges non-zero where required.
3. Heap buffers created with `DESCRIPTOR_HEAP` usage and BDA.
4. `write*DescriptorsEXT` into mapped heap: no validation errors; indices increase by alignment.
5. Frame: bind both heaps once, push data, draw — no validation errors.
6. Pipeline has `DESCRIPTOR_HEAP` create flag when in heap mode.
7. Texture reloads update heap slots / indices consistently.
8. Reserved range not written by application.
9. Alignments: descriptor offsets multiple of `*DescriptorAlignment`; bind addresses respect heap alignments.

---

## 8. Shader interaction

**Direct — shaders sample/bind resources by heap handles or heap indices, not classic descriptor sets.** Host binds resource + sampler heaps (`vkCmdBind*HeapEXT`) and pushes per-draw data (`vkCmdPushDataEXT`). Pipelines are created with `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT`.

### Two shader models

| Model | Shader view | When |
| --- | --- | --- |
| **Typed handles in push data (this engine today)** | `DescriptorHandle<Texture2D>` / `DescriptorHandle<SamplerState>` in push constants; resolve via `getDescriptorFromHandle` | Live in `shader.slang` / `mesh.slang` |
| **Untyped `layout(descriptor_heap)` arrays** | Global heap arrays indexed by byte-offset / stride; needs `VK_KHR_shader_untyped_pointers` + heap SPIR-V | Preferred long-term; not fully required for current Slang handle path |
| **Legacy set/binding mapping** | Old SPIR-V set/binding remapped via `VkShaderDescriptorSetAndBindingMappingInfoEXT` | Transition only |

### Engine Slang (push-data handles)

```slang
// Classic FS (shader.slang) and mesh FS (mesh.slang)
struct PushData /* or MeshPushData */ {
    // … BDA fields …
    DescriptorHandle<Texture2D> texture;
    DescriptorHandle<SamplerState> samplerHandle;
};
[[vk::push_constant]] ConstantBuffer<PushData> push;

Texture2D tex = getDescriptorFromHandle(push.texture);
SamplerState s = getDescriptorFromHandle(push.samplerHandle);
return tex.Sample(s, uv);
```

Host side matches `SlangHandle` / `uint2` packing in `push_data.hpp` and fills handles from `TextureAsset::descriptorHeapIndex` + sampler slot.

### SPIR-V / capability notes

- Heap bind makes resource and sampler heaps visible to the bound pipeline/shaders for the command buffer.
- Heap **reads** from shaders use access masks `VK_ACCESS_2_RESOURCE_HEAP_READ_BIT_EXT` / `SAMPLER_HEAP_READ_BIT_EXT` when synchronizing GPU-updated heaps.
- Push data replaces traditional push-constant-only or UBO-index patterns for handles + BDAs; size must match pipeline push range (`PushData2` 32 B classic; `MeshPushData` 72 B mesh — pipeline ranges must match).

### Pipeline / stage coupling

Works for **vertex, fragment, mesh, compute, RT** stages as long as the pipeline (or shader object) was created with the descriptor-heap flag and shaders use the heap model. Classic vertex path still uses vertex attributes for positions; mesh path uses BDA + heaps for textures.

Cross-refs: [VK_KHR_shader_untyped_pointers](./VK_KHR_shader_untyped_pointers.md), [VK_KHR_buffer_device_address](./VK_KHR_buffer_device_address.md), [VK_EXT_descriptor_indexing](./VK_EXT_descriptor_indexing.md).

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Vulkan 1.2+ BDA / `bufferDeviceAddress` | Heap buffers need device addresses (enabled) |
| `VK_KHR_maintenance5` / extended flags | Pipeline create flags 2 (enabled via 1.4 features) |
| `VK_KHR_shader_untyped_pointers` | Required support; needed for untyped heap shader model |
| `VK_EXT_descriptor_buffer` | Conceptual predecessor; heap is the preferred replacement (also listed in feature chain) |
| `VK_EXT_shader_object` | Shader create flag `DESCRIPTOR_HEAP` if migrating off pipelines |
| `VK_EXT_device_generated_commands` | Push-data tokens for DGC |
| VMA / host-visible memory | Current write path maps heap memory |

---

## 10. Non-goals / do not do

- Do not force `descriptorHeap = true` when the feature query is false.
- Do not access the reserved range of a bound heap.
- Do not assume null descriptors are all-zero bits; use explicit null writes if needed.
- Do not rebind heaps every draw without need (expensive).
- Do not mix combined-image-sampler assumptions into the untyped model (use separate image + sampler).
- Do not treat this as optional “nice to have” once heap mode is selected — pipeline, shaders, and frame bind must stay consistent.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is Descriptor Heap enabled? | **Yes when supported** — extension always listed; feature bit runtime-gated; mode in `DescriptorBindingMode` |
| Is it used? | **Yes, partially** — heap alloc, writes, bind, push data, pipeline flag, texture indices |
| Highest-value next steps | Finish shader indexing model; expand heap resource coverage |
| Secondary steps | GPU updates + barriers; property logging; retire legacy set path |
