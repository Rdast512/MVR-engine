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

`Camera` generates centered Halton(2,3) jitter. The phase count is
`max(1, round(8 × displayArea / renderArea))`, and the cycle starts at
index 1. Raster and barycentrics use the same
clip offset. Primary rays subtract the offset, while the guide motion
uses unjittered projections. Culling uses unjittered matrices and widens
the subpixel bound by jitter in pixels. RR receives the jitter separately.

By default jitter follows the Lit path that evaluates RR. The diagnostics
can force it with `ENGINE_JITTER=1`. At the smaller H1 render extent,
`lodBias = log2(renderWidth / displayWidth)` scales material gradients.
[Camera](../../src/scene/vk_camera.cpp) · [Culling](../../src/render/vk_meshlet_culling.cpp) · [Ray mapping](../../shaders/geometry/hit.slang)

## Coordinate and channel decisions

| Area | Implementation invariant | Failure mode it avoids |
|---|---|---|
| AO | Sample R with strength and store in diffuse guide alpha. Consumers that use just albedo read RGB. | Ambient occlusion would incorrectly darken punctual direct radiance if applied in the DI contribution. |
| Unlit | Zero specular weight, F0 and specular albedo. Gate lighting through the weight. | Zero F0 alone leaves the grazing term of Schlick Fresnel. |
| Jitter | Store an NDC offset separate from camera matrices. Raster adds `jitter × clip.w`. Resolve uses those corners. Ray origins undo the screen offset and motion reprojects with unjittered matrices. | Visibility and guides otherwise refer to different surface points or history gets a spurious motion vector. |
| Culling | Widen conservative subpixel bounds by the maximum jitter displacement in render pixels. The exact triangle test already reads jittered corners. | A subpixel test could discard coverage introduced by jitter. |
| Texture detail | Multiply explicit gradients by `exp2(lodBias)`, where bias depends on render/display widths. | RR receives appropriately sampled texture detail for the reduced grid. |

## Tasks and recorded results

| Task | State and evidence |
|---|---|
| Publish AO and unlit specular guide changes | Done. AO checkmarks in both UV sets and sample column. Unlit F0 captures are black. |
| Add jitter while preserving coverage and ray agreement | Done on RTX 5070. 16/16 culled poses matched at equal phases. Removing ray-side jitter subtraction failed all 32 frames of a separate ray check. |
| Set H1 LOD bias and drive jitter from RR eligibility | Done. At Quality the recorded bias is about −0.585. Debug views bypass automatic jitter. |
| Preserve motion regression | Done with default jitter off. Forced-jitter pixelwise albedo comparisons have their own caveat in C-T-3. |

## Verification

The AO and unlit guide captures passed on RTX 5070. With forced jitter,
the cull harness matched 16/16 poses at equal jitter phases and the
ray check matched all checked cull and motion frames. Use the default
jitter-off motion check for pixelwise pass/fail (C-T-3).
[PBR plan, E1](../pbr_implementation_plan.md) · [Considerations](../considerations.md)
