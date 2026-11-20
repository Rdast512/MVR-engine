# M6 · D2: micromap build and CLAS attach

**Status:** built 2026-11-20; address rule confirmed; load-time micromap build cost open (C-D2-1), AS memory has no UI row or Tracy zone (C-D2-2) · **Milestone:** M6
**Implements:** engine spec §10.4, §10.5; plan D2 ([pbr_implementation_plan.md](../pbr_implementation_plan.md))
**Depends on:** [D1](./d1_omm_bake.md) (`OmmBake` per primitive), C1 (CLAS build), C4 (PTLAS instance rows, the 2-state ray flag)
**Considerations:** C-D-1 (closed with D4), C-C4-3 (the ray flag's capability)
**Reference:** `reference/vk_mini_samples/samples/mm_opacity/mm_process.cpp` (`buildMicromapEXT`, buffer usages, barrier); `reference/vk_gltf_renderer/src/gltf_scene_omm.*` (per-primitive `micromap`, index address, type, stride, base triangle consumed at BLAS build); `vk.xml` / `validusage.json` of SDK 1.4.363 for the CLAS fields

Upload D1's output, build one `VkMicromapEXT` per baked primitive, point every cluster of that primitive at it, and let alpha primitives' PTLAS instances drop `FORCE_OPAQUE`.

## Scope

| Object | Rule |
|---|---|
| Micromap | one `VkMicromapEXT` (`OPACITY_MICROMAP`) per baked primitive, `PREFER_FAST_TRACE`, built on the device in the C1 build command buffer before the CLAS build |
| Cluster | `opacityMicromapArray` = the primitive's micromap, `opacityMicromapIndexBuffer` = the cluster's subrange of the primitive's OMM index, `opacityMicromapIndexType` / `opacityMicromapIndexBufferStride` from D1's `indexFormat` |
| Non-alpha cluster | `opacityMicromapArray = 0` (OMMs disabled), as today |
| Build flag | `ALLOW_CLUSTER_OPACITY_MICROMAPS` on the CLAS input when any primitive has a micromap (VUID-…-opacityMicromapArray-10881), in the size query and in the build |
| Instance | an alpha primitive's PTLAS instance has no `FORCE_OPAQUE` and no `FORCE_NO_OPAQUE`; every other instance keeps `FORCE_OPAQUE` |

## Decisions

| Decision | Rule | Why |
|---|---|---|
| Micromap address | `opacityMicromapArray` = the device address of the micromap's storage: storage buffer address + `VkMicromapCreateInfoEXT::offset`. Verified with validation, GPU-AV and the shadow check before anything else in this step (§1, first task) | EXT has no address query for `VkMicromapEXT`; the field is "the address of a valid opacity micromap array" and NVAPI's D3D12 equivalent is the OMM array's GPU VA. If the driver rejects it, stop and record the result in considerations before trying another reading |
| Grouping | one micromap per primitive; storage, input data and triangle arrays suballocated from one buffer each per model (`VkMicromapCreateInfoEXT::offset`, `minAccelerationStructureScratchOffsetAlignment` and 256-byte storage alignment) | per-primitive arrays match D1's unit; one allocation per model unloads with the model's slice (resource architecture) |
| Build | `vkGetMicromapBuildSizesEXT` per primitive → one `vkCmdBuildMicromapsEXT` call for all of them with a shared scratch region per build, then a `MICROMAP_BUILD → ACCELERATION_STRUCTURE_BUILD` barrier (`MICROMAP_READ`) | same command buffer as C1, so no new queue or semaphore |
| Inputs | `triangleArray` = D1's descriptor rows (`VkMicromapTriangleEXT` stride 8), `data` = `arrayData`, `pUsageCounts` = D1's usages. Input buffers are freed after the build completes | the built micromap no longer reads them (sample `cleanBuildData`) |
| Index buffer | D1's per-primitive index, uploaded as is; cluster `c` of the primitive points at `base + firstTriangle(c) × stride` | D1's index has one entry per input triangle in cluster order |
| Instance flag | `GpuRayInstanceBuild::pad0` becomes `flags`, with an alpha bit set per instance row at load; `rt_instances.slang` writes `instanceFlags = isAlpha ? 0 : kPtlasInstanceForceOpaque` | PTLAS has only `FORCE_OPAQUE` / `FORCE_NO_OPAQUE`; the 2-state ray flag (C4) handles the rest. A per-geometry OPAQUE bit is not used (feature spec §8) |
| Memory | micromap storage, index and the CLAS growth are counted in the `GPU/RayTracing` Tracy zone and the UI's AS memory row | spec §10.4 owns the AS budget |
| Disable index | `CLUSTER_GEOMETRY_DISABLE_OPACITY_MICROMAP` (index −5) is not used | no cluster needs OMMs switched off at runtime |

## 1. Tasks

1. **Address check first.** Build one micromap for the first `AlphaBlendModeTest` alpha primitive, attach it to that primitive's clusters, and run with validation and GPU-AV. A wrong address is a device fault or a silent full-opaque or full-transparent result; the shadow check (§2) separates them. Record the result in §Log.
2. `vk_mega_geometry.{hpp,cpp}`: a `MicromapSet` (RAII: micromaps, storage, index buffer) per model; build before the CLAS commands; fill the four OMM fields of `ClusterAccelerationStructureBuildTriangleClusterInfoNV`, and the flag on `ClusterAccelerationStructureInputInfoNV::flags` for both the size query and the build.
3. `GpuRayInstanceBuild::flags` (`types.hpp` / `types.slang` mirror, size unchanged) and the `rt_instances.slang` flag choice. Update the comments in `rt_instances.slang` and `geometry/hit.slang` (line 76: "Every instance is FORCE_OPAQUE until plan D").
4. Name every object (`MicromapStorage_<model>`, `Micromap_<primitive>`, …) before binding.

## 2. Verification

- Validation and GPU-AV clean at load, unload and a scene switch to and from `AlphaBlendModeTest`.
- **Shadow check:** `AlphaBlendModeTest` under the engine sun: the shadow of each cut-out panel shows its pattern. With `ENGINE_GUIDE_CAPTURE`, a `Direct lighting` shot of the panels and its pixel samples.
- **Sponza:** plants cast leaf shadows; C-D-1's rectangular shadows are gone.
- `ENGINE_RT_CHECK` still passes: the primary rays now pass through cut-outs where the vis buffer (still opaque until D3) does not, so before D3 lands the check's mismatch count rises on alpha pixels only. The check is rerun after D3 (D4).
- CLAS build time and memory before and after, release build on the 5070.

## Done when

- every baked primitive's clusters carry its micromap, validation and GPU-AV are clean;
- `AlphaBlendModeTest` and Sponza shadows show cut-outs;
- the micromap address rule is confirmed and logged, and memory and build time are recorded.

## Must not

- build micromaps in a separate submit or on another queue than the CLAS;
- add an any-hit shader or a candidate loop;
- keep `FORCE_OPAQUE` on an alpha instance, or drop it from an opaque one.

## Log

| Date | Change |
|---|---|
| 2026-11-20 | Spec written; micromap address rule to verify first (§1.1) |
| 2026-11-20 | Built. `MegaGeometry::buildModel` takes D1's bakes (`ModelGeometry::ommBakes`, from `GeometryStore::ommBakes`), builds one micromap per baked primitive with `vkCmdBuildMicromapsEXT` at the head of the CLAS size submit, then a `MICROMAP_BUILD → ACCELERATION_STRUCTURE_BUILD` barrier; storage `MicromapStorage_<model>`, micromaps `Micromap_<primitive>`. `GpuRayInstanceBuild::flags` (`GpuRayInstanceBuildFlag::OpacityMicromap` / `kRayInstanceBuildFlagOpacityMicromap`) comes from `MegaGeometry::micromapFlags`, and `rt_instances.slang` drops `FORCE_OPAQUE` on it. The build log line now reports the build's wall time |
| 2026-11-20 | Deviations: (1) the instance bit means "the primitive's clusters carry a micromap", not "the material is alpha-tested": a non-opaque instance without micromaps would yield candidates no ray commits (no candidate loop), so the two must agree, and every alpha-tested primitive that D1 bakes gets one. (2) No `MicromapSet` type: storage, micromaps and flags are members of the model's `ModelStructures`, destroyed with it. (3) The OMM index and the micromap data and triangle arrays live in the model's CLAS inputs buffer (256-byte base alignment, `MICROMAP_BUILD_INPUT_READ_ONLY` usage) and are freed with it after the build; a built CLAS no longer reads its index. (4) A bake made only of special indices (the `AlphaBlendModeTest` Blend decal) still needs a micromap array address, so its micromap holds one unreferenced level-0 4-state triangle (1 data byte). (5) The scratch buffer is shared: max of the CLAS passes and the micromap builds, each build at its own `minAccelerationStructureScratchOffsetAlignment` offset (VUID-vkCmdBuildMicromapsEXT-pInfos-11651) |
| 2026-11-20 | Address rule (§1.1) confirmed: `opacityMicromapArray` = storage buffer address + `VkMicromapCreateInfoEXT::offset` (driver 621.01, RTX 5070). No device fault and no validation or GPU-AV message. Neither full-opaque nor full-transparent: `ENGINE_RT_CHECK` with micromaps detached (temporary switch) matched 32 of 32 frames on `AlphaBlendModeTest` and on Sponza; attached, rays pass the cut-outs (below), and on Sponza only plant leaves and the plants' shadow outlines change. Forcing `FORCE_OPAQUE` on every instance with micromaps attached gave the identical mismatch count: an OMM-transparent micro-triangle is culled whatever the opacity flags, which only resolve unknown states (the 2-state ray flag already does). NVIDIA's `mm_opacity` sample relies on the same rule (`RAY_FLAG_FORCE_OPAQUE` shadow rays through OMMs) |
| 2026-11-20 | Verification (release, RTX 5070, no validation unless stated). `ENGINE_RT_CHECK`: `AlphaBlendModeTest` 0 → ~45,300 id mismatches per frame, 0 misses; Sponza 0 → ~1,240 id mismatches, edge mismatches 104 → ~6,900, 0 misses; only micromapped primitives differ, as expected until D3 makes the vis buffer cut out too. Shadow check: `AlphaBlendModeTest`'s panel shadows fall on the backdrop right behind panels the raster still draws whole, so they are not visible before D3; Sponza (sun 20°/65°, a camera in the courtyard) changes 795 Shadowed-lights pixels and nothing else: 432 on the vase plant's leaves, which turn lit where neighbouring card cut-outs shadowed them, and the rest along the outline of the plants' shadows on the floor and on the hanging plant above. The chains (C-D1-2) were not isolated in a shot. Validation + sync + best practices (NVIDIA) + GPU-AV over a full `ENGINE_GUIDE_CAPTURE` (loads and unloads of micromapped models; normal checks 2 of 2, convergence passed) and the Debug build on Sponza: only the known third-party messages (NGX `SYNC-HAZARD-WRITE-AFTER-WRITE` on `vkCmdClearColorImage`, NGX fences in VUID-vkDestroyDevice-device-05137, `BestPractices-SpirvDeprecated_WorkgroupSize`), none naming a micromap |
| 2026-11-20 | Memory and build time, Sponza: CLAS 4.72 → 5.36 MiB with `ALLOW_CLUSTER_OPACITY_MICROMAPS` (host estimate 15.81 → 23.93 MiB), 14 micromaps 1.11 MiB, cluster BLAS 0.24 MiB unchanged, rt geometry 4.96 → 6.72 MiB. `buildModel` 5.4–6.5 ms → 60–128 ms; a temporary split put 52–118 ms of it in the micromap build (31 ms on a later load with warm clocks; 0.3 ms for a 1-micromap model), i.e. proportional to Sponza's 18.2 M micro-triangles at subdivision ≤ 5 (C-D2-1) |
