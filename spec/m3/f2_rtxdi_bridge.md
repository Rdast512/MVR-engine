# M3 · F2 — RTXDI surface and light bridge

**Status** — Built on 2026-11-14. Previous world-light rows wired on 2026-11-15.\
**Implements** — PBR F2 and engine spec §11.2.\
**Depends on** — [B2 BRDF](b2_brdf.md), [B3 light evaluation](b3_punctual_lights.md), [E1 guides](e1_guide_followups.md), [E2 world lights](e2_world_light_buffer.md), [F1 constants](f1_rtxdi_context.md) and [M2 PTLAS](../m2/c4_partitioned_tlas.md).\
**Used by** — [F3 ReSTIR DI](f3_restir_di.md).
[PBR plan](../pbr_implementation_plan.md) · [Considerations C-F2-1–3 and C-F3-1](../considerations.md)

## SDK interface

- Include `lighting/rtxdi_bridge.slang` before the RTXDI DI headers.
  It adapts SDK resources to BDA and guide descriptors. Guard reservoir
  and light indices at the BDA boundary. Presampling is disabled for
  the punctual-light path.
- `RAB_GetGBufferSurface` reads current or previous guides. The
  ping-ponged `Position` guide stores world XYZ and positive view depth in
  w. `Motion.z` is previous minus current view depth. View directions
  use the camera position of the matching frame. Unlit surfaces score
  zero for light sampling.
- `RAB_LoadLightInfo` selects the matching frame's world-light rows.
  `prevWorldLights` is zero on a history reset. RTXDI's local and
  infinite regions use the same row indices across valid history.
  Slot reuse and buffer growth wait for readers of the previous rows.
- Punctual samples use B3. The target PDF is the luminance of the same
  BRDF × incident radiance × cosine term used for final shade. Visibility
  uses a geometric-normal offset from the resolved surface and a ray
  query against the current PTLAS, including temporal visibility rays.
- Slang warning E30081 is disabled for bridge compile units under C-F2-2.

[Bridge](../../shaders/lighting/rtxdi_bridge.slang) · [Visibility ray](../../shaders/raytracing/trace.slang) · [Guide writer](../../shaders/passes/resolve.slang) · [Frame data](../../src/core/vk_frame_data.cpp) · [Frame ordering](../../src/render/vk_renderer.cpp)

## Callback and history design

| Callback or helper | Reads | Output or invariant |
|---|---|---|
| `rtxdiBridgeInit` and constants access | Frame root, target sets, cameras, PTLAS, world-light and SDK buffer addresses | Initializes the bridge for the shader invocation. SDK macros resolve to bounded BDA resources. |
| `RAB_GetGBufferSurface` | Frame-selected guides and matching camera position | Geometry, shading normals, material and positive view depth. An empty guide is an invalid surface. |
| `RAB_LoadLightInfo` and light sampling | Current or previous world row for the requested ID | `evalLight` returns an analytic punctual sample. This class has no finite solid-angle PDF. |
| `shadeLightSample` and target PDF | Valid lit surface and punctual sample | Linear BRDF × incident light × surface cosine. PDF target uses the luminance of that contribution. |
| Conservative visibility | Resolved position, geometric normal, sampled direction and distance, current PTLAS | Offset along the geometric-normal side of the ray. Trace first-hit visibility. The temporal variant uses the previous surface. |

Previous surfaces need exact resolved positions even though primary
depth is single-buffered. The position guide adds 16 B per pixel
per set, with previous camera position and motion z supplying
frame-correct view and depth validation. Previous light rows preserve
motion of the light itself. A layout/light-set reset disables temporal
reuse before old IDs or views can be interpreted. The next compute
submit waits for the preceding graphics ray work before it overwrites
world-light storage that could still be read as previous history.
[World-light write](../../src/render/vk_world_lights.cpp) · [History and submits](../../src/render/vk_renderer.cpp)

## Tasks and recorded results

| Task | State and evidence |
|---|---|
| Bridge the pinned SDK resource and callback interface | Done. Include/probe compiled at bring-up. Bounds checks protect BDA and E30081 handling is C-F2-2. |
| Match shading and shadow reference | Done. Forced-visible lamp differed at 20 of 58,206 pixels by 1/255. Temporary origin test recorded zero self-hits. |
| Validate previous-surface depth | Done. With motion z, 0.28–0.63% exceeded 1% depth error in the motion probe. Zeroing motion z raised that to 39% under camera translation. |
| Preserve previous light rows | Done on 2026-11-15. Static-lamp convergence stayed unchanged. C-F2-1 records the fix. |

## Verification

`Shadowed lights` is the exhaustive punctual reference for F3. F2's
bring-up reproduced `Unshadowed lights` with visibility forced on,
checked temporal depth agreement under motion and recorded zero self-hits
in the temporary raster-origin check. C-F2-3 explains the inward-facing
`DirectionalLight` fixture. Use the lamp and courtyard captures for
shadow checks.
[PBR plan, F2](../pbr_implementation_plan.md) · [Debug views](../../shaders/passes/debug_view.slang)
