# M1 · V8 — Visibility payload comparison

**Status** — Deferred. M1 shipped the 32-bit payload.\
**Implements** — Visibility-buffer V8 and engine spec Q3.\
**Depends on** — [Visibility raster and payload](v4_v5_visibility_raster.md) and [E1 resolve](e1_resolve_guides.md).
[Visibility-buffer feature](../features/visibility_buffer.md) · [Engine spec §8.3 and Q3](../../docs/engine_spec.md)

## Comparison

Use the existing `R32_UINT` path as the baseline. The proposed comparison
build uses `R32G32_UINT`, with the visible-list row in x and the triangle
and any payload flags in y. Use the same rasterized geometry, reverse-Z
background rule, resolve math and guides for both encodings.

Measure raster time, resolve time and bandwidth on the RTX 5070 and
RTX 2060 Super under matching scene, resolution and Release settings.
Use the culling image check and guides as correctness checks.
[Payload helpers](../../shaders/geometry/visbuffer.slang) · [Baseline procedure](../m0/a10_culling_baseline.md)

## Decision

Record the comparison under Q3. The 32-bit payload fits the planned L1
packed 64-bit depth-and-payload atomic. A format change needs measured
benefit and a supported path for L1.

## Log

| Date | Change |
|---|---|
| 2026-11-08 | V8 deferred and 32-bit payload retained |
