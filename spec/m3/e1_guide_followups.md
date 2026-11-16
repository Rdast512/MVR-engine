# M3 · E1 — Guide and jitter follow-ups

**Status** — Built and checked on 2026-11-14.\
**Implements** — PBR E1's M3 follow-ups and engine spec §12.2.\
**Depends on** — [M1 resolve](../m1/e1_resolve_guides.md) and [motion contract](../m1/motion_vectors.md).\
**Used by** — [F2](f2_rtxdi_bridge.md), [H1](h1_dlss_ray_reconstruction.md) and [M4 indirect](../m4/g2_indirect_pass.md).
[PBR plan](../pbr_implementation_plan.md) · [Engine spec](../../docs/engine_spec.md)

## Guide channels

- `DiffuseAlbedo.a` stores AO from B1, `lerp(1, occlusion.r, strength)`.
  DLSS uses RGB. Direct light leaves AO unapplied. The M4 indirect plan
  uses AO on the primary radiance-cache term.
- `SpecularF0.a` stores specular weight, 1 for regular materials and 0
  for unlit. Resolve scales F0 and specular albedo by the weight. The
  F2 surface uses the weight to exclude unlit from light evaluation,
  including specular Fresnel at grazing angles.
- F2 owns the position guide and the view-depth delta in motion z.

[Resolve](../../shaders/passes/resolve.slang) · [Material sampler](../../shaders/material/pbr.slang)

## Jitter and texture detail

`Camera` generates centered Halton(2,3) jitter with a phase count scaled
by display area over render area. Raster and barycentrics use the same
clip offset. Primary rays subtract the offset, while the guide motion
uses unjittered projections. Culling uses unjittered matrices and widens
the subpixel bound by jitter in pixels. RR receives the jitter separately.

By default jitter follows the Lit path that evaluates RR. The diagnostics
can force it with `ENGINE_JITTER=1`. At the smaller H1 render extent,
`lodBias = log2(renderWidth / displayWidth)` scales material gradients.
[Camera](../../src/scene/vk_camera.cpp) · [Culling](../../src/render/vk_meshlet_culling.cpp) · [Ray mapping](../../shaders/geometry/hit.slang)

## Verification

The AO and unlit guide captures passed on RTX 5070. With forced jitter,
the cull harness matched 16/16 poses at equal jitter phases and the
ray check matched all checked cull and motion frames. Use the default
jitter-off motion check for pixelwise pass/fail (C-T-3).
[PBR plan, E1](../pbr_implementation_plan.md) · [Considerations](../considerations.md)
