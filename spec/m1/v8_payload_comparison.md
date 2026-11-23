# M1 · V8 — Visibility payload comparison

**Status** — Deferred. M1 shipped the 32-bit payload.\
**Implements** — Visibility-buffer V8 and engine spec Q3.\
**Depends on** — [Visibility raster and payload](v4_v5_visibility_raster.md) and [E1 resolve](e1_resolve_guides.md).
[Visibility-buffer feature](../features/done/visibility_buffer.md) · [Engine spec §8.3 and Q3](../../docs/engine_spec.md)

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

## Tasks and acceptance

| Task | State and gate |
|---|---|
| Add a temporary wide path | Deferred. Change integer attachment, fetch and pack/unpack together. Preserve depth-based coverage and all row/cull-phase semantics. |
| Validate equal outputs | Open. Run the same cull and guide checks before comparing cost. |
| Time and profile both widths | Open. Match GPU, driver, scene, resolution, frame count and flags. Record mean raster/resolve times and bandwidth evidence for each. |
| Resolve Q3 | Open. Record whether a change wins and how the L1 packed software-raster depth path stays viable. |

V7 records current 32-bit cost. The milestone record has no 64-bit
measurement.
[V7 benchmark](v7_cleanup_and_timings.md)

## Log

| Date | Change |
|---|---|
| 2026-11-08 | V8 deferred and 32-bit payload retained |
