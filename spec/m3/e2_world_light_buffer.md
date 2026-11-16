# M3 · E2 — GPU world-light buffer

**Status** — Punctual lights built on 2026-11-14. Emissive triangles are M7.\
**Implements** — PBR E2 and engine spec §6 and §11.2.\
**Used by** — [F1 context](f1_rtxdi_context.md), [F2 bridge](f2_rtxdi_bridge.md) and [E3 sun](e3_engine_sun.md).
[PBR plan](../pbr_implementation_plan.md)

## Frame contract

- `AssetBuffers` keeps per-entity punctual rows in model space and
  partitions them on scene changes. Point and spot rows come first,
  then directional rows. `localLightCount` gives the split.
- `light_world` converts one row per GPU invocation. It transforms
  position by the entity matrix and transforms and normalizes
  direction. Color, intensity, range and cone values stay authored.
  E3 appends the engine sun to the directional region.
- `WorldLights` owns device-local buffers by frame slot. It publishes
  addresses through the frame root, grows in powers of two and waits
  for outstanding ray readers before replacing a buffer. The pass is
  recorded on async compute before the PTLAS build. Readers use the
  compute timeline. F2 handles previous-frame reads across slot reuse.

[World lights](../../src/render/vk_world_lights.cpp) · [GPU conversion](../../shaders/passes/light_world.slang) · [Asset light upload](../../src/core/vk_asset_buffers.cpp) · [Renderer submits](../../src/render/vk_renderer.cpp)

## Verification

`Unshadowed lights` sums the guide-based BRDF and punctual contributions.
On RTX 5070 the guide capture checked `LightsPunctualLamp` and
`DirectionalLight`. A matched 90° lamp and camera yaw kept the highlight
with the mesh. Omitting the entity transform in the test build changed
44,299 lit pixels, compared with 11 above the same 8/255 threshold with
the transform.
[PBR plan, E2](../pbr_implementation_plan.md) · [Debug views](../../shaders/passes/debug_view.slang)
