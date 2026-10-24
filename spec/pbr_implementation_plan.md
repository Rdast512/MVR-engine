---
name: pbr-implementation-plan
description: Task plan for glTF metallic-roughness PBR built only on NVIDIA RTX SDKs (NVIDIA RTX hardware only, no AMD target) — RTXDI (ReSTIR DI) for direct light, RTXGI NRC (Neural Radiance Cache) for indirect light, opacity micromaps for alpha-tested rays, DLSS Ray Reconstruction for denoise and upscale. Raster G-buffer, KHR BLAS/TLAS, ray queries in compute. Use when implementing BRDF, lights, FrameRoot, G-buffer, BLAS/TLAS, OMM, RTXDI, NRC, DLSS-RR, or vk_pbr.
---

# Plan: PBR on RTXDI + RTXGI NRC + OMM + DLSS-RR

**Date:** 2026-10-23 · **Verified against:** working tree on `fa2222941`, Slang `2026.14.1`, Vulkan SDK `1.4.357.0`
**Status:** Phase A is partly in place (frame root, model table, `GpuMaterial` padding); no lighting code exists. See §3.

| Doc | Role |
|---|---|
| [pbr_lighting_materials.md](./pbr_lighting_materials.md) | Shading rules (normative for agents) |
| [pbr_lighting_materials.html](./pbr_lighting_materials.html) | Human explainer with live graphs and scenes |
| [gltf_parse_resources.md](./gltf_parse_resources.md) §5, §13.3, §14 | Normative glTF math |
| [gltf_storage_plan.md](./gltf_storage_plan.md) | CPU/GPU storage (done) |
| [../docs/resource_architecture.md](../docs/resource_architecture.md) | Current buffers, model slices, frame root, BDA + heap handle rules |
| [../docs/shader_architecture_plan.md](../docs/shader_architecture_plan.md) | Shader file map and pass order (follows this plan) |
| [VK_EXT_opacity_micromap.md](./VkExtensionsSpecs/VK_EXT_opacity_micromap.md) | OMM extension notes |

---

## 0. The short version

Four SDKs own all lighting. The engine writes only the glue they require.

| Concern | Owner | Engine writes |
|---|---|---|
| Direct light | **RTXDI** ReSTIR DI | `RAB_*` bridge, light buffer, two CS entry points |
| Indirect light | **RTXGI NRC** (Neural Radiance Cache) | one path loop (update + query variants), one combine CS, host train/infer/resolve calls |
| Alpha-tested rays | **OMM SDK** + `VK_EXT_opacity_micromap` | load-time bake, micromap build, attach to BLAS |
| Denoise + upscale | **DLSS Ray Reconstruction** (NGX) | guide images, host evaluate |

Everything else is the minimum those SDKs need:

1. **One BRDF, one light model.** glTF Appendix B GGX + `KHR_lights_punctual`, written once in Slang. RTXDI and NRC call it through their bridges. There is no reference light loop, no homegrown cache, no custom denoiser.
2. **Primary visibility is raster.** The existing mesh shader + DGC path writes a G-buffer. Alpha test is an exact `discard`. RTXDI, NRC and DLSS-RR all read this one surface.
3. **Rays are ray queries in compute.** Standard KHR BLAS (one per primitive) and one TLAS per frame. No RT pipeline, no SBT, no any-hit, no SER.
4. **Alpha on rays is OMM 2-state.** Every alpha-tested instance forces 2-state micromaps, so traversal never yields a candidate.

---

## 1. Frame at a glance

```
load (once per asset)
  OMM bake (OMM SDK, CPU)   MASK/BLEND primitives → micromap arrays + per-tri indices   [D1]
  vkCmdBuildMicromapsEXT → BLAS build (one per primitive)                              [D2 C1]

every frame
  TLAS build (CPU-written instances, full rebuild)                                     [C2]
  gbuffer (mesh + frag, DGC)  jittered raster → G-buffer + motion + DLSS guides        [E1]
  rtxdi_initial (CS + RQ)     local + infinite + BRDF candidates → reservoirs           [F3]
  rtxdi_shade (CS + RQ)       fused spatiotemporal resampling → final visibility → direct [F3]
  nrc_update (CS + RQ)        sparse training paths, NRC_UPDATE                         [G2]
  nrc_query (CS + RQ)         1 path per pixel → query records, NRC_QUERY               [G2]
  NRC QueryAndTrain (SDK)     inference on query records + one training step            [G3]
  NRC Resolve (SDK)           throughput × cached radiance → indirect                   [G3]
  noisy_color (CS)            direct + indirect + emissive → noisy color                [G3]
  DLSS-RR (host NGX)          noisy HDR + guides → display-res HDR                     [H1]
  composite (CS)              exposure + tonemap → swapchain                           [H2]
```

Every ray-query pass traces the TLAS address in `GpuFrameRoot.tlas`.

---

## 2. Locked decisions

| # | Decision |
|---|---|
| 0 | **Target hardware:** NVIDIA RTX GPUs only. AMD (and Intel) compatibility is out of scope for now: no vendor checks, no fallbacks, no workarounds for non-NVIDIA drivers. Every SDK below is NVIDIA-only anyway |
| 1 | **Lighting stack:** RTXDI (direct), RTXGI NRC (indirect), OMM (alpha on rays), DLSS-RR (denoise + upscale). All four are required. There are no fallbacks, compile-time toggles, or alternative paths |
| 2 | **Primary visibility:** raster G-buffer via the existing mesh shader + DGC pipeline, at DLSS render resolution, 1× sample count |
| 3 | **Rays:** `VK_KHR_ray_query` in compute only. `VK_KHR_acceleration_structure` BLAS/TLAS. No RT pipeline, no SBT, no SER, no NV cluster/partitioned AS |
| 4 | **BLAS granularity:** one BLAS per glTF primitive, one triangle geometry, built from its model's vertex slice (stride 32) and index slice. A built BLAS no longer reads its inputs, so unload compaction moving other models' slices does not invalidate it |
| 5 | **IDs on a hit:** `CommittedInstanceIndex()` → `GpuRayInstance { instanceIndex, primitiveIndex }`, a per-frame table written next to the TLAS instances. `CommittedPrimitiveIndex()` = triangle within the primitive. Nothing persistent stores an entity or primitive row, because both shift on unload |
| 6 | **Alpha on rays:** OMM with `VK_GEOMETRY_INSTANCE_FORCE_OPACITY_MICROMAP_2_STATE_EXT` on every alpha-tested instance. Every other instance is `FORCE_OPAQUE`. Ray queries therefore never see candidates |
| 7 | **Bind model:** descriptor heaps for images and samplers, BDA typed pointers for engine buffers. SDK-declared resources (`RWStructuredBuffer`, `RWTexture2D`, …) get heap slots in their bridge files |
| 8 | **Material table:** `MaterialStore` on `Scene`. `vk_pbr` hosts passes and does not own a second table |
| 9 | **Default material:** metallic 1, roughness 1, white. It renders dark without GI, which is correct |
| 10 | **AO:** sampled into the G-buffer, applied only to indirect at the primary vertex. Never on direct |
| 11 | **GI cache:** NRC only. SHaRC is out of scope |
| 12 | **RTXDI local light sampling:** `ReSTIRDI_LocalLightSamplingMode::Uniform` in v1 (no PDF texture, no presample pass). Resampling: `FusedSpatiotemporal` |
| 13 | **Scene data per frame:** CPU writes TLAS instances, `GpuRayInstance` rows, RTXDI light rows and `GpuFrameRoot`. Light and instance counts are small |
| 14 | **C++ style** (`cpp-guidelines` skill, `.clang-tidy`): PascalCase types and `enum class` enumerators; camelCase functions, members, locals; class constants `kPascalCase`; getters `getX` / `isX` / `hasX`; no `m_` or trailing `_`. RAII owners of AS, micromaps and SDK objects are move-only (Rule of Five); everything else follows the Rule of Zero. `const`/`constexpr` by default, `explicit` single-argument constructors, `[[nodiscard]]` on getters, named constants instead of magic numbers. Failures throw `std::runtime_error` with context, as the rest of the engine does. `.hpp` + `#pragma once`, no `using namespace` in headers |
| 15 | **Comments** (`main-guidelines` skill), in C++ and Slang: lowercase, 2–8 words, why or non-obvious behavior only, no trailing period |

---

## 3. Current state (verified 2026-10-23, working tree on `fa2222941`)

Resource details: [resource_architecture.md](../docs/resource_architecture.md).

| Piece | State |
|---|---|
| glTF parse → `MaterialStore` / `LightStore` / `GeometryStore` | done |
| Asset buffers | 17 device-local buffers (`AssetBuffer`), one **model slice** each per loaded model. `flushGpuAssets` uploads exactly one model. Indices, meshlet vertex ids and `GpuPrimitive` ranges/`materialId` are **model-relative**. Unload compacts the buffers, so later slices and their addresses move. Usage is `TransferSrc \| TransferDst \| Storage \| ShaderDeviceAddress`: **no** `eAccelerationStructureBuildInputReadOnlyKHR` |
| Frame root | done: `GpuFrameRoot` (80 B: camera, instances, models, primitives, materials, pbrExtensions, lights, tlas, 3 counts), one host-visible buffer per frame slot, written by `ResourceManager::writeFrameRoot`. `GpuModel` (104 B) holds slice addresses + `attributeMask`. `MeshPushData` is 24 B `{frameRoot, instanceIndex, primitiveIndex, meshletOffset, meshletCount}`. All mirrored in `shaders/types.slang` with C++ `static_assert`s |
| Raster: mesh shader + DGC (`vk_dgc.cpp`), CPU-filled sequences, MSAA color target resolved to the swapchain | done. Fragment outputs `sampleBaseColor` only. No G-buffer, no motion, `jitterOffset` unused. `passes/gbuffer.slang`, `postprocessing/gbuffer.slang` exist, unwired |
| Vertex streams | `GpuVertex` 32 B (pos, color, uv0). Normals `float3`, tangents `float4`, uv1 `float2` as separate float32 streams |
| Device: AS, ray query, OMM features enabled | done |
| Device: RT pipeline, SER, RT maintenance1, NV cluster/partitioned AS enabled | **unused by this plan** (A3) |
| `HardwareCapabilities`: OMM props | **queried but not cached** |
| `src/render/vk_pbr.{hpp,cpp}` | **empty files** |
| `GpuMaterial` | done: 112 B both sides, size + offset asserts |
| `GpuMaterialFlag` (C++) | done. Slang flag constants **missing** |
| `GpuLightType` | **missing**: `GpuLight.type` is a raw `uint` |
| `shaders/material/brdf.slang` | Lambert stub |
| Slang modules | **not used**: every file is `#include` + `#ifndef` guards |
| `ThirdParty/` | `rtxdi`, `sharc`, `omm`, `nrc` (prebuilt, Windows) fetched. **DLSS (NGX) missing** |
| Engine exception type | none: code throws `std::runtime_error` / `std::logic_error` |
| Stubs for dropped tech: `geometry/mega_geometry.slang`, `geometry/mega_tess.slang`, `passes/mega_select.slang`, `passes/mega_tess.slang`, `gi/sharc.slang`, `passes/sharc_resolve.slang`, `gi/rtxgi.slang` (SHaRC/NRC switch), `passes/nrc_pack.slang`, `passes/gbuffer.slang`, `raytracing/hit.slang`, `raytracing/payload.slang`, `raytracing/trace.slang`, `postprocessing/dlss.slang`, the `sharc` fetch in `ThirdParty/CMakeLists.txt` | to delete (A3). `gi/nrc.slang` is replaced by `gi/nrc_bridge.slang` (G1) |

Toolchain facts (compiled with the engine capability string and checked with `spirv-val`):

| Fact | Consequence |
|---|---|
| `RaytracingAccelerationStructure(uint64_t)` → `OpConvertUToAccelerationStructureKHR` | the TLAS address goes straight into `GpuFrameRoot.tlas` with no AS descriptor |
| `module` / `import a.b;` (dot = folder), typed pointers in structs, `enum X : uint`, `RWTexture2D<float4>.Handle` in push data all validate | adopt them (§4) |
| heap access (`getDescriptorFromHandle`, `ResourceDescriptorHeap[]`) **never** emits `NonUniform`, even with `NonUniformResourceIndex` | §4.4 |
| `RAY_FLAG_FORCE_OMM_2_STATE` emits the flag without `RayTracingOpacityMicromapEXT` | avoided: 2-state is forced per instance (§2 #6), no ray flag needed |
| `slangc -depfile` exists | replace hand-written `DEPENDS` lists (§4.5) |

---

## 4. Slang conventions

### 4.1 Modules

Library files become Slang modules (`module brdf;`, exports `public`). Passes `import` them. Vendor HLSL headers (RTXDI, NRC) are `#include`d **only** from pass files and their bridge file, because they depend on textual order (the bridge must define `RAB_*` before the RTXDI headers).

| Module (path) | Owns | New? |
|---|---|---|
| `types` | POD mirrors of `src/core/types.hpp` (`GpuFrameRoot`, `GpuModel`, `GpuPrimitive`, `GpuRayInstance`), flags, enums | edit |
| `core.binding` | heap helpers (`tex2D`, `samplerOf`, new `sampleOr`), `bdaPtr` | edit |
| `core.frame` | `loadDrawContext`, `loadCamera`, `loadMaterial` (model-relative ids, row 0 default) | edit |
| `core.math` | `kPi`, ONB, octahedral encode, PCG RNG | edit |
| `geometry.mesh` | raster meshlet fetch | – |
| `geometry.hit` | ray-query hit → 3 vertex ids → interpolated world `SurfaceHit` | **new** |
| `material.pbr` | `SampledMaterial`, `sampleMaterial` | edit |
| `material.brdf` | `evalBrdf`, `sampleBrdf`, `pdfBrdf` | edit |
| `lighting.lights` | `LightSample`, `evalLight` | **new** |
| `lighting/rtxdi_bridge.slang` | `RAB_*` + RTXDI resource macros (textual include) | **new** |
| `gi/nrc_bridge.slang` | NRC buffers + `NrcConstants` setup (textual include) | **new** (replaces `gi/nrc.slang`) |
| `postprocessing.gbuffer` | G-buffer layout, pack/unpack | edit |
| `passes/*.slang` | entry points only | edit / new |

### 4.2 Types

```slang
module types;

public static const uint kNoneIndex = 0xffffffffu;

public enum AlphaMode : uint { Opaque = 0, Mask = 1, Blend = 2 }
public enum LightType : uint { Directional = 0, Point = 1, Spot = 2 }

// mirrors GpuMaterialFlag in src/core/types.hpp
public static const uint kMaterialAlphaModeMask = 0x3u;
public static const uint kMaterialDoubleSided   = 1u << 2;
public static const uint kMaterialUnlit         = 1u << 3;

// [CommittedInstanceIndex()]; rewritten with the tlas
public struct GpuRayInstance
{
    public uint instanceIndex;    // row in GpuFrameRoot.instances
    public uint primitiveIndex;   // row in GpuFrameRoot.primitives
}

// existing struct; new fields marked
public struct GpuFrameRoot
{
    public uint64_t camera;
    public uint64_t instances;      // GpuObjectUB[entity]
    public uint64_t models;         // GpuModel[entity]
    public uint64_t primitives;     // absolute rows, model-relative values
    public uint64_t materials;      // row 0 only: default material
    public uint64_t pbrExtensions;
    public uint64_t lights;         // load-time GpuLight rows
    public uint64_t tlas;
    public uint64_t rayInstances;   // new: GpuRayInstance[]
    public uint     lightCount;
    public uint     instanceCount;
    public uint     primitiveCount;
    public uint     frameIndex;     // new, replaces pad0
    public float    exposure;       // new
    public uint     debugFlags;     // new
}

public struct PassPush
{
    public GpuFrameRoot* frame;
}
```

`GpuFrameRoot` grows from 80 to 96 B. Update the C++ `static_assert`s (`sizeof == 96`, `offsetof(lightCount) == 72`). Raster keeps `MeshPushData`, whose first field is already the frame root address.

The model of a hit is `models[instances[ray.instanceIndex].modelIndex]`. Material ids, ranges and index values inside a `GpuPrimitive` are relative to that model's slices (`frame.slang` `loadMaterial` already works this way).

`GpuFrameRoot.lights` keeps the load-time rows. RTXDI owns its own light buffer layout (F2).

C++ mirrors use `vk::DeviceAddress` for every pointer, with `static_assert(sizeof)` and `static_assert(offsetof…)` per field (the `MeshPushData` pattern).

### 4.3 Rules of thumb

| Rule | Why |
|---|---|
| Read `ptr[i]` into a local struct once | one load, readable code |
| `SampleLevel(…, 0)` outside fragment shaders | no derivatives in compute. Ray-cone LOD is out of scope |
| `enum` + `switch` for light types | data-dependent, interfaces would diverge |
| `static const` for tunables | no magic numbers in bodies |
| Entry points in `passes/`, logic in modules | each pass fits one screen |

### 4.4 Non-uniform heap indices

Material texture indices diverge across a wave in compute. Slang 2026.14.1 never emits `NonUniform` on heap access. Route **all** material fetches through one helper:

```slang
public float4 sampleOr(uint tex, uint samp, float2 uv, float lod, float4 fallback)
{
    if (tex == kNoneIndex)
        return fallback;
    // slang 2026.14.1 drops NonUniform; re-probe on upgrade
    return tex2D(tex).SampleLevel(samplerOf(samp), uv, lod);
}
```

NVIDIA drivers tolerate the missing decoration, but it is UB by spec. Re-probe on every Slang upgrade (`NonUniformResourceIndex` → grep `NonUniform` in `spirv-dis`). If artifacts appear, add a `WaveReadLaneFirst` waterfall inside `sampleOr` only.

### 4.5 CMake

- `engine_add_slang_pass` passes `-depfile ${_spv}.d` and `DEPFILE ${_spv}.d`. Drop `DEPENDS`.
- Per-variant defines go in `DEFINES` (`NRC_UPDATE=1`, `NRC_QUERY=1`). One source may produce several `.spv`.
- Add `-warnings-as-errors all` once the tree compiles clean.

---

## 5. Tasks

Each task lists **Do**, **Done when**, and **Must not** where relevant. Dependencies are in §6.

### Phase A — Contracts

#### A1 · POD layouts and constants

- **Files:** `src/core/types.hpp`, `src/render/push_data.hpp`, `shaders/types.slang`
- **Do:**
  - Slang flag constants mirroring `GpuMaterialFlag` (§4.2). `GpuMaterial` is already padded and asserted.
  - `enum class GpuLightType : uint32_t { Directional, Point, Spot }` (values used by `lightTypeFromName`); `GpuLight.type` becomes `GpuLightType`. Keep 64 B.
  - `GpuRayInstance` (8 B) and the `GpuFrameRoot` fields of §4.2, with size and offset asserts.
  - Cache `vk::PhysicalDeviceOpacityMicromapPropertiesEXT` in `HardwareCapabilities::opacityMicromap`.
- **Done when:** changing any field or pad breaks the build.
- **Must not:** add a slang `MaterialPbrExtension` (pbrExt stays unread in v1).

#### A2 · Slang modules + frame root fields

- **Files:** all `shaders/**`, `CMakeLists.txt`, `src/core/vk_resource_manager.cpp`
- **Do:** convert library files to modules (§4.1). `ResourceManager::writeFrameRoot(frameSlot)` (exists) also fills `frameIndex`, `exposure`, `debugFlags`; `rayInstances` stays 0 until C2. Compute push = `PassPush` (8 B).
- **Done when:** `mesh_raster` and `mesh_fragment` still compile and render. A dummy CS reads `push.frame.frameIndex`.

#### A3 · Remove what this plan does not use

- **Files:** `src/core/vk_device.cpp`, `CMakeLists.txt`, `shaders/**`
- **Do:**
  - Stop requesting `VK_KHR_ray_tracing_pipeline`, `VK_EXT_ray_tracing_invocation_reorder`, `VK_KHR_ray_tracing_maintenance1`, and the NV cluster/partitioned AS features and capability queries; drop the matching `HardwareCapabilities` fields. Remove the matching Slang capabilities (`spvRayTracingKHR` pipeline stages, SER, `spvRayTracingClusterAccelerationStructureNV`); keep ray query.
  - Delete the stub files listed in §3.
- **Done when:** the device creates and raster renders unchanged.

### Phase B — Shading library

#### B1 · `sampleMaterial`

- **Files:** `shaders/material/pbr.slang`, `shaders/core/binding.slang`

```slang
public struct SampledMaterial
{
    public float3 baseColor;   // linear, × COLOR_0
    public float  opacity;
    public float  metallic;
    public float  roughness;   // perceptual; α = roughness² lives in brdf
    public float3 emissive;
    public float  ao;
    public float3 n;           // world, normal-mapped, double-sided flipped
    public uint   flags;
}

public SampledMaterial sampleMaterial(GpuMaterial mat, SurfaceHit hit, float lod);
```

| Slot | Space | Channels | Missing |
|---|---|---|---|
| baseColor | sRGB format (HW decode) | rgb × factor × `COLOR_0`, a = coverage | factor |
| metallicRoughness | linear | **B** metal, **G** rough | factors |
| normal | linear | rgb, OpenGL +Y, `scale` on xy | vertex N |
| occlusion | linear | **R** | `ao = 1` |
| emissive | sRGB format | rgb × factor | factor |

- UV set from `*Uv` (0 → uv0, 1 → uv1). No `KHR_texture_transform` in v1. Normal map only when a tangent exists.
- **Unlit:** return `baseColor`/`metallic` such that `cDiff = f0 = 0`, and put `baseColor` into `emissive`. Every consumer then handles unlit with no special case.
- One function serves the G-buffer fragment shader (`lod` from derivatives) and ray hits (`lod = 0`).
- **Done when:** a metal/rough debug view on `MetalRoughSpheres` matches the texture layout.
- **Must not:** sRGB-sample metal-rough/normal/occlusion, use Unity ORM on the metal-rough slot, or flip normal Y.

#### B2 · BRDF

- **Files:** `shaders/material/brdf.slang` (delete `evalLambert`)

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

The signature takes `cDiff`/`f0` rather than `SampledMaterial` so RTXDI and NRC can call it straight from G-buffer data. A `BrdfInputs fromMaterial(SampledMaterial)` helper computes `cDiff = baseColor·(1−metallic)`, `f0 = lerp(0.04, baseColor, metallic)`.

Also add `sampleBrdf(…, u) -> BrdfSample { l, weight, pdf }` (lobe pick by Fresnel-weighted luminance, cosine diffuse, GGX **VNDF** specular) and `pdfBrdf(…, l)`. RTXDI (`RAB_SurfaceImportanceSampleBrdf`, `RAB_SurfaceEvaluateBrdfPdf`) and NRC paths use them.

- **Done when:** [pbr_lighting_materials.html §7](./pbr_lighting_materials.html#combined) numbers reproduce (white plastic r=0.5 → 0.295 head-on).
- **Must not:** feed perceptual roughness into `D`, drop `NdotL` after the correlated `V`, or multiply by it twice.

#### B3 · `evalLight`

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

This is the only light evaluation. `RAB_SamplePolymorphicLight` wraps it.

- **Done when:** an on-axis spot debug gives `cosToAxis = +1`.
- **Must not:** use `dot(spotDir, L)` or scale intensity, range, or cone by node scale.

### Phase C — Acceleration structures

#### C1 · BLAS per primitive

- **Files:** `src/render/vk_acceleration.{hpp,cpp}` (new), `src/core/vk_resource_manager.cpp`
- **Do:**
  - Add a per-row extra-usage column to `kAssetBufferInfo` and set `eAccelerationStructureBuildInputReadOnlyKHR` on `Vertices` and `Indices`. `createAssetBuffer` applies it on create, grow and unload compaction alike.
  - One `VkAccelerationStructureGeometryTrianglesDataKHR` per primitive of the model just flushed. Ranges come from its `GpuPrimitive` (model-relative); addresses from the model's slices (`ResourceManager::modelAssetAddress`):

    | Field | Value |
    |---|---|
    | `vertexFormat` / `vertexStride` | `R32G32B32_SFLOAT` / `sizeof(GpuVertex)` (32) |
    | `vertexData` | model vertex slice address (index values are model-relative vertex ids) |
    | `maxVertex` | `firstVertex + vertexCount - 1` |
    | `indexType` / `indexData` | `UINT32` / model index slice address `+ firstIndex * 4` |
    | geometry flags | `OPAQUE` unless the primitive has an OMM (D2) |
    | pNext | `VkAccelerationStructureTrianglesOpacityMicromapEXT` when baked (D2) |

  - Build flag `PREFER_FAST_TRACE`. Batch the model's primitives into one `vkCmdBuildAccelerationStructuresKHR`, sized with `vkGetAccelerationStructureBuildSizesKHR`, scratch aligned to `minAccelerationStructureScratchOffsetAlignment`.
  - Build right after `flushGpuAssets` (one model per flush), while the slice addresses are current.
  - BLASes are grouped per model in a vector indexed like `modelAllocations`. `eraseModel` destroys that model's group and erases its slot, so later groups shift with the entity ids. Other BLASes stay valid: a built AS does not read its inputs.
- **Done when:** one BLAS address per primitive, validation clean, and loading two models then unloading the first keeps the second traceable.
- **Must not:** build from `meshletTriangles`, or duplicate a CPU-side ray mesh.

#### C2 · TLAS per frame

- **Do:**
  - Per frame slot: a host-visible `VkAccelerationStructureInstanceKHR` array, a host-visible `GpuRayInstance` array (same order), TLAS storage and scratch.
  - CPU writes one instance per (active entity, primitive): `transform` = 3×4 of the entity's model matrix, `instanceCustomIndex = 0` (unused), `mask = 0xff`, `accelerationStructureReference` = BLAS address. The matching `GpuRayInstance` holds the current entity and primitive rows.
  - Instance flags: `FORCE_OPAQUE` when no OMM, `FORCE_OPACITY_MICROMAP_2_STATE_EXT` when OMM. No facing flags (no pass culls).
  - Full rebuild every frame (`PREFER_FAST_TRACE`). Write the TLAS address to `GpuFrameRoot.tlas` and the `GpuRayInstance` array to `GpuFrameRoot.rayInstances`.
- **Done when:** a debug CS ray-queries the TLAS and colors by `rayInstances[CommittedInstanceIndex()].primitiveIndex`; a yawing entity moves without BLAS rebuilds.

#### C3 · `geometry.hit`

```slang
// committed ray-query hit → world-space surface
public SurfaceHit decodeHit(GpuFrameRoot f, uint rayInstance, uint triangle, float2 bary,
                            float3x4 objectToWorld, float3x4 worldToObject);
```

`rayInstances[rayInstance]` → instance row → `GpuModel` and `GpuPrimitive` → `model.indices[firstIndex + 3·triangle + k]` → 3 model-relative vertex ids → interpolate pos/uv0 (`model.vertices`), uv1, normal, tangent (absent stream: address 0, mask bit clear → default) → `objectToWorld` for positions, `transpose(worldToObject)` for normals. The material is `loadMaterial` semantics: model-relative `materialId`, `kNoneIndex` → row 0.

### Phase D — Opacity micromaps

#### D1 · Bake

- **Files:** `src/core/omm_baker.{hpp,cpp}` (new), `ThirdParty/CMakeLists.txt` (`NVIDIA-RTX/OMM`, pinned tag)
- **Do:**
  - Qualify: `AlphaMode::Mask` (cutoff = `alphaCutoff`) and `AlphaMode::Blend` (cutoff 0.5) with a base-color alpha texture. Anything else is opaque for rays.
  - OMM SDK CPU baker at load time, disk cache keyed by (texture hash, primitive, cutoff). It runs before `flushGpuAssets`: material rows and index scratch are dropped after upload, and decoded pixels are freed after texture upload, so the loader keeps the base-color alpha of qualifying textures until the bake.
  - Format **4-state**, subdivision level from §9, clamped to `opacityMicromap.maxOpacity4StateSubdivisionLevel`. 4-state keeps unknown regions classified; the instance flag collapses them to 2-state at trace time.
  - Triangle order is index-buffer order (`indices[firstIndex..]`), which matches `CommittedPrimitiveIndex()`.
  - Special indices: `FULLY_OPAQUE` / `FULLY_TRANSPARENT` for uniform triangles.
- **Done when:** the cache round-trips, and baker stats show > 70 % known micro-triangles on Sponza foliage.

#### D2 · Build and attach

- **Do:**
  - Buffers with `eMicromapBuildInputReadOnlyEXT` / `eMicromapStorageEXT`. `vkGetMicromapBuildSizesEXT` → `vkCmdBuildMicromapsEXT`, one `VkMicromapEXT` per primitive, RAII-owned, built before C1 in the same command buffer.
  - C1 geometry pNext: `VkAccelerationStructureTrianglesOpacityMicromapEXT { indexType, indexBuffer, indexStride, baseTriangle = 0, micromap }`. Geometry not `OPAQUE`.
- **Done when:** validation is clean, and shadow rays through `AlphaBlendModeTest` foliage show cut-outs.

#### D3 · Ray-query contract

- Ray-query passes need no pipeline flag for OMM.
- Every ray query in the engine goes through one helper in `geometry.hit`:

```slang
// opaque or 2-state omm only: no candidates
public bool traceVisibility(float3 origin, float3 dir, float tMax);
public bool traceClosest(float3 origin, float3 dir, float tMax, out RayHit hit);
```

- In debug builds, count any candidate in `Proceed()` into a debug buffer and assert zero.
- **Must not:** write candidate alpha-test loops, or premultiply alpha.

### Phase E — G-buffer

#### E1 · Raster G-buffer + DLSS guides

- **Files:** `shaders/passes/mesh_raster.slang`, `shaders/passes/mesh_fragment.slang`, `shaders/postprocessing/gbuffer.slang`, `src/render/vk_renderer.cpp`, `src/scene/vk_camera.cpp`
- **Do:**
  - Render at DLSS render resolution (H1 optimal settings), 1× samples. MSAA is not used on this path.
  - Projection is jittered by `jitterOffset` (Halton 2,3, phase count `8 · (displayRes / renderRes)²`). Motion is computed from **unjittered** current and previous clip positions (`prevViewProj`, `prevModelMatrix`).
  - Mesh shader outputs world position, world normal, tangent, uv0, uv1, color, current and previous clip.
  - Fragment: `sampleMaterial` → `discard` when `opacity < cutoff` (Mask: `alphaCutoff`, Blend: 0.5) → flip `n` on back faces for double-sided → write:

    | Image | Format | Content | Read by |
    |---|---|---|---|
    | depth | D32F | hardware depth | RTXDI, NRC, DLSS (`InUseHWDepth`) |
    | `normalRoughness` | RGBA16F | world shading N, **perceptual** roughness | RTXDI, NRC, DLSS (packed roughness) |
    | `geoNormal` | RG16_SNORM | oct geometric normal | ray origin offset |
    | `diffuseAo` | RGBA8 | `cDiff`, ao | RTXDI, NRC, DLSS diffuse albedo |
    | `specularF0` | RGBA8 | `f0` | RTXDI, NRC |
    | `specAlbedo` | RGBA8 | env-BRDF approx of `f0`, roughness, `NdotV` | DLSS specular albedo |
    | `emissive` | R11G11B10F | emissive (unlit: baseColor) | `noisy_color` |
    | `motion` | RG16F | render-res pixels, current → previous | DLSS, RTXDI temporal |

  - Two G-buffer sets, ping-ponged by frame parity. RTXDI temporal resampling reads the previous set.
- **Done when:** debug views of every image are plausible on `MetalRoughSpheres`, and `AlphaBlendModeTest`, `TwoSidedPlane`, `UnlitTest` match the Khronos screenshots in kind.
- **Must not:** write GGX `α` as roughness, jitter without unjittered motion, or sRGB-encode guides.

#### E2 · Lights follow their entity

- **Do:** `LightStore` keeps its rows (node-local position/direction + owning entity) after flush; today they are dropped. Rows are erased with their model and the entity link shifts like other entity rows. Each frame the CPU transforms them by the entity model matrix into the RTXDI light buffer (F2), normalizing the direction and keeping photometric values. `GpuLight` stays the load-time layout.
- **Done when:** spinning `LightsPunctualLamp` moves its highlight with the mesh.

### Phase F — RTXDI

SDK: `NVIDIA-RTX/RTXDI-Library` (host C++ + HLSL headers), pinned tag via `ThirdParty/CMakeLists.txt`. Required; a missing SDK is a configure error.

#### F1 · Host context

- `rtxdi::ReSTIRDIContext` with `ReSTIRDIStaticParameters { renderWidth, renderHeight, CheckerboardMode::Off }`. `SetFrameIndex` each frame.
- Local light sampling `Uniform`, resampling `FusedSpatiotemporal` (§2 #12).
- Buffers: DI reservoirs (`RTXDI_PackedDIReservoir`, sized by the context), neighbor offsets (`rtxdi::FillNeighborOffsetBuffer` once), one host-visible constants buffer per frame slot (`RTXDI_LightBufferParameters`, `RTXDI_RuntimeParameters`, `ReSTIRDI_Parameters` from the context).

#### F2 · Light buffer + bridge

- **Light buffer** (CPU, per frame slot): local lights (point, spot) first, then infinite (directional), stable order so `RAB_TranslateLightIndex` is the identity.
- **`lighting/rtxdi_bridge.slang`** defines the RTXDI resource macros against heap slots and implements the `RAB_*` set:

  | Group | Functions | Backed by |
  |---|---|---|
  | surface | `RAB_Surface`, `RAB_GetGBufferSurface`, `RAB_IsSurfaceValid`, `RAB_GetSurfaceWorldPos`, `RAB_GetSurfaceNormal`, `RAB_GetSurfaceLinearDepth`, `RAB_GetMaterial`, `RAB_AreMaterialsSimilar` | E1 G-buffer (current/previous set) |
  | BRDF | `RAB_SurfaceEvaluateBrdfPdf`, `RAB_SurfaceImportanceSampleBrdf` | `pdfBrdf`, `sampleBrdf` |
  | lights | `RAB_LightInfo`, `RAB_LoadLightInfo`, `RAB_StoreCompactLightInfo`, `RAB_SamplePolymorphicLight`, `RAB_LightSample`, `RAB_IsAnalyticLightSample`, `RAB_LightSampleSolidAnglePdf`, `RAB_EvaluateLocalLightSourcePdf`, `RAB_TranslateLightIndex` | `evalLight` |
  | target pdf | `RAB_GetLightSampleTargetPdfForSurface` | `luminance(evalBrdf · L_i · NdotL)` |
  | visibility | `RAB_GetConservativeVisibility`, `RAB_GetTemporalConservativeVisibility` | `traceVisibility` |

  The bridge adds no BRDF or light math of its own. Implement exactly the `RAB_*` set the pinned SDK version requires.

#### F3 · Passes

| Pass | Calls |
|---|---|
| `rtxdi_initial` CS | `RTXDI_SampleLocalLights` + `RTXDI_SampleInfiniteLights` + `RTXDI_SampleBrdf` → `RTXDI_CombineDIReservoirs` → `RTXDI_StoreDIReservoir` |
| `rtxdi_shade` CS | `RTXDI_DISpatioTemporalResampling` → final `traceVisibility` → `evalBrdf · L_i · NdotL · RTXDI_GetDIReservoirInvPdf` → `RTXDI_StoreDIReservoir` |

Output: `directRadiance` (RGBA16F, linear HDR, render res).

- **Done when:** on `LightsPunctualLamp`, resampling mode `None` with high initial sample counts (the SDK's own unbiased configuration, used as ground truth) matches `FusedSpatiotemporal` with default counts within noise, and the latter is temporally stable.

### Phase G — RTXGI NRC

SDK: NVIDIA RTX Neural Radiance Cache (Vulkan host library + HLSL headers), pinned tag via `ThirdParty/CMakeLists.txt`. Required; a missing SDK is a configure error. The host library ships as prebuilt DLLs (NRC and its CUDA runtime dependencies); CMake copies them next to the executable. Verify every SDK name in this phase against the pinned headers when vendoring.

#### G1 · Context + resources

- **Device:** merge NRC's required device extensions and features into `Device`, next to H1's NGX set.
- **Context:** `nrc::vulkan::Context` created once after device init. `Configure` with the context settings (render resolution, `kMaxBounces`, training path budget) at init and on resize. The SDK allocates its own buffers (query path info, training path info and vertices, query/training radiance, counters); the engine fetches them and gives each a heap slot or BDA. Engine code never allocates or resizes them.
- **Per frame:** `BeginFrame` with the frame settings (scene AABB from §9, training enabled) → the SDK fills `NrcConstants`, uploaded into one host-visible buffer per frame slot. `EndFrame` after the frame's submit.
- **`gi/nrc_bridge.slang`** declares the SDK buffer types against those slots and exposes `NrcConstants`. The SDK headers assume HLSL buffer packing; match it in the bridge with an explicit Slang layout, never by editing vendor headers.
- **Done when:** the context configures, its buffers are bound, and validation is clean with NRC enabled.

#### G2 · Update + query paths

- **`pathtrace.slang`** is one compute source compiled twice (`NRC_UPDATE=1`, `NRC_QUERY=1`). It contains the only path loop in the engine:
  - per vertex: `traceClosest` → `decodeHit` → `sampleMaterial` → direct light at the hit via `RTXDI_SampleLocalLights` / `RTXDI_SampleInfiniteLights` (no reuse) + one `traceVisibility` → add emissive → `NrcUpdateOnHit` with `NrcSurfaceAttributes` built from `SampledMaterial` (the SDK decides continue, terminate, or write a query) → `sampleBrdf` + `NrcSetBrdfPdf` for the next direction. A miss calls `NrcUpdateOnMiss`.
  - Russian roulette after bounce 2, only where `NrcCanUseRussianRoulette` allows it. `kMaxBounces` from §9.
- **`nrc_update`** (`NRC_UPDATE`): training paths on the sparse pixel set the context's training budget gives, starting from the G-buffer surface. Writes training records.
- **`nrc_query`** (`NRC_QUERY`): every pixel, one path from the G-buffer surface with initial throughput `ao` (AO only on indirect, §2 #10). The SDK terminates the path at a query vertex and records it; radiance gathered before that vertex goes to the per-pixel indirect target via `NrcWriteFinalPathInfo`.
- **Done when:** the SDK's debug resolve modes show cache radiance, training loss falls after load, and an interior lit only by a sun is not black.

#### G3 · Train, infer, resolve, noisy color

- **Host:** after both passes, `QueryAndTrain` (inference on this frame's query records, then one training step on the update paths), then `Resolve`, which adds `throughput · cachedRadiance` per query into `indirectRadiance` (RGBA16F, render res).
- **`noisy_color`** CS: `noisyColor = directRadiance + indirectRadiance + emissive` (RGBA16F). This is the only place the three terms meet.
- **Done when:** `Sponza` with a sun shows bounce light under arches, stable after DLSS-RR.
- **Must not:** apply AO to direct, add a second path loop, or touch NRC buffers outside `gi/nrc_bridge.slang` and `vk_nrc`.

### Phase H — DLSS Ray Reconstruction

SDK: `NVIDIA/DLSS` (NGX, Vulkan), pinned tag, added to `ThirdParty/CMakeLists.txt` (not fetched today). Required: unsupported RR at init throws `std::runtime_error`.

#### H1 · NGX

- **Device:** merge NGX's required instance/device extensions (`NVSDK_NGX_VULKAN_RequiredExtensions` or the feature-requirement query of the pinned SDK) into `Device`, together with NRC's set (G1). `NVSDK_NGX_VULKAN_Init` once.
- **Create:** `NGX_VULKAN_CREATE_DLSSD_EXT1` with HW depth, packed roughness (`NVSDK_NGX_DLSS_Roughness_Mode_Packed`), HDR input. Render resolution from the RR optimal-settings query for the chosen quality mode; recreate on resize.
- **Evaluate** (`NGX_VULKAN_EVALUATE_DLSSD_EXT`):

  | Input | Source |
  |---|---|
  | color | `noisyColor` (G3), linear, pre-exposure |
  | depth / motion | E1 depth / `motion` (MV scale 1) |
  | diffuse / specular albedo | E1 `diffuseAo` / `specAlbedo` |
  | normals + roughness | E1 `normalRoughness` |
  | jitter | `GpuCameraData.jitterOffset` (same as E1 raster) |

  Output: display-res RGBA16F HDR.
- Verify every name above against the pinned headers (`nvsdk_ngx_helpers_dlssd_vk.h`) when vendoring.
- **Done when:** 1 spp + RR is stable under camera motion on `Sponza`.

#### H2 · Composite

- `composite` CS: `exposure` → tonemap (ACES fitted) → swapchain. Tonemap exactly once.
- Until H1 lands, `composite` reads `noisyColor` at render res. This is bring-up order, not a shipped path.

### Validation assets

| Asset | Proves |
|---|---|
| `MetalRoughSpheresNoTextures` (+ debug sun) | `cDiff` / `f0` lerp, `α = r²` |
| `MetalRoughSpheres` | B/G packing, sRGB vs linear |
| `DirectionalLight` | lux, no falloff, `L = −worldDir` |
| `LightsPunctualLamp` | range window, spot sign, RTXDI convergence |
| `NormalTangentTest` | OpenGL Y, TBN |
| `VertexColorTest` | `COLOR_0` |
| `AlphaBlendModeTest` | raster discard + OMM shadows |
| `TwoSidedPlane` / `UnlitTest` | flip / unlit |
| `Sponza` | many materials, OMM stats, NRC bounce, RR stability |

With no glTF lights, a debug directional light behind `debugFlags` is allowed (off by default).

---

## 6. Dependencies

```
A1 → A2 → A3
A2 → B1 → B2 → B3
A2 → C1 → C2 → C3
D1 → D2 (with C1)
B1 + A2 → E1 → E2
B3 + C3 + E1 → F1 → F2 → F3
F2 + C3 → G1 → G2 → G3
E1 + G3 → H1 → H2
```

PR order: A1–A3, B1–B3, C1–C3, E1–E2, F1–F3 (**first lit pixel**, via H2 bring-up), G1–G3, D1–D2, H1–H2.

---

## 7. Host (C++) layout

| Unit | Owns |
|---|---|
| `vk_acceleration` | BLAS groups per model, per-slot TLAS + instance + `GpuRayInstance` buffers, `getTlasAddress(slot)`, `getRayInstanceAddress(slot)` |
| `omm_baker` | bake cache, `VkMicromapEXT` RAII |
| `vk_pbr` (`PbrPass`) | G-buffer sets, compute pipelines, composite. Frame roots stay in `ResourceManager` |
| `vk_rtxdi` | `ReSTIRDIContext`, reservoirs, neighbor offsets, light + constants buffers, `rtxdi_initial` / `rtxdi_shade` |
| `vk_nrc` | NRC context, SDK buffer bindings, `NrcConstants` per slot, `nrc_update` / `nrc_query` / `noisy_color`, `QueryAndTrain` / `Resolve` |
| `vk_dlss` | NGX init, RR feature, evaluate |

Rules: `MaterialStore` / `LightStore` stay on `Scene`. `ResourceManager::flushGpuAssets` remains the only asset upload site. Every AS, micromap and SDK object is owned by a move-only RAII type (destroy the object, then its buffer). Build or SDK failure throws `std::runtime_error` naming the failing call. All other C++ rules: §2 #14.

---

## 8. Must-not checklist

| Area | Never |
|---|---|
| lighting | any light, GI, or denoise path outside RTXDI / NRC / DLSS-RR. A reference loop, homegrown cache or MLP, SVGF, or TAA |
| hardware | AMD/Intel code paths, vendor checks, or fallbacks (§2 #0) |
| shading | a second BRDF. Perceptual roughness in `D`. Double or missing `NdotL`. AO on direct. Emissive × `NdotL`. sRGB on metal-rough/normal/AO. Premultiplied alpha before the BRDF |
| lights | `dot(spotDir, L)`. Node scale on intensity/range/cone. Unattached light defs |
| rays | RT pipelines, SBTs, any-hit, SER. Candidate loops in ray queries. Camera culling on TLAS instances |
| OMM | OMM on opaque primitives. Baking in meshlet order instead of index order |
| Slang | `Sample` (implicit LOD) outside fragment. Material fetches bypassing `sampleOr` |
| SDKs | NGX calls from Slang. Unpinned SDK versions. Optional/disabled SDK builds |

---

## 9. Open choices

| Choice | Default |
|---|---|
| RTXDI local light sampling | `Uniform` until a scene has > ~32 local lights, then `Power_RIS` (adds PDF texture + presample pass) |
| Emissive meshes as RTXDI lights | out of v1. Emissive reaches the image via the G-buffer and NRC path hits |
| BLAS compaction | off in v1 |
| Skinned / morphed meshes in BLAS | bind pose in v1. Later: refit from a skinning CS output |
| `Blend` | treated as mask (cutoff 0.5) in v1 |
| OMM subdivision level | 5, clamped to device max |
| NRC scene AABB / `kMaxBounces` | `GeometryStore` world bounds, recomputed on load / 4 |
| NRC training budget | SDK default for the render resolution |
| DLSS quality mode | Quality |

---

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
