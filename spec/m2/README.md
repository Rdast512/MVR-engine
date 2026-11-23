# M2 · Cluster RT bring-up

**Status** — C1–C4 built. M2 was reopened for mandatory PTLAS and closed
on 2026-11-14. The async overlap capture and RTX 2060 Super checks remain open.\
**Reviewed** — 2026-11-16 against the working tree at `9ae42eac2`.
[Engine spec](../../docs/engine_spec.md) · [PBR plan](../pbr_implementation_plan.md) · [RTX Mega Geometry feature](../features/planned/ray_tracing_mega_geometry.md) · [Considerations](../considerations.md)

| Task | Contract |
|---|---|
| [C1 · CLAS and cluster BLAS](c1_cluster_acceleration_structures.md) | Build and lifetime of model acceleration structures |
| [C2 · GPU instances and async compute](c2_gpu_instances_async_compute.md) | Instance metadata, frame slots and split graphics submissions |
| [C3 · Ray-hit reconstruction](c3_ray_hit_decode.md) | Shared raster and ray surface inputs and parity checks |
| [C4 · Partitioned TLAS](c4_partitioned_tlas.md) | One in-place PTLAS, GPU-generated instance writes and ordering across frames |

M2 consumes [M0 static geometry](../m0/a4_shading_attributes.md) and shares the
[M1 resolve](../m1/e1_resolve_guides.md) surface contract. C2 established
the full-build baseline and the async queue split. C4 is the current TLAS
build path. M3 uses it for direct-light visibility, M4 for shaded rays, and
M6 attaches OMMs to the same cluster geometry.

## Recorded acceptance

`ENGINE_RT_CHECK` confirmed raster and ray-query agreement on the RTX 5070
for the cull, motion and guide runs, including unloads. A still frame
writes zero PTLAS instances. Capture the async overlap in Tracy or Nsight
and verify cluster AS, position fetch and partitioned AS on the RTX 2060
Super under engine spec §16.2 and Q7.
