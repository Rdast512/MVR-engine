# Feature: GPU opacity micromap bake

**Date:** 2026-11-27 · **Status:** planned, not started · **Milestone:** unscheduled; after M8 unless a load-time budget pulls it in
**Implements:** OMM baking on the GPU with the OMM SDK GPU baker (`ommGpu*`), amortized over frames on the compute queue, next to the CPU baker D1 ships. The CPU baker stays as the reference and the fallback.
**Touches:** C-D1-1 (bake on every load), C-D1-2 (chains at 19 % known area), C-D2-1 (micromap build cost), C-D1-3..C-D1-6 (this audit, [considerations.md](../../considerations.md)); M6 D1/D2 ([d1_omm_bake.md](../../m6/d1_omm_bake.md), [d2_micromap_clas.md](../../m6/d2_micromap_clas.md))
**Reference:** RTXPT `Rtxpt/OpacityMicroMap/OmmBaker.{h,cpp}`, `OmmBuildQueue.{h,cpp}`, `OmmBakeInputKey.h` (`reference/RTXPT-main`); OMM SDK 1.9.2 `docs/integration_guide.md` §GPU baker, `libraries/omm-lib/include/omm.h` (`ommGpu*`), `libraries/omm-lib/shaders/*.hlsl`; `omm-gpu-nvrhi` (RTXPT's integration layer, the SDK's own sample of the dispatch loop)

---

## 0. Why

D1 bakes on the CPU, on the asset workers, at every load:

- Sponza takes 741 ms on 14 threads (C-D1-1). One primitive (27,796 triangles) bounds it, so more threads do not help.
- The Debug build takes 37.6 s.
- The UI re-bake (2026-11-27, `EngineUi::drawOmmPanel`) reloads every model from disk, so each settings change costs a full load.

The SDK guide names the GPU baker as the choice when baking runs during interactive rendering: it is "significantly more efficient", and it can run on async compute or be amortized over frames. RTXPT bakes only on the GPU.

What the GPU bake buys:

- Load time no longer carries the bake.
- Settings can be re-baked without a reload: the alpha textures and UVs are already on the GPU.
- Higher subdivision levels become affordable (C-D1-2).
- Dynamic or streamed content can be baked later.

What it does not fix:

- The micromap build cost (C-D2-1) stays. The bake feeds the same `vkCmdBuildMicromapsEXT`.
- Raster/ray edge differences (C-D3-1) stay. They come from 2-state tracing, not from the baker (§7).

## 1. Reference: how RTXPT does it

Read before implementing:

- `OmmBuildQueue::Update` runs once per frame and does nothing while the previous OMM submit is still in flight (`ReadyToRecordWork` polls an event query; it never waits). Each frame it advances at most one task per state, latest state first:
  - `SharedBuild`
  - `BakeAndBuild`
  - `WaitingForSharedBake`
  - `Setup`
  - `None`
- Task states:
  - `None` → `RunSetup`: `PerformSetup` only. It copies the desc/index histograms and `OUT_POST_DISPATCH_INFO` to a readback buffer.
  - `Setup` → `RunBakeAndBuild`: it maps the readback and sizes `OUT_OMM_ARRAY_DATA` / `OUT_OMM_DESC_ARRAY` from the post-dispatch info. This is the guide's Strategy 2, the bake prepass, which RTXPT recommends. Then it runs `PerformBake` and `buildOpacityMicromap`.
  - `BakeAndBuild` → `Finalize`: it reads the stats and rebuilds the BLAS with the micromap.
  - Shared bakes: tasks with the same `OmmBakeInputKey` (texture, UVs, indices, settings) wait for one bake and reuse it.
- The baker is created lazily and released when the queue drains. This frees its scratch pool and binding cache.
- Defaults (`OpacityMicroMapUIData::BuildState`, `OmmBaker.h:115-130`):
  - `MaxSubdivisionLevel` 6, 4-state, `FastTrace`;
  - dynamic subdivision on, scale 2.0 (`DynamicSubdivisionScale = 2.f`);
  - `ComputeOnly` on;
  - `MaxOmmArrayDataSizeInMB` 100;
  - special indices on;
  - level-line intersection on.

## 2. Integration variant

The SDK makes no API calls. It returns dispatch chains and, optionally, precompiled SPIR-V that addresses resources by HLSL register and set/binding (`ommGpuSPIRVBindingOffsets`). Our pipelines must use descriptor heaps; there are no descriptor sets, not even as repros.

| Variant | How | Fits the heap rule by | Cost |
|---|---|---|---|
| **A. White-box (recommended)** | compile `libraries/omm-lib/shaders/*.cs.hlsl` with slangc at build time, as engine shader objects; the SDK still drives the dispatch list through `ommGpuDispatch` | Slang's own bindings: `-fvk-*-shift` into one set that `VkShaderDescriptorSetAndBindingMappingInfoEXT` maps to the heap — `HEAP_WITH_PUSH_INDEX` / `HEAP_WITH_CONSTANT_OFFSET` for textures, `SamplerState s_samplers[]` (8 contiguous static sampler slots in the sampler heap, `bake_gpu_impl.cpp:303-311`), and typed `Buffer<uint> t_indexBuffer` (`VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER`, or rewrite `FetchTexCoords` to `ByteAddressBuffer` / BDA); `PUSH_ADDRESS` / `INDIRECT_ADDRESS` for uniform (`GlobalConstants`, `b0`) and storage (`ByteAddressBuffer` / `RWByteAddressBuffer`) buffers. `LocalConstants` (`b1`) is `[[vk::push_constant]]` (16–32 B at push offset 0), so mapped push offsets start after it | HLSL-in-Slang compile risk (the RTXDI bridges show the pattern); re-check after every SDK update |
| B. Black-box | build omm-lib with `OMM_ENABLE_PRECOMPILED_SHADERS_SPIRV ON` and create shader objects from its blobs | the same mapping struct on the SDK's SPIR-V, built from `ommGpuPipelineInfoDesc::descriptorSetDesc` and the binding offsets (with a resource-heap texel buffer descriptor for `Buffer<uint> t_indexBuffer` and 8 contiguous sampler-heap slots for `s_samplers[]`) | no compile step; SPIR-V we cannot patch; set/binding layout read at run time |

Both use `VkShaderDescriptorSetAndBindingMappingInfoEXT` (`VK_EXT_descriptor_heap`, Vulkan SDK 1.4.363). This is the one place outside the engine's own shaders that needs it.

**Decision G0:** start with A. Prove `omm_rasterize_cs_a.cs.hlsl` (not `omm_clear_buffer.cs.hlsl`, which binds only one RW buffer) compiles and runs validation clean (core, sync and GPU-AV) under the mapping — it exercises `Texture2D`, typed `Buffer<uint>`, `ByteAddressBuffer`, `RWByteAddressBuffer`, `s_samplers[]`, `GlobalConstants`, and `LocalConstants` together. If Slang cannot compile the SDK's HLSL, fall back to B.

**ComputeOnly only.** `ommGpuBakeFlags_ComputeOnly` (beta in 1.9.2, "will become default") drops the raster path:

- no geometry shader;
- no conservative raster;
- no null RTV;
- no indirect draws;
- supports subdivision up to level 12 (`bake_gpu_impl.cpp:255`, vs 10 on graphics).

It is the mode RTXPT ships and the one async compute can run. `STATIC_VERTEX_BUFFER` and `STATIC_INDEX_BUFFER` (`ommGpuGetStaticResourceData`) are bound only on the graphics path (`bake_gpu_impl.cpp:1149-1150`) and are never needed in `ComputeOnly`.

## 3. Inputs

| SDK resource | Source | Note |
|---|---|---|
| `IN_ALPHA_TEXTURE` | the material's runtime alpha texture, read through the heap at the mip the shader samples (`mipOffset`/`mipNum` in `ommGpuResource`) | BC7 sampled on the GPU replaces D1's CPU `decodeBc7Alpha`; same texels by construction |
| `IN_TEXCOORD_BUFFER` | per-primitive transformed `float2` UVs (`uv0` or `uv1` + `uvMatrix`) | GPU `AssetBuffers` keep `uv0`/`uv1` untransformed; a compute pre-pass materializes transformed UVs into transient scratch |
| `IN_INDEX_BUFFER` | per-primitive 32-bit triangle indices in final cluster order (`Buffer<uint>` / texel buffer, or `ByteAddressBuffer` in Variant A) | GPU `AssetBuffers` store 8-bit cluster-local `MeshletTriangles` + 32-bit `MeshletVertices`, not a flat 32-bit index buffer; the same compute pre-pass builds cluster-ordered 32-bit indices matching D2's micromap index layout per cluster (spec/m6/d2) |
| `runtimeSamplerDesc` | the glTF sampler (`wrapS`, `magFilter`) | `ommSamplerDesc` (`omm.h:198`) still takes a single `addressingMode` on GPU (selecting one of 8 static samplers, `bake_gpu_impl.cpp:303-311`); does **not** fix `wrapS != wrapT` |
| `alphaCutoff` | same `nextafter` cutoff as D1 | keep the CPU/GPU comparison exact (§6) |

## 4. Flow

One GPU bake task per model load or re-bake, executing one `ommGpuDispatch` chain per alpha-tested primitive (each primitive has its own `IN_ALPHA_TEXTURE`, `alphaCutoff`, `runtimeSamplerDesc`, and `VkMicromapEXT`), as RTXPT iterates geometries per mesh:

1. **Queue.** `AssetsLoader` skips the CPU bake when `OmmBakeSettings::baker == Gpu` and queues a task once the model's GPU buffers are flushed. Until the task finishes, the model's alpha primitives trace opaque, as with OMM disabled.
2. **Setup** (frame N, compute queue). Run the pre-pass to materialize cluster-ordered 32-bit indices and transformed UVs. `ommGpuGetPreDispatchInfo` sizes the transient pool within `maxScratchMemorySize` (default 256 MB, from device memory at init). Run `PerformSetup` only (do not expect stats here: `bake_gpu_impl.cpp:733` sets `EnablePostDispatchInfoStats = flag && doBake`). Copy `OUT_OMM_DESC_ARRAY_HISTOGRAM` and `OUT_POST_DISPATCH_INFO` to a host-visible readback (ReBAR VRAM, read after the slot's timeline value).
3. **Bake and build** (frame N + k, once the timeline value is reached; never waited on):
   - allocate `OUT_OMM_ARRAY_DATA` and `OUT_OMM_DESC_ARRAY` at `PostDispatchInfo` sizes, **256 B aligned** (`kMicromapAlignment`, `VUID-vkCmdBuildMicromapsEXT-pInfos-07515`), with `eStorageBuffer | eMicromapBuildInputReadOnlyEXT | eShaderDeviceAddress` and `maxOutOmmArraySize` capped by the array budget (§5); `OUT_OMM_INDEX_BUFFER` uses `eStorageBuffer | eAccelerationStructureBuildInputReadOnlyKHR | eShaderDeviceAddress`;
   - run `PerformBake | EnablePostDispatchInfoStats`;
   - convert read-back 8-byte histogram rows (`omm_work_setup_cs.cs.hlsl:90`, `{count, level, format}`) to 12-byte `VkMicromapUsageEXT` rows on CPU (filtering zero-count buckets and falling back to `kUnreferencedMicromap*` when `outOmmDescSizeInBytes == 0`), then run `vkCmdBuildMicromapsEXT` from the GPU buffers.
4. **Swap** (frame N + k + 1):
   - **Blocker to address in G3:** `GeometryStore::clearScratch()` drops CPU `vertices` / `meshlets` / `meshletVertices` / `meshletTriangles` right after `AssetBuffers::flush()`, so `MegaGeometry::buildModel` cannot be called as-is. Async CLAS/BLAS rebuild must source vertex positions and cluster indices from the resident GPU `AssetBuffers` (retaining minimal per-primitive cluster ranges) with `opacityMicromapArray` set;
   - update `RayGeometry` BLAS addresses and `micromapFlags`;
   - bump the layout generation, so `RayTracingScene::buildTemplates` re-reads them and marks a scene change (caches reset);
   - free the old CLAS, BLAS and micromaps through deferred destruction after the frames in flight retire.
5. **Finish.** Read back the counts from `OUT_POST_DISPATCH_INFO` (`outStatsTotalOpaqueCount` / `Transparent` / `Unknown` + `Fully*` special-index counts; note the GPU baker does **not** produce UV-area-weighted `knownAreaMetric`, so `minKnownAreaMetric` stays `-1.0f` / `n/a` unless computed separately) into `OmmBakeSummary`. Free the scratch when the queue is empty (RTXPT's lazy baker release).

Budget: at most one state step per task per frame, as RTXPT does. Add a per-frame cap on micro-triangles baked, so one large model does not stall a frame. Each step gets Tracy CPU and GPU zones (`OMM/Setup`, `OMM/Bake`, `OMM/MicromapBuild`, `OMM/ClasRebuild`).

Queue: the compute queue, with a timeline semaphore before the first ray pass that reads the new BLAS. Swapping on the graphics timeline at a frame boundary avoids a cross-queue wait inside the frame.

## 5. Settings and UI

Extend `OmmBakeSettings` (`src/core/omm_baker.hpp`):

- `baker`: Cpu / Gpu;
- `maxArrayDataSizeMiB`: RTXPT 100. It feeds `maxOutOmmArraySize` on the GPU and `maxArrayDataSize` on the CPU, which D1 leaves unlimited (C-D1-5);
- `scratchBudgetMiB`: `ommGpuScratchMemoryBudget`;
- `isLevelLineIntersectionEnabled`: debug, on by default;
- `isTexCoordDeduplicationEnabled`: on by default;
- `isForce32BitIndices`: debug.

The Opacity micromaps panel gains:

- a Baker combo;
- the budgets;
- progress (pending tasks, current state, as RTXPT's build-progress bar);
- GPU-only UI locks: when `baker == Gpu`, lock `Promotion` to `Force opaque` (the GPU compute path uses boolean opaque/transparent flags, `omm_rasterize_cs.hlsli:379-396`, so `ommGpuDispatchConfigDesc` has no `unknownStatePromotion` and always promotes mixed micro-triangles to `UnknownOpaque` / `Opaque`) and disable `Near-duplicate detection` (CPU-only);
- re-bake without a reload when `baker == Gpu`: the textures and UVs are resident, so re-baking is a re-queue, not `Engine::rebakeOmms`.

Defaults stay as D1 ships until §6 passes. After that, the GPU defaults are RTXPT's (`MaxSubdivisionLevel` 6, scale `2.0`, `ComputeOnly`), as the NVIDIA-defaults rule asks.

## 6. Checks

- **CPU vs GPU equivalence.** Same model, same settings (with CPU `unknownStatePromotion = ForceOpaque` to match GPU behavior), both bakers. Compare per primitive:
  - opaque / transparent / unknown micro-triangle counts and known-micro-triangle ratio `(opaque + transparent) / (opaque + transparent + unknown)` (the GPU baker does not emit UV-area-weighted `knownAreaMetric`);
  - array size.

  States are not expected to match bit for bit: the GPU's level-line rasterization differs from the CPU resampler. Limit: known micro-triangle ratio within 0.01, and counts within 1 %. Run on Sponza and `AlphaBlendModeTest`.
- **`ENGINE_RT_CHECK`** with GPU-baked micromaps: 32 of 32 with the `alpha-edge` class, as D4.
- **`ENGINE_GUIDE_CAPTURE`**: `Sponza_plants` and `AlphaBlendModeTest_sun` shots match the CPU bake.
- **Validation:** core + sync + best practices + GPU-AV on the bake passes (descriptor heap mapping, BDA).
- **Timing** (release, no layers):
  - bake GPU time per task;
  - frames from load to swap;
  - frame-time spikes during the bake (per-step budget);
  - load time with and without the CPU bake.

  The exit for C-D1-1 is a load time without the 741 ms.

## 7. Out of scope, related

- **Exact alpha edges on rays.** Rays force 2-state (`RAY_FLAG_FORCE_OMM_2_STATE`), so 4-state unknowns resolve by promotion. To reach the guide's Type 1 quality, 4-state needs an alpha test on unknown candidates. In a ray query, that means dropping the 2-state flag and alpha-testing candidate non-opaque triangles in the `Proceed()` loop. It is a ray-contract change (spec/m6/d4) and its own step. It is the real fix for C-D3-1 and the only reason to keep 4-state (C-D1-3).
- **Processing cache.** `ommCpuSerialize` (engine spec §7.2) covers offline bakes. The GPU bake makes it optional rather than required.
- **OMM debug view.** RTXPT's `DebugView` (InWorld / Overlay of micro-triangle states) has no counterpart here (C-D1-6). It is useful for §6, but it is its own debug-view entry.

## 8. Steps

| Step | Content | Gate |
|---|---|---|
| G0 | `omm_rasterize_cs_a.cs.hlsl` through slangc under the heap mapping (variant A), or blobs (variant B) — proving `Texture2D`, typed `Buffer<uint>`, `ByteAddressBuffer`, `RWByteAddressBuffer`, `s_samplers[]`, `GlobalConstants`, and `LocalConstants` | runs, validation + GPU-AV clean |
| G1 | `OmmGpuBaker` module: pre-pass for cluster-ordered 32-bit indices + transformed UVs, compute-only pipelines from `ommGpuCreatePipeline`, dispatch-chain executor (no static VB/IB needed) | one primitive baked, 8 B → 12 B histograms & stats read back |
| G2 | per-model task queue iterating alpha-tested primitives (§4 steps 1–3) on the compute queue, readback without waits | Sponza baked over frames, no frame waits in Tracy |
| G3 | GPU-input CLAS/BLAS rebuild (independent of cleared CPU scratch) and swap (§4 step 4) with deferred destruction | `ENGINE_RT_CHECK` 32/32 |
| G4 | settings, UI (locking Promotion / CPU-only flags on GPU), budgets (§5) | re-bake without a reload |
| G5 | CPU vs GPU check (§6, CPU pinned to `ForceOpaque`), timings, decide defaults | §6 limits; C-D1-1 exit |
