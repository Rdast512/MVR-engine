# Feature: motion vectors

**Date:** 2026-11-08 · **Status:** computed in the resolve (M1); correctness under motion unverified, two history bugs open (§3) · **Milestone:** M3 ([engine_spec.md](../../docs/engine_spec.md) §16.2), before DLSS-RR and RTXDI temporal reuse
**Implements:** engine spec §9.2 step 3, §9.3 `motion`, §12.2
**Related:** [visibility_buffer.md](./visibility_buffer.md) §3.6, §10; [pbr_implementation_plan.md](../pbr_implementation_plan.md) E1, H1

Every temporal consumer (DLSS-RR, RTXDI and ReSTIR GI temporal reuse) finds a pixel's previous-frame position through the `Motion` guide. Wrong motion vectors show up as ghosting and smearing after DLSS, and as reuse of the wrong reservoirs, which looks like lighting noise rather than a motion bug. This spec fixes the convention, closes the history bugs, and adds a debug view that proves the vectors right.

---

## 1. Convention

| Item | Rule |
|---|---|
| Image | `Motion` guide, `RG16F`, render resolution, both guide sets |
| Unit | render-resolution pixels (DLSS `InMVScaleX/Y = 1`, feature flag `MVLowRes`) |
| Direction | current → previous: `previousPixel = currentPixel + motion` |
| Jitter | excluded: both positions use the **unjittered** `viewProj` / `prevViewProj`. DLSS gets the jitter separately (`InJitterOffsetX/Y`, render pixels); `MVJittered` is not set |
| Surface point | the jittered sample's world position (barycentrics from the jittered corners), projected with unjittered matrices. This is what DLSS expects |
| Object motion | `GpuNodeInstance::prevWorldMatrix` applied to the same object-space point |
| Empty pixels (depth 0) | the point at infinity in the pixel's view direction: camera rotation only, no translation |
| No history | zero motion, and the frame reports a history reset (§4) |
| Depth | reverse Z; DLSS creation sets `DepthInverted` |

`RG16F` holds sub-pixel precision for small motions (11-bit mantissa: 1/2048 px below 1 px) and about 0.5 px at 1000 px of motion. Both are within what DLSS tolerates.

---

## 2. Current implementation

- `resolve.slang` `motionPixels(currentClip, previousClip, targetSize)`: `(prevNdc − curNdc) · 0.5 · targetSize`; zero when either `w ≤ 0` (also covers the first frame, where `prevViewProj` is zero).
- `currentClip = viewProj · surface.position`, `previousClip = prevViewProj · surface.prevPosition`; `surface.prevPosition = prevWorldMatrix · objectPos` (`buildSurface`).
- Empty pixels use `w = 0` directions through `viewProj` and `prevViewProj`.
- `Camera::updateCameraData` copies last frame's `viewProj` into `prevViewProj`; `jitterOffset` is applied in shaders through `jitterClip`, so `viewProj` stays unjittered.
- `ObjectStorage::prevModelMatrices` is per entity and advanced in `ResourceManager::updateUniformBuffers` after `writeNodeInstances` has read it, so it follows the entity through unload compaction.
- Debug view `MotionVectors` shows `|motion| / 16` as red/green.

---

## 3. Bugs found

| # | Bug | Effect | Fix |
|---|---|---|---|
| B1 | `ObjectStorage::create` initializes `prevModelMatrices` to **identity** ([object_storage.cpp:16](../../src/core/object_storage.cpp#L16)) | a newly loaded entity's first frame has motion from the world origin to its position; the early-phase Hi-Z test (which uses `prevWorldMatrix`) tests the wrong place for one frame | initialize to `computeModelMatrix(transform)` at create, so a new entity has zero motion |
| B2 | a camera cut or projection change (`Camera::isCut()`: teleport, resize, FOV change) keeps the old `prevViewProj` | one frame of huge camera motion; after resize the previous guide set has undefined content of the old size | on a cut frame set `prevViewProj = viewProj` and report a history reset (§4) |
| B3 | inactive entities skip the prev update in `updateUniformBuffers` | reactivation after a pause jumps by the whole pause's motion | advance `prevModelMatrices` for inactive entities too, or set it to the current matrix on activation |
| B4 | `drawFrame` can return after `updateCameraData` (swapchain out of date); since present got its own submit, the frame work still runs, but its image is never presented | the next frame's `prevViewProj` is a frame that was never shown | treat a recreate as a cut (B2) |

---

## 4. History reset

One per-frame bit, `kFrameFlagHistoryReset` in a new `GpuFrameRoot::frameFlags` field (added with the plan's A1 fields), set on the CPU when:

- `Camera::isCut()` (teleport, projection change, resize);
- the render extent changed (`RenderTargets::recreate`);
- the swapchain was recreated (B4);
- the scene layout changed in a way that invalidates last frame's rows (load or unload): the `visibleRow` and node-instance rows of last frame no longer mean the same thing.

Consumers: DLSS `InReset = 1`; RTXDI / ReSTIR GI skip temporal resampling for that frame; the cache keeps training (world space, not screen space).

---

## 5. Verification: reprojection debug view

A new `DebugView::Reprojection` (Guides category) proves the vectors without DLSS:

```
prev  = previous.DiffuseAlbedo.SampleLevel(linear, (pixel + 0.5 + motion) / renderSize, 0)
cur   = current.DiffuseAlbedo[pixel]
error = abs(cur − prev)           // shown ×4; disocclusions light up, everything else is black
```

Both guide sets already exist, so this needs no new images. Albedo is jitter-free and lighting-free, so a correct field shows black everywhere except at disocclusion edges and when the camera leaves the previous frame's view.

| Test | Pass when |
|---|---|
| static camera, static scene | black, `motion = 0` everywhere |
| camera yaw only | black, including the sky (direction-only motion) |
| camera translation | black except disocclusion slivers behind silhouettes |
| entity moved or rotated through the debug UI transform while the camera stands still | black on the entity |
| load a model while moving | no full-entity flash on its first frame (B1) |
| teleport / resize / FOV change | the reset frame shows zero motion, no flash (B2) |
| mirrored instance (negative scale) | black (motion does not depend on winding) |
| jitter on (M3) | still black: jitter is excluded on both sides |

---

## 6. Tasks

| # | Task | Milestone |
|---|---|---|
| MV1 | fix B1–B4 | M3, first |
| MV2 | `kFrameFlagHistoryReset` in `GpuFrameRoot` (with the A1 fields), set per §4 | M3 |
| MV3 | `DebugView::Reprojection` (§5) and the test list | M3 |
| MV4 | jitter (plan E1): Halton(2,3), phase count `8 · (display / render)²`; verify §5 stays black | M3 |
| MV5 | DLSS-RR inputs: `MVLowRes`, `DepthInverted`, `InMVScale = 1`, `InJitterOffset`, `InReset` (plan H1) | M3 |

---

## 7. Must not

- include jitter in the vectors, or apply jitter to `viewProj` on the CPU;
- compute motion from the rasterizer's interpolated position of the previous frame (there is none: always reproject the current surface point);
- store previous matrices per node-instance row (rows shift on unload; matrices follow the entity);
- skip the reset on a cut and let temporal passes reuse unrelated history.
