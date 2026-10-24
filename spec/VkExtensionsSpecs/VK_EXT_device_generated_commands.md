# VK_EXT_device_generated_commands — Implementation Spec

**Status in this repo: ENABLED and CONSUMED — `PUSH_DATA` + `DRAW_MESH_TASKS` is the only mesh draw path. CPU fills sequences; no `pushDataEXT`/`drawMeshTasksEXT` fallback. No GPU cull pass yet.**

Official sources:

- [VK_EXT_device_generated_commands](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_device_generated_commands.html)
- [Proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_device_generated_commands.html)
- [VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT.html)
- [VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT.html)

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::EXTDeviceGeneratedCommandsExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → feature chain | `deviceGeneratedCommands = true` |
| Property query | `src/core/vk_device.cpp` → properties chain | `vk::PhysicalDeviceDeviceGeneratedCommandsPropertiesEXT` queried |
| Capability cache | `src/core/types.hpp` → `HardwareCapabilities::deviceGeneratedCommands` | Cached |
| API usage | renderer / GPU-driven | **Consumed** — `src/render/vk_dgc.cpp` layout + preprocess/execute; CPU fills sequences |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::EXTDeviceGeneratedCommandsExtensionName,

// src/core/vk_device.cpp (feature chain)
// vk::PhysicalDeviceDeviceGeneratedCommandsFeaturesEXT
{.deviceGeneratedCommands = true},

// properties + P1 log of cached EXT limits
capabilities.deviceGeneratedCommands =
    propertiesChain.get<vk::PhysicalDeviceDeviceGeneratedCommandsPropertiesEXT>();

// src/core/types.hpp
vk::PhysicalDeviceDeviceGeneratedCommandsPropertiesEXT deviceGeneratedCommands;

// src/render/vk_dgc.cpp — PUSH_DATA + DRAW_MESH_TASKS layout,
// preprocess buffer from vkGetGeneratedCommandsMemoryRequirementsEXT,
// CPU-filled sequences, preprocessGeneratedCommandsEXT + executeGeneratedCommandsEXT
```

### Not wired yet

- GPU compute fill of the sequences buffer (cull / compact)
- Pipeline / shader-object bind tokens (`EXECUTION_SET`)
- Indexed-draw tokens (live path is mesh)
- Optional extended DGC feature bits beyond the master switch

---

## 2. What the extension provides

Device-side generation and execution of a limited command stream: the GPU writes sequences of “tokens” (draws, state binds, push data) that the GPU later executes — core building block for fully GPU-driven rendering without CPU command buffer rebuilds.

| Feature | New API / bit | Engine relevance |
| --- | --- | --- |
| Layout object | `VkIndirectCommandsLayoutEXT` | Describes token stream |
| Preprocess | `vkCmdPreprocessGeneratedCommandsEXT` | Optional explicit preprocess pass |
| Execute | `vkCmdExecuteGeneratedCommandsEXT` | Run generated stream |
| Tokens | draw/draw indexed/mesh, bind pipeline/shader, push constants/data, … | Matches engine features over time |
| Properties | max token counts, alignments, preprocess sizes | Buffer allocation |

DGC lets the GPU write a token stream (push data, draws, optional binds) that the GPU later executes. It does not add shader stages. Tokens must match the live heap contract: `pushDataEXT` layouts (`PushData2` / `MeshPushData`) and `DESCRIPTOR_HEAP` pipelines.

Size layouts and sequence buffers from `HardwareCapabilities::deviceGeneratedCommands`. Query those cached EXT fields; do not hardcode Required Limits numbers from the NV table.

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
typedef struct VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT
    void*           pNext;
    VkBool32        deviceGeneratedCommands;
    // additional optional bits may exist depending on spec revision
    // (e.g. dynamic vertex input generation, etc.) — enable only when consumed
} VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT;
```

**Already enabled:** master `deviceGeneratedCommands = true`.

### 3.2 Properties (cached)

```cpp
// Key fields (names per VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT)
// maxIndirectCommandsLayoutTokenCount
// maxIndirectCommandsLayoutStreamCount
// maxIndirectSequenceCount
// maxIndirectCommandsTokenOffset
// maxIndirectCommandsStreamStride
// minIndirectCommandsBufferOffsetAlignment
// minSequencesCountBufferOffsetAlignment
// minSequencesIndexBufferOffsetAlignment
// ... plus supported graphics / compute stages mask fields per revision
```

**Already cached** as `HardwareCapabilities::deviceGeneratedCommands`. Not consumed for buffer sizing yet.

### 3.3 Core objects and commands

```cpp
// Create layout from VkIndirectCommandsLayoutCreateInfoEXT
//   - tokens: VkIndirectCommandsLayoutTokenEXT array
//   - stream strides, pipeline bind point, etc.

void vkCmdPreprocessGeneratedCommandsEXT(
    VkCommandBuffer                             commandBuffer,
    const VkGeneratedCommandsInfoEXT*           pGeneratedCommandsInfo);

void vkCmdExecuteGeneratedCommandsEXT(
    VkCommandBuffer                             commandBuffer,
    VkBool32                                    isPreprocessed,
    const VkGeneratedCommandsInfoEXT*           pGeneratedCommandsInfo);
```

`VkGeneratedCommandsInfoEXT` references:

- Indirect commands layout
- Pipeline / shader groups (as required by tokens)
- Sequences buffer / count
- Preprocess buffer (if used)

With descriptor heaps enabled, token types include push-data variants (`VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_DATA_EXT`, etc.).

---

## Limits

Cached in `HardwareCapabilities::deviceGeneratedCommands` (`VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT`). Use the **EXT** field names from that struct — do not copy NV-only names onto EXT.

Cached EXT fields:

| Field | Role |
| --- | --- |
| `maxIndirectPipelineCount` | Pipelines in an indirect execution set |
| `maxIndirectShaderObjectCount` | Shader objects in an indirect execution set (0 = not supported) |
| `maxIndirectSequenceCount` | Sequences in `VkGeneratedCommandsInfoEXT` |
| `maxIndirectCommandsTokenCount` | Tokens in an indirect commands layout |
| `maxIndirectCommandsTokenOffset` | Max token offset |
| `maxIndirectCommandsIndirectStride` | Max stream stride |
| `supportedIndirectCommandsInputModes` | Supported input modes |
| `supportedIndirectCommandsShaderStages` | Stages that can generate / be targeted |
| `supportedIndirectCommandsShaderStagesPipelineBinding` | Indirect pipeline bind stages |
| `supportedIndirectCommandsShaderStagesShaderBinding` | Indirect shader-object bind stages |
| `deviceGeneratedCommandsTransformFeedback` | XFB interaction |
| `deviceGeneratedCommandsMultiDrawIndirectCount` | COUNT multi-draw tokens |

Numeric Required Limits (including `maxIndirectCommandsTokenCount` min **16** in some tables) may differ from NV listings. **See cached properties; do not hardcode.** Official table: [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html).

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep extension name |
| `src/core/vk_device.cpp` | Keep feature + property cache |
| `src/core/types.hpp` | Keep `deviceGeneratedCommands` properties |

### 4.2 Helper module — DONE

`src/render/vk_dgc.cpp` / `.hpp` owns layout creation, sequence/preprocess buffers, and record preprocess/execute. `vk_renderer.cpp` always records DGC. Init throws if heaps/limits/stages cannot support the layout. GPU cull/compact of the sequence buffer is the remaining consumer.

### 4.3 Dependencies in-engine

| Existing system | Role |
| --- | --- |
| `DescriptorManager` + push data | DGC push-data tokens must match heap model |
| Pipeline / future shader objects | Bind tokens |
| Mesh shader (optional) | Mesh draw tokens |
| Multi-draw / indirect buffers | Sequence content source |

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep enablement as-is | `vk_device.*` | Already correct |
| P1 | Log DGC property limits | `vk_device.cpp` | Done |
| P2 | Minimal layout: push data + mesh draw | `src/render/vk_dgc.cpp` | Done — live path is mesh, not indexed |
| P2 | Preprocess + execute path | `vk_dgc.cpp` + `vk_renderer.cpp` | Done |
| P3 | GPU sequence fill / cull | compute pass | Replaces CPU `updateSequences` |
| P3 | Pipeline/shader bind tokens | DGC helper | Multi-material streams |

### Implementation recipe

1. Keep `HardwareCapabilities::deviceGeneratedCommands` cached in `src/core/types.hpp`. Size layouts/sequences from those **EXT** fields (`maxIndirectCommandsTokenCount`, `maxIndirectSequenceCount`, …). See properties; do not hardcode.
2. Do not start DGC until the heap + `pushDataEXT` contract is stable (`vk_descriptors.cpp`, `vk_pipeline.cpp`, `vk_renderer.cpp`). Tokens must match `PushData2` / `MeshPushData` and `DESCRIPTOR_HEAP` pipelines.
3. Add a small helper (e.g. `src/render/vk_dgc.cpp`) that creates an `IndirectCommandsLayoutEXT` with `PUSH_DATA` + `DRAW_INDEXED` (or mesh draw when host mesh is wired).
4. GPU compute fills the sequences buffer; record `preprocessGeneratedCommandsEXT` (optional) then `executeGeneratedCommandsEXT`.
5. Mesh (`mesh.slang` 32 threads / 64 verts / 126 tris) draws only through DGC. Shader-object bind tokens are later work.

---

## 6. Concrete integration sketch

```cpp
// 1) Create IndirectCommandsLayoutEXT with tokens:
//    - PUSH_DATA (object transforms / heap indices)
//    - DRAW_INDEXED
// 2) GPU compute fills sequences buffer (draw counts, push blobs)
// 3) Record:
vk::GeneratedCommandsInfoEXT info{ /* layout, pipeline, sequences, preprocess */ };
if (useExplicitPreprocess) {
    cmd.preprocessGeneratedCommandsEXT(info);
}
cmd.executeGeneratedCommandsEXT(/*isPreprocessed*/ useExplicitPreprocess, info);
```

Size preprocess buffer using property queries / `vkGetGeneratedCommandsMemoryRequirementsEXT` (when available on the device revision).

---

## 7. Validation / test checklist

1. Device creates with DGC extension + feature true.
2. Layout create succeeds for chosen token set.
3. Sequence buffer alignments match property minima.
4. Execute without preprocess (if allowed) and with preprocess both validate cleanly.
5. Push-data tokens agree with heap-bound pipeline (`DESCRIPTOR_HEAP` pipelines).
6. Sequence count ≤ `maxIndirectSequenceCount`.
7. Missing heaps / DGC limits / mesh stages fail at `DeviceGeneratedCommands::init` (no CPU draw fallback).

---

## 8. Shader interaction

**Indirect — GPU writes the command stream that binds state and issues draws/dispatches those shaders already understand.** DGC does not add stages; tokens bind pipeline/shader, push data, bind heaps, and draw/draw-mesh/dispatch. Push-data tokens must match shader push layouts (`PushData2` / `MeshPushData`). A preprocess compute shader often culls and writes DGC/indirect content — ordinary CS SPIR-V.

### Engine status

Consumed for mesh draws. CPU still writes `MeshPushData` + `DrawMeshTasksIndirectCommandEXT` into the sequence buffer. Slang push layouts and heap handles must match that token stream. There is no per-object `pushDataEXT` / `drawMeshTasksEXT` path.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Graphics pipeline / shader object | Bind tokens target these |
| `VK_EXT_descriptor_heap` | Push-data tokens; heap-compatible pipelines |
| `VK_EXT_mesh_shader` | Mesh draw tokens |
| `VK_EXT_shader_object` | Alternate bind tokens vs full pipelines |
| Compute queue / graphics queue | Preprocess vs execute queue family rules per layout |

---

## 10. Non-goals / do not do

- Do not enable every optional DGC feature bit “just in case.”
- Do not generate tokens the layout did not declare.
- Do not skip alignment requirements for sequence/preprocess buffers.
- Do not reintroduce a per-object `pushDataEXT` / `drawMeshTasksEXT` mesh path.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is Device Generated Commands enabled? | **Yes** — extension, feature, properties cached |
| Is it used? | **Yes** — mesh `PUSH_DATA` + `DRAW_MESH_TASKS`, preprocess + execute |
| Highest-value next steps | GPU compute fill / cull of the sequence buffer |
| Secondary steps | Execution-set / shader-object bind tokens |
