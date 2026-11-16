# M3 · B1 — Texture-transform verification

**Status** — Checked on RTX 5070 on 2026-11-14.\
**Implements** — The texture-transform acceptance check in PBR B1.\
**Depends on** — [M0 GPU packing](../m0/a1_gpu_contracts.md) and [M1 material sampling](../m1/b1_material_sampling.md).
[PBR plan](../pbr_implementation_plan.md)

## Contract

The loader resolves the slot's `texCoord` override. The sampler applies the
CPU-packed transform to its UV and applies only the linear part to its
gradients. The normal slot also adjusts the tangent frame using the inverse
linear transform so rotations, scales and mirroring preserve its axes.
The five core slots share the path.
[Asset loader](../../src/core/assets_loader.cpp) · [Material sampling](../../shaders/material/pbr.slang)

## Verification

`ENGINE_GUIDE_CAPTURE=1` records the asset views. On `TextureTransformTest`
the offset quads showed the checkmarks and the transform arrows pointed
at the expected green markers. `TextureTransformMultiTest` exercised UV0,
UV1 and sampled transforms across base color, metallic-roughness, normal,
occlusion and emissive. E1's AO guide now makes occlusion visible directly.
The captures passed with core, sync and NVIDIA best-practices validation.
[PBR plan, B1 and E1](../pbr_implementation_plan.md) · [Guide capture](../../src/core/engine_guide_capture.cpp) · [M3 E1](e1_guide_followups.md)
