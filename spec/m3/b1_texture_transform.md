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

## Decisions and math

Pack one matrix for each non-identity core slot at material upload.
For scale `sx, sy`, angle θ and offset `tx, ty`, the rows are
`(cosθ × sx, sinθ × sy, tx)` and
`(−sinθ × sx, cosθ × sy, ty)`. This rotation follows the v-down
convention checked against the fixtures. Matching flag bits guard the
matrix reads. Identity slots take the input gradients unchanged apart
from the normal LOD-bias scale.

Use `M × (uv, 1)` for a sample. Multiply the derivative vectors by
the 2×2 part only. Normal maps act in the transformed UV basis,
so use the inverse linear transform to adjust tangent and bitangent.
The determinant supplies orientation for mirrors. A singular transform
keeps the existing tangent frame.

## Tasks and acceptance

| Task | Result |
|---|---|
| Resolve texture coordinate overrides | Loader stores the effective UV set. Multi-test checks UV0 and UV1. |
| Apply one transform path to core textures | The guide captures exercise base color, metal/roughness, normal, occlusion and emission. |
| Validate normal direction and color-slot rotation | Rotated normal and arrow fixtures agree with their reference markings. |
| Keep future material extensions outside the core work | M7 owns extension material bins. |

[Packing code](../../src/core/vk_asset_buffers.cpp) · [Current sampler](../../shaders/material/pbr.slang)

## Verification

`ENGINE_GUIDE_CAPTURE=1` records the asset views. On `TextureTransformTest`
the offset quads showed the checkmarks and the transform arrows pointed
at the expected green markers. `TextureTransformMultiTest` exercised UV0,
UV1 and sampled transforms across base color, metallic-roughness, normal,
occlusion and emissive. E1's AO guide now makes occlusion visible directly.
The captures passed with core, sync and NVIDIA best-practices validation.
[PBR plan, B1 and E1](../pbr_implementation_plan.md) · [Guide capture](../../src/core/engine_guide_capture.cpp) · [M3 E1](e1_guide_followups.md)
