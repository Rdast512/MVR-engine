# M8 · I3: Mega Geometry fixes

**Status:** built 2026-11-21 · **Milestone:** M8 ([README](./README.md))
**Implements:** findings MG-B1, MG-B2, MG-B3, MG-B4 ([mg_optimizations_fixes.md](./mg_optimizations_fixes.md) §2), C-C4-5 ([considerations.md](../considerations.md))
**Depends on:** M2 C1/C4, M6 D2
**References:**
- Vulkan 1.4.363 `validusage.json`: `VUID-VkMicromapCreateInfoEXT-offset-07438`, `VUID-vkCmdBuildPartitionedAccelerationStructuresNV-pBuildInfo-10537/-10538/-10542/-10544/-10545`
- `reference/vk_partitioned_tlas-main/shaders/animation_update_instances.comp.glsl` (in-place `WRITE_INSTANCE` partition reassignment)
- `reference/RTXGI/Include/NrcCommon.h:145` (NRC `sceneBounds` requirement)

## Scope

Fix all four Mega Geometry bugs and spec issues from [mg_optimizations_fixes.md](./mg_optimizations_fixes.md) §2 and measure partition reassignment (C-C4-5) on a moving instance field. No perf levers from §3 (those wait for I4 and I10).

| Finding | Target change | Files |
|---|---|---|
| MG-B1 | Pass `minAlignment = alignment` to `createBuffer` (`vmaCreateBufferWithAlignment`), drop `+ alignment - 1` slack, verify `base % alignment == 0` so `VkMicromapCreateInfoEXT::offset == plan.storageOffset` (always a multiple of 256 B, `VUID-VkMicromapCreateInfoEXT-offset-07438`) | `src/core/vk_mega_geometry.{hpp,cpp}`, `src/render/vk_rt_scene.{hpp,cpp}` |
| MG-B2 | Allocate `eHostVisible \| eHostCoherent \| eHostCached` (`HOST_ACCESS_RANDOM_BIT`) readback buffers for `MegaGeometry` `eComputeSizes` outputs and `RayTracingScene` per-frame `argCount` (via 4 B `copyBuffer2` after PTLAS build, keeping GPU atomics in VRAM); return `blasAddresses` from CPU in `placeStructures()` instead of reading back mapped WC VRAM | `src/core/vk_mega_geometry.{hpp,cpp}`, `src/render/vk_rt_scene.{hpp,cpp}` |
| MG-B3 | Clamp grown PTLAS `instances` (computed in `uint64_t`) to `maxInstanceCount` and divide 64-bit `vk::DeviceSize` directly in the PTLAS size log | `src/render/vk_rt_scene.cpp` |
| MG-B4 | Track per-entity local ray-traced AABBs and last-checked `Transform`s in `RayTracingScene`; on `prepareFrame`, grow `templates.sceneBounds` monotonically with geometric headroom (`max(25% extent, 1 m)`) whenever a moved entity leaves its current box | `src/core/object_storage.hpp`, `src/render/vk_rt_scene.{hpp,cpp}` |
| C-C4-5 | Add `ENGINE_BENCH_FIELD_MOVE=1` (wraps field instances across XZ partitions each frame) and ABBA-measure fixed partitions vs dynamic partition reassignment (`Settings::isPartitionReassignmentEnabled`) on `ENGINE_BENCH_FIELD=64` | `src/core/engine_bench.{hpp,cpp}`, `src/core/vk_engine.cpp`, `src/render/vk_rt_scene.{hpp,cpp}` |

## Decisions

| Decision | Rule | Why |
|---|---|---|
| Alignment check | Pass `minAlignment = alignment` to `createBuffer` and throw `std::runtime_error` if `alignment > 1 && base % alignment != 0` | Enforced in Release as well as Debug; guarantees `storageStart == 0` for `VkMicromapCreateInfoEXT::offset` |
| `argCount` readback | Keep `frames[slot].operation` in ReBAR VRAM (`HostBuffer` + `eTransferSrc`) and copy its 4-byte `argCount` to a 4-byte `eHostCached` `argCountReadback` buffer after PTLAS build | Eliminates WC CPU reads in `collectResults()` while keeping GPU `InterlockedAdd` atomics in local VRAM and outside timestamp queries |
| Dynamic `sceneBounds` hysteresis | Skip entities whose `Transform` is unchanged; when a moved entity leaves `templates.sceneBounds`, expand only breached face(s) by overshoot + `max(25% extent, 1.0 m)` | Zero overhead and bit-identical bounds on static scenes; O(log distance) NRC `Configure` calls instead of per-frame stalls / SDK leaks (C-G3-3) |
| C-C4-5 partition reassignment | Reassign changed instances to their current XZ grid cell on CPU, sync `builds` per frame slot (without forcing a full PTLAS build unless `occupancy > ptlas.sizedFor.maxPerPartition`); keep on or off based on ABBA moving-field measurements | `WRITE_INSTANCE` supports in-place partition moves; `VUID-vkCmdBuildPartitionedAccelerationStructuresNV-pBuildInfo-10538` requires resizing if a partition exceeds `maxPerPartition` |

## Results

RTX 5070, release, no layers (unless stated), 1280×720 window (853×480 render, DLSS-RR Quality), 2026-11-21.

### 1. MG-B1, MG-B2, MG-B3, MG-B4

- **MG-B1 (`vk_mega_geometry.{hpp,cpp}`, `vk_rt_scene.{hpp,cpp}`):** `createOwnedBuffer` and `createAlignedBuffer` pass `minAlignment = alignment` directly to `createBuffer` (`vmaCreateBufferWithAlignment`), drop `+ alignment - 1` slack, and enforce `base % alignment == 0` at runtime. `VkMicromapCreateInfoEXT::offset` is now `plan.storageOffset` (guaranteed multiple of 256 B, `VUID-VkMicromapCreateInfoEXT-offset-07438`). Sponza loads cleanly (`3500 CLAS` in `5.36 MiB`, `103` cluster BLAS in `0.24 MiB`, `14` micromaps in `1.11 MiB`, `built in 40.4–51.0 ms`).
- **MG-B2 (`vk_mega_geometry.{hpp,cpp}`, `vk_rt_scene.{hpp,cpp}`):**
  - `MegaGeometry::buildModel` allocates a host-cached `ClusterAsSizesReadback` (`eHostVisible | eHostCoherent | eHostCached`, `HOST_ACCESS_RANDOM_BIT`) for `eComputeSizes` outputs and copies `blasAddresses` from the CPU vector returned by `placeStructures()` instead of reading mapped WC VRAM.
  - `RayTracingScene` keeps `frames[slot].operation` in ReBAR VRAM (preserving local GPU `InterlockedAdd` atomics) and copies the 4-byte `argCount` via `copyBuffer2` after the PTLAS build timestamp into a host-cached `PtlasArgCountReadback_<slot>` buffer read by `collectResults()`.
- **MG-B3 (`vk_rt_scene.cpp`):** PTLAS growth computes `grownInstances` in `uint64_t`, clamps to `min(maxInstanceCount, UINT32_MAX)`, only doubles `instances` when `templates.shape.instances > previous.instances` (avoiding wasted 2× instance capacity when only `maxPerPartition` grows), and uses a `std::unsigned_integral` `divideRoundUp` template to log 64-bit `vk::DeviceSize` directly.
- **MG-B4 (`object_storage.hpp`, `vk_rt_scene.{hpp,cpp}`):** `updateDynamicEntities()` skips unchanged entities in O(1) (`Transform::operator==`) and expands `templates.sceneBounds` monotonically on breached faces with `max(25% extent, 1.0 m)` headroom. Verified under `ENGINE_INDIRECT_MODE=nrc-restir-gi ENGINE_MOTION_TEST=1` with validation: Sponza's translation and rotation expanded bounds 3 times across 66 moving frames (`(-15.7,-1.1,-9.6)–(14.7,11.6,9.0)` → `(-25.1,-1.1,-14.4)–(22.3,11.6,13.7)`), triggering clean NRC reconfigures with 13 of 13 motion scenarios passing and zero engine validation messages.

### 2. C-C4-5 partition reassignment ABBA benchmark

`ENGINE_BENCH_FRAMES=1200 ENGINE_BENCH_FIELD=64 ENGINE_BENCH_FIELD_MOVE=1` (4,096 bunny instances + 103 Sponza primitives = 4,199 instances, 9×9 = 81 partitions, alternating rows translating ±0.25 cell/frame and wrapping across the full 64-cell field width, 4,096 PTLAS writes/frame). Alternating `A1 / B1 / B2 / A2`:

| Scope (mean ms) | Fixed partitions (`REASSIGN=0`, A1 / A2) | Partition reassignment (`REASSIGN=1`, B1 / B2) | Mean delta |
|---|---|---|---|
| **PTLAS build (async)** | 0.1941 / 0.1869 (**0.1905**) | 0.1416 / 0.1379 (**0.1398**) | **−0.0507 ms (−26.6%)** |
| **RT instances (async)** | 0.0009 / 0.0009 (**0.0009**) | 0.0009 / 0.0010 (**0.0010**) | +0.0001 ms |
| **ReSTIR DI initial** | 0.2970 / 0.2942 (**0.2956**) | 0.2809 / 0.2812 (**0.2811**) | **−0.0145 ms (−4.9%)** |
| **Indirect** | 0.5625 / 0.5664 (**0.5645**) | 0.5576 / 0.5565 (**0.5571**) | **−0.0074 ms (−1.3%)** |
| **ReSTIR GI shade** | 0.5936 / 0.5955 (**0.5946**) | 0.5857 / 0.5817 (**0.5837**) | **−0.0109 ms (−1.8%)** |
| **GPU total** | 3.2605 / 3.2564 (**3.2585**) | 3.2363 / 3.2184 (**3.2274**) | **−0.0311 ms (−1.0%)** |

- **Decision:** Shipped on by default (`RayTracingScene::Settings::isPartitionReassignmentEnabled = true`, overridable via `ENGINE_PTLAS_REASSIGN=0`). Zero cost on static scenes (skipped by transform equality); −26.6% PTLAS build time and consistent trace gains across all ray passes when instances travel across cells. Tightened `createPtlas()` kept PTLAS storage at `1405 KiB` / `1386 KiB` scratch (`4199` instances, `312` max/partition) when occupancy briefly exceeded `156`. Closes **C-C4-5**.

### 3. Verification

- **Vulkan validation** (core + sync + best practices + NVIDIA best practices, GPU-AV off per C-E1-3):
  - `ENGINE_INDIRECT_MODE=cycle ENGINE_BENCH_FRAMES=120`: **clean** (only known NGX C-H1-2 and NRC C-G3-3 messages, 29 lines).
  - `ENGINE_INDIRECT_MODE=nrc-restir-gi ENGINE_MOTION_TEST=1`: **clean** (13/13 scenarios passed, 3 dynamic bounds NRC reconfigures, 29 lines).
  - `ENGINE_BENCH_FRAMES=120 ENGINE_BENCH_FIELD=64 ENGINE_BENCH_FIELD_MOVE=1`: **clean** (4,096 moving instances/frame with partition reassignment + `copyBuffer2` sync, 29 lines).
- **Regression suite (release, no layers):**
  - CTest: **7 of 7 passed** (`hiz_footprint`, `brdf`, `lights`, `rtxdi_math` — updated for I2 F20's symmetric `[1/3, 3]` bounds — `layout`, `nrc_library`, `dlss`).
  - `ENGINE_RT_CHECK=1`: **32 of 32 frames** matched the vis buffer.
  - `ENGINE_CULL_CHECK=1`: **16 of 16 poses** identical.
  - `ENGINE_MOTION_TEST=1`: **13 of 13 scenarios** passed.
  - `ENGINE_GUIDE_CAPTURE=1`: **2 of 2 normal checks**, **3 of 3 DI convergence checks** passed.
  - `ENGINE_GI_CHECK=1`: **8 of 8 passed**.

## Done when

- [x] MG-B1, MG-B2, MG-B3, MG-B4 implemented
- [x] C-C4-5 partition reassignment ABBA-measured on `ENGINE_BENCH_FIELD=64 ENGINE_BENCH_FIELD_MOVE=1` and shipped on by default (−26.6% PTLAS build, −0.031 ms GPU total)
- [x] Vulkan validation (core + sync + best practices + NVIDIA best practices; GPU-AV off per C-E1-3) clean on `ENGINE_INDIRECT_MODE=cycle`, `ENGINE_INDIRECT_MODE=nrc-restir-gi ENGINE_MOTION_TEST=1`, and moving instance field
- [x] CTest (7/7), `ENGINE_RT_CHECK` (32/32), `ENGINE_CULL_CHECK` (16/16), `ENGINE_MOTION_TEST` (13/13), `ENGINE_GUIDE_CAPTURE` (2/2 + 3/3), and `ENGINE_GI_CHECK` (8/8) all pass
