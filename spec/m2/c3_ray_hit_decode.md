# M2 · C3 — Ray-hit reconstruction and verification

**Status** — Built. Ray-query parity recorded on RTX 5070.\
**Implements** — PBR C3 and engine spec §10.1, §10.3 and §10.4.\
**Depends on** — [C1 geometry IDs](c1_cluster_acceleration_structures.md), [C2 instance rows](c2_gpu_instances_async_compute.md) and [M1 resolve](../m1/e1_resolve_guides.md).\
**Used by** — [M4 G1](../m4/g1_ser_rt_pipeline.md).
[PBR plan](../pbr_implementation_plan.md) · [RTX Mega Geometry feature](../features/ray_tracing_mega_geometry.md)

## Hit contract

1. `primaryRay` traces from the near to far plane through the pixel center
   with the raster jitter subtracted from its screen coordinate.
2. `traceClosest` returns the committed hit's instance index, cluster and
   triangle IDs, barycentrics, distance, fetched positions and transforms.
   `decodeHit` uses `GpuRayInstance` to recover the node and primitive and
   the cluster ID to fetch the same raster corner attributes.
3. Build the shared `Surface`. Use fetched AS positions for the geometric
   normal and next-ray origin, hit facing for normal orientation and
   ray-cone gradients for B1 material sampling. `safeSpawnPoint` offsets
   along the geometric normal on the outgoing ray's side.

Ray meshlet, node-instance and shading-normal views run through
`debug_view` and share the raster ID palettes. Ray work waits for the
compute timeline through C2's split submission.
[Hit helpers](../../shaders/geometry/hit.slang) · [Surface reconstruction](../../shaders/geometry/surface.slang) · [Debug views](../../shaders/passes/debug_view.slang)

## Verification

`ENGINE_RT_CHECK=1` compares primary rays to visibility-buffer pixels
during cull, motion or guide capture, or 32 startup frames by itself.
Check interior IDs, shared decode normals at raster barycentrics,
pixel-space position, ray-cone mip and self-hits. The harness accounts
for raster snap near edges and coplanar depth ties. It reports normals
at the ray's own barycentrics separately to expose texture filtering
differences on steep normal maps.

On 2026-11-13 the RTX 5070 matched 214/214 cull, 318/318 motion and
70/70 guide frames, including unloads. Interior IDs and same-barycentric
normals matched. Position error was at most 0.056 px and mip difference
at most 0.22 levels. C4 records the later PTLAS rerun.
[Ray check shader](../../shaders/passes/rt_check.slang) · [Harness](../../src/core/engine_ray_check.cpp) · [PBR plan, C3](../pbr_implementation_plan.md)

## Log

| Date | Change |
|---|---|
| 2026-11-13 | Shared decode and ray-query parity checked |
| 2026-11-14 | Position gate moved to pixel units for jittered checks |
