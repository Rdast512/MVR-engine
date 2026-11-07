---
name: pbr-implementation-plan
description: Task plan for glTF metallic-roughness PBR on the engine_spec architecture (NVIDIA RTX 20+ now, NVIDIA-only pieces behind their own modules) — visibility-buffer compute resolve, Mega Geometry cluster RT (CLAS, GPU TLAS), SER RT pipeline for shaded rays and ray queries for visibility, RTXDI ReSTIR DI, SHaRC/NRC radiance cache with ReSTIR GI, per-cluster opacity micromaps, DLSS Ray Reconstruction. Use when implementing BRDF, lights, sampleMaterial, the resolve and its guides, CLAS/TLAS, OMM, RTXDI, SHaRC, NRC, ReSTIR GI, DLSS-RR, or post.
---

# Plan: PBR on the visibility buffer + RTXDI + SHaRC/NRC + ReSTIR GI + OMM + DLSS-RR

**Date:** 2026-11-07 · **Verified against:** `feature-pbr` working tree on `a50ebcb46` plus the uncommitted visibility buffer (V1–V7), Slang `v2026.20`, meshoptimizer `v1.3`
**Precedence:** [docs/engine_spec.md](../docs/engine_spec.md) is the architecture. This plan holds the shading tasks and follows it; where they disagree, the spec wins. Milestones are the spec's M0–M9 (§16.2).
**Status:** M1 is built. The visibility buffer, the compute resolve, all DLSS guides, `sampleMaterial` (B1) and `envBrdfApprox` exist. No lighting, AS or OMM code exists yet. See §3.

| Doc | Role |
|---|---|
| [engine_spec.md](../docs/engine_spec.md) | Target architecture, milestones, budgets (normative) |
| [visibility_buffer.md](./features/visibility_buffer.md) | Vis buffer and resolve design, as-built notes (§10) |
| [pbr_lighting_materials.md](./pbr_lighting_materials.md) | Shading rules (normative for agents) |
| [pbr_lighting_materials.html](./pbr_lighting_materials.html) | Human explainer with live graphs and scenes |
| [gltf_parse_resources.md](./gltf_parse_resources.md) §5, §13.3, §14 | Normative glTF math |
| [resource_architecture.md](./resource_architecture.md) | Buffers, model slices, frame root, BDA + heap handle rules |
| [shader_architecture_plan.md](./shader_architecture_plan.md) | Shader file map and pass order |
| [VK_EXT_opacity_micromap.md](./VkExtensionsSpecs/VK_EXT_opacity_micromap.md) | OMM extension notes |
| `reference/vk_lod_clusters` | CLAS, cluster BLAS and GPU instance-write reference |
| `ThirdParty/_deps/src/meshoptimizer/src/meshoptimizer.h` | Geometry processing API (v1.3): tangents, normals, opacity micromaps, clusters |

---

## 0. The short version

Vendor SDKs own all lighting. The engine writes only the glue they need.

| Concern | Owner | Engine writes |
|---|---|---|
| Primary visibility | engine: vis buffer + compute resolve (**done**) | payload raster, resolve, guides |
| RT geometry | **Mega Geometry**: CLAS per cluster, cluster BLAS per primitive, GPU TLAS | load-time builds, GPU instance write |
| Direct light | **RTXDI** ReSTIR DI (punctual + emissive-triangle lights) | `RAB_*` bridge, GPU light buffer, two CS entry points |
| Indirect light | one SER bounce + **SHaRC** (floor) / **NRC** (reference) behind `IRadianceCache` + **RTXDI ReSTIR GI** | raygen, minimal closest-hit, cache bridges, resolve passes |
| Alpha-tested rays | **meshoptimizer** OMM baker + `VK_EXT_opacity_micromap`, per cluster | load-time bake, micromap build, CLAS attach |
| Denoise + upscale | **DLSS Ray Reconstruction** (NGX) | guides (exist), host evaluate |

Load-time geometry processing (tangents, normals, clusters, OMM) is **meshoptimizer**. The engine has no tangent, normal, clustering or OMM algorithm of its own.

Everything else is the minimum those SDKs need:

1. **One BRDF, one light model.** glTF Appendix B GGX with VNDF sampling + `KHR_lights_punctual`, written once in Slang. RTXDI, the indirect pass and the caches call it through their bridges. No reference light loop, no homegrown cache, no custom denoiser.
2. **Primary visibility is the visibility buffer.** Raster writes depth + a 32-bit payload. One compute resolve builds the surface, calls `sampleMaterial` and writes every guide. RTXDI, the caches and DLSS-RR all read those guides.
3. **Shaded rays use one SER RT pipeline shape; visibility rays use ray queries.** No any-hit anywhere.
4. **One decode for pixels and hits.** A vis-buffer pixel and a ray hit both resolve to `(nodeInstance, cluster, triangle)` and build the same `Surface`. `sampleMaterial` serves both.
5. **Alpha on rays is OMM 2-state.** Alpha-tested instances force 2-state micromaps and every other instance is `FORCE_OPAQUE`, so traversal never yields a candidate.

---

## 1. Frame at a glance

Spec §5 is the authority. Task ids in brackets.

```
load (once per asset)
  clean-up, normals/tangents (meshopt)                                                   [A4]
  clusters (clusterlod level 0) → optimize level 1 → OMM bake per cluster                [D1]
  micromaps → CLAS per cluster → cluster BLAS per primitive                              [D2 C1]

every frame                                     GRAPHICS                     ASYNC COMPUTE
  2-phase cull, vis-buffer raster, Hi-Z         (done, M1)                 │ instances + TLAS   [C2]
  resolve (CS)          vis buffer → Surface → sampleMaterial → guides     │
                        (done, M1; per-bin shaders in M7)          [E1]    │
  ─────────────── timeline wait: TLAS ready ───────────────────────────────┘
  rtxdi_initial (CS+RQ) local + infinite + BRDF candidates → reservoirs    [F3]
  rtxdi_shade (CS+RQ)   fused spatiotemporal → final visibility → direct   [F3]
  cache update          SHaRC: training paths → resolve | NRC: update paths [G3 G4]
  indirect (RT, SER)    1 ray/px → X₂ shading → cache query → GI samples   [G2]
  NRC only              QueryAndTrain → custom resolve into GI samples     [G4]
  restir_gi (CS+RQ)     temporal + spatial → indirect                      [G5]
  combine (CS)          direct + indirect + emissive → noisy HDR           [G6]
  DLSS-RR (host NGX)    noisy HDR + guides → display-res HDR               [H1]
  post (CS) + present   exposure → tonemap → display image → swapchain, ImGui [H2]
```

Every ray pass reads the TLAS address from `GpuFrameRoot.tlas`.

---

## 2. Locked decisions

| # | Decision |
|---|---|
| 0 | **Hardware:** NVIDIA RTX 20+ now, no vendor checks or fallbacks. AMD is a later target (spec §1.2): take a vendor-neutral choice where it costs nothing (SHaRC, RTXDI, KHR/EXT extensions) and keep NVIDIA-only pieces (NRC, NGX, `VK_NV_cluster_acceleration_structure`, cooperative vectors) behind their own modules (`vk_nrc`, `vk_dlss`, `vk_mega_geometry`) |
| 1 | **Lighting stack:** RTXDI ReSTIR DI (direct), one SER indirect bounce + SHaRC or NRC + RTXDI ReSTIR GI (indirect), OMM (alpha on rays), DLSS-RR (denoise + upscale). All required. Tiers change resolution, ray counts and the cache, never the algorithm (spec P1) |
| 2 | **Primary visibility:** visibility buffer (`R32_UINT`, `visibleRow << 7 \| triangle`) + reverse-Z `D32_SFLOAT`, render resolution, 1 sample, jittered from M3. The compute resolve writes the guides (spec §8.3, §9) |
| 3 | **Rays:** passes that **shade** hits use an RT pipeline built around SER: `HitObject::TraceRay` → `ReorderThread(hit, materialBin)` → minimal closest-hit returning ids, barycentrics and `hitT` → shade in raygen. One raygen, one miss, one hit group per pass. **Visibility-only** rays use `RayQuery`. No any-hit. Dispatch with `vkCmdTraceRaysIndirect2KHR` (DGC has no RT stages on driver 621.01) |
| 4 | **RT geometry:** one CLAS per cluster (the raster meshlets, same triangle order), one cluster BLAS per mesh primitive built once at load and shared by every instance of the model. Instances and the TLAS are written and built on the GPU every frame, on async compute. No KHR triangle BLAS |
| 5 | **IDs on a hit:** `InstanceIndex()` → `GpuRayInstance { nodeInstanceIndex, primitiveRow }`, written per frame next to the TLAS instances. `ClusterID` → the cluster's model-relative `meshletRow` (set at CLAS build). `PrimitiveIndex()` → triangle in the cluster. Same triple as the vis buffer's `GpuVisibleMeshlet` + triangle. Nothing persistent stores an entity, node-instance or primitive row |
| 6 | **Alpha:** raster tests alpha only in the `AlphaTested` bin's fragment shader (`discard`), never in the resolve. Rays use OMM per cluster with `FORCE_OPACITY_MICROMAP_2_STATE` on alpha-tested instances; every other instance is `FORCE_OPAQUE` |
| 7 | **Bind model:** descriptor heaps for images and samplers, BDA typed pointers for engine buffers, render-target indices in the per-frame `GpuTargets` block (`GpuFrameRoot::targets`). SDK-declared resources get heap slots in their bridge files |
| 8 | **Material table:** `MaterialStore` on `Scene`, deduplicated and refcounted across models. `GpuPrimitive::materialId` is an absolute row. No pass owns a second table |
| 9 | **Default material:** metallic 1, roughness 1, white. It renders dark without GI, which is correct |
| 10 | **AO:** sampled in the resolve and stored with the guides. Applied only to the radiance-cache query term at the primary vertex. Never on direct light |
| 11 | **GI cache:** SHaRC and NRC behind one Slang interface `IRadianceCache` (`update`, `query`). SHaRC first (M4, floor tier), NRC second (M5, reference tier) |
| 12 | **RTXDI light sampling:** `Uniform` for punctual-only scenes (M3). `Power_RIS` (presample pass + PDF texture) once emissive triangles are lights (M7). Resampling: `FusedSpatiotemporal` |
| 13 | **Scene data per frame:** the CPU writes `GpuFrameRoot`, the camera and changed transforms. Ray instances, the TLAS and the RTXDI light buffer are written on the GPU from the persistent tables (spec P2) |
| 14 | **C++ style** (`cpp-guidelines` skill, `.clang-tidy`): PascalCase types and `enum class` enumerators; camelCase functions, members, locals; class constants `kPascalCase`; getters `getX` / `isX` / `hasX`; no `m_` or trailing `_`. RAII owners of AS, micromaps, RT pipelines and SDK objects are move-only (Rule of Five); everything else follows the Rule of Zero. `const`/`constexpr` by default, `explicit` single-argument constructors, `[[nodiscard]]` on getters, named constants instead of magic numbers. Failures throw `std::runtime_error` with context. `.hpp` + `#pragma once`, no `using namespace` in headers |
| 15 | **Comments** (`main-guidelines` skill), in C++ and Slang: lowercase, 2–8 words, why or non-obvious behavior only, no trailing period |
| 16 | **Geometry processing:** meshoptimizer v1.3 for missing tangents (`meshopt_generateTangents`, `meshopt_TangentCompatible`), missing normals, clusters (`clusterlod.h` level 0) and OMM baking (`meshopt_opacityMap*`). No homegrown version of any of these |
| 17 | **Static geometry only** (spec §7.3): no skinning or morph targets. The animation code is removed in M0, before A4 |
| 18 | **Texture LOD:** the resolve uses `SampleGrad` with analytic barycentric gradients; ray hits use ray-cone gradients through the same function. LOD bias `log2(render / display)` from M3. Implicit-LOD `Sample` only in the alpha-tested fragment shader |
| 19 | **Material model** (spec §9.4): glTF metallic-roughness, `KHR_texture_transform`, `KHR_materials_emissive_strength`. Further KHR PBR extensions become separate material bins (M7+) |

---

## 3. Current state (verified 2026-11-07)

Resource details: [resource_architecture.md](./resource_architecture.md). Vis-buffer details: [visibility_buffer.md §10](./features/visibility_buffer.md#10-as-built-2026-11-07).

| Piece | State |
|---|---|
| glTF parse → `MaterialStore` / `LightStore` / `GeometryStore`, shared refcounted models and materials, mesh/node split, `GpuNodeInstance` | done |
| Frame root | `GpuFrameRoot` 136 B: camera, instances, models, primitives, materials, pbrExtensions, lights, `tlas` (0), nodeInstances, drawRecords, visibleMeshlets, 4 counts, `lateVisibleBase`, `debugView`, `cullCounters`, `triangleCull`, `subpixelSnap`, `targets`. Missing for this plan: `rayInstances`, `frameIndex`, `exposure`, SDK constants blocks |
| Render targets | done: `RenderTargets` ([vk_render_targets.cpp](../src/render/vk_render_targets.cpp)); `GpuTargets` 144 B per frame slot: vis buffer, depth, display (RGBA16F), linear sampler, render size, `current` / `previous` guide sets by frame parity |
| Culling + raster | done: two-phase GPU culling, reverse Z, 3-level Hi-Z, `RasterBin {Opaque, AlphaTested}` with `DrawRecordFlag::TwoSided` and winding flip. Both bins share one fragment shader and DGC state until M6 |
| Resolve | done: one tiled compute dispatch (8×8). Olano–Greer homogeneous barycentrics evaluated at (+1, 0) and (0, +1), `SampleGrad`, motion from current and previous transforms. `debug_view` writes the display image; `present` draws it into the sRGB swapchain with ImGui in the same rendering |
| Guides | done, two sets: `NormalRoughness` RGBA16F, `GeoNormal` RG16_SNORM (oct), `DiffuseAlbedo` RGBA8 (`cDiff`, **a = 1: AO not stored yet**), `SpecularF0` RGBA8, `SpecAlbedo` RGBA8 (`envBrdfApprox`), `Emissive` B10G11R11, `Motion` RG16F |
| `geometry/surface.slang`, `geometry/visbuffer.slang` | done: `TriangleCorners`, `loadTriangleCorners`, `Surface`, `buildSurface` (geometric-normal fallback, tangent sign flip under `FlipWinding`), payload pack/unpack, `computeBarycentrics` |
| `material/pbr.slang` | **B1 done**: `SampledMaterial`, `sampleMaterial(GpuMaterial, Surface, lodBias)`, `sampleSlot` with `NonUniformResourceIndex`, unlit handling. No `KHR_texture_transform`; `kMaterialFlagUnlit` is declared locally, not in `types.slang` |
| `material/brdf.slang` | `envBrdfApprox`, `kDielectricF0`. `evalLambert` stub still present. No GGX, sampling or pdf |
| Debug views | Guides category done (depth, shading/geometric normal, roughness, albedo, F0, spec albedo, emissive, motion). `LightingOnly` and `DlssInput` reserved, not implemented |
| Vertex streams | `GpuVertex` 32 B (pos, color, uv0); normals, tangents, uv1 as separate float32 streams. **No generation (A4)**: the resolve falls back to the geometric normal and skips normal mapping without tangents |
| Animation code | removed (2026-11-08): no morph or skin data is loaded or uploaded; glTF files load in their base pose |
| Lights | `GpuLight` 64 B per entity in model space, `type` a raw `uint` (`GpuLightType` **missing**); placed by `loadWorldLight` |
| Clusters | `clusterlod.h` level 0 (Flex builder, cone weight 0, `optimizeMeshletLevel` 1), groups via `partitionClusters` |
| Device | AS, ray query, RT pipeline, SER, OMM enabled. Cluster AS optional (`hasClusterAccelerationStructure`). OMM properties queried but **not cached**. Position fetch and cooperative vectors **not enabled**. Fragment barycentrics removed |
| `src/render/vk_pbr.{hpp,cpp}` | empty files |
| Slang modules | not used: every file is `#include` + `#ifndef` guards |
| `ThirdParty/` | meshoptimizer v1.3, `rtxdi`, `sharc`, `nrc`, `dlss` fetched; linkage tests in `tests/`. The `omm` fetch is unused (D1 bakes with meshoptimizer) |
| Shader stubs | kept, back in scope (spec §17): `gi/sharc`, `gi/rtxgi`, `raytracing/{hit,payload,trace}`, `passes/pathtrace*`, `passes/sharc_resolve`, `passes/composite`, `postprocessing/dlss`. Obsolete: `postprocessing/gbuffer.slang` (the guide layout is `GuideImage` in `types.slang`) |

Toolchain facts (from the 2026-10-25 probe on Slang 2026.14.1; **re-probe on v2026.20**, task A2):

| Fact | Consequence |
|---|---|
| `RaytracingAccelerationStructure(uint64_t)` → `OpConvertUToAccelerationStructureKHR` | the TLAS address goes straight into `GpuFrameRoot.tlas` with no AS descriptor |
| `module` / `import a.b;`, typed pointers in structs, `enum X : uint`, `RWTexture2D<float4>.Handle` in push data all validate | adopt them (§4) |
| heap access never emitted `NonUniform`, even with `NonUniformResourceIndex` | §4.4 |
| `[format(...)]` is accepted on globals only, not on heap handles | storage guides use write-without-format, checked at startup (`RenderTargets::checkFormatSupport`) |
| `slangc -depfile` exists | replace hand-written `DEPENDS` lists (§4.5) |

---

## 4. Slang conventions

### 4.1 Modules

Library files become Slang modules (`module brdf;`, exports `public`). Passes `import` them. Vendor HLSL headers (RTXDI, SHaRC, NRC) are `#include`d **only** from pass files and their bridge file, because they depend on textual order (the bridge defines `RAB_*` before the RTXDI headers).

| Module (path) | Owns | State |
|---|---|---|
| `types` | POD mirrors of `src/core/types.hpp`, flags, enums, guide ids | edit |
| `core.binding` | heap helpers (`tex2D`, `samplerOf`, `storageTex2D4`, `uintTex2D`), `bdaPtr` | exists |
| `core.frame` | `loadDrawContext`, `loadCamera`, `loadMaterial`, `loadTargets` | exists |
| `core.math` | `kPi`, ONB, `octEncode`/`octDecode`, `jitterClip`, `pixelToNdc`, `normalMatrix`, PCG RNG | edit |
| `geometry.visbuffer` | payload pack/unpack, `computeBarycentrics`, interpolation | exists |
| `geometry.surface` | `TriangleCorners`, `Surface`, `buildSurface`; shared by the resolve and hits | exists |
| `geometry.hit` | hit ids + barycentrics → `TriangleCorners` (position fetch) → `Surface` with ray-cone gradients | **new** (C3) |
| `material.pbr` | `SampledMaterial`, `sampleMaterial`, `sampleSlot` | exists |
| `material.brdf` | `evalBrdf`, `sampleBrdf`, `pdfBrdf`, `fromMaterial`, `envBrdfApprox` | edit |
| `lighting.lights` | `LightSample`, `evalLight` | **new** |
| `raytracing.*` | payload, minimal closest-hit, `traceVisibility` | stubs → edit |
| `gi.radiance_cache` | `IRadianceCache` | **new** (replaces `gi/rtxgi.slang`) |
| `lighting/rtxdi_bridge.slang`, `gi/sharc_bridge.slang`, `gi/nrc_bridge.slang` | SDK resource macros + callbacks (textual include) | **new** |
| `passes/*.slang` | entry points only | edit / new |

### 4.2 Types

```slang
module types;

public static const uint kNoneIndex = 0xffffffffu;

public enum AlphaMode : uint { Opaque = 0, Mask = 1, Blend = 2 }
public enum LightType : uint { Directional = 0, Point = 1, Spot = 2 }

// mirrors GpuMaterialFlag in src/core/types.hpp
public static const uint kMaterialAlphaModeMask = 0x3u;
public static const uint kMaterialFlagDoubleSided = 1u << 2;   // exists
public static const uint kMaterialFlagUnlit       = 1u << 3;   // moves here from material/pbr.slang

// [InstanceIndex()]; rewritten on the gpu with the tlas
public struct GpuRayInstance
{
    public uint nodeInstanceIndex; // row in GpuFrameRoot.nodeInstances
    public uint primitiveRow;      // row in GpuFrameRoot.primitives
}

// appended to the existing GpuFrameRoot (136 B today), after targets
public struct GpuFrameRoot
{
    // ... existing fields through `targets` ...
    public uint64_t rayInstances;   // GpuRayInstance[], 0 until C2
    public uint64_t rtxdiConstants; // F1, 0 until M3
    public uint64_t cacheConstants; // SHaRC or NRC constants, 0 until M4
    public uint     frameIndex;
    public float    exposure;       // H2 pre-exposure, fed to DLSS-RR
}
```

Update the C++ `static_assert`s on size and every new offset. Jitter stays in `GpuCameraData::jitterOffset` (applied by `jitterClip`). Raster keeps `MeshPushData` (16 B); compute passes keep `FramePushData` (8 B, root address). Lighting debug modes become `DebugView` entries, not a separate bit field.

The model of a hit is `models[nodeInstances[ray.nodeInstanceIndex].modelIndex]`; its world matrix is `nodeInstances[…].worldMatrix`. Values inside a `GpuPrimitive` are relative to that model's slices; `materialId` is an absolute row of the shared table (`loadMaterial` already works this way).

C++ mirrors use `vk::DeviceAddress` for every pointer, with `static_assert(sizeof)` and `static_assert(offsetof…)` per field.

### 4.3 Rules of thumb

| Rule | Why |
|---|---|
| Read `ptr[i]` into a local struct once | one load, readable code |
| `SampleGrad` everywhere outside fragment shaders: analytic gradients in the resolve, ray-cone gradients at hits | no derivatives in compute or raygen; one material path (§2 #18) |
| `enum` + `switch` for light types | data-dependent, interfaces would diverge |
| Interfaces + generics for the cache (`IRadianceCache`) | spec S9: no preprocessor variants except vendor-mandated defines (`NRC_UPDATE`, `NRC_QUERY`) |
| `static const` for tunables | no magic numbers in bodies |
| Entry points in `passes/`, logic in modules | each pass fits one screen |

### 4.4 Non-uniform heap indices

Material texture indices diverge across a wave in the resolve and at hits. Every material fetch goes through `sampleSlot` (`material/pbr.slang`, the spec's `sampleOr`), which wraps the index in `NonUniformResourceIndex`. Slang 2026.14.1 dropped the resulting `NonUniform` decoration on heap access; NVIDIA drivers tolerate it, but it is UB by spec.

Re-probe on every Slang upgrade, starting with v2026.20: compile `resolve.slang`, `spirv-dis`, grep `NonUniform`. If it is still missing and artifacts appear, add a `WaveReadLaneFirst` waterfall inside `sampleSlot` only.

### 4.5 CMake

- `engine_add_slang_pass` passes `-depfile ${_spv}.d` and `DEPFILE ${_spv}.d`. Drop `DEPENDS`.
- Vendor-mandated defines go in `DEFINES` (`NRC_UPDATE=1`, `NRC_QUERY=1`). One source may produce several `.spv`.
- RT pipeline stages (`raygeneration`, `miss`, `closesthit`) compile to their own `.spv` per entry point.
- Add `-warnings-as-errors all` once the tree compiles clean.

---

## 5. Tasks

Each task lists **Do**, **Done when**, and **Must not** where relevant, plus its milestone. Dependencies are in §6.

### Phase A — Contracts (M0)

#### A1 · POD layouts and constants

- **Files:** `src/core/types.hpp`, `src/render/push_data.hpp`, `shaders/types.slang`, `src/core/vk_device.cpp`
- **Do:**
  - Slang `AlphaMode`, `LightType` and material flag constants mirroring `GpuMaterialFlag` (§4.2); move `kMaterialFlagUnlit` out of `material/pbr.slang`.
  - `enum class GpuLightType : uint32_t { Directional, Point, Spot }` (values used by `lightTypeFromName`); `GpuLight.type` becomes `GpuLightType`. Keep 64 B.
  - `GpuRayInstance` (8 B) and the `GpuFrameRoot` fields of §4.2 plus `frameFlags` (`kFrameFlagHistoryReset`, [motion_vectors.md](./features/motion_vectors.md) §4), with size and offset asserts.
  - Cache `vk::PhysicalDeviceOpacityMicromapPropertiesEXT` in `HardwareCapabilities::opacityMicromap`, and `vk::PhysicalDeviceRayTracingInvocationReorderPropertiesEXT` with its hint logged at startup ([shader_execution_reordering.md](./features/shader_execution_reordering.md) SER1).
  - **Core-material data lives in `GpuMaterial`** (decided 2026-11-08). The resolve and every hit already load one `GpuMaterial` row; a second table would add a pointer load and a second row of cache lines per pixel, and `GpuMaterialExtension` (724 B per row) is mostly data no shader reads yet. So:
    - `KHR_materials_emissive_strength` is folded into `emissiveFactor` on the CPU when the row is packed: no field, no shader math;
    - `KHR_texture_transform` is precomputed on the CPU into one 2×3 float matrix per slot (`M = T · R · S`; 24 B × 5 slots), so the shader does no `sin`/`cos`;
    - flag bits 4–8 (`kMaterialFlagUvTransformBaseColor` … `Emissive`) mark slots with a non-identity transform; the shader reads a slot's matrix only when its bit is set, so identity materials (almost all) load no extra bytes. `texCoord` overrides are already resolved into the slot's UV index.

    `GpuMaterial` grows from 112 to 240 B; update the asserts on both sides. `GpuMaterialExtension` keeps the other KHR extensions for their future material bins.
- **Done when:** changing any field or pad breaks the build.
- **Must not:** add a Slang mirror of `GpuMaterialExtension` or read it per pixel before a KHR extension bin needs it.

#### A2 · Slang modules + frame root fields

- **Files:** all `shaders/**`, `CMakeLists.txt`, `src/core/vk_resource_manager.cpp`
- **Do:** convert library files to modules (§4.1). `ResourceManager::writeFrameRoot` also fills `frameIndex` and `exposure` (1.0 until H2); the new addresses stay 0 until their tasks. Re-probe the §3 toolchain facts on Slang v2026.20 and update the table.
- **Done when:** `mesh_raster`, `mesh_fragment`, `meshlet_cull`, `hiz_build`, `resolve`, `debug_view` and `present` compile and render unchanged; every guide debug view matches a capture taken before the conversion.

#### A3 · Remove what the plan no longer uses

- **Files:** `shaders/**`, `ThirdParty/CMakeLists.txt`
- **Do:** delete `postprocessing/gbuffer.slang`, `evalLambert`, and the `omm` fetch. Keep the RT pipeline, SER and cluster AS extensions and the stubs listed in §3 (spec §17 reverses the old A3).
- **Done when:** the tree builds and renders unchanged.

#### A4 · Shading vertex attributes (meshoptimizer)

- **Files:** `src/core/assets_loader.cpp`, `src/core/geometry_store.{hpp,cpp}`
- **Precondition:** animation code removed (spec §7.3, M0), so no morph deltas need remapping.
- **Why:** glTF says a primitive without `TANGENT` should get MikkTSpace tangents, and one without `NORMAL` gets flat normals. Today the loader generates neither; the resolve compensates with fallbacks (geometric normal, no normal map). The resolve, C3, RTXDI and the caches all read these streams.
- **Do,** per primitive, after the accessors are read and before clusters are built (clusters, CLAS and OMM must see the final vertex set):
  - **Normals missing:** flat normals. Deindex the primitive, then `meshopt_generateNormals(…, crease_angle = 0, smoothing = 0)` per corner (experimental in v1.3; pin the call behind one helper).
  - **Tangents missing, normal texture used by the primitive's material:** `meshopt_generateTangents(result, indices, …, positions, normals, uv, meshopt_TangentCompatible)` per corner, on the UV set named by `normalTexture.texCoord`. `meshopt_TangentCompatible` matches MikkTSpace, which baked normal maps assume.
  - **UV orientation:** the loader stores `v' = 1 - v` (`fillUvRange(…, flipV = true)`), which mirrors UV space and inverts tangent handedness. Feed `meshopt_generateTangents` the file's UVs (undo the flip in the scratch copy) so `w` matches MikkTSpace on the original asset, as file-provided tangents do. Confirm the flip on `NormalTangentTest` / `TextureCoordinateTest` with the shading-normal debug view.
  - **Reindex:** both functions return per-corner data. Weld with `meshopt_generateVertexRemapMulti` over every vertex stream plus the new per-corner stream, then `meshopt_remapVertexBuffer` / `meshopt_remapIndexBuffer`. Vertices split only on seams (UV mirror, hard edges).
  - Set `GpuModelAttribute::Normals` / `Tangents` so the streams upload.
- **Done when:** `NormalTangentTest` (provided tangents) and `NormalTangentMirrorTest` with `TANGENT` stripped show the same shading-normal debug view within the generator's tolerance; a mesh without `NORMAL` shows flat facets; the resolve's fallbacks remain only as guards; the cache-hit path (`instantiateModel`) is untouched.
- **Must not:** generate tangents for primitives without a normal texture, overwrite file-provided tangents or normals, or write a homegrown generator.

### Phase B — Shading library

#### B1 · `sampleMaterial` — done (M1), extensions open

- **Files:** `shaders/material/pbr.slang`, `shaders/geometry/surface.slang`
- **As built:**

```slang
struct SampledMaterial
{
    float3 baseColor; // linear, × COLOR_0
    float  opacity;
    float  metallic;
    float  roughness; // perceptual; α = roughness² lives in brdf
    float3 emissive;
    float  ao;
    float3 n;         // world, normal-mapped, facing-corrected
}

SampledMaterial sampleMaterial(GpuMaterial material, Surface surface, float lodBias);
```

| Slot | Space | Channels | Missing |
|---|---|---|---|
| baseColor | sRGB view (HW decode) | rgb × factor × `COLOR_0`, a = coverage | factor |
| metallicRoughness | linear | **B** metal, **G** rough | factors |
| normal | linear | rgb, OpenGL +Y, `scale` on xy | vertex N |
| occlusion | linear | **R**, `lerp(1, r, strength)` | `ao = 1` |
| emissive | sRGB view | rgb × factor | factor |

  - `Surface` carries per-UV-set `uv`, `uvDdx`, `uvDdy`; `textureGradients` scales them by `exp2(lodBias)`. The resolve passes analytic gradients; hits pass ray-cone gradients (C3). One function serves both.
  - Normals and tangents are transformed by the node instance's normal matrix; the bitangent sign flips under `FlipWinding`; back faces of two-sided materials flip the normal in `buildSurface`.
  - **Unlit:** `baseColor = 0`, `metallic = 0`, the color goes into `emissive`. Every consumer then handles unlit with no special case.
- **Open (M3, with E1):**
  - `KHR_texture_transform` per slot from the A1 `GpuMaterial` matrices, applied in `textureGradients` only when the slot's flag bit is set: `uv' = M·(uv, 1)`, and the matrix's 2×2 part on `ddx`/`ddy`.
  - `KHR_materials_emissive_strength`: nothing in the shader; A1 folds it into `emissiveFactor`.
  - `lodBias = log2(render / display)` once DLSS-RR sets the render resolution.
- **Done when:** the roughness and F0 debug views on `MetalRoughSpheres` match the texture layout; `TextureTransformTest` matches the Khronos screenshot in kind.
- **Must not:** sRGB-sample metal-rough/normal/occlusion, use Unity ORM on the metal-rough slot, flip normal Y, or sample with implicit LOD outside a fragment shader.

#### B2 · BRDF (M3)

- **Files:** `shaders/material/brdf.slang` (keeps `envBrdfApprox` and `kDielectricF0`; `evalLambert` goes in A3)

```slang
public float3 fresnelSchlick(float3 f0, float vDotH)
{
    return f0 + (1.0 - f0) * pow(1.0 - vDotH, 5.0);
}

public float ggxD(float nDotH, float a2)
{
    float d = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

// height-correlated smith; already contains 1 / (4 NdotL NdotV)
public float smithVisibility(float nDotL, float nDotV, float a2)
{
    float ggxV = nDotL * sqrt(nDotV * nDotV * (1.0 - a2) + a2);
    float ggxL = nDotV * sqrt(nDotL * nDotL * (1.0 - a2) + a2);
    return 0.5 / max(ggxV + ggxL, 1e-7);
}

// glTF appendix B; caller multiplies by radiance * NdotL once
public float3 evalBrdf(float3 n, float3 cDiff, float3 f0, float roughness, float3 v, float3 l)
{
    float nDotL = dot(n, l);
    if (nDotL <= 0.0)
        return 0.0;

    float3 h     = normalize(l + v);
    float  nDotV = max(dot(n, v), 1e-4);
    float  nDotH = saturate(dot(n, h));
    float  vDotH = saturate(dot(v, h));
    float  alpha = max(roughness * roughness, 1e-4);
    float  a2    = alpha * alpha;

    float3 f = fresnelSchlick(f0, vDotH);
    return (1.0 - f) * cDiff / kPi + f * ggxD(nDotH, a2) * smithVisibility(nDotL, nDotV, a2);
}
```

The signature takes `cDiff`/`f0` rather than `SampledMaterial`, so RTXDI and the caches call it straight from guide data (`DiffuseAlbedo`, `SpecularF0`, `NormalRoughness`). A `BrdfInputs fromMaterial(SampledMaterial)` helper computes `cDiff = baseColor·(1−metallic)`, `f0 = lerp(kDielectricF0, baseColor, metallic)`; the resolve switches its inline computation to it, so pixels and hits share one definition.

Also add `sampleBrdf(…, u) -> BrdfSample { l, weight, pdf }` (lobe pick by Fresnel-weighted luminance, cosine diffuse, GGX **VNDF** specular) and `pdfBrdf(…, l)`. RTXDI (`RAB_SurfaceImportanceSampleBrdf`, `RAB_SurfaceEvaluateBrdfPdf`), the indirect pass and ReSTIR GI use them.

- **Done when:** [pbr_lighting_materials.html §7](./pbr_lighting_materials.html#combined) numbers reproduce (white plastic r=0.5 → 0.295 head-on).
- **Must not:** feed perceptual roughness into `D`, drop `NdotL` after the correlated `V`, or multiply by it twice.

#### B3 · `evalLight` (M3)

- **Files:** `shaders/lighting/lights.slang` (new)

```slang
public struct LightSample
{
    public float3 l;          // unit, toward the light
    public float3 radiance;   // L_i before NdotL
    public float  distance;   // shadow ray tMax; kInfinity for directional
}

public LightSample evalLight(GpuLight light, float3 p)
{
    LightSample s;
    if (light.type == LightType.Directional)
    {
        s.l = -light.worldDir;
        s.radiance = light.color * light.intensity;   // lux
        s.distance = kInfinity;
        return s;
    }

    float3 toLight = light.worldPos - p;
    float  d2 = max(dot(toLight, toLight), 1e-8);
    s.distance = sqrt(d2);
    s.l = toLight / s.distance;

    float attenuation = rangeWindow(s.distance, light.range) / max(d2, 1e-4);
    if (light.type == LightType.Spot)
        attenuation *= spotCone(dot(light.worldDir, -s.l), light.innerCone, light.outerCone);

    s.radiance = light.color * light.intensity * attenuation;   // candela
    return s;
}

// khronos window; 0 at d >= range; range 0 = infinite
float rangeWindow(float d, float range)
{
    if (range <= 0.0)
        return 1.0;
    float r = d / range;
    return saturate(1.0 - r * r * r * r);
}

float spotCone(float cosToAxis, float inner, float outer)
{
    float scale  = 1.0 / max(0.001, cos(inner) - cos(outer));
    float offset = -cos(outer) * scale;
    float t = saturate(cosToAxis * scale + offset);
    return t * t;
}
```

This is the only punctual light evaluation. `RAB_SamplePolymorphicLight` wraps it; emissive triangles (M7) are sampled by RTXDI's own triangle-light path, not here.

- **Done when:** an on-axis spot debug gives `cosToAxis = +1`.
- **Must not:** use `dot(spotDir, L)` or scale intensity, range, or cone by node scale.

### Phase C — Ray-tracing scene (M2)

Spec §10. Reference: `vk_lod_clusters` (`renderer_raytrace_clusters_lod.cpp`, `instances_assign_blas.comp.glsl`).

#### C1 · CLAS per cluster + cluster BLAS per primitive

- **Files:** `src/render/vk_mega_geometry.{hpp,cpp}` (new), `src/core/vk_device.cpp`, `src/core/vk_resource_manager.cpp`
- **Do:**
  - `VK_NV_cluster_acceleration_structure` becomes **required**: `hasClusterAccelerationStructure == false` throws at device creation. Enable `VK_KHR_ray_tracing_position_fetch` (spec Q7: check `vulkaninfo` on the 2060 Super first).
  - One CLAS per cluster of the model just flushed: positions gathered into scratch as cluster-local arrays with 8-bit cluster-local indices, freed after the build. `clusterID = meshletRow` (model-relative), triangle order = the raster cluster's triangle order, so `PrimitiveIndex()` equals the vis-buffer triangle. `positionTruncateBitCount = 0` until measured.
  - Build into scratch, then move to the final allocation with the real sizes (host estimates are 3–5× too large, [clas_allocation.md](../reference/vk_lod_clusters/docs/clas_allocation.md)).
  - One cluster BLAS per **mesh** primitive from its CLAS (not per node: nodes are TLAS instances). Built right after `flushGpuAssets`, while the slice addresses are current. A cache hit (`instantiateModel`) builds nothing.
  - CLAS and BLAS are grouped per model, indexed like `modelAllocations`, destroyed when the model's `refCount` reaches 0. Other models stay valid: a built AS does not read its inputs.
  - Sizes, counts and alignments come from `VkPhysicalDeviceClusterAccelerationStructurePropertiesNV` and the AS properties, never constants.
- **Done when:** one BLAS address per mesh primitive; validation clean; loading two models then unloading the first keeps the second traceable; a second Sponza adds no AS memory.
- **Must not:** build KHR triangle BLASes, build from a CPU-side ray mesh, or reorder cluster triangles between raster and CLAS.

#### C2 · GPU instances + TLAS (async compute)

- **Files:** `src/render/vk_mega_geometry.cpp`, `shaders/passes/rt_instances.slang` (new)
- **Do:**
  - A compute pass writes one `VkAccelerationStructureInstanceKHR` and the matching `GpuRayInstance` per (node instance, mesh primitive) from `GpuNodeInstance` and the per-model BLAS address table. `transform` = 3×4 of the node instance's world matrix; `mask = 0xff`; flags `FORCE_OPAQUE`, or `FORCE_OPACITY_MICROMAP_2_STATE_EXT` for alpha-tested primitives (from the material's alpha mode). No facing flags. Instance capacity, counter and overflow counter in the debug UI (spec S11).
  - Full TLAS build every frame (`PREFER_FAST_TRACE`) on the async compute queue; graphics waits on a timeline semaphore before the first ray pass. Per-slot TLAS storage, scratch, instance and `GpuRayInstance` buffers. `GpuFrameRoot.tlas` and `GpuFrameRoot.rayInstances` point at them.
  - PTLAS only if the measured TLAS build exceeds 0.3 ms (spec §10.3).
- **Done when:** a yawing entity moves without BLAS rebuilds; Tracy/Nsight show the TLAS build overlapping the raster; no CPU loop over instances.
- **Must not:** write instances on the CPU, or cull TLAS instances by the camera.

#### C3 · `geometry.hit`

```slang
// committed hit → the same Surface the resolve builds
public Surface decodeHit(GpuFrameRoot f, uint rayInstance, uint meshletRow, uint triangle,
                         float2 bary, float3 hitPositions[3], RayCone cone);
```

`rayInstances[rayInstance]` → node-instance row → `GpuModel` → cluster `meshletRow` → 3 cluster-local vertex ids → uv0/uv1/normal/tangent/color from the model slices (absent stream: mask bit clear → default; after A4 tangents exist wherever a normal texture does) → `buildSurface`. Positions come from `HitTriangleVertexPosition` (position fetch), the truncated CLAS positions: build the geometric normal and the next ray origin from them only (spec §10.2). UV gradients come from the ray cone at `hitT`. The material is `loadMaterial(primitiveRow)`.

- **Debug pass:** a ray-query CS traces primary rays and colors by `meshletRow` and by `rayInstances[…].nodeInstanceIndex`, using the same palette as the vis-buffer `Meshlets` / `NodeInstances` debug views.
- **Done when:** the ray-traced debug view matches the vis-buffer debug view pixel for pixel away from silhouettes; a decoded primary hit gives the same shading normal as the resolve.

### Phase D — Alpha: raster bin + opacity micromaps (M6)

Spec §8.2, §10.5.

#### D1 · Bake per cluster

- **Files:** `src/core/omm_baker.{hpp,cpp}` (new)
- **Baker:** meshoptimizer v1.3 `meshopt_opacityMapMeasure` → `meshopt_opacityMapRasterize` → `meshopt_opacityMapCompact`. Output is the `VK_EXT_opacity_micromap` layout directly (bird order, 1 or 2 bits per micro-triangle, special indices `-1..-4`).
- **Do:**
  - Qualify: `AlphaMode::Mask` (cutoff = `alphaCutoff`) and `AlphaMode::Blend` (cutoff 0.5) with a base-color alpha texture. Anything else is opaque for rays.
  - Bake from the **final** cluster triangles, after `meshopt_optimizeMeshletLevel` 1, which may rotate corners (spec §7.1 step 4–5). Micromap data is corner-sensitive, so never bake from an earlier order or from the index buffer.
  - Per cluster, an OMM index buffer that maps the cluster-local triangle to its bake entry. Cached with the model's geometry (spec §7.2).
  - The loader keeps the base-color alpha (UNORM8, mip 0) of qualifying textures until the bake; decoded pixels are freed after it.
  - `maxLevel` from §9 clamped to `opacityMicromap.maxOpacity4StateSubdivisionLevel`; `targetEdge > 0` makes the level adaptive to texel area.
  - `meshopt_opacityMapRasterize(…, states = 4, …)`: the instance flag collapses unknown states to 2-state at trace time.
  - **Cutoff:** meshopt rasterizes at a fixed 0.5. For `Mask` with another `alphaCutoff`, give the baker a remapped alpha copy: `[0, cutoff] → [0, 0.5]`, `[cutoff, 1] → [0.5, 1]`, so the threshold lands where raster `discard` puts it.
  - UVs and texture in the orientation the shader samples, or the micromap is mirrored against `discard`.
- **Done when:** the cache round-trips, and baker stats show > 70 % known micro-triangles on Sponza foliage.

#### D2 · Build and attach to CLAS

- **Do:**
  - Micromap buffers with `eMicromapBuildInputReadOnlyEXT` / `eMicromapStorageEXT`; one `VkMicromapTriangleEXT { dataOffset, subdivisionLevel, format = OPACITY_4_STATE }` per bake entry; `vkGetMicromapBuildSizesEXT` → `vkCmdBuildMicromapsEXT`, RAII-owned, built before C1 in the same command buffer.
  - CLAS build info: `opacityMicromapArray`, `opacityMicromapIndexBuffer` (the per-cluster map of D1), `opacityMicromapIndexType`; the build sets `VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_CLUSTER_OPACITY_MICROMAPS_BIT_NV` (VUID-…-opacityMicromapArray-10881).
- **Done when:** validation is clean, and shadow rays through `AlphaBlendModeTest` foliage show cut-outs.

#### D3 · Alpha-tested raster bin

- **Files:** `shaders/passes/mesh_fragment.slang` (or a new `mesh_alpha.slang`), `src/render/vk_dgc.cpp`, `src/render/vk_shader_objects.cpp`
- **Do:** the `AlphaTested` bin gets its own fragment shader: base-color alpha (implicit-LOD `Sample` is fine in a fragment shader) against `alphaCutoff` (Mask) or 0.5 (Blend) → `discard`, then the same payload write. Its own DGC state command buffer, matching the execute state exactly. Cluster/group state bits `ALPHAMASKED` / `*_MIXED` route draw records to the bin (spec §8.1 A6).
- **Done when:** `AlphaBlendModeTest` cut-outs match in raster and shadows; the opaque bin's fragment shader still fetches no material.
- **Must not:** test alpha in the resolve, or premultiply alpha.

#### D4 · Ray contract

- Visibility rays: one helper, `traceVisibility(origin, dir, tMax)` in `raytracing.trace`, a `RayQuery` with no candidate loop. Shaded rays: the SER pipeline of G1, closest-hit only.
- In debug builds, count any candidate in `Proceed()` into a debug buffer and assert zero.
- **Must not:** write candidate alpha-test loops or any-hit shaders.

### Phase E — Resolve and guides

#### E1 · Vis-buffer resolve + guides — done (M1), open items listed

- **Files:** `shaders/passes/resolve.slang`, `src/render/vk_render_targets.cpp`, `src/render/vk_renderer.cpp`, `src/scene/vk_camera.cpp`
- **As built:** one 8×8-tiled compute dispatch. Payload → `DrawContext` → `loadTriangleCorners` → `computeBarycentrics` (Olano–Greer, also at (+1, 0) and (0, +1) for gradients) → `buildSurface` → `sampleMaterial` → guides. Empty pixels (depth 0) write zero guides and the motion of a direction at infinity. Two guide sets by frame parity (`GpuTargets::current` / `previous`); RTXDI and ReSTIR temporal passes read `previous`.

  | Image | Format | Content | Read by |
  |---|---|---|---|
  | depth | D32F reverse-Z | hardware depth | RTXDI, ReSTIR GI, DLSS (`InUseHWDepth`) |
  | `NormalRoughness` | RGBA16F | world shading N, **perceptual** roughness | RTXDI, indirect, DLSS (packed roughness) |
  | `GeoNormal` | RG16_SNORM | oct geometric normal | ray origin offset |
  | `DiffuseAlbedo` | RGBA8 | `cDiff`, a = ao (**open**) | RTXDI, indirect, DLSS diffuse albedo |
  | `SpecularF0` | RGBA8 | `f0` | RTXDI, indirect |
  | `SpecAlbedo` | RGBA8 | `envBrdfApprox(f0, NdotV, roughness)` | DLSS specular albedo |
  | `Emissive` | B10G11R11 | emissive (unlit: baseColor) | combine |
  | `Motion` | RG16F | render-res pixels, current → previous, no jitter | DLSS, RTXDI / ReSTIR GI temporal |

- **Open:**
  - **AO into `DiffuseAlbedo.a`** (M3, needed by G2). DLSS reads only rgb.
  - **Jitter** (M3, spec §12.2): Halton(2,3), phase count `8 · (display / render)²`, applied through `jitterClip` in the raster and the resolve's pixel → NDC mapping; motion stays unjittered; culling uses the unjittered matrix and the exact pixel tests widen by the jitter amplitude.
  - **`lodBias`** = `log2(render / display)` (B1).
  - `DebugView::LightingOnly` and `DlssInput` become views of the lighting outputs and H1 inputs; mark them `isImplemented` as each lands.
  - **Motion vectors:** [motion_vectors.md](./features/motion_vectors.md): history bugs B1–B4, the history-reset flag, and a reprojection debug view that proves the vectors (MV1–MV5).
  - Spec §2.3 timings deferred (2026-11-08); take them on a release build with validation off (vulkan-dev skill §6.1).
  - M7 splits the resolve into per-material-bin shaders (spec §9.1–9.2); the per-pixel body stays this one.
- **Done when (remaining):** `MetalRoughSpheres`, `TwoSidedPlane` and `UnlitTest` guides match the Khronos screenshots in kind; motion is zero for a static scene and consistent under rotation.
- **Must not:** write GGX `α` as roughness, jitter without unjittered motion, or sRGB-encode guides.

#### E2 · GPU light buffer

- **Already done:** per-entity `GpuLight` rows in model space, placed by `loadWorldLight`.
- **Do (M3):** a compute pass transforms the punctual rows by their entity's world matrix into the RTXDI light buffer (F2), normalizing the direction and keeping photometric values. Lights move with their entity with no CPU loop.
- **Do (M7):** emissive-triangle lights, built on the GPU at load from primitives with a non-zero emissive factor, appended to the local-light range.
- **Done when:** spinning `LightsPunctualLamp` moves its highlight with the mesh; Bistro emissives light their surroundings (M7).

### Phase F — RTXDI ReSTIR DI (M3; emissive + `Power_RIS` in M7)

SDK: `NVIDIA-RTX/RTXDI-Library` (host C++ + HLSL headers), pinned in `ThirdParty/CMakeLists.txt`. Required.

#### F1 · Host context

- `rtxdi::ReSTIRDIContext` with `ReSTIRDIStaticParameters { renderWidth, renderHeight, CheckerboardMode::Off }`. `SetFrameIndex` each frame; recreate on resize with `RenderTargets::recreate`.
- Local light sampling `Uniform` until M7, then `Power_RIS` (presample pass + PDF texture). Resampling `FusedSpatiotemporal` (§2 #12).
- Buffers: DI reservoirs (`RTXDI_PackedDIReservoir`, sized by the context), neighbor offsets (`rtxdi::FillNeighborOffsetBuffer` once), one host-visible constants block per frame slot reached through `GpuFrameRoot.rtxdiConstants`.

#### F2 · Light buffer + bridge

- **Light buffer** (GPU-written, E2): local lights (point, spot; emissive triangles from M7) first, then infinite (directional), in stable order so `RAB_TranslateLightIndex` is the identity.
- **`lighting/rtxdi_bridge.slang`** defines the RTXDI resource macros against heap slots and implements the `RAB_*` set:

  | Group | Functions | Backed by |
  |---|---|---|
  | surface | `RAB_Surface`, `RAB_GetGBufferSurface`, `RAB_IsSurfaceValid`, `RAB_GetSurfaceWorldPos`, `RAB_GetSurfaceNormal`, `RAB_GetSurfaceLinearDepth`, `RAB_GetMaterial`, `RAB_AreMaterialsSimilar` | E1 guides + depth, current/previous set; world position reconstructed from depth and `invViewProj` |
  | BRDF | `RAB_SurfaceEvaluateBrdfPdf`, `RAB_SurfaceImportanceSampleBrdf` | `pdfBrdf`, `sampleBrdf` |
  | lights | `RAB_LightInfo`, `RAB_LoadLightInfo`, `RAB_StoreCompactLightInfo`, `RAB_SamplePolymorphicLight`, `RAB_LightSample`, `RAB_IsAnalyticLightSample`, `RAB_LightSampleSolidAnglePdf`, `RAB_EvaluateLocalLightSourcePdf`, `RAB_TranslateLightIndex` | `evalLight`; triangle lights from M7 |
  | target pdf | `RAB_GetLightSampleTargetPdfForSurface` | `luminance(evalBrdf · L_i · NdotL)` |
  | visibility | `RAB_GetConservativeVisibility`, `RAB_GetTemporalConservativeVisibility` | `traceVisibility` |

  The bridge adds no BRDF or light math of its own. Implement exactly the `RAB_*` set the pinned SDK version requires.

#### F3 · Passes

| Pass | Calls |
|---|---|
| `rtxdi_initial` CS | `RTXDI_SampleLocalLights` + `RTXDI_SampleInfiniteLights` + `RTXDI_SampleBrdf` → `RTXDI_CombineDIReservoirs` → `RTXDI_StoreDIReservoir` |
| `rtxdi_shade` CS | `RTXDI_DISpatioTemporalResampling` → final `traceVisibility` → `evalBrdf · L_i · NdotL · RTXDI_GetDIReservoirInvPdf` → `RTXDI_StoreDIReservoir` |

Output: `directRadiance` (RGBA16F, linear HDR, render res), a new `GpuTargets` entry.

- **Done when:** on `LightsPunctualLamp`, resampling mode `None` with high initial sample counts (the SDK's unbiased configuration, used as ground truth) matches `FusedSpatiotemporal` with default counts within noise, and the latter is temporally stable.

### Phase G — Indirect: SER bounce, radiance cache, ReSTIR GI (M4; NRC in M5)

Spec §11.1, §11.3–11.6.

#### G1 · SER RT pipeline

- **Feature spec:** [shader_execution_reordering.md](./features/shader_execution_reordering.md) (SER1–SER6). It refines the steps below: the hint is computed from `InstanceIndex()` before the invoke, and the invoke stays because `HitObject::GetClusterID` is NV-only while position fetch on the hit object is EXT-only.
- **Files:** `src/render/vk_rt_pipeline.{hpp,cpp}` (new), `shaders/raytracing/*`
- **Do:**
  - `VkPipeline` + SBT per shaded-ray pass: one raygen, one miss, one hit group (closest-hit only). Descriptor-heap pipeline flag as on the shader objects; `allowClusterAccelerationStructure` for cluster hits. Dispatch with `vkCmdTraceRaysIndirect2KHR`.
  - Raygen: `HitObject::TraceRay` → `ReorderThread(hit, materialBin, bits)` → invoke the minimal closest-hit that returns `{rayInstance, meshletRow, triangle, barycentrics, hitT}` (skip the invoke if Slang exposes `ClusterID` and attributes on `HitObject`, spec Q8) → shade in raygen with `decodeHit` → `sampleMaterial` → BRDF. Until M7 the hint is the material's shader id; misses get their own bin.
  - Log `rayTracingInvocationReorderReorderingHint` at startup.
- **Done when:** a debug raygen reproduces the C3 debug view; reorder on/off timings recorded on the 5070.

#### G2 · Indirect pass

- One ray per pixel from the primary surface (guides + depth), direction from `sampleBrdf` (VNDF), origin offset along `GeoNormal` plus the CLAS truncation bound (spec §10.2). Floor tier: half resolution, checkerboard.
- At X₂: `decodeHit` → `sampleMaterial` → direct light via `RTXDI_SampleLocalLights` / `RTXDI_SampleInfiniteLights` without reuse + one `traceVisibility` → add emissive → query the cache for bounces 2…∞, weighted by the primary `ao` (§2 #10).
- Writes the ReSTIR GI initial sample (hit position, normal, radiance). With NRC the cache radiance is not known yet: the pass writes the query record and G4's resolve completes the sample.

#### G3 · `IRadianceCache` + SHaRC (M4)

- `gi/radiance_cache.slang`: `interface IRadianceCache { update(...); query(...); }`; the indirect pass and the update paths are generic over it.
- SHaRC: `gi/sharc_bridge.slang`, hash grid with 64-bit atomics. Frame order: update (2–4 % of pixels trace short training paths, SER pipeline) → resolve (`SharcResolveEntry`) → query in G2.
- Constants per frame slot through `GpuFrameRoot.cacheConstants`.
- **Done when:** the SHaRC debug view shows cached radiance; an interior lit only by a sun is not black.

#### G4 · NRC behind `IRadianceCache` (M5)

SDK: NVIDIA RTX Neural Radiance Cache (Vulkan host library + HLSL headers), pinned; prebuilt DLLs (NRC + CUDA runtime) copied next to the executable. Verify every SDK name against the pinned headers.

- **Device:** enable `VK_NV_cooperative_vector` and merge NRC's required extensions and features, next to H1's NGX set.
- **Context:** `nrc::vulkan::Context` after device init; `Configure` (render resolution, `kMaxBounces`, training budget) at init and on resize. The SDK allocates its buffers; the engine gives each a heap slot or BDA and never resizes them.
- **Per frame:** `BeginFrame` (scene AABB from §9, training on) → `NrcConstants` into the per-slot block → update paths (`NRC_UPDATE`) + G2 as the query pass (`NRC_QUERY`) → `QueryAndTrain` (inference, then one training step) → **custom resolve** writing `throughput × cachedRadiance` into the ReSTIR GI samples (NRC guide step 7), not the SDK's combined-signal resolve → `EndFrame` after submit.
- **`gi/nrc_bridge.slang`** matches the SDK's HLSL buffer packing with an explicit Slang layout, never by editing vendor headers.
- **Done when:** the reference tier runs NRC; training loss falls after load; an A/B capture against SHaRC exists.

#### G5 · ReSTIR GI (M4)

- RTXDI ReSTIR GI on the G2 samples: temporal then spatial reuse, boiling filter on, reconnection visibility through `traceVisibility`. Output `indirectRadiance` (RGBA16F, render res).
- **Done when:** `Sponza` with a sun shows bounce light under arches, stable after DLSS-RR.

#### G6 · Combine

- `combine` CS: `noisyColor = directRadiance + indirectRadiance + emissive` (RGBA16F). The only place the three terms meet.
- **Must not:** apply AO to direct, add a second path loop, or touch SDK buffers outside their bridge files and host modules.

### Phase H — DLSS Ray Reconstruction + post (M3)

SDK: `NVIDIA/DLSS` (NGX, Vulkan), fetched and pinned. Required: unsupported RR at init throws `std::runtime_error`.

#### H1 · NGX

- **Device:** merge NGX's required instance/device extensions into `Device`, with NRC's set (G4). `NVSDK_NGX_VULKAN_Init` once.
- **Create:** `NGX_VULKAN_CREATE_DLSSD_EXT1` with HW depth, packed roughness, HDR input. Render resolution from the RR optimal-settings query for Quality (spec §2.1); `ResourceManager::renderExtent` and `RenderTargets::recreate` follow it, and the display image moves to display resolution. Recreate on resize.
- **Evaluate** (`NGX_VULKAN_EVALUATE_DLSSD_EXT`):

  | Input | Source |
  |---|---|
  | color | `noisyColor` (G6; `directRadiance` + emissive until M4), linear, pre-exposure |
  | depth / motion | depth / `Motion` guide (render-res pixels, MV scale 1) |
  | diffuse / specular albedo | `DiffuseAlbedo` / `SpecAlbedo` guides |
  | normals + roughness | `NormalRoughness` guide |
  | jitter | `GpuCameraData.jitterOffset` (same as the raster, E1) |
  | `specHitDistance` | not passed in the baseline (spec §9.3) |

  Output: display-res RGBA16F HDR.
- Verify every name against the pinned headers (`nvsdk_ngx_helpers_dlssd_vk.h`).
- **Done when:** 1 spp + RR is stable under camera motion on `Sponza`.

#### H2 · Post + present

- `post` CS: auto exposure from a luminance histogram → tonemap (ACES fitted or AgX; one curve, applied once) → the display image. The existing `present` pass then draws it into the sRGB swapchain with ImGui (the swapchain has no storage support). `exposure` goes to `GpuFrameRoot` and DLSS pre-exposure.
- Until H1 lands, `post` reads `noisyColor` at render resolution. This is bring-up order, not a shipped path.
- `debug_view` keeps writing the display image when a debug view is selected; `Lit` becomes the post output.

### Validation assets

| Asset | Proves |
|---|---|
| `MetalRoughSpheresNoTextures` (+ debug sun) | `cDiff` / `f0` lerp, `α = r²` |
| `MetalRoughSpheres` | B/G packing, sRGB vs linear |
| `TextureTransformTest` | `KHR_texture_transform`, gradient transform |
| `DirectionalLight` | lux, no falloff, `L = −worldDir` |
| `LightsPunctualLamp` | range window, spot sign, RTXDI convergence |
| `NormalTangentTest`, `NormalTangentMirrorTest` | OpenGL Y, TBN, A4 tangents |
| `VertexColorTest` | `COLOR_0` |
| `AlphaBlendModeTest` | alpha bin `discard` + OMM shadows |
| `TwoSidedPlane` / `UnlitTest` | winding flip / unlit |
| `Sponza` | many materials, OMM stats, cache bounce, RR stability |
| `Bistro` | emissive lights, `Power_RIS`, M9 budgets |

With no glTF lights, a debug directional light behind a `DebugViewSettings` flag bit (next to `kFlagEdges`) is allowed (off by default).

---

## 6. Dependencies and milestones

```
animation removal (spec §7.3) → A4
A1 → A2 → A3
B1 (done), E1 (done)
A2 → B2 → B3
A1 + A4 → C1 → C2 → C3
B2 + B3 + C3 + E1(ao, jitter) → E2 → F1 → F2 → F3
F3 → H2 (bring-up) ; E1 + F3 → H1 → H2
F3 + C3 → G1 → G2 → G3 → G5 → G6 → H1 input switch
G3 → G4
A4 + C1 → D1 → D2 ; D1 → D3 ; D2 → D4
```

| Milestone (spec §16.2) | Tasks |
|---|---|
| M0 Contracts | A1, A2, A3, A4, animation removal (**done**) |
| M1 Vis buffer | B1, E1 (**done**); 32-bit payload kept, V8 deferred; culling image checked |
| M2 Cluster RT bring-up | C1, C2, C3 |
| M3 First lit frame | B1 extensions, B2, B3, E1 open items, E2 (punctual), F1–F3, H1, H2 |
| M4 Indirect | G1, G2, G3 (SHaRC), G5, G6 |
| M5 NRC | G4 |
| M6 Alpha | D1–D4 |
| M7 Material binning | E2 emissive triangles, F1 `Power_RIS`, resolve per bin (spec §9.1–9.2) |

PR order follows the milestones. The **first lit pixel** is F3 through the H2 bring-up path.

---

## 7. Host (C++) layout

| Unit | Owns |
|---|---|
| `vk_render_targets` (exists) | vis buffer, display image, guide sets, `GpuTargets` per slot; gains `directRadiance`, `indirectRadiance`, `noisyColor` |
| `vk_mega_geometry` | CLAS and cluster BLAS groups per model, per-slot instance + `GpuRayInstance` + TLAS buffers, instance-write pass, async-compute submit and timeline semaphore |
| `omm_baker` | meshopt bake + cache, `VkMicromapEXT` RAII |
| `vk_rt_pipeline` | SER RT pipelines and SBTs, `traceRaysIndirect2` dispatch |
| `vk_pbr` (`PbrPass`, empty today) | `combine` and `post`; the resolve stays in `Renderer` until M7 moves it to binned dispatch |
| `vk_rtxdi` | `ReSTIRDIContext` + ReSTIR GI, reservoirs, neighbor offsets, light-buffer pass, constants, `rtxdi_initial` / `rtxdi_shade` / `restir_gi` |
| `vk_sharc` | SHaRC buffers, update and resolve passes |
| `vk_nrc` (`nrc_library` exists for the linkage test) | NRC context, SDK buffer bindings, `NrcConstants` per slot, update / query / custom resolve, `QueryAndTrain` |
| `vk_dlss` | NGX init, RR feature, evaluate |

Rules: `MaterialStore` / `LightStore` stay on `Scene`. `ResourceManager::flushGpuAssets` remains the only asset upload site. Every AS, micromap, RT pipeline and SDK object is owned by a move-only RAII type (destroy the object, then its buffer). Build or SDK failure throws `std::runtime_error` naming the failing call. NVIDIA-only SDKs stay inside their module (§2 #0). Every pass gets a `GpuTimerScope` and a Tracy zone with its spec §2.3 budget. All other C++ rules: §2 #14.

---

## 8. Must-not checklist

| Area | Never |
|---|---|
| lighting | any light, GI or denoise path outside RTXDI / ReSTIR GI / SHaRC / NRC / DLSS-RR. A reference loop, homegrown cache or MLP, SVGF, or TAA. Raster lighting (shadow maps, SSAO, SSR) |
| hardware | AMD/Intel code paths, vendor checks or fallbacks now. NVIDIA-only calls outside `vk_nrc`, `vk_dlss`, `vk_mega_geometry` |
| shading | a second BRDF. Perceptual roughness in `D`. Double or missing `NdotL`. AO on direct. Emissive × `NdotL`. sRGB on metal-rough/normal/AO. Premultiplied alpha before the BRDF |
| lights | `dot(spotDir, L)`. Node scale on intensity/range/cone. A CPU loop over lights per frame |
| visibility | alpha tests or material fetches in the opaque raster bin. Alpha tests in the resolve. A fragment G-buffer |
| rays | any-hit. Candidate loops in ray queries. KHR triangle BLASes. CPU-written TLAS instances. Camera culling on TLAS instances. Shading hits outside the SER pipeline shape |
| OMM | OMM on opaque primitives. Baking from any triangle order other than the final cluster triangles. Baking before A4. A second baker next to meshoptimizer |
| geometry | homegrown tangent, normal, cluster or OMM code (§2 #16). Overwriting file-provided tangents or normals. Animation |
| Slang | implicit-LOD `Sample` outside fragment shaders. Material fetches bypassing `sampleSlot`. Preprocessor variants beyond vendor-mandated defines |
| SDKs | NGX calls from Slang. Unpinned SDK versions. Optional or disabled SDK builds |

---

## 9. Open choices

| Choice | Default |
|---|---|
| RTXDI local light sampling | `Uniform` for punctual-only scenes; `Power_RIS` once emissive triangles are lights (M7) |
| Radiance cache per tier | SHaRC on the floor tier, NRC on the reference tier (spec §11.4) |
| Indirect resolution | full on the reference tier, half + checkerboard on the floor tier |
| CLAS `positionTruncateBitCount` | 0; raise only with measured memory gain and the §10.2 origin rule |
| `Blend` | treated as mask (cutoff 0.5) |
| OMM subdivision level | max 5, clamped to device max; adaptive via `meshopt_opacityMapMeasure` `targetEdge` (start at 2 texels) |
| OMM baker | meshoptimizer (D1). The NVIDIA OMM SDK (GPU baker) only if meshopt's CPU bake is too slow on Bistro |
| Cache scene AABB / `kMaxBounces` | `GeometryStore` world bounds, recomputed on load / 4 |
| NRC training budget | SDK default for the render resolution |
| DLSS quality mode | Quality on every tier (spec §2.1) |
| Vis buffer payload | 32-bit; 64-bit only if V8 measurements favor it (spec Q3) |

---

## Appendix — changes vs the 2026-10-25 revision

| Removed | Replaced by | Why |
|---|---|---|
| "Superseded in part" banner pointing at spec §17 | the plan rewritten to follow `engine_spec.md`; tasks mapped to M0–M9 | one source per decision |
| §2 #2, E1: raster G-buffer from the mesh pass, fragment `sampleMaterial` → `discard` → G-buffer writes | vis buffer + compute resolve (done); alpha `discard` only in the M6 alpha-tested bin (D3) | spec §8.3, §9, §17; built in M1 |
| §2 #3: ray queries only, no RT pipeline, SBT or SER | SER RT pipeline for shaded rays (G1), ray queries for visibility | spec §11.1 |
| §2 #4, C1: KHR triangle BLAS per primitive from model slices | CLAS per cluster + cluster BLAS per primitive (C1) | spec §10.2 |
| C2: CPU-written TLAS instances | GPU instance write + TLAS on async compute (C2) | spec §10.3, P2 |
| C3: hit → primitive → index buffer → vertices | hit → cluster → triangle, positions by position fetch, same `Surface` as the resolve | spec §10.1–10.2 |
| D1–D2: OMM per primitive in index-buffer order, attached to BLAS | per cluster, from the final cluster triangles, attached to CLAS; new D3 alpha bin, D4 ray contract | spec §7.1, §10.5 |
| §2 #11, G: NRC only | SHaRC + NRC behind `IRadianceCache`, plus ReSTIR GI and the one-bounce indirect pass (G1–G6) | spec §11.3–11.5 |
| §2 #12, §9: `Uniform` only, emissive meshes out | `Power_RIS` with emissive-triangle lights in M7 | spec §11.2 |
| E2: CPU transforms light rows | GPU light-buffer pass | spec P2 |
| A3: delete RT pipeline, SER, cluster AS and their stubs | keep them; delete only `postprocessing/gbuffer.slang`, `evalLambert`, the `omm` fetch | spec §17 |
| A4 morph-delta remap | dropped: animation is removed first | spec §7.3 |
| §4.3 `SampleLevel(…, 0)` outside fragment; B1 "no `KHR_texture_transform`" | `SampleGrad` with analytic or ray-cone gradients, LOD bias; `KHR_texture_transform` and emissive strength in B1 | spec S10, §9.4, §12.2 |
| §4.4 `sampleOr` | `sampleSlot` (as built), re-probe on Slang v2026.20 | code |
| §2 #0: NVIDIA only, no AMD | NVIDIA now; NVIDIA-only pieces behind modules for a later AMD port | spec §1.2, P-AMD |
| §3 state on `fd67395d1` | re-verified on `a50ebcb46` + vis buffer: 136 B frame root, `GpuTargets`, guides, B1 done | the tree moved on |
| H2 composite → swapchain | `post` → display image → existing `present` pass (sRGB swapchain has no storage) | code |

## Appendix — changes vs the 2026-10-23 revision

| Removed | Replaced by | Why |
|---|---|---|
| §3 state from `fa2222941` (80 B frame root, 24 B push data, one draw per entity × primitive, model-relative `materialId`, lights dropped after flush) | §3 re-verified on `fd67395d1`: shader objects, shared refcounted models and materials, mesh/node split with `GpuNodeInstance`, 16 B push data + `GpuDrawRecord`, debug views, per-entity lights | the tree moved on; the plan's ids and layouts must match it |
| `GpuRayInstance { instanceIndex, primitiveIndex }`, TLAS instance per entity × primitive, BLAS group destroyed by `eraseModel` | `{ nodeInstanceIndex, primitiveIndex }`, TLAS instance per node instance × mesh primitive with the node world matrix, BLAS group lives while the model's `refCount` > 0 | meshes are stored once and placed by nodes; models are shared by entities |
| `GpuFrameRoot` 96 B with `debugFlags` | 120 B: existing `nodeInstances`, `drawRecords`, counts and `debugView` + new `rayInstances`, `frameIndex`, `exposure` | `debugView` already exists and lighting debug modes are `DebugView` entries |
| NVIDIA OMM SDK baker (D1) | meshoptimizer `meshopt_opacityMap*` | already linked; outputs the Vulkan micromap layout and special indices directly; one dependency fewer. The `omm` fetch joins A3's deletions |
| "Normal map only when a tangent exists"; no rule for missing normals | A4: MikkTSpace-compatible `meshopt_generateTangents` where a normal texture is used, flat `meshopt_generateNormals` where `NORMAL` is missing | glTF requires both; normal maps on tangent-less assets would otherwise be ignored |
| E2 "keep light rows after flush" | done (`LightStore::uploaded`, `loadWorldLight`); E2 is only the per-frame RTXDI light buffer | lights became per-entity rows with the shared-model work |
| — | C1 keeps `Indices` uploaded (overrides data-layout finding 5) | the index slice is the BLAS input |

## Appendix — changes vs the 2026-10-04 revision

| Removed | Replaced by | Why |
|---|---|---|
| RTXGI SHaRC (hash grid, `sharc_update` / `sharc_resolve` / `sharc_query`, 64-bit atomics) | RTXGI NRC (G1–G3): `nrc_update` / `nrc_query`, host `QueryAndTrain` + `Resolve`, `noisy_color` CS | NRC chosen as the GI cache. Accepts the costs the previous revision avoided: prebuilt NRC + CUDA runtime DLLs and HLSL buffer-layout matching in the bridge |
| Implicit NVIDIA-only targeting | §2 #0: NVIDIA RTX only, AMD/Intel out of scope for now | stated explicitly so no one adds vendor fallbacks |
| §3 state from `44055d48b`; draft `FrameRoot` with global vertex/index pointers; `GpuPrimitive` with flags; `instanceCustomIndex` = primitive | re-verified §3; §4.2 extends the existing `GpuFrameRoot`; `GpuRayInstance` table (§2 #5); BLAS from model slices, grouped per model (C1) | the code now has model slices, model-relative indices and unload compaction, so no GPU row may store an absolute entity/primitive id |
| `GpuLight.entityId` | CPU-kept light rows (E2) | entity ids shift on unload; GPU light rows are byte-compacted |
| "engine error type"; short C++ style row | `std::runtime_error`; §2 #14–15 from the `cpp-guidelines` / `main-guidelines` skills | no engine exception type exists |

## Appendix — changes vs the 2026-09-29 revision

| Removed | Replaced by | Why |
|---|---|---|
| RTX Mega Geometry (CLAS, cluster-BLAS, PTLAS, `cluster_prepare`, C5) | KHR BLAS per primitive + per-frame TLAS (C1–C2) | not part of the required stack; standard AS reuses existing buffers with no gather pass |
| RT pipeline primary pass, SBT, any-hit, SER | raster G-buffer (E1) + ray queries (D3) | primary visibility already exists in raster; no shader tables or payloads |
| Primary-ray alpha test + `RAY_FLAG_FORCE_OMM_2_STATE` workaround | raster `discard` + per-instance 2-state OMM | no any-hit and no Slang capability patch |
| `DirectMode::Reference` light loop (E3) | RTXDI resampling mode `None` as ground truth (F3) | no custom lighting code |
| NRC, `IRadianceCache`, `GI_CACHE` switch | SHaRC only (G) | one cache, no CUDA DLLs, no DX-layout constraints |
| GPU scene prep, DGC rewrite, PTLAS args, GPU culling (F1–F3) | CPU-written TLAS instances and lights (C2, E2) | unrelated to lighting; counts are small |
| RTXDI presample pass + light PDF texture | `Uniform` local light sampling (§9) | glTF scenes carry few punctual lights |
| Raw-HDR fallback when RR is unavailable | hard requirement | single path |
