# VK_EXT_mesh_shader — Implementation Spec

**Status in this repo: ENABLED (device extension + task/mesh/queries features + properties cached), CONSUMED — mesh+FS pipeline (`mesh.slang` / `createMeshPipeline`) is the only graphics path; dispatched solely via DGC `DRAW_MESH_TASKS` (no host `drawMeshTasksEXT`, no classic `drawIndexed`). Task stage unused.**

Official sources:

- [VK_EXT_mesh_shader](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_mesh_shader.html)
- [Proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_mesh_shader.html)
- [VkPhysicalDeviceMeshShaderFeaturesEXT](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMeshShaderFeaturesEXT.html)
- [VkPhysicalDeviceMeshShaderPropertiesEXT](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMeshShaderPropertiesEXT.html)

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` → `requiredDeviceExtension` | `vk::EXTMeshShaderExtensionName` present |
| Feature enable | `src/core/vk_device.cpp` → feature chain | `taskShader = true`, `meshShader = true` |
| Property query | `src/core/vk_device.cpp` → properties chain | `vk::PhysicalDeviceMeshShaderPropertiesEXT` queried |
| Capability cache | `src/core/types.hpp` → `HardwareCapabilities::meshShader` | Cached |
| API usage | pipeline / renderer | **Consumed** — `createMeshPipeline` + DGC `eDrawMeshTasks`. No `drawIndexed`, no host `drawMeshTasksEXT` |
| Shader source | `shaders/base/mesh.slang` | Mesh + FS with BDA meshlets + descriptor handles |

### Already wired

```cpp
// src/core/vk_device.hpp
vk::EXTMeshShaderExtensionName,

// src/core/vk_device.cpp (feature chain)
// vk::PhysicalDeviceMeshShaderFeaturesEXT
{.taskShader = true, .meshShader = true},

// properties
capabilities.meshShader =
    propertiesChain.get<vk::PhysicalDeviceMeshShaderPropertiesEXT>();

// src/core/types.hpp
vk::PhysicalDeviceMeshShaderPropertiesEXT meshShader;

// src/render/vk_pipeline.cpp — createMeshPipeline (eMeshEXT + fragMain)
// src/render/vk_dgc.cpp — ePushData + eDrawMeshTasks
// src/render/vk_renderer.cpp — dgc.recordPreprocess / recordExecute
```

### Not wired yet

- Task shader stage / amplification
- Host `vkCmdDrawMeshTasksEXT` / `vkCmdDrawMeshTasksIndirectEXT` / `vkCmdDrawMeshTasksIndirectCountEXT` (DGC is the draw path)
- Address-based mesh indirect (`vkCmdDrawMeshTasksIndirect2EXT`)
- Queries against `maxWorkGroup*`, payload sizes, output limits from properties

---

## 2. What the extension provides

GPU mesh pipeline: **task** (amplification) and **mesh** shader stages replace traditional vertex input assembly for selected draws. Mesh shaders emit vertices/primitives directly; task shaders can dispatch mesh workgroups.

| Feature | New API / bit | Engine relevance |
| --- | --- | --- |
| Stage flags | `VK_SHADER_STAGE_TASK_BIT_EXT`, `VK_SHADER_STAGE_MESH_BIT_EXT` | Pipeline stage create |
| Direct draw | `vkCmdDrawMeshTasksEXT` | Basic mesh path |
| Indirect | `vkCmdDrawMeshTasksIndirectEXT` / `IndirectCount` | GPU-driven |
| Features | `taskShader`, `meshShader`, optional multiview/queries bits | Enablement |
| Properties | workgroup sizes, output limits, payload sizes | Shader compile + validation gates |

This is a programmable geometry pipeline, not a replacement for fragment shading or descriptor binding. Mesh workgroups load geometry from buffers (this engine: BDA meshlet tables) and emit a bounded vertex/primitive list; the existing heap + `pushDataEXT` path still supplies camera/object addresses and texture handles.

Author shaders and host dispatches against `HardwareCapabilities::meshShader`. Workgroup size, output counts, and payload sizes are hardware limits. Engine meshlet constants (64 verts / 126 tris / 32 threads in `shaders/base/mesh.slang`) sit safely under the spec 256 / 256 / 128 minima.

---

## 3. Structures and commands

### 3.1 Feature (enable at device create)

```cpp
typedef struct VkPhysicalDeviceMeshShaderFeaturesEXT {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT
    void*           pNext;
    VkBool32        taskShader;
    VkBool32        meshShader;
    VkBool32        multiviewMeshShader;              // optional
    VkBool32        primitiveFragmentShadingRateMeshShader; // optional
    VkBool32        meshShaderQueries;                // optional
} VkPhysicalDeviceMeshShaderFeaturesEXT;
```

**Currently enabled:** `taskShader` and `meshShader` only. Optional bits remain default/false unless explicitly needed.

### 3.2 Properties (cached)

```cpp
typedef struct VkPhysicalDeviceMeshShaderPropertiesEXT {
    VkStructureType sType; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT
    void*           pNext;
    uint32_t        maxTaskWorkGroupTotalCount;
    uint32_t        maxTaskWorkGroupCount[3];
    uint32_t        maxTaskWorkGroupInvocations;
    uint32_t        maxTaskWorkGroupSize[3];
    uint32_t        maxTaskPayloadSize;
    uint32_t        maxTaskSharedMemorySize;
    uint32_t        maxTaskPayloadAndSharedMemorySize;
    uint32_t        maxMeshWorkGroupTotalCount;
    uint32_t        maxMeshWorkGroupCount[3];
    uint32_t        maxMeshWorkGroupInvocations;
    uint32_t        maxMeshWorkGroupSize[3];
    uint32_t        maxMeshSharedMemorySize;
    uint32_t        maxMeshPayloadAndSharedMemorySize;
    uint32_t        maxMeshOutputMemorySize;
    uint32_t        maxMeshPayloadAndOutputMemorySize;
    uint32_t        maxMeshOutputComponents;
    uint32_t        maxMeshOutputVertices;
    uint32_t        maxMeshOutputPrimitives;
    uint32_t        maxMeshOutputLayers;
    uint32_t        maxMeshMultiviewViewCount;
    uint32_t        meshOutputPerVertexGranularity;
    uint32_t        meshOutputPerPrimitiveGranularity;
    uint32_t        maxPreferredTaskWorkGroupInvocations;
    uint32_t        maxPreferredMeshWorkGroupInvocations;
    VkBool32        prefersLocalInvocationVertexOutput;
    VkBool32        prefersLocalInvocationPrimitiveOutput;
    VkBool32        prefersCompactVertexOutput;
    VkBool32        prefersCompactPrimitiveOutput;
} VkPhysicalDeviceMeshShaderPropertiesEXT;
```

**Already cached** as `HardwareCapabilities::meshShader`. Not yet used for shader authoring gates.

### 3.3 Draw commands

```cpp
void vkCmdDrawMeshTasksEXT(
    VkCommandBuffer commandBuffer,
    uint32_t        groupCountX,
    uint32_t        groupCountY,
    uint32_t        groupCountZ);

void vkCmdDrawMeshTasksIndirectEXT(
    VkCommandBuffer commandBuffer,
    VkBuffer        buffer,
    VkDeviceSize    offset,
    uint32_t        drawCount,
    uint32_t        stride);

void vkCmdDrawMeshTasksIndirectCountEXT(
    VkCommandBuffer commandBuffer,
    VkBuffer        buffer,
    VkDeviceSize    offset,
    VkBuffer        countBuffer,
    VkDeviceSize    countBufferOffset,
    uint32_t        maxDrawCount,
    uint32_t        stride);
```

Indirect command structure is `VkDrawMeshTasksIndirectCommandEXT` (`groupCountX/Y/Z`).

---

## Limits

Cached in `HardwareCapabilities::meshShader` (`VkPhysicalDeviceMeshShaderPropertiesEXT`). Spec minima when `meshShader` is supported ([Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html)):

| Property | Required limit (meshShader supported) |
| --- | --- |
| `maxTaskWorkGroupTotalCount` | 2^22 |
| `maxTaskWorkGroupCount` | (65535, 65535, 65535) |
| `maxTaskWorkGroupInvocations` | 128 |
| `maxTaskWorkGroupSize` | (128, 128, 128) |
| `maxTaskPayloadSize` | 16384 |
| `maxTaskSharedMemorySize` | 32768 |
| `maxTaskPayloadAndSharedMemorySize` | 32768 |
| `maxMeshWorkGroupTotalCount` | 2^22 |
| `maxMeshWorkGroupCount` | (65535, 65535, 65535) |
| `maxMeshWorkGroupInvocations` | 128 |
| `maxMeshWorkGroupSize` | (128, 128, 128) |
| `maxMeshSharedMemorySize` | 28672 |
| `maxMeshPayloadAndSharedMemorySize` | 28672 |
| `maxMeshOutputMemorySize` | 32768 |
| `maxMeshPayloadAndOutputMemorySize` | 48128 |
| `maxMeshOutputComponents` | 128 |
| `maxMeshOutputVertices` | 256 |
| `maxMeshOutputPrimitives` | 256 |
| `maxMeshOutputLayers` | 8 |
| `maxMeshMultiviewViewCount` | 1 |
| `meshOutputPerVertexGranularity` | **max 32** |
| `meshOutputPerPrimitiveGranularity` | **max 32** |
| `maxPreferredTaskWorkGroupInvocations` / `maxPreferredMeshWorkGroupInvocations` | implementation-dependent |
| `prefersLocalInvocationVertexOutput` / `prefersLocalInvocationPrimitiveOutput` / `prefersCompactVertexOutput` / `prefersCompactPrimitiveOutput` | implementation-dependent |

Engine meshlet constants in `shaders/base/mesh.slang` — 64 verts / 126 tris / 32 threads — are safely under 256 / 256 / 128. Clamp build and dispatch against the **cached** properties, not only these minima.

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep `vk::EXTMeshShaderExtensionName` |
| `src/core/vk_device.cpp` | Keep feature + property cache |
| `src/core/types.hpp` | Keep `HardwareCapabilities::meshShader` |

### 4.2 Pipeline — DONE (mesh+FS, no task)

**File:** `src/render/vk_pipeline.cpp` `createMeshPipeline`

- Mesh + fragment stages (`eMeshEXT` / `fragMain` from `mesh.spv`)
- No vertex input
- Heap flags + `MeshPushData` push range

### 4.3 Renderer draw — DONE via DGC (not host `drawMeshTasksEXT`)

**File:** `src/render/vk_renderer.cpp` + `src/render/vk_dgc.cpp`

Bind heaps, preprocess, `executeGeneratedCommandsEXT`. No `drawIndexed`. No host `drawMeshTasksEXT`.

### 4.4 Content / tooling

- Meshlet builders, task payloads, and shader sources (out of scope until product decision)
- Gate workgroup sizes on `capabilities.meshShader`

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep enablement + property cache | `vk_device.*`, `types.hpp` | Already correct |
| P1 | Log mesh property limits at init | `vk_device.cpp` | Debuggability |
| P2 | First mesh-only pipeline (no task) | `vk_pipeline.cpp` + shaders | **Done** |
| P2 | Mesh draw | `vk_dgc.cpp` / `vk_renderer.cpp` | **Done** (DGC `DRAW_MESH_TASKS`, not host `drawMeshTasksEXT`) |
| P3 | Task shader amplification | shaders + pipeline | LOD / culling |
| P3 | Host `drawMeshTasksEXT` fallback | renderer | Not needed while DGC is the path |

### Implementation recipe

1. Keep `HardwareCapabilities::meshShader` cached in `src/core/types.hpp` (filled from the properties chain in `vk_device.cpp`). Gate workgroup / output constants on that struct, not on guessed numbers.
2. Keep `shaders/base/mesh.slang` at `[numthreads(32,1,1)]`, `out vertices MeshVertexOut verts[64]`, `out indices uint3 tris[126]` — under spec 256 / 256 / 128. Geometry is BDA meshlet tables in `MeshPushData`; textures are `DescriptorHandle`.
3. Mesh+FS pipeline already exists in `src/render/vk_pipeline.cpp` (`eMeshEXT` + fragment, **no** vertex input). Heap mode: `eDescriptorHeapEXT` + `MeshPushData` (72 B).
4. Draw is DGC only: `vk_renderer.cpp` binds heaps, `vk_dgc.cpp` preprocess + `executeGeneratedCommandsEXT`. Do not add host `drawMeshTasksEXT` unless a fallback is required.
5. Do not add a task stage until payload size is checked against `maxTaskPayloadSize`.

---

## 6. Concrete integration sketch (pipeline + draw)

```cpp
vk::PipelineShaderStageCreateInfo meshStage{
    .stage = vk::ShaderStageFlagBits::eMeshEXT,
    .module = *meshModule,
    .pName = "main",
};
// optional taskStage with eTaskEXT

// GraphicsPipelineCreateInfo: pVertexInputState = nullptr for pure mesh
// stages = { task?, mesh, fragment }

// Record:
cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *meshPipeline);
cmd.bindResourceHeapEXT(resourceHeapInfo);
cmd.bindSamplerHeapEXT(samplerHeapInfo);
cmd.pushDataEXT(...);
cmd.drawMeshTasksEXT(groupCountX, 1, 1);
```

Respect property caps:

```cpp
const auto& ms = device.capabilities.meshShader;
// groupCountX <= ms.maxMeshWorkGroupCount[0], etc.
```

---

## 7. Validation / test checklist

1. Device creates with mesh extension + task/mesh features true.
2. Capability dump shows workgroup/output limits.
3. Mesh pipeline create succeeds; no vertex bindings required.
4. `drawMeshTasksEXT` produces geometry without validation errors.
5. Output vertex/primitive counts within `maxMeshOutput*` and granularities.
6. Task payload size ≤ `maxTaskPayloadSize` when task stage used.
7. Coexistence: classic vertex pipeline still works in other passes.

---

## 8. Shader interaction

**Direct — primary programmable stages for this extension.** Mesh shaders replace the classic vertex-input assembly path for selected draws. This engine authors them in **Slang** (`shaders/base/mesh.slang`); the host must enable `taskShader` / `meshShader`, create a graphics pipeline with mesh (and optional task) stages, and dispatch with `vkCmdDrawMeshTasks*EXT`.

### Stages and SPIR-V

| Stage | Stage bit | SPIR-V / language | Engine use |
| --- | --- | --- | --- |
| Task (amplification) | `VK_SHADER_STAGE_TASK_BIT_EXT` | Task execution model; `OpEmitMeshTasksEXT` | **Not used yet** (mesh-only path) |
| Mesh | `VK_SHADER_STAGE_MESH_BIT_EXT` | Mesh execution model; `OpSetMeshOutputsEXT` | `meshMain` in `mesh.slang` |
| Fragment | standard FS | Consumes mesh per-vertex / per-primitive outputs | `fragMain` in same file |

Key mesh rules (Vulkan + SPIR-V):

- Declare max outputs via execution modes (`OutputVertices`, `OutputPrimitivesEXT`) — Slang `[outputtopology("triangle")]` + sized `out vertices` / `out indices` arrays.
- Call `SetMeshOutputCounts(vertexCount, primitiveCount)` **once** (maps to `OpSetMeshOutputsEXT`) before writing outputs; counts must be ≤ declared max and within `maxMeshOutputVertices` / `maxMeshOutputPrimitives`.
- Workgroup size = `[numthreads(X,Y,Z)]` / local size; must fit `maxMeshWorkGroupSize` / `maxMeshWorkGroupInvocations`.
- **No vertex input state** on the pipeline; geometry is loaded from buffers (this engine: **BDA** in push data).
- Optional task stage can pass a payload (size ≤ `maxTaskPayloadSize`) into mesh workgroups.

### Engine Slang shape (mesh-only, one WG per meshlet)

```slang
// shaders/base/mesh.slang (summary)
struct MeshPushData {
    uint64_t cameraAddress, objectUbAddress;
    uint64_t vertices, meshlets, meshletVertices, meshletTriangles;
    uint firstMeshlet, meshletCount;
    DescriptorHandle<Texture2D> texture;
    DescriptorHandle<SamplerState> samplerHandle;
};
[[vk::push_constant]] ConstantBuffer<MeshPushData> push;

[shader("mesh")]
[numthreads(32, 1, 1)]
[outputtopology("triangle")]
void meshMain(
    uint gtid : SV_GroupThreadID,
    uint gid  : SV_GroupID,
    out vertices MeshVertexOut verts[64],
    out indices uint3 tris[126])
{
    // load MeshletDesc via BDA, SetMeshOutputCounts, cooperative vertex/index write
}

[shader("fragment")]
float4 fragMain(MeshVertexOut v) : SV_TARGET {
    return getDescriptorFromHandle(push.texture)
        .Sample(getDescriptorFromHandle(push.samplerHandle), v.fragTexCoord);
}
```

Host dispatch (when wired): `drawMeshTasksEXT(meshletCount, 1, 1)` with `MeshPushData` via `pushDataEXT`. Geometry BDAs are **static** (one address per buffer); only camera/object UBO addresses are per-frame-in-flight.

### Property gates for shader authors

Clamp meshlet build and shader constants against `HardwareCapabilities::meshShader`:

- `kMaxMeshletVertices` (64) ≤ `maxMeshOutputVertices`
- `kMaxMeshletTriangles` (126) ≤ `maxMeshOutputPrimitives`
- `kGroupSize` (32) ≤ `maxMeshWorkGroupInvocations` and preferred sizes when available
- Respect `meshOutputPerVertexGranularity` / `meshOutputPerPrimitiveGranularity` if non-1

### Status in repo

| Piece | State |
| --- | --- |
| Device features + properties | Enabled / cached |
| `mesh.slang` mesh + FS | Present (meshlet BDA path) |
| meshopt meshlet build | `assets_loader.cpp` |
| Pipeline stage + DGC `DRAW_MESH_TASKS` | **Live** — no host `drawMeshTasksEXT`, no `drawIndexed` |

Cross-refs: [VK_KHR_buffer_device_address](./VK_KHR_buffer_device_address.md), [VK_EXT_descriptor_heap](./VK_EXT_descriptor_heap.md), [VK_KHR_fragment_shading_rate](./VK_KHR_fragment_shading_rate.md) (`primitiveFragmentShadingRateMeshShader`).

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Fragment shader / dynamic rendering | Still needed for shading (enabled) |
| `VK_EXT_descriptor_heap` | Same bind/push model for mesh pipelines |
| `VK_KHR_device_address_commands` | Address-based mesh indirect variants |
| `VK_EXT_device_generated_commands` | Can generate mesh draws |
| `VK_KHR_fragment_shading_rate` | Optional `primitiveFragmentShadingRateMeshShader` |

---

## 10. Non-goals / do not do

- Do not remove the classic vertex path until mesh content is production-ready.
- Do not enable optional feature bits (`meshShaderQueries`, multiview mesh) without a consumer.
- Do not ignore property limits when authoring mesh shaders — portability hazard.
- Do not mix vertex input assembly with mesh stages in the same pipeline.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Is Mesh Shader enabled? | **Yes** — extension, `taskShader`/`meshShader`, properties cached |
| Is it used? | **Yes** — mesh+FS pipeline + DGC `DRAW_MESH_TASKS`; no host `drawMeshTasksEXT` |
| Highest-value next steps | Mesh pipeline + `MeshPushData` push + `drawMeshTasksEXT` |
| Secondary steps | Task amplification; indirect GPU-driven mesh draws |
| Shader interaction | See **§ 8. Shader interaction** (stages, Slang shape, property gates) |
