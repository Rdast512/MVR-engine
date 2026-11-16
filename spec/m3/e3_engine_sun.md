# M3 · E3 — Engine sun

**Status** — Built on 2026-11-14.\
**Implements** — PBR E3 and the M3 sunlit Sponza acceptance scene.\
**Depends on** — [E2 light rows](e2_world_light_buffer.md) and [B3 light evaluation](b3_punctual_lights.md).
[PBR plan](../pbr_implementation_plan.md) · [Considerations C-E3-1 and C-E3-2](../considerations.md)

## Directional row

`WorldLights::Sun` adds one directional row after the asset rows, in
RTXDI's infinite region. `light_world` writes the row from push data.
Azimuth runs from +X toward +Z. The row stores the direction the light
travels. The defaults are enabled, azimuth 20°, elevation 65°, 10 lux and
white. The Sun panel edits those fields and restores defaults.

`WorldLights` supplies the same totals to the frame root, RTXDI and the
Lit/RR path. Toggling the sun changes the light count and resets temporal
history. Guide captures disable it to preserve the assets' own lighting.
[World lights](../../src/render/vk_world_lights.cpp) · [Sun row](../../shaders/passes/light_world.slang) · [Sun panel](../../src/ui/engine_ui.cpp) · [Guide capture](../../src/core/engine_guide_capture.cpp)

## Verification

The sunlit courtyard shows floor and upper-arch lighting and the
planters' and columns' shadows. The startup camera stays in the
+Z gallery for benchmark continuity. Its roof and curtains block direct
sun across the recorded direction sweep, so gallery bounce lighting
comes with M4.

The original Quality-mode courtyard run recorded 0.35% temporal
deviation of the mean with the camera still. The pose-repeat checks
recorded 2.4% difference for camera yaw and 1.5% for sun motion.
[PBR plan, E3 and H1](../pbr_implementation_plan.md) · [Later settings measurements](../../docs/settings.md) · [M4 combine](../m4/g6_combine.md)
