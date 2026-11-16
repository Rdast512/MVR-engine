# M4 · G6: combine

**Status:** partly built: `passes/combine.slang` writes `noisyColor = directRadiance + emissive` (H1); the indirect term is missing · **Milestone:** M4
**Implements:** engine spec §11.6; plan G6 ([pbr_implementation_plan.md](../pbr_implementation_plan.md))
**Depends on:** [G5](./g5_restir_gi.md) (`indirectRadiance`)

## Scope (from the plan)

- `combine` CS: `noisyColor = directRadiance + indirectRadiance + emissive` (RGBA16F). The only place the three terms meet.
- The `Lighting only` debug view and the DLSS-RR color input switch to the full sum (plan H1 input table).

## Must not

Apply AO to direct, add a second path loop, or touch SDK buffers outside their bridge files and host modules.

## Log

| Date | Change |
|---|---|
| 2026-11-16 | created from plan G6 |
