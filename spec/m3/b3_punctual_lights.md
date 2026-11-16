# M3 · B3 — Punctual-light evaluation

**Status** — Built on 2026-11-13.\
**Implements** — PBR B3 and the engine's `KHR_lights_punctual` contract.\
**Used by** — [E2 world lights](e2_world_light_buffer.md) and [F2 bridge](f2_rtxdi_bridge.md).
[PBR plan](../pbr_implementation_plan.md) · [PBR material and light rules](../pbr_lighting_materials.md)

## Light sample

`evalLight` consumes a world-space `GpuLight` and shading position. It
returns the direction toward the light, incident radiance before `N·L`
and the distance used for visibility.

| Kind | Evaluation |
|---|---|
| Directional | Use the opposite of the authored pointing direction. Color times illuminance in lux, with a finite maximum ray distance. |
| Point | Color times candela divided by squared distance, with a near-distance floor and a clamped `1 − (d/range)^4` window. A nonpositive range leaves the window at one. |
| Spot | Same distance behavior as point, times the squared, clamped interpolation between the outer and inner cone cosines. |

The shared evaluator owns the photometric math. Consumers apply BRDF,
cosine and visibility. Entity scale leaves intensity, range and cone
parameters in their authored units.
[Light module](../../shaders/lighting/lights.slang) · [World transform](../../shaders/core/frame.slang)

## Verification

The `lights` CTest evaluates GPU worked cases for attenuation, range,
spot falloff, directions and ray distances. Guide captures exercise the
lamp and directional sample through E2.
[Host test](../../tests/lights_test.cpp) · [Test shader](../../tests/shaders/lights_numbers.slang)
