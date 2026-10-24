# VK_KHR_shader_untyped_pointers — Implementation Spec

**Status in this repo: ENABLED (device extension + feature), NOT CONSUMED by shader code yet (required interaction partner for `VK_EXT_descriptor_heap`).**

Official sources:

- [VK_KHR_shader_untyped_pointers](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_untyped_pointers.html)
- [Proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_shader_untyped_pointers.html)
- [VkPhysicalDeviceShaderUntypedPointersFeaturesKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceShaderUntypedPointersFeaturesKHR.html)
- SPIR-V: [SPV_KHR_untyped_pointers](https://github.khronos.org/SPIRV-Registry/extensions/KHR/SPV_KHR_untyped_pointers.html)

> Note: The extension is **KHR**. Descriptor-heap untyped shaders combine this with `SPV_EXT_descriptor_heap` / `layout(descriptor_heap)` in high-level languages.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::KHRShaderUntypedPointersExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → feature chain | `vk::PhysicalDeviceShaderUntypedPointersFeaturesKHR{ .shaderUntypedPointers = true }` |
| Property query | — | **None** (feature-only extension) |
| Capability cache | `src/core/types.hpp` | **No** dedicated property field |
| API usage | shaders / pipeline | **None** — no untyped-pointer SPIR-V path yet |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::KHRShaderUntypedPointersExtensionName,

// src/core/vk_device.cpp (feature chain)
// vk::PhysicalDeviceShaderUntypedPointersFeaturesKHR
{.shaderUntypedPointers = true},
```

### Not wired yet

- Explicit untyped loads/stores in engine shaders (`mesh.slang` uses typed BDA + `DescriptorHandle`)
- Validation that shader capabilities match enabled feature

Toolchain already lists `SPV_KHR_untyped_pointers` and `SPV_EXT_descriptor_heap` in root `CMakeLists.txt`.

---

## 2. What the extension provides

Vulkan support for SPIR-V untyped pointers: reinterpret data accessed through memory and atomic ops relative to the declared variable type without extra conversion instructions. Enables efficient translation from templated load/store in high-level languages and simplifies mixed-precision storage patterns.

| Feature | New API / bit | Engine relevance |
| --- | --- | --- |
| Master feature | `shaderUntypedPointers` | Must be true for untyped SPIR-V |
| SPIR-V capability | `SPV_KHR_untyped_pointers` | Shader compile target |
| Descriptor heap untyped model | With `VK_EXT_descriptor_heap` | Index heaps without set/binding mapping tables |

This is a SPIR-V capability (`SPV_KHR_untyped_pointers`), not a new host command. It lets loads/stores/atomics go through pointers without a single static pointee type on every instruction — the compiler target for templated access, byte-address patterns, and the descriptor-heap untyped shader model.

This engine already enables the feature as a heap-stack requirement. Live Slang (`shader.slang` / `mesh.slang`) uses BDA `reinterpret<T*>` and `DescriptorHandle`, not explicit `layout(descriptor_heap)` arrays. Keep the feature on; do not treat it as unused just because shaders still go through Slang handles.

For descriptor heaps, the guide’s “untyped shader model” uses heap-bound descriptor arrays:

```glsl
layout(descriptor_heap) buffer SSBO { vec4 payload; } s_buffers[];
layout(descriptor_heap) uniform texture2D Textures[];
// index = heap byte offset / type-specific descriptor size (stride)
```

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
typedef struct VkPhysicalDeviceShaderUntypedPointersFeaturesKHR {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR
    void*           pNext;
    VkBool32        shaderUntypedPointers;
} VkPhysicalDeviceShaderUntypedPointersFeaturesKHR;
```

**Already enabled** in this project. Keep `shaderUntypedPointers = true` whenever the extension name is listed.

### 3.2 Properties

None.

### 3.3 Commands

None at the C API level. Consumption is entirely through **shader SPIR-V** that declares the untyped-pointer capability and related instructions.

### 3.4 Interaction notes

- `VK_EXT_descriptor_heap` requires the **implementation** to support this extension; enabling it is required for the untyped heap shader model, not for pure set/binding mapping.
- This engine enables both and already uses push data + heap bind; shaders should eventually drop set/binding dependence.

---

## Limits

Feature `shaderUntypedPointers` only. **No property struct.** Nothing is cached in `HardwareCapabilities`.

This is a SPIR-V capability used with heap mapping / untyped loads. There are no extra numeric Required Limits for the extension itself. Heap strides still come from `HardwareCapabilities::descriptorHeap` (`imageDescriptorSize`, `bufferDescriptorSize`, `samplerDescriptorSize`). See [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html).

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep `vk::KHRShaderUntypedPointersExtensionName` |
| `src/core/vk_device.cpp` | Keep `{.shaderUntypedPointers = true}` |

### 4.2 Primary consumer — shaders (NOT YET)

**Likely paths:**

- Engine shader sources under the tree used by `ENGINE_SHADER_DIR` / `src/render/vk_pipeline.cpp` (`base/shader.spv`)
- Any future mesh/compute/RT shaders

Steps when consuming:

1. Compile with a toolchain that emits `SPV_KHR_untyped_pointers` (and descriptor-heap SPIR-V if using heaps).
2. Ensure pipeline/shader object create does not require conflicting set/binding mapping when using pure untyped heaps.
3. Index resources with strides from `HardwareCapabilities::descriptorHeap` (`bufferDescriptorSize`, `imageDescriptorSize`, `samplerDescriptorSize`) matching what `DescriptorManager` packs.

### 4.3 Coupling to descriptor heap — ACTIVE enablement, incomplete shaders

**Files:** `src/core/vk_descriptors.cpp`, `src/render/vk_renderer.cpp`, `src/render/vk_pipeline.cpp`

CPU side already:

- Binds heaps and pushes resource/sampler indices + BDAs

GPU side still needs:

- SPIR-V that reads those indices as heap array subscripts (untyped or mapping-based)

Until shaders catch up, the extension is “enabled for future/required support,” not a functional win by itself.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep enablement as-is | `vk_device.*` | Required sibling of descriptor heap |
| P1 | Compile main pass shaders with untyped + descriptor_heap | shader build + `shader.spv` | Unlocks heap indexing without mapping tables |
| P1 | Document index/stride convention matching `DescriptorManager` | shaders + descriptors | Avoid off-by-size bugs |
| P2 | Optional: keep mapping info path for transitional SPIR-V | `vk_pipeline.cpp` | Migrate assets gradually |
| P3 | Use untyped pointers for non-heap BDA reinterpret patterns | compute/RT shaders | Secondary value |

### Implementation recipe

1. Keep `vk::KHRShaderUntypedPointersExtensionName` and `shaderUntypedPointers = true` in `vk_device.*`. No property to cache in `types.hpp`.
2. Leave it enabled whenever `VK_EXT_descriptor_heap` is listed — implementations must support this extension for heaps.
3. Live shaders (`shaders/base/shader.slang`, `shaders/base/mesh.slang`) use BDA `reinterpret<T*>` and `DescriptorHandle` via `pushDataEXT`. That path does not yet emit a dedicated untyped-heap dialect; keep the feature on so slangc / heap SPIR-V stays legal.
4. When compiling explicit `layout(descriptor_heap)` arrays, index with strides from `HardwareCapabilities::descriptorHeap` matching `vk_descriptors.cpp` packing.
5. Validate that SPIR-V capabilities ⊆ enabled device features. Do not add C API call sites — consumption is shader-only.

---

## 6. Concrete integration sketch (shader-facing)

CPU remains as today (`pushDataEXT` with indices). Shader-side sketch:

```glsl
// Requires SPV_KHR_untyped_pointers + descriptor heap decorations
layout(descriptor_heap) uniform texture2D Textures[];
layout(descriptor_heap) uniform sampler   Samplers[];

layout(push_data) uniform Push {
    uint64_t objectUboAddress;
    uint64_t cameraAddress;
    uint     textureIndex;
    uint     samplerIndex;
} push;

void main() {
    vec4 c = texture(sampler2D(Textures[push.textureIndex], Samplers[push.samplerIndex]), uv);
}
```

Exact push-data / decoration syntax depends on the HLSL/GLSL → SPIR-V toolchain; keep CPU `PushData2` layout matching the shader push block.

---

## 7. Validation / test checklist

1. Device creates with extension + `shaderUntypedPointers = true`.
2. Loading a SPIR-V module that declares untyped-pointer capability succeeds only when feature is enabled.
3. Heap-mode draws with untyped shaders: correct texture/sampler selection vs known indices.
4. Stride mismatch test: wrong index math fails visibly — confirm packing uses property sizes.
5. No validation errors when mixing BDA push addresses with untyped buffer pointers (if used).

---

## 8. Shader interaction

**Direct — SPIR-V untyped pointer capability for flexible memory and heap models.** Feature `shaderUntypedPointers` enables `SPV_KHR_untyped_pointers`: loads/stores/atomics through pointers without forcing a single static pointee type in every instruction. High-level languages use this for templates, byte-address style access, and the **descriptor-heap untyped shader model**.

### Interaction with descriptor heaps

`VK_EXT_descriptor_heap` requires the **implementation** to support this extension. Full untyped heap shaders look like:

```glsl
// Conceptual GLSL untyped heap model (from Vulkan descriptor-heap guide)
layout(descriptor_heap) uniform texture2D Textures[];
layout(descriptor_heap) buffer SSBO { vec4 v; } Buffers[];
// index = heap_byte_offset / descriptor_stride_for_type
```

Slang can lower heap ops to `LoadResourceDescriptorFromHeap` / `LoadSamplerDescriptorFromHeap` style IR on bindless backends.

### Interaction with BDA

Physical storage buffer pointers (BDA) and untyped pointers are complementary:

- **BDA** = 64-bit address of buffer memory content (UBO/SSBO payload).
- **Untyped pointers** = SPIR-V typing rules for how those (and heap) accesses are represented.

This engine currently:

| Path | Untyped pointers needed? |
| --- | --- |
| Slang `reinterpret<T*>(uint64_t)` on BDA | Depends on backend emission; feature kept **true** |
| `DescriptorHandle` + `getDescriptorFromHandle` | Heap path; feature enabled as heap stack requirement |
| Explicit `layout(descriptor_heap)` arrays | Future; needs this feature |

### Engine status

Extension + feature **enabled**. No dedicated “untyped-only” shader dialect yet; keep enabled whenever heaps or advanced pointer SPIR-V are used. Validate that slangc SPIR-V capabilities ⊆ enabled device features.

Cross-refs: [VK_EXT_descriptor_heap](./VK_EXT_descriptor_heap.md), [VK_KHR_buffer_device_address](./VK_KHR_buffer_device_address.md).

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| `VK_EXT_descriptor_heap` | Primary consumer of untyped model in this engine |
| SPIR-V 1.x + `SPV_KHR_untyped_pointers` | Compile-time dependency |
| `bufferDeviceAddress` (Vulkan 1.2+) | Common partner for address-in-push-data patterns (already enabled) |
| `VK_EXT_shader_object` | Same SPIR-V can be bound as shader objects later |

---

## 10. Non-goals / do not do

- Do not disable this extension while `VK_EXT_descriptor_heap` remains required — implementations must support it for heaps; this project enables it explicitly.
- Do not assume untyped pointers alone change C API call sites.
- Do not ship SPIR-V with untyped capability if the feature bit is ever made optional on a fallback path.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is Shader Untyped Pointers enabled? | **Yes** — extension listed, `shaderUntypedPointers = true` |
| Is it used? | **Not in shaders yet** — enablement supports descriptor-heap roadmap |
| Highest-value next steps | Compile/bind heap-capable untyped shaders matching `DescriptorManager` packing |
| Secondary steps | Transitional mapping path; broader untyped BDA patterns |
