# M4 · G6: combine

**Status:** built 2026-11-19: `passes/combine.slang` writes `noisyColor = directRadiance + indirectRadiance + emissive` · **Milestone:** M4
**Implements:** engine spec §11.6; plan G6 ([pbr_implementation_plan.md](../pbr_implementation_plan.md))
**Depends on:** [G5](./g5_restir_gi.md) (`indirectRadiance`)
**Used by:** [G7](./g7_indirect_modes.md): every indirect mode writes `indirectRadiance`, so combine does not change per mode

## Scope (from the plan)

- `combine` CS: `noisyColor = directRadiance + indirectRadiance + emissive` (RGBA16F). The only place the three terms meet.
- The `Lighting only` debug view and the DLSS-RR color input switch to the full sum (plan H1 input table). Changed in §1: the full sum is the `DLSS input` view, and `Lighting only` became `Direct lighting`.

## Done when

G5's open check: `Sponza` with the sun shows bounce light under the arches in `Lit`, stable after DLSS-RR. Met (§3).

## Must not

Apply AO to direct, add a second path loop, or touch SDK buffers outside their bridge files and host modules.

## 1. Decisions

| Topic | Decision | Why |
|---|---|---|
| Sum | a plain sum of the three render-resolution terms, no weights, no AO, no NaN guard | NVIDIA's RTXDI sample (`CompositingPass.hlsl`) sums diffuse + specular + emissive the same way; both terms here are already modulated radiance (G5 shades with the BRDF), AO stays on the cache term (engine spec §11.6) |
| Barrier | the combine's existing compute storage-write → sampled-read barrier covers `indirectRadiance` (written by `rtxdi_gi_shade` in the same ray submit) | no new barrier; only its comment names the new term |
| Debug views | `LightingOnly` → `DirectLighting` ("Direct lighting"), unchanged content (`directRadiance` + emissive); the full sum is the existing `DLSS input` view (`noisyColor`) | the ReSTIR DI convergence check and the RTXDI settings guide (docs/settings.md) compare the direct term against `Shadowed lights`, which has no indirect; a second view of `noisyColor` would duplicate `DLSS input` |
| Guide capture | adds `DLSS input` as its 15th view | one PNG of the noisy full sum per shot. The extra view starts the ReSTIR DI convergence run later, so its RNG and numbers shift (−0.055% → −0.146%, still passed) |

## 2. Files

| File | Change |
|---|---|
| `shaders/passes/combine.slang` | adds `indirectRadianceSampled` to the sum |
| `src/render/debug_view.hpp`, `shaders/types.slang`, `shaders/passes/debug_view.slang` | `DebugView::DirectLighting` / `kDebugViewDirectLighting` (index 24 unchanged), label `Direct lighting`; `DlssInput` comment names the full sum |
| `src/core/engine_guide_capture.{hpp,cpp}` | 15 views (`DlssInput` added); the convergence run uses `DirectLighting` |
| `src/ui/engine_ui.cpp` | the ReSTIR DI reference checkbox follows the rename |
| `src/render/vk_renderer.cpp` | combine barrier comment |

## 3. Verification

Release, RTX 5070, 2026-11-19.

| Check | Result |
|---|---|
| Validation: core + sync + best practices, `ENGINE_GUIDE_CAPTURE=1` | clean apart from the known `SpirvDeprecated_WorkgroupSize` ×3 and `VUID-vkDestroyDevice-device-05137` (C-G3-3) |
| ReSTIR DI convergence, normal checks | passed (−0.146%, p90 2.28%; RNG shifted by the added view, §1); 2 of 2; 180 PNGs |
| `DLSS input` = direct + indirect + emissive (guide PNGs, 8-bit display encoding, different frames) | `LightsPunctualLamp`: mean red 0.0288 vs `Direct lighting` 0.0266 and `Indirect radiance` 0.0115; `DirectionalLight` (direct ≈ 0 in view): 0.00247 vs indirect 0.00261 |
| Bounce light under the arches in `Lit`, after DLSS-RR (window captures of the startup gallery pose, default jitter, manual EV 0, before and after G6 by swapping `combine.spv`) | walls and pillars beside the sunlit opening are lit by bounce light (seen at ×12 ≈ +3.6 EV; at EV 0 the gallery stays near black, its absolute radiance); pixels above 2/255: 0.30% → 0.65% |
| Stable after DLSS-RR | frame-to-frame RMS between two captures 0.5 s apart: 0.15% of full scale with G6, 0.20% without |

Observed: on `MetalRoughSpheres` (no lights, no emission), loaded after lit shots, `Indirect radiance` is nonzero on 6% of the pixels (max 0.18 in the PNG) and `NRC cache` on 9%: the cache still predicts the previous scenes' light (considerations C-G3-5). Through the combine it now reaches `Lit`.

## Log

| Date | Change |
|---|---|
| 2026-11-16 | created from plan G6 |
| 2026-11-19 | built: indirect term in the combine; `Lighting only` → `Direct lighting`, the full sum is `DLSS input`; guide capture 15 views; verified (§3) |
