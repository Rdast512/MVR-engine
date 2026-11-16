# M1 · V2 — Reverse-Z depth convention

**Status** — Built. Acceptance recorded on 2026-11-13.\
**Depends on** — [V1 targets](v1_render_targets.md).
[Engine spec §8.2](../../docs/engine_spec.md) · [A9 occlusion and history](a9_reverse_z_hiz.md)

## Decision and coupled changes

Finite reverse Z maps physical near to depth 1 and far to 0. Swap the
near and far arguments when constructing the projection. Keep their
physical meanings in camera data. Use `D32_SFLOAT` for precision, clear
depth to 0 and compare `GREATER`. C-M1-1 records the accepted D32 warning.

| Consumer | Rule |
|---|---|
| Raster | Early clears depth, late loads it. Both use the reverse comparison. |
| Resolve | Sample depth first. Zero is background. The vis payload is undefined there. |
| Culling | The near plane corresponds to the upper clip-z bound. A bound crossing the near or eye plane bypasses Hi-Z. Compare the nearest bound depth against the pyramid minimum with a conservative nudge. |
| Hi-Z and debug | V3 builds min depth. Debug depth and Hi-Z interpret the same reverse convention. |

Projection, depth state and every depth reader must agree. Changing the
cull comparison independently can reject real coverage even when raster
alone yields the expected image.
[Camera](../../src/scene/vk_camera.cpp) · [Raster state](../../src/render/vk_shader_objects.cpp) · [Cull tests](../../shaders/core/culling.slang) · [Considerations](../considerations.md)

## Tasks and recorded results

| Task | State and evidence |
|---|---|
| Switch projection, clear, compare and depth readers | Done. Current raster and resolve and recorded guide captures. |
| Keep one culling convention | Done. AU5 removed forward Z. The footprint test uses reverse Z. |
| Compare coverage with culling on and off | RTX 5070 done. All 16 Sponza poses matched depth and albedo. An intentionally over-culling build failed 14 poses. |

The reverse-Z image comparison replaced the proposed forward-Z
counter comparison as the acceptance gate.
[Baseline](../m0/a10_culling_baseline.md) · [Footprint test](../../tests/hiz_footprint_test.cpp)
