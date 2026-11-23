# M1 · Motion vectors and history

**Status** — Resolve motion and scripted verification complete for M1. M3
adds jittered raster, RTXDI depth delta and DLSS-RR input wiring.\
**Implements** — Visibility-buffer V6 and PBR E1.
[Motion-vector feature](../features/done/motion_vectors.md) · [PBR plan](../pbr_implementation_plan.md)

## Resolve contract

`Motion.xy` is current-to-previous motion in render-resolution pixels, so
`previousPixel = currentPixel + motion.xy`. Reproject the same object-space
surface point with the two world transforms and the unjittered current and
previous view-projection matrices. Barycentrics locate the point at the
raster sample. The screen motion excludes jitter.

Sky pixels reproject a direction with homogeneous w = 0 so camera translation
has no effect. Their depth delta is zero. A history reset or an invalid
projection behind either camera yields zero motion.
[Resolve](../../shaders/passes/resolve.slang) · [Surface reconstruction](../../shaders/geometry/surface.slang)

## History ownership

- `Camera` provides `prevViewProj` and copies the current matrix into it on
  a cut. Resize and projection changes also trigger a cut.
- `ObjectStorage` keeps previous model matrices by entity and initializes
  them from the spawn transform. `FrameData` reads them for previous node
  transforms before advancing them, including for inactive entities.
- `Renderer` sets `FrameFlag::HistoryReset` on camera cuts and scene-layout
  changes. The current renderer also resets on changes to the light set.
  Recreated render targets reset through the camera's render-extent path.
- In M3, DLSS-RR gets jitter separately. RTXDI uses the view-depth delta in
  `Motion.z` for temporal reprojection.

[Camera](../../src/scene/vk_camera.cpp) · [Object storage](../../src/core/object_storage.cpp) · [Frame data](../../src/core/vk_frame_data.cpp) · [Renderer](../../src/render/vk_renderer.cpp)

## Reprojection logic

For a covered pixel, transform the same interpolated object-space
point using both world transforms and both unjittered cameras.
Motion is `(prevClip.xy / prevClip.w − curClip.xy / curClip.w) × renderSize / 2`.
A reset or invalid clip W writes zero. Sky projects a direction
with w = 0, so camera translation drops out. F2 writes
`prevClip.w − curClip.w` in z for covered pixels.

Reprojection debug samples prior albedo at `pixel + motion` and shows
absolute RGB error. Camera-only validation independently reconstructs
a world point from current depth. When the object moves, the
script compares albedo reprojection against the zero-motion control.
[Debug view](../../shaders/passes/debug_view.slang) · [Feature scenarios](../features/done/motion_vectors.md)

## Tasks and recorded diagnostics

| Task | Result |
|---|---|
| Preserve previous transforms through spawn and reactivation | Spawn initializes from its current matrix. Inactive history advances. Mirrored and moving tests passed. |
| Suppress reuse after cuts, layout changes and resize | Scripted cuts each had one reset frame with zero motion. |
| Check camera and entity reprojection | Translation ≤ 0.017 px and mirrored camera yaw ≤ 0.013 px. Entity translation reduced changed pixels from 4.1% to 0.27%, rotation from 2.5% to 0.22%. |
| Validate the test's sensitivity | Current-only previous matrices failed moving cases. Keeping motion on invalid history failed cut/load cases. Current-camera sky projection failed sky yaw. |

## Verification

`ENGINE_MOTION_TEST=1` drives the startup Sponza scene and logs
`[MotionTest]` results. The check uses depth-reconstructed reprojection for
camera-only motion and compares albedo reprojection with the zero-motion
case for entity motion. Scenarios cover still frames, camera motion with
sky, translated and rotated entities, mirrored transforms, a load while
moving, and teleport, resize and FOV resets.

The recorded jitter-off baseline passed all 13 scenarios on RTX 5070.
Still frames measured 2.2×10⁻⁵ px maximum motion. C-T-1 and C-T-3 document
why GPU-assisted validation and forced jitter change the scripted pass/fail
results. Use the default jitter-off run with GPU-AV disabled for comparison
to the recorded baseline.
[Motion check shader](../../shaders/passes/motion_check.slang) · [Scripted test](../../src/core/engine_motion_test.cpp) · [Motion results](../features/done/motion_vectors.md#51-scripted-check-2026-11-13) · [Considerations](../considerations.md)

## Log

| Date | Change |
|---|---|
| 2026-11-13 | Scripted motion and reset checks recorded for M1 closure |
| 2026-11-14 | M3 connected the motion guide to DLSS-RR and RTXDI |
