# M8.4 · SHaRC Multi-Bounce Radiance Cache Toggle

**Date:** 2026-11-27 · **Status:** complete (verified on RTX 5070); §8.4 perf targets not met on Bistro at the default `queryBounces=2`. Open after the 2026-11-27 audit: [`C-M84-2`–`C-M84-7`](../considerations.md) (perf targets, `separateEmissive=off` double count, no occupancy counter, absolute firefly clamp, `resolve=off` corruption, per-frame cross-frame barrier) and [`C-M85-4`](../considerations.md) (`pipeline=rtxpt` SHaRC settings vs IntroSample)  
**Plan:** [M8 README](./README.md) step M8.4 · **Depends on:** [M8.3](./m8_3_pipeline_toggles.md) (complete)  
**Feeds into:** [M8.5](./m8_5_perf_and_nvidia_benchmark.md) (NRC vs SHaRC & NVIDIA RTXPT head-to-head benchmarks), M8.6 (full toggle verification & RTX 2060 Super floor tier)  
**References:**
- Vendored SDK: `ThirdParty/_deps/src/sharc` (`v1.8.3`, commit `4e21b585c33c83d723ca9a1e11bbb1090d145793` — `include/SharcCommon.h`, `include/HashGridCommon.h`, `include/SharcTypes.h`, `include/HashGridTypes.h`, `docs/Integration.md`)
- `reference/RTXPT-main/Rtxpt/Sharc/` (`SharcIntegration.h`, `SharcIntegration.cpp`)
- `reference/RTXPT-main/Rtxpt/IntroSample/` (`SHaRCRenderer.h`, `SHaRCRenderer.cpp`)
- `reference/RTXPT-main/Rtxpt/Shaders/IntroSample/` (`SHaRCUpdate.hlsl`, `SHaRCResolve.hlsl`, `SHaRCDebugCache.hlsl`)
- `reference/RTXPT-main/Rtxpt/Shaders/PathTracer/PathTracerSHaRC.hlsli`
- `reference/RTXGI/Samples/IndirectDiffuse/Shaders/` (`SharcIndirectDiffuse.hlsli`, `SharcResolve.hlsl`)
- [Engine spec §11.4, §11.7](../../docs/engine_spec.md) · [M8.3 spec](./m8_3_pipeline_toggles.md) · [Considerations](../considerations.md) (`C-G3-1`, `C-M83-1`, `C-M83-2`, `C-M83-3`)

---

## 1. Objective & Design Principles

Re-integrate NVIDIA **SHaRC v1.8.3** (Spatial Hash Radiance Cache) as a **pure Vulkan 1.4 BDA + Slang compute** runtime toggle (`RadianceCacheMode::Sharc`) beside NRC and `Off`, completing the M8.3 placeholder (`C-M83-2`) and moving `PipelinePreset::RtxptAnalog` (`pipeline=rtxpt`) onto real SHaRC (`C-M83-1`):

1. **Zero SDK / CUDA isolation overhead:** Unlike NRC (`C-G3-6`), SHaRC is 100% pure Vulkan compute + `rayQueryEXT` + BDA (`shaderBufferInt64Atomics`). Every SHaRC pass (`sharc_update`, `sharc_resolve`, inline query in `indirect.slang`) records directly inside the single pre-acquire ray command buffer (`preSdkRaySegmentCount`) with no `recordIsolated()` splits, no driver descriptor-heap swaps, and no external DLLs.
2. **Zero per-thread path-state buffer (compute `rayQueryEXT` win over RTXPT):** NVIDIA's `RTXPT` uses a ray-tracing pipeline where `SharcState` must cross `TraceRay` → closest-hit boundaries via a 33–133 MiB `SharcPathStateBuffer` (`SharcIntegration.cpp:171-231`). Because our engine traces inline compute `rayQueryEXT` (`traceClosest`), `SharcState` stays entirely in registers inside `passes/sharc_update.slang` — saving 100% of that VRAM and bandwidth.
3. **Inline cache hit at $X_2$ skips secondary NEE + shadow rays:** SHaRC stores **total outgoing radiance** (direct + multi-bounce indirect) per world-space voxel (`SharcCommon.h:606-685, 732-768`), unlike NRC (`includeDirectLighting = false`). In `indirect.slang`, eligible bounce-1 hits that hit SHaRC at $X_2$ read cached radiance directly and **skip `secondaryDirectLight` and its shadow ray** — and require no post-indirect resolve pass and no `rawIndirectWeight` scratch target (`0 B` extra in `RawPt + Sharc`). Ineligible hits (short segments $\le \sqrt{3}\cdot\text{voxelSize}$, sharp specular footprints $\le \text{voxelSize}$) or cache misses fall back cleanly to exact `secondaryDirectLight` + optional continuation bounce(s) up to `maxQueryBounces` (default `2`), preserving sharp contact lighting and multi-bounce glossy reflections.
4. **Lazy BDA allocation & frame-safe retirement (zero `waitIdle`):** The three SHaRC BDA buffers (`SharcHashEntries`, `SharcAccumulation`, `SharcResolved`: `40 B/voxel`, `160 MiB` at default $2^{22}$ entries; configurable `1M–8M` / `40–320 MiB`) are allocated only when SHaRC is active and retired once `frameNumber - lastUsedFrame >= MAX_FRAMES_IN_FLIGHT`, matching M8.3 `PERF-D2`. Capacity resizes also defer old-buffer destruction by `MAX_FRAMES_IN_FLIGHT` frames without `waitIdle`.
5. **Works with both `RawPt` and `RestirGi`:** `RawPt + Sharc` writes `indirectRadiance` directly in `indirect.slang`; `RestirGi + Sharc` stores the SHaRC-accelerated bounce-1 radiance into the initial GI reservoir for `rtxdi_gi_temporal` + `rtxdi_gi_shade`.
6. **Zero cost when off:** When `RadianceCacheMode != Sharc` or `IndirectBounceMode == Off`, `SharcCache` holds `0 B` of device memory, `GpuFrameRoot::sharcConstants == 0`, `SharcUpdate` and `SharcResolve` emit `0` dispatches and `0` barriers (`0.000 ms` timer scopes), and `indirect.slang` takes a uniform skip branch.

---

## 2. SDK, Device Features & Slang Compilation

### 2.1 Vendored SHaRC SDK (`ThirdParty/CMakeLists.txt`)

`ThirdParty/_deps/src/sharc` is already present in the tree at **v1.8.3** (`4e21b585c33c83d723ca9a1e11bbb1090d145793`, `https://github.com/NVIDIA-RTX/SHARC.git`). Restore its CMake FetchContent declaration (removed in `C-G3-1`) and add `${sharc_SOURCE_DIR}/include` to `_slangIncludeDirs` for all platforms (header-only HLSL/Slang; no platform DLLs):

```cmake
engine_fetch_declare(sharc
        GIT_REPOSITORY https://github.com/NVIDIA-RTX/SHARC.git
        GIT_TAG 4e21b585c33c83d723ca9a1e11bbb1090d145793 # v1.8.3
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(sharc)
```

Append `"${sharc_SOURCE_DIR}/include"` to `_slangIncludeDirs` alongside `rtxdi` and `nrc`. Vendor headers in `ThirdParty/_deps/src/sharc/include/` are **never edited**.

### 2.2 Vulkan 1.4 Device Features (`src/core/vk_device.cpp`)

Add two core Vulkan features to `kRequiredFeatures` (both supported on RTX 2060 Super floor tier, RTX 5070 reference tier, and AMD RDNA 2+):

| Feature Struct | Field | Why Required |
|---|---|---|
| `PhysicalDeviceVulkan12Features` | `shaderBufferInt64Atomics` | Native 64-bit `OpAtomicCompareExchange` on `uint64_t*` (`PhysicalStorageBuffer`) in `HashGridCommon.h:242` (`HASH_GRID_ENABLE_64_BIT_ATOMICS=1`, zero lock buffer) |
| `PhysicalDeviceVulkan11Features` | `storageBuffer16BitAccess` | Guarantees `float16_t4` loads/stores in `SharcPackedData` (`SharcTypes.h:30`) through `PhysicalStorageBuffer` |

*(Already enabled in `vk_device.cpp`: `shaderInt64`, `shaderInt16`, `shaderFloat16`, `bufferDeviceAddress`, `scalarBlockLayout`, `vulkanMemoryModel`, `vulkanMemoryModelDeviceScope`.)*

### 2.3 Slang 2026.22.1 Capabilities & Compile Units (`CMakeLists.txt`)

Verified directly with `ThirdParty/Slang-win/bin/slangc.exe` (2026.22.1) and `spirv-val --target-env vulkan1.4` against `ThirdParty/_deps/src/sharc/include/SharcCommon.h` (v1.8.3):

1. Add **`spvInt64Atomics`** to `ENGINE_SLANG_CAPABILITIES`.
2. Add **`set(SHARC_SLANG_FLAGS -warnings-disable 41018)`** alongside `RTXDI_SLANG_FLAGS`: `SharcGetCachedRadianceFromHash` (`SharcCommon.h:738, 767`) returns `false` without assigning `out float3 radiance`, which triggers Slang `warning[E41018]`. Keeping vendor headers untouched requires disabling `41018` on SHaRC compile units (documented in `spec/considerations.md`, mirroring `C-F2-2`).
3. Compile units:
   - **New:** `engine_add_slang_pass(passes/sharc_update.slang ENTRIES sharcUpdateMain FLAGS ${RTXDI_SLANG_FLAGS} ${SHARC_SLANG_FLAGS} -DSHARC_UPDATE=1)`
   - **New:** `engine_add_slang_pass(passes/sharc_resolve.slang ENTRIES sharcResolveMain FLAGS ${SHARC_SLANG_FLAGS})`
   - **Updated:** `passes/indirect.slang` adds `${SHARC_SLANG_FLAGS} -DSHARC_QUERY=1` (coexists cleanly with `-DNRC_QUERY=1`; the two bridges have disjoint symbols and are mutually exclusive at runtime).
   - **Updated:** `passes/debug_view.slang` adds `${SHARC_SLANG_FLAGS} -DSHARC_QUERY=1`.

---

## 3. GPU Data Contracts & BDA Shader Bridge

### 3.1 `GpuFrameRoot` Extension (`src/core/types.hpp`, `shaders/types.slang`)

`GpuFrameRoot` is currently `248 B` (`preDenoiserClamp` @ 240, `inputHistoryWeight` @ 244). Append one 8-byte BDA field at offset `248`:

```cpp
// GpuSharcConstants of this frame slot (M8.4); 0 when SHaRC is inactive
vk::DeviceAddress sharcConstants = 0;
```

- `sizeof(GpuFrameRoot)` becomes **`256 B`** (exact 16-byte multiple; zero padding).
- `offsetof(GpuFrameRoot, sharcConstants) == 248`.
- Propagated via `FrameDataInputs::sharcConstants` (`src/core/vk_frame_data.{hpp,cpp}`) and asserted in `tests/layout_test.cpp`.
- No new `FrameFlag` bit is needed: `root.sharcConstants != 0` is the single uniform GPU truth for SHaRC activity.

### 3.2 `GpuSharcConstants` (`src/render/vk_sharc.hpp`, `shaders/gi/sharc_constants.slang`)

Exact 96-byte scalar-layout struct double-buffered in `HostBuffer[MAX_FRAMES_IN_FLIGHT]` (zero padding, 16-byte aligned):

| Offset | Type | Field | Description |
|---:|---|---|---|
| `0` | `uint64_t` | `hashEntries` | BDA of `uint64_t[capacity]` (`SharcHashEntries`, 8 B/entry) |
| `8` | `uint64_t` | `accumulation` | BDA of `SharcAccumulationData[capacity]` (`SharcAccumulation`, 16 B/entry) |
| `16` | `uint64_t` | `resolved` | BDA of `SharcPackedData[capacity]` (`SharcResolved`, 16 B/entry) |
| `24` | `float3` | `cameraPos` | Hash-grid anchor (current camera, frozen when `!updateViewCamera`) |
| `36` | `float` | `sceneScale` | World-space voxel density (default `35.0`, range `1.0..100.0`) |
| `40` | `float3` | `prevCameraPos` | Previous grid anchor for `SHARC_BLEND_ADJACENT_LEVELS` |
| `52` | `float` | `radianceScale` | Fixed-point atomic scale (default `1000.0`, `SharcCommon.h:187`) |
| `56` | `uint32_t` | `capacity` | Number of hash-grid entries ($2^{20}..2^{23}$; default $2^{22} = 4{,}194{,}304$) |
| `60` | `uint32_t` | `downscaleFactor` | Sparse update grid step $N$ (`1/(N×N)` pixels; default `4`, range `2..5`) |
| `64` | `uint32_t` | `accumulationFrameNum` | Max temporal accumulation frames (default `32`, range `1..256`) |
| `68` | `uint32_t` | `staleFrameNumMax` | Stale eviction frames (default `64`, range `8..256`) |
| `72` | `uint32_t` | `maxUpdateBounces` | Max bounces per update path (default `4`, range `2..8`) |
| `76` | `uint32_t` | `maxQueryBounces` | Max indirect bounces on cache miss/ineligible hit (default `2`, range `1..4`) |
| `80` | `float` | `minRoughnessOverride` | Update-only roughness floor (default `0.4`, range `0.0..1.0`) |
| `84` | `float` | `fireflyClamp` | Update-pass per-vertex direct-light luminance clamp (`10.0`, `0` = off) |
| `88` | `uint32_t` | `flags` | `SharcFlag` bits + debug mode (`0..5` in bits 4..6) |
| `92` | `uint32_t` | `updateLaunch` | `width | (height << 16)` of sparse `sharc_update` dispatch |
| `96` | `uint64_t` | `counters` | BDA of per-slot `GpuSharcCounters` (`occupied`, `insertFailures`, device-local `CounterReadback`) |

`static_assert(sizeof(GpuSharcConstants) == 104)` and `static_assert(sizeof(GpuSharcCounters) == 8)` in C++, checked in `tests/layout_test.cpp`.

**`SharcFlag` bits:**
- `MaterialDemodulation = 1u << 0` (runtime toggle; passes `1.0` when clear)
- Emissive is always kept separate (`SHARC_SEPARATE_EMISSIVE = 1`, no runtime toggle so vertex 0 never double-counts sampled light triangles)
- `DebugModeShift = 4u`, `DebugModeMask = 0x7u << 4u` (`SharcDebugMode`: `0 = CachedRadiance`, `1 = ColoredHash`, `2 = Occupancy`, `3 = HashCollisions`, `4 = VoxelSize`, `5 = GridLevel`).

### 3.3 SHaRC Bridge (`shaders/gi/sharc_bridge.slang`)

Textual include (mirrors `shaders/gi/nrc_bridge.slang`), included once per compile unit:

```hlsl
#ifndef SHARC_UPDATE
#define SHARC_UPDATE 0
#endif
#ifndef SHARC_QUERY
#define SHARC_QUERY 0
#endif
#if SHARC_UPDATE && SHARC_QUERY
#error SHARC_UPDATE and SHARC_QUERY are separate compile units
#endif

#define SHARC_ENABLE_GLSL 0
#define SHARC_MATERIAL_DEMODULATION 1
#define SHARC_SEPARATE_EMISSIVE 1
#define SHARC_ENABLE_RESPONSIVE_LIGHTING 0
#define SHARC_ENABLE_SH_ENCODING 0
#define SHARC_ENABLE_FADE_ACCELERATION 1
#define HASH_GRID_ENABLE_64_BIT_ATOMICS 1
#define RW_STRUCTURED_BUFFER(name, type) type* name
#define BUFFER_AT_OFFSET(name, offset) name[offset]

#include "SharcCommon.h"
```

**Why each compile-time define is chosen:**
- `SHARC_MATERIAL_DEMODULATION 1` & `SHARC_SEPARATE_EMISSIVE 1`: compiled in and toggled at runtime via `GpuSharcConstants::flags` (passing `float3(1)` / `float3(0)` when disabled, matching `PathTracerSHaRC.hlsli:228,240`), eliminating shader permutations.
- `SHARC_ENABLE_RESPONSIVE_LIGHTING 0`: lets `SharcResolveEntry` clear `accumulationBuffer` in place (`SharcCommon.h:976-980`), avoiding an extra per-frame clear pass.
- `SHARC_ENABLE_SH_ENCODING 0`: keeps `40 B/voxel` (`160 MiB` at $2^{22}$) instead of `64 B/voxel` (`256 MiB`); directional SH2 encoding can be evaluated as an optional follow-up in M8.5 if glossy leaking warrants +96 MiB.
- `SHARC_ENABLE_FADE_ACCELERATION 1`: free in v1.8.3 (reuses existing `SharcPackedData::sampleDataExt` bitmask, `SharcCommon.h:928-942`), accelerating convergence when lights dim or turn off.
- `HASH_GRID_ENABLE_64_BIT_ATOMICS 1`: native `OpAtomicCompareExchange` on `uint64_t*` (`PhysicalStorageBuffer`), zero lock buffer.
- `SHARC_ENABLE_CACHE_RESAMPLING`: defaults to `SHARC_UPDATE` (`1` in update pass) with `SHARC_PROPAGATION_DEPTH = 2` (`SharcCommon.h:91-96`), propagating multi-bounce energy across frames with short 2–4 bounce update paths.

**Shared bridge helpers:**
- `sharcParametersFrom(GpuSharcConstants c)` → builds `SharcParameters` with `(uint64_t*)c.hashEntries`, `(SharcAccumulationData*)c.accumulation`, `(SharcPackedData*)c.resolved`, `logarithmBase = SHARC_GRID_LOGARITHM_BASE (2.0)`, `levelBias = SHARC_GRID_LEVEL_BIAS (0.0)`.
- `sharcMaterialDemodulation(float3 diffuseAlbedo, float3 specularF0)` → exact RTXPT (`PathTracerSHaRC.hlsli:232-233`) & RTXGI (`SharcIndirectDiffuse.hlsli:50-51`) formula:
  ```hlsl
  const float3 specularFAvg = specularF0 + (1.0 - specularF0) * (1.0 / 21.0);
  return max(diffuseAlbedo, 0.05) + max(specularF0, 0.02) * luminance(specularFAvg);
  ```
- `sharcMakeHitData(GpuSharcConstants c, float3 positionWorld, float3 geoNormal, float3 diffuseAlbedo, float3 specularF0, float3 emissive)` → sets `positionWorld`, `normalWorld = geoNormal` (piecewise-constant per triangle for hash stability per `SharcCommon.h:211` and `PathTracerSHaRC.hlsli:223`), `materialDemodulation`, and `emissive`.
- `sharcCanQuery(SharcParameters params, float3 hitPositionWorld, float segmentLength, bool isSpecularLobe, float launchRoughness)` → exact RTXGI (`SharcIndirectDiffuse.hlsli:65-90`) & `Integration.md:145` eligibility test:
  1. `voxelSize = HashGridGetVoxelSize(HashGridGetLevel(hitPositionWorld, params.hashGridParameters), params.hashGridParameters)`.
  2. Require `segmentLength > voxelSize * sqrt(3.0)`.
  3. If `!isSpecularLobe`: return `true`.
  4. If `isSpecularLobe`: `alpha = min(launchRoughness, 0.99)^2`, `alpha2 = alpha * alpha`, `footprint = 2.0 * segmentLength * sqrt(0.5 * alpha2 / max(1.0 - alpha2, 1e-6))`, return `footprint > voxelSize`.

---

## 4. Shader Passes & Radiance Correctness

### 4.1 Sparse Update Pass (`shaders/passes/sharc_update.slang`, `SHARC_UPDATE=1`)

- **Dispatch:** `[numthreads(8, 8, 1)]` over `launchSize = ceil(renderSize / downscaleFactor)`.
  - At 1707×960 (2560×1440 Quality) with `downscaleFactor = 4`: `427 × 240 = 102,480` threads (`6.25%` of pixels, `54 × 30` workgroups).
- **Deterministic temporal block-jittered pixel selection:**
  - Seed per-block RNG from `(launch, frameIndex, kSharcUpdatePathSeed = 44)` (separate from RTXDI `0..31`, NRC `41..42`, reference `43`; light RNG uses `kSharcUpdateLightSeed = 45`).
  - Pick `(offsetX, offsetY)` uniformly in `[0, downscaleFactor)` and clamp `pixel = min(launch * downscaleFactor + uint2(offsetX, offsetY), renderSize - 1)`.
- **Vertex 0 (primary G-buffer surface):**
  - Load `primary = RAB_GetGBufferSurface(int2(pixel), false)`. If `!RAB_IsSurfaceValid(primary) || !primary.isLit`: return.
  - Apply update-only roughness floor: `primary.roughness = max(primary.roughness, sharc.minRoughnessOverride)` (never affects query eligibility or main shading).
  - Direct lighting at vertex 0: `directLighting = directLightAt(root, primary, RasterOrigin(primary), rtxdiConstants, lightRng, coherentRng)` (plus `kGuideEmissive` only if `(sharc.flags & kSharcFlagSeparateEmissive) == 0`).
  - Optional firefly clamp: if `sharc.fireflyClamp > 0` and `luminance(directLighting) > sharc.fireflyClamp`, scale `directLighting` down so a single extreme RIS draw cannot poison a voxel for 32+ frames (`SharcIntegration.cpp:106`).
  - Build `hitData` (`emissive = 0` at vertex 0: vertex 0 has no predecessor in `sharcState` to back-propagate to, and `SHARC_SEPARATE_EMISSIVE=1` does not deposit `hitData.emissive` into vertex 0's own voxel).
  - `SharcInit(sharcState);`
  - `if (!SharcUpdateHit(sharcParams, sharcState, hitData, directLighting, RTXDI_GetNextRandom(pathRng))) return;`
  - Scatter bounce 1 using `scatterBrdf(primary, RasterOrigin(primary), pathRng, segmentThroughput, ray, pdf)` starting from `segmentThroughput = float3(1.0)` (no Russian Roulette per `PathTracerSHaRC.hlsli:23-25`). If absorbed, return.
  - `SharcSetThroughput(sharcState, segmentThroughput);`
- **Bounces `1 .. maxUpdateBounces - 1`:**
  - `traceClosest(root, ray, hit)`. On miss: `SharcUpdateMiss(sharcParams, sharcState, float3(0.0)); break;` (ready for M7 environment light later).
  - `PathHit vertex = shadeRayHit(root, hit, ray, pathLength); pathLength += hit.t;` (`vertex.emissive` already obeys the engine's one-emission rule `hitEmission()`!).
  - **Unlit surface guard:** if `!vertex.surface.isLit`: propagate `vertex.emissive` if non-zero via `SharcUpdateMiss(sharcParams, sharcState, vertex.emissive)` and `break;` without inserting a cache entry for an unlit surface.
  - Apply update-only roughness floor: `vertex.surface.roughness = max(vertex.surface.roughness, sharc.minRoughnessOverride)`.
  - `directLighting = directLightAt(root, vertex.surface, HitOrigin(hit, geoNormal), rtxdiConstants, lightRng, coherentRng)` (plus `vertex.emissive` only when `(sharc.flags & kSharcFlagSeparateEmissive) == 0`).
  - Apply optional `sharc.fireflyClamp` to `directLighting`.
  - Build `hitData` (`emissive = separateEmissive ? vertex.emissive : 0`).
  - `if (!SharcUpdateHit(sharcParams, sharcState, hitData, directLighting, RTXDI_GetNextRandom(pathRng))) break;` (cache resampling at bounce 1–2 terminates early once voxels warm up, or exits safely on hash overflow per v1.8.3).
  - If `bounce + 1 < sharc.maxUpdateBounces`: reset `segmentThroughput = float3(1.0)`, call `scatterBrdf(...)`; if absorbed `break;`, else `SharcSetThroughput(sharcState, segmentThroughput);`.

### 4.2 Resolve Pass (`shaders/passes/sharc_resolve.slang`)

- **Dispatch:** `[numthreads(256, 1, 1)]`, `dispatch(capacity / 256, 1, 1)` (capacity is always a power of two in $[2^{20}, 2^{23}]$, so group count is `4,096..32,768`, strictly within Vulkan's guaranteed `maxComputeWorkGroupCount[0] == 65,535`).
- Calls `SharcResolveEntry(id.x, sharcParams, resolveParams)` with `cameraPositionPrev = sharc.prevCameraPos`, `accumulationFrameNum = sharc.accumulationFrameNum`, `responsiveFrameNum = 1`, `staleFrameNumMax = sharc.staleFrameNumMax`, `frameIndex = root.frameIndex`.
- Automatically evicts stale voxels, merges new atomic accumulation with resolved FP16 history, runs `SHARC_ENABLE_FADE_ACCELERATION` and `SHARC_BLEND_ADJACENT_LEVELS`, and zeros `accumulationBuffer[id.x]` for next frame.

### 4.3 Inline Query in `shaders/passes/indirect.slang` (`SHARC_QUERY=1` + `NRC_QUERY=1`)

When `root.sharcConstants != 0` (uniform across the entire dispatch; NRC is paused so `gNrcBridge.isActive == false`, `giReferenceBounces == 0`):

1. Add `float primaryRoughness` to `PrimaryBounce` (free — `primary` is already loaded in `samplePrimaryBounce`).
2. Trace bounce-1 ray (`PrimaryBounce bounce = samplePrimaryBounce(root, pixel)`).
3. On hit $X_2$:
   - Decode `secondary = shadeRayHit(...)`.
   - **Cache-hit fast path:** If `secondary.surface.isLit && sharcCanQuery(sharcParams, secondary.surface.position, hit.t, bounce.isSpecularLobe, bounce.primaryRoughness)`:
     - Build `sharcHitData` (`emissive = separateEmissive ? secondary.emissive : 0`).
     - If `SharcGetCachedRadiance(sharcParams, sharcHitData, cachedRadiance, false)` succeeds and `isFiniteFloat3(cachedRadiance)`:
       - **`radiance = max(cachedRadiance, 0.0)`** — skips `secondaryDirectLight` and its shadow ray completely!
       - Done tracing for this pixel.
   - **Fallback / continuation path** (ineligible $X_2$, unlit $X_2$, or cache miss):
     - Evaluate exact bounce-1 lighting at $X_2$: `radiance = secondary.emissive + secondaryDirectLight(root, shadedSecondary, origin, constants, rng, coherentRng)`.
     - If `secondary.surface.isLit && sharc.maxQueryBounces > 1`:
       - Initialize `pathRng = RTXDI_InitRandomSampler(pixel, constants.runtime.frameIndex, kSharcQueryPathSeed = 46)`, `throughput = float3(1.0)`, `curSurface = secondary.surface`, `curOrigin = origin`, `curPathLength = length(ray.Origin - gRtxdiBridge.cameraPos) + hit.t`.
       - Loop `bounceIdx = 2 .. sharc.maxQueryBounces`:
         - Sample next BRDF ray (using a small helper returning `BrdfSample` so we keep `brdf.isSpecularLobe` and `curSurface.roughness` for `sharcCanQuery`!). If absorbed, `break;`.
         - `throughput *= brdf.weight`. Trace `traceClosest(root, nextRay, nextHit)`; on miss `break;`.
         - `nextHitShaded = shadeRayHit(...)`; `curPathLength += nextHit.t`.
         - If `nextHitShaded.surface.isLit && sharcCanQuery(sharcParams, nextHitShaded.surface.position, nextHit.t, brdf.isSpecularLobe, curSurface.roughness)` and `SharcGetCachedRadiance(...)` hits (finite):
           - `radiance += throughput * max(cachedRadiance, 0.0); break;`
         - Otherwise accumulate `radiance += throughput * (nextHitShaded.emissive + directLightAt(...))` and advance `curSurface`, `curOrigin`.
4. **Firefly clamp, glass transmittance, AO & output:**
   - Existing `kGiFlagFireflyClamp` (`kMaxIndirectRadiance = 10.0`) applies identically to `radiance`.
   - **No primary AO multiplier** on SHaRC radiance (matches `RTXPT` and `RTXGI SharcIndirectDiffuse.hlsli`: primary→$X_2$ is ray-traced geometric visibility; AO only applies to NRC's cache-only residual in `nrc_resolve.slang`).
   - Glass shadow transmittance (`rawReconnectionTransmittance`) runs identically under `(root.glassFlags & kGlassFlagShadowTransmittance) != 0 && any(radiance > 0.0)`.
   - `RawPt + Sharc`: writes `indirectRadiance` directly (`rawIndirectWeightStorage == kNoneIndex`, `0 B/px` extra write!).
   - `RestirGi + Sharc`: writes `RTXDI_MakeGIReservoir(samplePos, sampleNormal, radiance, pdf)` for `rtxdi_gi_temporal` + `rtxdi_gi_shade`.

### 4.4 Debug Visualization (`DebugView::SharcCache = 33`)

- Add `DebugView::SharcCache` (`"SHaRC cache"`, `DebugViewCategory::Lighting`, `kDebugViewSharcCache = 33u`) in `src/render/debug_view.hpp`, `shaders/types.slang`, `shaders/passes/debug_view.slang`, and `src/ui/engine_ui.cpp` (with paused/inactive indicator when `!renderer.isSharcActive()`, matching `DebugView::NrcCache`).
- In `passes/debug_view.slang`, when `mode == kDebugViewSharcCache`:
  - Returns black when `(root.frameFlags & kFrameFlagIndirectDisabled) != 0 || root.sharcConstants == 0`.
  - Mode `Occupancy` (`2`): calls `HashGridDebugOccupancy(pixel, targets.renderSize, sharcParams.hashGridData)` directly across the whole screen (even over sky pixels, matching `Integration.md:196`).
  - All other modes reconstruct primary `RAB_GetGBufferSurface(int2(pixel), false)` (sky → black) and evaluate:
    - `0` (`CachedRadiance`): `SharcGetCachedRadiance` at primary `(position, geoNormal, demod, emissive=0)` (matching `SHaRCDebugCache.hlsl:66-74` and `DebugView::NrcCache`).
    - `1` (`ColoredHash`): `HashGridDebugColoredHash(position, geoNormal, gridParams)`.
    - `3` (`HashCollisions`): `HashGridDebugHashCollisions(position, geoNormal, gridParams, hashGridData)`.
    - `4` (`VoxelSize`): `v = HashGridGetVoxelSize(level, gridParams); (v / (v + 1.0)).xxx`.
    - `5` (`GridLevel`): `idColor(HashGridGetLevel(position, gridParams))`.
- Unlike `DebugView::NrcCache`, `DebugView::SharcCache` requires **no** special path inside `indirect.slang` — the cache is already resolved in BDA memory.

---

## 5. Host Lifecycle, Synchronization & GPU Timing

### 5.1 `SharcCache` (`src/render/vk_sharc.{hpp,cpp}`)

```cpp
class SharcCache
{
public:
    struct Settings
    {
        bool isUpdateEnabled = true;
        bool isResolveEnabled = true;
        bool isUpdateViewCamera = true;
        bool isMaterialDemodulationEnabled = true;
        bool isContinuousClearEnabled = false;
        uint32_t downscaleFactor = 4;        // [2, 5]
        uint32_t capacityShift = 22;         // [20, 23] -> 1M..8M entries (40..320 MiB)
        uint32_t accumulationFrameNum = 32;  // [1, 256]
        uint32_t staleFrameNumMax = 64;      // [8, 256]
        uint32_t maxUpdateBounces = 4;       // [2, 8]
        uint32_t maxQueryBounces = 2;        // [1, 4]
        float sceneScale = 35.0f;            // [1.0, 100.0]
        float minRoughnessOverride = 0.4f;   // [0.0, 1.0]
        float fireflyClamp = 0.0f;           // [0.0, 1000.0]; 0 = off (scene units, matching gi.fireflyClamp)
        SharcDebugMode debugMode = SharcDebugMode::CachedRadiance;
        bool operator==(const Settings&) const = default;
    };
    ...
};
```

**Memory & lifecycle rules (zero `waitIdle`):**
1. **Startup:** Allocates only `std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT>` for `GpuSharcConstants` (`2 × 104 B`) and `std::array<CounterReadback, MAX_FRAMES_IN_FLIGHT>` for `GpuSharcCounters` (`2 × 8 B` device-local + host-cached readback). All three cache buffers stay `nullptr` (`0 B` VRAM).
2. **`prepareFrame(frameSlot, frameNumber, isActive, renderExtent, cameraPos, isSceneChanged)`:**
   - Reads back `lastCounters` from `counters[frameSlot]` when recorded, and drains any entries in `retiredBuffers` whose `frameNumber >= entry.lastUsedFrame + MAX_FRAMES_IN_FLIGHT`.
   - If `!isActive`: if cache buffers exist and `frameNumber >= lastUsedFrame + MAX_FRAMES_IN_FLIGHT`, destroys them cleanly (`logInfo("SHaRC buffers retired", "SHaRC")`); returns `constantsAddress = 0`.
   - If `isActive`:
     - Computes `targetCapacity = 1u << std::clamp(settings.capacityShift, 20u, 23u)`.
     - If buffers are null or `allocatedCapacity != targetCapacity`: allocates the 3 new buffers (`SharcHashEntries`, `SharcAccumulation`, `SharcResolved`) exception-safely with usage `eStorageBuffer | eShaderDeviceAddress | eTransferDst`, pushes any old buffers into `retiredBuffers` (with `lastUsedFrame`), logs total MiB, sets `isClearPending = true`.
     - If `isMaterialDemodulationEnabled` or `sceneScale` changed since last active frame, or `isResolveEnabled` transitioned `false → true`, or `isSceneChanged`, or `isClearRequested` / `isContinuousClearEnabled`: sets `isClearPending = true`.
     - Updates `prevCameraPos` / `cameraPos` anchor (frozen if `!settings.isUpdateViewCamera` or `!settings.isUpdateEnabled`; snapped to `cameraPos` on first frame / clear so `cameraOffset == 0`).
     - Writes `GpuSharcConstants` into `constants[frameSlot].mapped` and updates `lastUsedFrame = frameNumber`.
3. **Returns `isDirty` / `resetNeeded`:** Any change to SHaRC encoding/quality settings or manual Clear triggers `isIndirectHistoryReset = true`, `isDlssHistoryValid = false`, and `resetAccumulation()`.

### 5.2 Command Recording & Barriers (`src/render/vk_renderer.cpp`, `src/render/vk_gpu_timer.hpp`)

Add `GpuTimerScope::SharcUpdate` (`"SHaRC update"`) and `GpuTimerScope::SharcResolve` (`"SHaRC resolve"`) immediately after `NrcUpdate` (or right before `Indirect`) in `vk_gpu_timer.hpp`.

Inside `Renderer::recordIndirect(firstCmd)`:

```text
[NRC path — when isNrcRecorded]
  NrcBeginFrame (isolated) -> NrcUpdate -> [SharcUpdate=0, SharcResolve=0]
  -> Indirect -> NrcTrain (isolated) -> NrcResolve -> (GiTemporal+GiShade if RestirGi)

[SHaRC path — when isSharcRecorded]
  [NrcUpdate=0]
  -> WAR/RAW barrier (ComputeShader -> Transfer|ComputeShader, ordering prior frame's Indirect/DebugView reads & Resolve writes)
  -> (if isClearPending: vkCmdFillBuffer × 3 + TransferWrite -> ComputeShader barrier)
  -> (if settings.isUpdateEnabled: dispatch passes/sharc_update.slang)
  -> gpuTimer.end(SharcUpdate)
  -> ComputeShader(StorageWrite) -> ComputeShader(StorageRead | StorageWrite) barrier
  -> (if settings.isResolveEnabled: dispatch passes/sharc_resolve.slang)
  -> gpuTimer.end(SharcResolve)
  -> ComputeShader(StorageWrite) -> ComputeShader(StorageRead) barrier
  -> Indirect (inline SHaRC query at X2/X3)
  -> [NrcTrain=0, NrcResolve=0]
  -> (GiTemporal+GiShade if RestirGi)

[Cache Off path]
  [NrcUpdate=0, SharcUpdate=0, SharcResolve=0]
  -> Indirect -> [NrcTrain=0, NrcResolve=0] -> (GiTemporal+GiShade if RestirGi)
```

**Why every barrier is exact (zero sync hazards, zero redundant barriers):**
1. **Cross-frame barrier before Clear / `sharc_update`:** Because `MAX_FRAMES_IN_FLIGHT == 2` and SHaRC buffers are single-buffered persistent state, frame $N+1$'s `fillBuffer` / `sharc_update` writes must wait for frame $N$'s `indirect` / `debug_view` reads and `sharc_resolve` writes on the same graphics queue. One `ComputeShader(StorageWrite) → Transfer(TransferWrite) | ComputeShader(StorageRead | StorageWrite)` barrier guarantees zero `SYNC-HAZARD-WRITE-AFTER-READ` or `WRITE-AFTER-WRITE`.
2. **Clear → `sharc_update` barrier (only when `isClearPending`):** `Transfer(TransferWrite) → ComputeShader(StorageRead | StorageWrite)` (`SharcIntegration.cpp:483-485`).
3. **`sharc_update` → `sharc_resolve` barrier:** `ComputeShader(StorageWrite) → ComputeShader(StorageRead | StorageWrite)` orders atomic `hashEntries` & `accumulation` writes AND `resolved` WAR (`sharc_update` cache resampling reads previous frame's `resolved` before `sharc_resolve` updates it).
4. **`sharc_resolve` → `indirect` barrier:** `ComputeShader(StorageWrite) → ComputeShader(StorageRead)` orders `hashEntries` & `resolved` writes before `indirect.slang` and `debug_view.slang` read them.
5. **Pre-acquire submit (`MG-P7`):** Because `isNrcRecorded == false` under SHaRC, `recordDirectLight` + `sharc_update` + `sharc_resolve` + `indirect` + `combine` + `glass` ALL remain inside the pre-acquire submit before `AcquireImage` + `DLSS-RR`, eliminating the NRC CUDA isolation stall!

---

## 6. Presets, UI (`SettingsWindow`), `SettingsRegistry` & Docs

### 6.1 Unblocking SHaRC & Updating `RTXPT Analog`

1. `src/render/pipeline_modes.hpp`:
   - `isRadianceCacheBuilt(RadianceCacheMode::Sharc)` returns `true`.
   - `radianceCacheModeLabel(RadianceCacheMode::Sharc)` changes from `"SHaRC (M8.4)"` to `"SHaRC"`.
2. `src/render/settings_registry.cpp`:
   - Remove `kRadianceCachePlanned` from `frame.radianceCache` (`frame.radianceCache=sharc` parses and applies cleanly).
   - Switch `PipelinePreset::RtxptAnalog` (`pipeline=rtxpt`) from `RadianceCacheMode::Off` to **`RadianceCacheMode::Sharc`**. All 5 presets remain pairwise distinct:
     - `EngineTargetRis`: `StreamingRis` (`Power_RIS` 16/1/0, exact sun) + `RawPt` + `Nrc`
     - `EngineTargetTemporal`: `RestirDi` (`Temporal`, `Basic`, history 6) + `RawPt` + `Nrc`
     - `RtxptAnalog`: `StreamingRis` (`Uniform` 8/1/0, non-exact inf) + `RawPt` + **`Sharc`**
     - `RtxdiAnalog`: `RestirDi` (`Medium`) + `RestirGi` + `Off`
     - `RtxgiAnalog`: `StreamingRis` (`Uniform` 8/1/0, non-exact inf) + `RawPt` + `Nrc`

### 6.2 `SettingsRegistry` (`sharc.*`) & `SettingsWindow`

Register `SharcCache::Settings sharc` on `LabConfig` and add 15 built switches (`sharc.*`):

| Switch | Type | Default | Range / Values | Notes |
|---|---|---|---|---|
| `sharc.update` | `Bool` | `on` | `on\|off` | Freeze/unfreeze cache updates |
| `sharc.resolve` | `Bool` | `on` | `on\|off` | Freeze/unfreeze temporal resolve |
| `sharc.updateCamera` | `Bool` | `on` | `on\|off` | Freeze hash-grid camera anchor |
| `sharc.downscale` | `Uint` | `4` | `2..5` | Sparse update grid step `1/(N×N)` |
| `sharc.capacityShift` | `Uint` | `22` | `20..23` | Entries = $2^k$ (`1M..8M`, `40..320 MiB`) |
| `sharc.sceneScale` | `Float` | `35.0` | `1.0..100.0` | World-space voxel density |
| `sharc.accumulationFrames` | `Uint` | `32` | `1..256` | Temporal accumulation window |
| `sharc.staleFrames` | `Uint` | `64` | `8..256` | Stale eviction threshold |
| `sharc.updateBounces` | `Uint` | `4` | `2..8` | Max update bounces |
| `sharc.queryBounces` | `Uint` | `2` | `1..4` | Max query bounces on miss/ineligible |
| `sharc.minRoughness` | `Float` | `0.4` | `0.0..1.0` | Update-pass roughness floor |
| `sharc.fireflyClamp` | `Float` | `0.0` | `0.0..1000.0` | Optional scene-unit direct-light clamp (`0` = off, matches `gi.fireflyClamp`) |
| `sharc.materialDemodulation` | `Bool` | `on` | `on\|off` | Auto-clears cache on flip |
| `sharc.debugMode` | `Enum` | `radiance` | `radiance\|hash\|occupancy\|collisions\|voxelSize\|level` | Visualization for `DebugView::SharcCache` |

In `SettingsWindow` (`src/ui/settings_window.cpp` & `src/ui/settings_rows.hpp`):
- Add `kIndirectSharc` row table and an **`Indirect Lighting → SHaRC`** tree node beside `NRC` (disabled when `!renderer.isSharcActive()`), showing all rows above, current VRAM allocation (`0 MiB` when inactive, `40 / 80 / 160 / 320 MiB` when active), and a one-click **`Clear cache`** button.
- Reset `sharc` to defaults in `Settings → Defaults`.

---

## 7. Implementation Waves

### Wave A · SDK, Device Features, `SharcCache` Host & Shaders
- **Modified:** `ThirdParty/CMakeLists.txt`, `CMakeLists.txt`, `src/core/vk_device.cpp`, `src/core/types.hpp`, `shaders/types.slang`, `src/core/vk_frame_data.{hpp,cpp}`, `src/render/vk_gpu_timer.hpp`, `src/render/vk_shader_objects.{hpp,cpp}`, `src/render/debug_view.hpp`
- **Created:** `src/render/vk_sharc.{hpp,cpp}`, `shaders/gi/sharc_constants.slang`, `shaders/gi/sharc_bridge.slang`, `shaders/passes/sharc_update.slang`, `shaders/passes/sharc_resolve.slang`
- **Modified shaders:** `shaders/passes/indirect.slang`, `shaders/passes/debug_view.slang`
- **Done when:** Both `windows-clang-release` and `windows-clang-release-checks` compile all shaders cleanly (`0` warnings/errors) and `layout_test` passes (`GpuFrameRoot == 256 B`, `GpuSharcConstants == 96 B`).

### Wave B · Renderer Wiring, Presets, UI, Registry & Docs
- **Modified:** `src/render/vk_renderer.{hpp,cpp}`, `src/render/pipeline_modes.hpp`, `src/render/settings_registry.{hpp,cpp}`, `src/ui/settings_rows.hpp`, `src/ui/settings_window.{hpp,cpp}`, `src/ui/engine_ui.cpp`, `tests/layout_test.cpp`, `tests/lab_registry_test.cpp`, `docs/settings.md`, `spec/considerations.md`, `spec/m8/README.md`
- **Done when:** `ctest` passes (`8/8` checks, `7/7` release), including `frame.radianceCache=sharc` round-trip, `pipeline=rtxpt` using `RadianceCacheMode::Sharc`, all 15 `sharc.*` switches, and `settingsRows` coverage.

### Wave C · Convergence, Validation, GPU-AV & Performance Verification
- Execute all §8 gates and record measured convergence, GPU timing (ABBA vs NRC and vs Cache Off), and VRAM results in §9 of this spec.

---

## 8. Verification & Acceptance Criteria

### 8.1 CPU Unit Tests (`ctest`, both release & checks presets)
- `layout_test`: `sizeof(GpuFrameRoot) == 256` (`sharcConstants` @ 248), `sizeof(GpuSharcConstants) == 96`.
- `lab_registry_test`:
  - `frame.radianceCache=sharc` parses cleanly (`!isPlanned`).
  - `pipeline=rtxpt` sets `RadianceCacheMode::Sharc` and all 5 `pipeline=*` presets remain pairwise distinct.
  - All `sharc.*` switches round-trip through `formatLabConfig` / `parseLabConfig` and exist in `settingsRows::kAll`.
  - All 57 `tests/lab/*.lab` files + 5 built-in plans parse cleanly.

### 8.2 GPU Convergence & Correctness Gates (`windows-clang-release-checks`, layers off)
1. **Multi-bounce convergence vs 8-bounce ground truth (`ENGINE_LAB` on `sponza.gallery`, `sponza.courtyard`, `cafe.tables`):**
   - Measure `RawPt + Sharc` (`frame.indirectBounce=rawPt frame.radianceCache=sharc`) and `RestirGi + Sharc` against the 8-bounce path-traced truth (`giBias`: `8b.bias`, `8b.p90`, `darkLeak`), and compare side-by-side against `RawPt + Nrc` and `RawPt + Off`.
   - Pass criterion: `RawPt + Sharc` captures multi-bounce energy substantially closer to the 8-bounce truth than 1-bounce `RawPt + Off`, with `darkLeak == 0` and no NaN/Inf/firefly divergence.
2. **Debug views & modes:** Verify `DebugView::SharcCache` across all 6 `sharc.debugMode` values (`radiance`, `hash`, `occupancy`, `collisions`, `voxelSize`, `level`) and when switching `Cache: Off ↔ NRC ↔ SHaRC`.
3. **Existing self-test suite:** `ENGINE_CULL_CHECK=1`, `ENGINE_MOTION_TEST=1`, `ENGINE_DLSS_CHECK=1`, `ENGINE_GUIDE_CAPTURE=1 ENGINE_RT_CHECK=1`, `ENGINE_GI_CHECK=1` continue to pass with no regressions.

### 8.3 Vulkan Validation & GPU-AV (`windows-clang-release-checks`)
1. **Core + Synchronization + Best Practices + NVIDIA (`REPORT_FLAGS=error,warn,perf`):**
   - Static runs: `pipeline=rtxpt` (`RawPt + Sharc`), `frame.indirectBounce=restirGi frame.radianceCache=sharc`, `sharc.capacityShift=20`, `DebugView::SharcCache`.
   - **Live toggle & resize stress:** cycle `Cache: Off → SHaRC → NRC → SHaRC → Off` and live `sharc.capacityShift` / `sharc.materialDemodulation` flips across frames: **0 engine VUID errors, 0 sync hazards (`SYNC-HAZARD-*`), 0 VMA leak assertions at exit**.
2. **GPU-AV** (default no-DGC path):
   - Static `pipeline=rtxpt` + live `Off ↔ SHaRC ↔ NRC` cycling: **0 GPU-AV out-of-bounds, uninitialized-descriptor, or atomic-access errors**.

### 8.4 ABBA Release Timing & Lazy VRAM Verification (RTX 5070, 2560×1440 Quality, layers off, `ENGINE_BENCH_FRAMES=600`)
- **ABBA `RawPt + Sharc` vs `RawPt + Nrc` vs `RawPt + Off`** on Sponza (gallery + courtyard) and Bistro café:
  - Record `SharcUpdate`, `SharcResolve`, `Indirect`, `RayStartWait`, `AcquireWait`, and `GPU total` (ms).
  - Verify `pipeline=rtxpt` (`RawPt + Sharc`) beats `RawPt + Nrc` in GPU total frame time (target: SHaRC `Update + Resolve < 0.8 ms` combined, plus `Indirect` speedup from skipping $X_2$ NEE shadow rays and eliminating NRC CUDA isolation!).
  - Verify **zero-cost off**: when `Cache != Sharc`, `SharcUpdate == 0.000 ms`, `SharcResolve == 0.000 ms`, `0 MiB` SHaRC VRAM allocated; when switching `SHaRC → Off` or `SHaRC → NRC`, `SHaRC buffers retired` logs after `MAX_FRAMES_IN_FLIGHT` frames with zero `waitIdle`.

---

## 9. Verification Results (2026-11-27, RTX 5070)

### 9.1 Builds, SPIR-V & Unit Tests
- `windows-clang-release-checks` (`ctest`): **8/8 passed** (`layout_test`: `GpuFrameRoot == 256 B`, `GpuSharcConstants == 96 B`; `lab_registry_test`: `frame.radianceCache=sharc`, `pipeline=rtxpt`, all 15 `sharc.*` switches, `settingsRows::kAll`, all 57 `.lab` plans + 5 built-in plans).
- `windows-clang-release` (`ctest`): **7/7 passed**.
- `spirv-val --target-env vulkan1.4 --scalar-block-layout`: **4/4 passed** (`sharc_update.spv`, `sharc_resolve.spv`, `indirect.spv`, `debug_view.spv`).

### 9.2 Multi-Bounce Convergence vs 8-Bounce Ground Truth (`ENGINE_LAB` `giBias`, layers off)
`Indirect radiance`, `di=shipped gi=shipped frame=shipped frame.jitter=off`, seed 0 (`8b.*` against 8-bounce path-traced truth):

| Scene | Config | `8b.bias` | `8b.p50` | `8b.p90` | `8b.rgbBias` | `8b.darkLeak` |
|---|---|---:|---:|---:|---:|---:|
| `sponza.gallery` | `RawPt + Off` (1-bounce) | `-39.85%` | `100.0%` | `100.0%` | `44.94%` | `0` |
| `sponza.gallery` | **`RawPt + Sharc`** | **`-1.86%`** | `7.87%` | `14.56%` | `2.22%` | `0` |
| `sponza.gallery` | `RawPt + Nrc` | `-3.74%` | `6.97%` | `12.66%` | `4.44%` | `0` |
| `sponza.gallery` | `RestirGi + Sharc` *(gi=shipped Basic)* | `-22.22%` | `11.28%` | `33.41%` | `24.62%` | `0` |
| `sponza.courtyard` | `RawPt + Off` (1-bounce) | `-13.12%` | `23.50%` | `100.0%` | `14.35%` | `0` |
| `sponza.courtyard` | **`RawPt + Sharc`** | **`-5.34%`** | `6.95%` | `22.14%` | `7.43%` | `0` |
| `sponza.courtyard` | `RawPt + Nrc` | `-0.59%` | `1.12%` | `6.70%` | `0.69%` | `0` |
| `sponza.courtyard` | `RestirGi + Sharc` *(gi=shipped Basic)* | `-11.80%` | `11.32%` | `26.95%` | `14.47%` | `0` |
| `cafe.tables` | `RawPt + Off` (1-bounce) | `-18.29%` | `21.93%` | `35.42%` | `20.35%` | `0` |
| `cafe.tables` | **`RawPt + Sharc`** | **`-5.49%`** | `7.62%` | `19.96%` | `6.02%` | `0` |
| `cafe.tables` | `RawPt + Nrc` | `-3.84%` | `3.94%` | `13.45%` | `4.07%` | `0` |
| `cafe.tables` | `RestirGi + Sharc` *(gi=shipped Basic)* | `-14.70%` | `14.29%` | `31.76%` | `16.55%` | `0` |

### 9.3 Debug Views & Regression Self-Tests (layers off)
- **`DebugView::SharcCache` (`sponza.gallery`):** black (`mean 0`) when `Cache == Off` or `Cache == NRC`; finite non-zero output across all 6 modes (`radiance 0.001723`, `hash 0.1286`, `occupancy 0.01261`, `collisions 0.07295`, `voxelSize 0.06674`, `level 0.3559`).
- **Existing GPU self-test suite:** `ENGINE_CULL_CHECK=1` (`16/16`), `ENGINE_MOTION_TEST=1` (`13/13`), `ENGINE_DLSS_CHECK=1` (`11/11`, `0 failed`), `ENGINE_GUIDE_CAPTURE=1 ENGINE_RT_CHECK=1` (`3/3` convergence, `2/2` glass, `8890/8890` ray frames matched), `ENGINE_GI_CHECK=1` (**`14/14` passed**).

### 9.4 Vulkan Validation & GPU-AV
- **Core + Sync + Best Practices + NVIDIA (`REPORT_FLAGS=error,warn,perf`):** static `pipeline=rtxpt`, `RestirGi + Sharc (capacityShift=20)`, plus live `Off → SHaRC (160 MiB, all 6 debug modes) → NRC → SHaRC (40 MiB resize + materialDemodulation=off) → Off` cycling: **0 engine VUID errors, 0 sync hazards, 0 VMA leak assertions** (only known `C-H1-2` NGX WAW and `C-G3-3` NRC startup objects).
- **GPU-AV:** live `Off → pipeline=rtxpt → NRC → SHaRC (capacityShift=20, materialDemodulation=off) → Off`: **0 GPU-AV out-of-bounds, descriptor, or 64-bit atomic errors**.

### 9.5 ABBA Release Timings & Lazy VRAM (RTX 5070, 2560×1440 Quality / 1707×960 render, layers off, `ENGINE_BENCH_FRAMES=600`)

| Scene | Config | SHaRC Upd | SHaRC Res | Indirect | NRC Total | RayStartWait | GPU total |
|---|---|---:|---:|---:|---:|---:|---:|
| Sponza gallery | **`pipeline=rtxpt`** (`StreamingRis + RawPt + Sharc`, `q=2`) | 0.352 ms | 0.150 ms | 3.219 ms | 0.001 ms | 0.014 ms | **8.966 ms** |
| Sponza gallery | **`RawPt + Sharc` (`sharc.queryBounces=1`)** | 0.406 ms | 0.141 ms | **2.493 ms** | 0.001 ms | 0.012 ms | **10.050 ms** |
| Sponza gallery | **`RawPt + Sharc` (`q=2`, default)** | 0.403 ms | 0.139 ms | 3.334 ms | 0.001 ms | 0.013 ms | **10.925 ms** |
| Sponza gallery | `RawPt + Nrc` (`idleTrainingFrames=512`) | 0.000 ms | 0.000 ms | 2.956 ms | 1.148 ms | 0.015 ms | 11.151 ms |
| Sponza gallery | `RawPt + Nrc` (`idleTrainingFrames=0`, continuous) | 0.000 ms | 0.000 ms | 3.496 ms | 1.254 ms | 0.014 ms | 11.773 ms |
| Sponza gallery | `RawPt + Off` (1-bounce) | 0.000 ms | 0.000 ms | 2.646 ms | 0.001 ms | 0.012 ms | 9.609 ms |
| Sponza courtyard | **`RawPt + Sharc` (`q=2`)** | 0.576 ms | 0.142 ms | 4.029 ms | 0.001 ms | 0.015 ms | 12.916 ms |
| Sponza courtyard | `RawPt + Nrc` (`idleTrainingFrames=512`) | 0.000 ms | 0.000 ms | 3.413 ms | 1.328 ms | 0.016 ms | 12.809 ms |
| Sponza courtyard | `RawPt + Off` (1-bounce) | 0.000 ms | 0.000 ms | 3.513 ms | 0.001 ms | 0.014 ms | 11.693 ms |
| Bistro `cafe tables` | **`pipeline=rtxpt`** (`StreamingRis + RawPt + Sharc`, `q=2`) | 1.069 ms | 0.234 ms | 7.780 ms | 0.001 ms | 0.013 ms | **17.866 ms** |
| Bistro `cafe tables` | **`RawPt + Sharc` (`sharc.queryBounces=1`)** | 1.722 ms | 0.224 ms | **5.536 ms** | 0.001 ms | 0.014 ms | **23.196 ms** |
| Bistro `cafe tables` | `RawPt + Off` (1-bounce) | 0.000 ms | 0.000 ms | 8.068 ms | 0.001 ms | 0.012 ms | 23.509 ms |
| Bistro `cafe tables` | `RawPt + Nrc` (`idleTrainingFrames=512`) | 0.000 ms | 0.000 ms | 8.142 ms | 1.282 ms | 0.336 ms | 25.105 ms |
| Bistro `cafe tables` | `RawPt + Sharc` (`q=2`) | 1.762 ms | 0.224 ms | 8.603 ms | 0.001 ms | 0.013 ms | 25.972 ms |

- **Zero-cost off & lazy VRAM verified:** when `Cache != Sharc`, `SharcUpdate`/`SharcResolve` measure `0.0001–0.0004 ms` (empty timestamp scopes) with `0 MiB` allocated; switching away from SHaRC logs `SHaRC buffers retired (160.0 MiB freed)` after `MAX_FRAMES_IN_FLIGHT` frames with zero `waitIdle`.
- **Key M8.5 takeaway:** `sharc.queryBounces=1` skips the bounce-2 fallback ray on ineligible short/glossy segments, cutting Bistro `cafe tables` `Indirect` from `8.068 ms` (1-bounce `Off`) to **`5.536 ms` (`-31.4%`)** and bringing total frame time below both 1-bounce `Off` (`23.196` vs `23.509 ms`) and `RawPt + Nrc` (`25.105 ms`) while eliminating NRC's `Ray start wait` (`0.336 → 0.014 ms`).
