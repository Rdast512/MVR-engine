# M1 · V3 — Three-level Hi-Z build

**Status** — Built. RTX 5070 combined cull/preprocess/Hi-Z timing recorded.\
**Depends on** — [V2 reverse Z](v2_reverse_z.md).
[Visibility-buffer design](../features/visibility_buffer.md) · [A9 history](a9_reverse_z_hiz.md)

## Decisions and data flow

The port keeps the previous-power-of-two pyramid footprint and
far-depth min reduction. An 8×8 first-level tile yields its 4×4 and
2×2 descendants through groupshared storage. This amortizes dispatches
without a wave-size dependency. One mip-0 texel reduces up to three
depth texels per axis from the actual source extent. Later mips halve
each dimension down to 1.

1. Transition depth from attachment writes to compute sampling after
   each raster phase. Keep the `R32_SFLOAT` pyramid in `GENERAL`.
2. The first batch reads depth. Each later batch reads the preceding
   batch's last mip after its storage writes are visible. Clamp the
   batch output count to the remaining mips.
3. Early build feeds late culling. Late build feeds next-frame early
   culling and the present-time pyramid view. Freeze keeps the cull
   view's pyramid while still transitioning depth for the resolve.

The culling sampler uses min reduction. Mip selection must keep the
whole projected bounds in the footprint of one center fetch.
[Hi-Z host](../../src/render/vk_hiz.cpp) · [Reduction shader](../../shaders/passes/hiz_build.slang) · [Conservative fetch](../../shaders/core/culling.slang)

## Tasks and recorded results

| Task | State and evidence |
|---|---|
| Port min reduction and partial batches | Done. Typed writes and three mips per dispatch. |
| Order depth reads and inter-batch writes | Done. Phase recording and mip barriers. |
| Verify the footprint and integrated culling | RTX 5070 done. Footprint regression and 16/16 Sponza image matches. |
| Measure stage cost | Combined cull/preprocess/Hi-Z span recorded. An isolated Hi-Z result is absent from milestone acceptance. |

[Footprint test](../../tests/hiz_footprint_test.cpp) · [V7 timing](v7_cleanup_and_timings.md)
