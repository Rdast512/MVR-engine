---
name: pbr-implementation-plan
description: Task plan for glTF metallic-roughness PBR on the engine_spec architecture (NVIDIA RTX 20+ only) — visibility-buffer compute resolve, Mega Geometry cluster RT (CLAS, GPU TLAS), SER RT pipeline for shaded rays and ray queries for visibility, RTXDI ReSTIR DI, NRC radiance cache with ReSTIR GI, per-cluster opacity micromaps, DLSS Ray Reconstruction. Use when implementing BRDF, lights, sampleMaterial, the resolve and its guides, CLAS/TLAS, OMM, RTXDI, NRC, ReSTIR GI, DLSS-RR, or post.
---

# Plan: PBR on the visibility buffer + RTXDI + NRC + ReSTIR GI + OMM + DLSS-RR

**Date:** 2026-11-07 · **Verified against:** `feature-pbr` working tree on `a50ebcb46` plus the uncommitted visibility buffer (V1–V7), Slang `v2026.20`, meshoptimizer `v1.3`
**Precedence:** [docs/engine_spec.md](../docs/engine_spec.md) is the architecture. This plan holds the shading tasks and follows it; where they disagree, the spec wins. Milestones are the spec's M0–M9 (§16.2).
**Status (2026-11-14):** owned by engine spec §16.2 (spec §20.4). M1 is done, M2 is built (C1–C4), and M3 is done (B1–B3, E1, E2 punctual, E3, F1–F3, H1, H2; closed 2026-11-14 on `Sponza` lit by E3's sun). No OMM, SER pipeline or NRC code exists yet. See §3.

| Doc | Role |
|---|---|
| [engine_spec.md](../docs/engine_spec.md) | Target architecture, milestones, budgets (normative) |
| [visibility_buffer.md](./features/visibility_buffer.md) | Vis buffer and resolve design, as-built notes (§10) |
| [pbr_lighting_materials.md](./pbr_lighting_materials.md) | Shading rules (normative for agents) |
| [pbr_lighting_materials.html](../docs/pbr_lighting_materials.html) | Human explainer with live graphs and scenes |
| glTF 2.0 [§3.9 Materials](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#materials), [Appendix B](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#appendix-b-brdf-implementation), [Khronos extension specs](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos) | Normative glTF math |
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
| RT geometry | **Mega Geometry**: CLAS per cluster, cluster BLAS per primitive, PTLAS | load-time builds, GPU-written PTLAS ops |
| Direct light | **RTXDI** ReSTIR DI (punctual + emissive-triangle lights) | `RAB_*` bridge, GPU light buffer, two CS entry points |
| Indirect light | one SER bounce + **NRC** + **RTXDI ReSTIR GI** | raygen, minimal closest-hit, NRC bridge, custom resolve |
| Alpha-tested rays | **NVIDIA OMM SDK** CPU baker + `VK_EXT_opacity_micromap`, attached per cluster | load-time bake, micromap build, CLAS attach |
| Denoise + upscale | **DLSS Ray Reconstruction** (NGX) | guides (exist), host evaluate |

Load-time geometry processing (tangents, normals, clusters) is **meshoptimizer**; OMM baking is the **NVIDIA OMM SDK**. The engine has no tangent, normal, clustering or OMM algorithm of its own.

Everything else is the minimum those SDKs need:

1. **One BRDF, one light model.** glTF Appendix B GGX with VNDF sampling + `KHR_lights_punctual`, written once in Slang. RTXDI, the indirect pass and the caches call it through their bridges. No reference light loop, no homegrown cache, no custom denoiser.
2. **Primary visibility is the visibility buffer.** Raster writes depth + a 32-bit payload. One compute resolve builds the surface, calls `sampleMaterial` and writes every guide. RTXDI, the caches and DLSS-RR all read those guides.
3. **Shaded rays use one SER RT pipeline shape; visibility rays use ray queries.** No any-hit anywhere.
4. **One decode for pixels and hits.** A vis-buffer pixel and a ray hit both resolve to `(nodeInstance, cluster, triangle)` and build the same `Surface`. `sampleMaterial` serves both.
5. **Alpha on rays is OMM 2-state.** Every ray forces 2-state micromaps with a ray flag (PTLAS has no instance flag for it), and every instance that is not alpha-tested is `FORCE_OPAQUE`, so traversal never yields a candidate.

---

## 1. Frame at a glance

Spec §5 is the authority. Task ids in brackets.

```
load (once per asset)
  clean-up, normals/tangents (meshopt)                                                   [A4]
  clusters (clusterlod level 0) → optimize level 1 → OMM bake per primitive              [D1]
  micromaps → CLAS per cluster → cluster BLAS per primitive                              [D2 C1]

every frame                                     GRAPHICS                     ASYNC COMPUTE
  2-phase cull, vis-buffer raster, Hi-Z         (done, M1)                 │ instance ops + PTLAS [C4]
  resolve (CS)          vis buffer → Surface → sampleMaterial → guides     │
                        (done, M1; per-bin shaders in M7)          [E1]    │
  ─────────────── timeline wait: TLAS ready ───────────────────────────────┘
  rtxdi_initial (CS+RQ) local + infinite + BRDF candidates → reservoirs    [F3]
  rtxdi_temporal (CS)   temporal resampling → boiling filter               [F3]
  rtxdi_shade (CS+RQ)   spatial resampling → final visibility → direct     [F3]
  NRC update            training paths                                     [G3]
  indirect (RT, SER)    1 ray/px → X₂ shading → cache query → GI samples   [G2]
  NRC                   QueryAndTrain → custom resolve into GI samples     [G3]
  restir_gi (CS+RQ)     temporal + spatial → indirect                      [G5]
  combine (CS)          direct + indirect + emissive → noisy HDR           [G6]
  DLSS-RR (host NGX)    noisy HDR + guides → display-res HDR               [H1]
  post (CS) + present   exposure → tonemap → display image → swapchain, ImGui [H2]
```

Every ray pass reads the TLAS address from `GpuFrameRoot.tlas`: the one PTLAS (C4).

---

## 2. Locked decisions

| # | Decision |
|---|---|
| 0 | **Hardware:** NVIDIA RTX 20+ only, no vendor checks, fallbacks or ports (spec §1.2). NVIDIA-only pieces (Mega Geometry, SER, NRC, NGX, cooperative vectors) are the target stack. Each SDK still lives in its own module (`vk_nrc`, `vk_dlss`, `vk_mega_geometry`) for ownership |
| 1 | **Lighting stack:** RTXDI ReSTIR DI (direct), one SER indirect bounce + NRC + RTXDI ReSTIR GI (indirect), OMM (alpha on rays), DLSS-RR (denoise + upscale). All required. Tiers change resolution, ray counts and NRC settings, never the algorithm (spec P1) |
| 2 | **Primary visibility:** visibility buffer (`R32_UINT`, `visibleRow << 7 \| triangle`) + reverse-Z `D32_SFLOAT`, render resolution, 1 sample, jittered from M3. The compute resolve writes the guides (spec §8.3, §9) |
| 3 | **Rays:** passes that **shade** hits use an RT pipeline built around SER: `HitObject::TraceRay` → `ReorderThread(hit, materialBin)` → minimal closest-hit returning ids, barycentrics and `hitT` → shade in raygen. One raygen, one miss, one hit group per pass. **Visibility-only** rays use `RayQuery`. No any-hit. Dispatch with `vkCmdTraceRaysIndirect2KHR` (DGC has no RT stages on driver 621.01) |
| 4 | **RT geometry:** one CLAS per cluster (the raster meshlets, same triangle order), one cluster BLAS per mesh primitive built once at load and shared by every instance of the model. Instance ops are written on the GPU and one PTLAS is updated in place every frame, on async compute (C4; required since 2026-11-14). No KHR triangle BLAS. This is the state through 1.0. After it, cluster LOD (spec §16.3 L2) builds cluster BLAS per frame from the RT LOD cut, with sharing, merging and caching, so no code may assume a BLAS lives as long as its model |
| 5 | **IDs on a hit:** `InstanceIndex()` → `GpuRayInstance { nodeInstanceIndex, primitiveRow }`, written per frame next to the TLAS instances. `ClusterID` → the cluster's model-relative `meshletRow` (set at CLAS build). `PrimitiveIndex()` → triangle in the cluster. Same triple as the vis buffer's `GpuVisibleMeshlet` + triangle. Nothing persistent stores an entity, node-instance or primitive row |
| 6 | **Alpha:** raster tests alpha only in the `AlphaTested` bin's fragment shader (`discard`), never in the resolve. Rays use OMM per cluster. Every ray sets the 2-state ray flag, since PTLAS has no 2-state instance flag; alpha-tested instances carry no opacity flag and every other instance is `FORCE_OPAQUE` |
| 7 | **Bind model:** descriptor heaps for images and samplers, BDA typed pointers for engine buffers, render-target indices in the per-frame `GpuTargets` block (`GpuFrameRoot::targets`). SDK-declared resources get heap slots in their bridge files |
| 8 | **Material table:** `MaterialStore` on `Scene`, deduplicated and refcounted across models. `GpuPrimitive::materialId` is an absolute row. No pass owns a second table |
| 9 | **Default material:** metallic 1, roughness 1, white. It renders dark without GI, which is correct |
| 10 | **AO:** sampled in the resolve and stored with the guides. Applied only to the radiance-cache query term at the primary vertex. Never on direct light |
| 11 | **GI cache:** NRC only, on every tier (M4). No cache interface: the indirect pass calls the NRC bridge directly |
| 12 | **RTXDI light sampling:** `Uniform` for punctual-only scenes (M3). `Power_RIS` (presample pass + PDF texture) once emissive triangles are lights (M7). Resampling: `TemporalAndSpatial` (RTXDI 3.2 dropped `FusedSpatiotemporal`; [rtxdi_3_2_update.md](./features/rtxdi_3_2_update.md)) |
| 13 | **Scene data per frame:** the CPU writes `GpuFrameRoot`, the camera and changed transforms. Ray instance ops, the PTLAS update and the RTXDI light buffer are written on the GPU from the persistent tables (spec P2) |
| 14 | **C++ style** (`cpp-guidelines` skill, `.clang-tidy`): PascalCase types and `enum class` enumerators; camelCase functions, members, locals; class constants `kPascalCase`; getters `getX` / `isX` / `hasX`; no `m_` or trailing `_`. RAII owners of AS, micromaps, RT pipelines and SDK objects are move-only (Rule of Five); everything else follows the Rule of Zero. `const`/`constexpr` by default, `explicit` single-argument constructors, `[[nodiscard]]` on getters, named constants instead of magic numbers. Failures throw `std::runtime_error` with context. `.hpp` + `#pragma once`, no `using namespace` in headers |
| 15 | **Comments** (`main-guidelines` skill), in C++ and Slang: lowercase, 2–8 words, why or non-obvious behavior only, no trailing period |
| 16 | **Geometry processing:** meshoptimizer v1.3 for missing tangents (`meshopt_generateTangents`, `meshopt_TangentCompatible`), missing normals and clusters (`clusterlod.h` level 0). OMM baking is the NVIDIA OMM SDK's CPU baker. No homegrown version of any of these |
| 17 | **Static geometry only** (spec §7.3): no skinning or morph targets. The animation code is removed in M0, before A4 |
| 18 | **Texture LOD:** the resolve uses `SampleGrad` with analytic barycentric gradients; ray hits use ray-cone gradients through the same function. LOD bias `log2(render / display)` from M3. Implicit-LOD `Sample` only in the alpha-tested fragment shader |
| 19 | **Material model** (spec §9.4): glTF metallic-roughness, `KHR_texture_transform`, `KHR_materials_emissive_strength`. Further KHR PBR extensions become separate material bins (M7+) |

---

## 3. Current state (verified 2026-11-07)

Resource details: [resource_architecture.md](./resource_architecture.md). Vis-buffer details: [visibility_buffer.md §10](./features/visibility_buffer.md#10-as-built-2026-11-07).

| Piece | State |
|---|---|
| glTF parse → `MaterialStore` / `LightStore` / `GeometryStore`, shared refcounted models and materials, mesh/node split, `GpuNodeInstance` | done |
| Frame root | `GpuFrameRoot` 192 B: camera, instances, models, primitives, materials, pbrExtensions, lights, `tlas` (the PTLAS, C4), nodeInstances, drawRecords, visibleMeshlets, 4 counts, `lateVisibleBase`, `debugView`, `cullCounters`, `triangleCull`, `subpixelSnap`, `targets`, `frameFlags`, `localLightCount`, `rayInstances`, `rtxdiConstants` (F1), `cacheConstants` (0 until M4), `frameIndex`, `exposure` (1: H2 keeps the exposure in an image, C-H2-1), `worldLights` (E2), `prevWorldLights` (last frame's rows, F2) |
| Render targets | done: `RenderTargets` ([vk_render_targets.cpp](../src/render/vk_render_targets.cpp)); `GpuTargets` 168 B per frame slot: vis buffer, depth, display (RGBA16F), linear sampler, render size, `current` / `previous` guide sets by frame parity, `directRadiance` (RGBA16F, F3) |
| Culling + raster | done: two-phase GPU culling, reverse Z, 3-level Hi-Z, `RasterBin {Opaque, AlphaTested}` with `DrawRecordFlag::TwoSided` and winding flip. Both bins share one fragment shader and DGC state until M6 |
| Resolve | done: one tiled compute dispatch (8×8). Olano–Greer homogeneous barycentrics evaluated at (+1, 0) and (0, +1), `SampleGrad`, motion from current and previous transforms. `debug_view` writes the display image; `present` draws it into the sRGB swapchain with ImGui in the same rendering |
| Guides | done, two sets: `NormalRoughness` RGBA16F, `GeoNormal` RG16_SNORM (oct), `DiffuseAlbedo` RGBA8 (`cDiff`, a = ao), `SpecularF0` RGBA8 (`f0`, a = specular weight), `SpecAlbedo` RGBA8 (`envBrdfApprox`), `Emissive` B10G11R11, `Motion` RGBA16F (z = view depth change, F2), `Position` RGBA32F (world, w = view depth; F2) |
| `geometry/surface.slang`, `geometry/visbuffer.slang` | done: `TriangleCorners`, `loadTriangleCorners`, `Surface`, `buildSurface` (geometric-normal fallback, tangent sign flip under `FlipWinding`), payload pack/unpack, `computeBarycentrics` |
| `material/pbr.slang` | **B1 done**: `SampledMaterial`, `sampleMaterial(GpuMaterial, Surface, lodBias)`, `sampleSlot` with `NonUniformResourceIndex`, unlit handling, `KHR_texture_transform` per slot from the A1 `GpuMaterial` matrices (2026-11-09) |
| `material/brdf.slang` | **B2 done**: `evalBrdf`, `sampleBrdf` (VNDF), `pdfBrdf`, `fromMaterial`, `envBrdfApprox`; checked on the GPU by `tests/brdf_test.cpp` |
| `lighting/lights.slang` | **B3 done**: `evalLight`, `rangeWindow`, `spotCone`; checked on the GPU by `tests/lights_test.cpp`. Called by the RTXDI bridge (F2) and the `Unshadowed lights` / `Shadowed lights` views |
| Debug views | Guides category done (depth, shading/geometric normal, roughness, albedo, F0, spec albedo, emissive, motion). `LightingOnly` shows F3's `directRadiance` + emissive; `DlssInput` reserved, not implemented |
| Vertex streams | `GpuVertex` 32 B (pos, color, uv0); normals, tangents, uv1 as separate float32 streams. Missing normals and needed tangents are generated at load (A4); the resolve's fallbacks to the geometric normal and no normal mapping remain only as guards |
| Animation code | removed (2026-11-08): no morph or skin data is loaded or uploaded; glTF files load in their base pose |
| Lights | `GpuLight` 64 B per entity in model space, point and spot rows before directional, `type` a `GpuLightType`; `light_world` writes this frame's world rows (E2), which `loadWorldLight` reads |
| Clusters | `clusterlod.h` level 0 (Flex builder, cone weight 0, `optimizeMeshletLevel` 1), groups via `partitionClusters` |
| Device | AS, ray query, RT pipeline, SER, OMM enabled. Cluster AS and position fetch required (C1); partitioned AS required (C4). OMM, SER and cluster AS properties cached in `HardwareCapabilities`, SER hint and cluster AS limits logged. Cooperative vectors **not enabled**. Fragment barycentrics removed |
| Slang modules | done (A2): library files are `module x;`, passes `import` what they use. `gi/sharc.slang` is a textual include left over from SHaRC, to be removed (considerations C-G3-1) |
| `ThirdParty/` | meshoptimizer v1.3, `rtxdi`, `sharc` (to be removed, C-G3-1), `nrc`, `dlss` fetched; linkage tests in `tests/`. NVIDIA OMM SDK 1.9.2 fetched and built as `omm-lib` (CPU baker), no caller until D1 |
| Shader stubs | none: deleted 2026-11-12 (spec §20.2 AU7); each file returns with the task that fills it. `raytracing/trace` exists since F2. The guide layout is `GuideImage` in `types.slang` |

Toolchain facts (re-probed 2026-11-08 on Slang v2026.20, task A2):

| Fact | Consequence |
|---|---|
| `RaytracingAccelerationStructure(uint64_t)` → `OpConvertUToAccelerationStructureKHR` | the TLAS address goes straight into `GpuFrameRoot.tlas` with no AS descriptor |
| `module` / `import a.b;`, typed pointers in structs, `enum X : uint`, `RWTexture2D<float4>.Handle` in push data all validate | adopted (§4). The module name is a plain identifier (`module binding;`, no dots); the path is in the import (`import core.binding;`) |
| command-line `-D` reaches imported modules; `#define`s in a source file do not | one module per define value works; vendor headers that need `#define`s first stay textual |
| heap access still emits no `NonUniform`, even with `NonUniformResourceIndex` | §4.4 |
| `[format(...)]` on a heap handle is accepted but has no effect: `OpTypeImage` stays `Unknown` | the format goes in the type instead: `RWTexture2D<T, 0, format>` with a `SlangImageFormat` value (`core/binding.slang` `storageTex2D`) |
| `slangc -depfile` lists imported modules and works with Ninja | hand-written `DEPENDS` lists removed (§4.5) |

---

## 4. Slang conventions

### 4.1 Modules

Library files become Slang modules (`module brdf;`, exports `public`). Passes `import` them. Vendor HLSL headers (RTXDI, NRC) are `#include`d **only** from pass files and their bridge file, because they depend on textual order (the bridge defines `RAB_*` before the RTXDI headers).

| Module (path) | Owns | State |
|---|---|---|
| `types` | POD mirrors of `src/core/types.hpp`, flags, enums, guide ids | edit |
| `core.binding` | heap helpers (`tex2D`, `samplerOf`, `storageTex2D4`, `uintTex2D`), `bdaPtr` | exists |
| `core.frame` | `loadDrawContext`, `loadCamera`, `loadMaterial`, `loadTargets` | exists |
| `core.math` | `kPi`, ONB, `octEncode`/`octDecode`, `jitterClip`, `pixelToNdc`, `normalMatrix`, PCG RNG | edit |
| `geometry.visbuffer` | payload pack/unpack, `computeBarycentrics`, interpolation | exists |
| `geometry.surface` | `TriangleCorners`, `Surface`, `buildSurface`, `loadCornerVertices`, `buildRaySurface`; shared by the resolve and hits | exists |
| `geometry.hit` | `primaryRay`, `traceClosest`, `decodeHit`: hit ids + barycentrics → `TriangleCorners` (position fetch) → `Surface` with ray-cone gradients; `safeSpawnPoint` | exists (C3) |
| `material.pbr` | `SampledMaterial`, `sampleMaterial`, `sampleSlot` | exists |
| `material.brdf` | `evalBrdf`, `sampleBrdf`, `pdfBrdf`, `fromMaterial`, `envBrdfApprox` | edit |
| `lighting.lights` | `LightSample`, `evalLight`, `rangeWindow`, `spotCone` | exists (B3) |
| `raytracing.*` | `traceVisibility` (exists, F2); payload and minimal closest-hit (G1) | edit / new |
| `lighting/rtxdi_bridge.slang`, `gi/nrc_bridge.slang` | SDK resource macros + callbacks (textual include) | **new** |
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

// appended to GpuFrameRoot after frameFlags; 192 B since F2's previous-frame lights (the static_assert in types.hpp
// owns the size)
public struct GpuFrameRoot
{
    // ... existing fields through `frameFlags` ...
    public uint64_t rayInstances;    // GpuRayInstance[], 0 until C2
    public uint64_t rtxdiConstants;  // GpuRtxdiConstants (F1)
    public uint64_t cacheConstants;  // NRC constants, 0 until M4
    public uint     frameIndex;
    public float    exposure;        // 1: the exposure reaches post and DLSS-RR as an image (C-H2-1)
    public uint64_t worldLights;     // E2
    public uint64_t prevWorldLights; // last frame's worldLights; 0 on a history reset (F2)
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
| No preprocessor variants for the cache | spec S9: only vendor-mandated defines (`NRC_UPDATE`, `NRC_QUERY`) |
| `static const` for tunables | no magic numbers in bodies |
| Entry points in `passes/`, logic in modules | each pass fits one screen |

### 4.4 Non-uniform heap indices

Material texture indices diverge across a wave in the resolve and at hits. Every material fetch goes through `sampleSlot` (`material/pbr.slang`, the spec's `sampleOr`), which wraps the index in `NonUniformResourceIndex`. Slang 2026.14.1 dropped the resulting `NonUniform` decoration on heap access; NVIDIA drivers tolerate it, but it is UB by spec.

Re-probe on every Slang upgrade: compile `resolve.slang`, `spirv-dis`, grep `NonUniform` (still missing on v2026.20). If artifacts appear, add a `WaveReadLaneFirst` waterfall inside `sampleSlot` only.

### 4.5 CMake

- `engine_add_slang_pass` passes `-depfile ${_spv}.d` and `DEPFILE ${_spv}.d`. Drop `DEPENDS`.
- Vendor-mandated defines go in `DEFINES` (`NRC_UPDATE=1`, `NRC_QUERY=1`). One source may produce several `.spv`.
- RT pipeline stages (`raygeneration`, `miss`, `closesthit`) compile to their own `.spv` per entry point.
- Add `-warnings-as-errors all` once the tree compiles clean.

---

## 5. Tasks

Each task lists **Do**, **Done when**, and **Must not** where relevant, plus its milestone. Dependencies are in §6.

### Phase A — Contracts (M0)

#### A1 · POD layouts and constants — done (2026-11-08)

- **Files:** `src/core/types.hpp`, `src/render/push_data.hpp`, `shaders/types.slang`, `src/core/vk_device.cpp`
- **Do:**
  - Slang `AlphaMode`, `LightType` and material flag constants mirroring `GpuMaterialFlag` (§4.2); move `kMaterialFlagUnlit` out of `material/pbr.slang`.
  - `enum class GpuLightType : uint32_t { Directional, Point, Spot }` (values used by `lightTypeFromName`); `GpuLight.type` becomes `GpuLightType`. Keep 64 B.
  - `GpuRayInstance` (8 B) and the `GpuFrameRoot` fields of §4.2 plus `frameFlags` (`kFrameFlagHistoryReset`, [motion_vectors.md](./features/motion_vectors.md) §4), with size and offset asserts.
  - Cache `vk::PhysicalDeviceOpacityMicromapPropertiesEXT` in `HardwareCapabilities::opacityMicromap`, and `vk::PhysicalDeviceRayTracingInvocationReorderPropertiesEXT` with its hint logged at startup ([shader_execution_reordering.md](./features/shader_execution_reordering.md) SER1).
  - **Core-material data lives in `GpuMaterial`** (decided 2026-11-08). The resolve and every hit already load one `GpuMaterial` row; a second table would add a pointer load and a second row of cache lines per pixel, and `GpuMaterialExtension` (724 B per row) is mostly data no shader reads yet. So:
    - `KHR_materials_emissive_strength` is folded into `emissiveFactor` on the CPU when the row is packed: no field, no shader math;
    - `KHR_texture_transform` is precomputed on the CPU into one 2×3 float matrix per slot (`M = T · R · S`; 24 B × 5 slots), so the shader does no `sin`/`cos`. Rotation block in rows is `[c s; −s c]`, as in the Khronos sample renderer and the extension's example (v grows down); the GLSL snippet in the extension README has the opposite sign;
    - flag bits 4–8 (`kMaterialFlagUvTransformBaseColor` … `Emissive`) mark slots with a non-identity transform; the shader reads a slot's matrix only when its bit is set, so identity materials (almost all) load no extra bytes. `texCoord` overrides are already resolved into the slot's UV index.

    `GpuMaterial` grows from 112 to 240 B; update the asserts on both sides. `GpuMaterialExtension` keeps the other KHR extensions for their future material bins.
- **Done when:** changing any field or pad breaks the build.
- **Must not:** add a Slang mirror of `GpuMaterialExtension` or read it per pixel before a KHR extension bin needs it.

#### A2 · Slang modules + frame root fields — done (2026-11-08)

- **Files:** all `shaders/**`, `CMakeLists.txt`, `src/core/vk_resource_manager.cpp`
- **Do:** convert library files to modules (§4.1). `ResourceManager::writeFrameRoot` also fills `frameIndex` and `exposure` (1.0 until H2); the new addresses stay 0 until their tasks. Re-probe the §3 toolchain facts on Slang v2026.20 and update the table.
- **Done when:** `mesh_raster`, `mesh_fragment`, `meshlet_cull`, `hiz_build`, `resolve`, `debug_view` and `present` compile and render unchanged; every guide debug view matches a capture taken before the conversion. Met by a stronger check: the release SPIR-V of all eight passes is byte-identical before and after.

#### A3 · Remove what the plan no longer uses — done (2026-11-08)

- **Files:** `shaders/**`
- **Do:** delete `postprocessing/gbuffer.slang` and `evalLambert`. Keep the RT pipeline, SER and cluster AS extensions and the stubs listed in §3 (spec §17 reverses the old A3).
- **Done when:** the tree builds and renders unchanged.

#### A4 · Shading vertex attributes (meshoptimizer) — done (M0)

- **Files:** `src/core/assets_loader.cpp`, `src/core/geometry_store.{hpp,cpp}`
- **Precondition:** animation code removed (spec §7.3, M0), so no morph deltas need remapping.
- **Why:** glTF says a primitive without `TANGENT` should get MikkTSpace tangents, and one without `NORMAL` gets flat normals. Today the loader generates neither; the resolve compensates with fallbacks (geometric normal, no normal map). The resolve, C3, RTXDI and the caches all read these streams.
- **Do,** per primitive, after the accessors are read and before clusters are built (clusters, CLAS and OMM must see the final vertex set):
  - **Normals missing:** flat normals. Deindex the primitive, then `meshopt_generateNormals(…, crease_angle = 0, smoothing = 0)` per corner (experimental in v1.3; pin the call behind one helper).
  - **Tangents missing, normal texture used by the primitive's material:** `meshopt_generateTangents(result, indices, …, positions, normals, uv, meshopt_TangentCompatible)` per corner, on the UV set named by `normalTexture.texCoord`. `meshopt_TangentCompatible` matches MikkTSpace, which baked normal maps assume.
  - **UV orientation:** the loader stores the file's UVs as they are, since glTF and Vulkan share a top-left origin. Feed `meshopt_generateTangents` `1 - v` in a scratch copy. Measured on `NormalTangentMirrorTest`: `1 - v` matches the shipped `w` on 2770 of 2770 vertices, the file's `v` on 0 of 2770; direction differs by at most 0.034° either way. glTF normal maps are +Y up while its `v` points down, so the shipped tangents are MikkTSpace over the flipped `v`.
  - **Reindex:** both functions return per-corner data. Weld with `meshopt_generateVertexRemapMulti` over every vertex stream plus the new per-corner stream, then `meshopt_remapVertexBuffer` / `meshopt_remapIndexBuffer`. Vertices split only on seams (UV mirror, hard edges).
  - Set `GpuModelAttribute::Normals` / `Tangents` so the streams upload.
- **Done when:** `NormalTangentMirrorTest` with and without its `TANGENT` accessor shows the same shading-normal debug view within the generator's tolerance (`NormalTangentTest` ships no `TANGENT` and always takes the generated path); a mesh without `NORMAL` shows flat facets; the resolve's fallbacks remain only as guards; the cache-hit path (`instantiateModel`) is untouched.
- **Must not:** generate tangents for primitives without a normal texture, overwrite file-provided tangents or normals, or write a homegrown generator.
- **Checked (2026-11-13, RTX 5070, `ENGINE_GUIDE_CAPTURE=1`, validation clean):**
  - `NormalTangentMirrorTest` with generated tangents against its shipped `TANGENT`: shading normal within 0.040° on 99.9% of 128 595 px (max 0.056°), same coverage.
  - `MetalRoughSpheres` without `NORMAL`: shading normal against the geometric normal within 0.063° on 99.9% of 90 029 px. The smooth file normals sit 1.1° from their facets on average, so the check tells flat from smooth; it fails above 0.25°.
  - `NormalTangentTest` (generated path) against the same mesh with tangents computed offline as d*P*/d*u* over (*u*, 1 − *v*): within 0.43° on 99.9% of the pixels (max 0.87°).
  - The resolve keeps its guards: no normal map when `tangent.w` is 0 or the tangent is parallel to the normal (`material/pbr.slang`). `instantiateModel` is unchanged.

### Phase B — Shading library

#### B1 · `sampleMaterial` — done (M1); `KHR_texture_transform` checked (2026-11-14)

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
  - **`KHR_texture_transform`** (2026-11-09): per slot from the A1 `GpuMaterial` matrices, applied in `textureGradients` only when the slot's flag bit is set: `uv' = M·(uv, 1)`, and the matrix's 2×2 part on `ddx`/`ddy`. The normal slot also moves the tangent frame to `dP/du'`, `-dP/dv'` (`transformTangentFrame`).
  - `KHR_materials_emissive_strength`: nothing in the shader; A1 folds it into `emissiveFactor`.
- ~~**Open (M3, with E1):** `lodBias = log2(render / display)` once DLSS-RR sets the render resolution.~~ Done with H1 (E1): `Camera::updateProjection` writes `GpuCameraData.lodBias`, and the resolve passes it to `sampleMaterial`.
- **Done when:** ~~the roughness and F0 debug views on `MetalRoughSpheres` match the texture layout~~ (done 2026-11-13 by the E1 guide capture); ~~`TextureTransformTest` matches the Khronos screenshot in kind~~ (done 2026-11-14).
- **Checked (2026-11-14, RTX 5070):** `ENGINE_GUIDE_CAPTURE` now also captures `TextureTransformTest` and `TextureTransformMultiTest`, with validation clean (core, sync, best practices + NVIDIA). No code change was needed.
  - `TextureTransformTest` (diffuse albedo) matches the screenshot. All three offset quads show their checkmark, and the rotation, scale and combined arrows each point at the green marker, not the yellow (not applied) or red (opposite rotation) one.
  - `TextureTransformMultiTest` shows a checkmark in UV 0, UV 1 and Sample on every row it can show:
    - base color (albedo view);
    - emission and unlit (emissive view);
    - normal (shading-normal view);
    - metal/rough (roughness view, and albedo where metallic is 1);
    - occlusion, through a temporary resolve build that wrote `ao` into the emissive guide. There is no AO view until E1 stores AO.

    The clearcoat rows are dark, which the sample's README counts as a pass when clearcoat is not supported.
  - So the transform reaches all five slots and both UV sets, including the tangent frame of the normal slot.
- **Must not:** sRGB-sample metal-rough/normal/occlusion, use Unity ORM on the metal-rough slot, flip normal Y, or sample with implicit LOD outside a fragment shader.

#### B2 · BRDF (M3) — done (2026-11-09)

- **Files:** `shaders/material/brdf.slang` (keeps `envBrdfApprox` and `kDielectricF0`)

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
    float  alpha = max(roughness * roughness, kMinAlpha); // 1e-3: at a2 = 1e-8 float makes D(nDotH = 1) inf
    float  a2    = alpha * alpha;

    float3 f = fresnelSchlick(f0, vDotH);
    return (1.0 - f) * cDiff / kPi + f * ggxD(nDotH, a2) * smithVisibility(nDotL, nDotV, a2);
}
```

The signature takes `cDiff`/`f0` rather than `SampledMaterial`, so RTXDI and the caches call it straight from guide data (`DiffuseAlbedo`, `SpecularF0`, `NormalRoughness`). A `BrdfInputs fromMaterial(SampledMaterial)` helper computes `cDiff = baseColor·(1−metallic)`, `f0 = lerp(kDielectricF0, baseColor, metallic)`; the resolve switches its inline computation to it, so pixels and hits share one definition.

Also add `sampleBrdf(…, u) -> BrdfSample { l, weight, pdf }` (lobe pick by Fresnel-weighted luminance, clamped to [0.1, 0.9] unless `cDiff` is black, cosine diffuse, GGX **VNDF** specular; `pdf = 0` marks an invalid sample) and `pdfBrdf(…, l)`. RTXDI (`RAB_SurfaceImportanceSampleBrdf`, `RAB_SurfaceEvaluateBrdfPdf`), the indirect pass and ReSTIR GI use them.

- **Done when:** [pbr_lighting_materials.html §7](../docs/pbr_lighting_materials.html#combined) numbers reproduce (white plastic r=0.5 → 0.295 head-on).
- **Must not:** feed perceptual roughness into `D`, drop `NdotL` after the correlated `V`, or multiply by it twice.

#### B3 · `evalLight` (M3) — done (2026-11-13)

- **Files:** `shaders/lighting/lights.slang`; checked on the GPU by `tests/lights_test.cpp`
- **As built:**

```slang
public static const float kInfiniteDistance = 3.402823466e38; // flt max: a valid ray tMax

public struct LightSample
{
    public float3 l;        // unit, toward the light
    public float3 radiance; // incident radiance before NdotL
    public float distance;  // shadow ray tMax
};

// light in world space (loadWorldLight): unit direction, no node scale on intensity, range or cone
public LightSample evalLight(GpuLight light, float3 p)
{
    LightSample s;
    if (light.type == LightType.Directional)
    {
        s.l = -light.direction;
        s.radiance = light.color * light.intensity; // lux
        s.distance = kInfiniteDistance;
        return s;
    }

    const float3 toLight = light.position - p;
    const float d2 = max(dot(toLight, toLight), 1e-12);
    s.distance = sqrt(d2);
    s.l = toLight / s.distance;

    float attenuation = rangeWindow(s.distance, light.range) / max(d2, kMinDistanceSquared); // 1e-4
    if (light.type == LightType.Spot)
        attenuation *= spotCone(dot(light.direction, -s.l), light.innerCone, light.outerCone);

    s.radiance = light.color * (light.intensity * attenuation); // candela
    return s;
}

// khronos window: 1 - (d/range)^4 clamped; range 0 means infinite
public float rangeWindow(float d, float range);
// cosToAxis = dot(spotDir, -l): +1 on axis; ((cosToAxis - cos(outer)) / max(1e-3, cos(inner) - cos(outer)))² saturated
public float spotCone(float cosToAxis, float innerCone, float outerCone);
```

`GpuLight` rows are stored in model space. Since E2, `passes/light_world.slang` writes each frame's world-space rows: `toWorldLight` (`core.frame`) moves position and direction into the world and normalizes the direction. `loadWorldLight` reads those rows. `evalLight` takes either. The directional distance is `FLT_MAX` rather than infinity, so it is a valid ray `tMax` and stays finite in arithmetic.

This is the only punctual light evaluation. `RAB_SamplePolymorphicLight` wraps it; emissive triangles (M7) are sampled by RTXDI's own triangle-light path, not here.

- **Checked** (`tests/lights_test.cpp`, 14 cases against a double-precision reference written from the KHR_lights_punctual text, plus hand-derived numbers): directional lux unattenuated with `l = -direction`; `4 cd / 2² = 1`; range window `1 - 0.5⁴ = 0.9375`; zero at and past the range; the 1e-4 floor on `d²` at 5 mm; on-axis spot `= 1` (`cosToAxis = +1`); 30° in a 45° cone `= 0.2944`; inner 20° / outer 40° at 30° `= 0.3315`; zero outside the cone and behind a spot facing away; `inner = outer` stays finite. Flipping the cone to `dot(spotDir, L)` fails 6 checks.
- **Must not:** use `dot(spotDir, L)` or scale intensity, range, or cone by node scale.

### Phase C — Ray-tracing scene (M2)

Spec §10. Reference: `vk_lod_clusters` (`renderer_raytrace_clusters_lod.cpp`, `instance_assign_blas.comp.glsl`). **Feature spec:** [ray_tracing_mega_geometry.md](./features/ray_tracing_mega_geometry.md) (sample patterns mapped onto C2, C3, D4, G1; steps RT1–RT8).

#### C1 · CLAS per cluster + cluster BLAS per primitive — built (M2)

- **Files:** `src/core/vk_mega_geometry.{hpp,cpp}` (new; in `core`, since `AssetBuffers` owns it and `engine_core` cannot link `engine_render`), `src/core/vk_device.cpp`, `src/core/vk_asset_buffers.cpp`, `src/core/vk_frame_sync.cpp` (`computeCommands`: AS builds need a compute-capable queue)
- **Do:**
  - `VK_NV_cluster_acceleration_structure` becomes **required**: `hasClusterAccelerationStructure == false` throws at device creation. Enable `VK_KHR_ray_tracing_position_fetch` (spec Q7: check `vulkaninfo` on the 2060 Super first).
  - One CLAS per cluster of the model just flushed: positions gathered into scratch as cluster-local arrays with 8-bit cluster-local indices, freed after the build. `clusterID = meshletRow` (model-relative), triangle order = the raster cluster's triangle order, so `PrimitiveIndex()` equals the vis-buffer triangle. `positionTruncateBitCount = 0` until measured.
  - Build into scratch, then move to the final allocation with the real sizes (host estimates are 3–5× too large, [clas_allocation.md](../reference/vk_lod_clusters/docs/clas_allocation.md)). **As built:** the sample's preloaded path (`scene_preloaded.cpp`) instead: a `COMPUTE_SIZES` pass, exact allocation, then an `EXPLICIT_DESTINATIONS` build. Same exact sizes, no move. BLAS sizes are computed the same way after the CLAS build, because a cluster BLAS refers to its CLAS by address.
  - One cluster BLAS per **mesh** primitive from its CLAS (not per node: nodes are TLAS instances). Built right after `flushGpuAssets`, while the slice addresses are current. A cache hit (`instantiateModel`) builds nothing.
  - CLAS and BLAS are grouped per model, indexed like `modelAllocations`, destroyed when the model's `refCount` reaches 0. Other models stay valid: a built AS does not read its inputs.
  - Sizes, counts and alignments come from `VkPhysicalDeviceClusterAccelerationStructurePropertiesNV` and the AS properties, never constants.
- **Done when:** one BLAS address per mesh primitive; validation clean; loading two models then unloading the first keeps the second traceable; a second Sponza adds no AS memory.
- **Checked (2026-11-13, RTX 5070, validation clean with core, sync and best practices + NVIDIA):**
  - One BLAS address per mesh primitive: Sponza 103 of 103, MetalRoughSpheres 5 of 5; a primitive without meshlets keeps address 0.
  - Real sizes against the implicit-mode host estimate: Sponza 3500 CLAS in 4.72 MiB (estimate 15.81 MiB), BLAS 0.24 MiB; the build adds about 6 ms to the Sponza flush.
  - `ENGINE_GUIDE_CAPTURE` loads each shot's model before unloading the previous one, so model 0 is erased 8 times while model 1 stays; validation stays clean, and the normal checks still pass 2 of 2. The motion test passes 13 of 13 and ctest 7 of 7.
  - A second Sponza: `loadModel` takes the `instantiateModel` path, which `MegaGeometry` never sees (no diff there).
  - **Revisited (2026-11-13, [ray_tracing_mega_geometry.md](./features/ray_tracing_mega_geometry.md) RT1):** CLAS and BLAS are built with `PREFER_FAST_TRACE | ALLOW_DATA_ACCESS`, which position fetch requires (VUID-HitTriangleVertexPositionsKHR-None-08751, VUID-RuntimeSpirv-flags-08761). Sizes are unchanged at log precision, and validation stays clean on the motion test and the guide capture. Startup logs `accelerationStructureIndirectBuild` (false on the 5070) and `maxInstanceCount` for C2.
  - **Closed by C3 (2026-11-13):** the remaining model stays *traceable*. Under `ENGINE_GUIDE_CAPTURE` with `ENGINE_RT_CHECK`, every frame's primary rays match the vis buffer across the 8 erasures of model 0 (70 of 70 frames, 0 misses, 0 hits on empty pixels).
- **Must not:** build KHR triangle BLASes, build from a CPU-side ray mesh, or reorder cluster triangles between raster and CLAS.

#### C2 · GPU instances + TLAS (async compute) — built (M2)

- **Files:** `src/render/vk_rt_scene.{hpp,cpp}` (new, `RayTracingScene`), `shaders/passes/rt_instances.slang` (new), `src/core/vk_frame_sync.cpp` (compute pool, per-slot compute command buffers, `computeTimeline`), `src/render/vk_renderer.cpp` (`submitComputeCommands`), `src/core/vk_frame_data.cpp` (root `tlas` / `rayInstances`). The per-model CLAS/BLAS stay in `src/core/vk_mega_geometry.cpp` (C1).
- **Do:**
  - A compute pass writes one `VkAccelerationStructureInstanceKHR` and the matching `GpuRayInstance` per (node instance, mesh primitive) from `GpuNodeInstance` and the per-model BLAS address table. `transform` = 3×4 of the node instance's world matrix; `mask = 0xff`; flags `FORCE_OPAQUE`, or `FORCE_OPACITY_MICROMAP_2_STATE_EXT` for alpha-tested primitives (from the material's alpha mode). No facing flags. Instance capacity, counter and overflow counter in the debug UI (spec S11).
  - Full TLAS build every frame (`PREFER_FAST_TRACE`) on the async compute queue; graphics waits on a timeline semaphore before the first ray pass. Per-slot TLAS storage, scratch, instance and `GpuRayInstance` buffers. `GpuFrameRoot.tlas` and `GpuFrameRoot.rayInstances` point at them.
  - ~~PTLAS only if the measured TLAS build exceeds 0.3 ms~~. Since 2026-11-14 PTLAS is required (spec §10.3): C4 replaces this build.
- **Done when:** a yawing entity moves without BLAS rebuilds; Tracy/Nsight show the TLAS build overlapping the raster; no CPU loop over instances.
- **As built** ([ray_tracing_mega_geometry.md](./features/ray_tracing_mega_geometry.md) §3, RT2–RT3):
  - `GpuRayInstance` rows and a parallel BLAS-address table only change with the scene layout, so the CPU writes them per slot when `FrameData::layoutGeneration` moves (the same filter as the cull templates: active entities, primitives with a BLAS). Per frame the GPU writes each instance's transform from the slot's `GpuNodeInstance`; the CPU writes nothing per instance.
  - `instanceCustomIndex` = absolute primitive row (checked < 2^24), mask `0xFF`, `FORCE_OPAQUE` until D.
  - The count is exact on the CPU, so the build range is a host value (`accelerationStructureIndirectBuild` is false on the 5070) and nothing can overflow; the capacity doubles with the layout and is shown in the UI.
  - Queue: `Device::computeQueue` (family 2 on the 5070, its own family). Buffers are `CONCURRENT` over all families, so no ownership transfers. Each submit signals the frame number on `FrameSync::computeTimeline`; the CPU waits for the slot's last value before rewriting its node instances or buffers. Nothing on the graphics queue waited yet; since C3, the ray command buffer does ([ray_tracing_mega_geometry.md](./features/ray_tracing_mega_geometry.md) §3.5).
  - Requires the Vulkan 1.2 feature `timelineSemaphore`, now in the required set.
- **Checked (2026-11-13, RTX 5070, driver 621.1):**
  - A temporary readback compared every written instance against a CPU reference (transform from the entity and node matrices, custom index, mask, flags, BLAS address): 0 mismatches and transform error 0 for Sponza (103 instances) and instance fields of side 8, 16 and 64 (167, 359 and 4199 instances).
  - Async compute, release build without layers (bench): instance write 0.0008 ms; TLAS build 0.101 ms (103), 0.104 ms (167), 0.119 ms (359), 0.150 ms (4199). These are the baseline C4 is measured against. The graphics total stays 0.29–0.33 ms.
  - Validation clean (core, sync, best practices + NVIDIA) on the motion test (13 of 13) and the guide capture (7 layout rebuilds with model swaps, normal checks 2 of 2); GPU-AV reports no errors. ctest 7 of 7.
  - A yawing entity moves without BLAS rebuilds by construction: BLAS are only built in `MegaGeometry::buildModel`.
  - **Open:** the Tracy/Nsight capture of the overlap (the work runs on the compute family; no timeline view was taken), and the 2060 Super numbers.
- **Must not:** write instances on the CPU, or cull TLAS instances by the camera.

#### C3 · `geometry.hit` — built (M2)

```slang
// committed hit → the same DrawContext and Surface the resolve builds (as built)
public bool traceClosest(GpuFrameRoot root, RayDesc ray, out RayHit hit);
public DecodedHit decodeHit(GpuFrameRoot root, RayHit hit, float3 rayDirection, RayCone cone);
```

`rayInstances[rayInstance]` → node-instance row → `GpuModel` → cluster `meshletRow` → 3 cluster-local vertex ids → uv0/uv1/normal/tangent/color from the model slices (absent stream: mask bit clear → default; after A4 tangents exist wherever a normal texture does) → `buildSurface`. Positions come from `HitTriangleVertexPosition` (position fetch), the truncated CLAS positions: build the geometric normal and the next ray origin from them only (spec §10.2). UV gradients come from the ray cone at `hitT`. The material is `loadMaterial(primitiveRow)`.

- **Debug pass:** a ray-query CS traces primary rays and colors by `meshletRow` and by `rayInstances[…].nodeInstanceIndex`, using the same palette as the vis-buffer `Meshlets` / `NodeInstances` debug views.
- **Done when:** the ray-traced debug view matches the vis-buffer debug view pixel for pixel away from silhouettes; a decoded primary hit gives the same shading normal as the resolve.
- **As built** ([ray_tracing_mega_geometry.md](./features/ray_tracing_mega_geometry.md) §3.5, §5–7, RT4–RT5):
  - `shaders/geometry/hit.slang` holds the following. `primaryRay`: through the pixel center minus the jitter, from the near to the far plane, with a ray cone. `traceClosest`: a ray query returning the committed hit with its cluster id, fetched positions and instance matrices. `decodeHit`, which returns `{DrawContext, Surface}`. `safeSpawnPoint`, a port of RTXMG's. The decode runs the resolve's functions (`loadDrawContext`, `loadCornerVertices`, and `buildRaySurface` beside `buildSurface`); only the positions (fetched), the facing and the gradients (cone) come from the ray.
  - The debug views `Ray meshlets`, `Ray node instances` and `Ray shading normal` (category "Ray tracing") trace from `debug_view` with the vis-buffer palettes. There is no separate `rt_debug` pass.
  - The ray passes run in a second graphics command buffer per slot. It is submitted with the frame and waits for the slot's compute timeline value, so raster and resolve do not wait for the TLAS.
  - `ENGINE_RT_CHECK=1` (`src/core/engine_ray_check.*`, `passes/rt_check.slang`) is passive. It checks every frame of whichever runner scripts the scene (cull check, motion test, guide capture), or 32 frames of the startup view when run alone. It traces each pixel's primary ray, and on interior pixels (the 4 neighbours hold the same payload) requires:
    - the same (node instance, meshlet, triangle);
    - the decode of the ray's ids and fetched positions, at the resolve's barycentrics, reproduces the resolve's normal, geometric normal and material normal (≤ 1e-4°);
    - the ray and raster intersections within 0.1 px of each other on screen;
    - the ray-cone mip within 0.5 of the raster footprint;
    - no self-hit for a ray spawned with `safeSpawnPoint`.

    Two kinds of disagreement do not count as failures. When the pixel center lies within √2 · `subpixelSnap` of either triangle's edge, it is an edge mismatch: the rasterizer snaps vertices and rays do not. When another triangle lies within 1e-4 of the hit distance, it is a depth tie: coplanar overlap.
- **Checked (2026-11-13, RTX 5070):**
  - Frames matched: cull check 214 of 214, motion test 318 of 318, guide capture 70 of 70. The guide capture ran with validation clean (core, sync, best practices + NVIDIA). Misses, id mismatches, empty-pixel hits, self-hits and mip outliers are all 0, and 0.05–0.17% of edge pixels disagree.
  - The decode at the resolve's barycentrics reproduces the normal, geometric normal and material normal at 0.0000°.
    - At the ray's own barycentrics they differ by up to 0.066° (normal) and 1.87° (material normal).
    - The cause is float noise between the two intersections, crossing the sub-texel filter steps of steep normal maps.
    - This is reported, not gated.
  - Position within 0.056 px on screen, from the precision of the hardware hit barycentrics on large triangles; mip within 0.22 levels. The position error was first measured as a share of the hit distance (9.3e-5). E1's jitter pushed it past that gate on one frame without changing anything visible, so it moved to pixels (2026-11-14).
  - Excluded from the id check:
    - 7 pixel-frames (3 distinct pixels) in the cull check, 0.0001–0.0006 px from a triangle edge (the snap is 0.0028 px);
    - 1 pixel in the motion test, two coplanar double-sided triangles 1.4e-6 apart at 3.1 m.

    Both are deterministic, not culling losses.
  - Release bench without layers: frame 0.31 ms, in line with the ≈0.3 ms before C3; `Debug view` 0.040 ms (lit view, now in the ray command buffer).
  - Regressions: motion test 13 of 13, cull check 16 of 16, normal checks 2 of 2, ctest 7 of 7.
  - **Open:** the ray views were not compared by eye; the check above compares the same ids numerically.

#### C4 · PTLAS (M2, reopened 2026-11-14)

PTLAS became required on 2026-11-14 (spec §10.3). Design: [ray_tracing_mega_geometry.md](./features/ray_tracing_mega_geometry.md) §3.6, step RT9.

- **Device:** `VK_NV_partitioned_acceleration_structure` moves into the required set: device creation throws when it is missing, as for cluster AS. Sizes and limits from `HardwareCapabilities::partitionedAccelerationStructure` (`maxPartitionCount` bounds partitions plus global-partition instances, VUID-10535).
- **One PTLAS, updated in place** (src = dst; VUID-10549 forbids overlap only when they differ, and the sample does the same). It replaces C2's per-slot TLAS and full build. One scratch buffer, 256-byte aligned. Frame N's update waits for frame N−1's last ray pass: the ray command buffer signals a graphics timeline value that the compute submit waits on.
- **Ops from `rt_instances`:** after a layout change, a `WRITE_INSTANCE` for every ray-instance row (`instanceIndex` = row); otherwise one only for rows whose world matrix differs from the one the PTLAS holds, appended with one atomic per wave. The pass writes the op count, so the build is indirect with no readback, which also removes the `accelerationStructureIndirectBuild` limit. `GpuRayInstance` is unchanged.
- **Partitions:** a uniform grid over the instances' world positions. ~~Moving entities go to the global partition and return after N still frames~~: measured slower on the 5070 and removed (as built below).
- **OMM:** alpha-tested instances drop `FORCE_OPAQUE` and carry no opacity flag. Every ray sets the 2-state ray flag (`0x400`): add it to the built `traceVisibility` (F2) and `traceClosest` (C3) now, and G1's SER trace has it from the start. With no OMMs before M6, the images do not change.
- **Done when:** `ENGINE_RT_CHECK` matches every frame of the cull check, the motion test and the guide capture (with unloads) as before; a still frame writes zero ops; validation clean (core, sync, best practices + NVIDIA); update and trace times recorded on the 5070 against C2's full-build numbers for Sponza and the 8 to 64 instance fields, including a case with moving entities.
- **Must not:** write ops on the CPU, rebuild every instance on frames without a layout change, or keep a per-slot TLAS.
- **As built** ([ray_tracing_mega_geometry.md](./features/ray_tracing_mega_geometry.md) §3.6, RT9):
  - `Device` requires `VK_NV_partitioned_acceleration_structure` and its `partitionedAccelerationStructure` feature. The optional-extension path is gone, and `maxPartitionCount` is logged (16,777,215 on the 5070).
  - `RayTracingScene` owns one PTLAS (storage and scratch 256-byte aligned) and a state buffer with one 48-byte row per instance: the transform the PTLAS holds. Each frame slot owns its `GpuRayInstance` rows, a parallel `GpuRayInstanceBuild {blas, partition}` table (both CPU-written per layout), a write-instance list and a host-visible op entry with its op count.
  - **Partitions:** a uniform XZ grid over the instances' world-space centers at layout time, with about 64 instances per cell (Sponza 2×2, the side-64 field 9×9). An instance keeps its cell until the next layout change. There is no global partition: the measured update was slower with it (Checked).
  - **`rt_instances`:** a full build (layout change or new PTLAS) writes every row. Otherwise a row is written only when its world matrix differs from its state row, and the state row is updated with it. The write gets `instanceID` = primitive row, `instanceIndex` = row, mask `0xFF` and `FORCE_OPAQUE` (until D). Writes are appended to the op's `argCount` with one atomic per wave.
  - **Build:** `vkCmdBuildPartitionedAccelerationStructuresNV` with the PTLAS's sized input (`PREFER_FAST_TRACE`). The source is 0 on a full build and the destination itself otherwise. The op list and its count are device addresses, so nothing is read back. The CPU resets the slot's op entry (`argCount` 0) after the slot's compute wait, so no transfer command is recorded. The written count is read back after the slot's wait, for the UI and the bench only.
  - **Sync:** the ray command buffer signals `FrameSync::rayTimeline` = frame number. The compute submit for frame N waits for N−1 at `ACCELERATION_STRUCTURE_BUILD`, so the in-place update never runs while frame N−1 traces. Growing the PTLAS waits on the CPU for both timelines at the last submitted frame. A growth keeps headroom (instances doubled, other sizes monotonic) and forces a full build.
  - **OMM 2-state ray flag:** `traceVisibility` uses `ACCEPT_FIRST_HIT | SKIP_PROCEDURAL | FORCE_OMM_2_STATE` (`0x604`), and `traceClosest` uses `SKIP_PROCEDURAL | FORCE_OMM_2_STATE` (`0x600`). Slang 2026.20 emits the flag without the `RayTracingOpacityMicromapKHR` capability, which spirv-val rejects, so `requireOmm2StateRayFlag()` in `raytracing.trace` declares it through `spirv_asm` (considerations C-C4-3).
  - UI and bench show PTLAS instances, capacity, partitions and the last completed frame's written count.
- **Checked (2026-11-14, RTX 5070, driver 621.1):**
  - `ENGINE_RT_CHECK` matched every frame: motion test 318 of 318 (13 of 13 scenarios), cull check 294 of 294 (16 of 16 poses), guide capture 154 of 154 (model swaps and unloads, normal checks 2 of 2). `InstanceIndex()` returns the written `instanceIndex`: no id mismatches.
  - Validation clean (core, sync, best practices + NVIDIA) on all three runners. GPU-AV on the guide capture reports no errors. spirv-val passes on every pass. ctest 7 of 7.
  - A still frame writes 0 instances. A moving entity rewrites only its own instances each frame (103 for Sponza), and nothing once it stops. This was seen in a temporary per-frame log taken while the global partition still existed, when a stop cost one extra write to return home. Without the global partition, a stop needs no write.
  - Bench, release build without layers. Moving cases used a temporary per-frame yaw on every entity, or on every 16th entity; it was removed afterwards. Times are PTLAS build on async compute, against C2's full build:

    | Scene | Still (0 written) | Moving | C2 full build |
    |---|---|---|---|
    | Sponza (103) | 0.015–0.018 ms | 0.12 ms (103 written) | 0.101 ms |
    | Field 8 (167) | 0.017 ms | — | 0.104 ms |
    | Field 16 (359) | 0.017 ms | — | 0.119 ms |
    | Field 64 (4199) | 0.016–0.019 ms | 0.12 ms (359 written), 0.18 ms (4199 written) | 0.150 ms |

    The instance pass stays at 0.0008–0.0010 ms. Trace passes do not change between still and moving runs (ReSTIR DI initial 0.011–0.013 ms, `Debug view` 0.027 ms). The graphics total is 0.31–0.33 ms still and 0.31–0.36 ms moving.
  - **A/B (single runs, ±5% noise):**
    - Global partition holding every instance, moving entities parked there and returned after 16 still frames: 0.19 / 0.21 / 0.21 ms for the three moving cases. A global partition of 256 instances: 0.18 / 0.21 / 0.18 ms. Without it: 0.12 / 0.18 / 0.12 ms. The global partition was removed.
    - Build flags: none or `PREFER_FAST_BUILD` gave no gain on moving frames, and `PREFER_FAST_BUILD` raised still frames to 0.09–0.14 ms.
    - Grid of ~16 instances per cell: 0.11 / 0.16 / 0.16 ms. ~256 per cell: 0.13 / 0.18 / 0.12 ms. ~64 kept.
  - Any non-empty update costs at least ~0.11 ms, so a scene where everything moves updates slower than C2's full build (considerations C-C4-4).
  - **Open:** the 2060 Super, Bistro (M9, spec Q10), and the Nsight overlap capture inherited from C2.

### Phase D — Alpha: raster bin + opacity micromaps (M6)

Spec §8.2, §10.5.

#### D1 · Bake per primitive, index per cluster

- **Files:** `src/core/omm_baker.{hpp,cpp}` (new)
- **Baker:** NVIDIA OMM SDK 1.9 **CPU baker** (`ThirdParty::omm`, `ommBakerType_CPU`): `ommCreateBaker` → `ommCpuCreateTexture` → `ommCpuBake` → `ommCpuGetBakeResultDesc`. Output is the `VK_EXT_opacity_micromap` layout directly (format values and special indices `-1..-4` match Vulkan). Move-only RAII owners for the baker, texture and result handles; a failed `ommResult` throws with context; `messageInterface` goes to the engine log.
- **Do:**
  - Qualify: `AlphaMode::Mask` (cutoff = `alphaCutoff`) and `AlphaMode::Blend` (cutoff 0.5) with a base-color alpha texture. Anything else is opaque for rays.
  - **Unit of work:** one `ommCpuBake` per qualifying primitive, not per cluster, so the SDK's reuse passes span all of the primitive's clusters. The input index buffer is the **final** cluster triangles concatenated in cluster order, after `meshopt_optimizeMeshletLevel` 1, which may rotate corners (spec §7.1 step 4–5). Micromap data is corner-sensitive, so never bake from an earlier order or from the primitive's index buffer.
  - **Output:** `arrayData`, `descArray`, `descArrayHistogram` and `indexBuffer` are copied out, then `ommCpuDestroyBakeResult`. `indexBuffer` has one entry per input triangle in input order, so each cluster's OMM index buffer is a subrange of it. Use the reported `indexFormat` (16 or 32 bit; `Allow8BitIndices` stays off). All of it is cached with the model's geometry (spec §7.2).
  - **Alpha input** must be what the shader reads (SDK guide, "Texture Compression"): one `ommCpuTexture` per image, UNORM8, mip 0 only. Uncompressed sources use their decoded alpha; a KTX2/BC texture is decoded from its runtime blocks, not from the source image. The loader keeps this alpha until the bake and frees it after.
  - **Cutoff:** `alphaCutoff` in `ommCpuBakeInputDesc`. The shader tests `baseColorFactor.a × texture.a`, so the baked value is `cutoff / baseColorFactor.a`. `alphaMode = ommAlphaMode_Test` for both glTF modes, matching the raster `discard` (D3).
  - **Sampler:** `runtimeSamplerDesc` from the glTF sampler. The SDK takes one address mode for both axes: mixed `wrapS` / `wrapT` logs a warning and uses `wrapS`.
  - **UVs:** `ommTexCoordFormat_UV32_FLOAT` from the slot's UV set, with the slot's `KHR_texture_transform` matrix (`GpuMaterial::uvMatrices`) applied first, since the SDK bakes exactly the UVs it is given. Same orientation as the shader samples, or the micromap is mirrored against `discard`.
  - **Level:** `maxSubdivisionLevel` from §9 clamped to `opacityMicromap.maxOpacity4StateSubdivisionLevel`; `dynamicSubdivisionScale` from §9 makes it adaptive to texel area.
  - **States:** `ommFormat_OC1_4_State` with `ommUnknownStatePromotion_ForceOpaque` (the SDK's recommendation); the instance flag collapses unknown states to 2-state at trace time.
  - **Threading:** `ommCpuBake` is thread-safe on one baker. Run one bake per job on the loader's workers (spec X3). Never set `EnableInternalThreads`: it needs OpenMP, which the build turns off.
  - Debug builds set `ommCpuBakeFlags_EnableValidation`; `ommDebugSaveAsImages` is the tool for checking a bake against its texture.
- **Done when:** the cache round-trips, and `ommDebugGetStats` shows > 70 % known micro-triangles on Sponza foliage.
- **Must not:** bake from the source image of a BC texture, or per cluster.

#### D2 · Build and attach to CLAS

- **Do:**
  - Micromap buffers with `eMicromapBuildInputReadOnlyEXT` / `eMicromapStorageEXT`; one micromap per baked primitive; D1's `descArray` is the `VkMicromapTriangleEXT` array (same layout: `dataOffset`, `subdivisionLevel`, `format`); `descArrayHistogram` converted field by field to `VkMicromapUsageEXT`; `vkGetMicromapBuildSizesEXT` → `vkCmdBuildMicromapsEXT`, RAII-owned, built before C1 in the same command buffer.
  - CLAS build info: `opacityMicromapArray`, `opacityMicromapIndexBuffer` (the per-cluster map of D1), `opacityMicromapIndexType`; the build sets `VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_CLUSTER_OPACITY_MICROMAPS_BIT_NV` (VUID-…-opacityMicromapArray-10881).
- **Done when:** validation is clean, and shadow rays through `AlphaBlendModeTest` foliage show cut-outs.

#### D3 · Alpha-tested raster bin

- **Files:** `shaders/passes/mesh_fragment.slang` (or a new `mesh_alpha.slang`), `src/render/vk_dgc.cpp`, `src/render/vk_shader_objects.cpp`
- **Do:** the `AlphaTested` bin gets its own fragment shader: base-color alpha (implicit-LOD `Sample` is fine in a fragment shader) against `alphaCutoff` (Mask) or 0.5 (Blend) → `discard`, then the same payload write. Its own DGC state command buffer, matching the execute state exactly. Cluster/group state bits `ALPHAMASKED` / `*_MIXED` route draw records to the bin (spec §8.1 A6).
- **Done when:** `AlphaBlendModeTest` cut-outs match in raster and shadows; the opaque bin's fragment shader still fetches no material.
- **Must not:** test alpha in the resolve, or premultiply alpha.

#### D4 · Ray contract

- Visibility rays: one helper, `traceVisibility(origin, dir, tMax)` in `raytracing.trace`, a `RayQuery` with no candidate loop. Shaded rays: the SER pipeline of G1, closest-hit only. Every ray carries the OMM 2-state ray flag (C4).
  - **Built early with F2 (2026-11-14):** `traceVisibility(tlas, origin, direction, tMin, tMax)` with `ACCEPT_FIRST_HIT_AND_END_SEARCH | SKIP_PROCEDURAL_PRIMITIVES`, one `Proceed()`. The debug candidate counter, `pointOffset` and `minT` (feature spec §6.1) are still open here.
- In debug builds, count any candidate in `Proceed()` into a debug buffer and assert zero.
- **Must not:** write candidate alpha-test loops or any-hit shaders.

### Phase E — Resolve and guides

#### E1 · Vis-buffer resolve + guides — done (M1); AO, jitter and unlit specular done (2026-11-14), `lodBias` done with H1

- **Files:** `shaders/passes/resolve.slang`, `src/render/vk_render_targets.cpp`, `src/render/vk_renderer.cpp`, `src/scene/vk_camera.cpp`
- **As built:** one 8×8-tiled compute dispatch. Payload → `DrawContext` → `loadTriangleCorners` → `computeBarycentrics` (Olano–Greer, also at (+1, 0) and (0, +1) for gradients) → `buildSurface` → `sampleMaterial` → guides. Empty pixels (depth 0) write zero guides and the motion of a direction at infinity. Two guide sets by frame parity (`GpuTargets::current` / `previous`); RTXDI and ReSTIR temporal passes read `previous`.

  | Image | Format | Content | Read by |
  |---|---|---|---|
  | depth | D32F reverse-Z | hardware depth | RTXDI, ReSTIR GI, DLSS (`InUseHWDepth`) |
  | `NormalRoughness` | RGBA16F | world shading N, **perceptual** roughness | RTXDI, indirect, DLSS (packed roughness) |
  | `GeoNormal` | RG16_SNORM | oct geometric normal | ray origin offset |
  | `DiffuseAlbedo` | RGBA8 | `cDiff`, a = ao | RTXDI, indirect, DLSS diffuse albedo |
  | `SpecularF0` | RGBA8 | `f0` × specular weight, a = specular weight (0: unlit) | RTXDI, indirect |
  | `SpecAlbedo` | RGBA8 | `envBrdfApprox(f0, NdotV, roughness)` | DLSS specular albedo |
  | `Emissive` | B10G11R11 | emissive (unlit: baseColor) | combine |
  | `Motion` | RGBA16F | render-res pixels, current → previous, no jitter; z = view depth there − here (F2) | DLSS, RTXDI / ReSTIR GI temporal |
  | `Position` | RGBA32F | world position, w = view depth, 0 on empty pixels (F2) | RTXDI surfaces and ray origins, both frames |

- **Open:**
  - ~~**AO into `DiffuseAlbedo.a`** (M3, needed by G2). DLSS reads only rgb.~~ Done 2026-11-14: `ao = lerp(1, R, strength)`, with an `Ambient occlusion` debug view that the guide capture writes for every shot.
  - ~~**Jitter** (M3, spec §12.2): Halton(2,3), phase count `8 · (display / render)²`, applied through `jitterClip` in the raster and the resolve's pixel → NDC mapping; motion stays unjittered; culling uses the unjittered matrix and the exact pixel tests widen by the jitter amplitude.~~ Built 2026-11-14, off by default:
    - `Camera` writes `jitterOffset` (NDC) from Halton(2,3), starting the cycle at index 1. There are `8 · display area / render area` phases, 8 today.
    - Primary rays subtract the jitter (C3).
    - The cull views widen `subpixelSnap` by this frame's jitter in pixels, since `cullViewProj` is unjittered; the per-triangle test already reads jittered clip positions.
    - On by default since H1, which resolves it. `ENGINE_JITTER=0` or the Camera panel's `Jitter` checkbox turns it off. The motion test and the guide capture default to off, since they compare frames pixel by pixel.
  - ~~**`lodBias`** = `log2(render / display)` (B1).~~ Done with H1: `Camera::updateProjection` writes `GpuCameraData.lodBias` (the former `pad0`) from the render and swapchain widths, −0.585 at Quality, and the resolve passes it to `sampleMaterial`.
  - ~~`DebugView::LightingOnly`~~ done with F3 (`directRadiance` + emissive). ~~`DlssInput`~~ done with H1: it shows `noisyColor`.
  - **Motion vectors:** [motion_vectors.md](./features/motion_vectors.md): history bugs B1–B4, the history-reset flag, and a reprojection debug view that proves the vectors (MV1–MV5).
  - Spec §2.3 timings deferred (2026-11-08); take them on a release build with validation off (vulkan-dev skill §6.1).
  - M7 splits the resolve into per-material-bin shaders (spec §9.1–9.2); the per-pixel body stays this one.
- **Unlit specular (done 2026-11-14, before E2/F1):** unlit surfaces reached the guides with `f0` 0.04 and roughness 1, so a light loop would add a specular term that `KHR_materials_unlit` forbids. `f0` 0 alone is not enough, since Schlick's grazing term remains.
  - **As built:** `SampledMaterial.specularWeight` scales the whole specular lobe, Fresnel included: 1, or 0 for unlit. It is the role `KHR_materials_specular`'s `specular` factor would take.
  - The resolve writes it to `SpecularF0.a` and multiplies `f0` and `SpecAlbedo` by it, so unlit surfaces read `f0` 0 and DLSS specular albedo 0.
  - F2's `RAB_GetMaterial` and the path code must skip the specular lobe where the weight is 0. `evalBrdf` takes no weight yet; it gets one when the first light loop reads guides.
- **Done when:** ~~`MetalRoughSpheres`, `TwoSidedPlane` and `UnlitTest` guides match the Khronos screenshots in kind~~: done 2026-11-13 by the scripted guide capture. `ENGINE_GUIDE_CAPTURE=1` loads each sample alone, frames it and writes every guide view as a PNG under `logs/guides` (`src/core/engine_guide_capture.*`, `Renderer::requestDisplayCapture`).
  - `MetalRoughSpheres`: the metal row (top) has zero `cDiff` and a colored `f0`; the dielectric row has `f0` 0.04; roughness rises from Smooth (left) to Rough (right), as the labels say. Sphere normals point +X right, +Y up and +Z at the center.
  - `TwoSidedPlane`: both sides draw the same albedo and roughness. The shading and geometric normals read +Y from above and −Y from below.
  - `UnlitTest`: diffuse albedo 0; emissive uniform at sRGB (255, 128, 0) and (0, 128, 255). The README says `#FF7F00` / `#007FFF`, but the factor 0.2176 is 0.5 under gamma 2.2, so 128 is its exact sRGB encoding. On scattered pixels a 1.0 channel reads one storage step low (254 or 253): the interpolated `surface.color` sums to 1 − ε, and the B10G11R11 store rounds toward zero. This is storage precision, not shading.

  ~~Motion is zero for a static scene and consistent under rotation~~: done 2026-11-13 by the scripted motion test ([motion_vectors.md](./features/motion_vectors.md) §5.1).
- **Checked (2026-11-14, RTX 5070):**
  - **AO and unlit:** the `Ambient occlusion` view shows `TextureTransformMultiTest`'s occlusion row (checkmarks in UV 0, UV 1 and Sample), and 1 elsewhere. `f0` is black on `UnlitTest` and on that model's Unlit row.
  - **Culling with jitter on:** `ENGINE_CULL_CHECK` 16 of 16 bit-identical. Its unculled captures now come a whole number of jitter cycles after the culled ones, so both sit on the same phase. Without the widening the 16 poses still pass, so Sponza does not exercise it; the widening is the spec's conservative rule.
  - **Rays with jitter on:** `ENGINE_RT_CHECK` 294 of 294 (cull check) and 318 of 318 (motion test). With the jitter subtraction removed from the primary rays, 0 of 32 frames pass, which shows the jitter is real.
  - **Motion test with jitter on:** 9 of 13 is expected. The still-scene scenarios (static, teleport, FOV change, resize) also require identical albedo frame to frame, which jitter changes by design; their motion stays 2.2e-5 px. Camera-motion errors are ≤ 0.03 px, and entity reprojection passes. Run the motion test without `ENGINE_JITTER`; it passes 13 of 13.
  - Validation clean (core, sync, best practices + NVIDIA) on the guide capture with the ray check, and on the cull check with the ray check and jitter. ctest 7 of 7. Resolve 0.130 ms (0.142 ms before, within noise).
- **Must not:** write GGX `α` as roughness, jitter without unjittered motion, or sRGB-encode guides.

#### E2 · GPU light buffer — punctual built (2026-11-14), emissive triangles M7

- **Already done:** per-entity `GpuLight` rows in model space, placed by `loadWorldLight`.
- **Do (M3):** a compute pass transforms the punctual rows by their entity's world matrix into the RTXDI light buffer (F2), normalizing the direction and keeping photometric values. Lights move with their entity with no CPU loop.
- **Do (M7):** emissive-triangle lights, built on the GPU at load from primitives with a non-zero emissive factor, appended to the local-light range.
- **Done when:** spinning `LightsPunctualLamp` moves its highlight with the mesh; Bistro emissives light their surroundings (M7).
- **As built (punctual):**
  - **Pass:** `passes/light_world.slang` runs one thread per light. It writes `toWorldLight(root, lights[i])` into `GpuFrameRoot.worldLights`, the `WorldLights` buffer of the frame slot (`src/render/vk_world_lights.*`).
  - **Queue:** it is recorded on the async compute queue before the TLAS build. Readers wait for the compute timeline, as rays do (feature spec §3.5), and the CPU writes nothing per light.
  - **Buffer:** device-local, one per slot, grown to the next power of two after the slot's fence.
  - **Order:** `AssetBuffers::uploadLights` keeps the model-space rows point and spot first, then directional, with a stable partition on load and unload only. `GpuFrameRoot.localLightCount` (the former `pad0`) holds the split, which gives RTXDI's local and infinite regions with the identity `RAB_TranslateLightIndex`. The root grows to 184 B.
  - **Debug view:** `Unshadowed lights` (Lighting) sums `evalBrdf · evalLight · NdotL` over every world light on the guides, the way F2's surface bridge will read them. Unlit surfaces take no light. It is a check of E2 and the guides, not the lighting.
- **Checked (2026-11-14, RTX 5070):**
  - **Guide capture:** it adds `LightsPunctualLamp` (5 point lights) and `DirectionalLight` (1 lux sun) with the `Unshadowed lights` view.
    - The lamp is lit from inside its shade, as in the Khronos screenshot. The shade saturates without exposure or tonemapping.
    - The sun lights the spheres from the camera side (its node has identity rotation, so the light shines along −Z), and the highlights keep the light's hue, as the asset's README asks.
  - **Spin:** a temporary run yawed the lamp entity 90° and rotated the camera by the same 90° about the origin.
    - The `Unshadowed lights` capture matched the unrotated one: 11 of 58,206 lit pixels differ by more than 8/255 (max 24/255), against 402 albedo pixels at most 2/255 from raster noise.
    - With the entity matrix left out of the light pass, 44,299 pixels differ (max 255). So the highlight moves with the mesh.
  - Validation clean (core, sync, best practices + NVIDIA) on the guide capture with `ENGINE_RT_CHECK`. Motion test 13 of 13, cull check 16 of 16, ray check every frame, ctest 7 of 7.
  - Sponza has no lights, so the bench skips the dispatch.

#### E3 · Engine sun — built (2026-11-14)

- **Why:** M3's done-when names "Sponza + sun", and G3 and G6 need a sunlit interior, but the glTF samples in the repo (`models/Sponza`, `gltf/2.0/Sponza`) ship no `KHR_lights_punctual`. The §5 note allowed a debug directional light; no step owned it.
- **As built:**
  - `WorldLights::Sun` (enabled, azimuth from +X toward +Z, elevation, illuminance in lux, color) adds one directional row after the asset's rows, so it falls in RTXDI's infinite region with no reordering. `passes/light_world.slang` writes it from `LightWorldPushData` (40 B): thread `assetLightCount` builds the sun row, the others transform their model rows as before.
  - `WorldLights::lightCount()` and `localLightCount()` are the one source of the world light counts: `GpuFrameRoot.lightCount` and `localLightCount` (through `FrameRootInputs`), RTXDI's light regions (`RtxdiContext` reads `WorldLights`, not `AssetBuffers`), and `Renderer::isDlssPresented()`.
  - A change of the world light count (load, unload, sun toggle) resets history, since reservoirs hold light indices.
  - On by default: az 20°, el 65°, 10 lux, white. The courtyard runs along X and is about 11 units tall, so a sun across it is blocked by the opposite wing below ~55°. 10 lux keeps the RGBA16F `noisyColor` far from its limit (C-H2-1); auto exposure makes the scale irrelevant.
  - The `Sun` panel sets every field and restores the defaults. The guide capture turns the sun off, so the samples keep their own lights.
- **Checked (RTX 5070, driver 621.01):**
  - `Shadowed lights` on a courtyard view: the floor, the planters' and columns' shadows and the upper arches are lit, the curtains facing ±Z are not. A sun straight down lights the courtyard and none of the gallery under the arcade roof (a shadow ray tMax of 0.5 lit 32% of the gallery view, 5.0 none: the arcade ceiling is 4–5 units up).
  - The startup camera sits in the +Z gallery behind the curtains. No sun direction lights more than 0.6% of that view directly (sweep over 18 directions); it waits for M4's indirect light (considerations C-E3-1).
  - Motion test 13 of 13, cull check 16 of 16, guide capture 2 of 2, ray check every frame; ctest 7 of 7. Validation over the default scene (bench) and the motion test with jitter shows only NGX's messages (C-H1-2).

### Phase F — RTXDI ReSTIR DI (M3; emissive + `Power_RIS` in M7)

SDK: `NVIDIA-RTX/RTXDI-Library` (host C++ + HLSL headers), pinned in `ThirdParty/CMakeLists.txt`. Required.

#### F1 · Host context — built (2026-11-14)

- `rtxdi::ReSTIRDIContext` with `ReSTIRDIStaticParameters { renderWidth, renderHeight, CheckerboardMode::Off }`. `SetFrameIndex` each frame; recreate on resize with `RenderTargets::recreate`.
- Local light sampling `Uniform` until M7, then `Power_RIS` (presample pass + PDF texture). Resampling `TemporalAndSpatial` (§2 #12) since the RTXDI 3.2 update (2026-11-15, [rtxdi_3_2_update.md](./features/rtxdi_3_2_update.md)); F1–F3 below describe the 3.1 build, with `FusedSpatiotemporal`.
- Buffers: DI reservoirs (`RTXDI_PackedDIReservoir`, sized by the context), neighbor offsets (`rtxdi::FillNeighborOffsetBuffer` once), one host-visible constants block per frame slot reached through `GpuFrameRoot.rtxdiConstants`.
- **As built:** `RtxdiContext` (`src/render/vk_rtxdi.*`), owned by `Renderer`.
  - **Context:** created in `recreate(renderExtent)`, which `Renderer::rebuildSwapchainResources` calls after `RenderTargets::recreate` with the device idle. It is configured with `FusedSpatiotemporal` (the library default is `TemporalAndSpatial`) and `Uniform` local light sampling; every other parameter keeps the library default. `prepareFrame(slot, frameNumber)` runs after the slot's fence: `SetFrameIndex`, then the slot's constants.
  - **Constants:** `GpuRtxdiConstants` (352 B, `vk_rtxdi.hpp`) holds the ten RTXDI parameter blocks in their own field order (runtime, light buffer, reservoir buffer, buffer indices, initial sampling, temporal, boiling filter, spatial, spatiotemporal, shading), then the `reservoirs` (@ 336) and `neighborOffsets` addresses. The blocks are 16-byte multiples of `uint32` and `float`, so the scalar layout matches. Its Slang mirror (`lighting/rtxdi_constants.slang`) and layout-test entry came with F2. RIS segment parameters wait for `Power_RIS` (M7).
  - **Light regions:** local `{0, localLightCount}`, infinite `{localLightCount, lightCount − localLightCount}`, no environment light. They index `worldLights` (E2) with the identity `RAB_TranslateLightIndex`.
  - **Reservoirs:** one device-local buffer of `c_NumReSTIRDIReservoirBuffers` (3) × `reservoirArrayPitch` × 24 B, shared by both frame slots. The frames run in queue order, and the fused mode writes buffer `(last + 1) % 3` while reading `last`, so frame N+1 writes the buffer frame N−1 read. F2 needs the reservoir barriers between frames, not a copy per slot. Contents are undefined after a resize; resize arrives as a camera cut (`HistoryReset`).
  - **Neighbor offsets:** 8192 `float2`, converted once from the library's RG8_SNORM pairs (value / 127) into a host-visible buffer, since the bridge reads them through BDA instead of a typed buffer.
  - **Log:** each recreate logs the size, for example `ReSTIR DI 1280x720: 3 reservoir buffers of 921600 (63.3 MiB)`.
- **Checked (2026-11-14, RTX 5070):**
  - The reservoir pitch matches `ceil(w/16) · 256 · ceil(h/16)` at 1280×720 and 1100×620 (motion test resize).
  - A temporary log of the first frames showed `SetFrameIndex` rotating the fused indices: initial-sampling output 2, 0, 1, …, with the temporal input always the previous frame's output, and a new `uniformRandomNumber` each frame.
  - On the guide capture, `LightsPunctualLamp` gives 5 local and 0 infinite lights; `DirectionalLight` gives 0 local and 1 infinite.
  - Validation clean (core, sync, best practices + NVIDIA) on the guide capture with `ENGINE_RT_CHECK` and on the motion test, which resizes. Motion test 13 of 13, cull check 16 of 16, ray check 130 of 130 frames, guide normal checks 2 of 2, ctest 7 of 7.
  - No shader reads the constants yet; the per-frame cost is one 352 B write.

#### F2 · Light buffer + bridge — built (2026-11-14)

- **Light buffer** (GPU-written, E2): local lights (point, spot; emissive triangles from M7) first, then infinite (directional), in stable order so `RAB_TranslateLightIndex` is the identity. **As built:** `GpuFrameRoot.worldLights` with `lightCount` rows, of which the first `localLightCount` are local. Emissive triangles will need their own range after these rows, or a recount.
- **`lighting/rtxdi_bridge.slang`** defines the RTXDI resource macros against heap slots and implements the `RAB_*` set:

  | Group | Functions | Backed by |
  |---|---|---|
  | surface | `RAB_Surface`, `RAB_GetGBufferSurface`, `RAB_IsSurfaceValid`, `RAB_GetSurfaceWorldPos`, `RAB_GetSurfaceNormal`, `RAB_GetSurfaceLinearDepth`, `RAB_GetMaterial`, `RAB_AreMaterialsSimilar` | E1 guides + depth, current/previous set; world position reconstructed from depth and `invViewProj` |
  | BRDF | `RAB_SurfaceEvaluateBrdfPdf`, `RAB_SurfaceImportanceSampleBrdf` | `pdfBrdf`, `sampleBrdf` |
  | lights | `RAB_LightInfo`, `RAB_LoadLightInfo`, `RAB_StoreCompactLightInfo`, `RAB_SamplePolymorphicLight`, `RAB_LightSample`, `RAB_IsAnalyticLightSample`, `RAB_LightSampleSolidAnglePdf`, `RAB_EvaluateLocalLightSourcePdf`, `RAB_TranslateLightIndex` | `evalLight`; triangle lights from M7 |
  | target pdf | `RAB_GetLightSampleTargetPdfForSurface` | `luminance(evalBrdf · L_i · NdotL)` |
  | visibility | `RAB_GetConservativeVisibility`, `RAB_GetTemporalConservativeVisibility` | `traceVisibility` |

  The bridge adds no BRDF or light math of its own. Implement exactly the `RAB_*` set the pinned SDK version requires.

- **As built:**
  - **Previous-frame surfaces.** The plan assumed depth gives last frame's surface, but the engine keeps one depth buffer, and the camera had no previous position. F2 adds three things:
    - a `Position` guide (RGBA32F: world position, w = view depth) in both guide sets, written by the resolve from its barycentric position;
    - `Motion.z` = previous view depth − current view depth (clip w), RTXDI's expected-depth offset; `Motion` becomes RGBA16F;
    - `GpuCameraData.prevCameraPos` in the unused `cameraParams` slot (size unchanged), equal to `cameraPos` on a cut like `prevViewProj`.

    The position guide was chosen over a depth copy plus previous inverse matrices. It gives ray origins the resolve's exact positions, with no reconstruction or jitter terms. It costs 16 B per pixel per set: ≈0.004 ms of resolve time at 1280×720 on the 5070 (0.1455 ms mean with it, 0.1416 ms without, three alternating rounds), and 29.5 MB for both sets.
  - **`lighting/rtxdi_bridge.slang`** (textual, before any `Rtxdi/DI` header):
    - `rtxdiBridgeInit(root)` fills a static per-thread state: guide sets, render size, both camera positions, TLAS, world lights, reservoir and neighbor-offset addresses, local light count.
    - `loadRtxdiConstants(root)` returns the F1 block.
    - `RTXDI_LIGHT_RESERVOIR_BUFFER` and `RTXDI_NEIGHBOR_OFFSETS_BUFFER` are `bdaPtr` expressions.
    - `RTXDI_ENABLE_PRESAMPLING` is 0 until M7.
  - **RAB set:** exactly what RTXDI 3.1's DI headers reference with presampling off.
    - Relative to the table above, it drops `RAB_StoreCompactLightInfo` (presampling only).
    - It adds `RAB_EmptySurface`, `RAB_EmptyLightInfo`, `RAB_EmptyLightSample`, `RAB_ClampSamplePositionIntoView`, `RAB_GetLightDirDistance`, `RAB_TraceRayForLocalLight`, `RAB_GetEnvironmentMapRandXYFromDir` and `RAB_EvaluateEnvironmentMapSamplingPdf`.
    - **Surface:** one guide-set pixel. Shading normal, geometric normal (oct), `cDiff`, `f0`, roughness and the view direction from that frame's camera. `isLit` = specular weight > 0: unlit surfaces have target pdf 0.
    - **Material similarity:** the RTXDI sample's thresholds (roughness 0.5 relative, `f0` and albedo luminance 0.25).
    - **Lights:** `RAB_LightInfo` is `GpuLight` (E2's world row). `RAB_LightSample` holds `evalLight`'s direction, distance and radiance for the surface it was sampled for; RTXDI always scores a sample against that same surface. Punctual lights are analytic (`RAB_IsAnalyticLightSample` true, solid-angle pdf 0), so RTXDI skips light/BRDF MIS for them.
    - **Shading:** `shadeLightSample(surface, sample)` = `evalBrdf · L_i · NdotL`; the target pdf is its luminance, and F3 shades with the same function.
    - **Visibility:** `traceVisibility` (D4 contract) from `rasterRayOrigin`. That is the spec's `offsetRay` reference along the geometric normal on the ray's side, scaled by `max(|p|, viewDepth) / 65536`. Raster surfaces have no traced triangle, so `safeSpawnPoint` does not apply. The temporal variant traces from last frame's surface through this frame's TLAS.
  - **Previous-frame light rows (2026-11-15, considerations C-F2-1):** `GpuFrameRoot.prevWorldLights` is the previous frame slot's `WorldLights` buffer, 0 on a history reset; `RAB_LoadLightInfo(i, true)` reads it, or this frame's row when it is 0. A light set change resets history, so both frames share their rows. Sync: the next frame's light pass waits on the ray timeline at the compute stage as well, and growing a slot's buffer waits for the last submitted ray passes. On `LightsPunctualLamp` (static lights) the convergence check reads the same numbers with and without it, while `rtxdi_temporal.spv` differs.
  - **Not built (listed in the bridge header):**
    - presampling (M7);
    - emissive triangles (M7): BRDF rays never name a light, and the local source pdf is uniform;
    - environment light: `lightPresent` stays 0.
  - **Constants mirror:** `lighting/rtxdi_constants.slang`, textual because RTXDI's parameter structs come from its C header. `layout_test` loads it as its own module through RTXDI's include path and checks every field against `GpuRtxdiConstants`.
  - **Warnings:** RTXDI's HLSL converts `uint` to `bool` implicitly (E30081). Units that include the bridge compile with `RTXDI_SLANG_FLAGS` (`-warnings-disable 30081`, a new `FLAGS` argument of `engine_add_slang_pass`). `#pragma warning` cannot scope it: Slang 2026.20 ignores it once a unit imports a module.
  - **Debug view:** `Shadowed lights` (Lighting) sums `shadeLightSample` × `RAB_GetConservativeVisibility` over every world light through the bridge. It is the noise-free punctual reference that F3's ReSTIR output must converge to. The guide capture writes it for every shot (9 views).
- **Checked (2026-11-14, RTX 5070):**
  - **RAB completeness:** a scratch unit included the bridge with `InitialSampling`, `SpatioTemporalResampling` and `ReservoirStorage`, and called `RTXDI_SampleLightsForSurface`, `RTXDI_DISpatioTemporalResampling` and `RTXDI_StoreDIReservoir`. It compiles with the engine's flags and passes `spirv-val`.
  - **Shading:** with visibility forced on, `Shadowed lights` reproduced E2's `Unshadowed lights`.
    - Identical on `DirectionalLight`, `UnlitTest` and `TextureTransformMultiTest`.
    - On the lamp, 20 of 58,206 pixels differ by 1/255, because the bridge's view direction comes from the position guide instead of reconstructed depth.
  - **Temporal surfaces:** a temporary motion-check counter applied RTXDI's own test (`RTXDI_IsValidNeighbor`, normal 0.5, depth 0.1) to the bridge's previous surface at `round(pixel + motion)`.
    - Static camera: 0 rejects. Camera and entity motion: 0.26–0.80% rejects per frame, the disocclusion and silhouette level.
    - Depth off by more than 1%: 0.28–0.63% with `motion.z`. With `motion.z` zeroed, 39% under camera translation and 15% under mirrored entity rotation.
  - **Ray origins:** a temporary ray check spawned its hemisphere ray from the bridge surface and `rasterRayOrigin` instead of `safeSpawnPoint`.
    - 0 self-hits over 227.5 M rays (cull check), 212.9 M (motion test) and 13.1 M (guide capture).
    - Without the offset, about 45% of interior pixels hit their own triangle.
  - **Shadows:**
    - On `LightsPunctualLamp`, the shade and bulb geometry shadow the stand and base.
    - `DirectionalLight` is black, and that is correct. Its spheres are wound inside out: all 10,600 triangles and 5,374 normals face inward, on single-sided materials. The raster therefore shows the inside of each far hemisphere, which the near hemisphere shadows from the sun. It cannot serve as an acne test.
  - Validation clean (core, sync, best practices + NVIDIA) on the guide capture with `ENGINE_RT_CHECK` and on the motion test.
  - Regressions: motion test 13 of 13, cull check 16 of 16, ray check every frame (294, 318 and 142 frames), guide normal checks 2 of 2, ctest 7 of 7 (the layout test now includes `GpuRtxdiConstants`).

#### F3 · Passes — built (2026-11-14), split for RTXDI 3.2 (2026-11-15)

Since the RTXDI 3.2 update the fused `rtxdi_shade` below is three passes: `rtxdi_initial`, `rtxdi_temporal` (`RTXDI_DITemporalResampling` + boiling filter) and `rtxdi_shade` (`RTXDI_DISpatialResampling` + final visibility + shading). The configuration, the convergence numbers and the guide capture's permanent convergence check are in [rtxdi_3_2_update.md](./features/rtxdi_3_2_update.md); the text below records the 3.1 build.

| Pass | Calls |
|---|---|
| `rtxdi_initial` CS | `RTXDI_SampleLocalLights` + `RTXDI_SampleInfiniteLights` + `RTXDI_SampleBrdf` → `RTXDI_CombineDIReservoirs` → `RTXDI_StoreDIReservoir` |
| `rtxdi_shade` CS | `RTXDI_DISpatioTemporalResampling` → final `traceVisibility` → `evalBrdf · L_i · NdotL · RTXDI_GetDIReservoirInvPdf` → `RTXDI_StoreDIReservoir` |

Output: `directRadiance` (RGBA16F, linear HDR, render res), a new `GpuTargets` entry.

- **Done when:** on `LightsPunctualLamp`, resampling mode `None` with high initial sample counts (the SDK's unbiased configuration, used as ground truth) matches `FusedSpatiotemporal` with default counts within noise, and the latter is temporally stable.
- **As built:**
  - **Passes** (`passes/rtxdi_initial.slang`, `passes/rtxdi_shade.slang`, 8×8 tiles) run first in the ray command buffer, after the TLAS wait and before the debug view (`Renderer::recordDirectLight`). A memory barrier precedes each: the first orders the resolve's guide writes and last frame's reservoir writes, the second the initial reservoirs.
    - `rtxdi_initial` calls `RTXDI_SampleLightsForSurface`, the library's composition of the local, infinite and BRDF samplers and their combine. It then traces one `RAB_GetConservativeVisibility` ray for the chosen sample (`enableInitialVisibility`): an occluded sample is dropped and its M kept. Every pixel stores its reservoir, empty pixels included.
    - `rtxdi_shade` loads this pixel's initial reservoir and runs `RTXDI_DISpatioTemporalResampling` against last frame's buffer with `Motion.xyz`, then the boiling filter. The filter synchronizes the group, so no thread returns before it.
    - It then evaluates the chosen light again at this surface (resampling may have taken a neighbor's), traces one final visibility ray and writes `shadeLightSample · W`. The reservoir is stored for the next frame.
    - A history reset (cut, resize, load) and the reference configuration shade the initial reservoir instead.
    - With no light loaded, `rtxdi_initial` is skipped and `rtxdi_shade` only writes zeros (a uniform early-out). A transfer clear drew `BestPractices-AMD-ClearAttachment-ClearImage-color`. Lights arrive only with a load, which resets history, so the skipped reservoirs are never reused.
  - **`directRadiance`:** RGBA16F in `RenderTargets`, `eGeneral`, reached through `GpuTargets.directRadianceStorage` / `directRadianceSampled` (`GpuTargets` 168 B). It holds no emissive term: that is the combine's (G6).
  - **Debug view:** `Lighting only` shows `directRadiance` + the emissive guide, the lighting output until G6. The guide capture writes it for every shot (10 views, 120 PNGs).
  - **Reservoir view (2026-11-15):** `RTXDI reservoirs` (Lighting) shows the reservoir `rtxdi_shade` stored: hue by light index, brightness by M against `maxHistoryLength` (spatial reuse can saturate it), black where no light was sampled. The guide capture writes it as an 11th view (132 PNGs). The extra view shifts the frame index at the convergence shot, so its numbers moved to bias +0.034%, tile p90 2.14%, per-frame 4.80%, frame-to-frame 1.50%.
  - **Configuration** (`RtxdiContext`). Each change from the library defaults was measured on `LightsPunctualLamp`:
    - **Pairwise MIS** instead of `Basic` bias correction. RTXDI 3.1's fused `Basic` path skips naive neighbors (`discountNaiveSamples`, M ≤ 2) after it has already marked them for its normalization, the temporal neighbor included. A skipped temporal reservoir is not merged, so its M stays at 1 and it is skipped again the next frame. The result was 35% darker than the reference. `Basic` without the discount is unbiased but flickers twice as much as Pairwise.
    - **Permutation sampling off.** It raised the frame-to-frame change from 0.8% to 20.5% of the signal on a static camera. It exists to decorrelate frames for denoisers, so H1 revisits it with DLSS-RR. RTXDI 3.1's `SetFrameIndex` rehashes only the temporal pass's `uniformRandomNumber`, so turning it on also needs `SetSpatioTemporalResamplingParameters` every frame.
    - **`numBrdfSamples` 0** until emissive triangles (M7). No BRDF ray can find a light before then, so each one was a wasted ray per pixel.
    - **`reuseFinalVisibility` off:** one final visibility ray per pixel (spec §11.2).
    - Every other parameter keeps the library default: 8 local and 1 infinite candidates, history 20, boiling filter 0.2, 1 spatial sample, 8 on disocclusion.
    - Superseded on 2026-11-15: the defaults are the RTXDI sample's `Medium` preset, and the measured configuration lives on as `RtxdiContext::convergedSettings`, which the convergence check holds ([rtxdi_3_2_update.md](./features/rtxdi_3_2_update.md) §3.3, §6.4; considerations C-R32-1).
  - **Reference** (ground truth): resampling `None` with 32 local and 8 infinite candidates. Select it with `ENGINE_RTXDI_REFERENCE=1` or the `ReSTIR DI reference` checkbox under `Lighting only`.
  - **Constants:** `GpuRtxdiConstants` gains `isFusedResampling` and `reservoirCount` (360 B).
  - **Bounds-checked reads in the bridge.** RTXDI reads its buffers as structured buffers under robust access, where out-of-range reads return zeros.
    - The fused pairwise path loads the reservoir at the back-projected pixel even when no temporal surface was found there, off screen included.
    - It also loads the light of an empty result: light 0, even when no lights are loaded.
    - Through BDA those reads lost the device. The reservoir buffer is now a bounds-checked `__subscript` accessor (zeros out of range, stores dropped), and `RAB_LoadLightInfo` returns the empty light past `lightCount`.
- **Checked (2026-11-14, RTX 5070):**
  - **Convergence.** A temporary capture of `LightsPunctualLamp` (1280×720, static camera) took 64 frames after 16 warm-up frames and compared them with `Shadowed lights`, the exact sum over every light with one shadow ray each. Errors are mean absolute luminance differences as a share of the reference signal:

    | | Bias of the 64-frame mean | 16×16 tiles: median / p90 / max | Per-frame error | Frame-to-frame change |
    |---|---|---|---|---|
    | fused (as built) | −0.001% | 0.52% / 1.72% / 4.33% | 4.68% | 0.77% |
    | reference | +0.005% | 0.13% / 0.55% / 4.32% | 10.72% | 11.41% |

    The fused and reference tile means agree within 0.42% (median) and 1.76% (p90). The fused pass has less than half the reference's per-frame error, and its frames change 15× less.
  - **Before the configuration changes** (same capture): `Basic` −35.2% bias; `Basic` without the discount −3.1%, or +0.05% with permutation off as well; Pairwise with permutation −0.5% bias and 20.5% frame-to-frame change.
  - **Cost.** On the lamp (about 57,000 lit pixels of 921,600): initial 0.05 ms, shade 0.08 ms; in the reference configuration, 0.06 ms each. On Sponza without lights, before the skip, the two passes cost 0.051 and 0.250 ms. That is a floor for a fully lit 1280×720 frame, since it has no light evaluation and no rays. With the skip, the two spans cost 0.011 and 0.013 ms and the frame total returns to 0.30 ms.
  - Validation clean (core, sync, best practices + NVIDIA) on the guide capture with `ENGINE_RT_CHECK`, on the guide capture in the reference configuration, on the motion test and on the cull check. GPU-assisted validation was clean on the guide capture before the no-light path, which adds only a uniform early write. With AMD best practices and performance warnings on as well, only the older warnings in [considerations.md](./considerations.md) remain.
  - Regressions: motion test 13 of 13, cull check 16 of 16, ray check every frame (154, 318 and 294 frames), guide normal checks 2 of 2, ctest 7 of 7. The layout test covers `GpuTargets` (168 B) and `GpuRtxdiConstants` (360 B).
- **Open (spec §11.2):**
  - ~~an RTXDI reservoir debug view (spec §14)~~: done 2026-11-15 (`RTXDI reservoirs`, above);
  - the environment light, "when present" in spec §11.2: no task and no source yet (glTF carries none). It needs RTXDI's environment presampling, so it fits M7 with `Power_RIS` (considerations C-F3-5).

### Phase G — Indirect: SER bounce, NRC, ReSTIR GI (M4)

Spec §11.1, §11.3–11.6.

#### G1 · SER RT pipeline

- **Feature spec:** [shader_execution_reordering.md](./features/shader_execution_reordering.md) (SER1–SER6). It refines the steps below: the hint is computed from `InstanceIndex()` before the invoke, and the invoke stays because `HitObject::GetClusterID` is NV-only while position fetch on the hit object is EXT-only.
- **Files:** `src/render/vk_rt_pipeline.{hpp,cpp}` (new), `shaders/raytracing/*`
- **Do:**
  - `VkPipeline` + SBT per shaded-ray pass: one raygen, one miss, one hit group (closest-hit only). Descriptor-heap pipeline flag as on the shader objects; `allowClusterAccelerationStructure` for cluster hits. Dispatch with `vkCmdTraceRaysIndirect2KHR`.
  - Raygen: `HitObject::TraceRay` → `ReorderThread(hit, materialBin, bits)` → invoke the minimal closest-hit that returns `{rayInstance, meshletRow, triangle, barycentrics, hitT}` (skip the invoke if Slang exposes `ClusterID` and attributes on `HitObject`, spec Q8) → shade in raygen with `decodeHit` → `sampleMaterial` → BRDF. Until M7 the hint is the material's shader id; misses get their own bin.
  - `rayTracingInvocationReorderReorderingHint` is already logged at startup (A1).
- **Done when:** a debug raygen reproduces the C3 debug view; reorder on/off timings recorded on the 5070.

#### G2 · Indirect pass

- One ray per pixel from the primary surface (guides + depth), direction from `sampleBrdf` (VNDF), origin offset along `GeoNormal` plus the CLAS truncation bound (spec §10.2). Floor tier: half resolution, checkerboard.
- At X₂: `decodeHit` → `sampleMaterial` → direct light via `RTXDI_SampleLocalLights` / `RTXDI_SampleInfiniteLights` without reuse + one `traceVisibility` → add emissive → query the cache for bounces 2…∞, weighted by the primary `ao` (§2 #10).
- Writes the ReSTIR GI initial sample (hit position, normal, radiance). With NRC the cache radiance is not known yet: the pass writes the query record and G3's resolve completes the sample.

#### G3 · NRC (M4)

SDK: NVIDIA RTX Neural Radiance Cache (Vulkan host library + HLSL headers), pinned; prebuilt DLLs (NRC + CUDA runtime) copied next to the executable. Verify every SDK name against the pinned headers.

- **Device:** enable `VK_NV_cooperative_vector` and merge NRC's required extensions and features, next to H1's NGX set. They are required: device creation throws when one is missing, with no SHaRC fallback.
- **Context:** `nrc::vulkan::Context` after device init; `Configure` (render resolution, `kMaxBounces`, training budget) at init and on resize. The SDK allocates its buffers; the engine gives each a heap slot or BDA and never resizes them.
- **Per frame:** `BeginFrame` (scene AABB from §9, training on) → `NrcConstants` into the per-slot block → update paths (`NRC_UPDATE`) + G2 as the query pass (`NRC_QUERY`) → `QueryAndTrain` (inference, then one training step) → **custom resolve** writing `throughput × cachedRadiance` into the ReSTIR GI samples (NRC guide step 7), not the SDK's combined-signal resolve → `EndFrame` after submit.
- **Update paths** are shaded rays: they run in the G1 SER pipeline, reordering at every bounce (spec §11.1: the frame's most incoherent rays).
- **Debug view:** the cache's radiance at the primary surface (spec §14, "cache debug resolve").
- **`gi/nrc_bridge.slang`** matches the SDK's HLSL buffer packing with an explicit Slang layout, never by editing vendor headers.
- Constants per frame slot through `GpuFrameRoot.cacheConstants`.
- **Done when:** both tiers run NRC; training loss falls after load; the cache debug view shows cached radiance; an interior lit only by a sun is not black; the cache row of spec §2.3 is measured on the 2060 Super (spec R4).

#### G5 · ReSTIR GI (M4)

- RTXDI ReSTIR GI on the G2 samples: temporal then spatial reuse, boiling filter on, reconnection visibility through `traceVisibility`. Output `indirectRadiance` (RGBA16F, render res).
- **Done when:** `Sponza` with a sun shows bounce light under arches, stable after DLSS-RR, and converges to the ground truth of spec §14: the same path code accumulated over many frames with the cache off and a high bounce count, on a static frame.

#### G6 · Combine

- `combine` CS: `noisyColor = directRadiance + indirectRadiance + emissive` (RGBA16F). The only place the three terms meet.
- **Must not:** apply AO to direct, add a second path loop, or touch SDK buffers outside their bridge files and host modules.

### Phase H — DLSS Ray Reconstruction + post (M3)

SDK: `NVIDIA/DLSS` (NGX, Vulkan), fetched and pinned. Required: unsupported RR at init throws `std::runtime_error`.

#### H1 · NGX

- **Device:** merge NGX's required instance/device extensions into `Device`, with NRC's set (G3). `NVSDK_NGX_VULKAN_Init` once.
- **Create:** `NGX_VULKAN_CREATE_DLSSD_EXT1` with HW depth, packed roughness, HDR input. Render resolution from the RR optimal-settings query for Quality (spec §2.1); the render extent that `Renderer::rebuildSwapchainResources` passes to `RenderTargets::recreate` and `RtxdiContext::recreate` follows it (`ResourceManager` was split, spec AU2), and the display image moves to display resolution. Recreate on resize.
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
- **As built (2026-11-14):**
  - `Dlss` (`src/render/vk_dlss.*`), owned by `Renderer`, is the only NGX user. `Dlss::requiredExtensions()` hands NGX's instance and device extension lists to `Device` through a new `SdkExtensions` constructor argument. A missing one fails device creation. `VK_KHR_push_descriptor`, promoted to 1.4, also sets `Vulkan14Features::pushDescriptor`.
  - The constructor runs `NVSDK_NGX_VULKAN_Init_with_ProjectID` with a custom project id. It throws when RR is unavailable or the driver needs an update. NGX's log goes to the engine log under `NGX`.
  - `recreate(display)` runs on every swapchain rebuild. It asks the RR optimal-settings callback for Quality (the D3D-only helper is not used) and creates the feature with `DLUnified`, packed roughness, HW depth and `IsHDR | MVLowRes | DepthInverted | AutoExposure` on a one-time compute submit. It returns the render extent, which `RenderTargets`, `HiZ`, `RtxdiContext` and the camera take. 1280×720 renders at 853×480.
  - `RenderTargets` gains `noisyColor` (render resolution) and the RR output (display resolution, RGBA16F), both in `eGeneral`, with image views for NGX. The new `combine` pass writes `noisyColor = directRadiance + emissive`. `present` samples `GpuTargets::presentSampled`: the RR output for Lit, the display image for a debug view.
  - In the ray command buffer, after ReSTIR DI: combine, then (Lit only) a barrier, the evaluation and a heap rebind. The barrier also orders NGX's fills of its own buffers against the previous evaluation. `jitterPixels = jitterOffset · render / 2`, the same sign as the raster offset. Matrices are transposed to row-major. History resets on the frame's reset, a camera cut, a layout change, a resize, or a frame without Lit. The Lit display capture copies the RR output.
  - With no light loaded, Lit would be black, so `Renderer::isDlssPresented()` is false: the evaluation is skipped and the debug view pass shows the unlit albedo, as before H1. The default `Sponza` has no lights. The stats overlay says so.
  - Only frames DLSS-RR evaluates are jittered, since a debug view or the unlit fallback below DLAA boiled when scaled up (considerations C-H1-7). `ENGINE_JITTER=1` jitters every frame.
  - `Dlss::evaluate` binds an empty legacy descriptor set first, so NGX's image view handles index the driver's heap and not the engine's (considerations C-H1-1).
  - Linking NGX into the mimalloc-wrapped module needed `__imp___wrap_*` slots (C-H1-3).
  - The Ray tracing panel's DLSS section shows render → display resolution and the jitter phase count. It sets the mode (DLAA, Quality, Balanced, Performance, Ultra Performance; Quality by default) and the RR preset hint (Default, D, E, F), and can reset history. A changed mode or preset makes `Renderer::drawFrame` wait for the device and rebuild the swapchain-sized resources. Every mode was switched at runtime under validation; render sizes at 1280×720 are 1280×720, 853×480, 742×418, 640×360 and 427×240.
- **Checked (RTX 5070, driver 621.01):**
  - The motion test passes 13 of 13 and the cull check 16 of 16. The guide capture passes 2 of 2 normal checks. The ray check matches every frame under all three. ctest passes 7 of 7.
  - Validation (sync, best practices, NVIDIA checks, GPU-AV off) is clean over the bench, and over the motion test's resizes and loads with jitter on, apart from NGX's own messages (C-H1-2).
  - Jitter sign, measured on `Sponza` with `LightsPunctualLamp`: the chosen sign changes 0.000030 per frame on a still camera, against 0.00064 for the opposite sign and 0.000025 with jitter off. The done-when condition on a lit `Sponza` stays open (C-H1-5).
  - Bench, 6000 frames at 1280×720 display: DLSS-RR 1.14 ms (best window 1.13 ms), combine 0.005 ms. GPU total 1.38 ms.

#### H2 · Post + present

- `post` CS: auto exposure from a luminance histogram → tonemap (ACES fitted or AgX; one curve, applied once) → the display image. The existing `present` pass then draws it into the sRGB swapchain with ImGui (the swapchain has no storage support). `exposure` goes to `GpuFrameRoot` and DLSS pre-exposure.
- Until H1 lands, `post` reads `noisyColor` at render resolution. This is bring-up order, not a shipped path.
- `debug_view` keeps writing the display image when a debug view is selected; `Lit` becomes the post output.
- **As built (2026-11-14):**
  - `PostProcess` (`src/render/vk_post.*`), owned by `Renderer`, records three compute passes after the DLSS-RR evaluation and its heap rebind, for the Lit view only:
    - `exposureHistogramMain` (16×16 groups): a 256-bin log2-luminance histogram of the RR output over 2^−16 … 2^16, into a host-visible buffer. Black pixels land in bin 0 and are not metered.
    - `exposureAdaptMain` (one group): reads and clears the bins, averages log2 luminance between the 40th and 95th percentile, and aims it at 0.18 plus the compensation, clamped to the EV range. It adapts in log space by `1 − e^(−dt·rate)` and writes the 1×1 R32F exposure image and a per-slot readback (`GpuExposureReadback`). Manual exposure, a snap request and a resize jump straight to the target; a frame with no lit pixel keeps the exposure.
    - `postMain` (8×8): RR output × exposure → ACES (Stephen Hill's fit of the RRT + sRGB ODT) → the display-resolution RGBA16F post target. It is the only tonemap.
  - Deviations from the plan above: the exposure lives in an image, not `GpuFrameRoot`, so the GPU computes it without a readback. DLSS-RR takes that image as `pInExposureTexture`, created without `AutoExposure`, and pre-exposure stays 1 (C-H2-1). DLSS-RR reads the exposure post wrote in the previous frame (C-H2-2). Post writes its own target, not the display image, which the debug views keep at render resolution.
  - `GpuTargets` (200 B) gains `upscaledSampled`, `postStorage`, `exposureStorage` and `displaySize`; `presentSampled` points at the post target for Lit. The Lit display capture copies the post target.
  - The Post panel shows exposure, EV, target and the metered luminance. It sets auto or manual exposure, compensation, adaptation rate and the EV range, and can snap. A `Post` GPU timer scope follows `DLSS-RR`.
- **Checked (RTX 5070, driver 621.01):**
  - The motion test passes 13 of 13 and the cull check 16 of 16. The guide capture passes 2 of 2 normal checks. The ray check matches every frame under all three. ctest passes 7 of 7.
  - Validation (sync, best practices, NVIDIA checks, GPU-AV off) over a 600-frame bench with `LightsPunctualLamp` beside `Sponza` (post running), and over the motion test with jitter on, shows only NGX's messages (C-H1-2).
  - On `Sponza` + `LightsPunctualLamp`, the exposure falls from 2.64 at frame 20 to 1.39 at frame 200 toward a target near 1.2, with about 70 000 lit pixels metered. The post output tops out at 0.97, so nothing clips.
  - Bench, 6000 frames at 1280×720 display with the lamp: post 0.030 ms (best window 0.028 ms), DLSS-RR 1.18 ms. GPU total 1.69 ms.

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
| `TwoSidedPlane` / `UnlitTest` | winding flip / unlit (guides checked 2026-11-13, E1) |
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
A1 + A4 → C1 → C2 → C3 ; C2 → C4
B2 + B3 + C3 + E1(ao, jitter) → E2 → F1 → F2 → F3 ; E2 → E3
F3 → H2 (bring-up) ; E1 + F3 → H1 → H2
F3 + C3 → G1 → G2 → G3 → G5 → G6 → H1 input switch
A4 + C1 → D1 → D2 ; D1 → D3 ; D2 → D4
```

| Milestone (spec §16.2) | Tasks |
|---|---|
| M0 Contracts | A1, A2, A3, A4 (**done**; A4 checked by `ENGINE_GUIDE_CAPTURE`), animation removal (**done**) |
| M1 Vis buffer | B1, E1 (**done**; E1 guides vs Khronos screenshots and motion checked 2026-11-13); 32-bit payload kept, V8 deferred; culling lossless under reverse Z (`ENGINE_CULL_CHECK`, 2026-11-13); **M1 done** (spec §16.2) |
| M2 Cluster RT bring-up | C1, C2, C3 (**built**; C3's `ENGINE_RT_CHECK` closes C1's trace check), C4 PTLAS (**built** 2026-11-14 after M2 was reopened). Open: the async overlap capture and the 2060 Super |
| M3 First lit frame | B1 (**done**), B2 (**done**), B3 (**done**), E1 (**done**: AO, jitter, unlit specular, `lodBias`), E2 (**done**: punctual; emissive triangles M7), F1 (**done**), F2 (**done**), F3 (**done**), H1 (**done**), H2 (**done**), E3 sun (**done**); **M3 done** (2026-11-14) |
| M4 Indirect | G1, G2, G3 (NRC), G5, G6 |
| M5 | merged into M4 (2026-11-14) |
| M6 Alpha | D1–D4 |
| M7 Material binning | E2 emissive triangles, F1 `Power_RIS`, resolve per bin (spec §9.1–9.2) |

PR order follows the milestones. The **first lit pixel** is F3 through the H2 bring-up path.

---

## 7. Host (C++) layout

| Unit | Owns |
|---|---|
| `vk_render_targets` (exists) | vis buffer, display image, guide sets, `GpuTargets` per slot; gains `directRadiance`, `indirectRadiance`, `noisyColor` |
| `vk_mega_geometry` (exists, `src/core`) | `MegaGeometry`: CLAS and cluster BLAS groups per model (C1) |
| `vk_rt_scene` (exists) | `RayTracingScene`: `GpuRayInstance` tables, instance-write pass, async-compute submit (C2); one PTLAS updated in place, its state rows and scratch, per-slot write-instance lists and ops (C4). The compute submit signals `FrameSync::computeTimeline` and waits on `FrameSync::rayTimeline` |
| `vk_world_lights` (exists) | per-slot world light buffer and the `light_world` pass (E2) |
| `omm_baker` | OMM SDK bake + cache, SDK handle and `VkMicromapEXT` RAII |
| `vk_rt_pipeline` | SER RT pipelines and SBTs, `traceRaysIndirect2` dispatch |
| `vk_pbr` (new; the empty files were deleted with spec AU7) | `combine` and `post`; the resolve stays in `Renderer` until M7 moves it to binned dispatch |
| `vk_rtxdi` (exists) | `ReSTIRDIContext` + ReSTIR GI, reservoirs, neighbor offsets, constants, `rtxdi_initial` / `rtxdi_shade` / `restir_gi` |
| `vk_nrc` (`nrc_library` exists for the linkage test) | NRC context, SDK buffer bindings, `NrcConstants` per slot, update / query / custom resolve, `QueryAndTrain` |
| `vk_dlss` | NGX init, RR feature, evaluate |

Rules: `MaterialStore` / `LightStore` stay on `Scene`. `AssetBuffers::flush` remains the only asset upload site. Every AS, micromap, RT pipeline and SDK object is owned by a move-only RAII type (destroy the object, then its buffer). Build or SDK failure throws `std::runtime_error` naming the failing call. NVIDIA-only SDKs stay inside their module (§2 #0). Every pass gets a `GpuTimerScope` and a Tracy zone with its spec §2.3 budget. All other C++ rules: §2 #14.

---

## 8. Must-not checklist

| Area | Never |
|---|---|
| lighting | any light, GI or denoise path outside RTXDI / ReSTIR GI / NRC / DLSS-RR. A second renderer or reference loop (ground truth is RTXDI's `None` mode and the accumulation mode of the same path code, spec §14), a homegrown cache or MLP, SVGF, or TAA. Raster lighting (shadow maps, SSAO, SSR) |
| hardware | AMD/Intel code paths, vendor checks or fallbacks. NVIDIA SDK calls outside their module (`vk_nrc`, `vk_dlss`, `vk_mega_geometry`) |
| shading | a second BRDF. Perceptual roughness in `D`. Double or missing `NdotL`. AO on direct. Emissive × `NdotL`. sRGB on metal-rough/normal/AO. Premultiplied alpha before the BRDF |
| lights | `dot(spotDir, L)`. Node scale on intensity/range/cone. A CPU loop over lights per frame |
| visibility | alpha tests or material fetches in the opaque raster bin. Alpha tests in the resolve. A fragment G-buffer |
| rays | any-hit. Candidate loops in ray queries. KHR triangle BLASes. CPU-written TLAS instances or PTLAS ops. A per-frame full TLAS rebuild. A ray without the OMM 2-state flag. Camera culling on TLAS instances. Shading hits outside the SER pipeline shape |
| OMM | OMM on opaque primitives. Baking from any triangle order other than the final cluster triangles. Baking before A4. A second baker next to the OMM SDK |
| geometry | homegrown tangent, normal, cluster or OMM code (§2 #16). Overwriting file-provided tangents or normals. Animation |
| Slang | implicit-LOD `Sample` outside fragment shaders. Material fetches bypassing `sampleSlot`. Preprocessor variants beyond vendor-mandated defines |
| SDKs | NGX calls from Slang. Unpinned SDK versions. Optional or disabled SDK builds |

---

## 9. Open choices

Open problems, accepted limitations and current workarounds, stamped by milestone and step: [considerations.md](./considerations.md).

| Choice | Default |
|---|---|
| RTXDI local light sampling | `Uniform` for punctual-only scenes; `Power_RIS` once emissive triangles are lights (M7) |
| Indirect resolution | full on the reference tier, half + checkerboard on the floor tier |
| CLAS `positionTruncateBitCount` | 0; raise only with measured memory gain and the §10.2 origin rule. RTXMG ships 7, which measured −22% CLAS on Sponza (4.72 → 3.66 MiB); not worth the origin bound at this scale (feature spec §9.4, R6) |
| `Blend` | treated as mask (cutoff 0.5) |
| OMM subdivision level | max 5, clamped to device max; adaptive via `dynamicSubdivisionScale` (start at 2 texels, the SDK default) |
| OMM baker | NVIDIA OMM SDK CPU baker (D1), at load and cached: the SDK's choice for pre-baked content, with reuse and spatial-sort steps the GPU baker lacks. Its GPU baker only if load-time baking is too slow on Bistro; it needs the SDK's shaders and a binding adapter for descriptor heaps |
| Cache scene AABB / `kMaxBounces` | `GeometryStore` world bounds, recomputed on load / 4 |
| NRC training budget | SDK default for the render resolution |
| DLSS quality mode | Quality on every tier (spec §2.1) |
| Vis buffer payload | 32-bit; 64-bit only if V8 measurements favor it (spec Q3) |

---

Appendices are history, newest first. The plan body above is the current state. Rows a later revision reversed are marked: in the **Now** column of the two oldest tables, inline elsewhere.

## Appendix — changes on 2026-11-14

| Removed | Replaced by | Why |
|---|---|---|
| §2 #0: NVIDIA now, AMD a later target; vendor-neutral choices preferred | NVIDIA RTX only; NVIDIA-only pieces are the target stack | the target is NVIDIA only |
| C2: PTLAS only if the TLAS build measures over 0.3 ms (M9 trigger) | PTLAS required: C4 reopens M2; OMM 2-state moves from an instance flag to a ray flag (§2 #6) | the full Mega Geometry stack is the target |
| §2 #11, G3–G4: SHaRC (floor, M4) + NRC (reference, M5) behind `IRadianceCache` | NRC only on every tier (G3, M4); no cache interface; M5 merged into M4 | one cache once AMD is out; NRC handles glossy |
| — | §2 #11's floor-tier cost moves to spec R4: NRC on the 2060 Super is unmeasured | no cheaper cache remains |

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
| §2 #11, G: NRC only | SHaRC + NRC behind `IRadianceCache`, plus ReSTIR GI and the one-bounce indirect pass (G1–G6) | spec §11.3–11.5. **Cache part reversed 2026-11-14:** NRC only |
| §2 #12, §9: `Uniform` only, emissive meshes out | `Power_RIS` with emissive-triangle lights in M7 | spec §11.2 |
| E2: CPU transforms light rows | GPU light-buffer pass | spec P2 |
| A3: delete RT pipeline, SER, cluster AS and their stubs | keep them; delete only `postprocessing/gbuffer.slang`, `evalLambert` | spec §17 |
| A4 morph-delta remap | dropped: animation is removed first | spec §7.3 |
| §4.3 `SampleLevel(…, 0)` outside fragment; B1 "no `KHR_texture_transform`" | `SampleGrad` with analytic or ray-cone gradients, LOD bias; `KHR_texture_transform` and emissive strength in B1 | spec S10, §9.4, §12.2 |
| §4.4 `sampleOr` | `sampleSlot` (as built), re-probe on Slang v2026.20 | code |
| §2 #0: NVIDIA only, no AMD | NVIDIA now; NVIDIA-only pieces behind modules for a later AMD port | spec §1.2, P-AMD. **Reversed 2026-11-14:** NVIDIA only |
| §3 state on `fd67395d1` | re-verified on `a50ebcb46` + vis buffer: 136 B frame root, `GpuTargets`, guides, B1 done | the tree moved on |
| H2 composite → swapchain | `post` → display image → existing `present` pass (sRGB swapchain has no storage) | code |

## Appendix — changes vs the 2026-10-23 revision

| Removed | Replaced by | Why |
|---|---|---|
| §3 state from `fa2222941` (80 B frame root, 24 B push data, one draw per entity × primitive, model-relative `materialId`, lights dropped after flush) | §3 re-verified on `fd67395d1`: shader objects, shared refcounted models and materials, mesh/node split with `GpuNodeInstance`, 16 B push data + `GpuDrawRecord`, debug views, per-entity lights | the tree moved on; the plan's ids and layouts must match it |
| `GpuRayInstance { instanceIndex, primitiveIndex }`, TLAS instance per entity × primitive, BLAS group destroyed by `eraseModel` | `{ nodeInstanceIndex, primitiveIndex }`, TLAS instance per node instance × mesh primitive with the node world matrix, BLAS group lives while the model's `refCount` > 0 | meshes are stored once and placed by nodes; models are shared by entities |
| `GpuFrameRoot` 96 B with `debugFlags` | 120 B: existing `nodeInstances`, `drawRecords`, counts and `debugView` + new `rayInstances`, `frameIndex`, `exposure` | `debugView` already exists and lighting debug modes are `DebugView` entries |
| meshoptimizer `meshopt_opacityMap*` baker (D1) | NVIDIA OMM SDK CPU baker, one bake per primitive (decided 2026-11-08) | contour-based resampling maximizes known states; reuse passes and spatial sort; cutoff, sampler and unknown-state promotion are bake inputs, so no alpha remap. The `omm` fetch stays |
| "Normal map only when a tangent exists"; no rule for missing normals | A4: MikkTSpace-compatible `meshopt_generateTangents` where a normal texture is used, flat `meshopt_generateNormals` where `NORMAL` is missing | glTF requires both; normal maps on tangent-less assets would otherwise be ignored |
| E2 "keep light rows after flush" | done (`LightStore::uploaded`, `loadWorldLight`); E2 is only the per-frame RTXDI light buffer | lights became per-entity rows with the shared-model work |
| — | C1 keeps `Indices` uploaded (overrides data-layout finding 5) | the index slice is the BLAS input |

## Appendix — changes vs the 2026-10-04 revision

| Removed | Replaced by | Why | Now |
|---|---|---|---|
| RTXGI SHaRC (hash grid, `sharc_update` / `sharc_resolve` / `sharc_query`, 64-bit atomics) | RTXGI NRC (G1–G3): `nrc_update` / `nrc_query`, host `QueryAndTrain` + `Resolve`, `noisy_color` CS | NRC chosen as the GI cache. Accepts the costs the previous revision avoided: prebuilt NRC + CUDA runtime DLLs and HLSL buffer-layout matching in the bridge | holds: SHaRC was added back beside NRC on 2026-10-25 and dropped again on 2026-11-14 (§2 #11) |
| Implicit NVIDIA-only targeting | §2 #0: NVIDIA RTX only, AMD/Intel out of scope for now | stated explicitly so no one adds vendor fallbacks | holds: NVIDIA only (§2 #0) |
| §3 state from `44055d48b`; draft `FrameRoot` with global vertex/index pointers; `GpuPrimitive` with flags; `instanceCustomIndex` = primitive | re-verified §3; §4.2 extends the existing `GpuFrameRoot`; `GpuRayInstance` table (§2 #5); BLAS from model slices, grouped per model (C1) | the code now has model slices, model-relative indices and unload compaction, so no GPU row may store an absolute entity/primitive id | BLAS from model slices reversed: cluster BLAS from CLAS (C1, built) |
| `GpuLight.entityId` | CPU-kept light rows (E2) | entity ids shift on unload; GPU light rows are byte-compacted | holds |
| "engine error type"; short C++ style row | `std::runtime_error`; §2 #14–15 from the `cpp-guidelines` / `main-guidelines` skills | no engine exception type exists | holds |

## Appendix — changes vs the 2026-09-29 revision

Most of these removals were reversed by the 2026-10-25 revision. Mega Geometry, the SER pipeline and NRC are core targets.

| Removed | Replaced by | Why | Now |
|---|---|---|---|
| RTX Mega Geometry (CLAS, cluster-BLAS, PTLAS, `cluster_prepare`, C5) | KHR BLAS per primitive + per-frame TLAS (C1–C2) | not part of the required stack; standard AS reuses existing buffers with no gather pass | **reversed:** CLAS per cluster + cluster BLAS (C1, built in M2); PTLAS required since 2026-11-14 (C4) |
| RT pipeline primary pass, SBT, any-hit, SER | raster G-buffer (E1) + ray queries (D3) | primary visibility already exists in raster; no shader tables or payloads | **reversed:** SER RT pipeline for shaded rays (G1). Primary visibility stays raster (vis buffer); no any-hit |
| Primary-ray alpha test + `RAY_FLAG_FORCE_OMM_2_STATE` workaround | raster `discard` + per-instance 2-state OMM | no any-hit and no Slang capability patch | holds, with OMM per cluster (D1–D4) |
| `DirectMode::Reference` light loop (E3) | RTXDI resampling mode `None` as ground truth (F3) | no custom lighting code | holds (F3, built) |
| NRC, `IRadianceCache`, `GI_CACHE` switch | SHaRC only (G) | one cache, no CUDA DLLs, no DX-layout constraints | **reversed:** NRC is the only cache (G3, M4) |
| GPU scene prep, DGC rewrite, PTLAS args, GPU culling (F1–F3) | CPU-written TLAS instances and lights (C2, E2) | unrelated to lighting; counts are small | **reversed:** GPU culling and DGC built; GPU-written TLAS instances (C2, built); GPU light pass (E2) |
| RTXDI presample pass + light PDF texture | `Uniform` local light sampling (§9) | glTF scenes carry few punctual lights | **reversed:** `Power_RIS` with emissive triangles in M7 |
| Raw-HDR fallback when RR is unavailable | hard requirement | single path | holds |
