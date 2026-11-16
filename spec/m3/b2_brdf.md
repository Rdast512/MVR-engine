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

## Algorithm and numerical decisions

| Decision | Implementation | Reason |
|---|---|---|
| One evaluation for sampling and lighting | Evaluate `(1 − F) × cDiff / π + F × D × V`, with height-correlated visibility already including the GGX denominator. | F2's target PDF and final weight can use the same evaluated contribution. |
| Guard low roughness and grazing views | Floor GGX alpha at 0.001 and the view cosine at 1e−4 where required. Clamp Fresnel's input range. Lift a below-surface view direction for VNDF and its PDF. | Keep finite values near zero roughness and with steep normal maps. |
| Sample a view-dependent mixture | Choose the specular lobe using Fresnel and diffuse luminance at the view angle. For two lobes clamp to [0.1, 0.9]. Pure metal uses specular. | Both supported lobes receive samples even at grazing angles. |
| Use a visible-normal distribution | Stretch the local view, sample the spherical cap, unstretch and normalize the half vector, and reflect. The diffuse branch uses cosine-weighted hemisphere sampling. | The sample and the analytic solid-angle density represent the same distribution. |

`u.x` selects the lobe and `u.yz` determines direction. The reported
PDF mixes both lobe densities. Below-horizon GGX reflections are
rejected. Their probability remains outside the valid-hemisphere
integral, so the unit test adds the rejected fraction to that integral.

## Tasks and acceptance

| Task | State and gate |
|---|---|
| Implement shared glTF evaluation | Built. Worked fixtures compare `f × N·L` and PDF against hand-derived values. |
| Implement matched sample and PDF | Built. Test compares returned weight with evaluation and checks the integral independently. |
| Supply reconstruction's specular guide approximation | Built. E1 applies the unlit weight when writing the guide. |

[GPU test](../../tests/brdf_test.cpp) · [E1 guide interface](e1_guide_followups.md)

## Verification

The `brdf` CTest runs the Slang code on the GPU. It checks hand-derived
evaluation and PDF values, near-zero roughness finiteness, sampled weights
against evaluation, and the sampled integral against independent uniform
integration. The PDF integral plus the rejected-sample fraction must
sum to one within tolerance. The head-on white-plastic case at
roughness 0.5 expects `f × N·L = 0.29540` and solid-angle PDF
`0.41380`. The suite uses a 1e−3 relative tolerance for worked
values, 1e−4 for sample/evaluate consistency and 2% for integration.
It covers pure metal, a grazing dielectric, a tilted normal, mirror
roughness, a black material and views at or below the shading horizon.
[Host test](../../tests/brdf_test.cpp) · [Test shader](../../tests/shaders/brdf_numbers.slang) · [CTest registration](../../tests/CMakeLists.txt)
