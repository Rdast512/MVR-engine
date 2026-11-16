# M1 · V1 — Render targets and extent

**Status** — Built. `RenderTargets` owns the current images and descriptors.\
**Depends on** — [M0 A1](../m0/a1_gpu_contracts.md).\
**Used by** — [V2 depth](v2_reverse_z.md), [V5 raster](v5_visibility_output.md) and [V6 resolve](v6_material_resolve.md).
[Visibility-buffer design](../features/visibility_buffer.md)

## Decisions and implementation

| Decision | Implementation | Reason |
|---|---|---|
| Separate extents | Visibility, depth and guides use render resolution. The swapchain uses display resolution. M1 ran both at the same size. | M3 can render a smaller grid without changing the swapchain or motion units. |
| One visibility and depth image, two guide sets | Visibility is `R32_UINT` color attachment + sampled. Depth is sampled `D32_SFLOAT`. Guides have storage and sampled descriptors and ping-pong by frame number. | Raster data serves the current resolve. Temporal consumers retain prior surfaces. |
| Slot-local binding block | `prepareFrame` publishes current and previous guide indices and extents through `GpuTargets` and the frame root. | Descriptor block ownership follows the frame slot. Frame number selects guide parity. |
| Check image capabilities | Require relevant sampled, attachment and storage capabilities. Storage writes use typed formats. | Resolve writes formats including RG16_SNORM normals and packed emissive. |

A resize recreates extent-dependent images and descriptors, invalidates
Hi-Z and makes the camera's extent update a cut. The next frame binds
the new indices and suppresses old guide history. H1 and H2 extend
the same owner with reconstruction, post and exposure resources.
[Target recreation](../../src/render/vk_render_targets.cpp) · [Renderer](../../src/render/vk_renderer.cpp)

## Tasks and recorded results

| Task | State and evidence |
|---|---|
| Allocate primary images and publish heap bindings | Done. V5 wrote them and V6 sampled them for the guide runs. |
| Allocate two typed guide sets | Done. V6 captures and motion reprojection exercise them. |
| Recreate extent-dependent targets and reset history | Done. Scripted resize passed. AU10 later consolidated recreation and recorded one rebuild for the scripted resize. |

[Motion check](motion_vectors.md) · [C-M0-2](../considerations.md)
