---
name: pbr-lighting-materials
description: Shading rules of the PBR plan — glTF 2.0 metallic-roughness BRDF, materials, KHR_texture_transform, KHR_materials_emissive_strength and KHR_lights_punctual on the engine_spec path (visibility-buffer resolve, RTXDI ReSTIR DI, SER indirect bounce + NRC + ReSTIR GI, OMM, DLSS-RR; NVIDIA RTX 20+ only). Use when writing BRDF, lights, materials, sampleMaterial, the resolve, shading, GGX, sampling glTF PBR textures, or editing brdf.slang / pbr.slang / lights.slang.
---

# PBR / lighting / materials — agent instructions

Human explainer (graphs, worked numbers): [pbr_lighting_materials.html](../docs/pbr_lighting_materials.html)  
Normative formulas: glTF 2.0 [§3.9 Materials](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#materials) and [Appendix B](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#appendix-b-brdf-implementation)  
Khronos extensions: [KHR_lights_punctual](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_lights_punctual), [KHR_texture_transform](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_texture_transform), [KHR_materials_emissive_strength](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_emissive_strength)  
Architecture: [engine_spec.md](../docs/engine_spec.md) §9, §11, §12  
Implementation tasks: [pbr_implementation_plan.md](./pbr_implementation_plan.md)  
Buffers, model slices, frame root: [resource_architecture.md](./resource_architecture.md)

This file is the normative shading part of the PBR plan. The engine spec owns the architecture, the task plan owns passes, buffers and order, and this file owns the math. When they disagree on math, fix whichever is wrong in the same change; on architecture, the spec wins.

Do not invent a second BRDF. Copy inputs from glTF Appendix B. Microfacet `D`/`G` may vary with GPU budget; `c_diff`, `f0`, `α` must not.

## Engine path

- Target is **NVIDIA RTX 20+** only. No vendor checks, fallbacks or ports; NVIDIA-only pieces (NRC, DLSS-RR, cluster AS, SER) are the target stack (spec §1.2).
- Lighting is only **RTXDI** ReSTIR DI (direct), one SER indirect bounce + **NRC** + **ReSTIR GI** (indirect), **OMM** (alpha on rays), **DLSS-RR** (denoise + upscale). No raster lighting, no reference path.
- Primary surface: **visibility buffer**. Raster writes depth + a 32-bit `(visibleRow, triangle)` payload. The compute resolve rebuilds the triangle, computes perspective-correct barycentrics and their screen gradients, runs `sampleMaterial`, and writes BRDF inputs (guides), not lit color.
- Rays: hits that are **shaded** go through one SER RT pipeline shape (raygen, miss, minimal closest-hit; shading in raygen). **Visibility** rays are ray queries. No any-hit. RT geometry is one CLAS per cluster + one cluster BLAS per primitive, TLAS built on the GPU (spec §10).
- One decode: a vis-buffer pixel and a ray hit both resolve to `(nodeInstance, cluster, triangle)` and build the same `Surface`; the same `sampleMaterial` shades both.
- Alpha `MASK`/`BLEND`: primary visibility cuts in the raster `AlphaTested` bin with `discard` at the cutoff, never in the resolve. Rays use OMM per cluster, forced to 2-state per instance, with the same `baseColor.a` rule (plan Phase D).
- BRDF home: Slang `material/brdf.slang` (`evalBrdf`, `sampleBrdf`, `pdfBrdf`, `envBrdfApprox`) and `lighting/lights.slang` (`evalLight`). Material fetch: `material/pbr.slang` (`sampleMaterial`). The resolve, the RTXDI bridge, the indirect raygen and the cache passes call these; none re-derives the math.
- Storage: [resource_architecture.md](./resource_architecture.md). Materials are one shared, deduplicated, refcounted table (`MaterialStore` on `Scene`). `GpuPrimitive::materialId` is an **absolute** row of `GpuFrameRoot.materials`; `kNoneIndex` means the default material (row 0). Shaders use `loadMaterial` semantics. No pass owns a second material table.

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

Specular sampling (`sampleBrdf`) uses GGX **VNDF**; the diffuse lobe is cosine-weighted. `pdfBrdf` must match `sampleBrdf` exactly, because RTXDI and ReSTIR GI divide by it.

## Sample glTF materials

Missing texture = `1`. The row stores `kNoneIndex` for it; fetch through `sampleSlot` with fallback 1 and never build a handle from `kNoneIndex`. `COLOR_0` is a linear extra multiplier on base color.

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
emissive  = emissiveFactor * emissiveStrength * sample_srgb(emissiveTex).rgb   // strength 1 without KHR_materials_emissive_strength
```

**Texture LOD:** outside fragment shaders, every fetch is `SampleGrad`. The resolve takes UV gradients from the analytic barycentric derivatives; ray hits take them from the ray cone. With upscaling, gradients are scaled by `exp2(lodBias)`, `lodBias = log2(render / display)`.

**`KHR_texture_transform`:** per slot, `uv' = M · uv + offset` with `M` = rotation × scale, and the same 2×2 `M` applied to both UV gradients. `texCoord` in the extension overrides the slot's UV set.

UV: stored as in the file, with no flip (glTF and Vulkan share a top-left origin). Normal: OpenGL +Y up, `scale` on XY then normalize, `B = cross(N,T)*tangent.w`. Missing `TANGENT` on a normal-mapped primitive is generated at load (MikkTSpace-compatible `meshopt_generateTangents`, plan A4); missing `NORMAL` becomes flat normals. A4 generates both at load; the resolve's fallbacks remain only as guards. Normals and tangents are mesh-local: use the node instance's normal matrix and negate `tangent.w` on mirrored placements (`FlipWinding`).

**Unlit (`KHR_materials_unlit`):** `sampleMaterial` returns `baseColor = 0`, `metallic = 0` and puts the color into `emissive`, so every consumer handles unlit with no special case.

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

**Emissive triangles** (M7) are lights too: RTXDI samples them as triangle lights with radiance = the triangle's `emissive` (including strength). They are not routed through `evalLight`. Emissive surfaces still add their own `emissive` term where they are seen directly.

## Must

- Linear lighting. sRGB decode only baseColor RGB and emissive RGB (hardware `*_SRGB` views).
- Floor α for GGX (`max(α, 1e-4)` or equivalent) so `roughness = 0` does not NaN.
- Guard visibility denominator.
- `doubleSided`: no cull (back faces are drawn by flipping winding in the mesh shader), `N = -N` on back faces **before** the BRDF; the bitangent follows the flipped normal.
- Alpha: not premultiplied. `OPAQUE` ignore A; `MASK` discard if `a < cutoff`; `BLEND` is treated as `MASK` with cutoff 0.5 (spec §12.4: no forward transparency pass).
- Emissive added, not multiplied by `NdotL` or AO.
- Guides hold BRDF inputs in linear space: `c_diff`, `f0`, **perceptual** roughness, `envBrdfApprox(f0, NdotV, roughness)` as specular albedo.
- Exposure / tonemap is engine-side, applied once after DLSS-RR. lux/candela are not SDR white.

## Must not

- sRGB-sample metal-rough, normal, or occlusion.
- Multiply direct light by AO. AO scales only the radiance-cache query term at the primary vertex.
- Drop the rendering-equation `NdotL` after using correlated `V` (already has the BRDF’s `1/(4 NdotL NdotV)`). Do not multiply `NdotL` twice either.
- Feed perceptual roughness into `D` (`α = roughness²`), or store `α` in the roughness guide.
- Treat unattached `lights[]` entries as scene lights.
- Apply Unity ORM packing (R=AO) to the metal-rough slot. Core ignores R there; occlusion is a separate textureInfo (often the same image).
- DirectX-flip normal Y unless the asset is known non-conformant.
- Premultiply baseColor by alpha before the BRDF.
- Alpha-test in the resolve, or use implicit-LOD `Sample` outside a fragment shader.

## Direct, indirect, denoise

Core glTF has no IBL, and the engine has none. Spec §11–12, plan Phases F–H:

- **Direct:** **RTXDI** ReSTIR DI picks which light to shade and shadow-tests it with one visibility ray. Light sources are punctual lights and, from M7, emissive triangles (`Power_RIS`). Its target pdf is `luminance(f · L_i · NdotL)` from the same `evalBrdf` / `evalLight`. The punctual loop above is the math RTXDI estimates; it is not a pass in the engine. Ground truth is RTXDI with resampling mode `None` and many initial samples.
- **Indirect:** one ray per pixel from the primary surface, direction from `sampleBrdf`. At the secondary hit: `sampleMaterial`, direct light through RTXDI sampling without reuse + one visibility ray, its emissive, then the **radiance cache** (NRC on every tier) for every further bounce. The cache term at the primary vertex is multiplied by `ao`. **ReSTIR GI** then resamples these samples temporally and spatially.
- **Combine:** `noisyColor = L_direct + L_indirect + emissive`, the only place the terms meet. Direct is never AO-scaled.
- **Denoise / upscale:** **DLSS-RR** on the noisy linear HDR. Guides: `c_diff` as diffuse albedo, `envBrdfApprox` as specular albedo, shading normal + **perceptual** roughness, hardware depth, unjittered motion.

Do not write a reference light loop, a homegrown cache or network, or a denoiser.

## Checks when a shaded asset looks wrong

| Symptom | First cause |
|---|---|
| Default mesh nearly black | metallic=1, roughness=1, no IBL |
| Neon plastic / grey metal | sRGB vs linear bind |
| Hot-spot black, ring lit | spot `dot(spotDir, L)` instead of `-L` |
| Crevices black in sunlight | AO on direct light |
| Highlight too sharp until high roughness | forgot `α = r²` |
| Glancing lights too bright/dark | missing or doubled `NdotL` |
| Dents look like bumps | DirectX normal Y |
| Unlit “sun” | light def with no node |
| Indirect lags behind moving lights | cache training paths missing or too few |
| Wrong material on a second model | `materialId` treated as model-relative instead of an absolute row |
| Textures shimmer or blur in the resolve | wrong UV gradients (missing `KHR_texture_transform` matrix on `ddx`/`ddy`, or no LOD bias with upscaling) |
| Lighting detached from the surface at grazing angles | ray origin not offset along `GeoNormal` |
| Alpha edges differ between raster and shadows | OMM baked with another cutoff or UV orientation than the raster `discard` |
