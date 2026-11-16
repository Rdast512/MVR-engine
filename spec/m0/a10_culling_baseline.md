# M0 · A10 — Culling capacity and baseline verification

**Status** — Capacity checks built. RTX 5070 baseline recorded. RTX 2060 Super open.\
**Implements** — Engine spec §2.3, §8.1 A10 and §16.2.\
**Consideration** — C-M0-1.\
[Engine spec](../../docs/engine_spec.md) · [Considerations](../considerations.md)

## Capacity contract

`MeshletCulling` sizes the draw-record and visible regions from scene templates
and provides capacities to `GpuCullView`. The backing buffers can grow with
headroom. Draw-record and visible regions are split by early and late phase,
and the visible region is further split by raster bin.

The shader bounds candidate, draw-record and visible-list writes. Dropped
entries increment the corresponding `GpuCullCounters` fields. Consumers guard
their reads, and DGC generation clamps each visible count to its bin capacity
before computing the sequence count. The culling panel shows dropped entries
and Tracy plots their aggregate. Overflow with current scene-derived sizing
indicates a sizing defect. Revisit sizing when the post-1.0 LOD cut makes
list counts view-dependent.
[Host culling](../../src/render/vk_meshlet_culling.cpp) · [Cull passes](../../shaders/passes/meshlet_cull.slang) · [Culling panel](../../src/ui/engine_ui.cpp)

## Producer and consumer limits

| List | Reservation and bound | Reader rule |
|---|---|---|
| Late instance candidates | Early occlusion reserves an index using the late instance dispatch count. Store only below `instanceCandidateCapacity`. | The indirect dispatch may include overflow. The shader rejects the group before reading the list. |
| Draw records | Visible instances reserve a contiguous range in the phase's indirect meshlet dispatch count. Count each element beyond `drawRecordCapacity`. | Copy only the fitting prefix. Cull and reject-mask readers guard their group index. |
| Visible meshlets | Wave append into each phase/bin range using a bin capacity from the scene's templates. | Clamp the count before emitting DGC sequences. Each sequence stays within the stored visible region. |

One early reject mask is written per valid record, including zero masks.
The mask has at most one bit per meshlet in its group. The host checks
compute dispatch limits when creating the templates and the total size
of the visible region against the 25-bit payload row space.
[Host template sizing](../../src/render/vk_meshlet_culling.cpp) · [GPU counters and lists](../../shaders/passes/meshlet_cull.slang)

## Tasks and acceptance

| Task | State | Evidence |
|---|---|---|
| Carry bounds to every producer and guard consumers | Done | Candidate, draw-record and visible bounds in the GPU cull path. |
| Report dropped work | Done | UI shows separate list losses and Tracy plots aggregate overflow. |
| Compare culling against disabled tests | RTX 5070 done | 16 Sponza poses. Floor-tier capture remains C-M0-1. |
| Record per-stage Release timing | RTX 5070 done | Both benchmark scenes below. Floor-tier timing remains C-M0-1. |

## Image check

Run `ENGINE_CULL_CHECK=1` on Sponza. The harness compares diffuse albedo and
depth with every cull test enabled versus disabled across 16 poses, including
checks after a turn. The reference is reverse Z. Pass requires zero differing
pixels in both views at every pose. Record the summary and inspect the list
overflow counters.

The RTX 5070 result was 16 of 16 on 2026-11-13. Engine spec §8.1 separately
tracks the frozen fly-out, cone-cull loss and the M6 foliage check.
[Cull harness](../../src/core/engine_cull_check.cpp) · [Engine spec](../../docs/engine_spec.md)

## Timing baseline

Use `ENGINE_BENCH_FRAMES=3000` in Release with vkconfig overrides and validation
layers disabled. Capture the startup Sponza view and a second run with
`ENGINE_BENCH_FIELD=128`. Record the GPU, driver, commit, render extent and
`[Bench]` stage means. Record the enabled passes and render settings when
using a newer build.

The recorded RTX 5070 run used driver 621.01 and rendered at 1280×720.
All values below are mean milliseconds.

| Stage | Sponza | Sponza + 128×128 bunny field |
|---|---:|---:|
| Cull, DGC preprocess and Hi-Z, both phases | 0.086 | 0.182 |
| Visibility raster, both phases | 0.031 | 0.032 |
| Resolve and guides | 0.147 | 0.139 |
| GPU total including debug view, present and ImGui | 0.308 | 0.395 |

[Benchmark harness](../../src/core/engine_bench.cpp) · [Engine spec, §2.3](../../docs/engine_spec.md)

## Done when

Record the RTX 2060 Super per-stage timings and passing culling comparison
in the milestone evidence, then resolve C-M0-1.
[Milestone acceptance](../../docs/engine_spec.md) · [C-M0-1](../considerations.md)

## Log

| Date | Change |
|---|---|
| 2026-11-13 | RTX 5070 baseline and 16-pose check recorded |
| 2026-11-16 | Documented capacity guards and the remaining floor-tier acceptance checks |
