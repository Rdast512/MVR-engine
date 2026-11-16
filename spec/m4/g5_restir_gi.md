# M4 · G5: ReSTIR GI

**Status:** not started · **Milestone:** M4
**Implements:** engine spec §11.5; plan G5 ([pbr_implementation_plan.md](../pbr_implementation_plan.md))
**Depends on:** [G2](./g2_indirect_pass.md) (initial samples), [G3](./g3_nrc.md) (resolved sample radiance), F1 (RTXDI context)
**Used by:** [G6](./g6_combine.md)
**Related:** [rtxdi_3_2_update.md](../features/rtxdi_3_2_update.md) (pinned RTXDI version)

## Scope (from the plan)

- RTXDI ReSTIR GI on the G2 samples: temporal then spatial reuse, boiling filter on, reconnection visibility through `traceVisibility`. Output `indirectRadiance` (RGBA16F, render res).
- Defaults from the RTXDI sample, as for ReSTIR DI (plan F3); the check pins its own settings.

## Done when

`Sponza` with a sun shows bounce light under arches, stable after DLSS-RR, and converges to the ground truth of spec §14: the same path code accumulated over many frames with the cache off and a high bounce count, on a static frame.

## Log

| Date | Change |
|---|---|
| 2026-11-16 | created from plan G5 |
