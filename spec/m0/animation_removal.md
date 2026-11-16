# M0 · Animation removal

**Status** — Done on 2026-11-08 in `f03f91f99`.\
**Implements** — Engine spec §7.3 and the PBR Phase A precondition for [A4](a4_shading_attributes.md).
[Engine spec](../../docs/engine_spec.md) · [PBR plan](../pbr_implementation_plan.md)

## Static geometry contract

- glTF meshes load in their base pose. The loader dropped skin and morph
  stream loading. The asset buffer catalog and CPU/Slang layouts dropped
  joints, weights, morph position/normal/tangent data, target and weight
  rows, and the corresponding model slices and primitive ranges.
- Raster uses the stored positions directly. The mesh shader's morph
  position function and the morph-specific culling bypass were removed.
  Base-pose degenerate triangles can be filtered at load.
- Entity and node transforms still place the mesh. Moving an entity updates
  its transform and motion history. The shared model's vertex data is static.
- A4 finishes the vertex streams before clustering. M2 builds its CLAS
  from those same clustered triangles.

[Loader](../../src/core/assets_loader.cpp) · [GPU layouts](../../src/core/types.hpp) · [Mesh helpers](../../shaders/geometry/mesh.slang) · [Cull passes](../../shaders/passes/meshlet_cull.slang)

## Verification

The subsequent A4 guide capture checked generated normal and tangent streams.
The M1 motion harness checked rigid entity motion and mirrored transforms.
[A4 results](a4_shading_attributes.md) · [M1 motion](../m1/motion_vectors.md)

## Log

| Date | Change |
|---|---|
| 2026-11-08 | Skin and morph data paths removed and static base-pose geometry adopted |
