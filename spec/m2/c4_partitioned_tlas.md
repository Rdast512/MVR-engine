# M2 · C4 — Partitioned TLAS

**Status** — Required and built on 2026-11-14.\
**Implements** — PBR C4 and engine spec §10.3.\
**Depends on** — [C1 cluster BLAS](c1_cluster_acceleration_structures.md), [C2 async compute](c2_gpu_instances_async_compute.md) and [C3 ray checks](c3_ray_hit_decode.md).
[PBR plan](../pbr_implementation_plan.md) · [RTX Mega Geometry feature](../features/planned/ray_tracing_mega_geometry.md) · [Considerations C-C4-1–5](../considerations.md)

## Build path

- Require `VK_NV_partitioned_acceleration_structure` and query partition
  limits during device initialization. `RayTracingScene` owns one PTLAS,
  aligned scratch and a 48 B stored transform per instance. Build rows
  include the BLAS address and grid partition.
- Partition instance centers in XZ at layout time. The default targets
  about 64 instances per cell, with no global partition. An instance
  keeps its cell until the layout changes (C-C4-5).
- `rt_instances` emits `WRITE_INSTANCE` operations for all rows on a full
  build and for changed transforms on steady-state frames. It appends
  with one atomic per wave. Instance index is the decode row and the
  custom instance ID identifies the primitive. Mask is `0xFF`. The
  current shader forces opaque instances. M6 owns the alpha exception.
- The CPU zeroes the slot's op count after waiting for its previous
  compute work. The PTLAS build consumes the GPU-written count by
  address. Full build uses an empty source, and updates use the same
  source and destination address. The CPU reads the count after
  completion for telemetry.
- Frame N's in-place update waits for frame N−1's ray timeline signal.
  Ray work for frame N waits for frame N's compute completion. Storage
  growth waits for submitted readers and writers and forces a full build.

[Device](../../src/core/vk_device.cpp) · [PTLAS host](../../src/render/vk_rt_scene.cpp) · [GPU ops](../../shaders/passes/rt_instances.slang) · [Renderer](../../src/render/vk_renderer.cpp)

The two-state OMM flag travels with rays. With Slang 2026.20,
`requireOmm2StateRayFlag` declares the SPIR-V capability the compiler omits.
C-C4-3 specifies rechecking it on a Slang upgrade.
[Visibility trace](../../shaders/raytracing/trace.slang) · [Closest query](../../shaders/geometry/hit.slang) · [M4 G1](../m4/g1_ser_rt_pipeline.md)

## Update states and barriers

| State | GPU work | Build behavior |
|---|---|---|
| New layout, resized PTLAS or forced rebuild | Emit all rows and save transforms. | Source address 0. Use the input shape that sized the destination. |
| Valid layout with changed transforms | Compare to the saved 48 B transform. Append each changed row once with a wave reservation. | Update in place. Make shader writes to the operation data and count visible to AS build. |
| Still or empty scene | Unchanged rows emit no writes. The empty scene still gets a valid PTLAS. | A zero-op stream preserves the AS address contract. |

The CPU sizes for at most one write per row and resets the slot's
count after the compute wait. The previous ray timeline protects
the shared AS, and the current compute signal protects the next
ray submit. Growth waits for both timelines. Count readback is
only telemetry, ordered by a compute-to-host barrier and the slot
wait. The UI can force a full build or change grid density.

Fixed membership makes transform edits cheap to express. It can
widen bounds for a moving instance far from its original cell.
The 5070 comparison measured additional update cost from a global
partition variant and kept the uniform XZ grid.
[PTLAS recording](../../src/render/vk_rt_scene.cpp) · [Grid settings](../../docs/settings.md)

## Tasks and acceptance

| Task | State |
|---|---|
| Require PTLAS support and align storage/scratch | Built. RTX 2060 Super capability check open. |
| Emit GPU ops/count and protect in-place updates | Built. Still frames write zero instances. |
| Apply OMM two-state at ray level | Built. SPIR-V workaround remains C-C4-3. |
| Verify parity and compare against C2 | Recorded on the 5070, including moving rows and model unload. |

## Verification

The 2026-11-14 RTX 5070 checks matched 318/318 motion, 294/294 cull and
154/154 guide frames. Still frames wrote zero instances. These are
the recorded async-build times on driver 621.01 in Release without
layers. Fields add their instances to startup Sponza.

| Scene | C2 full build | C4 still | C4 moving |
|---|---:|---:|---:|
| Sponza, 103 rows | 0.101 ms | 0.015–0.018 ms | 0.12 ms, 103 writes |
| Field 8, 167 rows | 0.104 ms | 0.017 ms | — |
| Field 16, 359 rows | 0.119 ms | 0.017 ms | — |
| Field 64, 4,199 rows | 0.150 ms | 0.016–0.019 ms | 0.12 ms for 359 writes, 0.18 ms for 4,199 |

C-C4-4 tracks the cost of non-empty updates. Floor-tier capability and
overlap evidence are tracked by [M2 acceptance](./README.md#recorded-acceptance).
[PBR plan, C4](../pbr_implementation_plan.md) · [Considerations](../considerations.md)

## Log

| Date | Change |
|---|---|
| 2026-11-14 | Required PTLAS replaced the per-slot TLAS and passed the ray-query regressions |
