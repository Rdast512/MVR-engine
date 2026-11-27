# M8.5 · Lighting-stack GPU performance (NRC, SHaRC, RTXDI, RTXGI) and NVIDIA Vulkan practices

**Date:** 2026-11-27 (revised post-M8.3 & M8.4 audit; initial draft 2026-11-26) · **Status:** in progress — pre-M8.3 baseline (§1.1–§1.3), P1 + P2 `ONE_TIME_SUBMIT` (2026-11-26), and M8.3/M8.4 fast paths & SHaRC (§1.4, §7) landed; P0 locked-clock baseline, P2 barrier cleanup, P3–P4 tuning, P5 default pick, and P6 demo benchmark remaining
**Plan:** [M8 README](./README.md) step M8.5 · **Depends on:**
- M8.3 ([composable pipeline toggles](./m8_3_pipeline_toggles.md)) — **complete**;
- M8.4 ([SHaRC multi-bounce cache](./m8_4_sharc_cache.md)) — **complete**;
- P6 needs P5 (final Engine Target default and tuned presets).

**Budgets:** [engine spec §2.3](../../docs/engine_spec.md#23-frame-budget)
**References:**
- `reference/RTXGI` (`Docs/NrcGuide.md`, `Docs/SharcGuide.md`, `Samples/IndirectDiffuse`, `Samples/Pathtracer`);
- `reference/RTXDI` (`Doc/Integration.md`, `Samples/FullSample`);
- `reference/RTXPT-main` (`README.md`, `Docs/Design.md`, `Docs/intro-sample.md`, `Rtxpt/`);
- NVIDIA developer blog (§9);
- Vulkan-ValidationLayers `layers/best_practices/` at tag `vulkan-sdk-1.4.363.0` (installed SDK 1.4.363.1).

The renderer is correct, validated, and now ships composable runtime toggles (M8.3) plus both NRC and SHaRC radiance caches (M8.4), but the lighting stack is still over its §2.3 allocations on Bistro's interior poses at the reference tier's render resolution (`1707×960`). M8.3's fused Streaming RIS + Raw 1-Bounce PT and M8.4's SHaRC already closed several large overheads (removing `ReSTIR GI` off-cost, eliminating NRC's `0.33 ms` `Ray start wait` on Bistro, and cutting Bistro `cafe tables` from `25.1 ms` down to `17.9 ms` under `pipeline=rtxpt`). This spec does four things:
- fixes the locked-clock measurement protocol (P0);
- turns the NVIDIA Best Practices layer and NVIDIA's published Vulkan / RTX guidance into engine rules (§3);
- lists the remaining engine-wide and per-subsystem optimizations (P2–P4), each with a measured ABBA gate;
- picks the 1.0 Engine Target defaults (P5) and ends with the head-to-head FPS comparison against NVIDIA's demos (P6).

---

## 1. Baselines

### 1.1 Pre-M8.3 baseline (2026-11-26)

**Setup:**
- RTX 5070, driver 621.40, `windows-clang-release-checks`, no layers;
- `ENGINE_BENCH_FRAMES=6000`, `ENGINE_WINDOW_SIZE=2560x1440`: render 1707×960, DLSS-RR Quality;
- pre-M8.3 default settings: DI `Medium` (`ReGIR_RIS` temporal + spatial), `NRC` / `NRC + ReSTIR GI`, plain-indirect raster (`ENGINE_DGC` unset);
- mean ms, unlocked clocks, one round per column; scopes under 0.01 ms omitted.

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

The interior and street poses are the ones the café and glass checks use (`ENGINE_BENCH_POSE` accepts them since 2026-11-26). Measured after P1, pre-M8.3 NRC mode, one round each:

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

Inside the café the pre-M8.3 frame is **over the 16.6 ms total** on the reference GPU:
- ReSTIR DI (`Medium`) is 5–6× its allocation;
- the indirect pass is 2× its allocation;
- glass costs as much as the whole cache.

These Bistro poses are where M8.5 is decided. The Sponza columns stay as the lighter case.

Against the §2.3 reference allocations (which apply to this 1707×960 render size):

| Stage | Allocation | Measured (pre-M8.3 / post-M8.4) | Verdict |
|---|---|---|---|
| Direct lighting | 1.5 | `Medium` ReSTIR DI: 1.85–3.52 Sponza, 7.6–9.4 Bistro interior; `pipeline=rtxpt` Streaming RIS: **0.42 Sponza**, **2.05 Bistro `cafe tables`** | ReSTIR DI `Medium` is 5–6× over in café; Streaming RIS is within on Sponza and 0.55 ms over in café (P3, P5) |
| Indirect bounce 1 | 3.0 | Pre-M8.3: 1.52–2.53 Sponza, 5.9–6.7 Bistro interior; post-M8.4: 2.49–4.03 Sponza, **5.54** (`Sharc q=1`) – **8.60** (`Sharc q=2`) / **8.14** (`Nrc`) Bistro `cafe tables` | **#1 shader target**: `indirect.slang` grew into a 391-line uber-shader with both caches + 3 ray loops (P3 exp 1) |
| Radiance cache (update + train + resolve) | 0.8 | NRC (post-P1): 1.15–1.33 ms; SHaRC: **0.50–0.72 Sponza**, **1.30 (`Uniform`) – 1.99 (`ReGIR`) Bistro `cafe tables`** | SHaRC beats budget on Sponza; Bistro `sharc_update` inherits heavy primary DI sampling (P3/§5); NRC stays ~1.2–1.3 ms |
| ReSTIR GI (toggle) | 0.0, or 1.2 when on | **0.000 ms off (done in M8.3 `RawPt`)**; 0.93–1.89 ms when on | Off-cost closed; on-cost still over 1.2 ms in Sponza (`pipeline=rtxdi`) |
| DLSS-RR | 2.5 | 3.68–3.93 | over (NGX Transformer default; P4 measures presets D/E/F, modes & guide formats) |
| Glass (no own row; counted with resolve) | — | 0.43 outside; 1.84–3.16 at the café windows | needs optimization (P3 exp 5) and its own §2.3 budget line |
| Total | 16.6 | Sponza: **8.97 (`rtxpt`) – 12.92**; Bistro `cafe tables`: **17.87 (`rtxpt`)**, **23.20 (`Sharc q=1`)**, **25.11 (`Nrc`)** | Sponza well under 16.6; Bistro interior still over 16.6 (1.3 ms away on `rtxpt`, 6.6–8.5 ms away on `ReSTIR DI Medium`) |

### 1.2 First finding: GPU atomics into system memory (Fixed in P1, 2026-11-26)

**What happened:**
- `NrcCache` allocated `NrcHealth_<slot>` as host-visible + host-coherent + **host-cached**, which is system memory on NVIDIA. Every `InterlockedAdd` / `InterlockedMax` in `nrc_resolve.slang:countNrcHealth` (3 per wave) crossed PCIe.
- The sunlit courtyard had the most waves with traced light, paying ~1.9 ms.

**Probe** (2026-11-26, health address forced to 0, 3000 frames, courtyard, then reverted):

| Scope | Before | Probe |
|---|---|---|
| NRC resolve | 2.151 | 0.250 |
| GPU total | 13.98 | 11.72 |

`GlassCounters_<slot>` had the same memory type and was written via `waveCount` in `glass.slang`. **P1 fixed both** (`CounterReadback` in `vk_utils`, `C-M85-1` resolved): device-local counters + `vkCmdFillBuffer` clear + single copy to host-cached readback. Courtyard NRC resolve fell `2.151 → 0.282 ms` and Bistro startup `0.414 → 0.151 ms`.

### 1.3 NVIDIA Best Practices layer (2026-11-26; re-verified post-M8.4 2026-11-27)

**Setup:**
- SDK 1.4.363.1, 300 bench frames per mode;
- `VK_KHRONOS_VALIDATION_VALIDATE_BEST_PRACTICES=true`, `..._BEST_PRACTICES_NVIDIA=true`, `VK_KHRONOS_VALIDATION_REPORT_FLAGS=error,warn,perf`, duplicate limit 3.

| Message | Source | Owner | Status |
|---|---|---|---|
| `BestPractices-NVIDIA-BindMemory-NoPriority` | `vkBindBuffer/ImageMemory` without `VkMemoryPriorityAllocateInfoEXT` | **External (already attributed):** 54× NGX (`nv.ngx.dlssd.resource`, `C-H1-2`), 1× NRC 144 B UBO (`C-G3-3`). VMA sets priorities on every engine allocation; ImGui wrapped since 2026-11-21 (`C-T-4`) | closed (external) |
| `BestPractices-vkBind*Memory-small-dedicated-allocation` | small buffers/images in dedicated `VkDeviceMemory` | **External / accepted (already attributed):** 28× NGX (`C-H1-2`), NRC internals (`C-G3-3`), ImGui font atlas & small buffers (accepted in `C-T-4`) | closed (external/accepted) |
| `BestPractices-NVIDIA-CreateImage-Depth32Format` (1) | vis-buffer depth `D32_SFLOAT` | engine (`C-M1-1`) | accepted: reverse Z needs float depth (engine spec §8.2) |
| `BestPractices-NVIDIA-CreatePipelineLayout-SeparateSampler` (1) | `vkCreatePipelineLayout` | external (SDK / ImGui; engine uses shader objects + descriptor heaps) | none |
| `BestPractices-SpirvDeprecated_WorkgroupSize` (≥3) | `vkCreateComputePipelines` | external (NGX / NRC pipelines, `C-H1-2`) | none |
| `BestPractices-SyncObjects-HighNumberOfFences` (6+ > 3) | `vkCreateFence` | **External (already attributed, `C-T-4`):** engine creates only `MAX_FRAMES_IN_FLIGHT = 2` fences (`vk_frame_sync.cpp:18-21`, under the 3-fence threshold); NRC creates 8 per context + 1 per `Configure` | closed (external) |
| `SYNC-HAZARD-WRITE-AFTER-WRITE` ×2 | NGX DLSS-D | external (`C-H1-2`) | none |
| `VUID-vkDestroyDevice-device-05137` | NRC leaks | external (`C-G3-3`) | none |

**Post-M8.3 & M8.4 recheck (2026-11-27):** Static `pipeline=rtxpt`, `RestirGi + Sharc`, and live `Off ↔ SHaRC ↔ NRC` + resize cycling added **0 new engine VUID, sync, or Best Practices messages** ([`m8_4_sharc_cache.md` §9.4](./m8_4_sharc_cache.md#94-vulkan-validation--gpu-av)). `ONE_TIME_SUBMIT` was added to all per-frame command buffers in P2 (2026-11-26). The layer cannot see this frame's large costs (§1.1, §1.4); the source of truth is timers + Nsight.

### 1.4 Post-M8.3 / Post-M8.4 snapshot (2026-11-27)

**Setup:** RTX 5070, driver 621.40, `windows-clang-release-checks`, no layers, `ENGINE_WINDOW_SIZE=2560x1440` (render `1707×960`, DLSS-RR Quality), `ENGINE_BENCH_FRAMES=600` (unlocked clocks, ABBA mean; full details in [`m8_4_sharc_cache.md` §9.5](./m8_4_sharc_cache.md#95-abba-release-timings--lazy-vram-rtx-5070-25601440-quality--1707960-render-layers-off-engine_bench_frames600)). All rows use `RawPt` (`GiTemporal == 0.000 ms`, `GiShade == 0.000 ms`).

**Caveats (audit 2026-11-27, [`C-M85-3`](../considerations.md)):**
- `GPU total` is the post-2026-11-27 definition (no RayStartWait, no AcquireWait). §1.1's totals are the old one (= today's `GPU span`), so §1.1 and §1.4 totals do not compare directly, and NRC's RayStartWait is not in the NRC rows' totals.
- `DI Total` values marked `~` are copied from §1.1's pre-M8.3 sums, not measured in these runs. Only the bold `pipeline=rtxpt` values are measured.
- 600 frames is under the 2000-frame floor (±5% noise): deltas under ~5% of a total are not findings.

| Scene | Config | DI Total | SHaRC Upd+Res | Indirect | NRC Total | RayStartWait | GPU total |
|---|---|---:|---:|---:|---:|---:|---:|
| Sponza gallery | **`pipeline=rtxpt`** (`StreamingRis` U8 + `Sharc q=2`) | **0.416** | **0.502** | 3.219 | 0.001 | 0.014 | **8.966** |
| Sponza gallery | `RawPt + Sharc (q=1)` (`Medium` DI) | ~2.4 | 0.547 | **2.493** | 0.001 | 0.012 | **10.050** |
| Sponza gallery | `RawPt + Sharc (q=2)` (`Medium` DI) | ~2.4 | 0.542 | 3.334 | 0.001 | 0.013 | 10.925 |
| Sponza gallery | `RawPt + Nrc` (`idle=512`, `Medium` DI) | ~2.4 | 0.000 | 2.956 | 1.148 | 0.015 | 11.151 |
| Sponza gallery | `RawPt + Off` (`Medium` DI) | ~2.4 | 0.000 | 2.646 | 0.001 | 0.012 | 9.609 |
| Sponza courtyard | `RawPt + Sharc (q=2)` (`Medium` DI) | ~3.5 | **0.718** | 4.029 | 0.001 | 0.015 | 12.916 |
| Sponza courtyard | `RawPt + Nrc` (`idle=512`, `Medium` DI) | ~3.5 | 0.000 | 3.413 | 1.328 | 0.016 | 12.809 |
| Bistro `cafe tables` | **`pipeline=rtxpt`** (`StreamingRis` U8 + `Sharc q=2`) | **2.047** | 1.303 | 7.780 | 0.001 | **0.013** | **17.866** |
| Bistro `cafe tables` | **`RawPt + Sharc (q=1)`** (`Medium` DI) | ~9.3 | 1.946 | **5.536** | 0.001 | **0.014** | **23.196** |
| Bistro `cafe tables` | `RawPt + Off` (`Medium` DI) | ~9.3 | 0.000 | 8.068 | 0.001 | 0.012 | 23.509 |
| Bistro `cafe tables` | `RawPt + Nrc` (`idle=512`, `Medium` DI) | ~9.3 | 0.000 | 8.142 | 1.282 | **0.336** | 25.105 |
| Bistro `cafe tables` | `RawPt + Sharc (q=2)` (`Medium` DI) | ~9.3 | 1.986 | 8.603 | 0.001 | **0.013** | 25.972 |

**Four M8.5 leads from this snapshot (unlocked, 600 frames; each is retaken in P0 before it drives a change):**
1. **Direct lighting dominates Bistro:** `pipeline=rtxpt` (`StreamingRis` `2.05 ms`, zero reservoirs, `brdfSamples=0`, no temporal raytraced bias ray) totals **`17.87 ms`** on `cafe tables` against `25.11 ms` for `Medium + NRC`. That `-7.24 ms` mixes two changes (direct mode and cache); the direct-only delta is `pipeline=rtxgi` (same DI as `rtxpt`, NRC) against `Medium + NRC`, which P0 adds on every pose.
2. **`Indirect` grew since M8.3 (`6.20 → 8.07–8.60 ms` on Bistro `cafe tables`, `2.14 → 2.65–3.33 ms` on Sponza gallery, cache Off included); cause unproven ([`C-M85-5`](../considerations.md)):** one suspect is the uber-shader: `indirect.slang` compiles both `NRC_QUERY=1` and `SHARC_QUERY=1` plus three ray loops (`traceClosest` bounce-1, SHaRC bounce-2 continuation, `referencePathRadiance`) plus ReSTIR GI reservoir packing + secondary spatial resampling into one 391-line shader, and runs `nrcQueryContext`/`NrcCreatePathState` even when NRC is off. Other suspects: RawPt's glass reconnection transmittance moved into `indirect.slang` (it was in GI shade), unlocked clocks, 600- vs 6000-frame runs. P0 bisects it by building `0400dd037` (pre-M8.3) in the baseline worktree; a `.spv` swap cannot, because `GpuFrameRoot` grew 248 → 256 B. Specializing `indirect.slang` stays the top P3 target either way.
3. **`sharc.queryBounces=1` is the largest SHaRC lever:** skipping the bounce-2 fallback ray on ineligible short/glossy segments cuts Bistro `Indirect` from `8.603 ms` (`q=2`) and `8.068 ms` (`Cache Off`) to **`5.536 ms`**. The total-time margin over `RawPt + Off` (`23.196` vs `23.509 ms`) is inside noise. It is also IntroSample's behaviour (`IndirectPathTracer.hlsl:183-190` stops after one cache miss, [`C-M85-4`](../considerations.md)). Convergence- and stability-gated in §5.
4. **SHaRC removes NRC's `0.336 ms` `RayStartWait` on Bistro (`→ 0.013 ms`; visible in `GPU span`, not in the new `GPU total`), but `sharc_update` inherits primary DI's heavy sampling:** under `ReGIR_RIS`, `sharc_update` costs `1.72–1.76 ms` on Bistro (vs `1.07 ms` under `Uniform` 8 and `0.35–0.58 ms` on Sponza) because `sharc_update.slang` calls `directLightAt()` with the primary's full `initialSampling` settings at all 4 bounces.

---

## 2. Measurement protocol (P0)

Every item in §4–§6 is accepted on this protocol only.

1. **Build:** `windows-clang-release-checks` for `[Bench]` scoped runs (`-DFMT_MODULE=OFF` is fmt's own option and not in the engine presets: add it to the `-checks` preset with its reason, or drop it here), no layers (`VK_LOADER_LAYERS_DISABLE='*validation*'`), vkconfig override off (`VK_KHRONOS_VALIDATION_GPUAV_ENABLE=false`). Cross-check P6 wall FPS against `windows-clang-release` (no checks DLL) to confirm zero instrumentation delta.
2. **Clocks:** lock the GPU (`nvidia-smi -lgc <base>,<base>` from an admin shell, reset with `-rgc` afterwards) for scope work. Unlocked runs are allowed for exploratory/sanity checks only and must be marked as unlocked.
3. **Matrix** at `ENGINE_WINDOW_SIZE=2560x1440` (render `1707×960`, DLSS-RR Quality):
   - **Poses:** Sponza gallery, Sponza courtyard, and Bistro `cafe tables`, `cafe dining`, `street windows` (`ENGINE_MODEL=models/bistro/bistro.gltf ENGINE_BENCH_POSE=...`), not the startup view;
   - **Primary target & preset columns:**
     - `pipeline=targetRis` (`StreamingRis` `Power_RIS` 16 + `RawPt` + `NRC`) and its `SHaRC` counterpart (`frame.radianceCache=sharc`, both `q=2` and `q=1`);
     - `pipeline=targetTemporal` (`ReSTIR DI Temporal Basic` `Power_RIS` 8 + `RawPt` + `NRC`) and its `SHaRC` counterpart;
     - `pipeline=rtxpt` (`StreamingRis` `Uniform` 8 + `RawPt` + `SHaRC`);
     - `pipeline=rtxdi` (`ReSTIR DI Medium` + `ReSTIR GI` + `Off`);
     - `pipeline=rtxgi` (`StreamingRis` `Uniform` 8 + `RawPt` + `NRC`): differs from `rtxpt` only in the cache, so the pair is the apples-to-apples NRC vs SHaRC comparison;
     - Continuity column: shipped (`ReSTIR DI Medium` + `RawPt` + `NRC`) for before/after comparison against §1.1.
   - **Floor resolution check:** Sponza gallery & courtyard at `1920×1080` (render `1280×720`) as preview for M8.6.
4. **Order:** ABBA per column, `ENGINE_BENCH_FRAMES=6000` (or `2000`, the bench floor, during rapid P3 screening, promoted to `6000` for acceptance). A change counts when every pair moves the same way and the mean moves more than the round-to-round spread of that scope.
   - **Totals:** every table reports `GPU total` (work scopes only, since 2026-11-27) **and** `GPU span` (total + RayStartWait, comparable with §1.1), and names which one a claim uses. Cache and submit-structure comparisons use `GPU span`; P5/P6 frame-rate gates use `GPU span` and wall frame time, never `GPU total` alone.
   - **Changes below the noise:** a change whose expected gain is below the ABBA spread (barrier and access-mask trims) is accepted on clean validation + the Nsight Systems bubble it removes, plus "no worse" in ABBA.
5. **Per-pass hardware view:** Nsight Graphics GPU Trace of one frame per key column (`cafe tables`, `street windows`, `courtyard`), before and after each P-step. Record per pass in §7 (numbers, not screenshots):
   - SM throughput, L1TEX throughput, L2 throughput, VRAM throughput, RT core throughput;
   - warp occupancy and its limiter (registers, shared memory, or block size);
   - registers per thread (and local memory spill bytes, if any).
6. **Gaps and submits:** Nsight Systems over 300 frames. Record queue submits per frame, GPU idle between segments, and async-compute overlap. Current frame submit structure (post-MG-P7 / M8.4):
   - `submitComputeCommands()` on async compute (`rtScene.recordBuild` + `rtxdi.recordLightPreparation`, waiting on `rayTimeline[N-1]`);
   - `earlySubmits` on graphics (pre-acquire): `frameCmd` + all pure-Vulkan ray segments before the first SDK call (`DirectLight` only under NRC; **`DirectLight + SharcUpdate + SharcResolve + Indirect + Combine + Glass`** under SHaRC/Off, which also signals `rayTimeline[N]` pre-acquire so frame `N+1`'s async compute overlaps frame `N`'s DLSS-RR + Post + Present);
   - `postRaySubmit + presentCmd` on graphics (post-acquire): remaining SDK segments (`NRC BeginFrame`, `NRC Train`, `DLSS-RR`, each isolated per `C-G3-6`) + `Post` + `Present + UI`.
7. **Quality gate:** every change keeps the checks that cover it:
   - `ENGINE_CULL_CHECK=1` (`16/16`), `ENGINE_MOTION_TEST=1` (`13/13`), `ENGINE_DLSS_CHECK=1` (`11/11`);
   - `ENGINE_GUIDE_CAPTURE=1 ENGINE_RT_CHECK=1` (`3/3` DI convergence, `2/2` glass, `8,890/8,890` ray frames);
   - `ENGINE_GI_CHECK=1` (`14/14`);
   - `ENGINE_LAB` 8-bounce `giBias` (`sponza.gallery`, `sponza.courtyard`, `cafe.tables`, `darkLeak == 0`) for any indirect / cache / NEE change;
   - `ENGINE_LAB` café stability (`Lit` temporal std, flicker, pops, `C-I12-1`) for any DI / denoiser / target-stack change;
   - Stability lines identical when the change claims bit-exactness, otherwise within runner limits;
   - Validation (core + sync + Best Practices + NVIDIA, `REPORT_FLAGS=error,warn,perf`) stays at the §1.3 set (zero engine errors/warnings) and GPU-AV stays clean.

`tools/` gets a bench script (`tools/bench_m85.ps1`) that runs the §2 matrix in ABBA order, parses `[Bench]` scopes, and prints the markdown table.

---

## 3. NVIDIA Vulkan / RTX rules for this engine

Checked against the post-M8.4 codebase (`ok` = followed, `gap` = work in §4–§6).

| # | Rule | Source | Status |
|---|---|---|---|
| N1 | No GPU atomics or frequent GPU writes into host-cached (system) memory; accumulate in device-local memory, copy once to readback | Dos and Don'ts; §1.2 | **ok** since P1 (`CounterReadback`, `C-M85-1`) |
| N2 | `ONE_TIME_SUBMIT` on every command buffer recorded per submit | Dos and Don'ts | **ok** since P2 (2026-11-26; `vk_renderer.cpp:648, 797, 829, 854`) |
| N3 | Few submits per frame; no tiny command buffers (each costs driver work, and compute cannot overlap across a boundary) | Dos and Don'ts | **partial:** SHaRC/Off already collapse all lighting into 1 pre-acquire segment (only DLSS-RR splits); NRC still opens 2 extra SDK segments (`C-G3-6`), measured in P0.6 |
| N4 | Barriers: one `pipelineBarrier2` per transition point, tightest stages, minimum access flags, no read-to-read or back-to-back duplicate barriers | Advanced API Performance: Barriers | **gap (concrete hits found in audit):** duplicate barriers in `vk_indirect.cpp:17-38`, overly broad `crossFrameBarrier` in `vk_sharc.cpp:227-234`, unused `eShaderSampledRead` in `vk_renderer.cpp:1138-1144`, unconditional `recordDebugView` barrier on Lit frames (`vk_renderer.cpp:1512-1518`) — fixed in P2 |
| N5 | Full-screen compute: 8×8 to 16×16 groups (2–8 warps); inline ray-query compute: 8×8 rule of thumb, 16×8 practical choice, ≥3 warps/group if registers allow | Shaders; RTX best practices | ok (`8×8` everywhere); P3 exp 2 tests `16×8` on ray passes (`indirect`, `rtxdi_streaming_ris`, `rtxdi_shade`, `rtxdi_initial`, `sharc_update`, `glass`) |
| N6 | Low live state & zero dead branches across `TraceRayInline` / `Proceed`: live registers and extra ray loops crush occupancy | RTX best practices | **gap (major):** `indirect.slang` (391 lines) compiles both NRC + SHaRC + 3 ray loops + ReSTIR GI into one shader and runs `nrcQueryContext` unconditionally; P3 exp 1 splits/specializes and audits VGPRs in Nsight |
| N7 | Thread-group swizzling for L1/L2 and ray coherence in full-screen compute | Shaders; RTX best practices | **gap** (row-major today); P3 exp 3 |
| N8 | Opaque geometry and compile-time ray flags; `ACCEPT_FIRST_HIT_AND_END_SEARCH` for visibility rays; no face culling unless needed | RTX best practices | **ok** (`trace.slang`, OMM 2-state, `FORCE_OPAQUE`) |
| N9 | Avoid divergent shading in ray passes; avoid deep ID → buffer → index indirection chains (`RegirParametersView` BDA loads, `C-F1-3`) | RTX best practices | partial: primary resolve is binned (M7 E1); ray passes shade all materials inline (P3 exp 4 measures divergence and secondary NEE cost) |
| N10 | Async compute pairs: complementary units, never two RT-core-bound passes together; async work must finish before its dependency | Async Compute and Overlap | **partial:** SHaRC already drops Bistro `RayStartWait` `0.336 → 0.013 ms` via pre-acquire `rayTimeline` signaling (`isPreSdkRaySignal = true`); NRC still waits `0.336 ms` on Bistro (P3 exp 6) |
| N11 | Memory priority on every allocation; `pageable_device_local_memory` enabled; stay inside `VK_EXT_memory_budget` | Dos and Don'ts; BP-NVIDIA | **ok** for engine allocations (VMA + ImGui); SDK allocations external (`C-H1-2`, `C-G3-3`) |
| N12 | Suballocate; no small dedicated allocations | Dos and Don'ts; BP | **ok** for engine (VMA); all §1.3 hits already attributed to NGX/NRC/ImGui (`C-H1-2`, `C-G3-3`, `C-T-4`) |
| N13 | FP16: no general ALU gain on Ampere+, 2× on Turing (RTX 2060 Super floor); pack data in smallest type that fits | Shaders | candidate for floor tier (P3 exp 7, measured on 2060 Super in M8.6) |
| N14 | Prefer wave intrinsics to shared memory; coalesce atomics per wave | Shaders | **ok** (`waveCount` skips its atomic on a zero wave count, `countNrcHealth`) |
| N15 | Specialization constants for per-session / per-mode constant features to cut registers and branches | Shaders; Dos and Don'ts | partial (`kDebugRays`, `kIsSerReorderEnabled`); P3 exp 1 promotes cache mode, `isRawIndirect`, `giReferenceBounces == 0`, `isCacheView`, `kGiFlagSecondaryResampling`, `kGlassFlagShadowTransmittance` |
| N16 | Benchmark without layers, clocks locked, over many frames; keep debug-utils labels in release | Dos and Don'ts | **ok** (§2) |
| N17 | Depth: `D24_UNORM_S8` or `D32_SFLOAT`, never `D32_SFLOAT_S8` | Dos and Don'ts; BP | **ok** (`D32_SFLOAT` for reverse Z, accepted `C-M1-1`) |

---

## 4. Engine-wide steps

### P1 · No GPU writes into system memory (N1) — **DONE (2026-11-26, `C-M85-1`)**

- `NrcHealth` and `GlassCounters` moved to device-local `CounterReadback` (`vk_utils`), cleared via `vkCmdFillBuffer` and copied once to host-cached readback.
- Audited all remaining `eHostCached` buffers (`vk_rt_scene.cpp`, `vk_mega_geometry.cpp` are copy targets; `check_passes.cpp` checks-only).
- **Result:** courtyard NRC resolve `2.151 → 0.282 ms`, Bistro startup `0.414 → 0.151 ms`; divergence detector, GI `14/14`, stability lines, and validation all clean (§7).
- **Rule (permanent):** shaders never write `eHostCached` memory.

### P2 · Command buffers, barriers, and validation hygiene (N2–N4, N11, N12)

**Already done:**
- `ONE_TIME_SUBMIT` on frame, ray-segment, present, and compute command buffers (2026-11-26).
- §1.3 `NoPriority`, `small-dedicated`, and `HighNumberOfFences` hits attributed (`C-H1-2`, `C-G3-3`, `C-T-4`); engine owns only 2 `InFlightFence`s (`vk_frame_sync.cpp:18-21`).

**Remaining barrier audit & fixes (verified in code):**
1. **`vk_indirect.cpp:17-38`:** remove the unconditional `inputBarrier` and `outputBarrier` wrapped around `indirect.slang`. Expected gain is small: `nrc_update` costs `0.02–0.04 ms`, so overlapping it with `indirect` saves at most that, and each removed barrier is a µs-scale drain. That is below the ABBA spread, so this step is accepted under §2 step 4's rule for changes below the noise, not on a measured ABBA win.
   - `inputBarrier` fires redundantly after `sharc.recordResolve` (`vk_sharc.cpp:285-291`), after NRC BeginFrame's closing `nrcBoundary` (`vk_nrc.cpp:445-448`, ALL_COMMANDS), and after `rayStartBarrier` (`vk_renderer.cpp:731`) when `Direct == Off && Cache == Off`. It also serializes `nrc_update` against `indirect`, which `NrcGuide.md` states are independent.
   - **Replacement:** a `Direct → Indirect` barrier only when `Direct == RestirDi && isSecondaryResamplingEffective()` (the sole case where `indirect` reads DI reservoirs). Record it right after `recordDirectLight`, not inside `recordIndirectPass` (which would re-serialize `nrc_update` against `indirect`). Skip it when a barrier already sits between the passes (NRC's BeginFrame `nrcBoundary`; SHaRC's `resolveToQuery`). Edge case: SHaRC with `sharc.update=off`, `sharc.resolve=off` and no pending clear records no SHaRC barrier, so the DI barrier must still fire there.
   - `outputBarrier` is redundant on every path: `nrc.recordTrain` opens with `nrcBoundary` (`vk_nrc.cpp:475`), `recordIndirectResampling` opens with `reservoirBarrier` (`vk_renderer.cpp:1288`), `recordReconstruction` opens with `combineBarrier` (`vk_renderer.cpp:1318`), and NRC's debug resolve orders `indirectRadiance` with its own pre-blit barrier (`vk_nrc.cpp:536-556`). Each successor keeps a comment naming the indirect writes it orders.
2. **Done in M8.4 (`e84efcef5`, C-M84-7 resolved): the compute → clear barrier now runs only `if (isClearPending)` in `SharcCache::recordUpdate`.** Original item: **`vk_sharc.cpp:227-234` (`crossFrameBarrier`):** it runs after `DirectLight` and stalls `sharc_update` behind `DirectLight`'s writes, which it does not read. `rayStartBarrier` already orders the previous frame's compute work before this frame's compute dispatches; only the `vkCmdFillBuffer` clear needs more. Emit it only when `isClearPending` (dst `eClear` / `eTransferWrite`), or fold that case into `rayStartBarrier` when `isSharcRecorded`.
3. **`vk_renderer.cpp:1138-1144` (`resamplingBarrier`):** drop `eShaderSampledRead` from the DI `initial → temporal → shade` barriers: reservoirs are read via BDA (`eShaderStorageRead`), not sampled images. Hygiene only: access-mask trims have no measurable cost on NVIDIA.
4. **`vk_renderer.cpp:1512-1518` (`recordDebugView`):** skip `guideBarrier` when `isLitPresented() && frameHook == nullptr` (nothing executes between `Post` and `Present`'s `displayBarrier`).
5. **Gate:** Sync validation + Core + Best Practices + NVIDIA (`REPORT_FLAGS=error,warn,perf`) clean across `Off`, `NRC`, `SHaRC`, `RestirGi`, `StreamingRis`, `RestirDi` (with & without secondary resampling); Nsight Systems confirms fewer pipeline stalls and `nrc_update ∥ indirect` overlap; GPU total and CPU `record`/`submit` no worse.

### P3 · Ray-pass occupancy, coherence, glass, and overlap (N5–N7, N9, N10, N13–N15)

Targets are the largest ray passes at Bistro's interior poses (`cafe tables`, `cafe dining`, `street windows`):
- **Indirect:** `5.54–8.60 ms` on Bistro `cafe tables` (`2.49–4.03 ms` Sponza);
- **Direct lighting:** `2.05 ms` (`StreamingRis` U8) to `7.6–9.4 ms` (`ReSTIR DI Medium`) on Bistro;
- **Glass:** `1.84–3.16 ms` on Bistro (plus `kGlassFlagShadowTransmittance` shadow walks inside DI and Indirect);
- **SHaRC update:** `1.07–1.76 ms` on Bistro (`0.35–0.58 ms` Sponza).

**Experiments** (one at a time, locked clocks, ABBA, kept only on a measured win):
1. **Shader specialization & register/occupancy reduction (`indirect.slang` first, then `rtxdi_streaming_ris`, `rtxdi_shade`, `rtxdi_initial`, `sharc_update`, `glass`):**
   - Capture Nsight registers/thread, local-memory spill bytes, and occupancy limiter for every ray pass before editing.
   - **Split or compile-time specialize `indirect.slang` (391 lines):** today one SPIR-V binary contains `-DNRC_QUERY=1`, `-DSHARC_QUERY=1`, the SHaRC bounce-2..N continuation loop, `referencePathRadiance` (another full bounce loop), `secondaryDirectLight` with `RTXDI_DISpatialResampling`, `ReSTIR GI` reservoir packing, and `RawPt` direct write — and executes `nrcQueryContext` / `NrcCreatePathState` even when `Cache != Nrc`! Specialize (separate entry points / compile defines or specialization constants) into lean variants:
     - `RawPt + Off`, `RawPt + Nrc`, `RawPt + Sharc (q=1)`, `RawPt + Sharc (q≥2)`, `RestirGi` (plus `Reference` / `CacheView` out of the hot shader).
   - **Shrink live state across `traceClosest` / `traceVisibility` (N6):** avoid keeping unpacked primary `RAB_Surface` or unused RNG/reservoir structs live across ray queries; load material/guide data only after the hit/branch needs it.
   - **Specialization constants (N15):** promote mode/session-constant branches (`giReferenceBounces == 0`, `isCacheView`, `kGiFlagSecondaryResampling`, `kGiFlagSecondaryRoughnessFloor`, `kGiFlagFireflyClamp`, `kGlassFlagShadowTransmittance`, `localLightSamplingMode`).
2. **Workgroup shape (N5):** test `16×8` (4 warps) vs `8×8` (2 warps) on `indirect`, `rtxdi_streaming_ris`, `rtxdi_shade`, `rtxdi_initial`, `sharc_update`, and `glass`.
3. **Thread-group swizzling (N7):** 8×8 / Morton tile-swizzled dispatch coordinates for full-screen ray passes so adjacent warps trace spatially coherent rays and share L1/L2 texture + BVH cache lines.
4. **Hit shading divergence & decoupled secondary NEE (N9):**
   - Measure `sampleLightsAt()` (`path_shading.slang:57-78`) cost in `indirect.slang` and `sharc_update.slang`: today secondary vertices inherit primary DI's full `localLightSamplingMode` and candidate count (e.g. `ReGIR_RIS` 8 or `Power_RIS` 16–32), which drives Bistro `sharc_update` to `1.76 ms` and bloats `indirect`. Test decoupling secondary/cache-update NEE (`Power_RIS` 2–4 vs `Uniform` 4–8 vs `ReGIR_RIS` 4) gated on 8-bounce `giBias` and café `Lit` stability. Reference value: RTXPT decouples its SHaRC training pass the same way (`SharcIntegration.h:56`, `NEEFullSamples = 1` over the main path tracer's setting). Sharing the primary's mode follows the RTXDI sample's `neeLocalLightSampling` (`path_shading.slang:55-56`), so that set stays callable next to any decoupled one.
   - Measure branch efficiency of `shadeRayHit` + `secondaryDirectLight` in `indirect`; only split into trace → material-binned shade if Nsight shows severe divergence and the prototype wins ≥ 15%.
5. **Glass pass & shadow transmittance (`1.84–3.16 ms` on Bistro):**
   - Measure the hidden cost of `kGlassFlagShadowTransmittance` inside `rtxdi_streaming_ris`, `rtxdi_shade`, and `indirect` (toggle off vs on in Bistro café) and test skipping `glassTransmittance` when ray `tMax` or scene bounds cannot intersect glass, or using `ACCEPT_FIRST_HIT_AND_END_SEARCH` / simplified opacity for distant shadow rays if bias stays within glass check limits.
   - In `glass.slang`: no counter gate. `waveCount` already skips its atomic when the wave's count is 0 (`counters.slang:5-12`), so a glass-free wave pays only 11 `WaveActiveCountBits`. The pass cost is the layer walk and NEE. Test cheaper NEE candidate count in `reflectedRadiance` and `diffuseShare`; sweep `glassMaxLayers` (`4` vs `2`) against Bistro `street windows` quality.
   - Add an explicit Glass allocation row to `docs/engine_spec.md §2.3`.
6. **Async compute overlap (N10):**
   - SHaRC/Off already cut Bistro `RayStartWait` from `0.336 → 0.013 ms` because `isPreSdkRaySignal = true` lets frame `N+1`'s async compute (PTLAS + light prep) overlap frame `N`'s DLSS-RR (`~3.8 ms`).
   - For NRC mode (where `isPreSdkRaySignal = false` keeps `RayStartWait` at `0.336 ms`): check Nsight Systems whether `light_world` / `light_pdf_mips` / `rtxdi_presample` can be reduced (`Power_RIS` vs `ReGIR_RIS` removes the `rtxdiPresampleRegir` dispatch!) or started earlier.
   - For `nrc_update ∥ indirect`: removing `vk_indirect.cpp:17-24` (P2) already lets them overlap on the graphics queue with zero cross-queue complexity.
   - **Caveat (`C-G3-6`):** do **not** run NRC `QueryAndTrain` or DLSS-RR concurrently on async compute against graphics submits without re-running the 18,000-frame `C-G3-6` uncapped stress test (driver 621.x previously lost the device on concurrent submits while CUDA kernels ran). Note `RTXPT 1.9.0` achieves async DLSS-RR overlap on D3D12/Vulkan with Streamline; test only if driver stability holds.
7. **Floor-tier FP16 (N13):** evaluate `half` packing in reservoirs/radiance math on RTX 2060 Super in M8.6 (RTX 5070 must not regress).

**Gate:** per experiment, ABBA mean drops by more than spread on target poses, no other pass rises, and the §2.7 quality gate holds.

### P4 · DLSS-RR (largest single fixed scope, 3.68–3.93 ms at 1707×960 → 2560×1440)

NGX is closed-source, so the engine controls preset, mode, inputs, and surrounding synchronization:
1. **Confirm zero wasted work around evaluate:** verify in Nsight Systems / GPU Trace that `Dlss` scope contains only NGX evaluate (no hidden layout round-trips, copies, or stalls from `recordIsolated` + `driverHeap.bind`).
2. **Measure DLSS-RR Render Presets (`Default` / `D` / `E` / `F`):** `vk_dlss.cpp:45-58` already exposes presets D/E/F; measure GPU ms and `Lit` temporal stability / sharpness across presets (Transformer vs CNN models differ strongly on both RTX 5070 and RTX 2060 Super).
3. **Measure `Quality` / `Balanced` / `Performance` modes** on the `2560×1440` matrix as reference rows (default stays `Quality` per `engine_spec.md §2.1` unless the user decides otherwise).
4. **Measure optional guide & format bandwidth:**
   - Measure `frame.dlss.specHitDist=off` vs `on` (`R16F` specular hit distance write in `indirect.slang` + NGX read);
   - Audit guide formats (`vk_render_targets.cpp:16-23`: `NormalRoughness` `RGBA16F`, `Motion` `RGBA16F`, `DiffuseAlbedo`/`SpecAlbedo` `RGBA8`) against `reference/vk_denoise_dlssrr` minimums (e.g., whether `Motion` `RG16F` saves 4 B/px × 2 ping-pong targets if `.z` depth delta moves to reconstruction or stays in `Surface` guide).
5. **Verify `CreateFeature` flags** (`vk_dlss.cpp:246-258`) against NVIDIA's `vk_denoise_dlssrr` sample for RR (HDR, MVLowRes, DepthInverted, Roughness Packed, HWDepth).

**Gate:** documented per-preset and per-mode cost + bandwidth table; no change to default mode/preset without user sign-off.

---

## 5. Subsystem steps

### SHaRC (M8.4 landed — primary pure-Vulkan cache candidate)

| Item | Now (2026-11-27) | M8.5 Action | Gate |
|---|---|---|---|
| Query bounces (`q=1` vs `q=2`) | `q=2` default: Bistro `Indirect` `8.603 ms` (slower than `Cache Off` `8.068 ms` on every pose, [`C-M84-2`](../considerations.md)); **`q=1`: `5.536 ms`** | Measure `q=1` (and glossy-only bounce-2) 8-bounce `giBias` across Sponza + Bistro + glossy scenes, and DLSS-RR `Lit` stability. If convergence holds, adopt `q=1` (or glossy-only `q=2`) as default. `pipeline=rtxpt` takes `q=1` regardless, as IntroSample does ([`C-M85-4`](../considerations.md)) | `8b.bias` / `p90` + `darkLeak == 0` + visual check |
| Update light sampling (`sharc_update`) | `1.72–1.76 ms` on Bistro under `ReGIR_RIS` vs `1.07 ms` under `Uniform` 8 (`0.35–0.58 ms` Sponza) | Decouple `sharc_update.slang`'s NEE candidate mode/count from primary DI (test `Power_RIS` 2–4 vs `Uniform` 4–8, `brdfSamples=0`) | Target Bistro `sharc_update ≤ 0.6 ms` with equal 8-bounce `giBias` |
| Update density & bounces | `downscale=4` (1/16 px), `maxUpdateBounces=4` | Sweep `downscale=4,5` (`5` is `SharcCache::kMaxDownscaleFactor` and RTXPT's `DownScaleFactorMax`) and `maxUpdateBounces=2,3,4` (IntroSample traces 2 update vertices) against 8-bounce `giBias` and dynamic-light responsiveness | Keep `Upd + Res ≤ 0.8 ms` on Sponza and Bistro |
| Capacity & resolve cost | `capacityShift=22` (4M entries, `160 MiB`, `sharc_resolve` `0.14–0.23 ms`) | Sweep `capacityShift=21` (2M, `80 MiB`, `resolve ~0.11 ms`) on Sponza & Bistro; needs the occupancy counter first ([`C-M84-4`](../considerations.md)), plus `collisions` / `occupancy` debug modes | Zero visible hash-collision artifacts, measured occupancy headroom, `-80 MiB` VRAM |
| Barriers | `vk_sharc.cpp:227-234` + duplicate `vk_indirect.cpp:17-24` | Clean up in P2 | Sync validation clean, fewer stalls in Nsight |
| RTXPT parity (`pipeline=rtxpt`) | engine SHaRC defaults (`q=2`, `32`/`64` frames, `4` update vertices, RTXGI eligibility) | Give the analog the chosen demo's SHaRC set ([`C-M85-4`](../considerations.md)); the engine tuning stays callable | Required before P6 |
| Open M8.4 defects | `C-M84-3` (`separateEmissive=off` double count), `C-M84-5` (absolute firefly clamp), `C-M84-6` (`resolve=off` corruption) | Fix or measure alongside the SHaRC rows above | Per entry's exit |

### NRC (RTXGI 0.16)

| Item | Now | Change | Reference |
|---|---|---|---|
| Health atomics | **Done (P1, 2026-11-26)** | Device-local `CounterReadback`; courtyard `2.15 → 0.28 ms` | §1.2, `C-M85-1` |
| RawPt resolve | **Done (M8.3)** | Writes `indirectRadiance` directly without GI reservoir round-trip | [`m8_3_pipeline_toggles.md`](./m8_3_pipeline_toggles.md) |
| Update ∥ query | `vk_indirect.cpp:17-24` injects an unnecessary barrier between `nrc_update` and `indirect` | **P2:** remove `vk_indirect.cpp:17-24` so update and query overlap on graphics | `NrcGuide.md` ("The update pass is independent of the query pass") |
| SDK isolation | 2 extra ray segments (`BeginFrame`, `Train`), delays `rayTimeline` post-acquire (`+0.32 ms` `RayStartWait` on Bistro) | P0.6 measures exact segment gap; check if NRC 0.16 allows fewer splits without tripping `C-G3-6` | `NrcGuide.md` Step 4, `C-G3-6` |
| Training resolution & iterations | `computeIdealTrainingDimensions(frame, 0)`, `idleTrainingFrames=512` (`1.15–1.33 ms` total) | Sweep `trainingIterations` & `avgTrainingVerticesPerPath` vs `RTXGI/Samples/IndirectDiffuse` defaults against `giBias` and `Train` ms | `NrcGuide.md` Step 5, `IndirectDiffuse` |

### RTXDI (Streaming RIS & ReSTIR DI)

| Item | Now | M8.5 Action |
|---|---|---|
| Fused Streaming RIS | **Done (M8.3):** `passes/rtxdi_streaming_ris.slang`, zero DI reservoirs (`0.42 ms` Sponza, `2.05 ms` Bistro `cafe tables` under `Uniform` 8) | P3 exp 1–3 on `rtxdi_streaming_ris.slang`; sweep `Power_RIS` 8 vs 16 vs `Uniform` 8 with exact sun + per-pixel tiles on `ENGINE_LAB` café bias & stability (`C-I12-1`) |
| Temporal-only `Basic` ReSTIR DI | **Done (M8.3):** `pipeline=targetTemporal` (`Power_RIS` 8, `brdfSamples=0`, `Max History 6`, `Basic`, Spatial OFF, exact sun) | Measure full §2 ABBA cost + `ENGINE_LAB` café bias & stability (`C-I12-1`, incl. `isPermutationSamplingEnabled` on/off and `Max History 4–8`) vs Streaming RIS |
| ReGIR vs `Power_RIS` | `Medium` uses `ReGIR_RIS` (`3.83 ms` initial on Bistro + async presample cost + `C-F1-3` BDA view) | Compare `Power_RIS` vs `ReGIR_RIS` vs `Uniform` at equal café quality (guide capture + lab); `Power_RIS` also removes `rtxdiPresampleRegir` from async compute |
| Reservoir bandwidth | 3 buffers (`28.5 MiB` at `853×480`, `112.8 MiB` at `1707×960`, `0 MiB` in Streaming RIS via M8.3 lazy retirement) | If Temporal-only ReSTIR DI wins P5, measure L2/VRAM share in GPU Trace and evaluate 2-buffer ping-pong (`112.8 → 75.2 MiB`) |
| `ReSTIR DI Medium` (`pipeline=rtxdi`) | `7.6–9.4 ms` on Bistro interior | P3 exp 1–3 on `rtxdi_initial`, `rtxdi_temporal`, `rtxdi_shade` + P2 barrier tightening |

### ReSTIR GI (optional toggle) and Indirect Bounce 1

| Item | Now | M8.5 Action |
|---|---|---|
| GI with reuse off (`RawPt`) | **Done (M8.3):** `0.000 ms` (`GiTemporal` & `GiShade` skipped, GI reservoirs lazily retired) | Keep at `0.000 ms` |
| Indirect (`indirect.slang`) | `2.49–4.03 ms` Sponza, `5.54–8.60 ms` Bistro `cafe tables` | **P3 exp 1–4** (uber-shader split/specialization, live-state reduction, 16×8, swizzle, decoupled $X_2$ NEE, `sharc.queryBounces=1`) |
| GI on (`pipeline=rtxdi`) | `0.93–1.89 ms` (`temporal 0.27–0.47`, `shade 0.66–1.43`) | P3 exp 1–3 on `rtxdi_gi_temporal` / `rtxdi_gi_shade` to bring Sponza within the `1.2 ms` toggle budget |

---

## 6. Target stack selection and demo comparison

### P5 · Target-stack shootout and 1.0 default sign-off

**Already landed in M8.3 & M8.4:**
- Zero-cost off (`< 0.001 ms`, no dispatches/barriers, lazy VRAM allocation & `MAX_FRAMES_IN_FLIGHT`-deferred retirement for DI reservoirs, GI reservoirs, `giStagnancy`, `giDuplication`, `rawIndirectWeight`, and SHaRC buffers).
- `RawPt` direct write (`indirect.slang` + `nrc_resolve.slang` write `indirectRadiance` directly, zero GI reservoir round-trip).
- Fused `StreamingRis` (`passes/rtxdi_streaming_ris.slang`), `pipeline=targetRis`, `pipeline=targetTemporal`, and `RadianceCacheMode::Sharc`.

**M8.5 decision matrix (after P2–P4 land):**
1. **Direct default shootout:** `StreamingRis` (`Power_RIS` 8 / 16) vs `ReSTIR DI Temporal-only` (`Power_RIS` 8, `Max History 4–8`, `Basic`, Spatial OFF, `isPermutationSamplingEnabled` on/off):
   - Compare locked-clock §2 ABBA GPU ms, guide-capture DI convergence, and `ENGINE_LAB` café bias + `Lit` temporal stability / flicker / pops (`C-I12-1`).
2. **Cache comparison:** `NRC` (`idleTrainingFrames=512`) vs `SHaRC` (tuned `q=1`/`q=2`, decoupled `sharc_update` NEE, `capacityShift=21`/`22`), on equal DI (`pipeline=rtxgi` vs `pipeline=rtxpt`, and each Engine Target with `frame.radianceCache=sharc`):
   - Compare locked-clock §2 ABBA `GPU span` and wall frame time (`GPU total` hides NRC's RayStartWait), `Indirect`, 8-bounce `giBias`, dynamic-light responsiveness, VRAM, and clean shutdown (SHaRC has no `C-G3-3` SDK leaks and no `C-G3-6` CUDA isolation).
   - **Open decision ([`C-M85-6`](../considerations.md)):** the 1.0 target cache is NRC, with SHaRC as a toggle (2026-11-25). The comparison is recorded either way; proposing SHaRC as the Engine Target cache needs the user's decision first.
3. **Update the `Engine Target` presets** (`pipeline=targetRis` / `targetTemporal`: `src/render/settings_registry.cpp`, `src/render/vk_rtxdi.cpp`, `docs/engine_spec.md §2.3`) with the winning direct mode upon user sign-off. Shipped settings stay NVIDIA's (DI `Medium`, NRC), with the tuned sets callable beside them.
4. **Gate:** Engine Target stack on RTX 5070 at `1707×960` render (`2560×1440` Quality) meets §2.3 budgets:
   - direct ≤ `1.5 ms` (Sponza; target ≤ `1.5–2.0 ms` Bistro interior);
   - indirect ≤ `3.0 ms` (Sponza; target ≤ `3.5–4.0 ms` Bistro interior);
   - cache ≤ `0.8 ms`;
   - ReSTIR GI = `0.0 ms`;
   - Glass and DLSS-RR per P3 exp 5 / P4 documented allocations;
   - **`GPU span` ≤ `16.6 ms` and wall frame time ≤ `16.6 ms`** (≥ 60 FPS) on both Sponza **and** Bistro interior poses (`GPU total` alone leaves out idle gaps, so it does not prove 60 FPS).

### P6 · Head-to-head against NVIDIA's demos

For each demo, the engine runs the matching analog preset (`M8 §1`) plus the winning **Engine Target** preset:

| NVIDIA Demo | Demo Config / Notes (verified in `reference/`) | Matching Engine Preset | Known Parity Differences (recorded in table) |
|---|---|---|---|
| **`RTXPT` v1.9.0** (`reference/RTXPT-main`) | Ships two executables: `IntroSample` (deferred G-buffer + Streaming RIS / ReSTIR DI + 1-bounce PT + SHaRC — closest architectural match) and `Rtxpt` (unified PT + NEE-AT + SER + OMM + SHaRC). Note: `RTXPT` README marks Vulkan SER/OMM as WIP, so test **D3D12** (full features) and **Vulkan** where supported | `pipeline=rtxpt` (`StreamingRis` U8 + `RawPt` + `SHaRC`) + **Engine Target** | `C-M83-1`: engine's Streaming RIS samples unified local lights (punctual + emissive triangles) where `IntroSample` samples analytic lights only; our SER is shelved post-1.0 (`C-G1-3`). **`C-M85-4`:** SHaRC settings differ from IntroSample (query `q=2` + RTXGI footprint test vs `q=1` + `hitDistance >= voxelSize`; accumulation/stale `32`/`64` vs `64`/`16`; `4` vs `2` update vertices) and from `Rtxpt` (`SharcIntegration.h:46-60`). P6 runs only after `pipeline=rtxpt` carries the chosen demo's set |
| **`RTXDI FullSample` 3.2.0** (`reference/RTXDI`) | Donut Vulkan & D3D12, Bistro (`RTXDI-Assets`), ReSTIR DI `Medium` + ReSTIR GI + DLSS-RR | `pipeline=rtxdi` (`ReSTIR DI Medium` + `ReSTIR GI` + `Off`) + **Engine Target** | Engine uses visibility buffer + Mega Geometry CLAS/PTLAS + descriptor heaps instead of traditional G-buffer + TLAS |
| **`RTXGI IndirectDiffuse` 2.9.0** (`reference/RTXGI`) | Donut Vulkan & D3D12, NRC (and `Samples/Pathtracer` for SHaRC/NRC); does not ship DLSS-RR in `IndirectDiffuse` by default | `pipeline=rtxgi` (`StreamingRis` U8 + `RawPt` + `NRC`) + **Engine Target** | `C-M83-1` unified light buffer; compare **both** denoiser-off lighting frame time (apples-to-apples) and full denoised FPS |

**Matching:** same scene (Sponza, Bistro, matching camera pose & FOV as closely as each demo allows), same render (`1707×960`) and display (`2560×1440`) resolution, matched denoiser state (DLSS-RR Quality where both support it, plus denoiser-off lighting comparison where a demo lacks DLSS-RR), clocks locked (`nvidia-smi -lgc`), Release build (`windows-clang-release` cross-checked against `release-checks`), validation layers off.

**Record:** GPU frame time (ms) and FPS — mean and 1% low — from each demo's built-in profiler and **Nsight Systems** (our `[Bench]` log alone is insufficient because the demos do not emit it).

**Gate:** Engine Target and each analog preset at or above the corresponding NVIDIA demo's FPS on RTX 5070, recorded in §7 and [`spec/m8/README.md`](./README.md). Any loss stays recorded transparently with its Nsight root cause.

---

## 7. Results

Filled per step: ABBA GPU timings plus Nsight per-pass metrics (§2.5).

| Step | Date | Columns changed | GPU total before → after | Notes |
|---|---|---|---|---|
| P1 + P2 `ONE_TIME_SUBMIT` | 2026-11-26 | courtyard NRC; Bistro NRC (startup view) | `13.98 → 12.04`; `9.66 → 9.41` | NRC resolve `2.151 → 0.282` and `0.414 → 0.151 ms` (`C-M85-1`). Unlocked clocks, 1 round. GI `14/14`; stability identical; validation unchanged |
| M8.3 + M8.4 (toggles + `RawPt` + `StreamingRis` + SHaRC) | 2026-11-27 | Sponza gallery & courtyard; Bistro `cafe tables` | Sponza gallery `11.15` (`Nrc`) → **`8.97`** (`rtxpt`) / **`10.05`** (`Sharc q=1`); Bistro `cafe tables` `25.11` (`Nrc`) → **`17.87`** (`rtxpt`) / **`23.20`** (`Sharc q=1`) | Unlocked clocks, `600` frames ABBA ([`m8_4_sharc_cache.md` §9.5](./m8_4_sharc_cache.md#95-abba-release-timings--lazy-vram-rtx-5070-25601440-quality--1707960-render-layers-off-engine_bench_frames600)). `ReSTIR GI` off-cost `0.20 → 0.000 ms`; SHaRC `RayStartWait` `0.336 → 0.013 ms` on Bistro; `sharc.queryBounces=1` cuts Bistro `Indirect` `8.60 → 5.54 ms`; `indirect.slang` uber-shader growth flagged for P3 exp 1 |
| P0 (locked-clock baseline + Nsight GPU Trace) | *pending* | Full §2.3 matrix | — | Establish locked-clock baseline + VGPR / occupancy / throughput table before P2–P4 edits |
| P2 (barrier audit cleanup) | *pending* | Full §2.3 matrix | — | `vk_indirect.cpp`, `vk_sharc.cpp`, `resamplingBarrier`, `recordDebugView` |
| P3 (ray-pass specialization, coherence, glass, NEE, SHaRC tuning) | *pending* | Full §2.3 matrix | — | Track per-experiment ABBA deltas + Nsight VGPR/occupancy before → after |
| P4 (DLSS-RR presets, modes & guides) | *pending* | Full §2.3 matrix | — | Document presets D/E/F, Quality/Balanced/Performance, `specHitDist`, guide formats |
| P5 (Engine Target default sign-off) | *pending* | Engine Target | — | Direct (`StreamingRis` vs `Temporal Basic`) + Cache (`SHaRC` vs `NRC`) final pick |
| P6 (head-to-head vs NVIDIA demos) | *pending* | `RTXPT`, `RTXDI`, `RTXGI` | — | Matched-config FPS (mean + 1% low) via Nsight Systems |

---

## 8. Risks

- **SDK-bound costs:** NRC train (`~1.0 ms`) and DLSS-RR (`3.7–3.9 ms`) are closed-source SDK kernels. The engine controls presets, inputs, sizes, and overlap, not kernel code. If DLSS-RR Quality stays above `2.5 ms` on the Transformer preset, §2.3 allocations are explicitly re-balanced (engine spec §2.3 states allocations, not guarantees).
- **SHaRC `queryBounces=1` near-field / glossy bias:** Skipping the bounce-2 fallback ray saves `3.07 ms` on Bistro `Indirect`, but short/glossy segments fall back to 1-bounce `secondaryDirectLight`. Must pass 8-bounce `giBias` (`sponza.gallery`, `sponza.courtyard`, `cafe.tables`) and glossy-surface inspection; if diffuse/glossy corners darken, restrict bounce-2 continuation to low-roughness glossy hits only.
- **Async overlap vs `C-G3-6`:** Cross-queue overlap of NRC `QueryAndTrain` or DLSS-RR against graphics submits risks triggering driver 621.x device-lost (`C-G3-6`). Any change touching SDK submit isolation requires the 18,000-frame uncapped stress test. SHaRC already gets pre-acquire async compute overlap (`RayStartWait 0.013 ms`) without touching SDK isolation.
- **Sampling / NEE precision:** Decoupling `sharc_update` / $X_2$ NEE candidates, switching DI to `StreamingRis` or `Temporal Basic`, or floor-tier FP16 can move bias or introduce café confetti (`C-I12-1`). The GI check, guide capture, 8-bounce `giBias`, and café `Lit` stability decide — never timers alone.
- **Metric change:** `GPU total` lost RayStartWait and AcquireWait on 2026-11-27 (`C-M85-3`); a total compared across that date, or a frame-rate claim made from `GPU total` alone, is wrong by the idle time.
- **Measurement drift:** Both §1.1 (2026-11-26) and §1.4 (2026-11-27) were measured with unlocked clocks. P0 re-takes the full §2.3 matrix with locked clocks (`nvidia-smi -lgc`) before any P2–P5 optimization is accepted.

---

## 9. Sources

- NVIDIA, [Vulkan Dos and Don'ts](https://developer.nvidia.com/blog/vulkan-dos-donts) (updated 2025-01-14): submission, command buffers, barriers, memory, measurement.
- NVIDIA, [Best Practices for Using NVIDIA RTX Ray Tracing (Updated)](https://developer.nvidia.com/blog/best-practices-for-using-nvidia-rtx-ray-tracing-updated/): inline ray-query group sizes, live state, ray flags, coherence, AS builds.
- NVIDIA, [Advanced API Performance: Async Compute and Overlap](https://developer.nvidia.com/blog/advanced-api-performance-async-compute-and-overlap).
- NVIDIA, [Advanced API Performance: Barriers](https://developer.nvidia.com/blog/advanced-api-performance-barriers).
- NVIDIA, [Advanced API Performance: Shaders](https://developer.nvidia.com/blog/advanced-api-performance-shaders/).
- Khronos, Vulkan-ValidationLayers `layers/best_practices/*.cpp` at `vulkan-sdk-1.4.363.0`:
  - the 14 `BestPractices-NVIDIA-*` checks;
  - thresholds in `bp_constants.h`: Z-cull ratio 20, allocation reuse 5 s, 256 B fast descriptor space.
- `reference/RTXGI/Docs/NrcGuide.md`, `SharcGuide.md`; `reference/RTXDI/Doc/Integration.md`; `reference/RTXPT-main/README.md`, `Docs/Design.md`, `Docs/intro-sample.md`.
