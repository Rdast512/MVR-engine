# M1 · E1 — Visibility resolve and guides

**Status** — M1 resolve and guide verification complete on 2026-11-13.\
**Implements** — Visibility-buffer V1 and V6, PBR E1 and engine spec §9.\
**Depends on** — [Visibility raster](v4_v5_visibility_raster.md), [B1 sampling](b1_material_sampling.md) and [M0 target bindings](../m0/a1_gpu_contracts.md).
[Visibility-buffer feature](../features/visibility_buffer.md) · [PBR plan](../pbr_implementation_plan.md)

## Resolve

1. Dispatch one 8×8-tiled compute shader over render pixels. Check depth
   first. Empty pixels write zero surface guides and sky motion.
2. Decode the payload through `loadDrawContext` and fetch the meshlet-local
   triangle's three corners. `geometry.surface` provides the shared surface
   path that M2 also uses for ray hits.
3. Evaluate perspective-correct Olano–Greer barycentrics at the pixel center
   and its +X and +Y neighbors on the triangle's plane. Homogeneous edge
   functions handle triangles crossing the eye plane. The neighbor differences
   provide analytic UV gradients to `sampleMaterial`.
4. Write the guide set selected by frame parity. `RenderTargets` owns the
   images, checks format support and publishes the current and previous
   descriptor indices in per-frame-slot `GpuTargets`. Typed storage views
   write the images and sampled views serve consumers.

Raster and barycentrics use the same jittered corner positions when M3
jitter is active. The motion calculation uses separate unjittered projections.
M7 owns classification and resolve dispatch per material bin.
[Resolve](../../shaders/passes/resolve.slang) · [Barycentrics](../../shaders/geometry/visbuffer.slang) · [Surface](../../shaders/geometry/surface.slang) · [Render targets](../../src/render/vk_render_targets.cpp)

## Guide contract

Formats and channels below describe the current interface. M3 additions are
identified where they extend the M1 guides.

| Guide | Format | Channels |
|---|---|---|
| `NormalRoughness` | RGBA16F | World shading normal and perceptual roughness |
| `GeoNormal` | RG16_SNORM | Octahedral, facing-corrected world geometric normal |
| `DiffuseAlbedo` | RGBA8 UNORM | Diffuse albedo. M3 E1 adds AO in alpha. |
| `SpecularF0` | RGBA8 UNORM | F0. M3 E1 scales RGB by specular weight and stores the weight in alpha. |
| `SpecAlbedo` | RGBA8 UNORM | Environment BRDF approximation. M3 E1 suppresses it for unlit materials. |
| `Emissive` | B10G11R11 UFLOAT | Emission, including base color on unlit materials |
| `Motion` | RGBA16F | Unjittered current-to-previous motion in render pixels. M3 F2 adds previous-minus-current view depth in z. |
| `Position` | RGBA32F | Added in M3 F2. World position and view depth in w, zero on empty pixels. |

The depth and visibility targets belong to the current frame. The two guide
sets retain the previous surface data for temporal consumers.
[Motion contract](motion_vectors.md) · [Engine spec §9.3](../../docs/engine_spec.md)

## Verification

`ENGINE_GUIDE_CAPTURE=1` records PNGs under `logs/guides`. M1 evidence
includes metal versus dielectric F0 and roughness on `MetalRoughSpheres`,
opposite-facing normals with matching albedo on `TwoSidedPlane`, and
emission with zero diffuse albedo on `UnlitTest`. The startup Sponza resolve
measured 0.147 ms on the RTX 5070 at 1280×720.
[PBR plan, E1](../pbr_implementation_plan.md) · [Guide capture](../../src/core/engine_guide_capture.cpp) · [Engine spec §2.3](../../docs/engine_spec.md)

## Log

| Date | Change |
|---|---|
| 2026-11-13 | M1 guide capture and resolve timing recorded |
| 2026-11-14 | M3 extended the guides and added jitter and DLSS LOD bias |
