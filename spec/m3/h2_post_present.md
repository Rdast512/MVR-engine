# M3 · H2 — Exposure, tonemap and present

**Status** — Built on 2026-11-14. Metering updated by the [dark-area exposure fix](../fixesM1-6/auto_exposure_dark_boiling.md).\
**Implements** — PBR H2 and engine spec §12.3.\
**Depends on** — [H1 reconstruction](h1_dlss_ray_reconstruction.md) and [M1 present](../m1/v4_v5_visibility_raster.md).
[PBR plan](../pbr_implementation_plan.md) · [Considerations C-H2-1–3](../considerations.md)

## Lit post chain

1. `exposureHistogramMain` builds a 256-bin log-luminance histogram of the
   display-resolution RR output over the 2⁻¹² to 2¹² range. It includes
   black in bin 0 and splits finite samples between adjacent bins in
   fixed point.
2. `exposureAdaptMain` reads and clears the histogram. It meters the
   40th–95th percentiles toward 0.18 with exposure compensation and EV
   bounds, with a default maximum of +6 EV. Each frame keeps
   `exp2(−dt × speed)` of the existing EV gap. The default speed is 2
   when metered luminance rises and 1 when it falls. Manual exposure
   and snap requests apply the target immediately. If no pixels are
   finite, automatic exposure holds the current value inside the EV range.
3. `postMain` applies exposure and one ACES fitted curve into a linear
   display-resolution RGBA16F target. Present samples that target into
   the sRGB swapchain and renders ImGui. Debug views keep their own
   render-resolution display path.

`PostProcess` writes the 1×1 R32F exposure image and per-slot readback
statistics for the UI. Tonemapping uses this frame's exposure. NGX
receives scene-linear input with unity pre-exposure. Resize initializes
exposure and requests a snap.
[Post host](../../src/render/vk_post.cpp) · [Exposure shader](../../shaders/passes/exposure.slang) · [Tonemap](../../shaders/passes/post.slang) · [Present](../../shaders/passes/present.slang)

## Exposure math and data dependencies

Each 16×16 group builds a local histogram and merges nonzero bins
into the global buffer. One 256-lane reduction group copies and
clears those bins, then evaluates the weighted overlap of each
bin with the 40th–95th percentile interval. Use each bin's log
luminance for the mean. The automatic target in EV is
`clamp(log2(0.18) − meanLog2 + compensationEv, minEv, maxEv)`.
Manual mode uses the clamped manual EV. Adapt from a valid prior
value in log space, then exponentiate for the exposure texture.

| Edge | Data and result |
|---|---|
| H1 to histogram | RR output must be readable before histogram sampling. Previous histogram clear must be complete. |
| Histogram to adapt | Global bin updates are visible before the reduction reads and clears them. |
| Adapt to tonemap | New R32F exposure is visible to this frame's post pass and becomes the next frame's adaptation history. |
| Tonemap to present and telemetry | Linear post color becomes fragment-readable for sRGB presentation. Exposure readback receives a compute-to-host dependency and is collected after the slot wait. |

C-H2-1 tracks FP16 headroom because the HDR input uses unity pre-exposure.
C-H2-2 and C-H2-3 record the exposure feedback and black-pixel fixes.
[Meter shader](../../shaders/passes/exposure.slang) · [Recording barriers](../../src/render/vk_post.cpp)

## Tasks and recorded results

| Task | State and evidence |
|---|---|
| Implement percentile exposure and manual/snap settings | Done. Meter and adaptation recorded on the Sponza + lamp bench. |
| Provide exposure to H1 and keep synchronization GPU-side | Done. Resize initializes exposure and requests a snap. The frame root retains scalar exposure 1. |
| Apply one fitted ACES curve for Lit | Done. Recorded peak post value 0.97 on the exposure test. |
| Verify guide/presentation compatibility and stage cost | RTX 5070 done. Motion 13/13, culling 16/16 and post timing below. Known NGX messages remain C-H1-2. |

## Verification

On the RTX 5070 Sponza + lamp bench, exposure moved from 2.64 at frame 20
to 1.39 at frame 200 toward a target near 1.2. The 6,000-frame Release
bench at 1280×720 display recorded 0.030 ms mean post time.
[PBR plan, H2](../pbr_implementation_plan.md)
