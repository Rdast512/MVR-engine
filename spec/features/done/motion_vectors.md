# Feature: motion vectors

**Date:** 2026-11-08, updated 2026-11-13 · **Status:** computed in the resolve (M1); history bugs fixed, reset flag and reprojection view in (MV1–MV3); the §5 test list runs as a scripted check and passes (§5.1); MV4–MV5 done with DLSS-RR (plan H1, 2026-11-14) · **Milestone:** M3 ([engine_spec.md](../../../docs/engine_spec.md) §16.2), before DLSS-RR and RTXDI temporal reuse
**Implements:** engine spec §9.2 step 3, §9.3 `motion`, §12.2
**Related:** [visibility_buffer.md](./visibility_buffer.md) §3.6, §10; [pbr_implementation_plan.md](../../pbr_implementation_plan.md) E1, H1

Every temporal consumer (DLSS-RR, RTXDI and ReSTIR GI temporal reuse) finds a pixel's previous-frame position through the `Motion` guide. Wrong motion vectors show up as ghosting and smearing after DLSS, and as reuse of the wrong reservoirs, which looks like lighting noise rather than a motion bug. This spec fixes the convention, closes the history bugs, and adds a debug view that proves the vectors right.

---

## 1. Convention

| Item | Rule |
|---|---|
| Image | `Motion` guide, `RGBA16F` (xy motion; z = view depth there − here, for RTXDI since PBR F2), render resolution, both guide sets |
| Unit | render-resolution pixels (DLSS `InMVScaleX/Y = 1`, feature flag `MVLowRes`) |
| Direction | current → previous: `previousPixel = currentPixel + motion` |
| Jitter | excluded: both positions use the **unjittered** `viewProj` / `prevViewProj`. DLSS gets the jitter separately (`InJitterOffsetX/Y`, render pixels); `MVJittered` is not set |
| Surface point | the jittered sample's world position (barycentrics from the jittered corners), projected with unjittered matrices. This is what DLSS expects |
| Object motion | `GpuNodeInstance::prevWorldMatrix` applied to the same object-space point |
| Empty pixels (depth 0) | the point at infinity in the pixel's view direction: camera rotation only, no translation |
| No history | zero motion, and the frame reports a history reset (§4) |
| Depth | reverse Z; DLSS creation sets `DepthInverted` |

Half floats hold sub-pixel precision for small motions (11-bit mantissa: 1/2048 px below 1 px) and about 0.5 px at 1000 px of motion. Both are within what DLSS tolerates.

---

## 2. Current implementation

- `resolve.slang` `motionPixels(currentClip, previousClip, targetSize)`: `(prevNdc − curNdc) · 0.5 · targetSize`; zero on a history-reset frame (§4) and when either `w ≤ 0`.
- `currentClip = viewProj · surface.position`, `previousClip = prevViewProj · surface.prevPosition`; `surface.prevPosition = prevWorldMatrix · objectPos` (`buildSurface`).
- Empty pixels use `w = 0` directions through `viewProj` and `prevViewProj`.
- `Camera::updateCameraData` copies last frame's `viewProj` into `prevViewProj`, or this frame's on a cut; `jitterOffset` is applied in shaders through `jitterClip`, so `viewProj` stays unjittered.
- `ObjectStorage::prevModelMatrices` is per entity and advanced in `FrameData::write` after `writeNodeInstances` has read it, so it follows the entity through unload compaction.
- Debug view `MotionVectors` shows `|motion| / 16` as red/green.
- Debug views `MotionVectors` and `Reprojection` show entity transform controls (`Engine::drawEntityTransformControls`) for the §5 cases.

---

## 3. Bugs found

All fixed (MV1). B4 needs no code of its own: every recreation goes through `Renderer::rebuildSwapchainResources` → `Camera::setRenderExtent`, which makes the next frame a cut.

| # | Bug | Effect | Fix |
|---|---|---|---|
| B1 | `ObjectStorage::create` initializes `prevModelMatrices` to **identity** ([object_storage.cpp:16](../../../src/core/object_storage.cpp#L16)) | a newly loaded entity's first frame has motion from the world origin to its position; the early-phase Hi-Z test (which uses `prevWorldMatrix`) tests the wrong place for one frame | initialize to `computeModelMatrix(transform)` at create, so a new entity has zero motion |
| B2 | a camera cut or projection change (`Camera::isCut()`: teleport, resize, FOV change) keeps the old `prevViewProj` | one frame of huge camera motion; after resize the previous guide set has undefined content of the old size | on a cut frame set `prevViewProj = viewProj` and report a history reset (§4) |
| B3 | inactive entities skip the prev update in `updateUniformBuffers` | reactivation after a pause jumps by the whole pause's motion | advance `prevModelMatrices` for inactive entities too, or set it to the current matrix on activation |
| B4 | `drawFrame` can return after `updateCameraData` (swapchain out of date); since present got its own submit, the frame work still runs, but its image is never presented | the next frame's `prevViewProj` is a frame that was never shown | treat a recreate as a cut (B2) |

---

## 4. History reset

One per-frame bit, `kFrameFlagHistoryReset` in `GpuFrameRoot::frameFlags` (`FrameFlag::HistoryReset` on the CPU), set in `Renderer::drawFrame` when:

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

### 5.1 Scripted check (2026-11-13)

`ENGINE_MOTION_TEST=1 ./engine.exe` runs the table above on the startup Sponza and quits; results are `[MotionTest]` lines in `logs/engine.log`. `EngineMotionTest` (`src/core/engine_motion_test.*`) drives the camera and entity 0 for 24 frames per scenario; after every frame (`waitIdle`) it reads `GpuMotionCheckStats`, written by `passes/motion_check.slang`, which runs after the debug view only in this mode. The pass checks two things per pixel:

- **camera-only motion:** the world point rebuilt from depth (a direction for empty pixels) projected with `prevViewProj`. While no entity moves, every pixel's motion must match it; this is exact, independent of textures, and covers disocclusions and the sky;
- **albedo reprojection:** the §5 debug-view comparison, counted against the same comparison at zero motion. Used for entity motion, where the camera-only motion does not apply: correct motion must remove at least 75% of the change.

| Test | Scenario | Rule | Result on the RTX 5070 |
|---|---|---|---|
| static camera, static scene | still | max motion ≤ 1e-3 px, albedo identical | 2.2e-5 px: float noise of two matrix chains, not exactly 0 |
| camera yaw (incl. the sky) | yaw; yaw from the courtyard, 18% sky | camera-only motion within 0.05 px + 0.1%; at most 1e-5 of the pixels outside, none over 0.5 px | max 0.010 px; sky view: 1 outlier px at 0.2 px |
| | pitch + yaw | same | max 0.009 px |
| camera translation | right + forward | same | max 0.017 px |
| entity moved or rotated | translate, rotate entity 0 | reprojection leaves ≤ 25% of the zero-motion change | 0.27% vs 4.1%; 0.22% vs 2.5% of pixels |
| mirrored instance | entity 0 at scale x −1: camera yaw, entity rotation | as camera / entity | max 0.013 px; 0.10% vs 2.3% |
| load a model while moving | load at frame 8 during camera translation | the load frame resets with zero motion; later frames match the camera-only motion, the new model included | pass, max 0.017 px |
| teleport / FOV change / resize | cut at frame 8 | the reset frame has zero motion; the other frames are still | pass, one reset frame each |
| jitter on (M3) | — | — | not built |

Each rule was shown to fail on a deliberately broken resolve: the previous position from the current matrix fails every entity scenario; motion kept on reset frames fails every cut and the load; `viewProj` for empty pixels fails the sky view (169k px per frame, up to 6.4 px). B1 cannot be seen in motion vectors: a load resets history, so the frame where a new entity's previous matrix would be wrong writes zero motion anyway. B1 still matters for the early Hi-Z test.

The isolated outliers are pixels where the resolve's interpolated point lands up to 0.2 px off the pixel ray (seen once per scenario at most, on 4–9 m geometry): a precision limit of the barycentrics on thin triangles, not a motion bug. Validation (core, sync, best practices) is clean over the whole run.

---

## 6. Tasks

| # | Task | Milestone |
|---|---|---|
| MV1 | fix B1–B4 (**done**) | M3, first |
| MV2 | `kFrameFlagHistoryReset` in `GpuFrameRoot`, set per §4 (**done**) | M3 |
| MV3 | `DebugView::Reprojection` (§5) and the test list (**done**: scripted, §5.1) | M3 |
| MV4 | jitter (plan E1): Halton(2,3), phase count `8 · (display / render)²`; verify §5 stays black. **Done:** on by default since H1; motion stays unjittered, and the motion test runs with jitter off because it compares albedo pixel by pixel | M3 |
| MV5 | DLSS-RR inputs: `MVLowRes`, `DepthInverted`, `InMVScale = 1`, `InJitterOffset`, `InReset` (plan H1). **Done:** `InJitterOffset = jitterOffset · render / 2` (same sign as the raster offset, measured); `InReset` on the frame's reset, a camera cut, a layout change, a resize or a frame without Lit | M3 |

---

## 7. Must not

- include jitter in the vectors, or apply jitter to `viewProj` on the CPU;
- compute motion from the rasterizer's interpolated position of the previous frame (there is none: always reproject the current surface point);
- store previous matrices per node-instance row (rows shift on unload; matrices follow the entity);
- skip the reset on a cut and let temporal passes reuse unrelated history.
