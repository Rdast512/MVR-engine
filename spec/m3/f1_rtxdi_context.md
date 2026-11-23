# M3 · F1 — RTXDI DI host context

**Status** — Built on 2026-11-14 and updated to RTXDI 3.2 on 2026-11-15.\
**Implements** — PBR F1 and engine spec §11.2.\
**Depends on** — [E2 world-light regions](e2_world_light_buffer.md).\
**Used by** — [F2 bridge](f2_rtxdi_bridge.md) and [F3 DI passes](f3_restir_di.md).
[PBR plan](../pbr_implementation_plan.md) · [RTXDI 3.2 update](../features/done/rtxdi_3_2_update.md)

## Context and storage

- `Renderer` owns `RtxdiContext`. Recreate its `ReSTIRDIContext` at the
  render extent with checkerboard off. Call `SetFrameIndex` and prepare
  constants after the frame-slot wait.
- Allocate a device-local reservoir array from the SDK pitch and buffer
  count. Both slots share it in graphics queue order. The SDK supplies
  the pass and history indices. Upload the SDK neighbor-offset table
  once as `float2` rows for BDA.
- Publish one host-visible `GpuRtxdiConstants` block per slot through
  `GpuFrameRoot.rtxdiConstants`. The DI-only 3.2 block was 328 B,
  with the reservoir address at offset 304. M4's GI additions extend
  the current block to 392 B and keep the DI reservoir address at 304.
  It contains the SDK blocks and mode, indices, addresses and bounds.
- Local lights start at zero. Infinite lights start at
  `localLightCount`. Local sampling stays `Uniform` for punctual
  lights. M7 owns `Power_RIS` and presampling.
- Current reuse mode is `TemporalAndSpatial`, with `None` for the
  reference. A resize recreates reservoirs and resets history before
  temporal reuse (C-F1-1).

[Host context](../../src/render/vk_rtxdi.cpp) · [Constants layout](../../src/render/vk_rtxdi.hpp) · [Slang mirror](../../shaders/lighting/rtxdi_constants.slang) · [M4 G2](../m4/g2_indirect_pass.md)

## Decisions and ownership

| Decision | Rule | Reason |
|---|---|---|
| Size from render extent and SDK pitch | Recreate with H1's selected render extent. Allocate SDK reservoir count × array pitch × packed reservoir stride. | Surface tiles and reservoir addressing must describe the same pixel grid. |
| Share the device-local reservoir array | Allocate one array for DI and rotate SDK indices per frame. Keep constants per slot. | F3 uses the previous frame's final reservoir and must order reuse across frames. |
| Publish constants through BDA | Fill one whole block after the slot wait. Copy the SDK parameter blocks in host order. Mirror them in the bridge's Slang unit. | Shader layout is checked at every field. No separate descriptor-set path is needed. |
| Keep two contiguous light regions | Local is `[0, localCount)` and infinite is `[localCount, lightCount)`. | The bridge preserves light IDs across valid history and can translate them by identity. |
| Load offsets once | Convert SDK offset pairs to `float2` for the BDA accessor. | The SDK neighbor table can be shared by every frame and does not depend on scene layout. |

On prepare, apply changed settings, advance the SDK frame index and
publish the new indices and random state. The 3.2 host asserts that
temporal output aliases initial output, and that spatial reads and
writes use distinct buffers when reuse is enabled. A resize discards
reservoir contents and sets a camera cut before the next temporal step.
[Host implementation](../../src/render/vk_rtxdi.cpp) · [F3 ordering](f3_restir_di.md)

## Tasks and recorded results

| Task | State and evidence |
|---|---|
| Allocate and rebuild the DI context | Done. Recorded reservoir-pitch check at 1280×720 and the scripted resize. |
| Advance frame indices and publish per-slot constants | Done. Original 3.1 bring-up checked index rotation and random state. The 3.2 split changed the DI-only ABI from 360 B to 328 B. |
| Supply local and infinite counts | Done. Lamp had five local rows. Directional fixture had one infinite row. |
| Verify host/shader layout and recreate history | Done. `layout` covers constants, and motion/guide runs exercise resize and layout reset. |

## Verification

F1's bring-up checked reservoir pitch over resize, changing frame indices
and light-region counts on the lamp and directional fixtures. The
`layout` CTest checks the current Slang mirror, and F3's guide capture
checks the configured sampling path.
[Layout test](../../tests/layout_test.cpp) · [Considerations](../considerations.md)
