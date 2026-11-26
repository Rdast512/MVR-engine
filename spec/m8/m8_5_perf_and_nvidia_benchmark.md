# M8.5 · Lighting-stack GPU performance (NRC, RTXDI, RTXGI) and NVIDIA Vulkan practices

**Date:** 2026-11-26 · **Status:** baseline measured (§1); P1 and P2's `ONE_TIME_SUBMIT` done 2026-11-26 (§7)
**Plan:** [M8 README](./README.md) step M8.5 · **Depends on:**
- P0–P4 run on the current pipeline;
- P5 needs M8.3 (composable toggles) and M8.4 (SHaRC);
- P6 needs P5.

**Budgets:** [engine spec §2.3](../../docs/engine_spec.md#23-frame-budget)
**References:**
- `reference/RTXGI` (`Docs/NrcGuide.md`, `Docs/SharcGuide.md`, `Samples/IndirectDiffuse`);
- `reference/RTXDI` (`Doc/Integration.md`, `Samples/FullSample`);
- `reference/RTXPT-main`;
- NVIDIA developer blog (§9);
- Vulkan-ValidationLayers `layers/best_practices/` at tag `vulkan-sdk-1.4.363.0` (the installed SDK).

The renderer is correct and validated, but the lighting stack is over its §2.3 allocations at the reference tier's render resolution. The first measured pass already finds a 2 ms GPU loss in a cheap pass (§1.2). This spec does four things:
- fixes the measurement protocol;
- turns the NVIDIA Best Practices layer and NVIDIA's published Vulkan / RTX guidance into engine rules;
- lists the optimizations per subsystem, each with a measured gate;
- ends with the head-to-head FPS comparison against NVIDIA's demos that M8 promises.

---

## 1. Baseline (2026-11-26)

**Setup:**
- RTX 5070, driver 621.40, release checks tree, no layers;
- `ENGINE_BENCH_FRAMES=6000`, `ENGINE_WINDOW_SIZE=2560x1440`: render 1707×960, DLSS-RR Quality;
- default settings: DI ReGIR_RIS temporal + spatial, plain-indirect raster (`ENGINE_DGC` unset);
- mean ms, one round per column;
- scopes under 0.01 ms in every column are left out.

### 1.1 Per scope

| Scope | Sponza gallery, NRC | Courtyard (sunlit), NRC | Bistro, NRC | Gallery, NRC + ReSTIR GI | Courtyard, NRC + ReSTIR GI | Bistro, NRC + ReSTIR GI |
|---|---|---|---|---|---|---|
| Cull + raster + Hi-Z, both phases | 0.13 | 0.14 | 0.15 | 0.12 | 0.13 | 0.14 |
| Classify + resolve | 0.33 | 0.33 | 0.30 | 0.30 | 0.33 | 0.29 |
| Ray start wait | 0.01 | 0.01 | **0.33** | 0.01 | 0.01 | **0.30** |
| ReSTIR DI initial | 0.40 | 0.66 | 0.77 | 0.38 | 0.64 | 0.77 |
| ReSTIR DI temporal | 0.32 | 0.42 | 0.15 | 0.31 | 0.40 | 0.15 |
| ReSTIR DI shade | **1.49** | **2.44** | 0.93 | 1.61 | 2.40 | 0.92 |
| NRC update | 0.02 | 0.04 | 0.02 | 0.02 | 0.04 | 0.02 |
| Indirect (bounce 1 + NRC query) | **2.14** | **2.53** | **1.52** | 2.08 | 2.52 | 1.57 |
| NRC train | 0.91 | 1.00 | 0.53 | 0.93 | 1.04 | 0.52 |
| NRC resolve | 0.32 | **2.15** | 0.41 | 0.36 | 2.12 | 0.41 |
| ReSTIR GI temporal | 0.04 | 0.05 | 0.03 | 0.47 | 0.46 | 0.27 |
| ReSTIR GI shade | 0.20 | 0.22 | 0.13 | 1.36 | 1.43 | 0.66 |
| Combine | 0.03 | 0.03 | 0.02 | 0.05 | 0.05 | 0.04 |
| Glass | 0.00 | 0.00 | **0.41** | 0.00 | 0.00 | 0.41 |
| DLSS-RR | **3.93** | **3.84** | **3.83** | 3.80 | 3.84 | 3.80 |
| Post + present + UI | 0.13 | 0.13 | 0.12 | 0.12 | 0.12 | 0.12 |
| **GPU total** | **10.41** | **13.98** | **9.66** | **11.92** | **15.54** | **10.38** |

The Bistro columns above are the **startup view**: no `ENGINE_BENCH_POSE`, so the camera stands outside the café and looks across the whole scene (199 visible meshlets). They understate Bistro badly.

The interior and street poses are the ones the café and glass checks use (`ENGINE_BENCH_POSE` accepts them since 2026-11-26). Measured after P1, NRC mode, one round each:

| Scope | Startup view | `cafe tables` | `cafe dining` | `street windows` |
|---|---|---|---|---|
| Cull + raster + Hi-Z, both phases | 0.15 | 0.32 | 0.31 | 0.48 |
| Classify + resolve | 0.27 | 0.55 | 0.57 | 0.59 |
| Ray start wait | 0.36 | 0.22 | 0.26 | 0.24 |
| ReSTIR DI initial | 0.82 | **3.83** | **3.87** | **3.00** |
| ReSTIR DI temporal | 0.17 | 1.05 | 1.04 | 0.77 |
| ReSTIR DI shade | 0.67 | **4.46** | 2.71 | **5.62** |
| Indirect | 1.64 | **6.20** | **6.74** | **5.89** |
| NRC update + train + resolve | 0.71 | 1.23 | 1.31 | 1.19 |
| ReSTIR GI shade (reuse off) | 0.14 | 0.19 | 0.20 | 0.19 |
| Glass | 0.43 | **1.84** | **2.61** | **3.16** |
| DLSS-RR | 3.91 | 3.76 | 3.92 | 3.68 |
| **GPU total** | 9.41 | **23.80** | **23.68** | **24.97** |

Inside the café the frame is **over the 16.6 ms total** on the reference GPU:
- direct lighting is 5–6× its allocation;
- the indirect pass is 2× its allocation;
- glass costs as much as the whole cache.

These poses are where M8.5 is decided. The Sponza columns stay as the lighter case.

Against the §2.3 reference allocations, which apply to this render size:

| Stage | Allocation | Measured, NRC mode | Verdict |
|---|---|---|---|
| Direct lighting (initial + temporal + shade) | 1.5 | 1.85–3.52 Sponza; 7.6–9.4 Bistro interior | over in every scene, 5–6× inside the café |
| Indirect bounce 1 | 3.0 | 1.52–2.53 Sponza; 5.9–6.7 Bistro interior | within in Sponza, 2× over inside the café |
| Radiance cache (update + train + resolve) | 0.8 | 0.96–3.19 | over; 1.9 ms of the courtyard's 3.19 is §1.2 |
| ReSTIR GI (toggle) | 0.0, or 1.2 when on | 0.17–0.27 off, 0.93–1.89 on | not free when off (P5); over when on in Sponza |
| DLSS-RR | 2.5 | 3.83–3.93 | over (NGX; P4 measures options) |
| Glass (no own row; counted with resolve) | — | 0.43 outside; 1.8–3.2 at the café windows | needs its own allocation (P3) |
| Total | 16.6 | 9.66–13.98 Sponza; 23.7–25.0 Bistro interior | over inside the café |

### 1.2 First finding: GPU atomics into system memory

**What happens:**
- `NrcCache` allocates `NrcHealth_<slot>` as host-visible + host-coherent + **host-cached** (`vk_nrc.cpp:109-114`) so the CPU reads it cheaply.
- Host-cached memory is system memory on NVIDIA. Every `InterlockedAdd` / `InterlockedMax` in `nrc_resolve.slang:countNrcHealth` (3 per wave) therefore crosses PCIe.
- The sunlit courtyard has the most waves with traced light, so it pays most.

**Probe** (2026-11-26, health address forced to 0, 3000 frames, courtyard, then reverted):

| Scope | Before | Probe |
|---|---|---|
| NRC resolve | 2.151 | 0.250 |
| GPU total | 13.98 | 11.72 |

`GlassCounters_<slot>` (`vk_renderer.cpp:55-60`) has the same memory type and is written by `glass.slang` through `waveCount` (up to 3 + `kGlassMaxLayersLimit` atomics per wave). P1 fixes both. After the fix, Bistro's Glass scope did not move (0.41 → 0.43 ms): at the bench pose few waves count glass, so the pass's cost is its own work, not the counters.

### 1.3 NVIDIA Best Practices layer (2026-11-26)

**Setup:**
- SDK 1.4.363.1, 300 bench frames per indirect mode;
- `VK_KHRONOS_VALIDATION_VALIDATE_BEST_PRACTICES=true`, `..._BEST_PRACTICES_NVIDIA=true`, `VK_KHRONOS_VALIDATION_REPORT_FLAGS=error,warn,perf`, duplicate limit 3.

All three modes give the same set:

| Message | Source | Owner | Action |
|---|---|---|---|
| `BestPractices-NVIDIA-BindMemory-NoPriority` (≥3) | `vkBindBuffer/ImageMemory` on memory without `VkMemoryPriorityAllocateInfoEXT` | not VMA: the allocator sets `EXT_MEMORY_PRIORITY` and priorities (`vk_utils.cpp:105-111`). So the NGX or NRC SDK allocates internally | P2: attribute by object name; external if SDK |
| `BestPractices-vkBind*Memory-small-dedicated-allocation` (≥6) | 384 B, 10 KiB buffers; 512 B, 720 KiB images, each in its own `VkDeviceMemory` | same: VMA suballocates | P2: attribute; external if SDK |
| `BestPractices-NVIDIA-CreateImage-Depth32Format` (1) | the vis-buffer depth `D32_SFLOAT` | engine | keep: reverse Z needs float depth (engine spec §8.2). Recorded as accepted |
| `BestPractices-NVIDIA-CreatePipelineLayout-SeparateSampler` (1) | `vkCreatePipelineLayout` | external: the engine creates no pipeline layouts (shader objects + heaps) | none |
| `BestPractices-SpirvDeprecated_WorkgroupSize` (≥3) | `vkCreateComputePipelines` | external (SDK pipelines) | none |
| `BestPractices-SyncObjects-HighNumberOfFences` (6 > 3) | `vkCreateFence` | engine and SDKs | P2: count the engine's fences; the frame sync needs one per slot |
| `SYNC-HAZARD-WRITE-AFTER-WRITE` ×2 | NGX DLSS-D | external, C-H1-2 | none |
| `VUID-vkDestroyDevice-device-05137` | NRC leaks | external, C-G3-3 | none |

**What did not fire:**
- `vkBeginCommandBuffer-one-time-submit`, although `recordFrameCommands`, the ray segments and the present command buffer begin without `ONE_TIME_SUBMIT` (`vk_renderer.cpp:592, 729, 761`). NVIDIA's Vulkan Dos and Don'ts still asks for the flag on single-use buffers (P2).
- `AccelerationStructure-NotAsync`: the PTLAS and cluster builds are on the compute queue.
- `ClearColor-NotCompressed` and `Zcull-LessGreaterRatio`.

The engine owns one perf message (`Depth32Format`, accepted). The layer cannot see this frame's large costs (§1.1). The source of truth is the timers plus Nsight, not the layer.

## 2. Measurement protocol (P0)

Every item in §4–§6 is accepted on this protocol only.

1. **Build:** release checks tree (`windows-clang-release-checks`), no layers (`VK_LOADER_LAYERS_DISABLE='*validation*'`), vkconfig override off.
2. **Clocks:** lock the GPU (`nvidia-smi -lgc <base>,<base>` from an admin shell, reset with `-rgc` afterwards) for scope work. Unlocked runs are allowed for frame totals only and are marked as such.
3. **Matrix** at `ENGINE_WINDOW_SIZE=2560x1440`:
   - Sponza gallery and courtyard;
   - Bistro `cafe tables`, `cafe dining` and `street windows` (`ENGINE_MODEL=models/bistro/bistro.gltf ENGINE_BENCH_POSE=...`), not the startup view;
   - each in NRC and NRC + ReSTIR GI mode;
   - plus Sponza at 1920×1080 (render 1280×720), the floor tier's resolution.
4. **Order:** ABBA per column, `ENGINE_BENCH_FRAMES=6000`. A change counts when every pair moves the same way and the mean moves more than the round-to-round spread of that scope.
5. **Per-pass hardware view:** Nsight Graphics GPU Trace of one frame per column, before and after each P-step. Record per pass:
   - SM throughput, L1TEX throughput, L2 throughput, VRAM throughput, RT core throughput;
   - warp occupancy and its limiter (registers, shared memory, or block size);
   - registers per thread.

   Store the numbers in this spec's §7 table, not screenshots.
6. **Gaps and submits:** Nsight Systems over 300 frames. Record queue submits per frame, GPU idle between segments, and the async-compute overlap. Today's frame submits:
   - one graphics command buffer;
   - one ray segment per NRC SDK call (C-G3-6 isolation);
   - present;
   - the async compute submit.
7. **Quality gate:** every change keeps the checks that cover it:
   - cull 16/16, RT 32/32, GI 14/14;
   - stability lines identical when the change claims bit-exactness, otherwise within the runner's limits;
   - DLSS check;
   - guide capture for DI changes.

   Validation (core + sync + Best Practices + NVIDIA, `REPORT_FLAGS=error,warn,perf`) stays at the §1.3 set.

`tools/` gets a bench script that runs the §2 matrix and prints the §1.1 table. It is today's scratch script, moved in: run matrix, alternate order, parse `[Bench]` scopes.

## 3. NVIDIA Vulkan / RTX rules for this engine

These come from NVIDIA's published guidance (§9), checked against the engine. **Status:** ok = already followed, gap = work in §4–§6, n/a = does not apply.

| # | Rule | Source | Status |
|---|---|---|---|
| N1 | No GPU atomics or frequent GPU writes into host-cached (system) memory. Accumulate in device-local memory, copy once to a readback buffer | Dos and Don'ts (host-visible memory reads/writes are slow); §1.2 | ok since P1 (`CounterReadback`) |
| N2 | `ONE_TIME_SUBMIT` on every command buffer recorded per submit | Dos and Don'ts | ok since P2 (2026-11-26) |
| N3 | Few submits per frame; no tiny command buffers (each costs driver work, and compute cannot overlap across a boundary) | Dos and Don'ts | **gap**: NRC SDK segments (C-G3-6), measured in P0.6, reduced in P3 if the SDK allows |
| N4 | Barriers: one `pipelineBarrier2` per transition point, tightest stages, minimum access flags, no read-to-read barriers | Advanced API Performance: Barriers | ok, mostly; audit the lighting chain in P2 (`vk_renderer.cpp:1050-1182`) |
| N5 | Full-screen compute: 8×8 to 16×16 groups (2–8 warps). Inline ray-query compute: 8×8 rule of thumb, 16×8 a practical choice, ≥3 warps per group for full occupancy if registers allow | Shaders; RTX best practices | ok (8×8 everywhere); P3 tries 16×8 on the ray passes |
| N6 | Low live state across `TraceRayInline` / `Proceed`: values live across the query cost registers and occupancy | RTX best practices | **gap**: unmeasured; P3 measures registers per ray pass |
| N7 | Thread-group swizzling for cache and ray coherence in full-screen compute | Shaders; RTX best practices | **gap** (row-major today); P3 |
| N8 | Opaque geometry and compile-time ray flags; `ACCEPT_FIRST_HIT_AND_END_SEARCH` for visibility rays; no face culling unless needed | RTX best practices | ok (`trace.slang:62-64`, OMM 2-state, FORCE_OPAQUE) |
| N9 | One dispatch per material model rather than divergent shading in a ray pass; avoid ID → buffer → index indirection chains | RTX best practices | partial: the resolve is binned (M7 E1). The ray passes shade every hit in one pass (P3 measures divergence) |
| N10 | Async compute pairs: low-throughput work on complementary units; never two RT-core-bound passes together; async work must finish before its dependency | Async Compute and Overlap | **gap**: Bistro's 0.33 ms `Ray start wait` is the graphics queue waiting on async light prep + PTLAS (P3) |
| N11 | Memory priority on every allocation; `pageable_device_local_memory` enabled; stay inside `VK_EXT_memory_budget` | Dos and Don'ts; BP-NVIDIA | ok for engine allocations; SDK allocations external (§1.3) |
| N12 | Suballocate; no small dedicated allocations | Dos and Don'ts; BP | ok for engine (VMA); attribute the §1.3 hits (P2) |
| N13 | FP16: no general gain on Ampere and later; Turing (2060 Super, floor) issues FP16 at 2× FP32. Pack data in the smallest type that fits | Shaders | candidate for the floor tier only (P3, measured on the 2060 Super in M8.6) |
| N14 | Prefer wave intrinsics to shared memory; coalesce atomics per wave | Shaders | ok (`waveCount`, `countNrcHealth`) |
| N15 | Specialization constants for per-session constant features, to cut registers and branches | Shaders; Dos and Don'ts | partial (`kDebugRays`, `kIsSerReorderEnabled`); P3 lists the per-frame flags that are constant per session |
| N16 | Benchmark without layers, clocks locked, over many frames; keep debug-utils labels in release | Dos and Don'ts | ok (§2) |
| N17 | Depth: `D24_UNORM_S8` or `D32_SFLOAT`, never `D32_SFLOAT_S8`. BP-NVIDIA prefers 24-bit | Dos and Don'ts; BP | ok: `D32_SFLOAT` for reverse Z, accepted |

## 4. Engine-wide steps (current pipeline)

### P1 · No GPU writes into system memory (N1)

- `NrcHealth`:
  - the resolve's atomics target a device-local `GpuNrcHealth` per slot;
  - after the resolve, `vkCmdCopyBuffer` copies the 12 bytes into the existing host-cached readback buffer, with a transfer → host barrier;
  - `isNetworkDiverged` reads the readback as today;
  - the clear moves from the host write (`vk_nrc.cpp:187, 205`) to a `vkCmdFillBuffer` before the resolve.
- `GlassCounters`: same pattern.
- Audit every other `eHostCached` buffer (`vk_rt_scene.cpp:649` and `vk_mega_geometry.cpp:167` are copy targets, fine; `check_passes.cpp:150` is checks-only).
- **Rule:** shaders never write `eHostCached` memory. Add it to `docs/settings.md`'s perf rules and the vulkan-dev checklist.
- **Gate:**
  - courtyard NRC resolve ≤ 0.30 ms;
  - Bistro Glass drops by what the atomics cost (expected ≥ 0.1 ms);
  - the C-I16-5 divergence detector still trips on the poisoned-network repro (`NrcCache` log line);
  - GI 14/14 and stability lines identical.

### P2 · Command buffers, submits, barriers, layer findings (N2–N4, N11, N12)

- `ONE_TIME_SUBMIT` on the frame, ray-segment and present command buffers.
- Count fences per frame slot and drop any the timeline semaphores already cover.
- Attribute every §1.3 `NoPriority` and `small-dedicated` hit by object name (`VK_KHRONOS_VALIDATION_LOG_FILENAME` plus debug-utils names). Fix the engine's, and record external ones in considerations with their owner.
- Barrier audit of the lighting chain (DI, NRC, indirect, GI, combine):
  - one barrier per dependency;
  - no read-to-read;
  - no `eShaderSampledRead` when only storage access follows;
  - a pass that writes nothing the next pass reads gets no barrier.

  The DI chain has 3 global barriers today, the GI chain up to 3.
- **Gate:** the §1.3 set minus the fixed rows; CPU `submit` and `record` (`[Bench]` CPU frame) no worse; GPU total no worse.

### P3 · Ray-pass occupancy, coherence and overlap (N5–N7, N9, N10, N13, N15)

Targets are the largest ray passes, judged at the Bistro interior poses first:
- Indirect: 5.9–6.7 ms (Sponza 1.5–2.5);
- ReSTIR DI shade: 2.7–5.6 (Sponza 1.5–2.4);
- ReSTIR DI initial: 3.0–3.9 (Sponza 0.4–0.8);
- Glass: 1.8–3.2.

Bistro's many local lights and glass layers stress them in ways Sponza does not.

**Experiments** (one at a time, ABBA, kept only on a measured win):
1. **Registers and occupancy:**
   - Nsight registers per thread and the occupancy limiter for each ray pass;
   - shrink state live across `traceClosest` / `traceVisibility`: load material data after the query, not before;
   - session-constant branches become spec constants: `giReferenceBounces == 0`, the cache-view flag, `kGiFlagSecondaryRoughnessFloor`, `kGiFlagFireflyClamp`, the NRC termination copy in `indirect.slang:84-90`.
2. **Group shape:** 16×8 for `indirect`, `rtxdi_shade`, `rtxdi_initial`, keeping `kFullscreenTile` for the others.
3. **Swizzle:** tile-swizzled group ids (row-major tiles of 8 groups) for the ray passes, so neighbouring warps trace neighbouring rays.
4. **Divergence in `indirect`:** the hit shading at X₂ (`shadeRayHit` + `secondaryDirectLight` with its own visibility ray) runs in one pass for every material. Measure branch efficiency; if it is low, split into trace → shade-by-bin like the primary resolve (N9). This is the largest structural change; only with a measured ≥ 15% win on the pass.
5. **Async overlap (N10):**
   - Bistro's 0.33 ms `Ray start wait` means the async light prep + PTLAS finish after the raster. Start the async submit earlier (it needs only the scene update), or move the light prep that does not depend on this frame's culling ahead.
   - NRC train is math-bound, and the next passes are RT-bound. Try training on the async queue while the graphics queue runs DI shade of the same frame (DI does not read NRC). This needs DI recorded after the indirect query, cross-queue semaphores, and the SDK's buffers shared concurrently; the NRC guide allows the update and query passes to be independent.
6. **Floor-tier FP16 (N13):** reservoir and radiance math in `half` where the precision analysis allows. Measured on the 2060 Super only (M8.6); the 5070 must not regress.

**Gate:** per experiment, the pass's ABBA mean drops by more than its spread, the other passes do not rise, and the quality gate (§2.7) holds.

### P4 · DLSS-RR (largest single scope, 3.8–3.9 ms)

NGX, so the engine controls only inputs and mode:
- confirm the evaluate is the only DLSS work in the scope: no extra copies, no layout round-trips around it (Nsight Systems / GPU Trace);
- measure the Balanced and Performance modes on the 2560×1440 matrix as preset rows. The default stays Quality unless the user decides otherwise;
- guide formats at the minimum DLSS-RR accepts (`vk_denoise_dlssrr` sample), recorded with their bandwidth;
- check whether the NGX `CreateFeature` flags in use match NVIDIA's sample for RR. Wrong flags (for example an HDR / exposure mismatch) cost both time and quality.

**Gate:** documented per-mode cost. No change to the default without the user.

## 5. Subsystem steps

### NRC (RTXGI)

| Item | Now | Change | Reference |
|---|---|---|---|
| Health atomics | system memory, 1.9 ms in the courtyard | P1 | §1.2 |
| SDK isolation | each SDK call opens a new ray segment (C-G3-6) | P0.6 measures the cost; if over 0.05 ms, check whether NRC 0.16's command-buffer requirement allows one segment for BeginFrame + Train | `NrcGuide.md` Step 4, C-G3-6 |
| Training resolution | `computeIdealTrainingDimensions(frame, 0)` | sweep `avgTrainingVerticesPerPath` against train cost and GI-check bias | `NrcGuide.md` Step 5 |
| Train cost | 0.53–1.04 ms, scales with scene, not pixels | compare iterations and batch with `RTXGI/Samples/IndirectDiffuse` defaults; record cost per training vertex | IndirectDiffuse |
| Update ∥ query | update, then query, serialized | the guide calls them independent: no barrier between them, or update on async (P3 exp. 5) | `NrcGuide.md` "The update pass is independent of the query pass" |
| Resolve | reads the answer, rewrites the GI reservoir | with ReSTIR GI off (P5), write `indirectRadiance` directly; the reservoir round-trip is only for reuse | M8 plan, Raw 1-Bounce PT fast path |

### RTXDI (ReSTIR DI)

| Item | Now | Change |
|---|---|---|
| Shade pass (1.5–2.4 ms) | the largest DI cost; final visibility ray + BRDF per pixel | P3 experiments 1–3; then the M8.3 target modes: Streaming RIS fused (no reservoir round-trip) and Temporal-only `Basic` (no spatial pass). Pick the default by §2 numbers (the M8 plan defers this choice to M8.5) |
| Initial (0.4–0.8 ms) | ReGIR_RIS candidates | compare ReGIR vs Power_RIS candidate counts at equal quality (guide-capture convergence checks), per RTXDI `Integration.md` §7 |
| Temporal (0.15–0.42 ms) | full temporal resampling | Max History 4–8, `Basic` bias correction in the target mode |
| Reservoir bandwidth | 3 buffers, 28.5 MiB at 853×480 (≈113 MiB at 1707×960) | measure the L2/VRAM share in GPU Trace; reduce buffer count in Temporal-only mode |
| Presample | async light prep | P3 exp. 5 (start earlier) |

### ReSTIR GI (RTXDI GI, toggle) and the indirect pass

| Item | Now | Change |
|---|---|---|
| GI with reuse off | NRC mode still runs GI shade 0.13–0.22 ms and temporal 0.03–0.05 ms | P5: 0 ms when the toggle is off (M8 acceptance) |
| GI on | temporal 0.27–0.47, shade 0.66–1.43 | P3 experiments on the shade pass; it is a toggle, so its budget is 1.2 ms (Sponza is over) |
| Indirect | 1.5–2.5 ms | P3 experiments 1–4 |

### SHaRC (M8.4)

When M8.4 lands, run it through the §2 matrix as a cache column beside NRC. SharcGuide's update is sparse (1 per 4×4 block), so its cost must come in below NRC's update + train. This is the RTXPT-analog acceptance row.

## 6. Target stack and demo comparison

### P5 · Target-stack fast paths (after M8.3)

- **Zero cost when off:** with Direct, Indirect, ReSTIR GI, the cache or DLSS-RR off, its scopes read < 0.005 ms. No dispatch, no barrier, buffers released lazily.
- **Raw 1-Bounce PT without ReSTIR GI:** `indirect` + NRC resolve write `indirectRadiance` directly. No GI reservoir store or load, no GI shade.
- **DI default:** Streaming RIS (initial sampling only, fused shade) vs ReSTIR DI Temporal-only (`Max History 4–8`, `Basic`, spatial off).
  - Measured on §2 at matched guide-capture convergence and stability results.
  - The faster one that passes becomes the Engine Target default, with the user's sign-off.
- **Gate:** Engine Target stack on the 5070 at render 1707×960 meets the §2.3 rows:
  - direct ≤ 1.5;
  - indirect ≤ 3.0;
  - cache ≤ 0.8;
  - ReSTIR GI 0;
  - DLSS-RR as P4 documents.

### P6 · Head-to-head against NVIDIA's demos

For each demo, the engine runs the analog preset (M8 §1):
- `RTXPT` (Vulkan);
- `RTXDI FullSample`;
- `RTXGI IndirectDiffuse`.

**Matching:** same scene (Sponza, Bistro, as each demo ships them), same render and display resolution, DLSS-RR on both sides, clocks locked.

**Record:** GPU frame time and FPS, mean and 1% low, from each demo's own timer and Nsight Systems. Our [Bench] numbers alone are not enough here: the demos do not log them.

**Gate:** the M8 promise, Engine Target and each analog preset at or above the demo's FPS on the 5070, recorded in the README's M8 tables. A loss stays recorded with its cause.

## 7. Results

Filled per step: the §1.1 table after each P-step, plus the Nsight per-pass metrics of §2.5.

| Step | Date | Columns changed | GPU total before → after | Notes |
|---|---|---|---|---|
| P1 + P2 `ONE_TIME_SUBMIT` | 2026-11-26 | courtyard NRC; Bistro NRC (startup view) | 13.98 → 12.04; 9.66 → 9.41 | NRC resolve 2.151 → 0.282 and 0.414 → 0.151 ms. Unlocked clocks, one round: DI shade (2.44 → 1.77) and Indirect (2.53 → 2.80) moved by clock noise, which P0 must pin down. Health readback verified with a temporary log (≈27k waves queried per frame, plausible divergence counts). GI 14/14; stability lines identical; core + sync + Best Practices + NVIDIA give the §1.3 set unchanged on Sponza and Bistro |

## 8. Risks

- **SDK-bound costs:** NRC train and DLSS-RR are SDK work. The engine changes inputs, sizes and overlap, not kernels. Their numbers may stay over §2.3, and the budgets get re-balanced (engine spec §2.3 says allocations, not measurements).
- **Async overlap:** NRC train on async compute needs cross-queue sharing of SDK buffers. If NRC 0.16 assumes one queue, P3 experiment 5b is dropped.
- **Precision:** any precision or sampling change (FP16, candidate counts, Temporal-only) can move bias. The GI check, guide capture and stability limits decide, not the timers.
- **Measurement drift:** this baseline was taken with unlocked clocks, one round per column. P0 re-takes it with §2.2 and 2.4 before any change is judged against it.

## 9. Sources

- NVIDIA, [Vulkan Dos and Don'ts](https://developer.nvidia.com/blog/vulkan-dos-donts) (updated 2025-01-14): submission, command buffers, barriers, memory, measurement.
- NVIDIA, [Best Practices for Using NVIDIA RTX Ray Tracing (Updated)](https://developer.nvidia.com/blog/best-practices-for-using-nvidia-rtx-ray-tracing-updated/): inline ray-query group sizes, live state, ray flags, coherence, AS builds.
- NVIDIA, [Advanced API Performance: Async Compute and Overlap](https://developer.nvidia.com/blog/advanced-api-performance-async-compute-and-overlap).
- NVIDIA, [Advanced API Performance: Barriers](https://developer.nvidia.com/blog/advanced-api-performance-barriers).
- NVIDIA, [Advanced API Performance: Shaders](https://developer.nvidia.com/blog/advanced-api-performance-shaders/).
- Khronos, Vulkan-ValidationLayers `layers/best_practices/*.cpp` at `vulkan-sdk-1.4.363.0`:
  - the 14 `BestPractices-NVIDIA-*` checks;
  - thresholds in `bp_constants.h`: Z-cull ratio 20, allocation reuse 5 s, 256 B fast descriptor space.
- `reference/RTXGI/Docs/NrcGuide.md`, `SharcGuide.md`; `reference/RTXDI/Doc/Integration.md`; `reference/RTXPT-main/Docs`.
