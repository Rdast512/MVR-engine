# M2 · C1 — CLAS and cluster BLAS

**Status** — Built. C3 verified traceability after unloading another model.\
**Implements** — PBR C1 and engine spec §10.1–10.2.\
**Depends on** — [M0 A4](../m0/a4_shading_attributes.md).\
**Used by** — [C2 instance rows](c2_gpu_instances_async_compute.md) and [C3 hit decode](c3_ray_hit_decode.md).
[PBR plan](../pbr_implementation_plan.md) · [RTX Mega Geometry feature](../features/planned/ray_tracing_mega_geometry.md)

## Build and ownership

1. `AssetBuffers` invokes `MegaGeometry::buildModel` during the model upload.
   The build gathers cluster-local positions and 8-bit local triangles
   from the final raster meshlets. Each CLAS uses the model-relative
   meshlet row as `clusterID` and preserves raster triangle order.
2. Run the cluster AS size pass, allocate actual destinations and build
   in explicit-destination mode. After CLAS completes, size and build
   one cluster BLAS per mesh primitive from its CLAS addresses. The
   implementation uses device properties for AS limits and alignment.
3. Build with fast-trace and data-access flags. Position fetch returns
   the positions used by the AS. The host sets
   `minPositionTruncateBitCount = 0` and passes it to each cluster build.
4. All instances of the same model share its BLAS entries. A primitive
   without meshlets keeps address zero. Cache-hit instantiation reuses
   existing structures. Final model release frees its structures, and
   surviving models keep their own allocations.

Cluster acceleration structures and ray-tracing position fetch are
required device capabilities. Per-node world transforms enter at C2.
[Build implementation](../../src/core/vk_mega_geometry.cpp) · [Asset buffers](../../src/core/vk_asset_buffers.cpp) · [Device requirements](../../src/core/vk_device.cpp)

## Build decisions and memory contract

| Decision | Effect | Reason |
|---|---|---|
| Build from final raster meshlets | Cluster-local FP32 positions, byte triangle indices, model-relative cluster ID. | C3 can identify the same triangle and fetch its actual traced vertices. |
| Use exact preloaded sizing | Size CLAS, place aligned addresses, build CLAS, then size and build BLAS from those addresses. | Host estimates can overallocate. BLAS directly depend on CLAS addresses. |
| Query limits per device | Reject meshlets above supported cluster maxima. Use separate scratch, cluster and BLAS alignments. | The three allocation constraints are distinct Vulkan properties. |
| Request fast trace and data access | Position fetch sees the vertices used by the AS. Use zero position truncation at this stage. | Truncation requires measured error and cost evidence. |

Persistent model storage contains the CLAS allocation, cluster BLAS
allocation and per-primitive address table. Size, temporary input and
scratch buffers are load-time state. C2/C4 use the table for the
current layout. Object motion changes instance transforms while the
asset's geometry stays resident.
[Build implementation](../../src/core/vk_mega_geometry.cpp) · [C4](c4_partitioned_tlas.md)

## Tasks and acceptance

| Task | State |
|---|---|
| Enable cluster AS and position fetch | Built. Floor-tier verification remains Q7. |
| Validate inputs and build CLAS/BLAS | Built. Check one BLAS per primitive with meshlets and no allocated BLAS for empty primitives. |
| Preserve model ownership and triangle IDs | Verified via cache-hit reuse and C3 model swap/unload parity. |

## Verification

RTX 5070 evidence on 2026-11-13 includes 103 BLAS addresses for Sponza and
5 for MetalRoughSpheres. Sponza's 3,500 CLAS used 4.72 MiB against the
15.81 MiB implicit estimate. Cluster BLAS used 0.24 MiB. C3 checked 70 of
70 guide frames across eight erasures of the earlier model. RTX 2060
Super support remains open under Q7.
[PBR plan, C1](../pbr_implementation_plan.md) · [C3 verification](c3_ray_hit_decode.md)

## Log

| Date | Change |
|---|---|
| 2026-11-13 | Exact-size build and shared model ownership verified. Position fetch and unload checked. |
