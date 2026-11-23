# M4 · G1: SER ray-tracing pipeline

**Started:** 2026-11-16 · **Status:** built 2026-11-16; check passes on the RTX 5070; **shelved 2026-11-17** (considerations C-G1-3: Slang's EXT `HitObject` support; files kept, not built) · **Milestone:** M4 ([engine_spec.md](../../docs/engine_spec.md) §16.2)
**Implements:** engine spec §11.1 (ray model), §2.2 SER row, Q8; plan G1 ([pbr_implementation_plan.md](../pbr_implementation_plan.md))
**Depends on:** M2 (cluster RT scene, `GpuRayInstance`, `decodeHit`), M3 (`sampleMaterial`, BRDF, RTXDI sampling)
**Used by:** [G2 indirect pass](./g2_indirect_pass.md), [G3 NRC](./g3_nrc.md) update paths
**Related:** [Slang SER reference](../../ThirdParty/Slang-win/share/doc/slang/shader-execution-reordering.md) (pinned v2026.22), [vk_raytracing_tutorial_v2_reference.md](../../reference/vk_raytracing_tutorial_v2_reference.md), [ray_tracing_mega_geometry.md](../features/planned/ray_tracing_mega_geometry.md) §6
**Considerations:** C-G1-1 (`ClusterIDNV` through `spirv_asm`), C-G1-2 (no subgroup ops after `ReorderThread`)
**Follow-ups:** [g1_ser_followups.md](./g1_ser_followups.md) (alignment with NVIDIA's SER code)
**History:** written 2026-11-08 as `spec/features/shader_execution_reordering.md`; moved here on 2026-11-16 when M4 started

Secondary rays scatter: neighbouring pixels in a warp hit different objects with different materials, so the warp runs every material's code path with most lanes idle, and fetches unrelated memory. SER lets the hardware regroup threads by what they hit **between traversal and shading**, so each warp shades one material over coherent data. It is the main lever on the indirect bounce and the cache training paths, the most expensive and most incoherent work in the frame (spec §2.3: 3.0–3.5 ms indirect, 0.6–0.8 ms cache).

G1 builds the pipeline infrastructure and proves it on primary rays, where the result is known exactly (the ray-query path of C3). It shades nothing new: the first shaded-ray pass is G2.

---

## 1. Where it applies

| Pass | Rays | SER |
|---|---|---|
| Indirect bounce (G2) | 1 per pixel, shaded at the hit | yes |
| Cache training paths (NRC update; G3) | multi-bounce, shaded at every vertex | yes, reorder at every bounce; largest gain |
| RTXDI final visibility, path shadow rays, ReSTIR GI reconnection | visibility only | no: ray queries (`traceVisibility`), nothing to shade |
| Primary visibility | — | no: raster + vis buffer. G1's check raygen traces primary rays only to compare with C3 |

---

## 2. API facts

| Fact | Source | Consequence |
|---|---|---|
| `VK_EXT_ray_tracing_invocation_reorder` + `rayTracingInvocationReorder`, `rayTracingPipeline`, `rayTracingPipelineTraceRaysIndirect2`, `rayTracingPositionFetch`, `clusterAccelerationStructure` are enabled | [vk_device.cpp](../../src/core/vk_device.cpp) | nothing added at device creation |
| `rayTracingInvocationReorderReorderingHint` is `REORDER` on the RTX 5070 (driver 621.01) | startup log `SER: hint=Reorder` | the 5070 reorders in hardware |
| `VkPhysicalDeviceRayTracingPipelinePropertiesKHR` cached in `HardwareCapabilities::rayTracingPipeline` | [vk_device.cpp](../../src/core/vk_device.cpp) | SBT handle size, handle alignment and base alignment |
| `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` through `VkPipelineCreateFlags2CreateInfo`, `VK_NULL_HANDLE` layout | SDK 1.4.363, [VK_EXT_descriptor_heap.md](../VkExtensionsSpecs/VK_EXT_descriptor_heap.md) | same heaps and `pushDataEXT` as the shader objects |
| `VkRayTracingPipelineClusterAccelerationStructureCreateInfoNV { allowClusterAccelerationStructure }` | SDK `vulkan_core.h` | hits on CLAS geometry and `ClusterIDNV` |
| EXT `HitObject` on Slang 2026.22: `TraceRay`, `ReorderThread`, `Invoke`, `IsHit`, `GetInstanceIndex`, `GetPrimitiveIndex`, `GetAttributes`, `GetTriangleVertexPositions`, `GetObjectToWorld` / `GetWorldToObject` (`float4x3`), `GetRayDesc().TMax` = hit distance (`OpHitObjectGetRayTMaxEXT`) | SER4 probe, `spirv-val` clean | everything but the cluster id comes from the hit object |
| `HitObject::GetRayTCurrent` is HLSL / CUDA only: the core module declares `[require(hlsl, …)]`, `[require(cuda, …)]` and has no SPIR-V case, in 2026.22 and on master (2026-11-17). The SER doc's "Vulkan EXT" note is wrong. No flag helps: `-ignore-capabilities` fails with E41009 (no body), `-emit-spirv-via-glsl` and `-target glsl` with E36107 | SER4 probe; core-module source checked 2026-11-17 ([follow-ups](./g1_ser_followups.md) S5) | `GetRayDesc().TMax` instead, which emits the one `OpHitObjectGetRayTMaxEXT`. Upstream: issue [#12554](https://github.com/shader-slang/slang/issues/12554), fix in draft PR [#12566](https://github.com/shader-slang/slang/pull/12566) (open 2026-11-17); switching gains nothing, the value is the same |
| `HitObject::GetClusterID` is NV-only; no `ClusterID()` intrinsic for SPIR-V hit shaders | SER4 probe | the closest-hit invoke stays and reads `ClusterIDNV` through `spirv_asm` (C-G1-1). Closes Q8 |
| `[vk::constant_id(0)] const bool` in a module is emitted as `OpSpecConstantTrue` + `SpecId 0` | probe | reordering is a specialization constant (§3) |
| Slang 2026.22 emits `RAY_FLAG_FORCE_OMM_2_STATE` without its SPIR-V capability | considerations C-C4-3 | `traceShaded` calls `requireOmm2StateRayFlag()` |

---

## 3. Design

```
// raygen (raytracing/ser.slang traceShaded)
hit = HitObject::TraceRay(tlas, SKIP_PROCEDURAL | FORCE_OMM_2_STATE, 0xff, 0, 0, 0, ray, payload)
if (kIsSerReorderEnabled) ReorderThread(hit)            // no hint before M7 (follow-ups S1)
if (hit.IsHit()) HitObject::Invoke(hit, payload)      // closest-hit writes ClusterIDNV only
RayHit from the hit object + payload.clusterId → decodeHit → sampleMaterial → BRDF (G2)
```

| Decision | Rule | Why |
|---|---|---|
| Hint | material shader bin of the hit primitive from M7 (spec §9.1); misses get their own value | groups lanes that run the same shading code |
| Hint before M7 | none: `ReorderThread(hit)`. The sort already groups by shader (hit group, miss) and NOPs before any hint; NVIDIA's `vk_gltf_renderer` and `ser_pathtrace` use the plain form. Replaced the 1-bit hit/miss hint on 2026-11-16 ([follow-ups](./g1_ser_followups.md) S1) | the hint would repeat the shader sort |
| Payload | `ClusterPayload { clusterId }`, 4 B | the only field the EXT hit object lacks |
| `RayHit` | the same struct `traceClosest` fills, so `decodeHit` serves both paths | one decode path |
| Misses | not invoked; the miss shader is an empty SBT record | nothing to shade |
| Shading location | raygen, after the invoke | same functions as the vis-buffer resolve |
| SBT | 1 raygen, 1 miss, 1 hit group (closest-hit only), each region at `shaderGroupBaseAlignment` | spec §11.1, no any-hit |
| Reorder on / off | specialization constant `kIsSerReorderEnabled` (`constant_id 0`), one pipeline per mode | a runtime branch costs 0.18 ms on the off path (§5) |
| Live state across `ReorderThread` | ray, throughput, RNG state, pixel id only | everything live moves with the thread |
| After `ReorderThread` | no subgroup operations | they miscount or lose the device (C-G1-2) |
| Push data | per pass, root address first, as the compute passes | one root pointer |

### 3.1 Host module

[vk_rt_pipeline.hpp](../../src/render/vk_rt_pipeline.hpp), class `RtPipeline`, one instance per shaded-ray pass and reorder mode:

- **Create:** `RtPipeline::Source { file, raygenEntry, missEntry, closestHitEntry, debugName, isReorderEnabled }`. One `VkShaderModule` from `readPassSpirv` (shared with `ShaderObjects`), the three stages, the specialization constant on raygen, `maxPipelineRayRecursionDepth = 1`, the descriptor-heap flag and the cluster create info. Named `RtPipeline_<name>`.
- **SBT:** written once at creation into a host-visible buffer (ReBAR, no staging), base aligned to `shaderGroupBaseAlignment`.
- **Dispatch:** `traceRays(cmd, slot, extent)` writes the slot's `VkTraceRaysIndirectCommand2KHR` (SBT regions + extent) into a per-slot host buffer, then `vkCmdTraceRaysIndirect2KHR`. The slot's fence was waited, and host writes before the submit are visible to it.
- **Sync:** the ray submit's TLAS wait includes `RAY_TRACING_SHADER` (it covered compute only).

### 3.2 Shader modules

| File | Content |
|---|---|
| [raytracing/ser.slang](../../shaders/raytracing/ser.slang) | `ClusterPayload`, hint constants, `kIsSerReorderEnabled`, `writeClusterPayload` (closest-hit body), `traceShaded(root, ray, out RayHit)` |
| [passes/rt_ser_check.slang](../../shaders/passes/rt_ser_check.slang) | `serCheckRaygen`, `serCheckClosestHit`, `serCheckMiss`: the §4 check |

Each SER pass is one `.spv` with three entries: `engine_add_slang_pass(... ENTRIES raygen closesthit miss)`.

---

## 4. Verification

`rt_ser_check` runs inside `ENGINE_RT_CHECK=1` after the compute ray check: every pixel's primary ray through `traceShaded`, then through `traceClosest`, and compares hit status, `(rayInstance, meshletRow, triangle)`, barycentrics, hit distance, the fetched positions and both transforms **bit for bit**. One dispatch per reorder pipeline into `GpuSerCheckStats[2]`; `EngineRayCheck` fails a frame on any mismatch and logs `SER check, reorder off|on` summary lines. The SER trace runs first, so only the ray lives across the reorder. Counters use one atomic per lane (C-G1-2).

Results (2026-11-16, RTX 5070, driver 621.01):

| Run | Frames | Rays | Hits | Mismatches (off / on) |
|---|---|---|---|---|
| standalone, startup view | 32 | 13.1 M | 13.1 M | 0 / 0 |
| `ENGINE_CULL_CHECK` poses | 454 | 186 M | 186 M | 0 / 0 |
| `ENGINE_GUIDE_CAPTURE` models (sky: misses covered) | 246 | 101 M | 11.8 M | 0 / 0 |

Maximum barycentric and distance error: 0. Validation: core + sync + best practices (NVIDIA) clean apart from NGX's known messages (considerations H1); GPU-AV clean. `spirv-val` clean.

---

## 5. Measurement

Release, validation off, bench mode (`ENGINE_RT_CHECK=1 ENGINE_BENCH_FRAMES=2000`), startup view, 1280×720 window (853×480 render, DLSS-RR Quality), RTX 5070. Each span is the whole check pass: SER trace + `traceClosest` + per-lane atomics.

| Variant | SER check span |
|---|---|
| reorder compiled out (`kIsSerReorderEnabled = false`) | 0.69 ms |
| reorder on | 0.77 ms |
| reorder behind a runtime push-data branch, taken off | 0.87 ms |

Dispatch order has no effect (both spans equal when both use one mode). Primary rays are coherent, so reordering only adds cost here; the gain is measured on G2's incoherent bounce rays. A runtime branch around `ReorderThread` slows the off path by 0.18 ms even when not taken, hence the specialization constant.

On G2's bounce rays (2026-11-17, [follow-ups](./g1_ser_followups.md) §6): `ReorderThread` costs ~0.067 ms at 853×480 by itself, the closest-hit `Invoke` < 1 %, and the reorder still does not pay with one material model.

Still open: the 2060 Super's logged hint and on/off numbers.

---

## 6. Tasks

| # | Task | Status |
|---|---|---|
| SER1 | query and cache `VkPhysicalDeviceRayTracingInvocationReorderPropertiesEXT`, log the hint | done (M0) |
| SER4 | compile probe on the pinned Slang: EXT `HitObject` accessors, cluster id, OMM flag, `spirv-val` | done 2026-11-16 (§2) |
| SER2 | `vk_rt_pipeline`: pipeline + SBT, `traceRaysIndirect2`, descriptor-heap flag, reorder specialization constant | done 2026-11-16 |
| SER3 | `raytracing/ser.slang`: payload, closest-hit body, `traceShaded` | done 2026-11-16 |
| SER5 | hint: hit / miss until M7, 1 bit | done 2026-11-16; material bins from M7 |
| SER6 | `rt_ser_check` in `ENGINE_RT_CHECK`, timer scopes `SER check` / `SER check, reorder`, §5 numbers | done on the 5070; 2060 Super open |

Moved out of G1: the UI reorder toggle goes to G2 (done), the first pass a user sees; with the specialization constant it switches between two pipelines. The `SER` source for the `Ray *` debug views is dropped: the check compares every field bit for bit, which a debug image would only repeat.

---

## 7. Driver finding: subgroup ops after `ReorderThread` (C-G1-2)

Bisected 2026-11-16 by swapping `rt_ser_check.spv` variants (RTX 5070, driver 621.01, Slang 2026.22):

| Raygen after the reorder | Result |
|---|---|
| `HitObject::TraceRay`, no reorder, wave-op counters | correct |
| `ReorderThread(hint, bits)` only, per-lane atomic | correct |
| `ReorderThread(hint, bits)` only, `WaveActiveCountBits` + `WaveIsFirstLane` counter | 13,098,546 of 13,102,080 counted |
| `TraceRay` + any `ReorderThread` form (hit object, hint only, hit object + hint; with or without the OMM flag) + wave-op counters | device lost |
| `TraceRay` + `ReorderThread` + per-lane atomics | correct |

The SPIR-V keeps the order (`OpReorderThreadWithHintEXT`, then `OpGroupNonUniformBallot`). To recheck on a new driver: put a `WaveActiveCountBits` counter after `ReorderThread` in `rt_ser_check.slang` and run `ENGINE_RT_CHECK=1`.

---

## 8. Must not

- shade in closest-hit, or add per-material hit groups;
- add any-hit shaders;
- compute the hint from anything the hit does not determine (pixel position, ray direction) as the main key;
- carry large structs across `ReorderThread`;
- use subgroup operations after `ReorderThread` (C-G1-2);
- branch on a runtime value around `ReorderThread`: use the specialization constant;
- use `TraceReorderExecute` with a pre-trace hint as the main path;
- trace a shaded ray without the OMM 2-state flag (C-C4-3);
- assume a reordering gain on the floor card before the logged hint and timings show one.

---

## 9. Log

| Date | Change |
|---|---|
| 2026-11-08 | written as the SER feature spec; SER1 done |
| 2026-11-16 | moved to `spec/m4/`; Slang 2026.22 (`sm_6_10`); SER4 probe; `RtPipeline`, `raytracing/ser.slang`, `rt_ser_check` in `ENGINE_RT_CHECK`; reorder as a specialization constant after the runtime branch measured 0.18 ms; subgroup-op driver issue bisected (§7); check passes bit for bit on the 5070 |
