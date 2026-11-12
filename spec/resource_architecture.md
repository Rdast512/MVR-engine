# Resource architecture

**Date:** 2026-10-24, updated 2026-11-11
**Status:** §1–§11 describe the working tree on `7dc04f965`; §12 lists the resource changes the target architecture requires. Living document: update it in the same change that alters resource code, and log the change in [Changelog](#changelog).
**Target:** [engine_spec.md](../docs/engine_spec.md) is normative for the end state. Where this document describes code that the spec removes or replaces, §12 says so.
**Related:** [shader_architecture_plan.md](./shader_architecture_plan.md), [pbr_implementation_plan.md](./pbr_implementation_plan.md), [culling_comparison.md](../reference/culling_comparison.md)
**Code:** `src/core/vk_asset_buffers.*`, `src/core/vk_frame_data.*`, `src/core/vk_frame_sync.*`, `src/render/vk_render_targets.*`, `src/core/types.hpp`, `src/core/geometry_store.*`, `src/render/push_data.hpp`, `src/core/texture_manager.*`, `src/core/vk_descriptors.*`, `src/render/vk_dgc.*`, `src/render/vk_meshlet_culling.*`, `src/render/vk_hiz.*`, `src/util/vk_utils.*`, `shaders/types.slang`, `shaders/core/{frame,binding}.slang`

How the engine owns GPU memory, how asset data gets from a glTF file into GPU buffers, and how shaders reach it.

---

## 1. Overview

```
load:   AssetsLoader ──► scratch (GeometryStore, MaterialStore new rows, LightStore)
                     ──► AssetBuffers::flush  (one model per flush, one staging submit)
                           ├─ 23 device-local asset buffers: 20 sliced per model; shared: Materials, PbrExt; per entity: Lights
                           └─ GpuModel table: slice addresses per model
        TextureManager ──► images (host image copy) ──► resource-heap slots
                           samplers (dedup)          ──► sampler-heap slots

frame:  MeshletCulling::prepareFrame ──► GpuCullView per phase, templates on layout change
        FrameData::write             ──► GpuObjectUB[entity], GpuNodeInstance[entity × node], GpuFrameRoot
        GPU: cull passes ──► GpuDrawRecord ──► GpuVisibleMeshlet lists ──► DGC sequences + counts

shader: MeshPushData {frameRoot, firstVisible} ──► GpuVisibleMeshlet {nodeInstance, meshletRow}
          ──► GpuNodeInstance ──► GpuModel ──► MeshletPrimitives[meshletRow] ──► GpuPrimitive ──► material ──► heap indices
```

Two access mechanisms only (spec P3):

| Kind | CPU handle | Shader access |
|---|---|---|
| Buffer payload | `vk::DeviceAddress` (BDA) + row index | `bdaPtr<T>(address)[i]` |
| Image / sampler | `uint32` heap index | `tex2D(i)`, `samplerOf(i)` → `DescriptorHandle` |

Payloads never live in descriptor heaps; heaps never hold vertex or material bytes. No descriptor sets are used except ImGui's own pool.

---

## 2. Resource kinds

| Kind | Owner / type | Memory | Count | Lifetime |
|---|---|---|---|---|
| Asset buffer | `AssetBuffers::assetBuffers[AssetBuffer]`, `DeviceBuffer` | device-local | 23, shared | grows on load, compacts on unload |
| Model table | `AssetBuffers::modelTable`, `HostBuffer` | host-visible, mapped | 1 | rows rewritten in place on flush and unload; grows ×2 |
| Frame root | `FrameData::frameRoots`, `HostBuffer` | host-visible, mapped | 1 per frame slot | rewritten every frame |
| Instance rows (`GpuObjectUB`, `GpuNodeInstance`) | `FrameData::instanceUbos`, `nodeInstanceBuffers` | host-visible, mapped | 1 each per frame slot | rewritten every frame; grow ×2 |
| Camera | `Camera` (`createBuffers`) | host-visible, `vmaMapMemory` | 1 per frame slot | rewritten every frame |
| Cull view, counters, instance items, draw-record templates | `MeshletCulling::FrameBuffers`, `HostBuffer` | host-visible, mapped | 1 each per frame slot | view every frame; templates on layout change |
| Draw records, instance candidates, reject masks, visible lists | `MeshletCulling::FrameBuffers`, `DeviceBuffer` | device-local | 1 each per frame slot, one region per phase | GPU-written; resized on layout change |
| DGC sequences | `DeviceGeneratedCommands` | device-local | 1 per frame slot | GPU-written by `buildMeshSequences` |
| DGC preprocess | `DeviceGeneratedCommands` | device-local, dedicated | 1 per frame slot, one region per (phase, state group) | resized with the sequence capacity |
| Descriptor heaps | `DescriptorManager` | host-visible, dedicated, `vmaMapMemory` | resource + sampler | engine lifetime |
| Texture | `TextureManager::loadedTextures` → `TextureAsset` | device-local image | per unique image | refcounted per model |
| Depth | `RenderTargets::depthAttachment`, `AttachmentImage` | device-local image | 1 | recreated on resize |
| Hi-Z pyramid | `HiZPyramid` | device-local image | 1 | recreated on resize |
| Staging | local in `uploadAssetBuffers` | host-visible, `TransferSrc` only | 1 per batch | destroyed after the copy |

`MAX_FRAMES_IN_FLIGHT = 2` (`src/Constants.h`). The CPU writes slot `currentFrame` only after that slot's fence has signalled. The cull counters live in host-visible VRAM: atomics stay on the device and the CPU reads them after the fence.

---

## 3. Memory and allocation

All buffers go through `createBuffer` (`src/util/vk_utils.cpp`); images through `VkAllocator::createImage2D` (`src/core/vk_allocator.hpp`).

| Request | VMA policy |
|---|---|
| `eDeviceLocal` buffer | `AUTO_PREFER_DEVICE`, required `DEVICE_LOCAL`, priority 0.75, created unbound, named, then bound |
| `eHostVisible` buffer | `AUTO`, required `HOST_VISIBLE`, preferred `HOST_VISIBLE \| HOST_COHERENT`, `HOST_ACCESS_SEQUENTIAL_WRITE`, priority 0.25 |
| Image | optimal tiling, exclusive sharing; priority 1.0 for attachments, 0.9 otherwise |

- **Sharing:** buffers are `CONCURRENT` whenever more than one queue family is in use, so transfer-queue uploads need no ownership transfer. Images are `EXCLUSIVE` and touched only by the graphics queue.
- **ReBAR:** the engine targets ReBAR PCs (spec §3). Host-visible buffers do not request a memory type explicitly; VMA may place them in host-visible VRAM.
- **Host writes:** write whole structs front to back; never read mapped memory back, except the cull counters after their fence.
- **Mapping style decides teardown:** `MAPPED_BIT` buffers (`HostBuffer`) are unmapped by VMA. Buffers mapped with `vmaMapMemory` (camera, heaps) must be unmapped before `destroyVmaBuffer`, or VMA asserts `m_MapCount == 0`. Never mix the two on one allocation.
- **Teardown order:** unmap (explicit maps only) → reset image views → `destroyVmaBuffer` / `destroyVmaImage` with the same Tracy name used at allocation. `Engine::cleanup()` resets every module that owns allocations before `allocator.reset()`.
- **Limits:** capacities come from queried device properties (`HardwareCapabilities`), never fixed numbers (spec §3, S11).

### Naming

| Thing | Convention | Example |
|---|---|---|
| Vulkan object | PascalCase noun, `_N` per frame slot | `InstanceObjectUB_1` |
| VMA allocation | object name + `Memory`; `createBuffer` appends `_<id>` | `VertexBufferMemory_12` |
| Tracy memory pool | `GPU/<Plural>`, a string literal | `GPU/Vertices` |
| Tracy plot | `Vulkan/<Name>Bytes`, `…CapacityBytes`, `…Count`, string literals | `Vulkan/MeshletBytes` |

---

## 4. Asset buffers

`enum class AssetBuffer` (`vk_asset_buffers.hpp`) indexes `assetBuffers` and the metadata table `kAssetBufferInfo` (`vk_asset_buffers.cpp`). Rows must follow the enum order; nothing checks it at compile time, and a swapped row corrupts every stride-based count.

Usage for all: `TransferSrc | TransferDst | StorageBuffer | ShaderDeviceAddress`, device-local.

| `AssetBuffer` | Element (stride) | Contents | Shader access today |
|---|---|---|---|
| `Vertices` | `GpuVertex` (32 B) | pos, color, uv0 | mesh shader via `GpuModel.vertices` |
| `Meshlets` | `GpuMeshletDesc` (32 B) | vertex/triangle offsets and counts, snorm8 normal cone, sphere | mesh shader; cull (cone) |
| `MeshletBounds` | `GpuMeshletBounds` (32 B) | object-space box + shortest/longest edge, parallel to `Meshlets` | cull |
| `MeshletGroups` | `GpuMeshletGroup` (32 B) | union box, first meshlet (primitive-relative), count ≤ `kMeshletGroupMaxSize` (32) | cull |
| `MeshletPrimitives` | `uint32` | per meshlet: row of the model's primitive slice | mesh shaders (`loadDrawContext`) |
| `MeshletVertices` | `uint32` | model-relative vertex ids | mesh shader |
| `MeshletTriangles` | `uint8` | local corner indices | mesh shader |
| `Normals` | `float3` (12 B) | uploaded only if a primitive provides it | reachable, unused |
| `Tangents` | `float4` (16 B) | xyz + handedness; same rule | reachable, unused |
| `Uv1` | `float2` (8 B) | same rule | reachable, unused |
| `Indices` | `uint32` | model-relative vertex ids | reachable, unused |
| `Materials` | `GpuMaterial` (240 B) | factors (emissive strength folded in), flags, 5 texture + 5 sampler heap indices, 5 UV sets, 5 UV matrices | fragment via `loadMaterial`; `DoubleSided` flag read by `loadDrawContext` |
| `PbrExt` | `GpuMaterialExtension` (724 B) | glTF extension rows, parallel to `Materials` | `loadMaterialExtension`, no consumer yet |
| `Lights` | `GpuLight` (64 B) | model-space punctual lights + owning `instanceIndex` | `loadWorldLight`, no consumer yet |
| `Primitives` | `GpuPrimitive` (36 B) | one row per **mesh** primitive: meshlet/vertex/index/group ranges, `materialId`, model-relative | mesh + fragment via `loadDrawContext`; cull |
| `Nodes` | `GpuNode` (112 B) | mesh placements: affine `localToModel` rows, model-space sphere, mesh-space box, primitive range, `NodeFlag` | instance cull (box) |

`DeviceBuffer` = `{buffer, memory, address, usedBytes, capacityBytes}`. `uploadedCount(id) = usedBytes / stride`.

### Growth

`uploadAssetBuffers(uploads)` takes `{id, bytes, dstOffset}` per buffer (append: `dstOffset = usedBytes`; overwrite: any offset):
1. One host-visible staging buffer holds every upload at 16 B aligned offsets.
2. One command buffer records, per upload: if `dstOffset + bytes ≤ capacity`, a copy into the existing buffer; otherwise a new buffer of `nextCapacity` (exact on first create, then ×2) receives the old bytes the upload does not overwrite plus the staging data. Copies into one buffer never overlap, so no barriers are needed.
3. One `submitAndWait` on the transfer queue (dedicated family when available), then `installAssetBuffer` swaps in every grown buffer and caches its address.

Optional streams (`Normals`, `Tangents`, `Uv1`) are uploaded only when `GeometryStore::presentStreams` has their `GpuModelAttribute` bit; the loader sets it per accessor. Absent streams stay zero-filled in CPU scratch but never reach the GPU.

**Addresses change on growth and on unload.** Never cache an asset address across a flush or an unload; read it from `asset(id).address` / `modelAssetAddress(id, entity)` when building frame data.

---

## 5. Load path

`Engine::loadSelectedModel` first resolves the folder to its model file (`AssetsLoader::resolveModelPath`, weakly canonical) and looks it up with `AssetBuffers::findModel`. A hit calls `instantiateModel`: a new entity that shares the model's geometry, materials and textures; only its light rows are uploaded. A miss runs `waitIdle` → `AssetsLoader::loadModel` → `AssetBuffers::flush` → `FrameData::syncScene`. Startup does the same through the `AssetBuffers` and `FrameData` constructors.

1. **Parse.** The loader fills `GeometryStore` scratch (SoA streams, packed `GpuVertex`, meshlets, indices, `textureRefs`), adds materials to `MaterialStore` (dedup by value against every live row, §6.1), fills `LightStore`, and creates the entity in `ObjectStorage` with `modelIds = kInvalidModelId`. Scratch is empty before each load, so scratch offsets are already model-relative. Geometry clean-up uses meshoptimizer: triangle lists drop degenerate and repeated triangles (`meshopt_filterIndexBuffer`) and every bounding sphere is `meshopt_computeSphereBounds`.
2. **Meshlets and groups** (`GeometryStore::buildMeshlets`, level 0 of `clusterlod.h`'s `clodDefaultConfig`, spec §7.1):
   - `meshopt_buildMeshletsFlex` (64 vertices, 126 triangles, min triangles = max / 3, cone weight 0, split factor 2), then `meshopt_optimizeMeshletLevel` level 1 per meshlet;
   - per meshlet: `GpuMeshletDesc` (sphere + snorm8 cone from `meshopt_computeMeshletBounds`) and `GpuMeshletBounds` (box and edge lengths from its triangle corners);
   - per primitive: position remap, spatial `meshopt_partitionClusters` with target 24 (its +1/3 worst case fits 32), groups spatially sorted, meshlets reordered so each group is contiguous, groups over 32 split. A primitive with ≤ 24 meshlets is one group. Groups never span primitives;
   - per node: the mesh-space box of the mesh's vertex range (`NodeDraw::meshBounds`).
3. **Textures.** Uploaded during parse inside a `TextureUploadBatch` (§8).
4. **Flush.** `AssetBuffers::flush`:
   - throws `std::logic_error` if more than one entity is pending (`modelIds == kInvalidModelId`) or it is not the newest; discards scratch if none is;
   - packs `GpuPrimitive` rows from `primitiveDraws` **before** the remap, so they stay model-relative. `materialId` stays the absolute shared row, or `kNoneIndex` for the default material;
   - packs `GpuNode` rows from `nodeDraws` (`firstPrimitive` made model-relative);
   - `remapScratchOffsets` makes only the **CPU catalog** absolute: `PrimitiveDraw` first vertex/index/meshlet/group, `MorphTarget` offsets, `ObjectStorage::meshletDraws`. `NodeDraw::firstPrimitive` is catalog-absolute from the start. `GeometryStore::meshletGroups` stays as a CPU catalog (watermark and unload rebase like the primitives), because the cull templates are rebuilt from it;
   - one `uploadAssetBuffers` batch (one staging buffer, one submit + idle) for the 20 model-sliced buffers plus `MaterialStore::dirtyRows`, written in place at `row × stride`, one copy per run of consecutive rows (`appendMaterialUploads`);
   - builds the `ModelAllocation`: slices, node range, cache key, union `meshletDraw`, model-space bounding sphere (`GeometryStore::nodeBoundingSphere`), the model-space light template, `materialRefs` (`MaterialStore::commitModel`) and `textureRefs`;
   - the default material (row 0 of `Materials`) is dirty at startup, uploaded with the first flush, and belongs to no model;
   - `attachEntity` sets `modelIds` / `localBounds`, bumps the model's `refCount` and appends the entity's light rows; `uploadLights`, `clearScratch`, `rebuildModelTable`, `mi_collect`.

---

## 6. Model slices and unload

`ModelAllocation` (`vk_asset_buffers.hpp`) is one uploaded model file, shared by every entity loaded from it. It holds one `ElementRange` per model-sliced asset buffer (`isModelSliced`: all except `Materials`, `PbrExt`, `Lights`), CPU ranges (primitives, groups, nodes, aux blobs), the cache `key`, the light template, `materialRefs`, `textureRefs` and `refCount` (entities drawing it). `modelAllocations` (read through `AssetBuffers::models()`) is indexed by `ModelId`; `ObjectStorage::modelIds[entity]` and `GpuObjectUB::modelIndex` point into it.

### 6.1 Reference counts

| Resource | Holder | Count | Freed when |
|---|---|---|---|
| model (geometry slices, primitives) | entities | `ModelAllocation::refCount` | its last entity is erased |
| material row | models | `MaterialStore::MaterialInfo::refCount` | the last model holding it is erased; the slot goes on a free list and is reused, ids never shift |
| texture | models | `TextureManager` heap entry `refCount` | the last model holding it is erased (`TextureManager::release`) |

Material rows are deduplicated by value (`GpuMaterial` + `MaterialPbrExtension`, hashed buckets) against every live row, so two models with identical materials share rows. Row 0 (default) is permanent and not counted. Texture equality follows the texture cache key (file path or embedded image key), so identical materials across models also need shared textures to match.

**Rule:** data that points into other GPU data counts from the start of its own model's slice. Rows the CPU rewrites or erases in step with a CPU list are absolute.

| Value | Relative to |
|---|---|
| index values, meshlet vertex ids | model's vertex slice |
| `GpuMeshletDesc::vertexOffset` / `triangleOffset` | model's meshlet-vertex / meshlet-triangle slice |
| `GpuMeshletGroup::firstMeshlet` | its primitive's `firstMeshlet` |
| `MeshletPrimitives[i]`, `GpuVisibleMeshlet::meshletRow` | model's primitive slice / model's meshlet slice |
| `GpuPrimitive` ranges (including `firstGroup`) | model's slices |
| `GpuPrimitive::materialId` | absolute shared row, never shifts |
| `GpuNode::firstPrimitive` | model's primitive slice |
| `NodeDraw::firstPrimitive`, `GpuDrawRecord::primitiveIndex`, row in `GpuFrameRoot.primitives` | absolute |
| `GpuDrawRecord::meshletOffset` / `groupOffset` | the primitive's first meshlet / first group |
| `GpuNodeInstance` rows (`entityNodeInstanceFirst[entity] + node`) | absolute, rebuilt on every load / instance / erase |
| entity id: rows in `instances`, `GpuLight::instanceIndex` | absolute, shift down on entity erase |
| model id: rows in `models`, `modelAllocations`, `GpuObjectUB::modelIndex` | absolute, shift down on model erase |

`meshletRow` is the engine's cluster id (spec §8.3, §10.1): stable across draw records, phases and, later, LOD levels.

Because slices are self-relative, unloading is a plain byte copy:

`Engine::unloadModel`: `waitIdle` → `TextureManager::release(AssetBuffers::eraseEntity(entity))` → `FrameData::syncScene`. `eraseEntity`:
1. Lights: erase the entity's rows from `LightStore::uploaded`, decrement every `instanceIndex` above it; erase the entity.
2. Decrement the model's `refCount`. At 0, `eraseModel`:
   - `compactAssetBuffers`: for every model-sliced buffer the model touched, an exact-fit new buffer receives the data before and after the slice, all in one submit + idle. A buffer left empty is destroyed (address 0);
   - `rebaseCatalog`: erases the model's CPU ranges and its `ModelAllocation`, shifts later references and later model ids;
   - `MaterialStore::release(materialRefs)`;
   - `rebuildModelTable` (in place);
   - returns `textureRefs`, which the engine releases.
3. `uploadLights`: rewrites the `Lights` buffer from the mirror and sets its used bytes to the mirror size (one more submit).

Every flush, `instantiateModel` and `eraseEntity` bumps `AssetBuffers::sceneGeneration`; `FrameData::syncScene` then grows the instance rows, runs `rebuildNodeInstanceLayout` and bumps `FrameData::layoutGeneration`; `MeshletCulling` then rebuilds its templates and drops occlusion for one frame (`OcclusionHistory::SceneChanged`).

Erasing an entity whose model has other entities touches only the light buffer and the node-instance layout. After a model erase, every buffer the model used, every later slice address and every later model id change. Buffers the model did not use keep their address; the material table never moves rows; the model table keeps its address unless it grew.

`GpuModel` (112 B, rebuilt by `rebuildModelTable`): slice addresses for vertices, meshlets, meshletBounds, meshletGroups, meshletPrimitives, meshletVertices, meshletTriangles, normals, tangents, uv1, indices, nodes, and `primitives` (the model's slice of `GpuFrameRoot::primitives`), plus `attributeMask` (`GpuModelAttribute::*`) and `nodeCount`. An empty slice has address 0 and a clear bit.

### 6.2 Meshes and nodes

A glTF mesh is stored **once** per model: its primitives, vertices and meshlets stay in mesh space. Every scene node that references it becomes a `NodeDraw` / `GpuNode` with its world matrix inside the model (`localToModel`), a model-space sphere, a mesh-space box and the mesh's primitive range. `EXT_mesh_gpu_instancing` expands into one node per instance (`node world × instance TRS`); such a node draws only its instances.

Draw instance = entity × node. `rebuildNodeInstanceLayout` assigns rows entity-major (`entityNodeInstanceFirst`) and grows the per-frame `GpuNodeInstance` buffers; it runs from `FrameData::syncScene` after every flush, `instantiateModel` and `eraseEntity` (GPU idle). Each frame `writeNodeInstances` precomputes `world = entity × localToModel`, `prevWorld = prevEntity × localToModel`, and `NodeFlag::FlipWinding` from `det(world) < 0`. A mirrored instance keeps back-face culling: the mesh shader swaps two triangle corners instead of changing front-face state, which DGC cannot vary per sequence.

---

## 7. Shader access

### Per-frame data

Per frame, in recording order:
- `MeshletCulling::prepareFrame(frameSlot)` reads back the slot's counters, rebuilds and uploads `GpuInstanceCullItem` / `GpuDrawRecord` templates when the layout generation changed, writes one `GpuCullView` per phase and sizes the DGC buffers.
- `FrameData::write(frameSlot, FrameRootInputs)` writes:
  - `GpuFrameRoot` into `frameRoots[frameSlot]` (`writeFrameRoot`). Values owned by other modules (camera, targets, this slot's draw-record, visible-list and counter addresses, late-visible base, triangle-cull flags) arrive in `FrameRootInputs`, which `Renderer::drawFrame` fills from their getters;
  - `GpuNodeInstance[entity × node]` into `nodeInstanceBuffers[frameSlot]` (`writeNodeInstances`, §6.2);
  - `GpuObjectUB[entity]` into `instanceUbos[frameSlot]`: model matrix, previous model matrix, world-space `boundingSphere`, `modelIndex`, `instanceFlags`. Only lights still place themselves with it.

  Inactive entities are skipped.
- On the GPU, per phase: `cullInstances` copies the templates of visible instances into `drawRecords`; `cullMeshlets` (early) or `cullRejectedMeshlets` (late) appends `GpuVisibleMeshlet` entries per state group; `buildMeshSequences` writes `MeshDgcSequence`s and their counts. The CPU never writes draw records or sequences.

`GpuFrameRoot` (128 B):

| Field | Points at |
|---|---|
| `camera` | `GpuCameraData` (512 B) of this frame slot |
| `instances` | `GpuObjectUB[entity]` |
| `models` | `GpuModel[model]` |
| `primitives` | `GpuPrimitive[]`, absolute rows |
| `materials` | whole shared `GpuMaterial[]`, indexed by `GpuPrimitive::materialId` (`kNoneIndex` → row 0) |
| `pbrExtensions`, `lights` | whole buffers; `pbrExtensions` row 0 is the default material's |
| `tlas` | 0 until ray tracing exists |
| `nodeInstances` | `GpuNodeInstance[entity × node]` of this frame slot |
| `drawRecords` | `GpuDrawRecord[]` of this frame slot, written by `cullInstances` |
| `visibleMeshlets` | `GpuVisibleMeshlet[]` of this frame slot; `lateVisibleBase` is the late phase's first row |
| `lightCount`, `instanceCount`, `primitiveCount`, `nodeInstanceCount` | |
| `debugView` | `DebugViewSettings::packed()`: `kDebugView*` mode in the low byte, `kDebugViewFlagEdges` (bit 8) |
| `cullCounters` | `GpuCullCounters[kCullPhaseCount]`; the mesh shaders add triangle counts |
| `triangleCull`, `subpixelSnap` | `TriangleCullFlag` bits; snap of the small-triangle test |

### Culling data

| Struct | Size (B) | Where | Role |
|---|---|---|---|
| `GpuCullView` | 272 | host, per slot and phase | flags, matrices (`cullViewProj` for frustum and sub-pixel, `occlusionViewProj` for Hi-Z), pyramid heap indices, target size, snap, and the addresses below |
| `GpuCullCounters` | 112 | host-visible VRAM, per slot and phase | visible counts and `sequenceCount` per state group, indirect dispatch args, stats |
| `GpuInstanceCullItem` | 12 | host, per slot | one node instance: its draw-record template range |
| `GpuDrawRecord` | 20 | templates host, records device | one (node instance, meshlet group) work item: `{nodeInstanceIndex, primitiveIndex, meshletOffset, flags, groupOffset}`; cull-internal, the mesh shaders do not read it |
| instance candidates | `uint` per item | device | early-occluded instance items, re-tested late |
| reject masks | `uint` per early draw record | device | bit i = meshlet i of the group failed only occlusion; written every frame, so never cleared |
| `GpuVisibleMeshlet` | 8 | device, one region per phase, per state group | `{nodeInstanceIndex, meshletRow}`, the render-list entry (`vk_lod_clusters` `ClusterInfo`) |

Capacities are exact scene totals today, so no list can overflow; capacity checks with overflow counters are spec §8.1 A10 (§12).

### Push data and the lookup chain

| Push data | Size (B) | Used by |
|---|---|---|
| `MeshPushData {frameRoot, firstVisible, meshletCount}` | 16 | mesh shaders; one per DGC sequence |
| `CullPushData {frameRoot, cullView}` | 16 | cull passes and `buildMeshSequences` |
| `HiZPushData` | 32 | `hiz_build`: src/dst heap indices, max sampler, mip, sizes |
| `HiZViewPushData` | 24 | `hiz_view` debug pass |

`loadDrawContext` (`shaders/core/frame.slang`) and `geometry/mesh.slang`:

```
root      = frameRoot[0]
visible   = root.visibleMeshlets[push.firstVisible + groupId]
node      = root.nodeInstances[visible.nodeInstanceIndex]          worldMatrix, flags
model     = root.models[node.modelIndex]
primitive = model.primitives[model.meshletPrimitives[visible.meshletRow]]
material  = root.materials[primitive.materialId == kNoneIndex ? 0 : primitive.materialId]   DoubleSided flag
extension = same index as material, from root.pbrExtensions  (loadMaterialExtension)
light     = root.lights[i], moved to world space by root.instances[light.instanceIndex].modelMatrix  (loadWorldLight)
meshlet   = model.meshlets[visible.meshletRow]
vertex    = model.vertices[model.meshletVertices[meshlet.vertexOffset + i]]
```

Dependent-load depth from the push data to the primitive is 5.

### Handle or address

| Accessing | Use | Why |
|---|---|---|
| Buffer bytes the engine allocated (vertices, materials, frame root) | BDA, `bdaPtr<T>` | no descriptor, one less indirection |
| Sampled / storage image | heap index → `DescriptorHandle<Texture2D>` / `RWTexture2D.Handle` | images are opaque |
| Sampler | heap index → `DescriptorHandle<SamplerState>`, index in `.x` | samplers live only in the sampler heap |
| Acceleration structure | raw device address → `RaytracingAccelerationStructure(uint64_t)` | no heap slot needed |
| Texture table (material rows) | `uint` heap indices in the row, handles built in the shader | opaque objects cannot live in buffers |

Rules:
- Compile flags (`CMakeLists.txt`): `-capability …+spvDescriptorHeapEXT`, `-fvk-use-scalar-layout`, `-matrix-layout-column-major`. Scalar layout is what makes C++ `Gpu*` PODs match Slang.
- **`kNoneIndex` never becomes a handle.** Material rows store `kNoneIndex` for missing textures, and `sampleBaseColor` skips the fetch. Every new material fetch guards the same way.
- **Non-uniform indices.** Raster indices are uniform per draw today. Slang 2026.14.1 does not emit `NonUniform` on heap access even with `NonUniformResourceIndex`; read the PBR plan §4.4 before indexing per pixel or per hit (spec S6).
- **Mirrored structs.** Every struct in `shaders/types.slang` has a C++ twin with `static_assert` on size and key offsets.
- Raw descriptor blobs never go in buffers; the heap index is the only stored form.

### Struct reference

| Struct | Size (B) | Notes |
|---|---|---|
| `MeshPushData` | 16 | all offsets asserted |
| `MeshDgcSequence` | 32 | 16 B push data + 12 B draw command, `alignas(16)` so the BDA stays 8-byte aligned |
| `CullPushData` | 16 | |
| `GpuFrameRoot` | 128 | `lightCount` @ 88, `cullCounters` @ 112 |
| `GpuModel` | 112 | `meshletBounds` @ 16, `primitives` @ 96, `attributeMask` @ 104 |
| `GpuNode` | 112 | affine rows; `firstPrimitive` @ 64, `bboxMin` @ 80 |
| `GpuNodeInstance` | 144 | `entityIndex` @ 128 |
| `GpuPrimitive` | 36 | `firstGroup` @ 28 |
| `GpuMeshletDesc` | 32 | `coneAxis` @ 12, `boundingSphere` @ 16 |
| `GpuMeshletBounds` | 32 | `bboxMax` @ 16 |
| `GpuMeshletGroup` | 32 | `bboxMax` @ 16 |
| `GpuVisibleMeshlet` | 8 | |
| `GpuDrawRecord` | 20 | |
| `GpuInstanceCullItem` | 12 | |
| `GpuCullView` | 272 | key offsets asserted |
| `GpuCullCounters` | 112 | key offsets asserted |
| `GpuObjectUB` | 160 | `modelIndex` @ 144 |
| `GpuMaterial` | 240 | `alignas(16)` in C++; explicit pads on both sides |
| `GpuLight` | 64 | model space; `instanceIndex` @ 56; `type` is a raw `uint` |
| `GpuMaterialExtension` | 724 | `GpuTextureRef` 36 B, `GpuUvTransform` 20 B; key offsets asserted |
| `GpuVertex` | 32 | |
| `GpuCameraData` | 512 | size and key offsets asserted |

---

## 8. Textures, samplers, descriptor heaps

### Heaps

`DescriptorManager` creates one resource heap and one sampler heap, each the device maximum, as host-visible dedicated buffers (`STORAGE | SHADER_DEVICE_ADDRESS | DESCRIPTOR_HEAP_EXT`). The driver-reserved range sits at the end of each heap. Heaps are bound every frame and in the DGC state command buffers.

| Operation | Behavior |
|---|---|
| `writeImageDescriptor` | pops `freeImageSlots`, else bump-allocates (throws when it would reach the reserved range); writes a `SampledImage` descriptor; index = offset / descriptor size |
| `freeImageDescriptor` | rewrites the slot with the fallback image (the 1×1 white texture, set by `TextureManager::init`), then pushes the index on the free list |
| samplers | slot 0 = default (linear, anisotropic, repeat); `writeSamplerDescriptor` bump-allocates (throws at the reserved range); never freed: dedup keeps the count small. The Hi-Z max-reduction sampler is one more slot |

### Textures

| Path | Format | Upload | Cache key |
|---|---|---|---|
| `.ktx` / `.ktx2` | from the file (e.g. BC7); 2D, one layer, no Basis | host image copy of every mip, straight to `ShaderReadOnlyOptimal`, no command buffer | path |
| `.png` / `.jpg` / `.bmp`, glTF memory / pixels | RGBA8 sRGB or UNORM | host image copy of level 0, GPU blits for the mip chain, inside a `TextureUploadBatch` (one submit + fence) | `path\|srgb` / `path\|linear` |

- Images need `HOST_TRANSFER` usage support; no staging buffers are used.
- Decoded pixels are freed right after upload.
- `getOrCreateSampler` dedups by (min, mag, wrapS, wrapT); the glTF default maps to sampler 0.
- **Lifetime:** each model's `ModelTextureRefs` retains its textures; `release` destroys a texture at refcount 0 (view, image, heap slot). A permanent 1×1 white texture backs freed heap slots; materials do not reference it.

---

## 9. Render targets, DGC, sync

| Image | Format | Notes |
|---|---|---|
| Swapchain | `B8G8R8A8_SRGB` when available, `FIFO_LATEST_READY` when available | color attachment of both raster phases: early clears, late loads |
| Depth | `D32_SFLOAT`, 1 sample, standard Z (clear 1, `LESS`) | `DepthStencilAttachment \| Sampled`: the Hi-Z build reads it; stored |
| Hi-Z pyramid | `R32_SFLOAT`, previous power of two of the depth size, full mip chain, always `eGeneral` | storage view per mip + one full-chain sampled view; one mip per dispatch; skipped while culling is frozen |

- No MSAA, no G-buffer and no render-target heap slots for lighting exist yet.
- `createAttachment(target, AttachmentDesc)` destroys the old image, allocates at swapchain extent, names it, transitions it and creates the view.
- Resize: `recreateSwapChain` (`waitIdle`) → `Renderer::rebuildSwapchainResources` recreates the depth, the pyramid and sync objects; occlusion history drops for one frame (`NoPyramid`).
- **Frame order** (`Renderer::recordPhase`, once per `CullPhase`): cull → DGC preprocess → raster → Hi-Z build. The late phase draws over the early one and adds ImGui.
- **DGC:** `PUSH_DATA + DRAW_MESH_TASKS` sequences written by `buildMeshSequences`, one execute per (phase, `MeshStateGroup`) with `sequenceCountAddress` from `GpuCullCounters::sequenceCount`. Explicit preprocess outside the render pass, each (phase, group) region 4 KiB-aligned (NVIDIA 616.92 faulted with the reported 256 B alignment, `vk_dgc.cpp`).
- **State groups:** `SingleSided` and `DoubleSided` (cull mode none, no cone test), chosen per primitive from the material's `DoubleSided` flag (`MeshletCulling::stateGroupOf`).
- **Raster variants and debug views:** `ShaderObjects` holds one linked mesh + fragment pair per `RasterVariant` (`Lit`: `mesh_raster` + `mesh_fragment`; `GeometryDebug`: `mesh_debug`), plus the compute shaders (`cullInstances`, `cullMeshlets`, `cullRejectedMeshlets`, `buildMeshSequences`, Hi-Z build) and the `hiz_view` pair. DGC keeps one `GeneratedCommandsShaderInfoEXT` per variant and one state command buffer per (variant, state group), because explicit preprocess must see the shaders and state that execute binds. `Renderer::debugView` picks the variant once per recorded frame and goes into `GpuFrameRoot::debugView`. Views: meshlets, triangles, primitives, node instances, state groups, cull phase, Hi-Z pyramid, with an optional `SV_Barycentrics` edge overlay; lighting and upscaling entries are reserved and disabled.
- **Sync per frame slot:** image-acquired semaphore + fence; one render-finished semaphore per swapchain image.

---

## 10. CPU vs GPU

| Data | CPU after flush | GPU |
|---|---|---|
| Geometry streams, meshlets, bounds, indices | dropped | asset buffers |
| `PrimitiveDraw` catalog, `GeometryStore::meshletGroups` | kept: cull templates are built from them on layout change | `GpuPrimitive`, `GpuMeshletGroup` rows |
| Material rows | kept: `MaterialStore::infos` is the dedup source and the editor view; freed rows stay as free slots | `Materials`, `PbrExt` |
| Lights | kept: `LightStore::uploaded` mirrors the GPU rows so erase can fix `instanceIndex`; `ModelAllocation::lights` keeps the template | `Lights` |
| Model-space bounding spheres | kept (`ObjectStorage::localBounds`) | world-space copy in `GpuObjectUB` |
| Transforms, previous model matrices, flags, names | kept (`ObjectStorage`) | instance rows, rewritten every frame |
| Cull templates (`GpuInstanceCullItem`, `GpuDrawRecord`) | kept per layout generation | uploaded to each frame slot |
| Texture / sampler handles and cache keys | kept | images, heap slots |
| Buffer addresses, `modelAllocations`, used / capacity bytes | kept | — |
| `auxBlobs`, extension name lists, entity names | kept, never uploaded | — |

---

## 11. Recipes

### Add an asset buffer

1. **Scratch:** add the vector to its store. Per-vertex streams must also be resized in `GeometryStore::resizeVertices` and dropped in `clearScratch`.
2. **Enum:** add a value to `AssetBuffer` before `Count`.
3. **Metadata:** add the `kAssetBufferInfo` row **at the same position**: debug name, Tracy pool, two plot literals, `stride = sizeof(element)`.
4. **Source:** add the `case` to `AssetBuffers::scratchBytes` (`-Wswitch` catches a missing one).
5. **Offsets:** keep values inside the payload model-relative. Only CPU catalog fields go through `remapScratchOffsets`.
6. **Reach:** add the slice address to `GpuModel` (+ a `GpuModelAttribute` bit if optional) in `types.hpp` and `types.slang`, fill it in `rebuildModelTable`, update the `static_assert`s.

Growth, compaction on unload, destruction, per-model ranges, logs and Tracy plots come for free. Extra usage flags (AS build input) need a per-row usage column in `kAssetBufferInfo`, applied in `createAssetBuffer`.

### Add a per-frame host buffer

Follow `frameRoots` / `instanceUbos`: `std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT>`, created with `createHostBuffer`, destroyed with `destroyHostBuffer` under the same Tracy name, written only for `currentFrame` after its fence. Growing destroys every slot, so grow only where no frame is in flight (load, after `waitIdle`), or per slot after its fence as `MeshletCulling::FrameBuffers` does. Expose the address through the frame root.

### Add a GPU list

Follow `MeshletCulling::FrameBuffers`: a `DeviceBuffer` per frame slot, one region per phase, a count in `GpuCullCounters` (reset before the early cull), the address in `GpuCullView` or `GpuFrameRoot`. Append with one atomic per wave. Report count, capacity and overflow in ImGui (spec S11, §14).

### Add a swapchain-sized attachment

Add an `AttachmentImage` member and a create function that calls `createAttachment` with an `AttachmentDesc`. Call it from `RenderTargets::recreate`, and destroy it with `destroyAttachment` in `~RenderTargets` with the same Tracy name.
- `transitionImageLayout` infers stages only for `Undefined → ColorAttachmentOptimal / DepthStencilAttachmentOptimal / TransferDstOptimal` and `TransferDstOptimal → ShaderReadOnlyOptimal`.
- Shader-visible targets need a resource-heap slot from `DescriptorManager`; once `GpuTargets` exists (§12), the slot goes there.

### Other images

Use `VkAllocator::createImage2D`. Textures always go through `TextureManager`, which owns the heap slot.

### One-time GPU work

| Context | Command buffer | Queue |
|---|---|---|
| Asset buffer copies (`uploadAssetBuffers`, `compactAssetBuffers`) | `oneTimeTransferCmd()` | `oneTimeTransferQueue()` |
| Attachment transitions | `commandBuffers[0]`, also frame 0's render buffer: only when no frame is in flight | graphics |
| Textures | the upload batch's command buffer | graphics |

`submitAndWait` blocks until the queue is idle: load and resize only, never per frame. Submit a command buffer only to its pool's queue family.

---

## 12. Planned changes (engine spec)

What the target architecture adds to or removes from the resources above, by spec milestone. Section numbers are [engine_spec.md](../docs/engine_spec.md)'s.

| Milestone | Change | Spec |
|---|---|---|
| M0 | **Done (2026-11-08).** Remove the animation resources: `Joints`, `Weights`, `MorphPos`, `MorphNrm`, `MorphTan`, `MorphTargets`, `MorphWeights` buffers and their `GpuModel` slices and attribute bits, `GpuPrimitive` morph ranges, `GpuMorphTarget`, `DrawRecordFlag::SkipCulling` and the "morphed primitives skip culling" rule | §7.3 |
| M0 | `GpuTargets` block (heap indices of all render targets) reached from `GpuFrameRoot`; frame root gains `frameIndex`, `exposure`, jitter | §6.2, S7 |
| M0 | Every GPU list append through a capacity check with an overflow counter in `GpuCullCounters` and the culling panel | §8.1 A10, S11 |
| M1 | Depth becomes reverse Z (clear 0, `GREATER`); Hi-Z min reduction, `nvhiz` 3 levels per dispatch; `CULLING_REVERSED_Z` on | §8.1 A9, §8.2 |
| M1 | Visibility buffer (`R32_UINT`: visible-list row + triangle) at render resolution, `renderExtent` separate from the swapchain extent, `displayColor` until M3; `GpuVisibleMeshlet` rows are what it indexes ([visibility_buffer.md](./features/visibility_buffer.md)) | §8.3 |
| M1 | State groups replaced by bins by pipeline (opaque, later alpha-tested); two-sided by winding flip, cluster and group state bits | §8.1 A6, §8.2 |
| M1 | Render-resolution shading surface and DLSS guides (`normalRoughness`, `geoNormal`, `diffuseAlbedo`, `specularF0`, `specAlbedo`, `emissive`, `motion`), two sets ping-ponged by frame parity, written by the compute resolve | §9.3 |
| M2 | Per-cluster CLAS (object space, shared by instances), one cluster BLAS per primitive, GPU-written `VkAccelerationStructureInstanceKHR[]` + `GpuRayInstance[]`, TLAS; build scratch freed after load, CLAS moved to final size; `tlas` and `rayInstances` in the frame root | §10.2–10.3 |
| M2 | Async compute queue for the RT scene build, timeline semaphore before the first ray pass | §5, §13 |
| M3–M5 | RTXDI reservoirs, light buffer and constants; SHaRC hash/resolved buffers; NRC SDK buffers; `directRadiance`, `indirectRadiance`, `noisyColor`, DLSS output (display resolution); per-SDK constants in the frame root | §11, §12 |
| M6 | OMM arrays per primitive (NVIDIA OMM SDK) with a per-cluster OMM index buffer, baked from the final cluster triangles | §7.1, §10.5 |
| M7 | Material bin tile lists, counts and the per-pixel bin-id image; DGC compute execution set | §9.1–9.2 |
| M7 | Emissive-triangle light list built on the GPU at load | §6.1, §11.2 |
| M8 | Uploads through GDeflate pages in ReBAR staging, `vkCmdDecompressMemoryEXT` on a graphics or compute queue; host image copy only for first-load source images; KTX2 BC7 / BC5 produced automatically and cached; geometry cache per asset + processing version | §3, §6.4, §7.2 |
| M8 | Geometry streams: octahedral snorm16 normal + packed tangent, half2 UV, vertex order by `meshopt_optimizeVertexFetchRemap` | §6.3 |
| L1–L3 (post-1.0) | Cluster DAG levels in the same model slices (`meshletRow` space grows), node tree + `TraversalMetric`, `generatingGroup` per cluster, fixed list capacities with clamping, per-frame cluster BLAS with sharing / merging / caching, all LOD levels' CLAS resident | §7.1, §10.3, §16.3 |

The binding model (§1, model slices, frame root, handle-or-address) carries over unchanged.

---

## 13. Known issues

| Issue | Where |
|---|---|
| Optional streams are still zero-filled in CPU scratch during load (only the GPU upload is skipped) | `GeometryStore::resizeVertices` |
| Sampler slots are never freed (bounded by sampler dedup) | `vk_descriptors.cpp` |
| Normals and tangents are mesh-local; lighting will need the node instance's normal matrix (inverse-transpose of `worldMatrix`) | `GpuNodeInstance` |
| The material and light buffers never shrink: freed material rows stay as reusable slots, light rows past the mirror stay stale | `MaterialStore`, `uploadLights` |
| Models are cached by file path only: a file edited on disk while loaded is not re-read until all its entities are unloaded | `findModel` |
| `GpuObjectUB::boundingSphere` is still written every frame but read by no shader | `FrameData::write` |
| `GpuMeshletBounds` edge lengths are stored but unused until software raster | `buildMeshlets` |
| The late meshlet pass projects boxes twice (cull and occlusion matrices) even when they are equal | `meshlet_cull.slang` |
| Buffer policy lives in `vk_utils.cpp` (`createBuffer`), image policy in `VkAllocator` (`vk_allocator.hpp`) | allocator |
| `loadWorldLight` reads the owner's instance row, which inactive entities leave stale | `frame.slang` |
| Untested: load two models, then unload one (model table rewrite, primitive and group compaction, light owner rewrite, fallback heap slot) | — |

---

## Changelog

| Date | Change |
|---|---|
| 2026-11-11 | `ResourceManager` split ([resource_manager_split.md](./features/resource_manager_split.md)): `FrameSync` (fences, semaphores, command pools and buffers), `AssetBuffers` (asset buffers, model catalog, model table, lights), `FrameData` (frame roots, instance and node-instance rows); depth moved to `RenderTargets`, camera buffers to `Camera::createBuffers`. The frame root takes pass addresses through `FrameRootInputs`. No layout, address or upload-order change |
| 2026-11-08 | Animation removed (engine spec §7.3): `Joints`, `Weights`, `MorphPos`, `MorphNrm`, `MorphTan`, `MorphTargets`, `MorphWeights` buffers, their `GpuModel` slices and attribute bits (`Indices` is now bit 3), `GpuPrimitive` morph ranges, `GpuMorphTarget`, `MorphTarget`, `DrawRecordFlag::SkipCulling` (`TwoSided` is now bit 2), `GpuInstanceCullItem::flags`, the `unculledMeshlets` counter. Sizes: `GpuModel` 112, `GpuPrimitive` 36, `GpuInstanceCullItem` 12, `GpuCullCounters` 112. glTF morph targets and skins are ignored; files load in their base pose |
| 2026-11-07 | Moved to `spec/`. Re-verified against `7dc04f965`: GPU culling buffers and structs, GPU-written draw records and DGC sequences, `MeshletBounds` / `MeshletGroups` / `MeshletPrimitives` asset buffers, `GpuVisibleMeshlet` lookup chain (push data `{frameRoot, firstVisible, meshletCount}`), sizes (`GpuFrameRoot` 128, `GpuModel` 168, `GpuNode` 112, `GpuNodeInstance` 144, `GpuPrimitive` 48, `GpuDrawRecord` 20), meshlet build as `clusterlod.h` level 0, render targets (no MSAA, D32 depth, Hi-Z pyramid), debug views. New §12: resource changes per [engine_spec.md](../docs/engine_spec.md) milestone |
| 2026-10-25 | meshoptimizer clean-up: `meshopt_computeSphereBounds` for vertex and node spheres (`mergeBoundingSpheres` removed), `meshopt_filterIndexBuffer` on glTF triangle lists and OBJ, OBJ welding via `meshopt_generateVertexRemap` (`std::hash<GpuVertex>` removed) |
| 2026-10-25 | Debug views: `DebugView` table, `RasterVariant` shader-object pairs, per-variant DGC shader info + state command buffer, `mesh_debug.slang` (meshlets / triangles / primitives / node instances, barycentric edges), `GpuFrameRoot::debugView` replaces `pad0`, `VK_KHR_fragment_shader_barycentric` |
| 2026-10-25 | Mesh/node split: meshes stored once per model, `NodeDraw` / `GpuNode` placements (`Nodes` buffer), `EXT_mesh_gpu_instancing`; no transform baking; per-frame precomputed `GpuNodeInstance[entity × node]`; 16 B push data + per-frame `GpuDrawRecord` buffer; mirrored instances swap winding in the mesh shader |
| 2026-10-25 | Shared, refcounted resources: `ModelAllocation` per model file with `refCount`, reused by `findModel` / `instantiateModel`; entities point at models; material table deduplicated across models with per-row refcounts and a free list, absolute `materialId`, dirty-row uploads; models own `materialRefs` / `textureRefs`; lights per entity from a model template; `eraseEntity` replaces `eraseModel` as the public unload |
| 2026-10-24 | ImGui "Assets" tree: materials and lights; `MaterialStore::infos` mirror, `TextureManager::textureInfo` |
| 2026-10-24 | Morph deltas reachable: `MorphTargets` / `MorphWeights` asset buffers, morph slice addresses in `GpuModel`, morph range in `GpuPrimitive`; mesh shader applies default-weight position morphs |
| 2026-10-24 | Asset uploads batched into one staging buffer + submit; model table rewritten in place, grows ×2; optional streams uploaded only when present; `GpuMaterialExtension` POD + Slang mirror; lights in model space with `instanceIndex`; heap capacity checks and fallback image for freed slots |
| 2026-10-23 | Merged the six resource docs into this one and re-verified every section against the working tree on `fa2222941` |
