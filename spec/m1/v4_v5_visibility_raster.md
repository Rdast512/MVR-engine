# M1 · V4–V5 — Visibility raster and present

**Status** — Built. V7 cleanup done.\
**Implements** — Engine spec §8.1 A6 and §8.3. Visibility-buffer V4, V5 and V7.\
**Used by** — [E1 resolve](e1_resolve_guides.md).
[Visibility-buffer feature](../features/visibility_buffer.md) · [Engine spec](../../docs/engine_spec.md)

## Raster and payload

| Item | Contract |
|---|---|
| Visibility target | Render-resolution `R32_UINT`. Early raster discards old payloads with `DONT_CARE`. Late raster loads early payloads. |
| Encoding | `(visibleRow << 7) \| triangle`. The low 7 bits are the meshlet-local triangle and the high 25 index this frame's visible list. Late rows start at `lateVisibleBase`. |
| Bounds | C++ and Slang share the fixed 7-bit split. The host checks the combined early and late visible region size against `kMaxVisibleRows` when building cull templates. |
| Coverage | Reverse-Z depth decides if a pixel has a payload. Depth 0 leaves the color value undefined for consumers. |
| Bins | The host populates `Opaque` and tags double-sided draws with `TwoSided`. Alpha-masked materials enter the opaque bin through M1. M6 owns the alpha-tested fragment path. |

The cull pass skips cone culling for `TwoSided` draws. The mesh shader emits
clip position, a per-primitive payload and `SV_CullPrimitive`. For two-sided
back faces it swaps two indices so the same
backface-culling raster state can draw them. The triangle ID stays stable.
Resolve recovers facing from the original corners and mirrored-instance
winding. The fragment shader forwards `SV_PrimitiveID` to the unsigned target.
[Mesh raster](../../shaders/passes/mesh_raster.slang) · [Fragment shader](../../shaders/passes/mesh_fragment.slang) · [Payload helpers](../../shaders/geometry/visbuffer.slang) · [Cull templates](../../src/render/vk_meshlet_culling.cpp)

## Recording and display

- Each phase records culling, explicit DGC preprocessing, dynamic rendering
  with DGC execute, then Hi-Z. The populated opaque bin executes once per phase.
- Late raster loads both attachments. After late raster the visibility target
  transitions from color attachment write to compute sampled read. The
  late Hi-Z pass leaves depth in a shader-readable layout for resolve.
- `debug_view` reconstructs geometry IDs, bins, phase and edges from the
  payload and surface data. It also shows the guides. The Hi-Z overlay in
  present displays the final pyramid.
- M1's fullscreen `present` samples the debug/display image into the sRGB
  swapchain, with ImGui in the same rendering scope. Offscreen work is
  submitted before acquire. The present submit waits on the acquired image
  and signals the slot fence. An out-of-date acquire uses an empty submit to
  signal the fence and requests recreation. M3 adds the lit output source.

[Renderer](../../src/render/vk_renderer.cpp) · [DGC](../../src/render/vk_dgc.cpp) · [Debug views](../../shaders/passes/debug_view.slang) · [Present](../../shaders/passes/present.slang)

## State and synchronization decisions

Preprocessing's phase/bin state command buffer uses the same shaders
and dynamic state as execute. The visibility image carries no facing
flag. Stable triangle identity lets raster resolve and RT share a
corner loader.

The host orders early color writes before late attachment loads and
transitions depth back from early Hi-Z sampling before late testing.
Resolve receives the final color writes through a color-to-compute
barrier. Late Hi-Z leaves depth sampled.

## Tasks and acceptance

| Task | Contract and result |
|---|---|
| Separate bin assignment and winding | [V4](v4_raster_bins.md) records the two-sided checks and M6 handoff. |
| Produce and display a stable visibility payload | [V5](v5_visibility_output.md) documents coverage and resize validation for the present split. |
| Retire the old raster path | [V7](v7_cleanup_and_timings.md) records variant cleanup and phase timing. |

## Verification and remaining work

V7 removed the mesh-debug raster path and obsolete varyings. The RTX 5070
raster baseline was 0.031 ms for both phases at the startup Sponza view.
V8 stays deferred with the 32-bit encoding. A 64-bit comparison belongs
to engine spec Q3, and M0 tracks the RTX 2060 Super baseline.
[M1 acceptance](./README.md#recorded-acceptance) · [Engine spec §2.3 and §18](../../docs/engine_spec.md)

## Log

| Date | Change |
|---|---|
| 2026-11-08 | Acquired-image present and UI moved to their own submit |
| 2026-11-13 | Visibility raster baseline recorded on RTX 5070 |
