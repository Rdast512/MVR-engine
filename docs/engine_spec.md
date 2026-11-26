# Engine end-goal specification

**Date:** 2026-10-31, updated 2026-11-25 · **Status:** target architecture, normative for new work · **Working tree:** `feature-pbr` on `3d24416c6`
**Audit:** §20 holds the 2026-11-10 architecture audit, its decisions and the documents named below that are not in the repository (§20.4).
**Inputs:** `end_goal.md` (target) and `meshoptimizer.md`, both folded into this spec and not in the repository; [pbr_implementation_plan.md](../spec/pbr_implementation_plan.md), [resource_architecture.md](../spec/resource_architecture.md), [shader_architecture_plan.md](../spec/shader_architecture_plan.md), [meshlet_culling_options.md](./meshlet_culling_options.md), [culling_comparison.md](../reference/culling_comparison.md), [dgc_comparison.md](../reference/dgc_comparison.md)
**Precedence:** where this spec and an input disagree, this spec wins. §17 lists every overridden decision. Detailed task plans (PBR plan tasks, culling stages) stay valid wherever they don't conflict.

The engine is a **GPU-driven, bindless, hybrid visibility + real-time path tracing renderer**. Its purpose is to show how far a renderer gets when it uses only current GPU features and current algorithms, without legacy fallbacks.

---

## 1. Scope

### 1.1 Goals

1. **Beat NVIDIA's reference demos (RTXPT, RTXDI, RTXGI) in FPS** while hitting **60+ fps with full ray-traced lighting** (no raster lighting, no baked data) on the target tiers of §2, using modern Vulkan 1.4+ and its latest extensions.
2. **One unified engine covering RTXPT, RTXDI, and RTXGI via runtime toggles.** Every lighting stage — direct lighting (`Off` / Streaming RIS / ReSTIR DI), indirect bounce 1 (`Off` / raw uncorrelated 1-bounce PT / ReSTIR GI), multi-bounce radiance cache (`Off` / NRC / SHaRC), and denoiser (`DLSS-RR` / `Off`) — is independently switchable at runtime on the same visibility-buffer + Mega Geometry + OMM base (§11.7).
3. **Zero CPU per-object work per frame.** The CPU writes O(1) frame constants and O(changed) scene deltas. Culling, draw generation, acceleration-structure builds and material binning run on the GPU.
4. **Every stage optimized and measured.** Each pass has a GPU budget (§2.3), a timer and a Tracy zone. A pass over budget is a bug.
5. **Latest hardware paths only:** mesh shaders, shader objects, descriptor heaps, BDA, DGC, RTX Mega Geometry, OMM, SER (once Slang supports EXT `HitObject`), cooperative vectors, DLSS Ray Reconstruction, GPU memory decompression.
6. **Compressed uploads.** Textures and geometry cross PCIe GDeflate-compressed and are decompressed on the GPU with `VK_EXT_memory_decompression`; textures are converted to GPU-native KTX2 automatically (§6.4, §7.2).
7. **CPU and GPU work in parallel.**
   - The CPU never waits for the GPU inside a frame, and the GPU never idles waiting for the CPU.
   - Independent GPU work overlaps across queues: RT structure builds on async compute while graphics rasterizes.
   - Long CPU jobs (asset processing, conversion, cache I/O) run on worker threads while the GPU renders.
   - The jobs and their milestones are in §13.1.

**Final goal (version 1.0):** the full Bistro scene loaded and lit by the complete toggle-composable lighting stack (§11) at the §2.1 targets (milestone M10), beating NVIDIA's `RTXPT`, `RTXDI FullSample`, and `RTXGI IndirectDiffuse` demos in FPS at matched settings. The **1.0 target lighting stack** (decided 2026-11-25) is:
- **Denoiser + Upscaler:** **DLSS-RR** (with toggle to `Off` + `Accumulation`);
- **Direct Lighting:** **RTXDI** — either **Streaming RIS** (Initial Sampling only) or **ReSTIR DI** with **Temporal ON (`Max History = 4–8`), `Basic` bias correction, and Spatial OFF** (full ReSTIR DI modes/presets and `Off` kept as runtime toggles, §11.2);
- **Indirect Lighting (Bounce 1):** **raw uncorrelated 1-bounce PT** (1 probabilistic diffuse-or-specular ray/pixel, SER when Slang adds it + OMM enabled, `ReGIR` or simple/`Power_RIS` NEE for bounce lights; ReSTIR GI and `Off` kept as runtime toggles, §11.3, §11.5);
- **Multi-Bounce Tail:** **NRC** integrated into the pipeline, with a runtime toggle to **SHaRC** (and `Off` for 1-bounce only, §11.4).

**Post-1.0 targets:**
- the same Bistro scene rendered entirely from Nanite-like cluster-LOD geometry built and traversed as in `vk_lod_clusters`, in raster and ray tracing, with the same lighting stack and targets (§16.3, L3);
- multithreaded rendering: CPU frame work and command recording spread over a job system (§16.4);
- switchable ray paths: the SER RT pipeline beside the 1.0 ray-query passes, expected to become the default, and triangle BLAS instead of cluster BLAS for static geometry, each behind a toggle so both ways stay (§16.5);
- animation: glTF node animation, skinning and morph targets, deformed on the GPU, with the ray-traced geometry rebuilt every frame through RTX Mega Geometry cluster templates instead of refits (§16.6, AN1–AN4);
- streaming: geometry streamed per cluster-LOD group with GPU-built CLAS, hit-based streaming for rays, and texture mip streaming, so scenes no longer have to fit in VRAM (§16.7, ST1–ST4);
- game-like hybrid Ray Tracing with a **Cleanest Image Quality** toggle (§16.8, HQ1–HQ3).

1.0 stays static and resident (decided 2026-11-23): one CLAS path, every asset in VRAM, no deformation. The post-1.0 work above builds on that path rather than replacing it.

### 1.2 Non-goals

| Out of scope | Note |
|---|---|
| Audio, gameplay, physics, scripting, networking | rendering only |
| Input beyond a free-fly camera (keyboard + mouse) and ImGui | no controller or other devices |
| Editor / asset authoring | ImGui debug UI only |
| AMD, Intel, mobile, non-RTX NVIDIA | NVIDIA RTX only (PBR plan §2 #0): no vendor checks, fallbacks or ports. NVIDIA-only SDKs and extensions (Mega Geometry, SER, NRC, cooperative vectors, DLSS-RR) are the target stack, not options |
| Raster lighting fallbacks (shadow maps, SSAO, SSR, light probes, lightmaps) | rays only |
| MSAA, TAA, custom denoisers (SVGF and the like) | DLSS-RR owns reconstruction |
| [streaming_server_spec.md](./streaming_server_spec.md) | separate project; must not add work to the render path |
| Geometry and texture streaming in 1.0 | 1.0 scenes must fit in VRAM; streaming is post-1.0 (§16.7) |
| Animation in 1.0 | 1.0 renders glTF files in their base pose; animation is post-1.0 (§7.3, §16.6) |

### 1.3 Principles

| # | Principle | Consequence |
|---|---|---|
| P1 | **Composable toggles over one shared base, no legacy fallbacks** | Tiers change resolution, ray counts, cache settings and LOD error. Runtime toggles switch modern RT pipeline components (Direct: Off / Streaming RIS / ReSTIR DI; Indirect: Off / Raw 1-Bounce PT / ReSTIR GI; Cache: Off / NRC / SHaRC; Denoiser: DLSS-RR / Off; ray paths of §16.5) so one engine reproduces RTXPT, RTXDI and RTXGI on the same visibility-buffer + Mega Geometry base while shipping one target default (§11.7) |
| P2 | **GPU owns the scene per frame** | Persistent GPU scene + delta uploads; the CPU never loops over instances, clusters or lights per frame |
| P3 | **Two access mechanisms** | Buffers through BDA, images and samplers through descriptor-heap indices ([resource_architecture.md §1](../spec/resource_architecture.md#1-overview)) |
| P4 | **One root pointer** | Every pass reaches all scene data from `GpuFrameRoot`; push data carries the root address plus pass-local values |
| P5 | **Measure before and after** | No optimization lands without timer numbers on the reference tier |
| P6 | **Vendor SDKs over homegrown code** | RTXDI, RTXGI (NRC & SHaRC), DLSS-RR, NVIDIA OMM SDK, meshoptimizer. The engine writes only the glue |

---

## 2. Hardware and performance targets

### 2.1 Tiers

| Tier | GPU | Output | DLSS-RR mode (render res) | Target | Role |
|---|---|---|---|---|---|
| **Reference** | RTX 5070 (Blackwell, 12 GB) | 2560×1440 | Quality (≈1707×960) | 60 fps, 16.6 ms GPU | development and acceptance tier |
| **Floor** | RTX 2060 Super (Turing, 8 GB) | 1920×1080 | Quality (1280×720) | 60 fps, 16.6 ms GPU | proves the "RTX 20+" claim; the engine already runs on this card |
| Middle | RTX 30 / 40 | between the two | Quality | 60 fps | no separate tuning; must fall between the two tiers |

**Every tier targets DLSS-RR Quality.** Modes below Quality are not a target. A tier that misses 60 fps is fixed by tuning stages (§2.3); changing the mode would be an explicit decision recorded here.

Feature support on the floor card is established: the engine starts on an RTX 2060 Super, and its extension set was chosen on that card. Performance is the open part: path-traced lighting at 60 fps on Turing has not been demonstrated yet. It is accepted only after measurement on the 2060 Super (§16). If it fails, the documented outcome is the per-stage overrun list, and the floor moves to RTX 30. That outcome is itself a result of the "what old hardware bottlenecks" goal.

### 2.2 Feature availability per architecture

Every required extension is available on Turing: the engine creates its device on an RTX 2060 Super, and `VK_EXT_ray_tracing_invocation_reorder` and `VK_EXT_opacity_micromap` are confirmed for that card in the Vulkan hardware database. The same code runs on every tier; tiers differ only in how much of a feature is hardware-accelerated.

| Feature | Turing (20) | Ampere (30) | Ada (40) | Blackwell (50) | Engine use | Status |
|---|---|---|---|---|---|---|
| `VK_EXT_mesh_shader`, `VK_EXT_shader_object`, BDA, sync2, dynamic rendering | yes | yes | yes | yes | everywhere | known |
| `VK_EXT_descriptor_heap` | yes | yes | yes | yes | **hard requirement** (`pickPhysicalDevice` rejects GPUs without it) | confirmed: the engine runs on an RTX 2060 Super (driver ≥ 610) |
| `VK_EXT_device_generated_commands` | yes | yes | yes | yes | raster buckets, material binning | in the required set the engine runs with on the 2060 Super |
| RT pipeline, ray query, AS | yes | yes | yes | yes | all rays | known |
| SER (`VK_EXT_ray_tracing_invocation_reorder`) | yes | yes | yes | yes | all passes that shade hits, from post-1.0 V1 (§11.1, §16.5); shelved in 1.0 | confirmed on the 2060 Super. Whether the driver actually reorders is reported by `rayTracingInvocationReorderReorderingHint` (`REORDER` or `NONE`); log it at startup. Per Khronos, reordering may be a no-op on hardware without support for it; NVIDIA introduced hardware SER with Ada |
| OMM (`VK_EXT_opacity_micromap`) | yes | yes | yes | yes | alpha-tested rays | confirmed on the 2060 Super. NVIDIA OMM SDK FAQ: supported on any RTX GPU, with a dedicated OMM unit from Ada on. Subdivision limits from `maxOpacity2StateSubdivisionLevel` / `maxOpacity4StateSubdivisionLevel` |
| Mega Geometry (`VK_NV_cluster_acceleration_structure`, `VK_NV_partitioned_acceleration_structure`) | yes | yes | yes | yes, plus faster cluster intersection | RT geometry | in the extension set chosen on the 2060 Super; cluster AS **required** since M2 (PBR plan C1); partitioned AS **required** since 2026-11-14 (PBR plan C4, built). The RTX 5070 reports both (`maxPartitionCount` 16,777,215) |
| `VK_EXT_memory_decompression` (GDeflate 1.0) | yes | yes | yes | yes | texture and geometry uploads (§6.4, §7.2) | in the required set on the 2060 Super; methods from `decompressionMethods` |
| `VK_NV_cooperative_vector` | yes (tensor cores) | yes | yes + FP8 | yes + FP8/FP4 | optional neural materials | RTXNS lists RTX 20 as minimum; not enabled in `vk_device.cpp`. NRC 0.16 does not use it: its kernels are CUDA binaries launched through `VK_NVX_binary_import` (PBR plan G3) |
| DLSS Ray Reconstruction | yes | yes | yes | yes | reconstruction | per-tier cost differs strongly (transformer model) |
| DLSS Frame Generation / Multi Frame Generation | no | no | FG | FG + MFG | **not used**: the 60 fps target means rendered frames | — |
| Linear swept spheres, hardware cluster intersection | no | no | no | yes | not used | Blackwell-only features stay out of the required path |

Consequences:

- **Same code, measured gains.** OMM is used on every tier, and SER will be once it returns (post-1.0 V1, §16.5). The floor budget (§2.3) does not count on a reordering gain until the 2060 Super's reorder hint and timings show one.
- **VRAM:** the floor card has 8 GB. Bistro must fit resident without LOD; BC textures and quantized streams keep it there (§6.3, §6.4).

### 2.3 Frame budget

Allocations, not measurements. They are the acceptance criteria per stage: each row gets a `GpuTimerScope` and a Tracy GPU zone. "Render" = DLSS render resolution. The floor render resolution is 1280×720 (Quality), 1.8× the pixels the first allocation assumed; the floor column is re-balanced from the M0 measurements.

| Stage (§) | Reference ms | Floor ms | Notes |
|---|---|---|---|
| Scene update + 2-phase cull + Hi-Z (§8) | 0.6 | 0.8 | compute |
| Visibility-buffer raster, both phases (§8) | 1.0 | 1.2 | mesh shaders via DGC |
| RT scene: instance ops, PTLAS update (cluster BLAS builds after LOD) (§10) | 1.0 | 1.5 | async compute where possible |
| Material resolve + shading + guides (§9) | 1.2 | 1.2 | compute, DGC execution sets |
| Direct lighting: RTXDI Streaming RIS or ReSTIR DI Temporal-only (Max History 4–8, Basic, Spatial OFF) (§11.2) | 1.5 | 1.8 | ray query; full Spatial+Temporal ReSTIR DI stays selectable as a toggle |
| Indirect (Bounce 1): raw uncorrelated 1-bounce PT (1 diffuse-or-specular ray/px + hit shading + cache query) (§11.3) | 3.0 | 3.5 | ray query in compute (SER RT pipeline post-1.0), OMM enabled, ReGIR or simple/`Power_RIS` NEE at X₂; half-res on the floor tier if needed |
| Radiance cache tail: NRC (default) or SHaRC (toggle) (§11.4) | 0.8 | 0.6 | NRC or SHaRC update / train / resolve; 0 ms when cache is `Off` |
| ReSTIR GI resampling (optional toggle, off in 1.0 target) (§11.5) | 0.0 (1.2 if on) | 0.0 (1.0 if on) | 0 ms in the target stack; active only when ReSTIR GI toggle is enabled (RTXDI analog) |
| DLSS-RR (§12) | 2.5 | 3.0 | NGX; 0 ms when denoiser is `Off`; measure early, it is the floor-tier risk |
| Post + UI (§12.3) | 0.4 | 0.4 | |
| **Headroom** | 4.6 | 2.6 | variance, driver, OS; target also beats NVIDIA RTXPT / RTXDI / RTXGI demo FPS at matched settings |
| **Total** | 16.6 | 16.6 | |

**Measured, M1** (2026-11-13; RTX 5070, driver 621.01, 1280×720, release, validation off, `ENGINE_BENCH_FRAMES=3000`; mean ms). The 2060 Super is not measured yet.

| Stage | Sponza, startup view | + 128×128 bunny field | Reference budget |
|---|---|---|---|
| Cull + DGC preprocess + Hi-Z, both phases | 0.086 | 0.182 | 0.6 |
| Vis-buffer raster, both phases | 0.031 | 0.032 | 1.0 |
| Resolve + guides | 0.147 | 0.139 | 1.2 |
| GPU total (with debug view, present, ImGui) | 0.308 | 0.395 | — |

**Measured, M8 I4** (2026-11-21; RTX 5070, 1280×720 window, 853×480 render, DLSS-RR Quality, release, no layers; mean ms, ReSTIR GI mode). Full per-scope tables for all three indirect modes: [i4_frame_profile.md](../spec/fixesM1-6/i4_frame_profile.md) §1. Later steps moved these: I6 cut ReSTIR DI by 37–40%, and I10 cut ray tracing by 7–13%. The current baseline is in [spec/fixesM1-6/README.md](../spec/fixesM1-6/README.md) §Baseline.

| Stage | Sponza gallery | Sponza courtyard (sunlit) | Reference budget |
|---|---|---|---|
| Cull + raster + Hi-Z, both phases | 0.123 | 0.125 | 0.6 + 1.0 |
| Classify + binned resolve | 0.115 | 0.117 | 1.2 |
| ReSTIR DI (initial + temporal + shade) | 0.351 | 0.704 | 2.0 |
| Indirect bounce | 0.553 | 0.736 | 3.0 |
| ReSTIR GI (temporal + shade) | 0.861 | 0.945 | 1.2 |
| DLSS-RR | 1.211 | 1.216 | 2.5 |
| GPU total | 3.327 | 3.954 | 16.6 |

CPU budget: ≤ 2 ms per frame on the render thread at any scene size, excluding asset loading. Asset loading runs on worker threads and must not stall a frame (§13.1).

---

## 3. Platform and API layer

| Area | Rule |
|---|---|
| API | Vulkan 1.4.356+ (`vk::raii`, C++23), SDK pinned in CMake |
| Pipelines | `VK_EXT_shader_object` for raster and compute, all state dynamic. **Exception (post-1.0 V1, §16.5):** the SER passes use `VkPipeline` + SBT, because shader objects have no ray-tracing stages and SER requires `traceRays`. In 1.0 every ray pass is compute |
| Binding | `VK_EXT_descriptor_heap`: one resource heap, one sampler heap, bound once per command buffer. No descriptor sets except ImGui's pool |
| Buffers | BDA everywhere; `VK_KHR_device_address_commands` where commands take addresses |
| Sync | sync2, timeline semaphores for cross-queue work, `MAX_FRAMES_IN_FLIGHT = 2` |
| Queues | graphics (main), async compute (RT scene build, GPU decompression of uploads), transfer (plain buffer copies). `vkCmdDecompressMemoryEXT` needs a graphics or compute pool, so decompression never goes to a transfer-only queue |
| Memory | VMA; ReBAR assumed: host-visible VRAM for per-frame CPU-written buffers ([resource_architecture.md §3](../spec/resource_architecture.md#3-memory-and-allocation)); `VK_EXT_memory_budget` reported in ImGui and Tracy |
| Uploads | cache hits: GDeflate pages written to ReBAR staging, decompressed on the GPU, copied into images and asset buffers. Host image copy only for first-load source images (§6.4, §7.2) |
| Presentation | `FIFO_LATEST_READY` when available, `VK_EXT_present_timing` for pacing once enabled |
| Validation | layers via vkconfig, not in code (`Device(window, false)` is intentional); validation-clean is a merge requirement |
| Limits and drivers | every size, count and capacity comes from queried device properties (`HardwareCapabilities`), never from hardcoded numbers. Other GPUs are checked with the Vulkan Hardware Capability Viewer and vulkan.gpuinfo.org. The target is the latest NVIDIA driver; driver-specific notes in this spec are rechecked on each update |
| Device creation | required extensions throw at startup when missing; cluster AS and position fetch are required since M2; partitioned AS is required since 2026-11-14 (PBR plan C4) |

The device extension inventory is [vulkan_extensions_reference.md](./vulkan_extensions_reference.md). NGX's and NRC's required extensions are merged at device creation (PBR plan H1, G3). To add: `VK_NV_cooperative_vector`, with neural materials (§15).

---

## 4. Shader architecture

Normative detail: PBR plan §4 and [shader_architecture_plan.md](../spec/shader_architecture_plan.md). End-state rules:

| # | Rule |
|---|---|
| S1 | **All shaders are Slang**, SPIR-V 1.6, compiled at build time with `-depfile`, `-warnings-as-errors all` once clean |
| S2 | **Library code is Slang modules** (`module x;`, `import a.b;`). Entry points live in `passes/`. Vendor HLSL (RTXDI, NRC) is `#include`d only from bridge and pass files |
| S3 | **Buffers through typed BDA pointers** (`GpuFrameRoot*`, `bdaPtr<T>`), read once into locals. No `StructuredBuffer` bindings for engine data |
| S4 | **Images and samplers through heap handles** (`DescriptorHandle<T>`); `kNoneIndex` never becomes a handle |
| S5 | **Scalar layout** (`-fvk-use-scalar-layout`), column-major; every mirrored struct has C++ `static_assert`s on size and offsets |
| S6 | **Non-uniform heap indices** go through one helper (`sampleSlot`, formerly `sampleOr`), because Slang drops `NonUniform` on heap access (first seen on 2026.14.1, still missing on 2026.20; re-probe on every Slang upgrade, PBR plan §4.4) |
| S7 | **Push data within the device limit** (`maxPushDataSize`, `VK_EXT_descriptor_heap` properties): root address + pass-local values by convention, no fixed byte cap. Render-target heap indices are in a per-frame `GpuTargets` block reached from the root |
| S8 | **Intrinsics used:** `RayQuery`, `HitObject` + `ReorderThread` (SER, post-1.0 V1), `CoopVec` (neural materials), wave ops (culling, binning), `SV_Barycentrics` (debug only) |
| S9 | **Specialization by Slang generics and link-time constants**, not by preprocessor variants, except vendor-mandated defines (`NRC_UPDATE`, `NRC_QUERY`) |
| S10 | **No implicit-LOD `Sample` outside fragment shaders.** Compute shading uses analytic gradients (`SampleGrad`, §9.2); ray hits use ray-cone LOD (§11.3) |
| S11 | **Limits come from the device** (§3): shader constants such as cluster sizes and list capacities are derived from queried properties at init and passed as link-time constants; each GPU list has an overflow counter in ImGui |

---

## 5. Frame overview

```
CPU (per frame, O(1) + deltas)
  camera, GpuFrameRoot, changed transforms/lights → host-visible ring (ReBAR)

GRAPHICS QUEUE                                          ASYNC COMPUTE QUEUE
 1 scene update   transforms → GpuNodeInstance, prev    │
 2 phase 1 cull   instances → clusters                  │ 6' world lights, changed-instance ops → PTLAS update
                  (frustum, cone, prev Hi-Z)            │    (after §16.3: RT LOD traversal + cluster BLAS builds first)
 3 phase 1 raster DGC: vis buffer + depth (reverse-Z)   │
 4 Hi-Z build                                           │
 5 phase 2 cull   candidates vs fresh Hi-Z              │
 6 phase 2 raster DGC: newly visible clusters           │
 7 Hi-Z build     (next frame's phase 1)                │
 8 classify       vis buffer → material tiles/bins      │
 9 resolve+shade  DGC compute exec set → G-surface + DLSS guides
   ─────── wait: TLAS ready (timeline semaphore) ───────┘
10 Direct         [Off | Streaming RIS | ReSTIR DI]                → direct
                  (target: Streaming RIS or Temporal-only Basic ReSTIR DI, Max History 4–8, Spatial OFF)
11 Indirect + Cache (§11.3–11.4), graphics queue (skipped if Indirect = Off):
   - Cache update: NRC update paths OR SHaRC update + resolve (skipped if Cache = Off)
   - Indirect bounce 1: raw uncorrelated 1-bounce PT (1 diffuse-or-specular ray/px, OMM, SER when Slang adds it,
     ReGIR or simple/Power_RIS NEE at X₂) + cache query (NRC or SHaRC)
   - NRC QueryAndTrain + custom resolve (when Cache = NRC)
12 ReSTIR GI      [Off (target) | On (RTXDI analog)]               → indirect
                  (when Off, stage 11 writes indirectRadiance directly with zero GI reservoir traffic)
13 combine        direct + indirect + emissive                     → noisy HDR
13' glass         thin-walled glass layers over the noisy HDR (§12.4; skipped without glass)
14 Denoiser       [DLSS-RR (target) | Off (No AA / Accumulation)]  → display-res HDR
15 post           auto exposure → tonemap → swapchain, ImGui
```

Stages 1–13' run at render resolution (indirect rays at half resolution on the floor tier: target, not built, considerations C-G2-1, M8 I11); 14–15 at display resolution. Stage 8 is built (M7 E1); before M7, stage 9 was one tiled dispatch. Stage 6' also runs the light preparation (`light_world`, `light_pdf_mips`, `rtxdi_presample`; §11.2) before the TLAS signal. No stage reads CPU-recorded per-object data.

After 1.0, animation adds a deformation pass ahead of stage 1 and per-frame CLAS instantiation on the async compute queue (§16.6). Streaming adds request, upload and patch tasks around the LOD traversal (§16.7).

---

## 6. Scene and resource model

Base: [resource_architecture.md](../spec/resource_architecture.md) (model slices, refcounted shared models and materials, `GpuFrameRoot`, `GpuNodeInstance`, `GpuDrawRecord`). It stays valid; the end state extends it.

### 6.1 Persistent GPU scene

| Table | Owner | Update |
|---|---|---|
| Model slices (vertices, cluster data, indices) | `AssetBuffers` | on load / unload |
| `GpuModel`, `GpuPrimitive`, `GpuNode`, materials, `GpuMaterialExtension` | same | on load / unload |
| `GpuNodeInstance[entity × node]` | GPU scene-update pass | each frame from a delta list of changed entity transforms; `prevWorldMatrix` kept on the GPU |
| Lights: punctual rows + emissive-triangle light list | `light_world` on async compute (M7 E2) | every frame, from the model-space rows and the entity matrices; one row per emitting triangle |
| Per-cluster boxes (`GpuMeshletBounds`, parallel to `GpuMeshletDesc`), spheres + cones (`GpuMeshletDesc`), cluster → primitive row (`MeshletPrimitives`) (+ DAG error after §16.3) | asset buffers, model-sliced | on load |
| Cluster groups (`GpuMeshletGroup`: union box, first cluster, count ≤ 32), per primitive (`GpuPrimitive::firstGroup/groupCount`) | asset buffers, model-sliced | on load |
| Draw records `GpuDrawRecord` per (node instance, group): cull-internal work items, not read by mesh shaders | `MeshletCulling` | on layout change |
| CLAS | §10.2 | on load, persistent |
| Cluster BLAS, PTLAS, `GpuRayInstance` | §10.3 | GPU; the PTLAS is updated in place per frame |
| OMM arrays | §10.4 | on load |

Rules carried over: model-relative values inside slices, absolute rows in CPU-rewritten tables, addresses re-read after every flush and never cached across an unload.

### 6.2 Per-frame data

`GpuFrameRoot` (host-visible, one per frame slot) grows to hold `tlas`, `rayInstances`, `targets` (heap indices of all render targets, S7), `frameIndex`, `exposure`, jitter, the per-SDK constants blocks (RTXDI, NRC) and the GPU list addresses (visible clusters, material bins, light lists). Every field is asserted on both sides.

### 6.3 Data layout targets

Folded from the former data-layout spec and meshoptimizer backlog (neither is in the repository); this table is the only statement of these targets:

| Stream | End state |
|---|---|
| Position | float3: the CLAS build input; quantized copies only where a consumer accepts them |
| Normal / tangent | octahedral snorm16 normal + packed tangent; generated with `meshopt_generateTangents` where missing (PBR plan A4) |
| UV | half2 (`meshopt_quantizeHalf`) unless a texture needs float precision |
| Vertex order | meshlet order (`meshopt_optimizeVertexFetchRemap`) |
| Meshlet desc | sphere + normal cone, `cone_weight = 0` as in `clusterlod.h` (clusters do not trade size for tighter cones); the cone test still consumes it |
| Meshlet bounds | object-space box + shortest/longest edge (32 B, unquantized); quantize only if bandwidth shows in the timings |

### 6.4 Textures

- KTX2 BC7 (color, linear data) and BC5 (normal maps), produced **automatically** by the engine in the background and cached; uploaded through GPU decompression (§7.2). The offline `tools/gltf_ktx2` shares the same encoder. BC6H (HDR) is out of scope: libktx has no BC6H target.
- In 1.0 all mips are resident. Texture mip streaming is post-1.0 (§16.7 ST4).
- Neural texture compression (RTXNTC, cooperative vectors) is optional (§15), not part of the baseline.

---

## 7. Geometry pipeline (load and offline)

All geometry processing uses **meshoptimizer** (rule: no homegrown geometry code where meshopt has the function). The exception is OMM baking, which uses the **NVIDIA OMM SDK**.

### 7.1 Per primitive at load

1. Clean-up: `meshopt_filterIndexBuffer`, welding, missing normals/tangents (PBR plan A4).
2. **After the final goal (§16.3): cluster LOD DAG** following meshopt `demo/clusterlod.h` (`vk_lod_clusters` ships a lightly modified copy, used from `Scene::buildGeometryLod` in `scene_cluster_lod.cpp`): `meshopt_buildMeshletsSpatial` (clusters suited to ray tracing) → group with `meshopt_partitionClusters` → `meshopt_simplify*` with locked group borders → re-cluster, until one root group. Each cluster stores bounds, normal cone, its own error and its parent group's error (monotonic), as in `vk_lod_clusters`.
3. Until then clusters are today's meshlets, built as **level 0 of the cluster LOD config**, so LOD later adds levels above them without changing them ([culling_comparison.md](../reference/culling_comparison.md) A3). Since M8 I10 (MG-P3, 2026-11-22) the builder is `meshopt_buildMeshletsSpatial`, the builder of `vk_lod_clusters`' ray-tracing preset (`clodDefaultConfigRT`), with min triangles = max / 3 (42) and fill weight 0.75. It measured `RaySum` −10.9% / −6.6% / −12.8% (gallery / courtyard / Bistro) against the earlier Flex builder. The sample's own `min = max/4, fill 0.5` inflated Bistro's clusters by 53% and was rejected. Then, per primitive, `meshopt_partitionClusters` (spatial, target 24 so the +1/3 worst case fits 32) into groups with a union box; clusters are reordered so each group is contiguous. Groups never span primitives (materials and state are per primitive), unlike `vk_lod_clusters`, which partitions a whole mesh. Cluster size comes from the device: mesh-shader output limits and preferred sizes (`maxMeshOutputVertices`, `maxMeshOutputPrimitives`, `maxPreferredMeshWorkGroupInvocations`) and the CLAS limits (`VkPhysicalDeviceClusterAccelerationStructurePropertiesNV`). 64 vertices / 126 triangles is today's choice on NVIDIA, not a fixed constant; `vk_lod_clusters`' presets (default 128T / 128V) are the candidates to measure. The same clusters feed raster and CLAS builds.
4. `meshopt_optimizeMeshletLevel` level 1, as in `clusterlod.h`. Levels ≥ 1 may rotate triangle corners, which changes OMM data, so the OMM bake (step 5) reads the final, optimized cluster triangles and never an earlier order.
5. OMM bake for alpha-tested primitives: NVIDIA OMM SDK CPU baker, one bake per primitive over its final cluster triangles in cluster order, 4-state bake, forced 2-state at trace time (PBR plan D1).

### 7.2 Disk cache

Processing results (clusters, OMM, quantized streams; the DAG after §16.3) are cached per asset file + processing version, stored as GDeflate pages and decompressed on the GPU on load.

### 7.3 Static geometry in 1.0

1.0 has no animation: no skinning, no morph targets, no keyframe animation. Rigid entity transforms change only through the debug UI, through `GpuNodeInstance` and the per-frame PTLAS update. Moving entities are supported: the motion vectors, scene bounds and PTLAS partition reassignment are built (MG-B4). That makes rigid node animation (§16.6 AN1) the first post-1.0 step. Skinning and morph targets return post-1.0 on the GPU (AN2), with Mega Geometry cluster templates for the rays (AN3).

**Removal task (M0, done 2026-11-08):** the earlier animation code was deleted, since no animation was planned then. Its record is [animation_removal.md](../spec/m0/animation_removal.md); AN2 reintroduces the streams in the GPU-driven form. The deleted code was: the `Joints`, `Weights`, `MorphPos`, `MorphNrm`, `MorphTan`, `MorphTargets` and `MorphWeights` asset buffers and their `GpuModel` slices, `GpuPrimitive` morph ranges, `GpuMorphTarget`, `applyMorphPosition` in the mesh shader, morph and skin parsing in the loader (glTF files load in their base pose), and the "morphed primitives skip culling" rule. About 200 lines across `assets_loader.cpp`, `geometry_store.*`, `types.hpp`, `vk_resource_manager.*`, `vk_meshlet_culling.cpp`, `vk_engine.cpp`, `types.slang` and `geometry/mesh.slang`.

---

## 8. Primary visibility (raster)

### 8.1 Culling and LOD

Base culling is shaped like the **leaf level of `vk_lod_clusters`** with LOD switched off: same data shapes, same test functions, same two-pass bookkeeping. LOD then adds the node tree, the metric and the persistent traversal above it instead of replacing it. Detail and measurements: [culling_comparison.md](../reference/culling_comparison.md) (§4, steps A1–A10); history: [meshlet_culling_options.md](./meshlet_culling_options.md), `MeshletCulling`.

As built (A1–A5, A7, C1–C3):

| Step | Rule |
|---|---|
| Test library | [culling.slang](../shaders/core/culling.slang), a modified port of `culling.glsl` (Apache-2.0 header kept): `intersectFrustum` on 8 box corners with clip bits into `ClipBounds`, `isHiZOccluded` (one max-sampler fetch at the rect center, 1-texel mip with the niagara refinement, 2-ulp nudge), `isSubPixel`, `isFrontFacingHW`, `testTrianglePixel`. Engine extras: explicit near-plane rule (`kClipNear`); reverse Z only (the forward-Z branch and `CULLING_REVERSED_Z` were removed, §20.2 AU5) |
| Bounds | object-space boxes at every level: node mesh box, group box, cluster box. The cluster sphere stays only for the cone test |
| Hierarchy | node instance → draw record = cluster group (≤ 32) → cluster → triangle. One 32-lane workgroup per record tests the group box once (frustum, sub-pixel, occlusion); the group verdict holds for all its clusters, the cone stays per cluster |
| Tests | instance: frustum + sub-pixel + Hi-Z; group: same; cluster: frustum + cone + sub-pixel + Hi-Z; triangle (mesh shader, `SV_CullPrimitive`): homogeneous backface (`determinant(a.xyw, b.xyw, c.xyw)` before the `w ≤ 0` guard) + small-primitive test |
| Sub-pixel | lossless: culls only boxes whose conservative rect holds no pixel center, widened by the subpixel snap (`subPixelPrecisionBits`); independent of occlusion; final, never a phase 2 candidate |
| Matrices | frustum and sub-pixel with the current (or frozen) `cullViewProj`; early Hi-Z with `prevViewProj` + `prevWorldMatrix`, late with current. Stricter than the sample's static-instance assumption, kept |
| Two-phase handover | instances: candidate list (the sample's `rejectInstances`). Clusters: one reject bitmask `uint` per early draw record (`rejectMasks`), written every frame (no clear); the late `cullRejectedMeshlets` re-tests the group box, then each set bit, against the fresh pyramid (the sample's `traversal_reject_clusters`). No union-box filter: the early frustum already uses the current matrix |
| Freeze culling | matrices and pyramid frozen together; occlusion keeps working against the frozen view (no `CameraFrozen` fallback) |
| Lists | visible entries `GpuVisibleMeshlet {nodeInstanceIndex, meshletRow}` (8 B, the sample's `ClusterInfo`) per state group, wave-ballot appends |

Remaining before 1.0, each in the milestone that owns its dependency (§16.2):

| Step | Rule | Milestone |
|---|---|---|
| A10 capacities | every list append through a capacity check with an overflow counter in `GpuCullCounters` and the culling panel (S11); exact sizing stays until LOD | M0, built |
| A9 Hi-Z + reverse Z | port `nvhiz-update` (3 levels per dispatch, groupshared reductions in place of `subgroupShuffle`); reverse Z in the culling library, min reduction and the nudge sign | M1, built |
| A6 bins by pipeline | two-sided by winding flip in the mesh shader (`isFrontFacingHW`) with a back-face bit for shading; per-primitive `AlphaTested` bin (no `*_MIXED` group bits since cluster groups never span primitives, [m6/README.md](../spec/m6/README.md)); bins opaque / alpha-tested (later SW raster) | opaque bin built in M1; alpha-tested bin built in M6 D3 (2026-11-20) |
| A8 jitter | §12.2 | M3 |
| Verification | image and timings for A1–A7 on both GPUs (static, fast turn, fly out while frozen, double-sided foliage); cone-cull loss from `cone_weight = 0` | RTX 5070 done 2026-11-13: timings in §2.3; image identical with every test on and off, static and right after a turn, on 16 Sponza poses (`ENGINE_CULL_CHECK=1`). Open: the 2060 Super, the frozen fly-out, double-sided foliage (M6), cone-cull loss |

After the final goal (§16.3):

| Step | Rule |
|---|---|
| LOD cut | per cluster group: render when `ownError ≤ τ` and `parentError > τ`, error projected against **display** resolution (DLSS), not render resolution |
| Traversal | GPU DAG traversal with a work queue (`vk_lod_clusters` `traversal_init` / `traversal_run`); the same code produces the RT cut (§10.3) |

### 8.2 Raster

| Item | Rule |
|---|---|
| Depth | **reverse-Z** `D32_SFLOAT`, finite far plane, clear 0, `GREATER` (built in M1, §8.1 A9) |
| Hi-Z | `nvhiz-update` port, 3 levels per dispatch, min reduction (built in M1) |
| Target | visibility buffer (§8.3) + depth, render resolution, 1 sample, jittered projection |
| Buckets | opaque and alpha-tested (own fragment shader with `discard`); two-sided by winding flip, no cull-mode bucket (§8.1 A6). One DGC execute per bucket per phase. Both bins are built (alpha-tested since M6 D3), with one rule for which primitives are alpha-tested in raster and rays (`isAlphaTested`, §10.5) |
| DGC | the raster path ([dgc_comparison.md §7–9](../reference/dgc_comparison.md)): GPU-written sequences + count; an execution set once buckets need different shaders. The sample's one indirect mesh draw per list is intentionally not followed |
| Mesh shader | visible entry → node instance → model → `MeshletPrimitives[meshletRow]` → primitive (material `DoubleSided` flag; no draw record read) → vertex transform → triangle culling → ids only; no material fetch except in the alpha-tested bucket |

### 8.3 Visibility buffer

Detail: [visibility_buffer.md](../spec/features/done/visibility_buffer.md).

| Option | Bits | Choice |
|---|---|---|
| `R32_UINT`: `visibleRow << kVisTriangleBits \| triangle`, `kVisTriangleBits = ceil(log2(maxTrianglesPerCluster))` (7 today, 25 row bits) | 32 | **default**: half the bandwidth; fits next to 32-bit depth in the 64-bit `imageAtomicMax` that software raster (§15, L1) uses, so L1 changes the image format, not the encoding. No flags: facing is recomputed in the resolve; empty pixels are decided by depth |
| `R32G32_UINT`: x = visible-cluster row, y = triangle + flags | 64 | alternative, only if M1 runtime measurements favor it (Q3) |

Each visible-cluster row is the cull pass's `GpuVisibleMeshlet {nodeInstanceIndex, meshletRow}`; the primitive comes from `MeshletPrimitives[meshletRow]`, so the resolve needs no traversal. `meshletRow` is the cluster's row in its model's slice: stable across draw records and phases, and the row space LOD levels extend.

---

## 9. Material resolve and shading

### 9.1 Classification

One compute pass reads the visibility buffer in 8×8 tiles and appends each tile to the bin of every material **shader** it contains (rows sharing a shader share a bin). It also writes a per-pixel bin id (R8 or R16 image). Output: per-bin tile lists and counts, and the bin-id image.

**As built (M7 E1, 2026-11-20, [e1_material_binning.md](../spec/m7/e1_material_binning.md)):**
- `classify` → `binArgs` → one DGC execute over an execution set of three resolve shaders: `Background`, `Standard` (spec-gloss is a row flag inside it) and `Unlit`.
- Every guide is bit-identical to the single dispatch.
- I4 cost: 0.018 ms classify + 0.097 ms resolve on the Sponza gallery, about 0.045 ms over the single dispatch (F11, not ranked).
- GPU-AV cannot instrument indirect-bindable shader objects (considerations C-E1-3). GPU-AV runs use the temporary per-bin switch `ENGINE_GPUAV_RESOLVE_PER_BIN=1`, which is off by default and removed with C-E1-3.

### 9.2 Resolve and shade

Dispatched with **DGC compute**: one sequence per non-empty bin, an `EXECUTION_SET` of compute shader objects, `DISPATCH` tokens, sequence count from the GPU. This is the engine's main DGC use ([dgc_comparison.md §9](../reference/dgc_comparison.md#9-dgc-for-the-planned-gpu-work)).

**Overlap guard:** a tile with several materials is in several bins, so several bin shaders run over the same pixels. Each bin shader is `[numthreads(8, 8, 1)]`, launched with `groupCount = (tileCount[bin], 1, 1)`, and returns early when the pixel's bin id (one load from the bin-id image) is not its own. Every pixel is written by exactly one bin, so the §9.3 images see no races or overwrites.

Per pixel:
1. Fetch the triangle's three vertices through BDA; transform with current and previous world matrices.
2. **Analytic barycentrics and screen derivatives**, UV gradients for `SampleGrad`; no quad overdraw.
3. Motion vector from unjittered current and previous clip positions.
4. `sampleMaterial` (PBR plan B1): one function for vis-buffer resolve and ray hits.
5. Write the shading surface and DLSS-RR guides.

### 9.3 Outputs (render resolution)

| Image | Format | Consumers |
|---|---|---|
| depth | D32F reverse-Z (hardware) | Hi-Z, resolve, debug view, DLSS (`InUseHWDepth`) |
| `normalRoughness` | RGBA16F: world N, perceptual roughness | DLSS, debug view |
| `diffuseAlbedo` | RGBA8; a = ao | RTXDI (`RAB_GetGBufferSurface`), NRC resolve, combine, motion check, DLSS diffuse albedo |
| `specAlbedo` | RGBA8, env-BRDF approximation | DLSS specular albedo |
| `emissive` | R11G11B10F | combine, NRC update |
| `motion` | RGBA16F, render-res pixels, no jitter; z = view depth change | DLSS, ReSTIR temporal / GI shade |
| `surface` | RGBA32_UINT: `x` = `asuint(viewDepth)` (`0` = empty), `y` = `packOct16(n)`, `z` = `packOct16(geoNormal)`, `w` = `f0.rgb` unorm8×3 \| `roughness`/`isLit` (M8 O4) | RTXDI surfaces and ray origins (position rebuilt from `viewDepth` + jittered ray basis), this frame and the previous; debug view |
| `specHitDistance` | R16F | optional DLSS input (see below) |

`specHitDistance` is in the `OPTIONAL - only for research purposes` block of `NVSDK_NGX_VK_DLSSD_Eval_Params` in the vendored NGX 1.5 headers (`nvsdk_ngx_helpers_dlssd_vk.h`); the required inputs are color, depth, motion, diffuse/specular albedo, normals and roughness. The baseline does not pass it. If it is enabled for a measured quality gain, it must be full render resolution on both tiers: write it after ReSTIR GI (§11.5) as `length(X₂ − X₁)` of each pixel's final reservoir sample, never from the half-resolution indirect launch, whose missing checkerboard pixels would read as 0.

Two sets, ping-ponged by frame parity, for temporal resampling. The resolve writes them (built in M1, completed in M3 E1).

### 9.4 Material model

glTF 2.0 metallic-roughness, `KHR_lights_punctual`, `KHR_texture_transform`, `KHR_materials_emissive_strength`, and `KHR_materials_pbrSpecularGlossiness` (a row flag inside the `Standard` bin: its diffuse and specular-glossiness textures use the base-color and metal-rough slots, and `sampleMaterial` returns the extension's `cDiff` and `F0` directly; Bistro's materials). Further KHR PBR extensions become separate bins when added. One BRDF: glTF Appendix B GGX with VNDF sampling (PBR plan B2). Neural materials (cooperative vectors) are optional (§15).

---

## 10. Ray-tracing scene (cluster LOD + RTX Mega Geometry)

The RT scene is built around the cluster representation that raster uses, following `vk_lod_clusters` ([README](../reference/vk_lod_clusters/README.md), `renderer_raytrace_clusters_lod.cpp`). There are no per-primitive triangle BLASes.

### 10.1 One geometry representation

- **Clusters are the only geometry unit.** The engine's meshlets (64 vertices / 126 triangles) are the clusters; the DAG of §7.1 adds coarser levels later. Raster draws them, CLAS are built from them, and hits decode to them.
- **Same ids everywhere:** a vis-buffer pixel and a ray hit both resolve to `(nodeInstance, cluster, triangle)`, so one `decodeHit` and one `sampleMaterial` serve both (PBR plan C3, extended with the cluster id). The cluster id is the model-slice `meshletRow` of §8.3.
- **Post-1.0 (§16.5, V2):** a triangle BLAS per static primitive as a toggle beside the cluster BLAS; hits still decode to `(nodeInstance, cluster, triangle)`.
- **Post-1.0 (§16.6, AN3):** deforming geometry keeps the same clusters. Each cluster gets a template at load, and a CLAS is instantiated from the deformed positions every frame. Static geometry keeps its load-time CLAS, so ids and decode stay the same.

### 10.2 Load time (per model, once)

| Step | Rule |
|---|---|
| CLAS per cluster | every cluster (one level until §16.3, then every LOD level). Through 1.0 everything is resident, the sample's `scene_preloaded.cpp` mode; streaming is post-1.0 (§16.7). CLAS are object-space and shared by all instances of the model. Opaque and OMM clusters are built in two calls, so only alpha-tested clusters carry `ALLOW_CLUSTER_OPACITY_MICROMAPS` (MG-P1: CLAS −8% Sponza, −9% Bistro) |
| Build input | cluster-local position arrays gathered into scratch with 8-bit cluster-local indices, freed after the build. `positionTruncateBitCount` zeroes low mantissa bits to shrink CLAS (the sample's "CLAS Mantissa drop bits"). It stays 0: 7 bits saved 18–20% of CLAS memory but slowed tracing by 4–6% on the 5070 (MG-P2, reverted). It returns as a memory lever for streaming and the floor tier (§16.7) |
| Truncation consistency | fetched positions are the truncated CLAS positions, not the raster ones. At secondary hits, build the geometric normal and the next ray origin from the fetched positions only. Ray origins on the primary surface (full-precision raster positions) are offset by at least the truncation error, `≈ 2^(t − 23) · max(abs(p))` for `t` dropped bits, on top of the `geoNormal` offset (§10.4) |
| Hit positions | `VK_KHR_ray_tracing_position_fetch` (`HitTriangleVertexPosition`, as in the sample's cluster hit shaders). Full-precision positions stay resident anyway for raster and the vis-buffer resolve, so this saves no memory here. Its value is at incoherent secondary hits: positions come from the AS, without the dependent BDA chain instance → model → cluster → vertex index → position |
| CLAS memory | build into scratch, then move to their final place using the real build sizes: host estimates are 3–5× too large ([clas_allocation.md](../reference/vk_lod_clusters/docs/clas_allocation.md)) |
| OMM | attached per cluster (§10.5) |

### 10.3 Per frame (GPU, async compute)

| Step | Sample file | Rule |
|---|---|---|
| RT LOD traversal (after §16.3) | `traversal_init.comp.glsl`, `traversal_run.comp.glsl` | same DAG and traversal code as raster (§8.1), separate cut: error measured by distance and solid angle, **no culling**. The sample scales the error by `culledErrorScale` (default 2.0, `traversal_init.comp.glsl`) for instances that fail its frustum **or** previous-frame Hi-Z test. The engine applies the scale **only outside the current frame's view frustum** (with a margin), never for Hi-Z occlusion (§10.4) |
| BLAS reuse classification (after §16.3) | `geometry_blas_sharing.comp.glsl`, `traversal_init_blas_reuse.comp.glsl` | per instance: prebuilt low-detail BLAS, shared (another instance's), cached (discrete level, kept across frames), merged (highest resident detail) or own ([blas_sharing.md](../reference/vk_lod_clusters/docs/blas_sharing.md), [blas_caching.md](../reference/vk_lod_clusters/docs/blas_caching.md), [blas_merging.md](../reference/vk_lod_clusters/docs/blas_merging.md)) |
| Per-frame cluster BLAS builds (after §16.3) | `blas_setup_insertion.comp.glsl`, `blas_clusters_insert.comp.glsl` | indirect; with merging + caching at most 2–3 builds per unique geometry per frame. New BLAS addresses reach the PTLAS as `UPDATE_INSTANCE` ops, without rewriting transforms |
| Instance write | `instances_assign_blas.comp.glsl` | PTLAS instance ops + `GpuRayInstance` written on the GPU from `GpuNodeInstance`; no CPU loop |
| TLAS | `vk_partitioned_tlas` (`partitioned_acceleration_structures.hpp`) | **PTLAS, required** (2026-11-14). One partitioned TLAS, updated in place every frame from GPU-written ops: `WRITE_INSTANCE` for every row after a layout change, otherwise only for instances whose world matrix changed, so a still frame writes no ops. Grid partitions over the instances' world positions: empty cells are dropped, cells are numbered in Z-order, and the grid is refined while a cell holds more than 1,024 instances (MG-P5). Changed instances are rewritten in their own partition, and a moved entity is reassigned to its new cell (MG-B4). The global partition measured slower on the 5070 and is not used; explicit instance boxes were measured and reverted (MG-P5). The op list and its count are device addresses, so the build is indirect with no readback. PTLAS has no OMM 2-state instance flag, so every ray sets the 2-state ray flag (§10.5). Frame N's update waits for frame N−1's last ray pass. Rays trace the same raw address (`GpuFrameRoot.tlas`). [ray_tracing_mega_geometry.md](../spec/features/planned/ray_tracing_mega_geometry.md) §3.6. Built in PBR C4: a still frame updates in 0.015–0.019 ms, a moving one in 0.12–0.18 ms, against 0.10–0.15 ms for the earlier full build (5070) |
| Hit ids | — | `InstanceIndex()` → `GpuRayInstance`; `ClusterID` (needs `allowClusterAccelerationStructure` on the RT pipeline) → cluster; `PrimitiveIndex()` → triangle in the cluster |

Graphics waits on a timeline semaphore before the first ray pass (§5 stage 10). `rt_instances` and the PTLAS update are recorded before the light preparation, so they overlap it (MG-P7).

**Through the final goal:** one level (today's meshlets). Each mesh primitive gets one cluster BLAS from its CLAS, built once at load and referenced by every instance; only changed instances are written into the PTLAS per frame. LOD (§16.3) then changes which CLAS enter a BLAS, not the data model.

### 10.4 Hybrid consistency

Applies once LOD exists (§16.3); until then raster and RT trace the same clusters and differ only by position truncation (§10.2). Primary surfaces come from the raster cut, secondary rays from the RT cut. Where the RT geometry is coarser than the raster surface, ray origins sit off the traced surface and produce self-shadowing or light leaks.

- **Rule:** inside the view frustum the RT error threshold is never coarser than the raster one. Outside, RT detail falls off freely.
- **No occlusion penalty in the RT cut.** The RT traversal runs on async compute in parallel with raster, so it cannot see this frame's phase 2 results. An instance occluded in last frame's Hi-Z can be disoccluded in phase 2 and rasterized at full detail while a coarser RT cut is traced. Only being outside the frustum lowers RT detail.
- Ray origins are offset along `geoNormal` (§9.3).
- Debug view: primary-ray self-hit distance, so a mismatch is visible.
- `vk_lod_clusters` "Discrete LoD" ([raster_discrete_lod.md](../reference/vk_lod_clusters/docs/raster_discrete_lod.md)) is the sample's tool for making raster and RT converge on the same geometry in a hybrid renderer.

### 10.5 Alpha-tested geometry (OMM)

Alpha-tested instances carry baked OMMs and no opacity flag, and every ray sets `RAY_FLAG_FORCE_OPACITY_MICROMAP_2_STATE`, since PTLAS has no 2-state instance flag (§10.3). Every other instance is `FORCE_OPAQUE`, so traversal never yields candidates and no any-hit shader exists (PBR plan D). The sample uses any-hit for alpha masks; the engine does not.

Cluster AS supports OMM per cluster: `VkClusterAccelerationStructureBuildTriangleClusterInfoNV` has `opacityMicromapArray`, `opacityMicromapIndexBuffer` and `opacityMicromapIndexType`, and the build needs `VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_CLUSTER_OPACITY_MICROMAPS_BIT_NV` (VUID-…-opacityMicromapArray-10881). Consequences for §7.1:

- Clusters reorder triangles, so each cluster gets an OMM index buffer that maps its local triangle to the bake entry (`opacityMicromapIndexBuffer`; 0 would mean "triangle index in the geometry", which no longer matches the bake order). It is the cluster's subrange of the SDK's per-triangle output, and all clusters of a primitive share its micromap array.
- After §16.3, simplified LOD levels have new triangles: bake OMMs per LOD level from the simplified triangles and their UVs.

**Built (M6, 2026-11-20):**
- **D1:** OMM SDK CPU bake per alpha primitive.
- **D2:** one micromap per primitive, built before its CLAS in the same submit. Each cluster points at its index subrange, and alpha instances drop `FORCE_OPAQUE`.
- **D3:** an alpha-tested raster bin with the same threshold.
- **D4:** one `Proceed()` per query, with a Debug candidate counter that reads 0 in every runner, and an `alpha-edge` class in `ENGINE_RT_CHECK` for raster-vs-OMM differences next to a cut.
- One rule picks the alpha primitives for both raster and rays: `isAlphaTested` in `material_store.hpp`.
- Files: [spec/m6/](../spec/m6/README.md).

---

## 11. Lighting

### 11.1 Ray model: built around SER

**Shelved 2026-11-17 (considerations C-G1-3):** Slang's EXT `HitObject` support is incomplete: [#12554](https://github.com/shader-slang/slang/issues/12554) (`GetRayTCurrent` missing on SPIR-V), [#13763](https://github.com/shader-slang/slang/pull/13763) (EXT SER wrappers need an explicit capability), [#13772](https://github.com/shader-slang/slang/issues/13772) (wrong hit-query capabilities), [#13774](https://github.com/shader-slang/slang/issues/13774) (no fused trace-reorder-execute on `HitObject`), and the engine's own SER workarounds (C-G1-1, C-G1-2, C-G2-2) pile up. Until Slang's EXT path works, every pass shades its hits from ray queries in compute (`traceClosest`, the same `RayHit` and `decodeHit`); the SER code stays in the tree, unbuilt. SER returns after 1.0 as a toggle beside the ray-query passes and is expected to become the default (§16.5 V1); the design below is that variant.

Every pass that **shades** ray hits runs in an RT pipeline designed for Shader Execution Reordering. Rays that only test visibility use ray queries.

| Pass | Rays | Mechanism |
|---|---|---|
| Indirect bounce (§11.3) | 1 per pixel, shaded at the hit | RT pipeline + SER |
| Cache update / training paths (§11.4) | multi-bounce, shaded at each vertex | RT pipeline + SER; the most incoherent rays in the frame, so the largest SER gain |
| ReSTIR DI final visibility, shadow rays at path vertices | visibility only | ray query (in compute, or inline in raygen) |
| ReSTIR GI reconnection validation | visibility only | ray query |

Shaded-ray structure, identical in every SER pass:

1. `HitObject::TraceRay` (trace without shading).
2. `ReorderThread(hitObject, hint, bits)`. The hint is the hit's **material shader bin**, the same bin ids as vis-buffer classification (§9.1); misses get their own bin.
3. Invoke a minimal closest-hit that returns `{instance, cluster, triangle, barycentrics, hitT}` (the sample's `render_pathtrace_clusters.rchit.glsl` does this). If Slang exposes cluster id and attributes on `HitObject` directly, read them and skip the invoke.
4. Shade in raygen: `decodeHit` → `sampleMaterial` → BRDF, the same functions as the vis-buffer resolve.

SBT per pass: one raygen, one miss, one hit group (closest-hit only). No per-material hit groups, no any-hit. Texture LOD at hits comes from ray cones.

**Post-1.0 (§16.5, V1):** the SER pipeline returns as a toggle beside the 1.0 ray-query passes, which stay. SER is expected to be faster and is the intended default once the measurements confirm it.

The code is identical on every tier. Whether `ReorderThread` reorders is the driver's choice, reported by `rayTracingInvocationReorderReorderingHint` (§2.2); the floor budget (§2.3) assumes no reordering gain until the 2060 Super's timings show one. The RT pipeline uses the descriptor-heap pipeline flag like the shader objects do, plus `allowClusterAccelerationStructure` for cluster hits. DGC does not drive ray dispatch: driver 621.01 reports no RT stages for DGC, so dispatch is `vkCmdTraceRaysIndirect2KHR`.

### 11.2 Direct light: RTXDI (Streaming RIS / ReSTIR DI)

PBR plan F1–F3, extended (M8 target updated 2026-11-25):

- Light sources: punctual lights + **emissive triangles**, one light per triangle of each emitting primitive. `light_world` writes them every frame (M7 E2, [e2_emissive_triangles.md](../spec/m7/e2_emissive_triangles.md)), and a BRDF ray that hits an emitter names its light, so emission is counted once along every path. An environment map is used when present (none yet, C-F3-5).
- Local light sampling: presampled, as the RTXDI sample (`light_world` → `light_pdf_mips` → `rtxdi_presample` on async compute before the TLAS signal). `Power_RIS`, `ReGIR_RIS` and `Uniform` are all runtime-selectable (`ENGINE_DI_SAMPLING`, [spec/m7/f1_light_presampling.md](../spec/m7/f1_light_presampling.md)).
- **1.0 target direct lighting (decided 2026-11-25, [spec/m8](../spec/m8/README.md)):** RTXDI configured as either:
  1. **Streaming RIS** (Initial Sampling only, 1 final visibility ray, zero temporal/spatial reservoir traffic — matching `reference/RTXPT-main`'s `IntroSample/RTXDIStreamingRISLightingPass.hlsl`), or
  2. **ReSTIR DI Temporal-only:** Temporal ON with **Max History = 4–8**, **`Basic` bias correction**, and **Spatial OFF** (skipping spatial neighbor loads and rays so DLSS-RR receives low-correlation noise without spatial blotches).
  The choice of default between Streaming RIS and Temporal-only ReSTIR DI is decided by M8.5 FPS and stability measurements on Sponza and Bistro; both remain runtime toggles.
- **Toggles and presets:** Direct lighting can be disabled completely (`Off`, 0 ms GPU cost), switched to `Streaming RIS`, or run full `ReSTIR DI` (`Temporal`, `Spatial`, `Temporal + Spatial`, `Reference/None`). NVIDIA's sample presets (`Fast`, `Medium`, `Unbiased`, `Ultra`, `Reference`) and the M8 `fixesM1-6` `Tuned` / `Converged` sets stay callable for RTXDI-analog comparisons and convergence checks.
- Output: `directRadiance` (RGBA16F; skipped in `combine` when Direct = `Off`).

### 11.3 Indirect (Bounce 1): raw uncorrelated 1-bounce PT

Target updated 2026-11-25 ([spec/m8](../spec/m8/README.md)):

- **1.0 target indirect bounce 1:** **raw uncorrelated 1-bounce path tracing** — 1 probabilistic diffuse-or-specular ray per pixel from the primary surface (`sampleBrdf`, VNDF), **OMM 2-state enabled**, **SER enabled once Slang fixes EXT `HitObject`** (C-G1-3, §16.5 V1; compute `RayQuery` in the meantime), with **`ReGIR` or simple / `Power_RIS` NEE** for direct lights at the bounce hit X₂, plus one visibility ray and a multi-bounce **radiance cache query** (NRC or SHaRC, §11.4).
- When ReSTIR GI is `Off` (the 1.0 target), the indirect pass writes `indirectRadiance` directly (or via `nrc_resolve` when Cache = NRC) with **zero GI reservoir packing/unpacking or reuse overhead**, giving DLSS-RR temporally uncorrelated bounce-1 samples.
- **Toggles:** Indirect Bounce 1 can be set to `Off` (skips the indirect pass, cache passes, and ReSTIR GI completely — 0 ms), `Raw 1-Bounce PT` (target default), or `ReSTIR GI` (feeds the bounce-1 samples into §11.5 ReSTIR GI resampling for the RTXDI analog). Secondary roughness floor (O9), secondary DI resampling (O2), and X₂ firefly clamp (O6) remain optional toggles for ReSTIR GI mode.

### 11.4 Multi-bounce radiance cache: NRC (default) with SHaRC toggle

Target updated 2026-11-25 ([spec/m8](../spec/m8/README.md), reversing the 2026-11-14 "NRC only" restriction): both **NRC** and **SHaRC** are integrated as runtime toggles (plus `Off` for 1-bounce only), so the same engine covers both `RTXGI` (NRC) and `RTXPT` (SHaRC) and benchmarks both caches directly.

| | NRC (default) | SHaRC (toggle, M8.4) |
|---|---|---|
| Method | tiny MLP trained online on tensor cores (SDK CUDA kernels via `VK_NVX_binary_import`) | spatially hashed world-space radiance cache (pure Vulkan 1.4 compute + `shaderBufferInt64Atomics` over BDA) |
| Update | sparse training paths (`nrc_update.slang`) | sparse multi-bounce paths (e.g. 1 per 4×4 block, `sharc_update.slang`, following `reference/RTXPT-main/Rtxpt/Sharc/`) |
| Frame order | update + bounce-1 query → `QueryAndTrain` → resolve (`nrc_resolve.slang`) | `sharc_update` → `sharc_resolve` → bounce-1 query inline in `indirect.slang` |
| Cost / dependencies | prebuilt NRC + CUDA DLLs, isolated SDK command buffers (C-G3-6), `C-G3-7` sentinel workaround | pure Slang/Vulkan shaders, zero external runtime DLLs, lazy BDA hash/accum/resolved buffers |
| Quality | handles glossy, adapts smoothly, no grid discretization | fast on all tiers, deterministic Vulkan execution, matches RTXPT 1.9.0 |
| Toggle behavior | when switched away, NRC is paused (never destroyed, C-G3-3); when `Off`, 0 ms | buffers allocated lazily when selected and retired cleanly when switched off; 0 ms when inactive |

References: `reference/RTXGI` (NRC 0.16, `Docs/NrcGuide.md`), `reference/RTXPT-main` (`Rtxpt/Sharc/`, `Shaders/IntroSample/SHaRC*.hlsl`, `PathTracerSHaRC.hlsli`), `ThirdParty/_deps/src/sharc`.

### 11.5 Indirect resampling: ReSTIR GI (optional toggle)

RTXDI ReSTIR GI (`rtxdi_gi_temporal`, `rtxdi_gi_shade`, optional `rtxdi_gi_duplication`) remains built as a runtime toggle so the engine reproduces NVIDIA's `RTXDI FullSample` and supports A/B quality/FPS comparisons against raw 1-bounce PT (§11.3).

- **1.0 target state (decided 2026-11-25):** **Off by default.** ReSTIR GI adds ~0.85–0.95 ms per frame and feeds temporally correlated indirect noise into DLSS-RR (`spec/fixesM1-6/` I15–I16 findings).
- When enabled, both NVIDIA's library defaults (`nvidiaGiSettings`) and the `fixesM1-6` tuned set (`tunedGiSettings`: ray-traced spatial correction, 1 spatial sample, O11 DLSS-RR decorrelation profile, age 30) plus `convergedGiSettings` stay selectable and gated by `ENGINE_GI_CHECK`. ReSTIR PT stays in §15.

### 11.6 Combine

`noisyColor = directRadiance + indirectRadiance + emissive` (RGBA16F), the only place the terms meet. Disabled stages contribute zero without being sampled (or zero-masked by flags). AO is never applied to direct light (PBR plan §2 #10); with path-traced indirect, material AO applies only to the cache query term. Built in G6 ([g6_combine.md](../spec/m4/g6_combine.md)): the `DLSS input` debug view shows the sum, `Direct lighting` the direct term + emissive that the ReSTIR DI checks compare.

### 11.7 Pipeline toggles and demo-analog modes

Replaces the earlier 3-way `IndirectMode { NrcRestirGi, Nrc, RestirGi }` (G7) with independent, composable runtime toggles (implemented in M8.3–M8.4, [spec/m8/README.md](../spec/m8/README.md)):

| Axis | Runtime options | 1.0 Target default |
|---|---|---|
| **Direct Lighting** | `Off` \| `Streaming RIS` \| `ReSTIR DI` (`Temporal` / `Spatial` / `Temporal + Spatial` / `Reference`) | `Streaming RIS` *or* `ReSTIR DI Temporal` (`Max History = 4–8`, `Basic`, Spatial OFF) |
| **Indirect (Bounce 1)** | `Off` \| `Raw 1-Bounce PT` \| `ReSTIR GI` | `Raw 1-Bounce PT` |
| **Radiance Cache (Multi-Bounce)** | `Off` \| `NRC` \| `SHaRC` | `NRC` (with one-click toggle to `SHaRC`) |
| **Denoiser** | `DLSS-RR` \| `Off` (`No AA` / `Accumulation`) | `DLSS-RR` |

**Switchable NVIDIA demo-analog presets** (one engine, toggle presets only):

| Preset | Direct | Indirect Bounce 1 | Radiance Cache | Denoiser | Analog of |
|---|---|---|---|---|---|
| **Engine Target** (shipped) | Streaming RIS / ReSTIR DI Temporal (`Basic`, Spatial OFF, History 4–8) | Raw 1-Bounce PT | NRC *(or SHaRC)* | DLSS-RR | Target Vulkan 1.0 stack |
| **RTXPT Analog** | Streaming RIS / NEE | Raw 1-Bounce PT | SHaRC | DLSS-RR | `reference/RTXPT-main` |
| **RTXDI Analog** | ReSTIR DI (`Medium`) | ReSTIR GI | Off | DLSS-RR | `reference/RTXDI/Samples/FullSample` |
| **RTXGI Analog** | Streaming RIS / ReSTIR DI | Raw 1-Bounce PT | NRC | DLSS-RR | `reference/RTXGI/Samples/IndirectDiffuse` |

Rules: every toggle switches at a frame boundary without a scene reload, resets affected temporal histories (ReSTIR / DLSS-RR / Accumulation) for one frame, and incurs **0 ms GPU cost** when off. The NRC context is paused rather than destroyed on a switch (C-G3-3); SHaRC buffers are allocated lazily when active and retired cleanly when off. Every preset and toggle is exposed in `SettingsWindow`, `SettingsRegistry` (`ENGINE_CONFIG`), and covered by the self-test runners.

---

## 12. Reconstruction and output

### 12.1 DLSS Ray Reconstruction

NGX directly (not Streamline), `DLSSD` feature, HW depth, packed roughness and scene-linear HDR input. Inputs: `noisyColor`, depth, motion, diffuse/specular albedo, normal+roughness, jitter (PBR plan H1); `specHitDistance` only as the optional input of §9.3. Output: display-resolution RGBA16F. Required: missing RR throws at init.

**As built:** `Dlss` creates the feature at the Quality render resolution (853×480 for a 1280×720 window) and evaluates it in the ray submit for the Lit view, in a command buffer of its own since 2026-11-18 (considerations C-G3-6). RR uses unity pre-exposure with NGX auto exposure disabled. Post applies the exposure after reconstruction (§12.3, considerations C-H2-2). NGX's kernels need the driver-managed descriptor heap, so an empty legacy descriptor set is bound before each evaluation (considerations C-H1-1). 1.14 ms on the RTX 5070 at that size.

### 12.2 Jitter and resolution

Halton(2,3), phase count `8 · (display / render)²`. LOD error (§8.1) and texture LOD bias (`log2(render / display)`) use display resolution.

Culling and jitter (§8.1 A8, as in `vk_lod_clusters`): frustum, Hi-Z and the LOD metric use the **unjittered** matrix. The exact pixel tests use the jittered render matrix or widen their rect by the jitter amplitude: the lossless sub-pixel cull and the per-triangle small test (which already reads jittered clip positions).

**As built (PBR E1, 2026-11-14):**
- `Camera` writes the Halton(2,3) offset to `GpuCameraData.jitterOffset`. `viewProj` and every matrix derived from it stay unjittered, and `jitterClip` adds the offset in the raster, the resolve's barycentrics and the per-triangle test.
- The cull views add this frame's jitter in pixels to `subpixelSnap`.
- Jitter is on by default since DLSS-RR (PBR H1). `ENGINE_JITTER=0` or the Camera panel's `Jitter` checkbox turns it off, and the motion test and guide capture default to off. `lodBias` is `log2(render / display)`.
- Only frames DLSS-RR evaluates are jittered (2026-11-14). A debug view, or Lit without lights, shows the render-resolution image scaled up, where the jitter boiled: on unlit `Sponza` 10% frame-to-frame change at Quality and 12% at Performance, against 0 without jitter. `ENGINE_JITTER=1` jitters every frame, so the cull check still covers the jitter rule.
- With jitter on, `ENGINE_CULL_CHECK` stays bit-identical (16 of 16) and `ENGINE_RT_CHECK` matches every frame.

### 12.3 Post

Auto exposure from a luminance histogram (compute) → tonemap (ACES fitted or AgX; one curve, applied once) → swapchain → ImGui. No bloom or other effects in the baseline.

**As built:** `PostProcess` runs three compute passes after DLSS-RR, for the Lit view only. The 256-bin histogram meters finite pixels over −12 to +12 log2 luminance. Black pixels count in the first bin. Six fractional bits split each pixel's weight between adjacent bins. The adaptation pass computes the 40th–95th percentile geometric mean, targets 0.18 and smooths exposure in EV. By default the EV gap halves in 0.5 seconds as the scene brightens and in 1 second as it darkens. The default upper bound is +6 EV. `post` reads the 1×1 R32F exposure and applies ACES (Hill's fit) into the display-resolution RGBA16F target. `present` draws that target into the sRGB swapchain. The Post panel controls exposure compensation, both adaptation speeds, the EV range and manual exposure. A resize or snap request applies the target immediately.

### 12.4 Transparency

v1: `Blend` treated as mask (cutoff 0.5). No forward raster transparency pass.

**Thin-walled glass (M8 I13, [i13_glass_transmission.md](../spec/fixesM1-6/i13_glass_transmission.md)).** A `KHR_materials_transmission` material with `transmissionFactor > 0`, `metallicFactor < 1`, roughness ≤ `kGlassMaxRoughness` (0.3), no metallic-roughness texture, not unlit and not spec-gloss is glass; the others render opaque and are logged with the reason. Glass:
- stays out of the raster templates, so the vis buffer and every guide describe the surface behind it;
- is in the TLAS under `kRayMaskGlass` (0x02); every other ray, visibility included, traces `kRayMaskOpaque` (0x01);
- is composited by the glass pass (`passes/glass.slang`) after `combine` and before DLSS-RR, as the RTXDI FullSample's `GlassPass` does: the camera ray walks up to `maxLayers` (4) glass layers in front of the vis-buffer surface, and noisy color becomes `Σ (Πⱼ<ᵢ Tⱼ)(Eᵢ + Rᵢ + Dᵢ) + (Π Tᵢ) · color`. `T = t(1−m)(1−F)·baseColor`, `R = F_layer · L(mirror ray)` (shaded as X₂: emission and one direct-light sample), `D` the non-transmitted dielectric share lit by one light sample, Lambert only;
- attenuates final direct light (DI shading, X₂ direct light, glass reflections) and final ReSTIR GI shading by the product of `T` along the shadow ray (`shadowTransmittance`). Light sampling, targets, resampling, GI sample generation and NRC stay glass-free (C-I13-3).

DLSS-RR keeps the guides of the surface behind (variant a); blended albedo guides (variant b) are a switch. Refraction, volume attenuation, rough transmission and dispersion are path-traced transmission (§15, C-I13-1).

---

## 13. Synchronization and queues

| Producer → consumer | Mechanism |
|---|---|
| CPU frame data → GPU | host-visible ring per frame slot, written after the slot's fence |
| async compute RT build → graphics lighting | timeline semaphore per frame |
| GI cache passes | **graphics queue only, in frame order.** NRC: every command buffer with NRC work must go to the queue passed to `EndFrame` (`NrcVk.h`), and inference runs inside `QueryAndTrain`, before ReSTIR GI can read X₂ radiance. No NRC work may overlap stages 12–15 |
| DGC preprocess → execute | explicit preprocess, all preprocesses of a phase batched before its executes ([dgc_comparison.md §5](../reference/dgc_comparison.md#5-command-recording-and-state)) |

Barriers: sync2 memory barriers with exact stage/access masks; one barrier per dependency edge, merged where edges share stages.

### 13.1 CPU/GPU parallelism (goal 6)

Each row is a pair of jobs that must overlap instead of running back to back. Rule for all of them: the only CPU wait inside a frame is the frame slot's fence; `waitIdle` exists only at startup, shutdown and swapchain recreation.

| # | Overlap | Mechanism | Milestone |
|---|---|---|---|
| X1 | CPU prepares and records frame N+1 while the GPU executes frame N | `MAX_FRAMES_IN_FLIGHT = 2`, per-slot host buffers, slot fence; counters and stats read back one or more frames late, never waited on | in place; verified in M0 timings |
| X2 | GPU builds RT structures (instance ops, PTLAS update; cluster BLAS after L2) on async compute while graphics culls, rasterizes and resolves | async compute queue, timeline semaphore before the first ray pass (§5, stage 6'); the PTLAS update waits for the previous frame's last ray pass, since it updates in place; buffers `CONCURRENT`, images released/acquired between families | M2 |
| X3 | CPU loads and processes a model (parse, meshoptimizer clusters and DAG, OMM bake, KTX2 conversion, cache I/O) on worker threads while frames keep rendering | job queue on worker threads; the render thread only commits finished results at a frame boundary | M9 |
| X4 | GPU uploads and decompresses a finished model (`vkCmdDecompressMemoryEXT`, CLAS/BLAS builds) while the CPU processes the next one | upload and build on the async compute queue with a timeline semaphore; the scene sees the model once its timeline value is reached | M9; the load-time CLAS/BLAS builds from M2 move onto this queue then |
| X5 | Unload without stalling: resources still used by frames in flight are freed later | deferred destruction queue keyed by frame-slot fence values; buffer compaction recorded as GPU work instead of `submitAndWait` | M9 |
| X6 | Readbacks (cull counters, GPU timers, picking) never block | host-visible VRAM read after the producing slot's fence, results shown with one to two frames of latency | in place |

Done when (M10): Nsight Systems on the Bistro benchmark paths shows no CPU wait inside a frame other than the slot fence, async compute overlapping graphics work, and a model loaded and unloaded during the camera path without a frame-time spike over budget.

NRC passes stay on the graphics queue in frame order (table above); they are not candidates for overlap.

X3–X5 are the base for post-1.0 streaming (§16.7). ST1 replaces the load-time CLAS/BLAS builds of X4 with RTXMG's GPU-only build pipeline (implicit destinations, a GPU allocator, `MOVE_OBJECTS`), so a streamed group needs no CPU readback or `waitIdle`.

---

## 14. Profiling, validation and debug

| Tool | Rule |
|---|---|
| GPU timers | `GpuTimerScope` per §2.3 row, rolling averages in ImGui, Tracy plots |
| Tracy | CPU + GPU zones for every pass, memory pools per resource kind |
| Counters | every GPU list (visible clusters, candidates, bins, light samples) reports count, capacity, overflow |
| Nsight Graphics / Systems | per-milestone capture; occupancy and RT traversal checks |
| Validation | layers via vkconfig, best-practices NVIDIA preset; clean before merge. GPU-AV runs need `ENGINE_GPUAV_RESOLVE_PER_BIN=1` until C-E1-3 closes (§9.1). Performance is measured with every layer off |
| Self-test runners | env-var runners, each deterministic per frame index and documented in [settings.md](./settings.md) §6: `ENGINE_CULL_CHECK` (culling lossless, 16 poses), `ENGINE_RT_CHECK` (primary rays against the vis buffer, `alpha-edge` class), `ENGINE_MOTION_TEST` (13 scenarios), `ENGINE_GUIDE_CAPTURE` (guides, A4 normals, 3 ReSTIR DI convergence shots, 2 glass throughput checks), `ENGINE_GI_CHECK` (ReSTIR GI and NRC against the ground truth, seeded), and M8 I1's `ENGINE_STABILITY_CHECK`, `ENGINE_LIGHT_CHANGE_CHECK` and `ENGINE_SCREEN_EDGE_CHECK`. `ENGINE_CHECK_SCENE` moves the GI and temporal runners from Sponza to Bistro's café (`bistro_cafe`, F22 / F23) or Bistro's glass poses (`bistro`, I13); I12 Phase 0 adds `bistro_glossy`. Planned: `ENGINE_DLSS_CHECK` (M8 I14) |
| Settings lab | `ENGINE_LAB=<plan>` runs plans of settings switches over named cases and writes a CSV to `logs/lab` that ranks the switches. It is separate from the checks: the checks ignore `ENGINE_CONFIG`, and the lab refuses to start next to them ([full_testing.md](../spec/features/planned/full_testing.md), [settings.md](./settings.md) §9). It finds candidates, and the runners decide |
| Debug views | `DebugView` table: geometry (clusters, LOD level, triangles, instances), vis buffer ids, every §9.3 guide, RTXDI reservoirs, cache debug resolve, DLSS input vs output. Geometry views are derived from the vis buffer in compute; overdraw is not planned (it would need a raster debug variant) |
| Ground truth | accumulation mode of the same path tracer (many frames, cache off, high bounce count) on static frames; no second renderer (PBR plan §8). Built in G5: `RtxdiContext::giReferenceBounces` traces 8 bounces after X2 with NRC paused, the GI reference shades the samples without reuse, and `ENGINE_GI_CHECK` averages it against ReSTIR GI |
| Benchmarks | fixed camera paths per scene, recorded per tier, compared with §2.3 |

Benchmark scenes:
- Sponza (+ Intel Sponza curtains/ivy for alpha).
- **Bistro**, the final-goal scene: many lights, 76K emissive triangles, spec-gloss materials, thin-walled glass. It is in `models/bistro` as KTX2 only, through Git LFS, loaded as 2,909 instances; its café interior is lit by emissive triangles only.
- Khronos validation assets (PBR plan "Validation assets").
- After 1.0: Bistro on cluster LOD with the M10 benchmark paths (§16.3, L3); a dense scan or Zorah-class scene for cluster LOD and streaming (as used by `vk_lod_clusters`, §16.7); the Khronos animation assets in `gltf/2.0` (`AnimatedCube`, `BoxAnimated`, `InterpolationTest`, `SimpleSkin`, `RiggedFigure`, `CesiumMan`, `BrainStem`, `Fox`, `AnimatedMorphCube`, `MorphStressTest`) plus an instanced crowd for animation (§16.6).

---

## 15. Optional and later techniques

Each is a separate stage, added only when the baseline meets §2.3 and the technique is measured.

| Technique | Benefit | Cost / prerequisite | Reference |
|---|---|---|---|
| ReSTIR PT | multi-bounce resampling, better glossy | heavier than ReSTIR GI | RTXDI |
| Neural materials (cooperative vectors) | layered materials at fixed cost | training pipeline, tensor-core time | RTXNS |
| Neural texture compression | VRAM on the floor tier | decode cost per sample | RTXNTC |
| Tessellated / displaced clusters | micro-detail without stored triangles | cluster tessellation pipeline | `vk_tessellated_clusters`, RTXMG |
| Software raster for tiny clusters | sub-pixel triangle efficiency | compute raster into the vis buffer with 64-bit atomics | `vk_lod_clusters` (SW raster path) |
| Path-traced transmission | glass, water | §12.4 | RTXPT |

---

## 16. Roadmap

### 16.1 Current state (`3d24416c6` plus the working tree, 2026-11-25)

| Area | State |
|---|---|
| Vulkan 1.4 base, shader objects, descriptor heaps, BDA frame root, model slices, shared models/materials | done |
| meshoptimizer clusters as cluster-LOD level 0 (Spatial builder since M8 I10 MG-P3, level-1 optimize), groups via `partitionClusters`, box bounds + cones, index filtering | done |
| GPU culling aligned with `vk_lod_clusters` leaf level (§8.1): `culling.slang` port, box tests, group tier, per-group reject bitmasks, `{instance, cluster}` visible entries, frozen-pyramid freeze, 1-fetch Hi-Z, lossless sub-pixel, homogeneous per-triangle backface; history invalidation | done (A1–A5, A7, C1–C3); image and timings on the RTX 5070 done (§2.3, `ENGINE_CULL_CHECK`); the 2060 Super open |
| DGC raster: GPU-written sequences, explicit preprocess, one execute per (phase, raster bin) | done |
| Animation code (§7.3) | removed (2026-11-08); returns post-1.0 on the GPU and Mega Geometry (§16.6) |
| Culling list capacities and overflow counters (A10) | done |
| Reverse-Z, 3-level Hi-Z, `R32_UINT` vis buffer, opaque bin with winding flip (M1) | done; culling is lossless under reverse Z (`ENGINE_CULL_CHECK`: 16 of 16 poses identical with culling on and off) |
| Resolve writing the §9.3 guides and motion; `sampleMaterial` (PBR B1) | built as one tiled dispatch in M1, binned since M7 E1 (§9.1); motion checked by the scripted motion test (`ENGINE_MOTION_TEST`, [motion_vectors.md](../spec/features/done/motion_vectors.md) §5.1); guides match the Khronos screenshots for `MetalRoughSpheres`, `TwoSidedPlane` and `UnlitTest` (`ENGINE_GUIDE_CAPTURE`, PBR plan E1); A4 generated tangents and flat normals match their references pixel by pixel (PBR plan A4) |
| BRDF (PBR B2): GGX, VNDF sampling, `tests/brdf_test.cpp` | built |
| Punctual lights (PBR B3): `evalLight` in `lighting/lights.slang`, `tests/lights_test.cpp` | built; called by the RTXDI bridge (PBR F2) and the `Unshadowed lights` / `Shadowed lights` views |
| Debug views, GPU timers, Tracy | done |
| Per-stage timings on both cards (M0, M1 rows of §2.3) | RTX 5070 recorded 2026-11-13 and as the full frame profile in M8 I4 (§2.3); the 2060 Super not recorded (M8 I11) |
| DLSS, NRC, SHaRC | DLSS-RR evaluated on Lit with `DenoiserMode::Off` + `Accumulation` added (`nvidia_settings_layout.md`, GPU checks pending C-NS-1); NRC 0.16 built since M4 (G3, leak worked around C-G3-7, divergence guard C-I16-5); SHaRC planned in M8.4 as a runtime toggle beside NRC |
| Direct and indirect lighting (ReSTIR DI 3.2, compute indirect bounce, ReSTIR GI, NRC, combine, indirect mode toggle), DLSS-RR, auto exposure + ACES tonemap, cluster AS and PTLAS, GI ground truth | built (M2–M4; §16.2); composable Direct / Indirect / Cache / Denoiser toggles planned in M8.3 |
| OMM and the alpha-tested raster bin | built (M6, [spec/m6](../spec/m6/README.md), 2026-11-20) |
| Material binning, emissive-triangle lights, light presampling | built (M7, [spec/m7](../spec/m7/README.md), 2026-11-20/21); `Power_RIS` and `ReGIR_RIS` selectable |
| Bistro | loads with KTX2 textures and spec-gloss materials (C-M7-1); glass composited (I13); café and glass check scenes |
| Prior M1–M7 fixes & tuning ([spec/fixesM1-6](../spec/fixesM1-6/README.md)) | I1–I10, I13, I14, I15, partial I16, settings lab, and `SettingsWindow` built; superseded by new M8 ([spec/m8](../spec/m8/README.md)) |
| Unified RTXPT/RTXDI/RTXGI pipeline, audits, toggles, SHaRC, perf (M8) | planned 2026-11-25 ([spec/m8/README.md](../spec/m8/README.md)): M8.1–M8.6 |
| SER RT pipeline, triangle BLAS, cluster LOD, animation, streaming, hybrid RT + Cleanest Image Quality | SER shelved to post-1.0 (§16.5 V1); triangle BLAS post-1.0 (V2); cluster LOD (§16.3), multithreading (§16.4), animation (§16.6), streaming (§16.7), and game-like hybrid RT + Cleanest Image Quality toggle (§16.8) post-1.0 |
| Structure and document debt | §20 |

### 16.2 Milestones

Each milestone renders something testable and is measured against §2.3 before the next starts.

| # | Milestone | Content | Done when |
|---|---|---|---|
| M0 | Contracts | PBR A1, A2, A4; `GpuTargets` block; remove animation code (§7.3); log SER reorder hint and decompression methods; per-stage timers recorded on both cards; culling list capacities + overflow counters and the open culling checks (§8.1 A10, verification) | structs asserted; baseline timings on the 2060 Super and the 5070; culling image checked on both |
| M1 | Vis buffer | reverse-Z with the `nvhiz` Hi-Z port (§8.1 A9), vis buffer raster, opaque bin with winding-flip two-sided (A6), resolve with analytic derivatives as **one tiled compute dispatch** (one PBR shader until M7: no classification, no DGC), PBR B1 `sampleMaterial` brought forward from M3, §9.3 guides, motion; plan: [visibility_buffer.md](../spec/features/done/visibility_buffer.md) | debug views of every guide; same geometry as today; culling lossless under reverse Z: the image with every cull test on equals the image with all off (`ENGINE_CULL_CHECK`). Reverse Z is the reference; the old forward-Z build is not compared (2026-11-13) |
| M2 | Cluster RT bring-up | CLAS per meshlet, one static cluster BLAS per primitive, GPU-written instance ops + PTLAS updated in place on the async compute queue overlapping graphics (§13.1 X2), position fetch, `decodeHit` with cluster ids, ray-query debug pass (§10.3, last paragraph) | debug CS colors by cluster; unload keeps other models traceable; a still frame writes no PTLAS ops |
| M3 | First lit frame | BRDF + lights (PBR B2–B3), RTXDI DI (PBR F), DLSS-RR + post (PBR H), culling jitter rule (§12.2) | stable direct light on `LightsPunctualLamp`, Sponza + sun |
| M4 | Indirect | indirect pass and NRC paths with ray queries (§11.1; SER shelved to §16.5 V1), NRC (§11.4, PBR G3), ReSTIR GI, combine, indirect mode toggle NRC → ReSTIR GI / NRC / ReSTIR GI (§11.7, PBR G7) | bounce light under Sponza arches, stable after RR; NRC cache converges after load (the loss cannot be read on Vulkan, considerations C-G3-4); all three indirect modes switch at runtime and converge to their references, timings of each recorded on both tiers |
| M5 | — | merged into M4 (2026-11-14): NRC is the only cache | — |
| M6 | Alpha | OMM bake per primitive, attach per cluster, build (§10.5), alpha-tested raster bucket | foliage cut-outs in raster and shadows; zero ray candidates |
| M7 | Material binning | tile classification + bin-id image (§9.1), DGC compute execution set with the overlap guard (§9.2), emissive-triangle lights, `Power_RIS` | per-bin timings; Bistro emissives lit |
| M8 | Unified RTXPT / RTXDI / RTXGI pipeline, audits, toggles, SHaRC & perf | M8.1 pipeline audit (BRDF, Mega Geometry + OMM, ReSTIR DI, ReSTIR GI, NRC, NRC + ReSTIR GI) vs NVIDIA demos (`RTXPT-main`, `RTXDI`, `RTXGI`, `RTXMG-main`, `vk_lod_clusters`, `vk_denoise_dlssrr`, `vk_gltf_renderer`) + bug fixes (`C-GA-1`/`C-T-6`); M8.2 ImGui & settings wiring audit (`C-NS-1`); M8.3 composable pipeline toggles (Direct `Off`/Streaming RIS/ReSTIR DI, Indirect `Off`/Raw 1-Bounce PT/ReSTIR GI, Cache `Off`/NRC/SHaRC, Denoiser `DLSS-RR`/`Off`, plus demo-analog presets); M8.4 SHaRC multi-bounce cache toggle beside NRC; M8.5 lean target-stack implementation & modern Vulkan/shader perf optimization + head-to-head FPS benchmarks vs NVIDIA demos; M8.6 full verification & RTX 2060 Super floor tier. Plan: [spec/m8/README.md](../spec/m8/README.md) (supersedes [spec/fixesM1-6](../spec/fixesM1-6/README.md)) | audits complete with fixes landed; every toggle & demo-analog preset switches cleanly with 0 ms cost when disabled; SHaRC & NRC both work as toggles; target lighting stack meets §2.3 budgets and beats NVIDIA RTXPT / RTXDI / RTXGI demos in FPS on the RTX 5070; all runners and validation (including GPU-AV) clean; 5070 + 2060 Super tables recorded |
| M9 | Asset pipeline | §6.4 and §7.2: automatic KTX2 conversion, GDeflate cache, GPU decompression for textures and geometry; background loading on worker threads, async upload and build, deferred destruction on unload (§13.1 X3–X5) | §6.4 and §7.2 hold; Bistro load time and PCIe bytes recorded; loading a model while rendering causes no frame over budget |
| M10 | **1.0 final goal: Bistro** | full Bistro scene, complete toggle-composable lighting stack (target default: DLSS-RR + Streaming RIS or Temporal-only Basic ReSTIR DI + Raw 1-Bounce PT + NRC/SHaRC; RTXPT / RTXDI / RTXGI presets callable); fixed benchmark paths; per-stage tuning on both tiers; CPU/GPU overlap verified (§13.1) | §2.3 met on the reference tier and beats NVIDIA demos in FPS; floor result recorded (met, or overrun list → §2.1 outcome); §13.1 done criteria met |

**Status (2026-11-25):**
- **Done:** M1, M3 and M6.
- **Built on the RTX 5070:** M0, M2, M4 and M7. Their 2060 Super checks stay open (C-M0-1), as does the M2 overlap capture, which I4 recorded for the 5070 (F9).
- **M4:** G1 was built, then shelved 2026-11-17 to post-1.0 V1. G2, G3, G5, G6 and G7 are built.
- **M5** was merged into M4.
- **Prior M1–M7 fixes (`spec/fixesM1-6/`):** I1–I10, I13, I14, I15, partial I16, settings lab, and `SettingsWindow` landed; superseded on 2026-11-25 by the new **M8** ([spec/m8/README.md](../spec/m8/README.md)).
- **M8 is planned (2026-11-25):** M8.1–M8.6.
- **Not started:** M9 and M10.

AU items not marked done are optional (§20.2).

| # | Done | Open |
|---|---|---|
| M0 | PBR A1, A2, A4; `GpuTargets`; animation removed; SER reorder hint and decompression methods logged; culling capacities and overflow counters; shared structs asserted (layout test, AU11) | baseline timings and culling image on the 2060 Super, which is not in the dev machine (the RTX 5070 is done: §2.3, `ENGINE_CULL_CHECK`) |
| M1 | reverse-Z + Hi-Z, vis buffer, opaque bin with winding flip, single-dispatch resolve, B1, §9.3 guides, motion; debug views of every guide; guides match the Khronos screenshots (PBR E1, `ENGINE_GUIDE_CAPTURE`); motion checked by `ENGINE_MOTION_TEST`; culling lossless under reverse Z (`ENGINE_CULL_CHECK`, 16 of 16 poses); RTX 5070 timings (§2.3) | **done** (2026-11-13). Timings on the 2060 Super stay with M0 |
| M2 | PBR C1: cluster AS and position fetch required; one CLAS per meshlet and one cluster BLAS per mesh primitive, built at load per model (`MegaGeometry`). PBR C2: GPU-written TLAS instances and a full TLAS build per frame on the async compute queue (`RayTracingScene`, timeline semaphore). PBR C3: `decodeHit` over the resolve's own functions, ray-query debug views colored like the vis-buffer ones, `SafeSpawnPoint` origins and ray cones; `ENGINE_RT_CHECK` matches primary rays to the vis buffer on every checked frame, unload included. PBR C4: one PTLAS updated in place from GPU-written instance writes (`FrameSync::rayTimeline` orders it after the previous frame's rays), grid partitions without a global partition, and the OMM 2-state ray flag in `traceVisibility` and `traceClosest`; a still frame writes no instances | the overlap capture in Tracy/Nsight; position fetch, cluster AS and partitioned AS on the 2060 Super (Q7) |
| M3 | PBR B2 (BRDF), B3 (`evalLight`); B1's `KHR_texture_transform` check (`TextureTransformTest`, `TextureTransformMultiTest`); E1's AO guide, unlit specular weight, `lodBias` and Halton jitter with the culling jitter rule (§12.2); E2's GPU world light buffer for punctual lights; F1's RTXDI host context (reservoirs, neighbor offsets, constants per slot); F2's RTXDI bridge (`RAB_*` over the guides, `traceVisibility`) with the position guide and `motion.z`; F3's ReSTIR DI passes into `directRadiance` (`Direct lighting` view, formerly `Lighting only`), matching RTXDI's unbiased reference on `LightsPunctualLamp` (updated to RTXDI 3.2 on 2026-11-15); H1's DLSS-RR at the Quality render resolution (1.14 ms on the 5070); H2's histogram auto exposure and ACES tonemap (0.030 ms), with the exposure passed to DLSS-RR; E3's engine sun; on sunlit `Sponza` the Lit output holds still (temporal deviation 0.35% of the mean at Quality) and repeats at repeated poses under a yawing camera and a moving sun (2.4% and 1.5%) | **done** (2026-11-14). The 2060 Super timings stay with M0 (considerations C-M0-1, C-F3-7); E2's emissive triangles move to M7 |
| M4 | PBR G1 built then shelved 2026-11-17 to post-1.0 V1 (C-G1-3); G2 compute ray-query indirect pass (`IndirectPass`, `passes/indirect.slang`); G3 NRC (`NrcCache`, `passes/nrc_update.slang`, `passes/nrc_resolve.slang`, 0.62 ms on 5070); G5 ReSTIR GI (`rtxdi_gi_temporal`, `rtxdi_gi_shade`) + §14 GI ground truth and `ENGINE_GI_CHECK`, ray-traced spatial bias correction since 2026-11-20 (C-G5-2); G6 combine (`noisyColor = direct + indirect + emissive`, `Direct lighting` / `DLSS input` views); G7 runtime indirect mode toggle (`NRC → ReSTIR GI` / `NRC` / `ReSTIR GI`; ReSTIR GI default on both tiers since 2026-11-20; 5070 GPU total 3.3 / 2.9 / 2.7 ms at 1280×720) | 2060 Super timings and NRC hardware check (R4, C-M0-1, C-G3-2); default tuning and NRC +10% bias (C-M4-2, C-G5-3) moved to M8 |
| M5 | — | merged into M4 (2026-11-14) |
| M6 | D1 OMM SDK bake per alpha primitive; D2 micromap per primitive built before its CLAS, per-cluster OMM index ranges, alpha instances without `FORCE_OPAQUE`; D3 alpha-tested raster bin; D4 zero-candidate counter and the `alpha-edge` RT-check class. `AlphaBlendModeTest` and Sponza's plants cut out in raster and shadows; every runner green, GPU-AV clean ([spec/m6](../spec/m6/README.md)) | **done** (2026-11-20). Accepted: micromap build cost (C-D2-1), the known-area gate on Sponza's chains (C-D1-2) |
| M7 | E1 material binning (classify + DGC execution set of `Background` / `Standard` / `Unlit`, guides bit-identical); E2 emissive-triangle lights written per frame by `light_world`; F1 light PDF, `Power_RIS` tiles and ReGIR cells ([spec/m7](../spec/m7/README.md)). Bistro loads with its spec-gloss materials (C-M7-1) | built (2026-11-20/21). Open: GPU-AV on the execution set (C-E1-3, temporary per-bin switch), C-E2-2, C-F1-4 to C-F1-6; the "Bistro emissives lit" check is still to be run; M8 I14 Phase 4 adds an independent Bistro textured-emission check |
| M8 | Prior `spec/fixesM1-6/` fixes landed as baseline: I1–I10, I13 glass, I14 Bistro ray/DLSS/light-PDF fixes, I15 café DI/GI, partial I16 (exact sun, specular hit distance, NRC divergence guard), settings lab, and `SettingsWindow` + denoiser `Off` + `Accumulation` (`3d24416c6`) | New M8 ([spec/m8/README.md](../spec/m8/README.md)): M8.1 pipeline audit vs NVIDIA demos (incl. `C-GA-1`/`C-T-6`), M8.2 ImGui wiring audit (`C-NS-1`), M8.3 composable pipeline toggles, M8.4 SHaRC cache toggle, M8.5 target lighting stack + Vulkan perf optimization + beat NVIDIA demo FPS benchmark, M8.6 full verification & RTX 2060 Super floor tier |
| M9–M10 | — | not started |

**Work added by the audit (§20.2):**

| When | Work |
|---|---|
| Before M2 | close the M0/M1 checks above. Done: the document fixes of §20.4 (2026-11-13); the defects of §20.3 (2026-11-13); dead files and stubs (AU7, 2026-11-12); reverse-Z-only culling, the Slang/C++ layout test and typed storage images (AU5, AU11, 2026-11-13); `Engine` / `EngineUi` split (AU3); shared color-space rules (AU6, 2026-11-15). one swapchain recreate path (AU10) and build linkage (AU8), 2026-11-15 |
| M2 | optional: retire queue keyed by slot fence, used by acceleration structures from the start (AU1); GPU scene-update pass fed by a CPU delta list (AU4). Done: `ResourceManager` split (AU2) |
| M4 | done in G3: `vk_nrc` module built (`nrc_library` stays alongside it for the linkage test; AU9) |
| M9 | optional: remaining `waitIdle` and `submitAndWait` sites removed (AU1); `assets_loader.cpp` split (AU6) |

### 16.3 Post-1.0 targets: Nanite-like cluster LOD

Starts once rendering is complete (M10). Following `vk_lod_clusters` (local source in `reference/vk_lod_clusters`). The RT data model (§10) is already cluster-based and the culling is already its leaf level (§8.1), so this adds levels above it, not a new path.

| # | Milestone | Content | Done when |
|---|---|---|---|
| L1 | Cluster LOD raster | DAG build + cache (§7.1); node tree above the groups, `TraversalMetric` per node and group, `generatingGroup` per cluster, `testForTraversal` in display pixels, persistent traversal kernel with warp task packing, reject nodes and groups (§8.1); fixed list capacities with clamping (A10); `classifyInstanceLod` and discrete LOD for distant instances; software raster for tiny clusters into the vis buffer (64-bit atomics) | dense scene within budget on both tiers |
| L2 | Cluster LOD RT | RT LOD traversal, per-frame cluster BLAS with sharing / merging / caching, BLAS swaps as PTLAS `UPDATE_INSTANCE` ops, hybrid-consistency rule (§10.3–10.4) | RT build ≤ §2.3 row; no self-shadowing in the debug view |
| L3 | **Post-1.0 target: Bistro on cluster LOD** | the full Bistro scene with every mesh through the cluster DAG in raster and RT (no level-0-only path left), complete lighting stack, M10 benchmark paths | §2.3 met on the reference tier with LOD on; floor result recorded; per-stage A/B against the M10 numbers; no visible LOD popping or raster/RT mismatch on the benchmark paths |

### 16.4 Post-1.0 target: multithreaded rendering

Starts after M10; independent of L1–L3. The GPU-driven frame keeps per-frame recording small, so threading targets the CPU work that grows with features (more passes, more SDKs, UI, simulation), not per-object work, which stays on the GPU (goal 2).

| # | Milestone | Content | Done when |
|---|---|---|---|
| T1 | Job system | one task-graph scheduler for all CPU work (asset jobs of §13.1 X3 move onto it); worker count from the machine's core count; Tracy zones per job | asset loading and per-frame jobs share one scheduler; no ad-hoc threads left |
| T2 | Main / render thread split | the main thread (input, camera, scene edits, UI building) produces an immutable frame snapshot; the render thread consumes it one frame later and owns all Vulkan submission | Tracy shows the two threads overlapping; input-to-photon latency recorded before and after |
| T3 | Parallel recording | independent passes recorded on workers into their own primary command buffers (one command pool per thread per frame slot), submitted in frame order by the render thread; secondary command buffers only where one rendering scope must be split | render-thread CPU time drops versus T2 at the same GPU output; validation clean, including thread-safety checks |
| T4 | Parallel frame prep | scene delta gathering, frame-root and cull-view writes, GPU-timer and counter readback, and shader-object creation at load spread over jobs | CPU frame time scales with worker count until the GPU is the limit |

Rules: Vulkan objects that are externally synchronized (command pools, queues, the descriptor heap free list) have one owner thread or one lock; VMA stays in its default thread-safe mode. Measure the CPU frame in Tracy before T3/T4 and skip a step whose CPU cost is not on the critical path.

### 16.5 Post-1.0 target: switchable ray paths

Starts after M10; independent of L1–L3 and T1–T4. 1.0 shades every ray hit from ray queries in compute, because Slang's EXT SER support was not ready (considerations C-G1-3). SER is the direction the engine wants: it reorders the incoherent bounce and training rays, which is where it gains most, and hardware reordering has been on NVIDIA GPUs since Ada. NVIDIA's samples also show the other side can win for some content ([mega_geometry_comparison.md](../reference/mega_geometry_comparison.md) F1, F2): `vk_lod_clusters` path-traces 0.5–1.1 ms faster with ray queries than with its RT pipeline, and `vk_animated_clusters` recommends triangle BLAS for static content because they trace faster than cluster BLAS. The engine keeps **both ways of each behind a toggle**, so the choice is measured per scene and tier, never assumed, and neither path rots.

| # | Milestone | Content | Done when |
|---|---|---|---|
| V1 | SER RT pipeline beside ray queries | **Gate:** a pinned Slang with the EXT SER fixes (slang [#12554](https://github.com/shader-slang/slang/issues/12554), [#13763](https://github.com/shader-slang/slang/pull/13763), [#13772](https://github.com/shader-slang/slang/issues/13772), [#13774](https://github.com/shader-slang/slang/issues/13774)), expected around the 1.0 release. Every shaded-ray pass of §11.1 (indirect bounce, cache update paths) gets its SER pipeline variant back from the shelved code (`raytracing/ser.slang`, `RtPipeline`, `rt_ser_check`): `HitObject` trace → `ReorderThread` → minimal closest-hit → shade in raygen, sharing the shading body (decode, shading, outputs) with the 1.0 compute variant. Both are built at startup and a runtime toggle picks one per frame; reorder on/off is a second toggle | both variants give the same image (the SER check compares them bit for bit); both validation-clean, GPU-AV included (the 1.0 workarounds C-G1-1, C-G1-2, C-G2-2 rechecked and dropped where Slang or the driver fixed them); per-pass timings of both, reorder on and off, on both tiers recorded. SER becomes the default unless the numbers say otherwise |
| V2 | Triangle BLAS for static geometry | a compacted `PREFER_FAST_TRACE` triangle BLAS per static primitive, built at load from the meshlet-ordered triangle list, next to its cluster BLAS; a per-triangle cluster row maps `PrimitiveIndex` to `(cluster, triangle)`, so `decodeHit` is unchanged; a toggle selects which BLAS address the PTLAS instances reference (a switch is a full PTLAS build). **I10 measurement (2026-11-22, RTX 5070):** even against post-I10 spatial CLAS, compacted triangle BLAS traced `−12.4%` gallery / `−15.1%` courtyard / `−5.2%` Bistro clock-normalized `RaySum` (`−12.9% / −15.2% / −14.6%` raw) and saved `0.31 MiB` Sponza / `4.04 MiB` Bistro VRAM ([i10_mega_geometry_perf.md](../spec/fixesM1-6/i10_mega_geometry_perf.md)). **Decision (2026-11-23):** stays post-1.0 so 1.0 keeps one CLAS path ahead of post-1.0 animations, streaming, and large tree worlds | RT check and SER check pass with both; trace time, build time and BLAS memory of both recorded on both tiers, default set from the numbers |

Rules:
- Both variants of each toggle stay compiled and covered: the self-test runners (`ENGINE_RT_CHECK`, guide capture) run each.
- A toggle switches at a frame boundary, without a reload.
- V2 applies to static, single-level geometry. Instances on per-frame LOD cuts (L2) keep cluster BLAS. With OMM (M6), the triangle BLAS carries the same micromaps through `VkAccelerationStructureTrianglesOpacityMicromapEXT`.
- V1 and V2 compose: all four combinations are valid and measured.
- V2 and animation (§16.6): a deforming instance always uses cluster BLAS built from templates. V2 stays for geometry that never deforms.

### 16.6 Post-1.0 target: animation on RTX Mega Geometry

Starts after M10. AN1 is independent of L1–L3. AN2–AN4 come before or alongside L1, because a deforming instance needs its own vertex data, which the LOD traversal must address.

Every animated pixel must agree in raster and rays in the same frame, so it is both drawn and traced from one set of deformed positions. NVIDIA's reference is `vk_animated_clusters` (local source in `reference/vk_animated_clusters`). On an RTX 6000 Ada, with 8.43 M animated triangles at 2560×1440, it measured:

| Path | AS build / refit | Render | Sum | RT memory |
|---|---|---|---|---|
| Cluster templates | 0.80 ms | 1.52 ms | 2.32 ms | 271 MB |
| Triangle BLAS, refits | 2.78 ms | 1.45 ms | 4.23 ms | 564 MB |
| Triangle BLAS, 10% rebuilds | 5.22 ms | 1.39 ms | 6.61 ms | 564 MB |

Its README recommends CLAS for content that changes every frame and triangle BLAS for static content, which is the §16.5 V2 split. Further sources:
- RTXMG's cluster tessellator and `docs/ClusterTess.md` use the same template machinery;
- [ray_tracing_mega_geometry.md](../spec/features/planned/ray_tracing_mega_geometry.md) §9.6;
- [mega_geometry_comparison.md](../reference/mega_geometry_comparison.md) F8 and §4;
- [mg_optimizations_fixes.md](../spec/fixesM1-6/mg_optimizations_fixes.md) §4.

| # | Milestone | Content | Done when |
|---|---|---|---|
| AN1 | Node animation (rigid) | glTF `animations` channels (translation, rotation, scale; `LINEAR`, `STEP`, `CUBICSPLINE`) for node transforms. Each loaded model gets an animation slice of keyframe times and values in the asset buffers. A GPU pass evaluates the active clips, walks the node hierarchy and writes `GpuNodeInstance` with `prevWorldMatrix`. The CPU writes only clip time and play state per entity (goal 2). The existing moving-instance path does the rest: motion vectors, PTLAS `WRITE_INSTANCE` ops for changed instances only, partition reassignment and growing scene bounds (MG-B4). Lights attached to animated nodes move through `light_world` | `AnimatedCube`, `BoxAnimated` and `InterpolationTest` match the Khronos sample renders. `ENGINE_MOTION_TEST` gains animated-node scenarios. `ENGINE_RT_CHECK` matches every frame. A paused clip writes no PTLAS ops. ReSTIR DI and GI history survives the motion (stability runner on an animated pose) |
| AN2 | Skinning and morph targets on the GPU | The loader parses `JOINTS_n` / `WEIGHTS_n`, skins, inverse bind matrices and morph targets again (removed in M0; [animation_removal.md](../spec/m0/animation_removal.md)), into model-relative slices. A deformation compute pass at the start of the frame, before stage 1, writes per-instance deformed position, normal and tangent streams, plus last frame's positions for motion vectors. This follows `vk_animated_clusters`: each animated instance gets its own vertex data, while static instances keep sharing the model's. Raster, the vis-buffer resolve and ray-hit decode read the deformed streams through the same `(instance, cluster, triangle)` ids. Cluster boxes for deforming clusters are recomputed or padded per frame, and the normal-cone test is off for them, since a cone baked in the bind pose is not conservative. The pass is GPU-driven: no CPU loop over joints or vertices | `SimpleSkin`, `RiggedFigure`, `CesiumMan`, `BrainStem`, `Fox`, `AnimatedMorphCube` and `MorphStressTest` match the Khronos renders. `ENGINE_CULL_CHECK` is bit-identical on an animated pose (culling stays lossless with padded boxes). The motion test checks skinned motion vectors. The deformation pass cost and the per-instance memory are recorded |
| AN3 | Mega Geometry for animation: cluster templates | At load, one cluster template per meshlet of each deformable primitive (`BUILD_TRIANGLE_CLUSTER_TEMPLATE`, `PREFER_FAST_TRACE`), from the meshlet's 8-bit local indices. Each template's box is padded by a percentage of the object box: the sample's "template bbox bloat", 10% by default; too little gives artifacts. Every frame on async compute, after the deformation pass (timeline semaphore), the GPU does three things. It instantiates each deforming cluster's CLAS from the deformed positions (`INSTANTIATE_TRIANGLE_CLUSTER`, `PREFER_FAST_BUILD`) into implicit destinations sized exactly by the instantiation query. It rebuilds the instance's cluster BLAS over them. It patches the new BLAS address into the PTLAS as an `UPDATE_INSTANCE` op without a readback. OMM micromaps stay attached per cluster through the instantiation, so animated foliage keeps its cut-outs. `ClusterID` stays the meshlet row, so `decodeHit` is unchanged. Position truncation is re-measured here, since the sample calls templates "very sensitive" to it (MG-P2 numbers: −18–20% CLAS) | `ENGINE_RT_CHECK` matches the deformed raster on every frame of the AN2 assets, with 0 self-hits. A/B against a refit triangle BLAS per instance: build, trace and memory recorded on both tiers. The RT build stays within its §2.3 row on an instanced skinned crowd (the `vk_animated_clusters` bunny-grid analogue) |
| AN4 | Procedural deformation and foliage | Vertex animation outside glTF: wind on alpha-tested foliage and vegetation, written by the AN2 pass from per-material parameters and traced through AN3's templates. The large-world levers of [mg_optimizations_fixes.md](../spec/fixesM1-6/mg_optimizations_fixes.md) §4 are re-measured here, because thousands of moving instances change their balance: explicit PTLAS instance boxes (the reverted half of MG-P5), `WRITE_PARTITION_TRANSLATION` for moving groups, and a per-mesh BLAS merge for multi-primitive trees (MG-S2) | a wind-animated foliage scene keeps leaf-shaped shadows (M6 criteria) while moving. Each large-world lever is landed with its ABBA gain or closed with its numbers |

Rules:
- One set of deformed positions per frame feeds raster, the resolve and rays. No pass reads bind-pose positions for an animated instance.
- Static instances of an animated model's mesh keep the load-time CLAS and shared vertex data. Only instances whose animation is playing pay for deformation and instantiation.
- Deformation and instantiation have their own GPU timer rows, added to §2.3 when AN2 starts.
- Animation and cluster LOD compose as in RTXMG and `vk_lod_clusters`: the deformed level-0 clusters feed the DAG's coarser levels through the same templates. The order is fixed in the L1/AN2 step specs.

### 16.7 Post-1.0 target: streaming

Starts after M9's asset pipeline (§6.4, §7.2, §13.1 X3–X5). Geometry streaming needs cluster LOD (L1, L2): the unit streamed is a LOD group, and the coarsest groups always stay resident. 1.0's rule that everything is resident (§10.2) ends here.

NVIDIA's references:
- `vk_lod_clusters`: [streaming.md](../reference/vk_lod_clusters/docs/streaming.md), [hitbased_streaming.md](../reference/vk_lod_clusters/docs/hitbased_streaming.md), [clas_allocation.md](../reference/vk_lod_clusters/docs/clas_allocation.md), `src/scene_streaming*.cpp`, `shaders/stream_*.comp.glsl`;
- RTXMG's `rtxmg/cluster_lod/streaming.cpp` and `blas_pass.cpp`;
- the deferred items of [mg_optimizations_fixes.md](../spec/fixesM1-6/mg_optimizations_fixes.md) §4.

| # | Milestone | Content | Done when |
|---|---|---|---|
| ST1 | GPU-only CLAS/BLAS build pipeline | The deferred half of MG-P6: CLAS and BLAS built into implicit destinations in a reusable transient buffer, sized on the GPU. A GPU prefix sum and allocator place them, and `MOVE_OBJECTS` (`NO_OVERLAP`) moves them into persistent storage. Moves use `vk_lod_clusters`' persistent CLAS allocator, so a CLAS moves once after its build. Simple compaction is not used: it moves resident CLAS on every unload and is incompatible with BLAS caching. The model load path (M9 X4) moves onto it, so a load needs no `waitIdle` and no host size readback | a model loads and unloads during the M10 camera path with no frame over budget, no CPU readback and no `waitIdle` (Nsight Systems). CLAS memory within 5% of the exact sizes |
| ST2 | Geometry streaming per LOD group | `vk_lod_clusters`' four tasks: request, resident table, storage and update. The LOD traversal appends missing groups to a request list, and an age filter appends groups not used for a while to an unload list. The host reads the request one frame late and caps loads by a per-frame limit and a memory budget (`VK_EXT_memory_budget`, §3). It uploads the groups' GDeflate pages from M9's cache on the async queue, and an update task patches the group addresses and builds the new groups' CLAS through ST1. For rays, positions are uploaded only for the CLAS build and not kept: hits use position fetch (§10.2), which is RTXMG's position stripping, and MG-P4 already removed position loads from hit decode. Raster keeps positions with the persistent group data. A group's blob starts 16-byte aligned (the sample's rule) | a scene larger than the floor card's 8 GB streams on the M10 paths with no frame over budget. Missing groups render at their resident coarser level, never as holes. `ENGINE_RT_CHECK` and `ENGINE_CULL_CHECK` pass while streaming. Load latency and PCIe bytes per frame are recorded |
| ST3 | Hit-based streaming for rays | `hitbased_streaming.md`. Rays reach geometry the camera traversal does not predict (shadows, reflections, indirect), so every resident group stores its last hit frame and ray-cone width in one `uint32`, written with one `atomicMax` from the hit decode. A per-frame pass propagates hits to coarser groups one LOD level at a time. A missing finer group is requested only where its coarser group was hit with a narrow enough cone. Shadow rays register hits from the primary surface only, and random-direction samples register none. In the engine, the "closest-hit" is the ray-query hit decode in compute (§11.1); with SER (V1) it is the minimal closest-hit | off-screen reflections and shadows keep their detail on the M10 paths at a lower resident size than ST2 alone (both recorded) |
| ST4 | Texture mip streaming | The coarsest mips (the mip tail) of every texture stay resident. Finer mips are requested from a GPU-written per-texture minimum-mip buffer, filled by the resolve's `SampleGrad` footprints and by ray-cone LOD at hits, so reflections and GI request the mips they sample. Uploads go through M9's KTX2 + GDeflate path (§6.4). A mip that is not yet resident is clamped through the descriptor's mip range, never sampled undefined. There is no NVIDIA reference for this in `reference/`: the step spec picks one first (reference-first rule) | Bistro at full texture detail with a texture budget below its full size; no frame over budget while mips arrive; the clamp is visible in a debug view |

Rules:
- Streaming changes residency, never the image contract. A frame whose groups or mips are missing renders the coarser resident data, and the check runners compare against a fully resident run.
- Per-frame limits and the memory budget are settings, logged with every benchmark run. The sample's defaults are not recommendations, per its own docs.
- RT and raster stream from the same residency table. The RT cut may want groups the raster cut does not (ST3), but no ray traces a group whose CLAS is not built. The §10.4 hybrid rule holds inside the frustum.
- 7-bit CLAS truncation (reverted MG-P2) and the large-world PTLAS items (re-centering the grid, streaming partition allocation) are re-measured here, as residency levers.

### 16.8 Post-1.0 target: game-like hybrid Ray Tracing with Cleanest Image Quality toggle

Starts after M10 (added 2026-11-25). Separate from 1.0's FPS-first unified RTXPT / RTXDI / RTXGI path-tracing modes: builds a production game-engine style hybrid ray-tracing mode and a dedicated **Cleanest Image Quality** toggle that prioritizes rock-solid temporal stability and noise-free output under DLSS-RR (inspired by `reference/RTXPT-main`'s `IntroSample` hybrid progression and `Rtxpt` path-space guide decomposition / PSR).

| # | Milestone | Content | Done when |
|---|---|---|---|
| HQ1 | Game-like hybrid RT mode | Selective game-style ray-traced passes over the visibility buffer: raster primary surface + deterministic/analytic dominant lights + selective RT shadows, glossy reflections, and bounded-cost diffuse GI (NRC or SHaRC), tunable per effect like a shipping AAA hybrid renderer | hybrid mode toggles cleanly at runtime beside the 1.0 path-tracing presets; per-pass timings and quality recorded on Sponza and Bistro |
| HQ2 | Full DLSS-RR guide fidelity (PSR + reflection MVs + hit distances) | Primary Surface Replacement (PSR) for mirror/smooth reflections (`reference/RTXPT-main` `Docs/Design.md` §Path Trace Pre-Pass), specular reflection motion vectors (`pInMotionVectorsReflections`), diffuse and specular hit distances, and clean path-space guide blending for glass and glossy surfaces | `ENGINE_DLSS_CHECK` gains guide-sensitive moving-reflection tests (closes `C-I14-7`); reflection stability under camera motion matches or beats RTXPT |
| HQ3 | **Cleanest Image Quality** toggle | One-click quality toggle combining uncorrelated low-variance inputs, deterministic dominant lights, full DLSS-RR guides (HQ2), adaptive/multi-sample budget on high-variance edges/interiors, and zero ReSTIR boiling/popping — judged on static and moving `Lit` temporal stability (`cafe.tables`, `cafe.dining`, `sponza`, `bistro.street`) | static and moving `Lit` temporal std and flicker reach near DLSS-RR's clean-input floor across Sponza and Bistro café without bias regressions, with frame cost documented against the 1.0 FPS-first target |

Rules:
- Lives behind runtime toggles beside the 1.0 path-tracing modes; never slows down or complicates the 1.0 FPS-first path when disabled.
- Every guide and sampling upgrade is verified against the check runners and recorded with ABBA GPU timings.

---

## 17. Superseded decisions

| Source | Old decision | Now | Why |
|---|---|---|---|
| PBR plan §2 #2, E1 | raster G-buffer from the mesh pass | visibility buffer + compute resolve writes the same guide layout (§8.3, §9) | end_goal; per-pixel shading cost, cheap two-phase raster |
| PBR plan §2 #3, §8 | ray queries only; no RT pipeline, SBT, SER, cluster AS | ray query for visibility; every pass that shades hits is an SER RT pipeline with a minimal SBT; Mega Geometry for all RT geometry (§10, §11.1). **2026-11-17:** SER shelved for 1.0 (C-G1-3); 1.0 shades hits from compute ray queries, SER returns as a toggle in post-1.0 V1 (§11.1, §16.5). Any-hit stays forbidden | built around SER; end_goal. Shelving: Slang EXT `HitObject` gaps |
| PBR plan §2 #11 | NRC only, SHaRC out | both behind `IRadianceCache`, chosen per tier (§11.4); **reversed 2026-11-14:** NRC only again | end_goal; floor-tier cost. Reversal: NVIDIA-only target |
| PBR plan §2 #12, §9 | `Uniform` local lights, emissive meshes out of v1 | emissive triangles as RTXDI lights, `Power_RIS` (§11.2) | full RT lighting includes emissives |
| PBR plan A3 | delete RT pipeline, SER, RT maintenance1, cluster/partitioned AS, SHaRC and mega-geometry stubs | keep the extensions. Deleted 2026-10-31: `geometry/mega_geometry`, `geometry/mega_tess`, `passes/mega_select`, `passes/mega_tess`, `gi/nrc`, `passes/nrc_pack`, `passes/gbuffer`. Kept: `gi/sharc`. The stubs kept back then (`gi/rtxgi`, `raytracing/*`, `passes/pathtrace*`, `passes/sharc_resolve`) were deleted 2026-11-12 (AU7) and return with M2–M4 | needed by §10–11; no empty files (AU7) |
| PBR plan C1 | one KHR triangle BLAS per mesh primitive from the model slices | CLAS per cluster + cluster BLAS (§10.2–10.3); no triangle BLAS | RT built around cluster LOD, as in `vk_lod_clusters` |
| PBR plan C2 | CPU-written TLAS, full rebuild | GPU-written instances + full GPU TLAS build. **Since 2026-11-14:** PTLAS updated in place from GPU-written ops, required (§10.3, PBR C4) | zero CPU per-object work (§1.1 #2); matches the sample. PTLAS: the full Mega Geometry stack is the target |
| PBR plan C3 | hit → primitive → index buffer → vertices | hit → cluster → triangle; positions by position fetch (§10.1–10.2) | one decode path for vis buffer and rays |
| PBR plan D1–D2 | OMM per primitive in index-buffer order | OMM array per primitive with a per-cluster index buffer, baked per LOD level (§10.5) | clusters reorder triangles |
| shader_architecture_plan §7 "closed" | G-buffer over vis buffer; mega geometry and RT pipelines dropped | reversed as above | end_goal |
| shader_architecture_plan §1 | `MeshPushData` 24 B | 16 B (already in code) | stale doc |
| meshlet_culling_options decisions | raster target 5A forward; LOD none | 5C vis buffer now; 7C cluster DAG after the final goal | end_goal |
| meshlet_culling_options 3C | meshlet candidate buffer re-tested in phase 2 | instance candidates kept; meshlet candidates replaced by per-group reject bitmasks (§8.1) | `vk_lod_clusters` alignment ([culling_comparison.md](../reference/culling_comparison.md) A4) |
| meshoptimizer.md backlog #8 | cluster LOD "only if Mega Geometry returns" | planned after the final goal (§16.3) | Mega Geometry returns; LOD follows the Bistro goal |
| meshoptimizer.md, spec §6.3 / §7.1 (2026-11-04) | `meshopt_buildMeshlets`, `cone_weight = 0.25`, `meshopt_optimizeMeshlet` level 0 only for OMM corner order | `clusterlod.h` level 0: Flex builder, `cone_weight = 0`, `optimizeMeshletLevel` 1; OMM baked from the final cluster triangles (§7.1) | clusters must not change when LOD levels are added |
| spec §8.2 (2026-11-04) | opaque single-sided / double-sided buckets by cull mode | opaque / alpha-tested bins, two-sided by winding flip (§8.1 A6) | matches `vk_lod_clusters` `rasterBinning`; one fewer DGC execute per phase |
| resource_architecture.md morph and skin streams | morph deltas reachable, default-weight morphs applied in the mesh shader; joints/weights uploaded | removed (§7.3). **2026-11-23:** they return post-1.0 as GPU deformation into per-instance streams, with cluster templates for rays (§16.6 AN2–AN3), not as mesh-shader morphs | no animation in 1.0; post-1.0 animation must also be ray traced |
| spec §7.3 (2026-11-04) | no animation planned | 1.0 static; animation post-1.0 on Mega Geometry cluster templates (§16.6) | 2026-11-23: one CLAS path in 1.0, built so that animation and streaming extend it (I10 decision) |
| spec §1.2 (2026-10-31) | geometry and texture streaming a side project, out of this spec | out of 1.0 only; post-1.0 §16.7 (ST1–ST4) | 2026-11-23: the LOD and CLAS data model is the streaming unit of `vk_lod_clusters` |
| spec §11.2 (2026-11-21, M7 F1) | `ReGIR_RIS` default (NVIDIA `Medium`) | `Power_RIS` default; `ReGIR_RIS` callable (§11.2) | M8 I6 F12: ReGIR's presample costs 0.44 ms and stalls the ray start on Bistro, with no stability gain |
| spec §7.1 (2026-11-07) | `meshopt_buildMeshletsFlex` (min = max / 3, split factor 2) | `meshopt_buildMeshletsSpatial` (min = max / 3, fill 0.75; §7.1) | M8 I10 MG-P3: −7 to −13% `RaySum` |
| end_goal §2 | `VK_EXT_descriptor_buffer` / `mutable_descriptor_type` | `VK_EXT_descriptor_heap` | engine already migrated; descriptor heap replaces descriptor buffer |
| end_goal §3 | Hi-Z from "previous frame's" depth with previous transforms | as built: previous pyramid with previous matrices, no depth reprojection | [meshlet_culling_options.md §8](./meshlet_culling_options.md#8-review-proposed-4-tier-pipeline--visibility-buffer) |
| spec §1.1, §11.2–11.7 (2026-11-20 / 2026-11-24) | 1.0 target = ReSTIR DI + ReSTIR GI (or NRC → ReSTIR GI), NRC-only cache (SHaRC dropped 2026-11-14), fixed 3-way `IndirectMode` | **2026-11-25:** one unified Vulkan RTXPT + RTXDI + RTXGI analog via composable runtime toggles beating NVIDIA demos in FPS. 1.0 target stack: DLSS-RR + RTXDI Streaming RIS *or* Temporal-only `Basic` ReSTIR DI (`Max History = 4–8`, Spatial OFF) + raw uncorrelated 1-bounce PT (1 diffuse-or-specular ray/px, OMM, SER when Slang adds it, ReGIR or simple/`Power_RIS` NEE) + NRC default with SHaRC toggle. ReSTIR GI kept as runtime toggle (off in target) | ReSTIR GI and spatial/long-history ReSTIR DI feed temporally correlated blotches to DLSS-RR and burn GPU time; refocusing on beating NVIDIA RTXPT / RTXDI / RTXGI demos in FPS via modern Vulkan + toggles ([spec/m8/README.md](../spec/m8/README.md)) |
| spec §16.2 M8 (2026-11-20) | M8 = `spec/fixesM1-6/` optimization and fixes (I1–I16) | **2026-11-25:** M8 = [spec/m8/README.md](../spec/m8/README.md) (M8.1 pipeline audit, M8.2 ImGui audit, M8.3 composable toggles, M8.4 SHaRC toggle, M8.5 target stack & Vulkan perf / beat NVIDIA demos FPS, M8.6 floor tier & verification); `spec/fixesM1-6/` stays as completed M1–M7 fixes history | refocus on audited NVIDIA-parity unified architecture and FPS target; I16 ReSTIR boiling hacks superseded, I11/I12 folded into M8.5–M8.6 |

---

## 18. Risks and open questions

| ID | Item | Impact | Resolution |
|---|---|---|---|
| R3 | DLSS-RR cost on Turing | may consume most of the floor budget | 5070 measured (1.14 ms at 853×480 → 1280×720); measure on the 2060 Super (C-M0-1, M8 I11) |
| R4 | NRC on Turing, CUDA runtime dependency | since 2026-11-20 only the NRC comparison modes depend on it (§11.7); the shipped ReSTIR GI path does not | the pinned SDK names no minimum GPU but can return `UnsupportedHardware`; 5070 measured (0.62 ms in G3); check on the 2060 Super and measure the cache row of §2.3 there (C-G3-2, M8 I11) |
| R5 | 8 GB VRAM on the floor card | benchmark scene size is limited | choose scenes that fit; LOD, BC textures and quantized streams (§6.3–6.4) |
| R6 | No hardware SER/OMM before Ada | indirect pass slower on Turing/Ampere | floor runs indirect at half resolution (§11.3) |
| Q3 | Vis buffer 32 vs 64 bit | 32-bit chosen (§8.3); real-runtime pros and cons not measured yet | measure resolve and raster cost after M1 on both tiers; switch only if 64-bit wins and L1's atomic path stays possible |
| Q7 | `VK_KHR_ray_tracing_position_fetch` on the 2060 Super | without it, hit decode needs a persistent position copy | required since PBR C1 (supported on the RTX 5070, driver 621.1); the 2060 Super is not in the dev machine: if it lacks it, device creation names it as missing |
| Q8 | Slang access to `ClusterID` and attributes on `HitObject` | decides whether SER passes need the minimal closest-hit invoke | closed 2026-11-16 (Slang 2026.22 probe, G1 SER4): `GetClusterID` is NV-only and Slang has no SPIR-V `ClusterID()`, so a minimal closest-hit reads `ClusterIDNV` (4 B payload); everything else comes from the EXT hit object ([g1_ser_rt_pipeline.md](../spec/m4/g1_ser_rt_pipeline.md) SER4) |
| R7 | After §16.3: all LOD levels' CLAS resident (no streaming) on 8 GB | limits scene size on the floor card | until §16.7: `positionTruncateBitCount`, BLAS sharing/merging/caching, scene choice (R5); then geometry streaming (ST2, ST3) |
| R9 | After §16.6: per-instance vertex streams, CLAS and BLAS for every playing animated instance | VRAM and build time grow with the animated crowd, most on the 8 GB floor card | templates size instantiations exactly (271 against 564 MB in `vk_animated_clusters`); static instances share; truncation re-measured (AN3); crowd size per tier from the AN3 numbers |
| Q11 | After §16.6: culling bounds of deforming clusters | bind-pose boxes and cones are not conservative under deformation | AN2: per-frame or padded boxes, cone test off for deforming clusters; held by `ENGINE_CULL_CHECK` on animated poses |
| Q12 | After §16.7: texture streaming reference | no NVIDIA texture-streaming sample in `reference/` | picked before the ST4 spec |
| R8 | After §16.3: raster/RT LOD mismatch | self-shadowing, leaks at primary hits | §10.4 rule + debug view |
| Q9 | GDeflate ratio on BC7/BC5 data | decides how much of the decompression gain is textures vs geometry | §6.4, §7.2; measured in M9 |
| Q10 | PTLAS partitioning: grid cell size, the global partition's trace cost, and how long the wait for the previous frame's ray passes holds the async compute queue | trace time and the X2 overlap | PBR C4 measured Sponza and the instance fields: ~64 instances per grid cell, no global partition, no trace-time difference between still and moving frames; an all-moving scene updates slower than a full build. Again on Bistro (M10) |
| Q4 | DGC with RT stages: driver 621.01 reports none | ray dispatch uses `vkCmdTraceRaysIndirect2KHR` | recheck per driver |
| Q5 | Slang drops `NonUniform` on heap access | UB by spec, tolerated by NVIDIA | re-probe on every Slang upgrade (S6) |
| Q6 | RTXDI / NRC HLSL headers through Slang | bridge files may need HLSL shims | verified for RTXDI 3.2 and NRC 0.16 in M3/M4 (RTXDI units need `-warnings-disable 30081`, C-F2-2; NRC compiles clean); re-verify on SDK upgrades |
| P-AMD | ~~NVIDIA-only pieces for a future AMD port~~ | — | closed 2026-11-14: NVIDIA RTX only, no AMD port planned. SDK code still lives in its own module (`vk_nrc`, `vk_dlss`, `vk_mega_geometry`) for ownership, not portability |

---

## 19. Reference map

Local notes are in [reference/](../reference/); sources are under `reference/` or on GitHub.

| Subsystem | Reference | Local notes |
|---|---|---|
| Vulkan 1.4 plumbing, descriptor heaps, push data | nvpro-samples/vk_minimal_latest | [vk_minimal_latest_comparison.md](../reference/vk_minimal_latest_comparison.md) |
| DGC | nvpro-samples/vk_device_generated_cmds | [dgc_comparison.md](../reference/dgc_comparison.md), source in `reference/vk_device_generated_cmds-master` |
| Meshlet culling and tuning | nvpro-samples/gl_vk_meshlet_cadscene | [gl_vk_meshlet_cadscene_reference.md](../reference/gl_vk_meshlet_cadscene_reference.md) |
| Cluster LOD, raster + RT, BLAS sharing / merging / caching, SW raster, culling | nvpro-samples/vk_lod_clusters (Apache-2.0; license in `ThirdParty/licenses`, ported files keep the NVIDIA header and a "modified" note) | [culling_comparison.md](../reference/culling_comparison.md); source in `reference/vk_lod_clusters`; `docs/blas_*.md`, `docs/lod_generation.md`, `docs/raster_twopass_culling.md` |
| Partitioned TLAS | nvpro-samples/vk_partitioned_tlas | — |
| Animated geometry, cluster templates (post-1.0 §16.6) | nvpro-samples/vk_animated_clusters | [mega_geometry_comparison.md](../reference/mega_geometry_comparison.md) F8, §4; source in `reference/vk_animated_clusters` |
| Geometry streaming, GPU CLAS allocation, hit-based streaming (post-1.0 §16.7) | nvpro-samples/vk_lod_clusters (`scene_streaming*`, `stream_*.comp.glsl`), NVIDIA-RTX/RTXMG (`cluster_lod/streaming.cpp`, `blas_pass.cpp`) | `reference/vk_lod_clusters/docs/streaming.md`, `hitbased_streaming.md`, `clas_allocation.md`; [mg_optimizations_fixes.md](../spec/fixesM1-6/mg_optimizations_fixes.md) §4 |
| Tessellated clusters | nvpro-samples/vk_tessellated_clusters, NVIDIA-RTX/RTXMG | — |
| RT pipeline, SER, ray query in Slang | nvpro-samples/vk_raytracing_tutorial_KHR (`v2`) | [vk_raytracing_tutorial_v2_reference.md](../reference/vk_raytracing_tutorial_v2_reference.md) |
| glTF PBR, textures | nvpro-samples/nvpro_core2, vk_gltf_renderer | [nvpro_core2_textures_pbr.md](../reference/nvpro_core2_textures_pbr.md) |
| Full RT lighting stack integration (Streaming RIS, ReSTIR DI, 1-bounce PT, SHaRC, DLSS-RR, SER, OMM, PSR) | NVIDIA-RTX/RTXPT v1.9.0 (`Rtxpt` + `IntroSample`) | local source in `reference/RTXPT-main` (`Docs/Design.md`, `Docs/intro-sample.md`, `Rtxpt/Sharc/`, `Rtxpt/Shaders/IntroSample/`) |
| Direct + indirect resampling | NVIDIA-RTX/RTXDI | local source in `reference/RTXDI` |
| Radiance caches (NRC & SHaRC) | NVIDIA-RTX/RTXGI (NRC 0.16, SHaRC) | local source in `reference/RTXGI`, [NrcGuide.md](../reference/NrcGuide.md), `reference/RTXGI/Docs/SharcGuide.md`, `ThirdParty/_deps/src/sharc` |
| Neural shading / textures | NVIDIA-RTX/RTXNS, NVIDIA-RTX/RTXNTC | — |
| Reconstruction | NVIDIA/DLSS (NGX) | — |
| GPU decompression, GDeflate | `VK_EXT_memory_decompression`; GDeflate reference codec in microsoft/DirectStorage | [VK_EXT_memory_decompression.md](../spec/VkExtensionsSpecs/VK_EXT_memory_decompression.md) (its "transfer queue" advice is wrong, §3); §6.4, §7.2 |
| Geometry processing, cluster DAG | zeux/meshoptimizer (`demo/clusterlod.h`) | §6.3, §7.1 |

---

## 20. Architecture audit (2026-11-10)

**Scope:** `src/`, `shaders/`, `tests/`, the build files and every design document, at `bfb8d1587` plus the working tree. Five read-only summary passes covered the code; items marked *checked* were re-read in the source afterwards, the rest is as the passes reported it.
**Not covered:** the per-extension files in `spec/VkExtensionsSpecs/`, the ImGui panel bodies in `vk_engine.cpp`, and `reference/`.
**Shader code:** audited separately, line by line, for correctness and performance in [shader_audit.md](./shader_audit.md) (2026-11-11).

### 20.1 What conforms

| Area | State |
|---|---|
| Frame graph | two-phase cull → explicit DGC preprocess → vis-buffer raster → Hi-Z → resolve → debug view → present, in the order of §5 up to stage 9 |
| Barriers | sync2 with narrow stage and access masks. `ALL_COMMANDS` appears only on GPU-timer timestamps, deliberately |
| GPU lists | every cull list has a capacity check, an overflow counter and a UI row. DGC sequence writes stay in range because the visible count is clamped to the bin capacity (*checked*, `meshlet_cull.slang:386`) |
| Binding | BDA and descriptor-heap indices only (P3); one frame root (P4); push data checked against `maxPushDataSize` at startup |
| Mirrored structs | every struct in `types.slang` matches its C++ mirror in order, size and offsets (hand comparison) |
| Device creation | one table enables each required feature and names it when a GPU lacks it; missing requirements fail at startup |
| Shader sampling | no implicit-LOD `Sample` outside fragment shaders (S10); no barrier in non-uniform control flow; no wave-size assumption |
| Readbacks | cull counters and GPU timers are read after the slot's fence (X6) |

### 20.2 Findings and decisions

| ID | Finding | Rule broken | Decision | When |
|---|---|---|---|---|
| AU1 | **Scene changes stall the device.** `waitIdle` on every load, unload, instance spawn and instance-field clear (`vk_engine.cpp:1306, 1316, 1348, 1407, 1438`); `submitAndWait` (queue wait) per upload, buffer growth, compaction and attachment creation. No deferred destruction. One staging buffer allocated per batch. One-time work records into frame slot 0's command buffer. The compute queue is created and never used | §13.1: `waitIdle` only at startup, shutdown and swapchain recreation | Add a retire queue keyed by the slot fence, and a command pool of its own for one-time work. Acceleration structures use the queue from their first commit; buffer growth and unload move onto it. The remaining stalls go with background loading | optional; was: queue in M2, rest in M9 (X3–X5) |
| AU2 | **`ResourceManager` does too much** (about 1,450 lines): fences, semaphores and command pools; the depth attachment; asset upload, growth and compaction; the model catalog; material and light packing; frame-root and node-instance writes; camera buffer creation. It also stores state that the cull pass and the render targets write into it, and it fills `Camera`'s buffers, so `core` and `scene` include each other | separation of concerns | Split into frame sync (fences, semaphores, pools), asset buffers (slices, growth, compaction, catalog) and frame data (root, node instances, object rows). Passes hand their addresses to the frame-root writer through the renderer. `Camera` creates its own buffers. Plan: [resource_manager_split.md](../spec/features/done/resource_manager_split.md) | done (2026-11-12): `AssetBuffers`, `FrameData`, `FrameSync` |
| AU3 | **`Engine` does too much** (about 1,500 lines, roughly 900 of them UI panels): window, input, ImGui setup, asset scan, load sequencing, init and teardown order | separation of concerns | Split in two: `Engine` keeps the application and the scene actions, `EngineUi` gets everything ImGui and asks for scene actions through a request struct. Plan: [engine_ui_split.md](../spec/features/done/engine_ui_split.md) | done (2026-11-12): `EngineUi` in `src/ui/engine_ui.cpp` |
| AU4 | **Per-frame CPU work is O(entities).** Node instances and object rows are rewritten in full every frame, and the model matrix is computed twice per entity (`vk_frame_data.cpp:119, 180`). `ObjectStorage::erase` shifts every later entity id, so light rows are rewritten on unload | P2, goal 2, §5 stage 1 | The scene-update pass gets a milestone: a CPU list of changed entities feeds a GPU pass that writes `GpuNodeInstance` and keeps the previous matrix. The RT instance write of M2 reads the same rows | optional; was M2 |
| AU5 | **Limits and constants.** Cluster size (64 vertices, 126 triangles) is hardcoded and never checked against the mesh-shader limits. Shader variants are preprocessor-only (`CULLING_REVERSED_Z`, `GI_CACHE_MODE`). Shared constants are copied by hand between `types.hpp` and `types.slang`. No test compares Slang layout with C++. Storage images carry no format attribute | §3 limits, S9, S11, §7.1 | Cluster sizes stay fixed at 64 / 126: they follow NVIDIA's meshlet recommendation, not a device query, and are not asserted. ~~Delete `CULLING_REVERSED_Z` and the forward-Z branch; the Hi-Z footprint test keeps the reverse-Z case~~ (done). ~~`GI_CACHE_MODE` goes with the SHaRC code~~ (done in G3, 2026-11-17: considerations C-G3-1). ~~Add a test that compares the reflected layout of `types.slang` with the C++ sizes and offsets~~ (done: `tests/layout_test.cpp` reflects every struct of `types.slang` through the Slang library and compares size and every field offset with its C++ mirror). ~~Add format attributes to the storage-image helpers~~ (done: `storageTex2D<T, format>` in `core/binding.slang` types each storage image as `RWTexture2D<T, 0, format>`, since `[format]` has no effect on heap handles; the layout test checks the format constants) | done (2026-11-13) except cluster sizes; `GI_CACHE_MODE` removed 2026-11-17 |
| AU6 | **`assets_loader.cpp` is 2,029 lines** holding accessor decode, JSON and base64, a thread helper, texture scheduling, material parsing, scene graph and lights, mesh preprocessing and orchestration. The image-use dry run must mirror the real material parse. The color-space rules per texture slot are duplicated in `tools/gltf_ktx2` | DRY | Share the slot color-space rules between the loader and the tool now (§6.4 needs one encoder anyway). Split the file into accessor decode, material parse, geometry build and orchestration when loading moves to workers | rules done (2026-11-15): `src/core/texture_color_space.hpp`; split optional in M9 |
| AU7 | **Dead scaffolding.** Zero-byte sources, some still compiled: `vk_pbr.*`, `vk_postprocessing.*`, `vk_mesh.*`, `maths.cpp`, `vk_shaders.*`. Stub shaders: `base.slang`, `base/mesh.slang` (declares `module mesh`, as `geometry/mesh.slang` does), `raytracing/hit`, `raytracing/trace`, `passes/composite`, `passes/pathtrace*`, `passes/sharc_resolve`, `gi/rtxgi` (imported only by the `pathtrace` stub). Unused code: the descriptor-set members and binding mode of `DescriptorManager`, `GpuUniformBufferObject`, `EngineSettings`, `intersectSize` in `culling.slang` | YAGNI | Delete all of it. A file returns with the milestone that fills it. This reverses the §17 row that kept `gi/rtxgi`, `raytracing/*`, `passes/pathtrace*` and `passes/sharc_resolve` as empty files; `gi/sharc.slang` stays, the SHaRC test includes it. **Done 2026-11-12** ([dead_code_removal.md](../spec/features/done/dead_code_removal.md)); also deleted as orphans: `raytracing/payload`, `postprocessing/dlss`, `EngineContext`, the `ShaderObjects` descriptor-manager reference | before M2 |
| AU8 | **Build.** Five shared engine libraries each link the static third-party implementation library, so VMA, stb and tinygltf are compiled into every DLL. Coverage instrumentation defaults to on | — | Link the implementation library into `engine_util` only and export it. Coverage defaults to off | done (2026-11-15): whole-archive into `engine_util`, auto-exported; coverage OFF by default |
| AU9 | **NVIDIA-only code placement.** `nrc_library` is compiled into `engine_render`, and only a test uses it | §18 P-AMD (closed) | Leave it until NRC is integrated, then it becomes the `vk_nrc` module | done in M4 G3: `vk_nrc` built (`nrc_library` stays alongside it for the linkage test) |
| AU10 | **Swapchain recreation has two paths.** Every resize event rebuilds at once (`vk_engine.cpp:416`), several times during a drag, and the renderer rebuilds again on out-of-date. No old swapchain is passed. ImGui's image count and format are updated only on the engine path (*checked*, `vk_engine.cpp:1267`) | — | A resize sets a flag; one recreate function runs once per frame, passes the old swapchain and updates ImGui | done (2026-11-15): `Renderer::requestSwapchainRecreate`, `Engine::recreateSwapchain` |
| AU11 | **Test coverage.** Tests cover `core/culling` footprints, the BRDF, the SHaRC round trip and library linkage. Nothing tests the cull passes, the vis-buffer decode, the resolve output or layouts | S5 | The layout test of AU5. Pass-level tests are not planned; the debug views and counters are the check | done (2026-11-13) with AU5 |

### 20.3 Defects

| Defect | Location | Status |
|---|---|---|
| Embedded glTF images are cached under model directory + image index, so two `.glb` files in one folder share textures | `assets_loader.cpp:1120` | fixed 2026-11-13: keyed by the resolved model file path |
| The window is destroyed and SDL shut down before the device that owns the surface | `vk_engine.cpp:1504–1510` | fixed 2026-11-13: the device goes first |
| The init loop over both color spaces assigns one variable, so only the linear format's host-copy layout is kept | `texture_manager.cpp:226–228` | fixed 2026-11-13: init throws if the two formats need different layouts |
| `MaterialStore::release` guards the refcount with an assert only; it underflows in release builds | `material_store.hpp` | fixed 2026-11-13: every row is checked before any refcount changes; a row not held throws `std::logic_error` |
| An exception during an upload leaks the staging allocation and leaves the command buffer recording | `vk_asset_buffers.cpp:371–442` | fixed 2026-11-13: a scope guard frees the staging and any uninstalled grown buffers and resets a still-recording command buffer |
| `flushGpuAssets`, `instantiateModel` and `eraseEntity` can throw out of the main loop; only `loadModel` is caught | `vk_engine.cpp:1325, 1358` | fixed 2026-11-13: `Engine::applyUiRequests` catches every scene action, reports it in the UI and resyncs the frame data |
| `destroyTexture` dereferences a map lookup without checking it | `texture_manager.cpp` | fixed 2026-11-13: a missing image is logged and the heap slot is still freed |

Checked and dismissed: the debug view's barrier is sufficient (resolve writes only the guides, the debug view is the only writer of the display image), and a failed load does return its material rows (`flushGpuAssets` calls `discardModel`).

### 20.4 Document set

Root cause of the drift: the same facts (vis-buffer format, the §9.3 guide table, the SER design, the culling rules, extension lists) are restated in three or four documents, each with its own status marker.

**Ownership from now on:**

| Fact | Owner | Other documents |
|---|---|---|
| Milestones, status, decisions | this spec, §16–§18 | link here; carry no status of their own |
| Struct sizes and layouts | the `static_assert`s in `types.hpp` | [resource_architecture.md](../spec/resource_architecture.md) describes the model; any size it quotes must match the asserts |
| Shader files and passes | [shader_architecture_plan.md](../spec/shader_architecture_plan.md) | — |
| Task detail per feature | PBR plan, `spec/features/*` | — |

**Fixes to make:**

| Document | Fix |
|---|---|
| [resource_architecture.md](../spec/resource_architecture.md) | ~~`GpuFrameRoot` is 176 B (it says 128), `GpuCullView` is 288 B (272). Remove the standard-Z, "no render-target slots", state-group and `mesh_debug` text; M1 is built~~ done 2026-11-13: sizes, frame-root fields, targets, raster bins, frame order and §12 match the working tree |
| [shader_architecture_plan.md](../spec/shader_architecture_plan.md) | ~~`resolve`, `present` and `debug_view` exist; Hi-Z is 3 levels per dispatch; the open-decisions table still lists the closed Q8~~ done 2026-11-13: M1 marked built, pass and module statuses updated, Q8 moved to closed |
| `shaders/README.md` | ~~directory names are `geometry/`, `raytracing/`, `postprocessing/`; `base.slang` goes (AU7)~~ done with AU7: the table lists the directories that exist |
| `README.md` | ~~the binding mechanism is `VK_EXT_descriptor_heap`; the Linux claim has no backing~~ done 2026-11-13: heap for images and samplers, BDA for buffers; Windows only, Linux untested |
| [vulkan_extensions_reference.md](./vulkan_extensions_reference.md), `spec/VkExtensionsSpecs/README.md` | ~~`VK_EXT_shader_object` is the pipeline model; OMM and SER properties are cached; cluster AS becomes required in M2~~ done 2026-11-13; dead `vk_pipeline.cpp` link points to `vk_shader_objects.cpp` |
| `spec/vulkan_integration_playbook.md` | ~~a copy of the `vulkan-dev` skill, not a project document: delete~~ done 2026-11-13 (`git rm`); its link in the extensions reference checklist removed |
| `rendering_engine_idef0.md` | describes a state that no longer exists (MSAA, CPU-written sequences, `.obj` startup); it is ignored by git and stays out of the document set |
| PBR plan, `pbr_lighting_materials.md`, `tests/brdf_test.cpp` | ~~links to `spec/pbr_lighting_materials.html` and `spec/pbr_figures/` must point to `docs/`~~ done 2026-11-13; the html's own links back to `spec/` fixed too |

**Referenced but not in the repository:** `docs/end_goal.md`, `docs/meshoptimizer.md`, `docs/texture_pipeline_plan.md`, `docs/data_layout_optimization_spec.md`, `spec/gltf_parse_resources.md`. ~~Restore or fold~~ done 2026-11-13: none was ever in git, so they are folded, not restored. Their links are gone; §6.3, §6.4, §7.1 and §7.2 are the only statement of those plans, and the PBR docs cite glTF 2.0 §3.9 and Appendix B plus the Khronos extension specs instead of `gltf_parse_resources.md`. Mentions in §17 and the changelog stay as history.

**Subsystems with no document:** the texture pipeline, DGC internals, swapchain and frame sync, the asset loader, the build and `engine_add_slang_pass`. Each gets a section here or a feature file when its milestone starts; none is written ahead of need.

---

## Sources for §2.2

- [Khronos: VK_EXT_ray_tracing_invocation_reorder](https://www.khronos.org/blog/boosting-ray-tracing-performance-with-shader-execution-reordering-introducing-vk-ext-ray-tracing-invocation-reorder): reorder is a no-op without hardware support
- [Tom's Hardware: hardware SER since RTX 40](https://www.tomshardware.com/pc-components/gpus/microsoft-adds-shader-execution-reordering-ser-in-latest-directx-sdk-for-more-efficient-ray-tracing-intel-arc-b-series-gpus-show-90-percent-performance-uplift)
- [NVIDIA OMM SDK](https://developer.nvidia.com/rtx/ray-tracing/opacity-micro-map/get-started): any RTX GPU, hardware-accelerated on Ada
- [NVIDIA blog: descriptor heaps](https://developer.nvidia.com/blog/streamlining-resource-binding-with-end-to-end-support-for-vulkan-descriptor-heaps/): driver 610+
- [RTXNS](https://github.com/NVIDIA-RTX/RTXNS): `VK_NV_cooperative_vector` minimum RTX 20

## Changelog

| Date | Change |
|---|---|
| 2026-11-25 | Refocused 1.0 goal and new **M8** ([spec/m8/README.md](../spec/m8/README.md), superseding [spec/fixesM1-6](../spec/fixesM1-6/README.md)): one unified modern Vulkan 1.4+ renderer covering RTXPT, RTXDI, and RTXGI via composable runtime toggles and beating NVIDIA's demos in FPS. New 1.0 target lighting stack (§1.1, §2.3, §5, §11.2–11.7, §17): DLSS-RR + RTXDI Streaming RIS *or* Temporal-only `Basic` ReSTIR DI (`Max History = 4–8`, Spatial OFF) + raw uncorrelated 1-bounce PT (1 diffuse-or-specular ray/px, OMM, SER when Slang adds it, ReGIR or simple/`Power_RIS` NEE) + NRC default with SHaRC toggle (M8.4); ReSTIR GI kept as runtime toggle (off in target). M8 opens with M8.1 pipeline audit vs NVIDIA demos (`RTXPT-main`, `RTXDI`, `RTXGI`, `RTXMG-main`, `vk_lod_clusters`, `vk_denoise_dlssrr`, `vk_gltf_renderer`), M8.2 ImGui & settings wiring audit, M8.3 composable toggles, M8.4 SHaRC cache toggle, M8.5 target stack & Vulkan perf / NVIDIA demo FPS benchmark, M8.6 verification & floor tier. Added post-1.0 target §16.8: game-like hybrid Ray Tracing with Cleanest Image Quality toggle (HQ1–HQ3). Updated §19 reference map and synced `pbr_implementation_plan.md`, `pbr_lighting_materials.md`, `shader_architecture_plan.md`, `resource_architecture.md`, `docs/settings.md`, and `considerations.md` |
| 2026-11-23 | Post-1.0 animation (§16.6, AN1–AN4: GPU node animation, skinning and morphs, Mega Geometry cluster templates instantiated per frame, procedural foliage) and streaming (§16.7, ST1–ST4: GPU-only CLAS/BLAS builds, LOD-group geometry streaming, hit-based streaming, texture mips) added from `vk_animated_clusters`, `vk_lod_clusters` and RTXMG; §1.1, §1.2, §5, §6.4, §7.3, §10.1, §10.2, §13.1, §14, §17, R7, R9, Q11, Q12 and §19 follow. 1.0 stays static and resident |
| 2026-11-24 | NRC → ReSTIR GI is the default indirect mode on both tiers and the tuning target for DI and GI noise (§11.7, user decision, M8 I15) |
| 2026-11-23 | Consistency pass against the step specs (M6, M7, M8 I1–I15): M6 done, M7 built, M8 status per step (§16.1, §16.2); shipped ReSTIR DI (`Power_RIS`, I6) and ReSTIR GI (O11 profile, gated, I5) sets (§11.2, §11.5, §11.7); NRC bias found and gated in I7 (§11.4, §11.7); spatial meshlets (MG-P3, §7.1), split OMM CLAS and truncation result (MG-P1, MG-P2, §10.2), PTLAS layout and overlap (MG-P5, MG-P7, §10.3); material binning and emissive lights as built (§6.1, §9.1, §11.2); alpha bin built (§8.1, §8.2); glass in the frame overview (§5); I4 frame profile in §2.3; check runners, check scenes and the settings lab (§14); broken findings links fixed |
| 2026-11-23 | M8 I10 complete ([i10_mega_geometry_perf.md](../spec/fixesM1-6/i10_mega_geometry_perf.md)): 1.0 stays on CLAS (V2 / compacted triangle BLAS remains post-1.0 in §16.5 with I10 numbers recorded); unimplemented/reverted I10 levers useful for post-1.0 animations, streaming, and large tree worlds (MG-P2 truncation, MG-P5 explicit AABBs, MG-P6 GPU-only streaming builds, MG-S2 per-mesh BLAS merge, cluster templates, position stripping) catalogued in [mg_optimizations_fixes.md](../spec/fixesM1-6/mg_optimizations_fixes.md) §4 |
| 2026-11-21 | §9.4: `KHR_materials_pbrSpecularGlossiness` is read (Bistro, C-M7-1); `GpuMaterial` keeps 240 B, its pads now hold `specularFactor` and `glossinessFactor` |
| 2026-11-20 | Consistency pass: §11.7 separates the gated converged ReSTIR GI settings from the shipped ones (+8.1% / +1.7%); the floor-tier half-resolution indirect marked as target, not built (§5, §11.3, C-G2-1); §11.3 names the off-by-default X₂ options (O2, O6) |
| 2026-11-20 | Doc sync across `engine_spec.md`, `pbr_implementation_plan.md`, `shader_architecture_plan.md`, `resource_architecture.md` and `considerations.md`: updated M4 built status, §16.1/§16.2, §17 SER shelving, §11.6/§11.7 order, §1.1/§4 S6/§8.1 A6/§18 rows, actual shader/host layouts and struct sizes (`GpuFrameRoot` 192 B, `GpuTargets` 208 B, `GpuRtxdiConstants` 496 B, `GpuNrcConstants` 168 B) |
| 2026-11-20 | New milestone M8 "Optimization and fixes" (§16.2, [spec/fixesM1-6](../spec/fixesM1-6/README.md)): the RTXDI and NRC sample reviews merged into [m8_optimization_and_fixes.md](../spec/fixesM1-6/m8_optimization_and_fixes.md). Asset pipeline moves to M9, the 1.0 goal (Bistro) to M10; earlier changelog rows keep the old numbers |
| 2026-11-20 | ReSTIR GI ships NVIDIA's settings with ray-traced spatial bias correction (§11.5): bias −27% / −9% → +8% / +1.7% against the one-bounce ground truth, +0.17 ms |
| 2026-11-20 | 1.0 goal: the full RTXDI stack, ReSTIR DI + ReSTIR GI (§1.1); NRC stays as a runtime switch for comparison (§11.7), its budget row only for those modes (§2.3) |
| 2026-11-20 | ReSTIR GI mode is the default indirect mode on both tiers (§11.7; NRC bias, considerations C-G5-3) |
| 2026-11-19 | Indirect mode toggle built (§11.7, PBR G7); the GI check gates ReSTIR GI mode against a one-bounce ground truth |
| 2026-11-19 | Combine adds the indirect term (§11.6, PBR G6); the `Lighting only` view is renamed `Direct lighting`, the full sum is `DLSS input` |
| 2026-11-18 | The NRC calls and the DLSS-RR evaluation record into command buffers of their own within the ray submit, with the engine heaps bound before and after each; it fixed the device loss the user still hit (considerations C-G3-6). Costs +0.5% GPU time at a 2560×1369 window, nothing measurable at 1280×720 |
| 2026-11-18 | ReSTIR GI built (§11.5, PBR G5) with the §14 ground truth and its check; a failed BRDF sample counts as a zero sample. RTXDI's GI defaults are biased on static frames (considerations C-G5-1, C-G5-2), NRC reads ~10% bright (C-G5-3) |
| 2026-11-18 | Indirect mode toggle before 1.0 (§11.7, M4, plan G7): NRC → ReSTIR GI (default), NRC alone, ReSTIR GI alone; NRC stays initialized and is paused, never destroyed. P1 notes that runtime toggles keeping every path built are for A/B measurement, while each tier ships one default |
| 2026-11-17 | SER shelved for 1.0 (Slang's EXT `HitObject` support, considerations C-G1-3): every shaded-ray pass is compute with ray queries. Post-1.0 V1 (§16.5) inverted: the SER RT pipeline returns as a toggle beside the ray-query passes, gated on the Slang fixes, and is expected to become the default; §1.1, §2.2, §2.3, §3, S8, §11.1, §16.1, §16.2 M4 follow |
| 2026-11-17 | Post-1.0 target switchable ray paths (§1.1, §16.5): V1 ray queries instead of the RT pipeline, V2 triangle BLAS for static geometry, each behind a toggle with both ways kept; pointers in §10.1 and §11.1. From the re-check of NVIDIA's samples ([mega_geometry_comparison.md](../reference/mega_geometry_comparison.md)) |
| 2026-11-15 | Backlog through M3: AU8 (third-party code in `engine_util` only, coverage off by default) and AU10 (one swapchain recreate path) done; RTXDI previous-frame light rows (`GpuFrameRoot.prevWorldLights`, 192 B) and the `RTXDI reservoirs` debug view (§14); DGC preprocess buffers suballocated; §16.1 state rows corrected. AU1 and AU4 stay optional (§16.2) |
| 2026-11-15 | NVIDIA defaults: RTXDI takes the RTXDI sample's `Medium` preset (uniform local light sampling until M7); `rtxdi_shade` stores and reuses final visibility as the sample does. DLSS-RR (Quality, `Default`), CLAS and PTLAS already matched NVIDIA's samples. The defaults fail the convergence check on `LightsPunctualLamp` (bias −1.34%, tile p90 26.4%), so the check holds `RtxdiContext::convergedSettings` (considerations C-R32-1). ReSTIR DI 0.35 ms against 0.24 ms at 853×480 on the 5070 |
| 2026-11-15 | RTXDI 3.2 ([rtxdi_3_2_update.md](../spec/features/done/rtxdi_3_2_update.md)): the fused spatiotemporal pass is gone upstream, so ReSTIR DI runs `rtxdi_initial`, `rtxdi_temporal` and `rtxdi_shade` (spatial + shading). Spatial reuse runs on disocclusions only with Pairwise MIS, as the fused pass effectively did; 3.2's per-frame spatial default failed the new guide-capture convergence check (tile p90 49.6%). On `LightsPunctualLamp`: bias −0.13%, tile p90 2.87%, frame-to-frame 1.50% (3.1: +0.13%, 2.43%, 0.76%). ReSTIR DI 0.15 + 0.07 + 0.04 ms at 853×480 on the 5070 |
| 2026-11-14 | M3 done. PBR E3: an engine-owned directional sun after the asset lights (on by default, `Sun` panel), since the repo's Sponza ships no lights. World light counts come from `WorldLights`, and a changed count resets history. On sunlit `Sponza` at Quality: temporal deviation 0.35% of the mean on a still camera; repeated poses differ by 2.4% under a yawing camera and 1.5% under a moving sun. ReSTIR DI 0.14 + 0.10 ms at 853×480 on the 5070 |
| 2026-11-14 | Jitter only on frames DLSS-RR evaluates (§12.2): below DLAA, the debug views and the unlit fallback boiled. `ENGINE_JITTER=1` still jitters every frame for the culling checks |
| 2026-11-14 | PBR H2 built: auto exposure from a log2-luminance histogram of the DLSS-RR output into a 1×1 exposure image, then ACES (Hill's fit) once into a display-resolution post target that `present` draws for the Lit view. DLSS-RR takes the exposure image instead of its own auto exposure; `GpuFrameRoot.exposure` and pre-exposure stay 1. `GpuTargets` is 200 B. 0.030 ms on the 5070 at 1280×720. §12.1, §12.3 and §16.2 updated |
| 2026-11-14 | PBR H1 built: DLSS-RR through NGX at the Quality render resolution, fed by a new `combine` pass (`noisyColor`) and the guides, presented for the Lit view. Jitter is on by default and `lodBias` is `log2(render / display)` (§12.2). NGX needs the driver-managed descriptor heap, so an empty legacy descriptor set is bound before each evaluation. 1.14 ms on the 5070 at 853×480 → 1280×720. §12.1, §12.2 and §16.2 updated |
| 2026-11-14 | PBR C4 built, M2 built again: partitioned AS is required at device creation; one PTLAS is updated in place from GPU-written instance writes, with no global partition, which measured slower; every ray sets the OMM 2-state ray flag. Still frames update in 0.015–0.019 ms on the 5070. §10.3 row, X2, Q10 and §16.2 status updated |
| 2026-11-14 | PTLAS is required (§10.3): one partitioned TLAS updated in place from GPU-written ops replaces the per-slot full TLAS build, and every ray sets the OMM 2-state ray flag (§10.5). M2 is reopened for PBR C4; M3 keeps its built work (C4 adds the ray flag to `traceVisibility`); M9 loses the 0.3 ms trigger. New Q10 |
| 2026-11-14 | Target is NVIDIA RTX only: the AMD-later note (§1.2) and P-AMD are closed. NRC is the only radiance cache (§11.4): SHaRC and `IRadianceCache` are dropped, M5 is merged into M4, and R4 has no fallback |
| 2026-11-14 | PBR F3 built: `rtxdi_initial` and `rtxdi_shade` write `directRadiance` (RGBA16F), shown with emissive as the `Lighting only` view. The fused pass matches RTXDI's unbiased reference on `LightsPunctualLamp` (64-frame mean −0.001% against +0.005%; frame-to-frame change 0.8% against 11.4%). This needs pairwise MIS and permutation sampling off, since RTXDI 3.1's fused `Basic` path was 35% dark. It also needs bounds-checked reservoir and light reads in the bridge, where RTXDI assumes structured buffers with robust access. Scenes without lights skip the initial pass, and the shade pass only writes zeros |
| 2026-11-14 | PBR F2 built: `lighting/rtxdi_bridge.slang` implements the `RAB_*` set RTXDI 3.1's DI headers need, over the guide sets, E2's world lights and a new `traceVisibility`. Last frame's surfaces come from a new `position` guide (RGBA32F world position + view depth, both sets), `motion.z` (view depth change; motion is RGBA16F) and `GpuCameraData.prevCameraPos`. A `Shadowed lights` debug view is the punctual reference for F3. Raster ray origins had 0 self-hits over 450 M rays |
| 2026-11-14 | PBR F1 built: `RtxdiContext` holds the `ReSTIRDIContext` at render resolution (fused spatiotemporal, uniform local light sampling), recreated on resize, with the DI reservoirs (3 buffers, 63.3 MiB at 1280×720), the neighbor offsets and a 352 B `GpuRtxdiConstants` block per frame slot in `GpuFrameRoot.rtxdiConstants`. The light regions follow E2's local-then-infinite order. Nothing reads them until F2 |
| 2026-11-14 | PBR E2 (punctual) built: `passes/light_world.slang` writes the world-space light rows of each frame slot on the async compute queue from the model-space rows and the entity matrices (`WorldLights`, `GpuFrameRoot.worldLights`, root 184 B). Point and spot rows come before directional (`localLightCount`), ready for RTXDI's regions. An `Unshadowed lights` debug view lights the guides with every world light. Spinning `LightsPunctualLamp` keeps its lighting fixed to the mesh, and the check fails with the entity matrix removed |
| 2026-11-14 | PBR E1 open items done except `lodBias`. AO goes into `diffuseAlbedo.a`, with an `Ambient occlusion` debug view. Unlit surfaces write specular weight 0 into `specularF0.a`, with `f0` and the specular albedo zeroed. Halton(2,3) jitter is built with the §12.2 culling rule and stays off until DLSS (`ENGINE_JITTER=1`); the cull check is bit-identical with jitter, and the ray check matches every frame. The ray check's position error is now measured in pixels on screen (≤ 0.056 px, gate 0.1 px) |
| 2026-11-14 | PBR B1 closed: `ENGINE_GUIDE_CAPTURE` adds `TextureTransformTest` and `TextureTransformMultiTest`. Both match the Khronos screenshots: every offset, rotation and scale marker is correct, and the transform reaches all five material slots on both UV sets. Validation clean. `lodBias` stays with E1 until DLSS sets a render resolution |
| 2026-11-13 | PBR C3 built: `geometry/hit.slang` traces primary rays with ray queries and decodes hits through the resolve's `loadDrawContext` / `buildSurface` path, with fetched positions, ray-cone gradients (spread `pixelAngle · cos^1.5 θ`) and `SafeSpawnPoint` origins. The ray debug views (`Ray meshlets`, `Ray node instances`, `Ray shading normal`) and the checks run in a second graphics command buffer per slot, which waits for the compute timeline (§10.3). `ENGINE_RT_CHECK=1` matched every interior pixel in 214 of 214 cull-check, 318 of 318 motion-test and 70 of 70 guide-capture frames on the RTX 5070. The decode reproduces the resolve's normals exactly, the mip is within 0.22 levels, there are 0 self-hits, and validation is clean. M2 is built |
| 2026-11-13 | RT references reviewed (RTXMG, `vk_tessellated_clusters`, `vk_partitioned_tlas`): the feature spec gains the PTLAS design (§3.6), `SafeSpawnPoint` origins, RTXMG bake defaults and position truncation data, and cluster templates for deformed geometry; §10.3 TLAS row notes the PTLAS constraints. C1/C2 revisited: no code change; CLAS truncation measured (7 bits: −22% CLAS on Sponza, kept at 0) |
| 2026-11-13 | PBR C2 built: `RayTracingScene` writes one TLAS instance per (node instance, primitive with a BLAS) on the async compute queue from the slot's node instances (the CPU writes the ray-instance rows only per scene layout) and builds the TLAS fully every frame; `GpuFrameRoot.tlas` / `.rayInstances` are set. `timelineSemaphore` is required. TLAS build 0.10 ms for 103 instances, 0.15 ms for 4199 (RTX 5070). CLAS/BLAS now also carry `ALLOW_DATA_ACCESS` for position fetch |
| 2026-11-13 | PBR C1 built: `VK_NV_cluster_acceleration_structure` and `VK_KHR_ray_tracing_position_fetch` are required. `MegaGeometry` builds one CLAS per meshlet (clusterID = meshlet row, raster triangle order) and one cluster BLAS per mesh primitive at each model load, sized exactly by a compute-sizes pass (Sponza: 3500 CLAS in 4.72 MiB against a 15.81 MiB host estimate, 103 BLAS in 0.24 MiB), and drops them with the model. `ENGINE_GUIDE_CAPTURE` now loads each shot before unloading the previous model, validation clean (core, sync, best practices) |
| 2026-11-13 | PBR A4 checked: `ENGINE_GUIDE_CAPTURE` loads `NormalTangentMirrorTest` without `TANGENT` and `MetalRoughSpheres` without `NORMAL` (copies under `logs/guides/variants`). The generated tangents match the shipped ones within 0.04°, and the flat normals match the geometric normal within 0.063° (99.9% of pixels; the check fails above 0.25°), validation clean. A4 leaves the M0 open list |
| 2026-11-13 | M1 done: the M1 criterion "cull counts unchanged across the reverse-Z switch" is replaced by a check of reverse Z on its own. `ENGINE_CULL_CHECK=1` compares 16 Sponza poses with every cull test on and with all off; the first capture follows a camera turn, so phase 1 tests against a stale pyramid. All 16 are bit-identical in albedo and depth, with validation clean, and a deliberately over-culling Hi-Z test fails 14 of them. RTX 5070 stage timings recorded in §2.3 |
| 2026-11-13 | PBR E1 guide check done: `ENGINE_GUIDE_CAPTURE=1` writes every guide view of `MetalRoughSpheres`, `TwoSidedPlane` (both sides) and `UnlitTest` as PNGs under `logs/guides`; all three match the Khronos screenshots in kind, with validation clean. Open for M3: unlit surfaces carry `f0` 0.04 into the guides (PBR plan E1) |
| 2026-11-13 | Motion-vector test list (motion_vectors.md §5) run as a scripted check: `ENGINE_MOTION_TEST=1`, `passes/motion_check.slang`, 13 scenarios pass on the RTX 5070 with validation clean; each rule fails on a deliberately broken resolve |
| 2026-11-13 | M0 startup log complete: the memory decompression methods are logged next to the SER hint (`Memory decompression: methods={ Gdeflate10 }` on the RTX 5070) |
| 2026-11-13 | PBR B3 done: `lighting/lights.slang` (`evalLight`, KHR_lights_punctual range window and spot cone) with a GPU test (§16.1) |
| 2026-11-13 | §20.4 document fixes done: resource and shader architecture docs brought to M1 as built; README binding and platform lines; extensions reference and `VkExtensionsSpecs/README.md` (shader objects, cached OMM/SER properties, cluster AS required in M2); `vulkan_integration_playbook.md` deleted; PBR explainer links point to `docs/`; links to the five missing documents folded into §6.3, §6.4, §7.1, §7.2 and the glTF 2.0 spec; dead links in the vis-buffer and ResourceManager-split feature specs fixed. Subsystems without a document stay undocumented |
| 2026-11-13 | §20.3 defects fixed: per-file embedded image keys, device destroyed before the window, host-copy layout check, `MaterialStore::release` validation, upload cleanup on exceptions, scene actions caught at the UI boundary, `destroyTexture` lookup |
| 2026-11-13 | AU5 and AU11 done: forward-Z culling branch removed, Slang/C++ layout test, typed storage images; cluster sizes stay at NVIDIA's recommendation without an assert. AU2 and AU3 marked done; every other open AU item is optional (§16.2, §20.2) |
| 2026-11-12 | AU7 done: zero-byte sources, stub shaders, the descriptor-set binding mode and unused types deleted ([dead_code_removal.md](../spec/features/done/dead_code_removal.md)); §17 PBR plan A3 row and §16.2 updated |
| 2026-11-10 | Architecture audit added as §20 with decisions AU1–AU11, defects and document fixes; audit work assigned to milestones (§16.2); current state brought up to date: M0 and M1 built, their checks open (§8.1, §8.2, §16.1); stub shader files kept by §17 are now to be deleted (AU7) |
| 2026-11-08 | OMM baking moved from meshoptimizer to the NVIDIA OMM SDK CPU baker: one bake per primitive, per-cluster index subranges (P6, §7, §7.1, §10.5, M6) |
| 2026-11-07 | Goal 6, CPU/GPU parallelism: overlap targets X1–X6 with milestones (§13.1; M2, M8, M9 updated; §2.3 CPU budget note). Post-1.0 target multithreaded rendering T1–T4 (§1.1, §16.4) |
| 2026-11-07 | Visibility buffer plan ([visibility_buffer.md](../spec/features/done/visibility_buffer.md)): 32-bit `R32_UINT` payload is the default, 64-bit only if M1 measurements favor it (§8.3, Q3); finite far plane under reverse Z (§8.2); overdraw debug view dropped (§14); PBR B1 moved into M1 (§16.2) |
| 2026-11-07 | Culling aligned with the `vk_lod_clusters` leaf level per [culling_comparison.md](../reference/culling_comparison.md): as-built pipeline and remaining steps A6/A8/A9/A10 assigned to milestones (§8.1, §8.2, §12.2, §16.2); clusters as `clusterlod.h` level 0 with groups, `cone_weight = 0`, optimize level 1 with OMM baked after it (§6.1, §6.3, §7.1); visible entries `{nodeInstanceIndex, meshletRow}` (§8.3, §10.1); M9 named version 1.0; post-1.0 target L3: Bistro fully on cluster LOD (§1.1, §16.3) |
| 2026-11-04 | SER and OMM confirmed on the 2060 Super; DLSS-RR Quality on every tier; limits from device properties, not fixed numbers (S7, S11, §3, §7.1, §8.3); final goal = Bistro, cluster LOD moved after it (§16.3); animation removed (§7.3, §10.6 deleted); compressed uploads with `VK_EXT_memory_decompression` and automatic KTX2 conversion (goal 5, §3, §6.4, §7.2, M8, `texture_pipeline_plan.md`) |
| 2026-11-01 | Review fixes: GI cache passes on the graphics queue in SDK order, NRC custom resolve feeds ReSTIR GI (§5, §11.4, §13); `specHitDistance` optional per NGX 1.5, full resolution after ReSTIR GI if used (§9.3, §12.1); bin overlap guard (§9.1–9.2); position fetch rationale and truncation rule (§10.2); no occlusion LOD penalty in the RT cut (§10.3–10.4); M1 is one resolve dispatch |
| 2026-10-31 | RT rebuilt around cluster LOD (§10, following `vk_lod_clusters`): CLAS per cluster, no triangle BLAS, GPU TLAS with PTLAS optional, hybrid-consistency rule; §11.1 built around SER; milestones reordered (cluster LOD last); contradicting shader stubs deleted |
| 2026-10-31 | Q1 closed (OMM per cluster confirmed in the spec), Q2 closed (SHaRC + NRC, SHaRC first), streaming moved out of scope, AMD-later note |
| 2026-10-31 | Floor card fixed to RTX 2060 Super (8 GB): the engine runs on it, so extension availability on Turing is confirmed; risks R1/R2 removed |
| 2026-10-31 | Initial spec from end_goal.md, PBR plan, resource and shader architecture docs, meshoptimizer notes and the NVIDIA references |
