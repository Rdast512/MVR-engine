# M1 · Visibility buffer

**Status** — Done on 2026-11-13.\
**Reviewed** — 2026-11-16 against the working tree at `9ae42eac2`.

M1 establishes primary visibility and the shading guides. It consumes M0's
GPU layouts, target bindings and static vertex streams. The engine spec §16.2
owns milestone acceptance. The visibility-buffer feature spec holds the V1–V8
design and implementation history.
[Engine spec](../../docs/engine_spec.md) · [Visibility-buffer feature](../features/visibility_buffer.md) · [PBR plan](../pbr_implementation_plan.md) · [M0 contracts](../m0/README.md)

| Task | Contract |
|---|---|
| [A9 · Reverse Z and Hi-Z](a9_reverse_z_hiz.md) | V2–V3, conservative occlusion, previous and current pyramids |
| [V4–V5 · Visibility raster and present](v4_v5_visibility_raster.md) | A6, winding, 32-bit payload, DGC raster and display submit |
| [B1 · Material sampling](b1_material_sampling.md) | Core metallic-roughness channels, normal mapping and explicit gradients |
| [E1 · Resolve and guides](e1_resolve_guides.md) | V1 and V6, render targets, surface reconstruction and guide outputs |
| [Motion vectors and reprojection](motion_vectors.md) | E1 motion, history ownership and scripted verification |

## Visibility-buffer implementation steps

| Step | Technical spec | State |
|---|---|---|
| V1 | [Render targets and extent](v1_render_targets.md) | Built |
| V2 | [Reverse-Z depth convention](v2_reverse_z.md) | Built and checked |
| V3 | [Three-level Hi-Z build](v3_hiz_build.md) | Built and checked |
| V4 | [Raster bins and winding](v4_raster_bins.md) | Opaque built. M6 owns alpha coverage. |
| V5 | [Visibility output and debug presentation](v5_visibility_output.md) | Built |
| V6 | [Material resolve, guides and motion](v6_material_resolve.md) | Built and checked |
| V7 | [Cleanup and stage timing](v7_cleanup_and_timings.md) | RTX 5070 recorded. Floor-tier results tracked in M0. |
| V8 | [Payload comparison](v8_payload_comparison.md) | Deferred |

## Recorded acceptance

All results below are from 2026-11-13 on the RTX 5070.

| Check | Evidence |
|---|---|
| Culling | `ENGINE_CULL_CHECK=1` matched depth and albedo with all cull tests on and off across 16 Sponza poses. |
| Guides | `ENGINE_GUIDE_CAPTURE=1` confirmed the expected roughness, normals, albedo and emissive patterns on `MetalRoughSpheres`, `TwoSidedPlane` and `UnlitTest`. |
| Motion | `ENGINE_MOTION_TEST=1` verified still frames, camera and entity motion, mirrored transforms and history resets. |
| V7 baseline | At 1280×720 in Release with validation off, cull + DGC preprocess + Hi-Z took 0.086 ms, raster 0.031 ms and resolve 0.147 ms on startup Sponza. |

The RTX 2060 Super timings and culling image remain under M0 C-M0-1.
V8 stays deferred. Engine spec Q3 tracks a measured 32-bit versus 64-bit
payload comparison. C-M1-1 records the accepted NVIDIA D32 best-practices warning.
[Baseline and floor-tier checks](../m0/a10_culling_baseline.md) · [Engine spec §2.3 and §18](../../docs/engine_spec.md) · [Considerations](../considerations.md)

M2 reuses surface reconstruction for ray hits. M3 extends E1 with AO, jitter,
DLSS LOD bias, unlit specular suppression and the position guide. M6 owns
alpha-tested raster and ray agreement. M7 owns material classification and
dispatch per bin.
[PBR milestone map](../pbr_implementation_plan.md)
