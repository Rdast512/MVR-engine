# VK_EXT_descriptor_indexing — Implementation Spec

**Status in this repo: PROMOTED TO CORE (Vulkan 1.2), ENABLED via multiple `Vulkan12Features` descriptor-indexing bits, properties cached; still valid alongside `VK_EXT_descriptor_heap`.**

Official sources:

- [VK_EXT_descriptor_indexing](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_indexing.html) (historical extension name)
- [VkPhysicalDeviceVulkan12Features](https://docs.vulkan.org/spec/latest/chapters/features.html) (descriptor indexing feature bits)
- [VkPhysicalDeviceDescriptorIndexingProperties](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDescriptorIndexingProperties.html)
- [Descriptor indexing guide](https://docs.vulkan.org/guide/latest/descriptor_indexing.html)

> **PROMOTED TO CORE — do not list as a device extension.** With API version 1.2+, enable through `VkPhysicalDeviceVulkan12Features` and query properties via core / `VkPhysicalDeviceDescriptorIndexingProperties`. This project targets Vulkan 1.4; do **not** put `VK_EXT_descriptor_indexing` in `requiredDeviceExtension`.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` | **Must not list** — promoted (1.2 comment block) |
| Feature enable | `src/core/vk_device.cpp` → `Vulkan12Features` | Multiple indexing-related bits forced `true` |
| Property query | `src/core/vk_device.cpp` → properties chain | `vk::PhysicalDeviceDescriptorIndexingPropertiesEXT` queried |
| Capability cache | `src/core/types.hpp` → `HardwareCapabilities::descriptorIndexing` | Cached |
| Runtime bind mode | `DescriptorBindingMode` | Heaps preferred; **LegacySets** fallback still possible |
| API usage | pipelines / layouts / legacy sets | Feature foundation on; full bindless array consumption limited |

### Already wired (features)

```cpp
// src/core/vk_device.cpp — Vulkan12Features
.descriptorIndexing = true,
.shaderSampledImageArrayNonUniformIndexing = true,
.shaderStorageBufferArrayNonUniformIndexing = true,
.descriptorBindingPartiallyBound = true,
.descriptorBindingVariableDescriptorCount = true,
.runtimeDescriptorArray = true,
```

### Already wired (properties)

```cpp
// properties chain
vk::PhysicalDeviceDescriptorIndexingPropertiesEXT, // comment: "Maybe not ext"
// ...
capabilities.descriptorIndexing =
    propertiesChain.get<vk::PhysicalDeviceDescriptorIndexingPropertiesEXT>();

// src/core/types.hpp
vk::PhysicalDeviceDescriptorIndexingPropertiesEXT descriptorIndexing;
```

### Relationship to descriptor heaps

| Path | Role |
| --- | --- |
| **Indexing (this doc)** | Bindless-style **descriptor sets**: large arrays, non-uniform indexing, partially bound, variable count, runtime arrays. Still the model for **legacy sets** and for limits/tooling. |
| **`VK_EXT_descriptor_heap`** | Newer **heap bind** model (bind heaps + push data / untyped access). Preferred when supported (`DescriptorBindingMode::DescriptorHeaps`). |

**Not fully replaced:** heap is an alternative bind model; indexing features and properties remain relevant for:

- Fallback `LegacySets` path when heaps are unsupported
- SPIR-V / shader capability expectations on arrayed descriptors
- Limit queries (`maxUpdateAfterBind*`, non-uniform indexing limits, etc.)
- Any remaining set/layout APIs that still use update-after-bind or large arrays

---

## 2. What core provides

Vulkan 1.2 folded descriptor indexing into core as optional feature bits (not “always on”). Enabling them unlocks modern bindless set layouts.

| Feature bit (enabled here) | Meaning | Engine relevance |
| --- | --- | --- |
| `descriptorIndexing` | Master umbrella for indexing feature set | Required gate |
| `shaderSampledImageArrayNonUniformIndexing` | Non-uniform texture array index | Bindless textures in fragment/compute |
| `shaderStorageBufferArrayNonUniformIndexing` | Non-uniform SSBO array index | Bindless buffers |
| `descriptorBindingPartiallyBound` | Unused bindings may be invalid | Sparse material tables |
| `descriptorBindingVariableDescriptorCount` | Last binding variable count | Dynamic array sizes at allocate |
| `runtimeDescriptorArray` | SPIR-V runtime-sized descriptor arrays | `[]` without fixed compile size |

Other 1.2 indexing bits exist (update-after-bind per type, input attachments, etc.) — **only enable when a concrete path needs them**. This repo enables the subset above.

Properties (cached as `descriptorIndexing`) include max update-after-bind counts, non-uniform indexing robustness flags, and related limits. Treat as **still-valid usable modern limits**, not dead metadata.

Descriptor indexing is the set-based bindless model: large arrays, runtime-sized bindings, partially bound slots, and non-uniform indexing. It is **not** the heap model. This engine prefers heaps (`DescriptorBindingMode::DescriptorHeaps`) and push-data `DescriptorHandle`s in `shader.slang` / `mesh.slang`, but the 1.2 indexing bits stay enabled so `LegacySets` and any residual set layouts remain legal.

`shader*ArrayNonUniformIndexingNative` flags are performance hints, not errors. Gate set sizes on `HardwareCapabilities::descriptorIndexing` (and `maintenance3.maxPerSetDescriptors` for non-UAB sets). Do not disable these features because heaps are on.

---

## 3. Structures and commands

### 3.1 Features (Vulkan 1.2)

```cpp
// Subset of VkPhysicalDeviceVulkan12Features (core 1.2+)
VkBool32 descriptorIndexing;
VkBool32 shaderSampledImageArrayNonUniformIndexing;
VkBool32 shaderStorageBufferArrayNonUniformIndexing;
VkBool32 descriptorBindingPartiallyBound;
VkBool32 descriptorBindingVariableDescriptorCount;
VkBool32 runtimeDescriptorArray;
// ... plus optional update-after-bind / other indexing bits not currently forced
```

**Already enabled** for the subset listed in §1. Keep via `Vulkan12Features` only.

### 3.2 Properties

```cpp
// Core name: VkPhysicalDeviceDescriptorIndexingProperties
// Vulkan-Hpp may still expose *EXT alias used in this codebase
typedef struct VkPhysicalDeviceDescriptorIndexingProperties {
    VkStructureType    sType;
    void*              pNext;
    uint32_t           maxUpdateAfterBindDescriptorsInAllPools;
    VkBool32           shaderUniformBufferArrayNonUniformIndexingNative;
    VkBool32           shaderSampledImageArrayNonUniformIndexingNative;
    VkBool32           shaderStorageBufferArrayNonUniformIndexingNative;
    VkBool32           shaderStorageImageArrayNonUniformIndexingNative;
    VkBool32           shaderInputAttachmentArrayNonUniformIndexingNative;
    VkBool32           robustBufferAccessUpdateAfterBind;
    VkBool32           quadDivergentImplicitLod;
    uint32_t           maxPerStageDescriptorUpdateAfterBindSamplers;
    // ... additional maxPerStage / maxDescriptorSet update-after-bind limits ...
} VkPhysicalDeviceDescriptorIndexingProperties;
```

**Already cached** as `HardwareCapabilities::descriptorIndexing`.

### 3.3 Layout / pool interaction (when using sets)

When building **legacy set** bindless layouts (not heaps):

- Binding flags: `PARTIALLY_BOUND`, `VARIABLE_DESCRIPTOR_COUNT`, `UPDATE_AFTER_BIND` as needed
- Pool flags: `UPDATE_AFTER_BIND` when any binding uses UAB
- Descriptor set allocate: `VkDescriptorSetVariableDescriptorCountAllocateInfo` for variable count
- Shaders: nonuniformEXT / equivalent SPIR-V decorations when indices are non-uniform

Heap path does not require these set flags for heap descriptors, but **features remain enabled** for mixed/fallback use.

---

## Limits

Cached in `HardwareCapabilities::descriptorIndexing` (`VkPhysicalDeviceDescriptorIndexingPropertiesEXT`). Spec Required Limits when `descriptorIndexing` is supported ([Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html)):

| Property | Required limit |
| --- | --- |
| `maxUpdateAfterBindDescriptorsInAllPools` | min **500000** |
| `maxPerStageDescriptorUpdateAfterBind*` / `maxDescriptorSetUpdateAfterBind*` | raised vs non-UAB (typically 500000 for images/samplers when `descriptorIndexing` is supported) |
| `maxPerSetDescriptors` (`maintenance3`) | still 1024 min for **non-UAB** sets |

`shaderUniformBufferArrayNonUniformIndexingNative`, `shaderSampledImageArrayNonUniformIndexingNative`, `shaderStorageBufferArrayNonUniformIndexingNative`, `shaderStorageImageArrayNonUniformIndexingNative`, and `shaderInputAttachmentArrayNonUniformIndexingNative` are **performance hints**, not errors. Gate legacy bindless layout sizes on the cached properties.

---

## 4. Where to implement / interact in this codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.cpp` | Keep Vulkan12 indexing feature bits |
| `src/core/types.hpp` | Keep `descriptorIndexing` properties cache |
| `src/core/vk_device.hpp` | Keep extension string **out** of required list |

### 4.2 Descriptor manager interaction

**Files:** `src/core/vk_descriptors.*`, `src/render/vk_pipeline.cpp`, `src/render/vk_renderer.cpp`

| Mode | Indexing role |
| --- | --- |
| `DescriptorHeaps` | Primary bind path; indexing still provides capability baseline and any residual set layouts |
| `LegacySets` | Indexing bits + properties become the bindless set design surface |

When adding large bindless texture/SSBO **sets** on the legacy path:

1. Read limits from `capabilities.descriptorIndexing`
2. Create layouts with partial-bound / variable-count flags as needed
3. Use non-uniform indexing in shaders only if the corresponding feature bit is true (already forced on)

### 4.3 Do not treat heap as a reason to disable indexing features

Disabling `descriptorIndexing` / non-uniform bits would break fallback sets and many intermediate tools. Heaps and indexing **coexist**.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep current Vulkan12 feature subset | `vk_device.cpp` | Required modern baseline |
| P0 | Never re-list as device extension | `vk_device.hpp` | Promoted to 1.2 |
| P1 | Gate legacy bindless layout sizes on cached properties | `vk_descriptors.*` | Avoid over-alloc vs limits |
| P2 | Enable extra UAB bits only if update-after-bind sets are added | `vk_device.cpp` | Don’t enable unused bits |
| P3 | Document shader nonuniform usage when bindless sets grow | shaders / pipeline | Correctness under wave divergence |

### Implementation recipe

1. Keep `HardwareCapabilities::descriptorIndexing` cached in `src/core/types.hpp`. Keep the Vulkan 1.2 feature subset in `vk_device.cpp`. **Never** list `VK_EXT_descriptor_indexing` as a device extension.
2. Treat heaps as the primary bind path (`vk_descriptors.cpp` / `vk_pipeline.cpp` / `vk_renderer.cpp` + `pushDataEXT`). Leave indexing bits **on** so `LegacySets` and residual set layouts stay valid.
3. If a legacy bindless set is added, size arrays from `capabilities.descriptorIndexing` (`maxUpdateAfterBindDescriptorsInAllPools` min 500000) and `maintenance3.maxPerSetDescriptors` (1024 min, non-UAB).
4. Engine shaders (`shaders/base/shader.slang`, `shaders/base/mesh.slang`) use BDA + `DescriptorHandle`, not large set arrays. Mark non-uniform indices when a material ID varies per lane.
5. Read `shader*ArrayNonUniformIndexingNative` as a perf hint only. Do not enable extra UAB bits without a consumer.

---

## 6. Integration sketch (legacy bindless set — illustrative)

```cpp
// Only for DescriptorBindingMode::LegacySets bindless arrays — not heap path.
const auto& di = device.getCapabilities().descriptorIndexing;

vk::DescriptorSetLayoutBindingFlagsCreateInfo bindingFlags{
    .bindingCount = 1,
    .pBindingFlags = /* PARTIALLY_BOUND | VARIABLE_DESCRIPTOR_COUNT | ... */
};

vk::DescriptorSetLayoutCreateInfo layoutInfo{
    .pNext = &bindingFlags,
    .flags = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool, // if UAB
    // bindings: runtime array of sampled images / storage buffers
};

// Allocate with variable count ≤ di.max* limits
```

Heap path sketch remains in `VK_EXT_descriptor_heap.md` (bind heaps + push data).

---

## 7. Validation / test checklist

1. Device creates with indexing bits true and **no** `VK_EXT_descriptor_indexing` extension string.
2. Capability dump shows descriptor indexing properties populated.
3. Heap mode: no regression when indexing features stay enabled.
4. LegacySets mode (force or GPU without heaps): large arrays respect property limits.
5. Non-uniform shader indexing validation clean when features are on.
6. Partially-bound: unused array slots do not fault when flag is set on the binding.

---

## 8. Shader interaction

**Direct for set-based bindless; secondary when heaps are primary.** Descriptor indexing is the Vulkan 1.2 core feature family that lets shaders index descriptor arrays with dynamic / non-uniform indices (`runtimeDescriptorArray`, `shader*ArrayNonUniformIndexing`, update-after-bind, partially bound, variable count).

### SPIR-V / language

| Feature bit | Shader effect |
| --- | --- |
| `runtimeDescriptorArray` | Unsized descriptor arrays `texture2D textures[]` |
| `shaderSampledImageArrayNonUniformIndexing` | Non-uniform index (e.g. per-lane material ID) with `NonUniform` decoration |
| `shaderStorageBufferArrayNonUniformIndexing` | Same for SSBOs |
| Update-after-bind / partially bound | Host can leave holes; shader must not access unbound slots |

GLSL: `GL_EXT_nonuniform_qualifier` / `nonuniformEXT(index)`. HLSL/Slang: ResourceDescriptorHeap-style or array indexing with nonuniform annotations as required by the backend.

### Relation to this engine

| Mode | Role of descriptor indexing |
| --- | --- |
| `DescriptorBindingMode::DescriptorHeaps` | Primary bind path is heaps + push handles; indexing bits still enabled as **foundation** and for any residual set layouts / validation |
| `LegacySets` fallback | Full bindless set arrays would consume these features directly |

Engine shaders today prefer **DescriptorHandle in push data** over large set arrays. Keep indexing features **on** so hybrid / legacy SPIR-V and future set-based bindless remain legal.

### Non-uniform indexing rule of thumb

If the index is not dynamically uniform across the subgroup/wave, mark it non-uniform or validation/hardware may fault. Mesh/FS material ID from vertex attributes or BDA-loaded data is a common case.

Cross-refs: [VK_EXT_descriptor_heap](./VK_EXT_descriptor_heap.md).

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Vulkan 1.2+ core | Promotion home |
| `VK_EXT_descriptor_heap` | Newer bind model; **does not replace** indexing for sets/limits |
| `VK_KHR_shader_untyped_pointers` | Heap untyped model; orthogonal to set indexing |
| `runtimeDescriptorArray` | SPIR-V runtime arrays in set-based bindless |
| Descriptor buffers (`descriptorBuffer` feature) | Another bind path; also coexists — not a reason to drop indexing |

---

## 10. Non-goals / do not do

- Do **not** add `VK_EXT_descriptor_indexing` to the device extension list on 1.2+ targets.
- Do **not** disable indexing features solely because heaps are enabled.
- Do not assume heaps make indexing properties unused — still cache and consult for sets/limits.
- Do not enable every update-after-bind bit “just in case” without a consumer.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Promotion | **Vulkan 1.2 core** |
| Enable path | Multiple `Vulkan12Features` bits (see §1) |
| Properties | Cached in `HardwareCapabilities::descriptorIndexing` |
| Consumed? | **Foundation enabled**; heap is primary bind path, indexing still required modern baseline + legacy sets |
| Replaced by heaps? | **No** — parallel models; indexing remains relevant |
| Device extension string? | **Never** on this 1.4 target |
