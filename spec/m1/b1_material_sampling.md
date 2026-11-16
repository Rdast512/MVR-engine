# M1 · B1 — Material sampling

**Status** — Core sampler done in M1. Texture-transform captures checked on 2026-11-14.\
**Implements** — PBR B1.\
**Depends on** — [M0 GPU material packing](../m0/a1_gpu_contracts.md) and [shading attributes](../m0/a4_shading_attributes.md).\
**Used by** — [E1 resolve](e1_resolve_guides.md) and M2 hit shading.
[PBR plan](../pbr_implementation_plan.md) · [PBR material rules](../pbr_lighting_materials.md)

## Slots

| Slot | Sample and fallback |
|---|---|
| Base color | sRGB view decodes RGB to linear. Multiply by the factor and interpolated vertex RGB. Opacity comes from factor alpha and sampled alpha. Missing texture uses the factor and vertex color. |
| Metallic-roughness | Linear B supplies metallic and G supplies perceptual roughness, each scaled by its factor and clamped. Missing texture leaves the factors. |
| Normal | Linear XY decoded to a signed tangent-space vector, with Z reconstructed so BC5 works. Scale XY and transform through the facing-corrected tangent frame. A missing map or degenerate tangent keeps the surface normal. |
| Occlusion | Linear R blended from 1 by occlusion strength. Missing texture yields 1. |
| Emissive | sRGB view decodes RGB before the packed emissive factor is applied. Missing texture leaves the factor. A1 folds emissive strength into that factor. |

## Sampling path

`Surface` provides UV0/UV1 and explicit gradients. `sampleSlot` checks the
texture and sampler sentinels before heap access and calls `SampleGrad`.
A slot's A1 transform acts on UVs and its linear part acts on the gradients.
The normal slot also adjusts the tangent frame using the inverse linear
transform. LOD bias scales the gradients by `exp2(lodBias)`. It was zero
in M1 and became `log2(render / display)` with H1.

Unlit moves the sampled color into emission and clears its diffuse response.
M3 adds a zero specular weight for unlit. The resolve samples visible
surfaces, while M6 owns alpha coverage at raster time.
[Material shader](../../shaders/material/pbr.slang) · [Surface reconstruction](../../shaders/geometry/surface.slang) · [Texture color space](../../src/core/texture_color_space.hpp)

## Shader contract and fallback logic

Color views decode RGB from sRGB in hardware. Alpha remains coverage
data. The sampler checks both texture and sampler indices before
creating a handle. The default texel value of 1 passes the factors
through. Normal mapping additionally requires valid handedness and
a nondegenerate tangent after orthogonalizing it against the normal.

Decode normal XY to `[-1, 1]`, reconstruct Z from XY, scale XY by
normal strength and normalize after transforming through the world
tangent basis. This supports BC5 and keeps the glTF +Y convention.
The shared surface provides facing and handedness under mirroring.
[Color-space mapping](../../src/core/texture_color_space.hpp) · [Surface](../../shaders/geometry/surface.slang)

## Tasks and acceptance

| Task | State and evidence |
|---|---|
| Centralize core sampling and missing-slot logic | Done. Raster resolves sampled material through B1. M2 ray hits supply their own gradients. |
| Preserve sidedness and normal mapping | Done. A4 tangent and E1 two-sided comparisons. |
| Verify slot transforms | Done in [M3 B1](../m3/b1_texture_transform.md), covering UV0, UV1 and normal-frame adjustment. |

## Verification

The M1 `MetalRoughSpheres` guide capture showed the expected metallic,
dielectric and perceptual-roughness patterns. The M3 follow-up checked
`TextureTransformTest` and `TextureTransformMultiTest` across the five
core slots and both UV sets, including the transformed normal frame.
[PBR plan, B1 and E1](../pbr_implementation_plan.md) · [Guide capture](../../src/core/engine_guide_capture.cpp)

## Log

| Date | Change |
|---|---|
| 2026-11-13 | Guide evidence recorded for core material sampling |
| 2026-11-14 | Texture-transform captures and M3 sampler follow-ups recorded |
