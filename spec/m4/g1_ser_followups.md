# M4 · G1 follow-ups: SER aligned with NVIDIA's code

**Started:** 2026-11-16 · **Status:** built 2026-11-17 (S1, S2 shipped; S3–S5 measured, no change; S6 default stays off) · **Milestone:** M4
**Implements:** review of [G1](./g1_ser_rt_pipeline.md) and [G2](./g2_indirect_pass.md) against NVIDIA's SER code and the SER docs
**Depends on:** G1 (`traceShaded`, `RtPipeline`, SER check), G2 (`IndirectPass`, `Indirect` timer scope)
**Used by:** [G3](./g3_nrc.md) update paths, M7 material bins (hint)
**Considerations:** C-G1-1, C-G1-2, C-G2-2

G1 and G2 were built from the Slang reference and our own probes. This step compares them with what NVIDIA engineers wrote for the same API. Each change below follows a named NVIDIA source. Where no NVIDIA code covers our case (EXT SER on cluster geometry), the step measures and stops, and does not invent a pattern.

---

## 1. Sources

| Source | What it shows |
|---|---|
| `reference/vk_gltf_renderer/shaders/raytracer_interface.h.slang` (`TraditionalRaytracer`) | path tracer: `HitObject::TraceRay` → `ReorderThread(hitObj)` (no hint) → fields read from the hit object, **no `Invoke`**; shading in raygen; shadow rays as `HitObject::TraceRay` + `IsHit()`, no reorder |
| `reference/vk_gltf_renderer/src/renderer_pathtracer.cpp` | `USE_SER` specialization constant id 0, pipelines rebuilt on toggle; SER allowed only when `rayTracingInvocationReorderReorderingHint` has `REORDER`; the UI checkbox shown only then |
| `reference/vk_gltf_renderer/docs/benchmarking.md` | SER ~2.3–2.8× on divergent volumetric scatter scenes; the RT pipeline ~2× over ray query in compute |
| `reference/vk_mini_samples/samples/ser_pathtrace/` (`.slang`, `.rgen.glsl`, `.cpp`, README) | same trace → `ReorderThread(hitObj)` → read fields, `Invoke` commented out ("the information can be retrieved from hitObjectGet functions"); shadow ray: `HitObject::TraceRay` with `ACCEPT_FIRST_HIT_AND_END_SEARCH \| SKIP_CLOSEST_HIT_SHADER`, `IsHit()`; reports the reordering hint property |
| `reference/vk_mini_samples/samples/ray_trace_clusters/shaders/raytrace_clusters.slang` | `ClusterIDNV` read in the closest-hit through `spirv_asm`, "Slang has no dedicated intrinsic for it outside of HitObject": our C-G1-1 workaround exactly |
| `reference/vk_lod_clusters/shaders/render_pathtrace.rgen.glsl`, `render_pathtrace_clusters.rchit.glsl` | cluster path tracer without SER: the closest-hit writes `gl_ClusterIDNV` into the payload; shadow and AO rays are `traceRayEXT` with `TerminateOnFirstHit \| SkipClosestHitShader` and a miss shader |
| `reference/RTXMG-main/demo/shaders/rtxmg_demo_path_tracer.hlsl` | cluster id from `NvRtGetClusterID()` (NVAPI) in hit shaders; no SER |
| [Slang SER reference](../../ThirdParty/Slang-win/share/doc/slang/shader-execution-reordering.md) (pinned 2026.22) and [master](https://github.com/shader-slang/slang/blob/master/docs/shader-execution-reordering.md) | `ReorderThread`: at most 16 hint bits, use the fewest, the same count on every thread, NOPs grouped together, launch locality kept where possible. EXT-only fused `ReorderExecute` / `TraceReorderExecute`. `FromRayQuery` on EXT. `GetGeometryIndex` on EXT. `GetClusterID` NV-only. `GetRayTCurrent` listed for Vulkan EXT |
| [Vulkan sample `ray_tracing_invocation_reorder`](https://docs.vulkan.org/samples/latest/samples/extensions/ray_tracing_invocation_reorder/README.html) | sort order: shader ID, then the app hint, then implementation data. Hint: instance id on hits, 0 otherwise, 8 bits. Good hints: material ids, control-flow flags, texture indices, path length / Russian roulette. Keep live state small (fp16, packed flags, no unused payload fields). Helps most: multi-bounce, many hit shaders, scattered secondary rays; least: coherent rays, one uber-shader. 11–24 % in path tracing, 40–50 % in divergent synthetic tests |

No NVIDIA sample combines EXT SER with cluster acceleration structures: the cluster samples read the cluster id in a closest-hit, and the SER samples skip the closest-hit.

---

## 2. Comparison

| Topic | NVIDIA | Ours (G1/G2) | Action |
|---|---|---|---|
| Shade in raygen after the trace | yes | yes | — |
| Reorder switch | specialization constant id 0, one pipeline per value | same | — |
| Hit distance | `GetRayDesc().TMax` | same | re-probe `GetRayTCurrent` (S5) |
| Live state across the reorder | minimal | pixel + bounce ray only, frame data reloaded after | — |
| Reorder call | `ReorderThread(hitObj)` | `ReorderThread(hitObj, isHit ? 0 : 1, 1)` | S1 |
| Device gate | `REORDER` in `rayTracingInvocationReorderReorderingHint` | logged, not used | S2 |
| Closest-hit | not invoked | invoked on hits for `ClusterIDNV` (C-G1-1) | S3: measure; fused call as the NVIDIA-API option |
| Visibility rays in RT raygens | hit object or `traceRayEXT`, skip closest-hit, first hit ends | `RayQuery` (`traceVisibility`) | S4: A/B |
| Subgroup ops after the reorder | not covered by any doc or sample | forbidden (C-G1-2, driver 621.01) | — |
| Where SER pays | multi-bounce, many materials | one bounce, one material model: reorder measured slower (G2 §4) | S6: re-measure after S1–S3 |

---

## 3. Changes

### S1 · Plain `ReorderThread(hitObject)`

- `traceShaded` calls `ReorderThread(hitObject)`. Remove `kSerHintHit`, `kSerHintMiss`, `kSerHintBits`.
- **Why:** hits and misses run different shaders, and the hardware sorts by shader ID before any hint; NOPs are grouped as well. The 1-bit hint repeats what the sort already does. Both NVIDIA path tracers use the plain form.
- The G1 §3 hint rule for M7 stays: a material-bin hint returns with material bins, with the fewest bits that cover the bins and the same count on every thread.

### S2 · Reorder only where the device reorders

- `HardwareCapabilities` gets `isSerReordering = rayTracingInvocationReorderReorderingHint == eReorder`.
- `IndirectPass` (and every later SER pass) clamps `isReorderEnabled` to it. The UI disables the checkbox with a tooltip naming the property when it is false.
- The SER check keeps running both pipelines: with no hardware reorder, `ReorderThread` is a no-op and the comparison is still exact.
- **Why:** `renderer_pathtracer.cpp` sets `m_useSER = m_useSER && m_supportSER` from the same property. Expected: the RTX 2060 Super reports `None`.

### S3 · Closest-hit invoke

The invoke stays: it is the cluster samples' pattern (`raytrace_clusters.slang`, `render_pathtrace_clusters.rchit.glsl`), and the EXT hit object has no cluster id. This step measures it.

1. **Cost.** A bench-only `.spv` variant skips `Invoke` and sets `meshletRow = 0`. The images are wrong, the timing is the upper bound of any gain. Never committed.
2. **If the cost is ≥ 5 % of the `Indirect` scope** (above bench noise), try the fused EXT call from the Slang reference: `ReorderExecute(hitObject, payload)` replaces `ReorderThread` + `Invoke` in the reorder-on pipeline. Misses then run the empty miss shader, NOPs invoke nothing. Adopt only if faster and the SER check stays bit-exact.
3. **If the cost is below that,** record the number in G1 §5 and stop.

Out of scope here, since no NVIDIA code does it: carrying the cluster id in the CLAS geometry index (`GetGeometryIndex()`, 24 bits) or tracing with a ray query and converting with `FromRayQuery`. Either needs its own decision.

### S4 · Visibility rays inside RT raygens

- A/B, in the indirect pass's `directLightAt` only:
  - **A:** `traceVisibility` (`RayQuery`, today);
  - **B:** `HitObject::TraceRay` with `ACCEPT_FIRST_HIT_AND_END_SEARCH | SKIP_CLOSEST_HIT_SHADER` plus the existing flags, then `IsHit()`, with no reorder and no invoke.
- B follows `ser_pathtrace.slang` `traceShadow` and `vk_gltf_renderer` `TraceShadow`.
- Adopt B for RT raygens only if it is faster beyond noise. Compute passes keep `RayQuery`.
- B adds no subgroup ops. It is a trace after the reorder, the same class of code as `directLightAt` today.

### S5 · `GetRayTCurrent` re-probe

- G1 §2 says `GetRayTCurrent` is HLSL/CUDA only. The pinned 2026.22 doc lists it for Vulkan EXT (`GetRayTMax` in SPIR-V).
- Re-probe with the pinned compiler, then fix the G1 §2 row and the `ser.slang` comment either way.
- Keep `GetRayDesc().TMax` unless `GetRayTCurrent` emits the same single `OpHitObjectGetRayTMaxEXT`.

### S6 · Re-measure G2

- After S1–S3, repeat G2 §4 on an idle GPU (no other GPU process; check utilization first): both poses, reorder on and off.
- Set the default from the result. Update G2 §4 and the `vk_indirect.hpp` comment.

---

## 4. Rules for later SER passes (G3, M7)

From the Vulkan sample and the Slang reference:

- **Hint:** material bin or a control-flow flag (path terminated, Russian roulette), the fewest bits that cover the values, the same count on every thread.
- **Live state across the reorder:** the minimum, packed. Path throughput and flags in fp16 / bitfields where precision allows; reload frame data after the reorder.
- **Inactive lanes:** join the reorder with `MakeNop()` (grouped together) rather than branching around it.
- **NVIDIA's measured gains** (11–24 % path tracing, 2.3–2.8× divergent scatter) come from multi-bounce paths with many materials. Measure G3's update paths with the switch, and do not assume the gain.

---

## 5. Verification

| Check | Pass |
|---|---|
| `ENGINE_RT_CHECK=1` SER check | both modes bit-exact against `traceClosest` |
| `ENGINE_GUIDE_CAPTURE=1` | ReSTIR DI convergence check passed, normal checks 2/2, `indirect_samples` unchanged in kind |
| Validation, core + sync + best practices | clean apart from the known NGX messages |
| GPU-AV | G1 SER check clean; indirect pass through the `traceClosest` variant (C-G2-2) |

## 6. Measurement protocol

- Release build, no layers (`VK_LOADER_LAYERS_DISABLE='*validation*' VK_KHRONOS_VALIDATION_GPUAV_ENABLE=false`), `ENGINE_BENCH_FRAMES=2000`, RTX 5070.
- Variants interleaved by `.spv` swap, at least three rounds each.
- GPU idle before each round. Results within ±5 % count as equal.
- Poses: startup gallery and courtyard `lookAt({6, 2.5, 1.5}, {-10, 1, -1})`.
- Report the `Indirect` scope, plus `SerCheck` / `SerCheckReorder` for S1.

### Results (2026-11-17, RTX 5070, driver 621.01, idle GPU)

Medians of three interleaved rounds (round 1 dropped where its first runs read warm-up outliers), `Indirect` scope in ms, 853×480:

| Variant | Gallery off | Gallery on | Courtyard off | Courtyard on |
|---|---|---|---|---|
| S1 `ReorderThread(hitObject)` (shipped) | 0.420–0.433 | 0.458–0.484 | 0.547 | 0.569 |
| old 1-bit hit/miss hint | — | 0.462 | — | 0.571 |
| trace only, with `Invoke` | 0.310 | 0.379 | 0.343 | 0.410 |
| trace only, no `Invoke` (bench-only) | 0.310 | 0.376 | 0.342 | 0.410 |
| S4 hit-object visibility (B) | 0.422 | 0.457 | 0.551 | 0.566 |

- **S1:** plain and hinted reorder are equal within noise. Plain form shipped; SER check bit-exact in both modes.
- **S2:** the 5070 reports `Reorder`; the clamp and UI state are in. The `None` path is untested until the 2060 Super is measured.
- **S3:** `Invoke` costs nothing measurable (< 1 %, below the 5 % rule): the closest-hit stays, and no fused call is needed. Slang 2026.22 also lacks the Slang-syntax `ReorderExecute` its bundled doc lists; only GLSL `hitObjectReorderExecuteEXT` exists.
- **S4:** hit-object visibility equals `RayQuery` within noise (±0.5 %). `traceVisibility` stays.
- **S5:** `GetRayTCurrent` has no SPIR-V implementation: Slang's core module (`hlsl.meta.slang`, 2026.22 and master) requires `hlsl` or `cuda` and switches only on those two targets, so the doc's "Vulkan EXT" note is a doc error, not a flag issue. `-ignore-capabilities` reaches codegen and fails with E41009 (no body for SPIR-V); `-emit-spirv-via-glsl` and `-target glsl` fail with E36107. `GetRayDesc().TMax` stays: it emits `OpHitObjectGetRayTMaxEXT`, the same value. Upstream: issue [#12554](https://github.com/shader-slang/slang/issues/12554), draft PR [#12566](https://github.com/shader-slang/slang/pull/12566) adds the GLSL/SPIR-V cases (open as of 2026-11-17).
- **S6:** `ReorderThread` itself costs ~0.067 ms on this pass (trace-only rows: 0.310 → 0.378, 0.343 → 0.410). The shading it regroups saves ~0.045 ms at the courtyard and less at the gallery. Net: reorder off stays faster, 0.42 vs 0.46 ms and 0.55 vs 0.57 ms, so the default stays off. The gain NVIDIA reports needs divergent multi-bounce shading (G3 update paths, M7 material bins), where the fixed cost is amortized.
- Validation (core + sync + best practices): clean apart from the known NGX messages. Convergence check passed, normal checks 2/2. GPU-AV: SER check 358/358 both modes with the indirect pass on its `traceClosest` variant (C-G2-2).

---

## 7. Tasks

- [x] S1 plain `ReorderThread`; SER check bit-exact
- [x] S2 `isSerReordering` capability, clamp in `IndirectPass`, UI state
- [x] S3 `Invoke` cost measured: < 1 %, `ReorderExecute` not needed
- [x] S4 visibility A/B in `directLightAt`: equal, `RayQuery` stays
- [x] S5 `GetRayTCurrent` probe: still unavailable on SPIR-V; G1 §2 updated, `ser.slang` comment holds
- [x] S6 G2 re-measured on an idle GPU; default stays off
- [x] G1 §2/§3/§5 and G2 §4 updated with the results; no new driver finding

## 8. Must not

- Use NV-only `HitObject` methods (`GetClusterID`, `MakeHit`) or `VK_NV_ray_tracing_invocation_reorder`.
- Add subgroup operations after `ReorderThread` (C-G1-2).
- Change how the cluster id reaches the shader (geometry index, `FromRayQuery`) inside this step.
- Commit the bench-only no-invoke variant.

## 9. Log

| Date | Change |
|---|---|
| 2026-11-17 | built and measured (§6 results): S1 plain reorder and S2 device gate shipped; `Invoke`, hit-object visibility and the hint cost nothing either way; reorder off stays the G2 default |
| 2026-11-16 | created from the review of NVIDIA's SER code (`vk_gltf_renderer`, `vk_mini_samples` `ser_pathtrace` / `ray_trace_clusters`, `vk_lod_clusters`, RTXMG) and the Slang and Vulkan SER docs |
