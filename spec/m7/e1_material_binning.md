# M7 · E1: material binning

**Status:** built 2026-11-20; GPU-AV cannot instrument the execution set (C-E1-3), and the binned resolve costs +0.045 ms at the bench pose (C-E1-4) · **Milestone:** M7
**Implements:** engine spec §5 stages 8–9, §9.1, §9.2; plan E1, M7 part ("the resolve per material bin", [pbr_implementation_plan.md](../pbr_implementation_plan.md)); [shader_architecture_plan.md](../shader_architecture_plan.md) rows 8–9 (`classify`, per-bin `resolve`)
**Depends on:** M1 (the resolve and its per-pixel body), D3 (DGC state per bin), `DeviceGeneratedCommands` (`src/render/vk_dgc.*`)
**Considerations:** `M7 · E1` section: C-E1-3, C-E1-4; C-T-5 (NRC views differ run to run)
**Reference:** [reference/dgc_comparison.md](../../reference/dgc_comparison.md) §7, §9 (material-binned compute: `EXECUTION_SET` of compute shaders + `DISPATCH`, one sequence per bin); `reference/vk_device_generated_cmds-master` (`vk_ext_device_generated_commands.*`, `renderer_vkgen_ext.cpp`: execution sets, token layout, explicit preprocess; graphics only, so the compute layout follows the registry); [visibility_buffer.md](../features/visibility_buffer.md) §7 ("M7 binning"); `validusage.json` of SDK 1.4.363 for `VkIndirectExecutionSetShaderInfoEXT`, the `DISPATCH` token and `VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT`

Split the resolve's one tiled dispatch into a classification pass and one compute shader per material shader, launched by one DGC execute whose sequences the GPU writes. The per-pixel body does not change, so every guide stays bit-identical.

## Scope

| Item | Today | After E1 |
|---|---|---|
| Resolve | one 8×8-tiled dispatch over every pixel; the unlit model is a branch inside `sampleMaterial` | `classify`, then one DGC execute: one sequence per non-empty bin, an execution set of compute shader objects |
| Bins | — | `Background` (depth 0: zero guides and the motion of a direction at infinity), `Standard` (glTF metallic-roughness), `Unlit` (`KHR_materials_unlit`) |
| New images and buffers | — | `binIds` (R8_UINT, render resolution), per-bin tile lists and counts, the DGC sequences |
| Outputs | the §9.3 guides | unchanged, bit-identical |

## Decisions

| Decision | Rule | Why |
|---|---|---|
| Bin rule | one bin per material **shader**: `materialBin(const GpuMaterial&)` in `material_store.hpp`, mirrored in `material/pbr.slang` (as `isAlphaTested` is); `kMaterialFlagUnlit` → `Unlit`, else `Standard`. `Background` comes from depth 0, not from a material | engine spec §9.1 ("rows sharing a shader share a bin"). Unlit is the only second shading body today; later KHR extensions add bins (spec §9.4). Empty pixels still need their guides and motion written, and under the overlap guard only a bin may write a pixel |
| Pixel → bin | `classify` reads the payload chain `visibleRow → meshletRow → primitive → material → bin` | the cull pass writing a bin per visible row is the fallback if `classify` time shows that chain ([visibility_buffer.md](../features/visibility_buffer.md) §7) |
| Classification | one 8×8 group per resolve tile; each thread writes its pixel's bin to `binIds`; the group ORs the bins it saw (wave ops, then groupshared, without assuming a wave size), and one thread per present bin appends the tile with one atomic | engine spec §9.1. One append per (tile, bin), not per pixel |
| Capacity | each bin's tile list holds every tile of the render target; no overflow is possible, so no overflow counter | a tile appears at most once per bin; the size follows `renderExtent` on resize |
| Sequences | a one-group `binArgs` step after `classify` writes one sequence per non-empty bin (`EXECUTION_SET` index = bin, push data, `DISPATCH` {tileCount, 1, 1}) and the sequence count to `sequenceCountAddress` | the GPU decides which shaders run; empty bins cost no sequence. Splitting it from `classify` keeps `classify` free of a last-group handshake |
| Execution set | `VK_INDIRECT_EXECUTION_SET_INFO_TYPE_SHADER_OBJECTS_EXT` with the bin shaders, created with `VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT`; one compute indirect-commands layout (tokens `EXECUTION_SET`, `PUSH_DATA`, `DISPATCH`); explicit preprocess before the execute, as the raster bins do | engine spec §9.2; DGC is the engine's command path ([dgc_comparison.md](../../reference/dgc_comparison.md) §9). The 5070 reports compute in `supportedIndirectCommandsShaderStagesShaderBinding` (log: `shaderBind=0xff`); assert it at device creation |
| Overlap guard | each bin shader is `[numthreads(8, 8, 1)]`, launched with `groupCount = (tileCount[bin], 1, 1)`, maps its group to a tile from its list and returns early when `binIds` at its pixel is not its bin | engine spec §9.2: every pixel is written by exactly one bin |
| Shader bodies | entries `resolveBackgroundMain`, `resolveStandardMain`, `resolveUnlitMain` in `passes/resolve.slang` over today's per-pixel body; the unlit branch is decided per entry through a generic or a constant parameter, not the preprocessor. `sampleMaterial` keeps its runtime unlit branch for ray hits | plan E1: "the per-pixel body stays this one". Rays hit every material, so they cannot be binned |
| Push data | `{frameRoot, tileList}` per sequence; the bin's tile list address comes from the root plus the bin | small sequences; 64-bit fields stay 8-byte aligned |
| Buffers | `binIds`, tile lists, counts and sequences are written and read in the same frame on the graphics queue, so one copy serves both frame slots, behind barriers; counts are reset by a fill at the start of `classify` | resource architecture: GPU lists reset on the GPU each frame |
| Timing | GPU timer scopes `Classify` and `Resolve` (the preprocess and the execute). The per-bin split is measured once for the log, with the execute limited to one bin at a time in a temporary build | one DGC execute is one timer span |

## 1. Tasks

1. `materialBin` in C++ and Slang; `RasterBin`-style enum `MaterialBin { Background, Standard, Unlit }` with its count asserted on both sides.
2. `RenderTargets`: `binIds` (R8_UINT, render extent, named), a heap slot in `GpuTargets`; `GpuFrameRoot` gets the bin buffer address. Layout test entries.
3. `passes/classify.slang` (`classifyMain`, `binArgsMain`), registered in `CMakeLists.txt`.
4. `passes/resolve.slang`: the three entries and the overlap guard; the tiled dispatch goes away.
5. `vk_dgc`: the compute layout, the execution set, the preprocess region (the max with the raster layouts' regions, or a separate one), the state command buffer that binds the first bin shader, and `recordExecute` for the resolve. Name every object (`DgcResolveLayout`, `DgcResolveExecutionSet`, …).
6. `Renderer::recordResolve`: fill counts → `classify` → barrier → `binArgs` → barrier (`DRAW_INDIRECT` / command preprocess read) → preprocess → barrier → execute → the existing barriers to the guide readers.
7. Debug view `Material bin` (one color per bin, black on empty tiles), written by the guide capture for every shot; Tracy zones `Classify` and `Resolve`.

## 2. Verification

| Check | Expected |
|---|---|
| Guide capture, every shot and view, against a capture from the commit before E1 | bit-identical PNGs (the per-pixel body is unchanged); `Material bin` shows `Unlit` on `UnlitTest` and on the Unlit row of `TextureTransformMultiTest`, `Background` on empty pixels |
| `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST` | unchanged results |
| `ENGINE_GI_CHECK` | identical numbers to the D4 run (the check is deterministic per frame index and the guides are identical) |
| Validation (sync, best practices) and GPU-AV | clean, including the DGC preprocess / execute state match, at startup, resize and a scene switch |
| Timing (release, no layers, 1280×720, RTX 5070) | Sponza bench pose: the old resolve against `Classify` + `Resolve`, and the per-bin split; recorded in the log |

## Done when

- the resolve runs as one DGC execute over GPU-written sequences, with `classify` and the overlap guard;
- every guide is bit-identical to the single dispatch, and the runners give unchanged results;
- validation and GPU-AV are clean, and the timings are recorded.

## Must not

- let two bins write the same pixel, or leave a pixel unwritten;
- read bin counts back on the CPU, or size a launch from the CPU;
- replace the DGC execute with `vkCmdDispatchIndirect` per bin (engine spec §9.2: DGC is the command path);
- bin by material row instead of by shader.

## Log

| Date | Change |
|---|---|
| 2026-11-20 | Spec written |
| 2026-11-20 | Built. `MaterialBin` / `kMaterialBinCount` / `kMaterialTileSize` in `core/types.hpp` (mirrored in `types.slang`); `materialBin` in `material_store.hpp` and `material/pbr.slang`. `RenderTargets` owns `binIds` (R8_UINT, `MaterialBinIds`) and the `MaterialBins` buffer: `GpuMaterialBins` (3 tile counts, the sequence count, 3 × 40 B `ResolveDgcSequence`), then 3 tile lists of one `uint` (x \| y << 16) per render tile. `GpuTargets` gains `binIdsStorage` / `binIdsSampled` (216 B), `GpuFrameRoot` gains `materialBins` (208 B). `passes/classify.slang`: `classifyMain` (one 8×8 group per tile, `WaveActiveBitOr` then one groupshared `InterlockedOr` per wave, one append per present bin) and `binArgsMain` (one thread, compacted sequences in bin order). `passes/resolve.slang`: `resolveBackgroundMain` / `resolveStandardMain` / `resolveUnlitMain` over `resolveBackground` / `resolveSurface` (today's body), with the overlap guard in `binPixel`. `DeviceGeneratedCommands`: `DgcResolveLayout` (EXECUTION_SET, PUSH_DATA, DISPATCH; explicit preprocess), `DgcResolveExecutionSet` (initial shader `Background`, `Standard` and `Unlit` written by index), `DgcResolveStateCommandBuffer`, one `DgcResolvePreprocess` region for both slots. `Renderer::recordClassify` (WAR barrier, count fill, classify, binArgs) and `recordResolve` (sequence barrier, preprocess, execute). GPU timer scopes `Material classify` and `Vis-buffer resolve`; Tracy zones `Classify` / `Resolve`. Debug view `Material bin` (30), captured for every shot |
| 2026-11-20 | Deviations: (1) the Slang `materialBin` takes `GpuMaterial.flags`, not the row, so `classify` loads one field; the C++ rule takes the row. (2) The unlit body is a `bool isUnlit` overload of `sampleMaterial`, which each entry calls with a literal; ray hits keep the three-argument form, which derives it from the flags. (3) The PUSH_DATA token's `updateRange.stageFlags` is `VK_SHADER_STAGE_ALL`, as VUID-VkIndirectCommandsLayoutTokenEXT-type-11333 requires. (4) The device asserts compute in `supportedIndirectCommandsShaderStagesShaderBinding` and `maxIndirectShaderObjectCount ≥ 3`; `RenderTargets` asserts the tile count against `maxComputeWorkGroupCount[0]` and 16-bit tile coordinates |
| 2026-11-20 | Verification (release, RTX 5070, driver 621.01, no layers unless stated). Guide capture against a baseline built from the same tree with E1 stashed, with the view list unchanged: 316 PNGs each, every view without NRC bit-identical (232). The 84 that differ are all NRC views, and two runs of one E1 binary differ in 81 of the same views (C-T-5). With `Material bin` added (331 PNGs): `UnlitTest` reads only `Unlit` and `Background`; `TextureTransformMultiTest` shows its unlit row as three `Unlit` squares; Sponza is all `Standard`. Normal checks 2 of 2, ReSTIR DI convergence 2 of 2 with the baseline's numbers. `ENGINE_CULL_CHECK` 16 of 16, `ENGINE_MOTION_TEST` 13 of 13, `ENGINE_RT_CHECK` 454 of 454 and 318 of 318, `ctest` 7 of 7. `ENGINE_GI_CHECK` 8 of 8 in both builds; its 18 phases without NRC read the same numbers as the baseline (the D4-run numbers no longer apply: both builds include the uncommitted RTXDI fixes and noise reduction of the same day); the NRC phases differ by up to 1.7 pp (C-T-5). Core, sync and best-practice validation (NVIDIA checks on) over the guide capture (scene switches) and the motion test (resize): only the documented SDK diagnostics (WorkgroupSize from SDK compute pipelines, NGX's own SYNC hazard on its resources, the C-G3-3 shutdown leaks). GPU-AV disables itself on the indirect-bindable shaders and the process crashes (C-E1-3). A temporary build without the execution set (plain bin shaders, `vkCmdDispatchIndirect` per bin from the same buffer) ran both runners GPU-AV-clean, motion 12 of 13 (C-T-1), and was reverted |
| 2026-11-20 | Timing, Sponza bench pose (3000 frames, two runs each, release, no layers). Before: `Vis-buffer resolve` 0.0712–0.0756 ms. After: `Material classify` 0.0178–0.0184 ms + `Vis-buffer resolve` 0.1028–0.1033 ms. Per-bin split from swapped `classify.spv` variants: no sequence 0.0148 ms (preprocess, execute and barriers), `Background` only 0.0155, `Standard` only 0.1039, `Unlit` only 0.0157 (both nearly empty at this pose). GPU total 2.83–3.03 → 3.05–3.07 ms. Logged as C-E1-4 for M8 I2 |
