# Feature: shader execution reordering (SER)

**Date:** 2026-11-08 · **Status:** extension and feature enabled; no RT pipeline, no SER code yet · **Milestone:** M4 ([engine_spec.md](../../docs/engine_spec.md) §16.2), infrastructure with the first shaded-ray pass
**Implements:** engine spec §11.1 (ray model), §2.2 SER row, Q8; plan G1 ([pbr_implementation_plan.md](../pbr_implementation_plan.md))
**Depends on:** M2 (cluster RT scene, `GpuRayInstance`, `decodeHit`), M3 (`sampleMaterial`, BRDF, RTXDI sampling)
**Related:** [Slang SER reference](../../ThirdParty/Slang-win/share/doc/slang/shader-execution-reordering.md) (pinned v2026.20), [vk_raytracing_tutorial_v2_reference.md](../../reference/vk_raytracing_tutorial_v2_reference.md)

Secondary rays scatter: neighbouring pixels in a warp hit different objects with different materials, so the warp runs every material's code path with most lanes idle, and fetches unrelated memory. SER lets the hardware regroup threads by what they hit **between traversal and shading**, so each warp shades one material over coherent data. It is the main lever on the indirect bounce and the cache training paths, the most expensive and most incoherent work in the frame (spec §2.3: 3.0–3.5 ms indirect, 0.6–0.8 ms cache).

---

## 1. Where it applies

| Pass | Rays | SER |
|---|---|---|
| Indirect bounce (plan G2) | 1 per pixel, shaded at the hit | yes |
| Cache training paths (NRC update; G3) | multi-bounce, shaded at every vertex | yes, reorder at every bounce; largest gain |
| RTXDI final visibility, path shadow rays, ReSTIR GI reconnection | visibility only | no: ray queries, nothing to shade |
| Primary visibility | — | no: raster + vis buffer |

---

## 2. API facts (verified 2026-11-08)

| Fact | Source | Consequence |
|---|---|---|
| `VK_EXT_ray_tracing_invocation_reorder` + feature `rayTracingInvocationReorder` are enabled | [vk_device.cpp](../../src/core/vk_device.cpp) | nothing to add at device creation |
| `VkPhysicalDeviceRayTracingInvocationReorderPropertiesEXT { rayTracingInvocationReorderReorderingHint, maxShaderBindingTableRecordIndex }`; hint is `NONE` or `REORDER` | SDK 1.4.363 `vulkan_core.h` | query, cache in `HardwareCapabilities`, log at startup (spec M0). `NONE` means reorder calls are legal no-ops |
| `HitObject::TraceRay`, `ReorderThread(hit, hint, bits)` (max 16 hint bits), `HitObject::Invoke`, `MakeMiss`, `MakeNop`, `GetInstanceIndex`, `GetPrimitiveIndex`, `GetAttributes` | pinned Slang doc | the engine's pattern (§3) is expressible |
| `HitObject::GetTriangleVertexPositions` is **EXT-only** (needs position fetch) | pinned Slang doc | positions straight from the hit object |
| `HitObject::GetClusterID` is **NV-only** | pinned Slang doc | cannot be combined with the EXT path; the cluster id comes from the closest-hit invoke (§3). Closes Q8 for now |
| Fused `TraceReorderExecute` (EXT) takes the coherence hint **before** tracing | pinned Slang doc | not used: the engine's hint depends on what was hit |
| Shader objects have no RT stages; DGC on driver 621.01 reports no RT stages | spec §3, Q4 | `VkPipeline` + SBT, dispatch with `vkCmdTraceRaysIndirect2KHR` (`rayTracingPipelineTraceRaysIndirect2` is enabled) |

---

## 3. Design

One shape for every shaded-ray pass:

```
// raygen
hit = HitObject::TraceRay(tlas, flags, 0xff, 0, 0, kMissIndex, ray, payload)   // traversal only, no shading
hint = hit.IsHit() ? materialBin(rayInstances[hit.GetInstanceIndex()].primitiveRow) : kMissBin
ReorderThread(hit, hint, kHintBits)                                           // regroup by material
HitObject::Invoke(hit, ids)          // minimal closest-hit: { clusterId, triangle, barycentrics, hitT }
shade in raygen: decodeHit(ids, hit.GetTriangleVertexPositions()) → sampleMaterial → BRDF → light / cache
```

| Decision | Rule | Why |
|---|---|---|
| Hint | material shader bin of the hit primitive: the same bin ids as vis-buffer classification (spec §9.1); misses get `kMissBin` | groups lanes that run the same shading code and touch the same material data |
| Hint source | `InstanceIndex()` → `GpuRayInstance.primitiveRow` → `GpuPrimitive.materialId` → bin; no cluster id needed | available before the invoke, on the EXT path |
| Hint bits | `ceil(log2(binCount + 1))`, at most 16, uniform across the dispatch (a link-time constant) | fewer bits sort faster; the Slang doc asks for the minimum |
| Before M7 (no bins yet) | hint = material shader id (one PBR shader: hit vs miss, plus `AlphaTested`) | still separates misses and alpha-tested hits |
| Shading location | raygen, after the invoke | one shading path, the same functions as the vis-buffer resolve |
| Closest-hit | one minimal shader writing ids, barycentrics and `hitT` into a 24 B payload | the only way to read `ClusterID` on the EXT path |
| SBT | per pass: 1 raygen, 1 miss, 1 hit group (closest-hit only). No any-hit (alpha is OMM 2-state) | spec §11.1 |
| Live state across `ReorderThread` | only the ray, throughput, RNG state and the pixel id | everything live across the reorder moves with the thread; large state means spills |
| Pipeline | `VkPipeline` with the descriptor-heap pipeline flag (like the shader objects) and `allowClusterAccelerationStructure`; built at startup | spec §3 |
| Toggle | `DebugViewSettings`-style UI switch: reorder on / off (`ReorderThread` skipped, same image) | proves the gain on each GPU |

---

## 4. Measurement

Release build, vkconfig and validation off (vulkan-dev skill §6.1).

| Card | Expected | Record |
|---|---|---|
| RTX 5070 (hardware SER since Ada) | reorder on is faster for the indirect bounce and much faster for training paths | on/off timings per pass, in the spec §2.3 table |
| RTX 2060 Super | hint likely `NONE`: no gain, no cost | the logged hint and on/off timings; the floor budget assumes no gain (spec §2.2) |

Done when: the image is identical with reorder on and off; the on/off numbers are recorded for both cards; the reorder hint is logged at startup.

---

## 5. Tasks

| # | Task | Milestone |
|---|---|---|
| SER1 | query and cache `VkPhysicalDeviceRayTracingInvocationReorderPropertiesEXT`, log the hint (**done**) | M0 |
| SER2 | `vk_rt_pipeline`: RT pipeline + SBT builder (1 raygen, 1 miss, 1 hit group), `traceRaysIndirect2` dispatch, descriptor-heap flag | M4 (G1) |
| SER3 | `raytracing/*` modules: payload (24 B), minimal closest-hit, miss, the §3 raygen skeleton | M4 |
| SER4 | compile probe on the pinned Slang: EXT `HitObject` + `GetTriangleVertexPositions` + cluster AS in one pipeline, `spirv-val` clean; recheck whether `GetClusterID` works on the EXT path (would drop the invoke) | M4, first |
| SER5 | material-bin hint (§3), miss bin, hint-bit constant | M4; bins from M7 |
| SER6 | reorder on/off toggle and the §4 measurements | M4 |

---

## 6. Must not

- shade in closest-hit, or add per-material hit groups;
- add any-hit shaders;
- compute the hint from anything the hit does not determine (pixel position, ray direction) as the main key;
- carry large structs across `ReorderThread`;
- use `TraceReorderExecute` with a pre-trace hint as the main path;
- assume a reordering gain on the floor card before the logged hint and timings show one.
