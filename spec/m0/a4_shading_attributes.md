# M0 · A4 — Static geometry and shading vertex attributes

**Status** — Built and checked on RTX 5070.\
**Implements** — PBR A4 and engine spec §7.3.\
**Depends on** — Animation removal (`f03f91f99`, 2026-11-08).\
**Used by** — M1 resolve, M2 ray-hit decode and M6 opacity-micromap baking.\
[PBR plan](../pbr_implementation_plan.md) · [Engine spec](../../docs/engine_spec.md)

## Load path

1. Read glTF geometry in its base pose. M0 removed the skin and morph streams,
   morph application in the mesh shader, and the morph cull bypass.
2. Convert the primitive to triangles and filter redundant triangles. Decode
   vertex attributes and resolve the material before generating missing data.
3. For missing `NORMAL`, expand to corners and generate flat normals via
   `meshopt_generateNormals` in meshoptimizer v1.3 with zero crease angle and
   zero smoothing.
4. Generate tangents via `meshopt_generateTangents` with
   `meshopt_TangentCompatible` when the primitive has a normal texture and
   the selected UV stream and lacks `TANGENT`. Pass `1 - v` in a scratch
   copy of that UV stream. Stored UVs keep the file's values.
5. Re-weld all streams with `meshopt_generateVertexRemapMulti`. Use the remap
   as the new corner index list so hard edges and mirrored seams survive.
   Set the generated stream flags, pack vertices and then build meshlets.

Authored normals and tangents are preserved. Cache-hit instantiation reuses the
asset's completed geometry. Resolve guards remain for degenerate tangent frames.
[Asset loader](../../src/core/assets_loader.cpp) · [Geometry store](../../src/core/geometry_store.cpp) · [Material sampler](../../shaders/material/pbr.slang)

## Decisions and edge cases

| Input | Rule | Reason |
|---|---|---|
| Missing normals | Expand triangle corners and call `meshopt_generateNormals` with flat settings. | Shared indices can cross hard edges. Per-corner output can assign the correct normal to each face. |
| Missing tangent with a normal texture | Generate on the normal slot's UV set after resolving its `texCoord` override. Require the selected supported stream to exist. | The tangent frame must match the texture's UV axes. A primitive with a missing usable UV stays on the normal-map guard path. |
| File normals or tangents present | Copy the authored values through expansion and welding. | Generated and authored streams can coexist on a primitive. |
| UV orientation | Invert v in the tangent-generator input only. Rotation and scale of a material's UV transform are handled in B1's tangent-frame adjustment. | This matches the handedness of the shipped mirror fixture while retaining glTF's stored image coordinates. |
| Cache hit | Reuse the existing model geometry. | Generation is per model, upstream of raster and RT geometry, and independent of instance transforms. |

Expansion and welding include positions, normals, tangents, UV0,
UV1 and color. `meshopt_generateVertexRemapMulti` numbers unique corner
tuples and each stream is rewritten with the same remap. The resulting
corner index list replaces the source primitive's triangle indices.
Attribute flags control which streams upload. Cluster building and
any later CLAS or OMM work see this final vertex and triangle set.

## Tasks and acceptance

| Task | State | Evidence |
|---|---|---|
| Clean primitive topology before generating attributes | Done | Loader rejects missing positions and invalid indices and filters empty/degenerate triangle sets. |
| Generate and upload flat normals | Done | The no-`NORMAL` fixture distinguishes facets from smooth file normals. |
| Generate compatible tangents including mirrored seams | Done | The mirror fixture compares generated and authored normals and confirms handedness. |
| Preserve authored data and keep resolve guards | Done | Existing normals/tangents stay authored. Surface and B1 handle degenerate data. |

[M1 material sampling](../m1/b1_material_sampling.md) · [M3 texture transform](../m3/b1_texture_transform.md)

## Verification

`ENGINE_GUIDE_CAPTURE=1` is the integration entry point. The PBR plan records
the following RTX 5070 results on 2026-11-13.

| Sample | Recorded result |
|---|---|
| `NormalTangentMirrorTest` | Generated versus authored shading normals within 0.040° at p99.9, maximum 0.056°, identical coverage. Flipped v matched the authored tangent handedness on all 2,770 vertices. |
| `MetalRoughSpheres` without `NORMAL` | Shading versus geometric normals within 0.063° at p99.9. |
| `NormalTangentTest` | Generated shading normals versus offline tangents within 0.43° at p99.9, maximum 0.87°. |

[PBR plan, A4](../pbr_implementation_plan.md) · [Guide capture](../../src/core/engine_guide_capture.cpp)

## Log

| Date | Change |
|---|---|
| 2026-11-08 | Static base-pose contract landed |
| 2026-11-13 | Normal and tangent captures recorded |
