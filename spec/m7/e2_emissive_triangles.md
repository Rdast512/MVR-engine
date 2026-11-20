# M7 · E2: emissive-triangle lights

**Status:** planned 2026-11-20 · **Milestone:** M7
**Implements:** engine spec §6.1 (lights row), §11.2 (light sources), §11.3 (emission at X₂); plan E2, M7 part, and F3's BRDF candidate ([pbr_implementation_plan.md](../pbr_implementation_plan.md)); [pbr_lighting_materials.md](../pbr_lighting_materials.md) "Emissive triangles"
**Depends on:** E2 punctual (`passes/light_world.slang`, `src/render/vk_world_lights.*`), F2 (the RTXDI bridge), C2 / C4 (`GpuRayInstance` rows, hit decode), G2 / G3 / G5 (the path code that adds emission)
**Considerations:** C-F3-4 (closed here), C-F3-5 (emissive part closed here; presampling is F1, the environment light stays unscheduled), C-R32-3 (closed here: the BRDF candidate names the light it hits)
**Reference:** RTXDI FullSample (`reference/RTXDI/Samples/FullSample`): `Shaders/PrepareLights.hlsl` (one task per emissive geometry instance, binary search from thread to task, the emissive texture sampled once at the centroid with anisotropic gradients, flux into the PDF texture), `Source/RenderPasses/PrepareLightsPass.cpp` (`CountLightsInScene`, the task list, `GeometryInstanceToLight`), `Shaders/PolymorphicLight.hlsli` `TriangleLight` (`CalcSample`, `CalcSolidAnglePdf`, `GetPower`), `Shaders/LightingPasses/RtxdiApplicationBridge/RAB_LightSampling.hlsli` (`RAB_TraceRayForLocalLight`, `GetLightIndex`), `Shaders/LightingPasses/BrdfRayTracing.hlsl` with `LightingPasses.cpp` (`enableIndirectEmissiveSurfaces = false`). The sample is under the NVIDIA RTX SDKs license, like the RTXDI SDK the engine links; ported code keeps its header

Make every emitting triangle a light that RTXDI samples, that a BRDF ray can name, and that reaches each shading point exactly once.

## Scope

| Item | Today | After E2 |
|---|---|---|
| Light sources | punctual rows + the engine sun | + one triangle light per triangle of every primitive whose material emits |
| World rows | one per punctual light, written each frame by `light_world` | + one per triangle light, written each frame by the same pass (instances move, as in the sample) |
| Row order | `[point, spot][directional, sun]` | `[point, spot][triangles][directional, sun]`: RTXDI's local region is punctual + triangles; `RAB_TranslateLightIndex` stays the identity |
| BRDF candidate (`RAB_TraceRayForLocalLight`) | never names a light | names the triangle light it hits |
| Emission at ray hits | added at every path vertex (`indirect`, `path_shading`, `nrc_update`) | added only where the hit triangle is not a light (one emission rule below) |
| Local light sampling | `Uniform` | `Uniform` (F1 adds the PDF texture, `Power_RIS` and ReGIR) |

## Decisions

| Decision | Rule | Why |
|---|---|---|
| Which triangles | every triangle of a primitive whose `emissiveFactor` (strength already folded in, `material_store.hpp`) is non-zero, textured or not; unlit materials are not lights | the sample's rule (all triangles of emissive geometry). An unlit color is a display rule, not emission into the scene |
| Light instance | one task per emissive `GpuRayInstance` row (node instance × primitive): `{firstLight, rayInstance, triangleCount}`, built by the host on layout change. Triangles in cluster order (meshlet, then meshlet triangle) | the ray-instance row is the unit a hit decodes to; cluster order matches hit ids |
| Hit → light | `lightIndex = firstLight(rayInstance) + triangleInPrimitive(meshletRow, triangle)`; `firstLight` per ray instance in a table parallel to `rayInstances` (`kNoneIndex` when not emissive) | the sample's `GeometryInstanceToLight` + primitive index. Whether `GpuMeshletDesc::triangleOffset / 3` gives `triangleInPrimitive` directly depends on meshoptimizer's padding of `meshlet_triangles`: §1 task 1 checks it, otherwise a per-meshlet triangle prefix is stored |
| Per-frame rows | `light_world` gets a second range: one thread per triangle light, the sample's binary search from thread to task, three vertices through the node instance's world matrix | the sample rebuilds every frame (`PrepareLights`); entities move without a CPU loop, like the punctual rows |
| Radiance | `emissiveFactor × emissive texture`, the texture sampled once at the UV centroid with the sample's gradients (short edge × 2/3, the median to it / 3), constant over the triangle, clamped at 0 | the sample's `PrepareLights`. The camera still sees per-texel emission through the `Emissive` guide; only the light it casts is per triangle |
| Sidedness | a single-sided material emits from its front face (`saturate(dot(L, −n))`), a double-sided one from both (`abs`) | matches what the raster shows. **Deviation:** the sample is always single-sided |
| Row layout | the world row keeps its 64 B; a `LightType.Triangle` row stores `v0`, `edge1`, `edge2` and the radiance in named fields (mirrored in `types.hpp` / `types.slang`, layout test). Asset `GpuLight` rows are unchanged: triangles have no asset rows | one row size keeps `RAB_LoadLightInfo` a single load; 48 B of triangle data fit |
| Sampling | `RAB_SamplePolymorphicLight`: a triangle takes a uniform area sample from `uv` (the sample's `SampleTriangle`) and returns its solid-angle pdf; `RAB_IsAnalyticLightSample` is false for triangles; `RAB_LightSampleSolidAnglePdf` returns that pdf; `RAB_TraceRayForLocalLight` returns the hit's light index and the `randXY` that maps back to the hit point | the sample's `TriangleLight` and bridge; RTXDI's light/BRDF MIS needs both directions |
| Source pdf | `RAB_EvaluateLocalLightSourcePdf` stays uniform over the local region until F1 | F1 owns the PDF texture |
| One emission rule | a ray hit adds its surface emission only when its triangle is not a light; light triangles reach every shading point through light sampling: ReSTIR DI at X₁, `directLightAt` at X₂ and later vertices (`indirect`, the `path_shading` ground truth, `nrc_update`). Camera pixels keep their `Emissive` guide (combine) | the sample's `enableIndirectEmissiveSurfaces = false`: counting a light both ways doubles it. Non-light emitters (unlit) still show in reflections |
| Capacity | no fixed light count: the per-slot `WorldLights` buffer grows as today (next power of two after the slot's fence), RTXDI's local region is `punctual + triangles` | limits come from the load, not from constants |
| History | the light index of a triangle is stable between layout changes; a load or unload resets history, as today | `RAB_TranslateLightIndex` stays the identity; no index mapping buffer (the sample keeps one for streamed instances) |

## 1. Tasks

1. **Facts first**, recorded in §Log: `triangleInPrimitive` from `triangleOffset` (meshoptimizer's `meshlet_triangles` padding in `geometry_store.cpp`); triangle counts of the test emitters (`EmissiveStrengthTest`, `DamagedHelmet`, `LightsPunctualLamp`).
2. Host (`vk_world_lights`, `vk_rt_scene`): the task table and the `firstLight` table on layout change, the triangle light count in `GpuFrameRoot`, the row order above; RTXDI's local region grows by the triangles (`vk_rtxdi.cpp`).
3. `passes/light_world.slang`: the triangle range.
4. `lighting/rtxdi_bridge.slang`: polymorphic `RAB_LightInfo`, triangle sampling, `RAB_TraceRayForLocalLight`; update the "Not built yet" header.
5. The emission rule in `passes/indirect.slang`, `lighting/path_shading.slang` and `passes/nrc_update.slang`, through one helper that takes the hit and returns its emission or 0.
6. UI: light counts (punctual, triangle, infinite). Guide capture shots: `EmissiveStrengthTest` (cubes over the backdrop), `DamagedHelmet` (textured emitter), `LightsPunctualLamp` (emissive bulb next to its punctual light).
7. Convergence check: a second scene, `EmissiveStrengthTest`, with the same gates as `LightsPunctualLamp` (ReSTIR DI against RTXDI's `None` reference).

## 2. Verification

| Check | Expected |
|---|---|
| Emission rule | on `EmissiveStrengthTest`, the multi-bounce ground truth with emission by light sampling (the rule) against emission by BRDF hits only (lights excluded from sampling, a temporary switch): same image within the noise floor; recorded in the log |
| Convergence check | passes on `LightsPunctualLamp` and `EmissiveStrengthTest` |
| Guide capture | the backdrop lit by each cube in proportion to its strength (1, 2, 4, 8, 16); `DamagedHelmet` lit by its emissive panels; punctual shots unchanged |
| BRDF candidate | with `numBrdfSamples` 1 (NVIDIA's `Medium`), `RAB_TraceRayForLocalLight` names lights on `EmissiveStrengthTest` (debug counter or capture), and the convergence check still passes |
| `ENGINE_GI_CHECK` | 8 of 8; Sponza has no emitters, so its numbers equal the E1 run |
| `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST` | unchanged |
| Validation and GPU-AV | clean at load, unload and a switch between emissive and non-emissive scenes |
| Timing (release, no layers, RTX 5070) | `light_world` and the ReSTIR DI passes on `EmissiveStrengthTest` and `DamagedHelmet`, against Sponza |

## Done when

- emissive triangles are RTXDI lights, a BRDF ray names them, and their light is counted once along every path;
- the convergence check passes on an emissive scene, and the emission-rule comparison is recorded;
- C-F3-4, C-R32-3 and the emissive part of C-F3-5 are closed.

## Must not

- add an emitting light triangle's emission at a ray hit in any path code;
- write triangle rows on the CPU per frame;
- route triangle lights through `evalLight` (it stays punctual only, [pbr_lighting_materials.md](../pbr_lighting_materials.md)).

## Log

| Date | Change |
|---|---|
| 2026-11-20 | Spec written |
