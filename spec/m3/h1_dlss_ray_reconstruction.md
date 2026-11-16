# M3 · H1 — DLSS Ray Reconstruction

**Status** — Built and checked on 2026-11-14.\
**Implements** — PBR H1 and engine spec §12.1–12.2.\
**Depends on** — [E1 guides and jitter](e1_guide_followups.md), [F3 direct lighting](f3_restir_di.md) and [H2 exposure](h2_post_present.md).
[PBR plan](../pbr_implementation_plan.md) · [Settings](../../docs/settings.md) · [Considerations C-H1-1–7](../considerations.md)

## Feature lifetime and evaluate

`Dlss` owns NGX initialization, feature creation and evaluation.
It supplies required extensions during device setup. The RR
optimal-settings query sets the render extent. Quality is the default
and renders 853×480 at 1280×720 display. A mode, preset or swapchain
change recreates the feature and dependent render resources.
[NGX host](../../src/render/vk_dlss.cpp) · [Device setup](../../src/core/vk_device.cpp) · [Renderer](../../src/render/vk_renderer.cpp)

| Input | Contract |
|---|---|
| Color | Linear HDR `noisyColor`. In M3, combine adds direct radiance and emissive. M4 G6 adds indirect. |
| Depth and motion | Inverted hardware depth and low-resolution motion in render pixels, scale 1. |
| Guides | Diffuse albedo, specular albedo, and world normals with packed perceptual roughness. |
| Jitter | Raster offset converted from NDC to render pixels with the same sign. |
| Exposure | H2's R32F exposure image, written last frame. Pre-exposure is 1. |
| Output | Display-resolution RGBA16F HDR for H2. |

RR runs for Lit when the scene has lights. Debug views and the no-light
albedo path use the render-resolution display image. Returning to Lit
resets RR history. Cuts, layout or light-set changes and resource
recreation also reset it. Before evaluate, an empty legacy set selects
the driver heap for NGX's image view handles (C-H1-1). The renderer
rebinds its heaps after evaluate. C-H1-2 tracks validation messages inside
NGX, and C-H1-3 tracks the allocator link workaround.
[Combine](../../shaders/passes/combine.slang) · [Render targets](../../src/render/vk_render_targets.cpp) · [M4 G6](../m4/g6_combine.md)

## NGX and frame ordering decisions

| Boundary | Rule |
|---|---|
| Creation | Query NGX extensions, initialize with project ID, then create DLSSD in unified denoise mode with packed roughness and hardware depth. Use HDR, low-res motion and inverted-depth flags. External exposure supersedes NGX auto exposure. |
| Recreate | Apply the mode and preset when building the swapchain-sized resources. Propagate queried render size to the camera, targets, Hi-Z and RTXDI. Presets are hints and a driver application override can take precedence. |
| Prepare evaluate | Combine produces the linear input. Order its writes and prior NGX work before evaluation. Pass image views at their true render/display sizes, row-major view/projection, render pixel jitter, motion scale 1 and frame time. |
| Evaluate and return | Bind the empty compatibility set so NGX uses the driver heap. Evaluate into display-sized HDR. Rebind engine heaps and order RR writes before H2. |
| History | Cuts, scene/light layout changes, recreate and debug/Lit transitions invalidate reuse. A pending reset is consumed on the next eligible Lit evaluation. |

RR is a consumer of engine motion and jitter. Units are render pixels
even when the image is reconstructed to a larger display extent.
The jitter sign matches the raster offset: the historical still
comparison measured 0.000030 per-frame change with that sign versus
0.00064 with reversed sign on the tested Sponza setup.

## Tasks and recorded results

| Task | State and evidence |
|---|---|
| Initialize NGX and recreate features across modes | Done. All five modes switched under validation. At 1280×720 display, render sizes were 1280×720, 853×480, 742×418, 640×360 and 427×240. |
| Wire guides, depth, jitter, matrices and reset | Done. Guide, cull and motion checks passed with the recorded default settings. The forced-jitter motion check has its own caveat. |
| Isolate SDK binding and resource accesses | Done. C-H1-1 workaround plus the pre-evaluate barrier. Known SDK-internal messages remain C-H1-2. |
| Verify Lit stability and cost | RTX 5070 done. E3 courtyard and the Quality benchmark. Floor-tier measurements remain tracked in the PBR/engine acceptance. |

[NGX calls](../../src/render/vk_dlss.cpp) · [Jitter contract](e1_guide_followups.md) · [Considerations](../considerations.md)

## Verification

The RTX 5070 Quality bench recorded 1.14 ms for RR at 1280×720 display.
The mode switches, motion and cull checks and guide captures were checked
during bring-up. The Lit courtyard stability measurements are recorded
under [E3](e3_engine_sun.md). Behavior across modes and later RTXDI
defaults is recorded in the settings notes.
[PBR plan, H1](../pbr_implementation_plan.md) · [DLSS linkage test](../../tests/dlss_test.cpp) · [Settings](../../docs/settings.md)
