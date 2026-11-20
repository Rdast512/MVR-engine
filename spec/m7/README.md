# M7 · Material binning and emissive lights

**Status** — Spec planned on 2026-11-20. E1 built on 2026-11-20 (open: C-E1-3 GPU-AV, C-E1-4 cost); E2 and F1 not built.\
**Scope** — PBR plan's M7 row: the resolve split into per-material-bin shaders (engine spec §9.1–9.2), emissive triangles as RTXDI lights (E2), and local light presampling (F1).
[Engine spec](../../docs/engine_spec.md) · [PBR plan](../pbr_implementation_plan.md) · [Considerations](../considerations.md)

| Task | Contract |
|---|---|
| [E1 · Material binning](e1_material_binning.md) | `classify` (8×8 tiles, `binIds`, per-bin tile lists), one DGC execute with an execution set of per-bin resolve shaders and the overlap guard; guides bit-identical |
| [E2 · Emissive-triangle lights](e2_emissive_triangles.md) | One light per triangle of each emitting primitive, written each frame by `light_world`; triangle sampling in the bridge; BRDF rays name lights; one emission rule along every path |
| [F1 · Local light presampling](f1_light_presampling.md) | `ImportanceSamplingContext`, the light PDF texture and its mips, `Power_RIS` tiles and ReGIR cells; NVIDIA's `Medium` (`ReGIR_RIS`, 8 samples) becomes the default |

## Order

```
E1            (independent)
E2 → F1
```

E1 touches only the resolve, so it can be built before, after or beside the light work. F1 needs E2: with punctual lights alone there is little for presampling to choose between, and its power and compact-light formats include the triangle row.

## Frame and acceptance

```
async compute: light_world (punctual + triangle rows, power) → light_pdf_mips → rtxdi_presample → rtxdi_presample_regir → TLAS
graphics:      … raster → Hi-Z → classify → binArgs → DGC resolve (Background | Standard | Unlit) → rtxdi_initial (ReGIR_RIS) → …
```

M7 is done when:

- the resolve runs as one GPU-driven DGC execute, every guide is bit-identical to the single dispatch, and the per-bin timings are recorded (engine spec §16.2);
- `EmissiveStrengthTest`, `DamagedHelmet` and `LightsPunctualLamp` light their surroundings from their emissive triangles, and the convergence check passes on an emissive scene;
- the default local light sampling is `ReGIR_RIS` with `Medium`'s counts, and `Power_RIS` and `Uniform` stay callable;
- `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST`, `ENGINE_GUIDE_CAPTURE` and `ENGINE_GI_CHECK` pass, and validation and GPU-AV are clean;
- C-F3-4, C-R32-3 and C-F3-5's emissive and presampling parts are closed.

Engine spec §16.2 also names "Bistro emissives lit". Bistro is not in the repository (C-M7-1), so the acceptance above uses the glTF samples; Bistro is checked when the asset is added.

## Facts established while planning (2026-11-20)

| Fact | Source | Consequence |
|---|---|---|
| NVIDIA's `Medium` preset samples local lights with `ReGIR_RIS`, 8 samples in each mode; ReGIR fills its cells from the `Power_RIS` tiles | `reference/RTXDI/Samples/FullSample/Source/UserInterface.cpp` `ApplyPreset`, `Shaders/LightingPasses/Presampling/PresampleReGIR.hlsl` | the plan's "`Power_RIS`" becomes `Power_RIS` + ReGIR, with ReGIR the default (NVIDIA defaults are the engine's) |
| The sample rebuilds triangle lights every frame from instance tasks, samples the emissive texture once per triangle, treats every triangle light as one-sided, and does not add the emission of BRDF-ray hits (`enableIndirectEmissiveSurfaces = false`) | `Shaders/PrepareLights.hlsl`, `Shaders/PolymorphicLight.hlsli`, `BrdfRayTracing.hlsl`, `LightingPasses.cpp` | E2's per-frame rows, radiance and emission rule; double-sided emission is the one deviation |
| The path code adds hit emission at every vertex today | `passes/indirect.slang` (secondary emissive), `lighting/path_shading.slang` (`next.emissive`), `passes/nrc_update.slang` | with triangle lights each of those would count a light twice; E2 changes all three |
| `KHR_materials_emissive_strength` is folded into `emissiveFactor` when the material row is packed | `src/core/material_store.hpp` | triangle radiance needs no extension read |
| Emitting test scenes in the repository: `EmissiveStrengthTest` (5 cubes, 12 triangles each, strengths 1–16, over a backdrop), `DamagedHelmet` (emissive texture on its only material), `LightsPunctualLamp` (emissive bulb), `StainedGlassLamp`; no Bistro | `gltf/2.0`, `models/` | the acceptance scenes; C-M7-1 |
| The unlit model is a branch inside `sampleMaterial`; it is the only second shading body | `shaders/material/pbr.slang` | E1's bins: `Background`, `Standard`, `Unlit` |
| The 5070 driver allows compute shader objects in DGC execution sets (`shaderBind=0xff`); no NVIDIA sample in `reference/` uses compute DGC | `logs/engine.log` DGC limits; `reference/vk_device_generated_cmds-master` (graphics) | E1 follows the registry and VUIDs for the compute layout, and asserts the stage bit at device creation |
| The async compute submit waits for the previous frame's ray passes | `src/render/vk_rt_scene.cpp` (`rayTimeline`), considerations C-F2-1 | F1's PDF texture and RIS buffers need one copy, not one per slot |

## References

- `reference/RTXDI/Samples/FullSample` — light preparation, presampling, ReGIR, the bridge (`RtxdiApplicationBridge/`), and the `Medium` preset. NVIDIA RTX SDKs license, like the RTXDI SDK.
- `ThirdParty/_deps/src/rtxdi` — `ImportanceSamplingContext`, `ReGIR/`, `LightSampling/`.
- [reference/dgc_comparison.md](../../reference/dgc_comparison.md) §7, §9 and `reference/vk_device_generated_cmds-master` — DGC execution sets and preprocess.
- [visibility_buffer.md](../features/visibility_buffer.md) §7 — the M7 binning hook.
