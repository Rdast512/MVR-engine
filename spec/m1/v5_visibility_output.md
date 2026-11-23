# M1 · V5 — Visibility output and debug presentation

**Status** — Built. Separate present submit landed on 2026-11-08.\
**Depends on** — [V1 targets](v1_render_targets.md) and [V4 bins](v4_raster_bins.md).
[Visibility-buffer V5](../features/done/visibility_buffer.md) · [Raster integration](v4_v5_visibility_raster.md)

## Decision and producer/consumer edges

The payload uses 7 low bits for the meshlet-local triangle and 25 high
bits for the visible row. `lateVisibleBase` separates phase ranges. The
fragment shader writes the per-primitive integer unchanged. Depth is
the coverage flag, allowing early raster to discard old vis contents
and late raster to load early results.

| Edge | Contract |
|---|---|
| Cull to raster | Build and preprocess GPU sequences using the state for the matching execute. |
| Raster to resolve | Color writes transition to compute sampled reads. The late Hi-Z phase already made depth readable. |
| Resolve to debug/present | Guide writes become compute-readable. Debug output becomes fragment-readable. |
| Acquire to present | Offscreen work is queued before acquire. The present submit waits at color output and signals the slot fence. On out-of-date acquire an empty submit signals that fence. |

Debug IDs unpack the resolve's payload. Edges use analytic barycentrics.
Cull phase uses the row boundary. The Hi-Z overlay samples the final
pyramid during present. ImGui draws after fullscreen present in the
same rendering scope.
[Payload ABI](../../shaders/geometry/visbuffer.slang) · [Debug views](../../shaders/passes/debug_view.slang) · [Present](../../shaders/passes/present.slang) · [Renderer](../../src/render/vk_renderer.cpp)

## Tasks and recorded results

| Task | State and evidence |
|---|---|
| Emit vis payload and rebuild ID/edge views | Done. Current raster and compute debug path. |
| Show the final pyramid in present | Done. V5 as-built notes and present overlay. |
| Split acquire-dependent present | Done. Core and sync validation clean through repeated resize in the feature record. |

[As-built validation](../features/done/visibility_buffer.md) · [V7 timings](v7_cleanup_and_timings.md)
