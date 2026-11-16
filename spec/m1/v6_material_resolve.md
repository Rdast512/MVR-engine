# M1 · V6 — Material resolve, guides and motion

**Status** — Built and checked on 2026-11-13.\
**Depends on** — [V5 payload](v5_visibility_output.md), [B1 sampler](b1_material_sampling.md) and [A4 attributes](../m0/a4_shading_attributes.md).
[Guide formats](e1_resolve_guides.md) · [Motion](motion_vectors.md)

## Decision and pixel path

Use one 8×8-tiled compute dispatch until M7 introduces per-material
bins. Each thread reconstructs one visible sample. Two parity-selected
guide sets allow later temporal consumers to read previous surfaces.

1. Bound the dispatch against render extent and inspect depth first.
   Background writes zero surface terms and direction-only motion.
2. Unpack the payload to find the visible entry, node, model, primitive
   and three corners of its meshlet-local triangle.
3. Evaluate perspective barycentrics at the sample center, +X and +Y
   using homogeneous edge functions. The neighbor values stay on the
   same triangle's plane. Interpolate UV gradients and the facing-correct
   surface attributes.
4. Call `sampleMaterial` with explicit gradients. Write the typed
   current guide views. Project the same surface point under current
   and previous unjittered transforms for motion.

F2 later extends the interface with the position guide and motion z.
M2 uses the same corner attributes and surface reconstruction with
hit barycentrics and ray-cone gradients.
[Resolve](../../shaders/passes/resolve.slang) · [Barycentrics](../../shaders/geometry/visbuffer.slang) · [Surface](../../shaders/geometry/surface.slang)

## Tasks and recorded results

| Task | State and evidence |
|---|---|
| Decode and sample materials | Done. Metal/roughness, two-sided and unlit fixture captures. |
| Produce typed guides | Done. Unit normals, correct hemispheres and essentially zero still motion on Sponza. |
| Verify motion and resize | Done. Scripted reprojection regression. |
| Record resolve time | RTX 5070 done. 0.147 ms startup and 0.139 ms with the bunny field at 1280×720. |

[PBR E1 evidence](../pbr_implementation_plan.md) · [V7 timing](v7_cleanup_and_timings.md)
