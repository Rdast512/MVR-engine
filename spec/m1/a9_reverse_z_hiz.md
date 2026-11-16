# M1 · A9 — Reverse Z and Hi-Z

**Status** — Built and verified on RTX 5070.\
**Implements** — Visibility-buffer V2–V3 and engine spec §8.1 A9.\
**Depends on** — [M0 A10 culling capacities](../m0/a10_culling_baseline.md).
[Visibility-buffer feature](../features/visibility_buffer.md) · [Culling comparison](../../reference/culling_comparison.md)

## Depth and pyramid

- The camera uses a finite reverse-Z projection. Raster clears `D32_SFLOAT`
  depth to 0 in the early phase, uses `GREATER`, and loads depth in the late
  phase. Depth 0 identifies background throughout the resolve.
- The `R32_SFLOAT` pyramid stores farthest depth by min reduction. Its base
  dimensions are the previous power of two per axis, and mip 0 reduces
  the source footprint of up to three depth texels per axis. Higher levels
  halve down to 1×1.
- `hiz_build` writes up to three mips per dispatch from an 8×8 tile using
  groupshared reductions. Each dispatch's last written mip is visible to
  the next dispatch. The sampled pyramid uses a min-reduction sampler.
- `isHiZOccluded` chooses a mip whose filter footprint covers the projected
  box, samples once at its center and culls when the nearest reverse depth
  is below the stored minimum by more than the depth nudge. Bounds crossing
  the near or eye plane skip the occlusion test.

[Camera](../../src/scene/vk_camera.cpp) · [Raster state](../../src/render/vk_shader_objects.cpp) · [Hi-Z host](../../src/render/vk_hiz.cpp) · [Hi-Z shader](../../shaders/passes/hiz_build.slang) · [Cull tests](../../shaders/core/culling.slang)

## Two-phase history

1. Early frustum and size tests use the current cull view. Early occlusion
   tests against the previous pyramid using previous view-projection and
   previous instance transforms.
2. The early raster builds fresh depth and Hi-Z. Late culling re-tests early
   instance candidates and the rejected meshlets identified by one bitmask
   per draw record. It uses the current transforms and fresh pyramid.
3. Late raster adds the newly visible geometry. Its Hi-Z build produces
   the pyramid for the next frame and the pyramid debug view.

Missing history, a camera cut or a scene-layout change bypasses early
occlusion. Freeze culling keeps the view and pyramid together, while depth
still transitions for later passes. M3 jitter uses unjittered cull matrices
and widens the conservative subpixel bounds.
[Cull host](../../src/render/vk_meshlet_culling.cpp) · [Cull passes](../../shaders/passes/meshlet_cull.slang) · [Renderer](../../src/render/vk_renderer.cpp)

## Verification

`hiz_footprint` exercises mip and sample-footprint selection in reverse Z.
`ENGINE_CULL_CHECK=1` compares reverse-Z images across 16 Sponza poses with
all tests on or off, including a turn before the culled capture. All 16
matched in depth and albedo on the RTX 5070. A deliberately over-culling
Hi-Z test failed 14 poses. Timing was recorded for the combined cull,
DGC preprocess and Hi-Z stages.
[Footprint test](../../tests/hiz_footprint_test.cpp) · [Cull check](../../src/core/engine_cull_check.cpp) · [Engine spec §2.3 and changelog](../../docs/engine_spec.md)

C-M1-1 records the accepted `BestPractices-NVIDIA-CreateImage-Depth32Format`
warning. Floor-tier verification is tracked under M0.
[Considerations](../considerations.md) · [Depth format](../../src/render/vk_render_targets.cpp)

## Log

| Date | Change |
|---|---|
| 2026-11-13 | Recorded the reverse-Z culling comparison and RTX 5070 baseline |
