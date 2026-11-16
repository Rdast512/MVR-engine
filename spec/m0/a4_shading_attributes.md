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
