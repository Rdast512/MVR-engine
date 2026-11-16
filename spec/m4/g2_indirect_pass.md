# M4 · G2: indirect pass

**Status:** not started; detailed when G1 is done · **Milestone:** M4
**Implements:** engine spec §11.3; plan G2 ([pbr_implementation_plan.md](../pbr_implementation_plan.md))
**Depends on:** [G1](./g1_ser_rt_pipeline.md) (SER pipeline), E1 (guides, AO), F2 (RTXDI light sampling), C3 (`decodeHit`)
**Used by:** [G3](./g3_nrc.md) (query pass), [G5](./g5_restir_gi.md) (initial samples)

## Scope (from the plan)

- One ray per pixel from the primary surface (guides + depth), direction from `sampleBrdf` (VNDF), origin offset along `GeoNormal` plus the CLAS truncation bound (spec §10.2). Floor tier: half resolution, checkerboard.
- At X₂: `decodeHit` → `sampleMaterial` → direct light via `RTXDI_SampleLocalLights` / `RTXDI_SampleInfiniteLights` without reuse + one `traceVisibility` → add emissive → query the cache for bounces 2…∞, weighted by the primary `ao` (plan §2 #10).
- Writes the ReSTIR GI initial sample (hit position, normal, radiance). With NRC the cache radiance is not known yet: the pass writes the query record and G3's resolve completes the sample.
- First real SER measurement: reorder on/off timings on both cards (G1 §5).

## Open before start

- Sample buffer layout shared with G3's resolve and RTXDI's ReSTIR GI reservoir input.
- Ray cone at X₂ for texture LOD (spec §11.1).
- Half-resolution checkerboard mapping on the floor tier and how G5 consumes it.

## Log

| Date | Change |
|---|---|
| 2026-11-16 | created from plan G2 |
