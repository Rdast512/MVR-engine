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

## Data-path changes

| Boundary | Removed data and work | Static path |
|---|---|---|
| glTF ingestion | Per-primitive skin and morph stream loading | Read base attributes, triangulate, discard redundant base-pose triangles, then run A4. |
| Geometry and upload | `Joints`, `Weights`, `MorphPos`, `MorphNrm`, `MorphTan`, `MorphTargets` and `MorphWeights` | Upload final static streams and geometry slices. |
| CPU/Slang ABI | `GpuMorphTarget`, primitive morph ranges, model slices and draw-record skip-culling state | Maintain mirrored static model, primitive and node-instance layouts. |
| Raster and culling | `applyMorphPosition` and the morph culling bypass | Cull static cluster bounds under the instance transform, then raster the same vertex positions. |

Rigid placement remains available. The frame path stores current and
previous transforms, so an entity can move while retaining its shared
mesh, bounds and model acceleration structures. Base-pose degenerates
can be discarded because a later morph cannot open them up.
[Frame data](../../src/core/vk_frame_data.cpp) · [Resource architecture](../resource_architecture.md)

## Tasks and acceptance

| Task | State | Evidence |
|---|---|---|
| Remove skin/morph loader and upload paths | Done | Removal commit `f03f91f99` and the current static loader path. |
| Remove morph-specific GPU data and shader logic | Done | Host and shader mirror layouts and mesh/cull code. |
| Leave static geometry usable by A4 and M2 | Done | Generated attribute captures and the later clustered ray-query parity run. |

The recorded checks exercise static loading and rigid motion. The branch
history documents the removal. No dedicated animation-removal GPU result
was recorded.
[M2 C3](../m2/c3_ray_hit_decode.md)

## Verification

The subsequent A4 guide capture checked generated normal and tangent streams.
The M1 motion harness checked rigid entity motion and mirrored transforms.
[A4 results](a4_shading_attributes.md) · [M1 motion](../m1/motion_vectors.md)

## Log

| Date | Change |
|---|---|
| 2026-11-08 | Skin and morph data paths removed and static base-pose geometry adopted |
