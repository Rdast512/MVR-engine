# M6 · Alpha

**Status** — Spec planned on 2026-11-20; D1 built the same day (known-area gate open on Sponza's chains, C-D1-2); D2 built the same day (micromap build cost C-D2-1, no AS memory row C-D2-2); D3 built the same day (raster vs OMM edge differences C-D3-1); D4 built the same day, which completes M6 (acceptance below, 2026-11-20).\
**Scope** — PBR plan Phase D: alpha-tested geometry is cut out in raster and in every ray.
[Engine spec](../../docs/engine_spec.md) · [PBR plan](../pbr_implementation_plan.md) · [Considerations](../considerations.md)

| Task | Contract |
|---|---|
| [D1 · OMM bake](d1_omm_bake.md) | One OMM SDK CPU bake per alpha primitive over its final cluster triangles, from the alpha the shader reads |
| [D2 · Micromap build and CLAS attach](d2_micromap_clas.md) | `VkMicromapEXT` per baked primitive, per-cluster OMM index ranges, alpha instances drop `FORCE_OPAQUE` |
| [D3 · Alpha-tested raster bin](d3_alpha_raster_bin.md) | Draw records routed by alpha mode, a mesh + fragment variant with `discard`, its own DGC state |
| [D4 · Ray contract](d4_ray_contract.md) | Zero-candidate debug counter, shadow-origin offset, alpha checks in the self-test runners |

## Order

```
A4 + C1 → D1 → D2 → D4
          D1 → D3
```

D3 needs only D1's qualification rule (which primitives are alpha-tested), so it can be built in parallel with D2. D4 closes the milestone: its checks need both D2 (rays) and D3 (raster) to compare against each other.

## Frame and acceptance

No new frame pass. The load path bakes OMMs on the asset workers, then builds the micromaps in the command buffer that builds the CLAS (C1). Raster runs one more DGC execute per cull phase for the alpha-tested bin. Rays are unchanged: every ray already sets `RAY_FLAG_FORCE_OPACITY_MICROMAP_2_STATE` (C4, considerations C-C4-2 / C-C4-3); an alpha primitive's PTLAS instance carries no opacity flag, so its micromaps take effect.

M6 is done when:

- `AlphaBlendModeTest` cut-outs match between the raster (`discard`) and the rays (OMM), in primary-ray and shadow guide views;
- Sponza's plants cast leaf-shaped shadows, and the jitter flicker of C-D-1 is gone;
- `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST`, `ENGINE_GUIDE_CAPTURE` and `ENGINE_GI_CHECK` still pass, and validation and GPU-AV are clean;
- the bake time, micromap memory and the alpha bin's raster cost are recorded (RTX 5070; the 2060 Super stays with C-M0-1).

Acceptance, 2026-11-20 (D4 log): `AlphaBlendModeTest_sun` cuts each panel at its marker in raster and in its shadow; `Sponza_plants` shows leaf-shaped shadows. The flicker's cause, rays shadowing through card cut-outs the raster drew whole, is gone; no stability runner measured it. RT check 438 of 438 with the `alpha-edge` class, cull check 16 of 16, motion test 13 of 13, guide capture normal checks 2 of 2 and convergence passed, GI check 8 of 8, validation and GPU-AV clean. Bake time in D1, micromap build and memory in D2 (C-D2-1, accepted), alpha bin cost in D3, ray-pass cost with OMMs in D4 (+4–8%).

## Facts established while planning (2026-11-20)

| Fact | Source | Consequence |
|---|---|---|
| Sponza has 3 `MASK` materials (cutoff 0.5, base-color factor alpha 1) on 14 primitives, all on sampler 0 (linear, trilinear, `REPEAT` both axes) | `models/Sponza/Sponza.gltf` | D1 bakes 14 primitives; Blend is exercised only by `AlphaBlendModeTest` |
| Sponza base color is KTX2 `vkFormat` 146 (`BC7_SRGB_BLOCK`), 1024², 11 mips, Zstd; BC7 alpha is linear. The source PNGs sit next to the KTX2 files (`extras.gltf_ktx2.source`) | `models/Sponza`, `TextureManager::uploadKtx` | D1 decodes mip 0's BC7 blocks and never reads the PNG; libktx decodes no BC7 (its basisu decoder is internal to the DLL) |
| `VK_EXT_opacity_micromap` stays | `validusage.json` VUID-vkCmd*-micromap-11636 | with only the KHR extension, ray-query shaders tracing OMM geometry need the `OpacityMicromapIdKHR` execution mode, which Slang cannot emit (#13438, C-C4-3). Closes C-T-2's EXT vs KHR item |
| CLAS `opacityMicromapArray` is a `VkDeviceAddress`; 0 disables OMMs for the cluster | `vk.xml`, VUID-…-opacityMicromapArray-10881 / -parameter, refpage | EXT has no micromap address query: D2 must establish the address (§D2 1) |
| `VkPhysicalDeviceOpacityMicromapPropertiesEXT` is cached | `HardwareCapabilities::opacityMicromap` (`types.hpp`) | the subdivision clamp has its input; the note in `VkExtensionsSpecs/VK_EXT_opacity_micromap.md` §150 is stale |
| One cluster BLAS per primitive, one PTLAS instance per (node, primitive) | `vk_mega_geometry.cpp`, `rt_instances.slang` | the opacity choice is per primitive and fits the instance row; no per-geometry OPAQUE bit is needed |
| A cluster group never spans primitives | `geometry_store.cpp` | a draw record's bin is per primitive; the `*_MIXED` state bits of spec §8.1 A6 are not needed |
| `MeshVertexOut` carries only `SV_Position` | `shaders/types.slang` | D3 needs a mesh variant with UV and material outputs |
| No processing cache exists (engine spec §7.2 not built) | code search | D1 cannot meet "the cache round-trips"; it measures the bake time and logs C-D1-1 |
| No NVIDIA reference combines OMM with cluster AS | `reference/` | D2 follows `vk_mini_samples/mm_opacity` for the micromap build and the registry for the CLAS fields; GPU-AV is part of its done criteria |

## References

- `reference/vk_mini_samples/samples/mm_opacity` — `mm_process.cpp`: EXT and KHR micromap builds (`vkGetMicromapBuildSizesEXT` → `vkCreateMicromapEXT` → `vkCmdBuildMicromapsEXT`, `PREFER_FAST_TRACE`, buffer usages, the build-input barrier). Keeps an any-hit; the engine does not.
- `reference/vk_gltf_renderer/src/gltf_scene_omm.{hpp,cpp}` — per-primitive OMM linkage (`micromap`, index address, index type, stride, base triangle) consumed at BLAS build. Loads pre-baked `EXT_mesh_opacity_micromap` data through the KHR path; it does not bake.
- `ThirdParty/_deps/src/omm` — OMM SDK 1.9.2: `omm.h` and `docs/integration_guide.md` (runtime texture format, one mip, ForceOpaque promotion, `dynamicSubdivisionScale`). Its "FP32 only" texture note predates 1.9's UNORM8.
