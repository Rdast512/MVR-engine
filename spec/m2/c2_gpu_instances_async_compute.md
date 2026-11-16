# M2 · C2 — GPU instances and async compute

**Status** — Built on 2026-11-13. C4 superseded the original full-build TLAS.\
**Implements** — PBR C2 and engine spec §10.3 and §13.1 X2.\
**Depends on** — [C1 model AS](c1_cluster_acceleration_structures.md).\
**Current AS build** — [C4 PTLAS](c4_partitioned_tlas.md).
[PBR plan](../pbr_implementation_plan.md) · [RTX Mega Geometry feature](../features/ray_tracing_mega_geometry.md)

## Instance and queue contract

- For each scene layout, the CPU prepares one `GpuRayInstance` row per
  active node instance and mesh primitive with a BLAS. It records the
  node-instance and primitive rows and supplies the parallel build data.
  The host knows the exact row count and buffers grow with layout changes.
- The GPU reads the frame slot's node-instance world matrix and writes
  the AS transform. Rigid object motion leaves the model BLAS available
  for reuse. The RT scene includes off-camera geometry for shadow rays.
- The compute queue signals `FrameSync::computeTimeline`. The graphics
  command buffer containing ray work waits for that value. Cull, early
  and late raster, Hi-Z, and resolve are submitted independently of the
  TLAS-ready wait. A slot's previous compute work completes before the
  CPU reuses resources the compute submission reads.
- The C2 baseline rebuilt a KHR TLAS per slot on every frame with
  GPU-written instance structs and a host-known build range. The current
  `rt_instances` shader emits C4's PTLAS ops. `GpuFrameRoot` still carries
  the TLAS address and the hit decode rows.

[RT scene](../../src/render/vk_rt_scene.cpp) · [Instance shader](../../shaders/passes/rt_instances.slang) · [Frame sync](../../src/core/vk_frame_sync.cpp) · [Renderer submits](../../src/render/vk_renderer.cpp)

## Verification

The RTX 5070 readback matched the CPU reference for 103, 167, 359 and
4,199 instances. In Release with validation off, Sponza's instance write
took 0.0008 ms and its full TLAS build took 0.101 ms. The 4,199-instance
full build took 0.150 ms. The Tracy or Nsight overlap capture and RTX 2060
Super measurements remain open.
[PBR plan, C2](../pbr_implementation_plan.md)

## Log

| Date | Change |
|---|---|
| 2026-11-13 | GPU instance writes and async full-build baseline recorded |
| 2026-11-14 | C4 replaced the full build with in-place PTLAS |
