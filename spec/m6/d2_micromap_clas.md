# M6 · D2: micromap build and CLAS attach

**Status:** planned 2026-11-20 · **Milestone:** M6
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
