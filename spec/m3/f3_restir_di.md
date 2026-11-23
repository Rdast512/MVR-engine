# M3 · F3 — ReSTIR DI passes and convergence

**Status** — M3 closed on 2026-11-14 with RTXDI 3.1. Current path is the
RTXDI 3.2 split from 2026-11-15.\
**Implements** — PBR F3 and engine spec §11.2.\
**Depends on** — [F1 host context](f1_rtxdi_context.md) and [F2 bridge](f2_rtxdi_bridge.md).\
**Used by** — [H1 reconstruction](h1_dlss_ray_reconstruction.md) and [M4 combine](../m4/g6_combine.md).
[PBR plan](../pbr_implementation_plan.md) · [RTXDI 3.2 update](../features/done/rtxdi_3_2_update.md)

## Current passes

| Pass | Contract |
|---|---|
| `rtxdi_initial` | Generates initial candidates from the local and infinite light regions and configured BRDF sampling, then stores the initial reservoir. |
| `rtxdi_temporal` | Uses motion and previous guides and reservoirs for temporal reuse, then applies the boiling filter. Its output aliases the initial slot per pixel. A reset leaves that pixel's initial reservoir in place. |
| `rtxdi_shade` | Reads neighbors from the spatial input slot and writes a distinct output when spatial reuse runs. Evaluates final visibility according to the configured reuse policy and shades with the reservoir weight. |

The renderer orders reservoir reads and writes between passes and frames.
On a no-light frame it skips initial and temporal and runs shade to zero
`directRadiance`. The output is linear HDR RGBA16F at render resolution.
Combine adds emission. `Lighting only` and `RTXDI reservoirs` expose the
result and the selected light and history.
[Initial](../../shaders/passes/rtxdi_initial.slang) · [Temporal](../../shaders/passes/rtxdi_temporal.slang) · [Spatial and shade](../../shaders/passes/rtxdi_shade.slang) · [Renderer](../../src/render/vk_renderer.cpp)

## Reservoir and visibility decisions

1. Start with the SDK local, infinite and BRDF candidate counts.
   Combine candidates into the initial slot. The local region uses
   uniform sampling. Analytic directional samples use the infinite
   region. M7 owns emissive-triangle candidates and presampling.
2. Temporal reuse starts from the pixel's initial reservoir. Use
   current motion and previous surfaces/lights to find and score
   the historical reservoir. An invalid history returns uniformly,
   leaving the initial buffer usable as temporal output. When enabled,
   the boiling filter runs with group synchronization.
3. Spatial reuse reads neighbor reservoirs from its input buffer
   and writes its selected reservoir to a distinct destination.
   Evaluate the selected light for the receiving pixel, apply final
   visibility according to settings, then shade with
   `RTXDI_GetDIReservoirInvPdf`. Store the reservoir for next frame.
4. Order previous-frame reservoir use before new writes. Between
   stages, barriers expose the initial and temporal outputs to the
   next stage. Spatial can still use this frame's neighbors after a
   cut. A zero-light scene uses only the uniform zero-output path.

| Configuration | Role |
|---|---|
| RTXDI 3.1 `FusedSpatiotemporal` | Original M3 implementation. It established reference convergence and its own stage baseline. |
| RTXDI 3.2 `TemporalAndSpatial` | Current separate temporal and spatial algorithm, with spatial and final shade in one dispatch. |
| `None`, 32 local and 8 infinite candidates | High-candidate comparison without temporal or spatial reuse. |
| `convergedSettings` | Regression settings tuned for the static lamp. Comparison is against exhaustive `Shadowed lights`. |

C-F3-1's bounds checks remain required for BDA. C-R32-4 records that
the default final-visibility cache can reuse a value from a different
pixel near hard shadow boundaries. The default is distinct from the
regression configuration, so report the mode and toggles with a capture.
[Bridge safeguards](../../shaders/lighting/rtxdi_bridge.slang) · [RTXDI 3.2 pass and settings history](../features/done/rtxdi_3_2_update.md)

## Tasks and evidence

| Task | State and result |
|---|---|
| Produce reservoirs and direct-light output | Done in 3.1, split in 3.2. No-light output explicitly zeroed. |
| Validate convergence and keep a permanent check | Done. Historical fused mean bias −0.001% and tile p90 1.72% at 1280×720. The 3.2 comparison below uses its own render size/settings. |
| Record Release stage cost | RTX 5070 done. At 853×480 a 3.2 Release run measured approximately 0.15 ms initial, 0.07 ms temporal and 0.04 ms spatial/shade. The initial scope includes the TLAS wait. |
| Check floor tier and retune defaults | Open under C-F3-7, C-R32-1 and C-M4-2. The benchmark and raw convergence shot answer different questions from Lit output stability. |

## Verification and defaults

`ENGINE_GUIDE_CAPTURE=1` holds `convergedSettings` during the static
`LightsPunctualLamp` convergence shot. After 16 warm-up frames it compares
64 `Lighting only` frames with `Shadowed lights`. Pass requires absolute
bias ≤ 0.5% and tile p90 ≤ 3%. The 3.2 update recorded −0.134% bias and
2.87% p90 in the documented comparison. `ENGINE_RTXDI_REFERENCE=1`
selects the high-candidate, no-reuse reference mode for comparison.

Runtime settings follow NVIDIA's Medium preset with uniform local
sampling. They can fail the raw convergence gate (C-R32-1). The check
uses Basic temporal correction, Pairwise spatial correction with a
disocclusion boost, and disables permutation, BRDF candidates and
final-visibility reuse.

| Recorded 3.2 configuration | Lamp bias | Lamp tile p90 | DI mean on sunlit Sponza |
|---|---:|---:|---:|
| NVIDIA Medium-based settings | −1.336% | 26.36% | 0.347–0.349 ms |
| Converged check settings | −0.134% | 2.87% | 0.240 ms |

The cost columns come from two alternating Release rounds without
layers at 853×480, 6,000 frames per run. C-R32-2–5 record remaining temporal,
candidate and visibility tradeoffs. C-F3-7 owns the RTX 2060 Super
measurements, and C-F3-9 records that the initial timer includes the
TLAS wait.
[Guide harness](../../src/core/engine_guide_capture.cpp) · [Settings source](../../src/render/vk_rtxdi.cpp) · [Settings and benchmarks](../../docs/settings.md) · [Considerations](../considerations.md)
