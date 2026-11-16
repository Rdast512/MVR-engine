# M1 · Motion vectors and history

**Status** — Resolve motion and scripted verification complete for M1. M3
adds jittered raster, RTXDI depth delta and DLSS-RR input wiring.\
**Implements** — Visibility-buffer V6 and PBR E1.
[Motion-vector feature](../features/motion_vectors.md) · [PBR plan](../pbr_implementation_plan.md)

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
[Motion check shader](../../shaders/passes/motion_check.slang) · [Scripted test](../../src/core/engine_motion_test.cpp) · [Motion results](../features/motion_vectors.md#51-scripted-check-2026-11-13) · [Considerations](../considerations.md)

## Log

| Date | Change |
|---|---|
| 2026-11-13 | Scripted motion and reset checks recorded for M1 closure |
| 2026-11-14 | M3 connected the motion guide to DLSS-RR and RTXDI |
