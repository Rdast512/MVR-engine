# M1 · V4 — Raster bins and winding

**Status** — Opaque raster built. M6 owns alpha-tested coverage.
[Visibility-buffer V4](../features/visibility_buffer.md) · [A6 and raster integration](v4_v5_visibility_raster.md)

## Decision and shader path

Bins identify raster shader behavior. The M1 template builder places
every draw in `Opaque` and marks double-sided draws with
`DrawRecordFlag::TwoSided`. One backface-culling raster state serves
both sidedness cases. Cone culling skips two-sided records because
either side can contribute.

1. The mesh shader gets one visible meshlet from its DGC range. The
   corner loader accounts for the mirrored-instance winding flag.
2. For a two-sided triangle that is still back-facing in projection,
   swap the second and third raster indices. Keep the triangle's
   original index in the payload. Emit the triangle-test result via
   `SV_CullPrimitive`.
3. Resolve reloads the original corners and determines real facing
   after the instance winding correction. It uses facing for geometric
   and shading normals, along with the matching tangent frame.
4. Each enabled phase/bin is preprocessed and executed with the matching
   state. M1 has one populated bin and treats masked materials as opaque.

[Cull templates](../../src/render/vk_meshlet_culling.cpp) · [Mesh helpers](../../shaders/geometry/mesh.slang) · [Raster](../../shaders/passes/mesh_raster.slang) · [Surface](../../shaders/geometry/surface.slang)

## Tasks and recorded results

| Task | State and evidence |
|---|---|
| Replace sidedness groups with bin and flag | Done. Host cull templates and cone guard. |
| Preserve two-sided normals and mirrored winding | Done. `TwoSidedPlane` records matching albedo and opposite-facing normals. Mirrored transforms are exercised by motion. |
| Activate masked coverage | M6. M1's Sponza cull check covers the opaque path. |

[Guide results](e1_resolve_guides.md) · [Motion results](motion_vectors.md)
