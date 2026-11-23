# Feature: ResourceManager split

**Date:** 2026-11-11 · **Status:** RS1–RS5 built; builds clean (ImGui on/off, debug/release); §6 runtime checks pending · **Milestone:** M2, before acceleration-structure state is added ([engine_spec.md](../../../docs/engine_spec.md) §16.2)
**Implements:** engine spec §20.2 AU2
**Related:** [engine_ui_split.md](./engine_ui_split.md) (AU3), [resource_architecture.md](../../resource_architecture.md), engine spec §20.2 AU1 (retire queue) and AU4 (GPU scene update)

`ResourceManager` (`vk_resource_manager.hpp`, 316 lines; `.cpp`, 1,446 lines; both deleted by RS5, replaced by [vk_frame_sync](../../../src/core/vk_frame_sync.hpp), [vk_asset_buffers](../../../src/core/vk_asset_buffers.hpp) and [vk_frame_data](../../../src/core/vk_frame_data.hpp)) held three unrelated jobs plus a few strays, and nearly every member was public. M2 would add acceleration-structure state to it. This spec splits it into three classes with one job each and removes the class.

This is a move of code, not a change of behaviour: same GPU data, same frame, same stalls. The stalls are AU1's work and the per-frame entity loop is AU4's.

---

## 1. Current responsibilities

| Job | Members today |
|---|---|
| Frame sync | `inFlightFences`, `presentCompleteSemaphore`, `renderFinishedSemaphore`, `commandPool`, `transferCommandPool`, `commandBuffers`, `presentCommandBuffers`, `transferCommandBuffer`, `createSyncObjects`, `createCommandPool`, `createCommandBuffers`, `setSwapChainImageCount`, `oneTimeTransferCmd`, `oneTimeTransferQueue` |
| Asset buffers and model catalog | `AssetBuffer`, `ElementRange`, `ModelAllocation`, `DeviceBuffer`, `kAssetBufferInfo`, `assetBuffers`, `modelAllocations`, `modelTable`, `flushGpuAssets`, the `pack*` functions, `remapScratchOffsets`, `uploadAssetBuffers`, `appendMaterialUploads`, `uploadLights`, `compactAssetBuffers`, `rebaseCatalog`, `eraseModel`, `eraseEntity`, `instantiateModel`, `attachEntity`, `findModel`, `rebuildModelTable`, `discardScratch`, the size and address queries |
| Per-frame data | `instanceUbos`, `nodeInstanceBuffers`, `frameRoots`, `entityNodeInstanceFirst`, `nodeInstanceCount`, `nodeInstanceLayoutGeneration`, `instanceCapacity`, `updateUniformBuffers`, `writeFrameRoot`, `writeNodeInstances`, `rebuildNodeInstanceLayout`, `ensureInstanceCapacity`, `createUniformBuffers` |
| Pass state stored for the frame root | `cameraAddresses`, `drawRecordAddresses`, `visibleMeshletAddresses`, `lateVisibleBases`, `cullCounterAddresses`, `triangleCullFlags`, `targetsAddresses`, `subpixelSnap`, written by `MeshletCulling`, `RenderTargets` and `createCameraBuffers` |
| Render-target strays | `depthAttachment`, `createDepthResources`, `createAttachment`, `findDepthFormat`, `findSupportedFormat`, `createImageView`, `renderExtent`, `swapChainExtent`, `swapChainImageFormat`, `swapChainImageCount` |
| Helpers | `createHostBuffer`, `destroyHostBuffer`, `createCameraBuffers`, `tracyPlotResources`, `tracyPlotMemoryBudget` |

---

## 2. Target classes

All three live in `src/core`, in `engine_core`.

| Class | Files | Owns | Depends on |
|---|---|---|---|
| `FrameSync` | `vk_frame_sync.{hpp,cpp}` | fences, acquire and render-finished semaphores, the graphics and transfer command pools, the per-slot frame and present command buffers, the one-time command buffer | `Device` |
| `AssetBuffers` | `vk_asset_buffers.{hpp,cpp}` | the device-local asset buffers, `modelAllocations`, the `GpuModel` table, load flush, unload compaction, catalog rebase, entity attach and erase, light and material uploads | `Device`, `VkAllocator`, `FrameSync` (one-time commands), the four scene stores |
| `FrameData` | `vk_frame_data.{hpp,cpp}` | the per-slot frame roots, node-instance rows and object rows, the node-instance layout, the per-frame writes | `Device`, `VkAllocator`, `AssetBuffers` (read-only), `GeometryStore` (read-only, node draws), `ObjectStorage` |

Dependencies point one way: `FrameSync` ← `AssetBuffers` ← `FrameData`. No class reaches back.

### 2.1 FrameSync

- Takes the frame-sync members of §1 unchanged, names included.
- `recreate(swapChainImageCount)` replaces `setSwapChainImageCount` + `createSyncObjects`. A setter that rebuilds every fence was a trap.
- `oneTimeCommands()` returns the command buffer and queue for load-time work. It keeps today's fallback (transfer pool when a transfer family exists, frame slot 0's buffer otherwise). AU1 gives it a pool of its own and adds the retire queue to this class; neither is part of this spec.

### 2.2 AssetBuffers

- Takes the asset and catalog members of §1 unchanged.
- Members become private. Public surface: `flush`, `findModel`, `instantiateModel`, `eraseEntity`, `asset`, `modelAssetAddress`, `modelTableAddress`, `models()` (read-only span of `ModelAllocation`), `uploadedCount`, `modelGpuBytes`, `assetTotals`, `sceneGeneration`.
- `sceneGeneration` is a counter bumped by every `flush`, `instantiateModel` and `eraseEntity` that changes entities or models. It replaces the three direct calls to `rebuildNodeInstanceLayout`.
- `tracyPlotResources` stays here for the asset, material and light plots. The instance-capacity plots move to `FrameData`.

### 2.3 FrameData

- Takes the per-frame members of §1.
- `syncScene()` grows the object rows to the entity count and rebuilds the node-instance layout when `AssetBuffers::sceneGeneration` differs from the one it last saw. It needs the GPU idle, as the rebuild does today. Callers: the engine after every load, instance, unload and instance-field action, replacing `ensureInstanceCapacity` at those call sites, and once at init.
- `write(frameSlot, inputs)` replaces `updateUniformBuffers`. It asserts the layout is current.
- `layoutGeneration()` and `entityNodeInstanceFirst()` are the read accessors `MeshletCulling` uses.

Pass state no longer sits in a shared object. The renderer collects it and passes it in:

```cpp
// everything the frame root needs from the passes of this frame
struct FrameRootInputs {
    vk::DeviceAddress camera;
    vk::DeviceAddress targets;
    vk::DeviceAddress drawRecords;
    vk::DeviceAddress visibleMeshlets;
    vk::DeviceAddress cullCounters;
    uint32_t lateVisibleBase;
    uint32_t triangleCullFlags;
    float subpixelSnap;
    uint32_t debugView;
    uint32_t frameFlags;
    uint32_t frameIndex;
};
```

`Renderer::drawFrame` fills it after `recordFrameCommands` from `Camera`, `RenderTargets` and `MeshletCulling`, each of which exposes its own value for the slot. The write still happens after recording, as today.

---

## 3. Where the strays go

| Item | New home | Why |
|---|---|---|
| `HostBuffer`, `createHostBuffer`, `destroyHostBuffer` | free functions in `util/vk_utils`, next to `createBuffer` | used by `RenderTargets` and frame data; `MeshletCulling` has a near-copy (`createMappedBuffer`) that can then use the shared one |
| `depthAttachment`, `createDepthResources`, `createAttachment`, `AttachmentImage`, `findDepthFormat`, `renderExtent` | `RenderTargets` | depth is a render target; `RenderTargets` and `HiZPyramid` already build views of it |
| `findSupportedFormat`, `createImageView` | free functions in `util/vk_utils` | stateless |
| `swapChainExtent`, `swapChainImageFormat`, `swapChainImageCount` copies | deleted | readers take them from `SwapChain` |
| `createCameraBuffers` | `Camera` creates and destroys its own buffers | removes the `scene/vk_camera.hpp` include from `core`, which ends the `core` ↔ `scene` include cycle |
| `tracyPlotMemoryBudget` | `VkAllocator` | it reads only VMA heap budgets |

---

## 4. Lifetime

| Step | Order |
|---|---|
| Create | `FrameSync` → `AssetBuffers` (flushes the startup model) → `FrameData` (`syncScene`) → `Renderer` |
| Destroy | `Renderer` → `FrameData` → `AssetBuffers` → `FrameSync` → `VkAllocator` |

`Engine::cleanup` clears the command buffers before the resources they reference; that becomes `FrameSync`'s destructor order (buffers before pools) and the explicit clear goes.

---

## 5. Tasks

Each step builds, runs and passes §6 before the next starts.

| # | Task |
|---|---|
| RS1 | Move the strays of §3: host-buffer and format helpers to `vk_utils`, depth and `renderExtent` to `RenderTargets`, camera buffers to `Camera`, memory-budget plot to `VkAllocator`; delete the swapchain copies |
| RS2 | Extract `FrameSync`; `Renderer`, `HiZPyramid` and `RenderTargets` take it for command buffers, fences and semaphores |
| RS3 | Extract `FrameData` with `FrameRootInputs`; `MeshletCulling` and `RenderTargets` stop writing into shared arrays and expose their per-slot values; add `sceneGeneration` and `syncScene` |
| RS4 | Rename the remainder to `AssetBuffers`, make its members private, delete `ResourceManager` and update the `Engine` call sites |
| RS5 | Update [resource_architecture.md](../../resource_architecture.md) owner names and engine spec §6.1 (`ResourceManager` → `AssetBuffers`) |

---

## 6. Verification

No behaviour changes, so every check is "same as before the step".

| Check | Pass when |
|---|---|
| Build | clean on the debug and release presets, no new warnings |
| Validation | clean with core, sync and best practices at startup, in a steady frame, on resize and on unload |
| Image | Sponza from the start position matches a capture taken before RS1, in the lit view and the `MotionVectors` view |
| Cull counters | identical on the same frozen view |
| Scene actions | load a second model, instance it, spawn and clear the instance field, unload: no validation message, no stale geometry |
| Resize | drag-resize and minimize-restore: no validation message |
| Frame time | GPU timer rows and CPU frame time within noise of the pre-split numbers (release build, layers off) |
| Tests | the existing tests pass |

---

## 7. Must not

- change GPU layouts, addresses, upload order or barrier masks while moving code;
- let `AssetBuffers` call into `FrameData`, or `FrameSync` into either;
- keep a facade `ResourceManager` that forwards to the three classes;
- remove a `waitIdle` or `submitAndWait` here (AU1), or change the per-entity write loop (AU4);
- make `FrameRootInputs` a place where passes store state between frames: it is built and consumed in one `drawFrame`.
