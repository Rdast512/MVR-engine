---
name: pbr-lighting-materials
description: Shading rules of the PBR plan — glTF 2.0 metallic-roughness BRDF, materials and KHR_lights_punctual on the RTXDI + RTXGI NRC + OMM + DLSS-RR path (NVIDIA RTX only). Use when writing BRDF, lights, materials, shading, GGX, IBL, sampling glTF PBR textures, or editing vk_pbr / brdf.slang.
---

# PBR / lighting / materials — agent instructions

Human explainer (graphs, worked numbers): [pbr_lighting_materials.html](./pbr_lighting_materials.html)  
Normative formulas: [gltf_parse_resources.md](./gltf_parse_resources.md) §5, §13.3, §14  
Khronos: Appendix B, [KHR_lights_punctual](../docs/gltf%202.0/extensions/Khronos/KHR_lights_punctual/README.md)  
Implementation tasks: [pbr_implementation_plan.md](./pbr_implementation_plan.md)  
Buffers, model slices, frame root: [resource_architecture.md](../docs/resource_architecture.md)

This file is the normative shading part of the PBR plan. The task plan owns passes, buffers and order; this file owns the math. When they disagree, fix whichever is wrong in the same change.

Do not invent a second BRDF. Copy inputs from §14. Microfacet `D`/`G` may vary with GPU budget; `c_diff`, `f0`, `α` must not.

## Engine path

- Target is **NVIDIA RTX only**. No AMD/Intel paths, vendor checks or fallbacks.
- Lighting is only **RTXDI** (direct), **RTXGI NRC** (indirect), **OMM** (alpha on rays), **DLSS-RR** (denoise + upscale). No fallback or reference path.
- Primary surface: raster G-buffer (mesh shader + DGC). The fragment shader runs `sampleMaterial` and writes BRDF inputs, not lit color. Lighting runs in compute with ray queries.
- RT scene: KHR BLAS per glTF primitive + one TLAS per frame. No RT pipeline, SBT, any-hit, or SER. See [pbr_implementation_plan.md](./pbr_implementation_plan.md) §2 and Phase C.
- Alpha `MASK`/`BLEND`: raster `discard` at the cutoff on primary. Rays use OMM forced to 2-state per instance, with the same `baseColor.a` rule (Phase D).
- BRDF home: Slang modules `material.brdf` (`evalBrdf`, `sampleBrdf`, `pdfBrdf`) and `lighting.lights` (`evalLight`). The RTXDI bridge and the NRC path loop (`pathtrace`, built as `nrc_update` / `nrc_query`) call these; none re-derives the math.
- Storage: [gltf_storage_plan.md](./gltf_storage_plan.md). Material rows live on the GPU, one slice per model; `MaterialStore` keeps only rows not yet uploaded. Shaders fetch a material with `loadMaterial` semantics: `materialId` is relative to the model's material slice, `kNoneIndex` means the default material (row 0 of `GpuFrameRoot.materials`). `vk_pbr.hpp` hosts passes, not a second material table.

## Canonical evaluation

```
c_diff = lerp(baseColor.rgb, 0, metallic)
f0     = lerp(0.04, baseColor.rgb, metallic)   // dielectric IOR 1.5
α      = roughness^2
F      = f0 + (1 - f0) * (1 - abs(VdotH))^5
f      = (1 - F) * c_diff / π + F * D(α) * G(α) / (4 * abs(NdotV) * abs(NdotL))
```

Height-correlated visibility (allowed; `V` already includes `1/(4 NdotL NdotV)`):

```
V = 0.5 / max(NdotL * sqrt(NdotV²(1-a2)+a2) + NdotV * sqrt(NdotL²(1-a2)+a2), 1e-7)
f_specular = D * F * V
```

Punctual:

```
L_o = emissive
for each node-attached light:
    if NdotL <= 0: continue
    L_o += f * L_i * NdotL
```

`L_i = color * intensity * attenuation * angularAttenuation`.

Current Appendix B does **not** multiply dielectric albedo by extra `(1 - 0.04)`. Energy conservation is `(1 - F)` on diffuse.

## Sample glTF materials

Missing texture = `1`. The row stores `kNoneIndex` for it; fetch through `sampleOr` with fallback 1 and never build a handle from `kNoneIndex`. `COLOR_0` is linear extra multiplier on base color.

| Slot | Space | Channels | Format |
|---|---|---|---|
| baseColor | sRGB RGB, linear A | albedo / metal F0; A coverage | `*_SRGB` |
| metallicRoughness | linear | **B metal, G roughness**; ignore R,A | `*_UNORM` |
| normal | linear | RGB; ignore A | `*_UNORM` |
| occlusion | linear | **R only** | `*_UNORM` |
| emissive | sRGB | RGB; ignore A | `*_SRGB` |

```
baseColor = factor * sample_srgb(baseColorTex) * COLOR_0
metallic  = saturate(metallicFactor  * mr.b)
roughness = saturate(roughnessFactor * mr.g)
ao        = 1 + strength * (R - 1)          // 1 if no map
emissive  = emissiveFactor * sample_srgb(emissiveTex).rgb
```

UV: engine already stores `v' = 1 - v`. Normal: OpenGL +Y up, `scale` on XY then normalize, `B = cross(N,T)*tangent.w`.

Defaults if `pbrMetallicRoughness` omitted: **metallic 1, roughness 1, white**. That is a rough metal, not grey plastic.

## Lights (`KHR_lights_punctual`)

A light definition is not a scene light until a **node** references it. Inherit node world TRS. Do **not** scale `intensity` / `range` / cone by node scale.

| Type | `L` | `intensity` | Attenuation |
|---|---|---|---|
| directional | `-normalize(nodeWorld*(0,0,-1,0))` | lux | 1 |
| point | `normalize(lightPos - P)` | candela | `1/d²`, or windowed if `range` |
| spot | same as point | candela | point × cone |

Range window (and **0** for `d >= range`):

```
max(min(1 - (d/range)^4, 1), 0) / max(d*d, 1e-4)
```

Spot cone — **sign**:

```
cd = dot(spotDir, -L)    // L toward light; on-axis must be +1
```

Khronos sample renderer: `dot(spotDirection, normalize(-pointToLight))`. Never `dot(spotDir, L)`.

```
scale  = 1 / max(0.001, cos(inner) - cos(outer))
offset = -cos(outer) * scale
angularAttenuation = saturate(cd * scale + offset)
angularAttenuation *= angularAttenuation
```

`inner` default 0, `outer` default π/4. Single-angle engines: use `outer`, inner = 0.

## Must

- Linear lighting. sRGB decode only baseColor RGB and emissive RGB (hardware `*_SRGB` or explicit).
- Floor α for GGX (`max(α, 1e-4)` or equivalent) so `roughness = 0` does not NaN.
- Guard visibility denominator.
- `doubleSided`: no cull, `N = -N` on back faces **before** the BRDF.
- Alpha: not premultiplied. `OPAQUE` ignore A; `MASK` discard if `a < cutoff`; `BLEND` over with straight alpha **after** lighting.
- Emissive added, not multiplied by `NdotL` or AO.
- Exposure / tonemap is engine-side. lux/candela are not SDR white.

## Must not

- sRGB-sample metal-rough, normal, or occlusion.
- Multiply punctual `NdotL` by AO. AO is indirect only: it is the starting throughput of the `nrc_query` path.
- Drop the rendering-equation `NdotL` after using correlated `V` (already has the BRDF’s `1/(4 NdotL NdotV)`). Do not multiply `NdotL` twice either.
- Feed perceptual roughness into `D` (`α = roughness²`).
- Treat unattached `lights[]` entries as scene lights.
- Apply Unity ORM packing (R=AO) to the metal-rough slot. Core ignores R there; occlusion is a separate textureInfo (often the same image).
- DirectX-flip normal Y unless the asset is known non-conformant.
- Premultiply baseColor by alpha before the BRDF.

## Direct, indirect, denoise

Core glTF has no IBL, and the engine has none. [pbr_implementation_plan.md](./pbr_implementation_plan.md) Phases F–H:

- **Direct:** NVIDIA **RTXDI** (ReSTIR DI) picks which light to shade and shadow-test. Its target pdf is `luminance(f · L_i · NdotL)` from the same `evalBrdf` / `evalLight`. The punctual loop above is the math RTXDI estimates. It is not a pass in the engine. Ground truth is RTXDI with resampling mode `None` and many initial samples.
- **Indirect:** RTXGI **NRC**. Every pixel traces a short path from the G-buffer with starting throughput `ao`; NRC ends it with a query that the network answers. Sparse training paths teach the network every frame. `noisyColor = L_direct + L_indirect + emissive`, where `L_indirect` already carries `ao`; direct is never AO-scaled.
- **Denoise / upscale:** **DLSS-RR** on the noisy linear HDR. Guides: `c_diff` as diffuse albedo, perceptual roughness (not `α`).

Do not write a reference light loop, a homegrown cache or network, or a denoiser.

## Checks when a shaded asset looks wrong

| Symptom | First cause |
|---|---|
| Default mesh nearly black | metallic=1, roughness=1, no IBL |
| Neon plastic / grey metal | sRGB vs linear bind |
| Hot-spot black, ring lit | spot `dot(spotDir, L)` instead of `-L` |
| Crevices black in sunlight | AO on punctual |
| Highlight too sharp until high roughness | forgot `α = r²` |
| Glancing lights too bright/dark | missing or doubled `NdotL` |
| Dents look like bumps | DirectX normal Y |
| Unlit “sun” | light def with no node |
| Indirect lags behind moving lights | NRC training paths missing or too few |
| Wrong texture on a second model | absolute `materialId` used instead of the model slice |
