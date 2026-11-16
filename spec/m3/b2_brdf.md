# M3 · B2 — Metallic-roughness BRDF

**Status** — Built on 2026-11-09.\
**Implements** — PBR B2 and engine spec §9.4.\
**Depends on** — [B1 sampled material](../m1/b1_material_sampling.md).\
**Used by** — [F2 shading](f2_rtxdi_bridge.md) and [M4 indirect](../m4/g2_indirect_pass.md).
[PBR plan](../pbr_implementation_plan.md) · [PBR material rules](../pbr_lighting_materials.md)

## Evaluation and sampling

- `fromMaterial` computes `cDiff = baseColor × (1 − metallic)` and
  `f0 = lerp(0.04, baseColor, metallic)`.
- `evalBrdf` combines Schlick Fresnel, GGX with height-correlated Smith
  visibility and Fresnel-weighted Lambert diffuse. The roughness input
  stays perceptual and GGX uses `max(roughness², 0.001)` internally. The
  caller applies incoming radiance and `N·L` once.
- `sampleBrdf` selects GGX visible-normal or cosine diffuse sampling. The
  lobe mixture and `pdfBrdf` agree in solid angle. A valid sample returns
  `f × N·L / pdf`, and a rejected direction returns zero PDF and weight.
- `envBrdfApprox` produces the specular albedo guide. The guide and bridge
  apply the unlit material weight per [E1](e1_guide_followups.md).

[BRDF module](../../shaders/material/brdf.slang)

## Verification

The `brdf` CTest runs the Slang code on the GPU. It checks hand-derived
evaluation and PDF values, near-zero roughness finiteness, sampled weights
against evaluation, and the sampled integral against independent uniform
integration. The PDF integral plus the rejected-sample fraction must
sum to one within tolerance.
[Host test](../../tests/brdf_test.cpp) · [Test shader](../../tests/shaders/brdf_numbers.slang) · [CTest registration](../../tests/CMakeLists.txt)
