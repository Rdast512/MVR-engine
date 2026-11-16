# M3 · First lit frame

**Status** — Done on 2026-11-14, on `Sponza` lit by E3's sun. The
RTXDI 3.2 update landed on 2026-11-15.\
**Reviewed** — 2026-11-16 against the working tree at `9ae42eac2`.
[Engine spec](../../docs/engine_spec.md) · [PBR plan](../pbr_implementation_plan.md) · [Considerations](../considerations.md)

| Task | Contract |
|---|---|
| [B1 · Texture-transform verification](b1_texture_transform.md) | M3 check of the sampler introduced in M1 |
| [B2 · BRDF](b2_brdf.md) | glTF GGX evaluation, sampling and PDF |
| [B3 · Punctual-light evaluation](b3_punctual_lights.md) | Directional, point and spot light math |
| [E1 · Guide and jitter follow-ups](e1_guide_followups.md) | AO, unlit specular weight, jitter and LOD bias |
| [E2 · GPU world-light buffer](e2_world_light_buffer.md) | Per-slot world-space punctual light rows |
| [E3 · Engine sun](e3_engine_sun.md) | Engine-owned directional light for Sponza |
| [F1 · RTXDI host context](f1_rtxdi_context.md) | Reservoirs, light regions, frame indices and constants |
| [F2 · RTXDI bridge](f2_rtxdi_bridge.md) | Guide and light history, shading and visibility callbacks |
| [F3 · ReSTIR DI](f3_restir_di.md) | Direct-light passes and convergence evidence |
| [H1 · DLSS Ray Reconstruction](h1_dlss_ray_reconstruction.md) | NGX integration, inputs, output and history |
| [H2 · Post and present](h2_post_present.md) | Histogram exposure, ACES and sRGB presentation |

## Frame and acceptance

The [M1 resolve](../m1/e1_resolve_guides.md) writes guides while async compute
prepares the world lights and [M2 PTLAS](../m2/c4_partitioned_tlas.md). The
ray command buffer waits for compute, runs ReSTIR DI, and combines direct
radiance and emission. The Lit path evaluates DLSS-RR, meters exposure
and tonemaps before present.

The lamp convergence check measures the raw direct-light pipeline against
an exhaustive punctual reference. The sunlit courtyard records Lit
stability through DLSS-RR. RTX 2060 Super measurements remain tracked with
M0 C-M0-1 and C-F3-7. M4 adds indirect light. M7 adds emissive-triangle
lights and their presampling.
[Baseline owner](../m0/a10_culling_baseline.md) · [Settings and stability](../../docs/settings.md) · [M4 combine](../m4/g6_combine.md)
