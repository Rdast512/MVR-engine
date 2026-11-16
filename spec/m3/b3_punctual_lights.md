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

## Numerical contract

| Quantity | Rule |
|---|---|
| Finite direction and distance | `d² = max(dot(toLight,toLight), 1e−12)`. Use `sqrt(d²)` for ray distance and vector normalization. |
| Point and spot attenuation | Use `rangeWindow / max(d², 1e−4)`. This keeps coincident queries finite. |
| Spot angle | Use `dot(light.direction, −L)`. Divide its difference from outer-cone cosine by `max(1e−3, cos(inner) − cos(outer))`, saturate and square. |
| Directional shadow ray | Use `kInfiniteDistance`, the largest finite float. The light direction and radiance are independent of point position. |

Light rows store linear color and normalized world direction after
E2. The return contract deliberately leaves the BRDF and surface
cosine to the shading caller. The local distance stays available
for the shadow ray even when attenuation is zero.

## Tasks and acceptance

| Task | State and evidence |
|---|---|
| Implement one photometric path for direction, range and cone | Done. GPU worked cases compare radiance, light direction and distance. |
| Share evaluation across debug and DI | Done. E2 verifies scene placement. F2 uses the same B3 values for target evaluation and final shading. |
| Keep boundary behavior defined | Done. Unit fixtures cover the range and cone edges alongside direction-only and inverse-square cases. |

## Verification

The recorded `lights` CTest covers 14 GPU cases against a
double-precision reference and worked values. A 4 cd point light
at 2 m contributes 1 before range attenuation. At half range the
window is 0.9375. A 30° sample in a 45° spot cone with zero inner
angle uses approximately 0.2944 of the on-axis intensity. Range
and cone boundaries go to zero. Equal cone angles stay finite.
Flipping the cone dot-product sign failed six checks in the
recorded negative test. E2 covers scene and transform integration.
[Host test](../../tests/lights_test.cpp) · [Test shader](../../tests/shaders/lights_numbers.slang)
