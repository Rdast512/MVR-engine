# M6 · D1: OMM bake per primitive

**Status:** planned 2026-11-20 · **Milestone:** M6
**Implements:** engine spec §7.1 step 5, §10.5; plan D1 ([pbr_implementation_plan.md](../pbr_implementation_plan.md))
**Depends on:** A4 (material rows, `uvMatrices`), C1 (final cluster triangles and their order)
**Considerations:** C-D-1 (rays see alpha cards whole), C-T-2 (EXT vs KHR, closed here), new C-D1-1 (no processing cache)
**Reference:** OMM SDK 1.9.2 `omm.h` and `docs/integration_guide.md` (`ThirdParty/_deps/src/omm`); `reference/vk_gltf_renderer/src/gltf_scene_omm.*` (per-primitive linkage)

Turn each alpha-tested primitive into OMM SDK output (micromap data, per-triangle descriptors, usage histogram, per-triangle index) that D2 uploads unchanged. CPU only; no Vulkan object is created here.

## Scope

| Input | Rule |
|---|---|
| Qualifying primitive | material `AlphaMask` (cutoff `alphaCutoff`) or `AlphaBlend` (cutoff 0.5), with a base-color texture. Everything else is opaque to rays |
| Triangles | the primitive's **final** cluster triangles, concatenated in cluster order: after `meshopt_optimizeMeshletLevel` (`geometry_store.cpp`), which may rotate corners. Never the glTF index buffer or an earlier order |
| Alpha | what the shader samples: mip 0 of the runtime texture. KTX2/BC7 is decoded from its blocks; uncompressed images use their decoded RGBA8. Never the source image of a compressed texture |
| UVs | the base-color slot's UV set with `GpuMaterial::uvMatrices[baseColor]` applied, as `sampleMaterial` does |
| Sampler | the base-color texture's glTF sampler |

A primitive without a base-color texture, or with an untextured alpha (factor only), is not baked: its alpha is constant. A factor alpha below the cutoff makes it fully transparent; that case is logged and the primitive stays opaque to rays (no glTF sample needs it).

## Decisions

| Decision | Rule | Why |
|---|---|---|
| Extension | keep `VK_EXT_opacity_micromap`; do not enable `VK_KHR_opacity_micromap` | VUID-vkCmd*-micromap-11636: with only KHR, ray-query shaders tracing OMM geometry need `OpacityMicromapIdKHR`, which Slang cannot emit (#13438, C-C4-3). EXT is confirmed on the 2060 Super. Closes C-T-2's sub-item; the `WARNING-legacy-extension-new` notice stays, with this as its reason |
| Unit of work | one `ommCpuBake` per primitive | the SDK's duplicate detection spans all of the primitive's clusters; one micromap array per primitive (D2) |
| Format | `ommFormat_OC1_4_State`, `ommUnknownStatePromotion_ForceOpaque` | SDK recommendation; the 2-state ray flag collapses unknown states at trace time |
| Alpha mode | `ommAlphaMode_Test` for Mask and Blend | matches D3's `discard`; Blend is cut at 0.5 as the plan defines |
| Cutoff | `alphaCutoff / baseColorFactor.a` in both `ommCpuBakeInputDesc::alphaCutoff` and the texture's embedded cutoff | the shader tests `factor.a × texture.a`; the SDK requires both cutoffs equal |
| Comparison | `alphaCutoffLessEqual = Transparent`, `alphaCutoffGreater = Opaque` (SDK defaults) | `discard` when `alpha < cutoff` keeps `alpha == cutoff`; the one-ULP difference at the cutoff is below the bake's resolution and is not chased |
| Texture | `ommCpuTextureFormat_UNORM8`, one mip, `ommCpuTextureFlags_DisableZOrder` off (default Z-order) | SDK 1.9 UNORM8 is 4× smaller than FP32; one fixed mip per the integration guide |
| BC7 decode | `bcdec` (single header, MIT/Unlicense), added to `ThirdParty` and used only by `omm_baker.cpp` | libktx exports no block decoder; the SDK does not decode BC. Decode only the alpha channel's 1024² mip 0 into a scratch UNORM8 buffer |
| Address mode | the sampler's `wrapS`; mixed `wrapS` / `wrapT` logs a warning | the SDK takes one mode for both axes |
| Subdivision | `maxSubdivisionLevel = min(5, maxOpacity4StateSubdivisionLevel)`; `dynamicSubdivisionScale = 2` (SDK default, texels per micro-triangle edge) | plan §settings; queried device limit |
| Flags | `EnableValidation` in debug builds; never `EnableInternalThreads` (needs OpenMP, off in the build); `Allow8BitIndices` off, `DisableSpecialIndices` off | 16/32-bit indices map to Vulkan index types; special indices `-1..-4` are Vulkan's |
| Threading | one baker per load (`ommCreateBaker`, CPU type); bakes run one per job through the loader's `parallelFor` (`assets_loader.cpp`) | `ommCpuBake` is thread-safe on one baker |
| Lifetime | the decoded alpha lives until the last bake that reads it, then is freed; bake results are copied into `OmmBake` and the SDK result destroyed | no SDK handle outlives the loader call |
| Cache | none; the bake runs on every load. Bake time is logged | engine spec §7.2's processing cache is not built; `ommCpuSerialize` / `ommCpuDeserialize` is its intended format (C-D1-1) |

## 1. Design

- **`src/core/omm_baker.{hpp,cpp}`** (new):
  - Move-only RAII owners for `ommBaker`, `ommCpuTexture` and `ommCpuBakeResult` (a destroy function per type). A failed `ommResult` throws `std::runtime_error` with the call and the primitive. The SDK's `messageInterface` goes to the engine log (`Omm` channel).
  - `OmmBaker` holds the baker. `bake(const OmmBakeInput&) → OmmBake` is const and thread-safe.
  - `OmmBakeInput`: alpha texture (UNORM8 span, width, height), sampler address mode and filter, cutoff, UVs (`std::span<const glm::vec2>`, already transformed), triangle indices (`std::span<const uint32_t>`, final cluster order), subdivision settings.
  - `OmmBake` (plain data, what D2 uploads): `arrayData` (bytes), `triangles` (`descArray` as `VkMicromapTriangleEXT`-layout rows), `usages` (histogram as `VkMicromapUsageEXT`-layout rows), `indices` (bytes), `indexFormat` (16 or 32 bit), `triangleCount`. `static_assert`s pin `ommCpuOpacityMicromapDesc` against `VkMicromapTriangleEXT` (offset, size); the histogram is converted field by field.
- **Loader:** after geometry processing, collect qualifying primitives, decode each referenced alpha texture once, then `parallelFor` over primitives. The primitive's corner list is built from its meshlets (`firstMeshlet .. firstMeshlet + meshletCount`): for each meshlet in order and each local triangle `t`, the corners `meshletVertices[vertexOffset + meshletTriangles[triangleOffset + 3t + c]]`, exactly what C1 hands the CLAS build. UVs are read from the same vertices with the slot's `uvMatrices` applied.
- **Geometry store:** `GpuPrimitive` gains nothing; the bake output is kept per primitive row in a CPU vector next to the geometry, for D2.
- **Stats:** `ommDebugGetStats` per primitive (debug and the UI's Geometry section): micro-triangles opaque / transparent / unknown, `knownAreaMetric`, array bytes. A one-line load summary: primitive count, bake ms, total array and index bytes.

## 2. Files

| File | Change |
|---|---|
| `src/core/omm_baker.{hpp,cpp}` | new |
| `src/core/assets_loader.cpp` | qualify, decode alpha, `parallelFor` bakes, keep `OmmBake` rows |
| `src/core/texture_manager.cpp` | expose mip 0's BC7 blocks (or the decoded RGBA8 of an uncompressed image) to the loader before release |
| `ThirdParty/CMakeLists.txt` | `bcdec` (header only) |
| `src/ui/engine_ui.cpp` | OMM stats rows |
| `spec/considerations.md` | C-D1-1; C-T-2 sub-item closed |

## 3. Verification

- **Debug images:** with `ENGINE_OMM_DEBUG_IMAGES=1`, `ommDebugSaveAsImages` writes each primitive's bake over its texture to `logs/omm`. Compare a Sponza plant bake against its texture by eye and by sampling pixels numerically, as the guide capture does.
- **Orientation:** a primitive whose UVs are mirrored against the texture shows a mirrored overlay; `AlphaBlendModeTest`'s cut-out pattern is asymmetric enough to catch it.
- **Stats:** `knownAreaMetric > 0.7` on every Sponza mask primitive. Lower values are recorded with their subdivision level before any setting changes.
- **Determinism:** two loads give byte-identical `OmmBake`s (the hash is logged).
- **Bake time:** release build, load summary on the 5070; recorded in §Log and in C-D1-1.

## Done when

- every qualifying Sponza and `AlphaBlendModeTest` primitive has an `OmmBake`, with `knownAreaMetric > 0.7` on Sponza;
- debug images match the textures (right orientation, right cutoff);
- two loads are byte-identical, and the bake time is recorded.

## Must not

- bake from the source image of a compressed texture, from the glTF index order, or per cluster;
- set `EnableInternalThreads`, or keep SDK handles past the loader call.

## Log

| Date | Change |
|---|---|
| 2026-11-20 | Spec written; EXT kept (VUID-11636); `bcdec` chosen for BC7; cache deferred (C-D1-1) |
