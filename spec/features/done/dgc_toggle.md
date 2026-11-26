# Feature: DGC on/off (full DGC pipeline vs plain indirect)

**Date:** 2026-11-26 · **Status:** DT1–DT7 implemented 2026-11-26; correctness verified, RTX 5070 perf measured (§7); **default switched to no DGC 2026-11-26 (user)**, `ENGINE_DGC=1` runs DGC; 2060 Super open · **Milestone:** tooling (perf decision), independent of the M8 steps
**Implements:** a startup switch `ENGINE_DGC=0|1`. Off, the engine records no DGC command at all:
- the vis-buffer raster becomes one `vkCmdDrawMeshTasksIndirectCountEXT` per (phase, raster bin);
- the material resolve becomes one `vkCmdDispatchIndirect` per material bin.

**Why:** measure the full DGC pipeline against a no-DGC one on the RTX 5070 and the 2060 Super, then decide whether DGC stays the engine's GPU-driven command path (§5).
**Related:**
- [reference/dgc_comparison.md](../../../reference/dgc_comparison.md) §7.1 (plain indirect draw rejected in 2026-10 without numbers), §9 (planned DGC uses);
- [docs/engine_spec.md](../../../docs/engine_spec.md) §8.2 (DGC row), §9.2 (resolve), §13 (preprocess → execute);
- considerations C-E1-3 (the per-bin resolve path this reuses).

**Touches:**
- `src/core/vk_device.cpp`;
- `shaders/passes/mesh_raster.slang`, `shaders/passes/classify.slang`;
- `src/render/vk_shader_objects.{hpp,cpp}`, `src/render/vk_dgc.{hpp,cpp}`, `src/render/vk_meshlet_culling.{hpp,cpp}`, `src/render/vk_renderer.cpp`;
- `src/checks/check_harness.cpp`.

---

## 0. Decision

| Point | Choice | Why |
|---|---|---|
| What switches | every DGC use: raster (2 phases × 2 bins) and the resolve execute | the decision is about the whole command path, not one pass |
| When | startup only (`ENGINE_DGC`, default `1`) | off must be what a DGC-free engine would be, not DGC with its commands skipped (see the next rows) |
| DGC objects when off | not created: no indirect commands layouts, no state command buffers, no execution set, no preprocess buffers | memory and startup cost are part of the comparison |
| Resolve shaders when off | created without `INDIRECT_BINDABLE` | the flag may change driver codegen; off must not carry it |
| Extension when off | `VK_EXT_device_generated_commands` stays enabled on the device | enabling it records nothing. Dropping it from the required set belongs to the "move" outcome (§5) |
| GPU work that feeds the commands | the same in both modes: cull, `buildMeshSequences`, classify, tile lists, every shader body | the A/B isolates the command path only |

## 1. Raster without DGC

`buildMeshSequences` (`meshlet_cull.slang:388`) keeps writing the same 32 B `MeshDgcSequence`s and per-bin `sequenceCount`. Off, they feed:

```
vkCmdDrawMeshTasksIndirectCountEXT(
    buffer            = DgcSequences_<slot>,
    offset            = binSequenceOffset(phase, bin) * 32 + offsetof(MeshDgcSequence, draw),   // 16
    countBuffer       = CullCounters_<slot>,
    countBufferOffset = phase * sizeof(GpuCullCounters) + offsetof(GpuCullCounters, sequenceCount) + bin * 4,
    maxDrawCount      = binMaxSequences[bin],
    stride            = sizeof(MeshDgcSequence))                                                  // 32
```

- Both buffers already have `eIndirectBuffer` (`vk_dgc.cpp:323`, `vk_meshlet_culling.cpp:86`).
- The cull output barrier already targets `eDrawIndirect` / `eIndirectCommandRead` (`vk_meshlet_culling.cpp:543-546`).

**Push data.** A non-DGC draw cannot change push data per draw. The host pushes one `MeshPushData` per (phase, bin) and the mesh shader adds the draw index:

```
host push:  { frameRoot, firstVisible = binVisibleOffset[phase][bin], meshletCount = perSequence }
shader:     firstVisible = push.firstVisible + drawIndex * push.meshletCount
```

This reproduces each sequence's `firstVisible` exactly. The last draw's shorter batch comes from its indirect `groupCount.x`, and the shader reads nothing else from `meshletCount`: `frame.slang:77` reads only `frameRoot` and `firstVisible`.

**Rejected alternatives:**
- *`sequences[drawIndex].push` loaded in the shader:* an extra dependent load per workgroup, and a second push layout.
- *`drawIndex` in the DGC entry points:* changes the DGC shaders under test, and `DrawIndex` under DGC is not something to rely on.
- *`vkCmdDrawMeshTasksIndirectEXT` with `drawCount = 1`:* an empty bin leaves sequence 0 stale, and "Meshlets / sequence" can force several sequences.

## 2. Resolve without DGC

The per-bin path exists since 2026-11-20, as a GPU-AV workaround (C-E1-3, `ENGINE_GPUAV_RESOLVE_PER_BIN`):

- `binArgsPerBinMain` writes one `ResolveDgcSequence` per bin into a fixed slot, with 0 groups when the bin is empty;
- `vk_renderer.cpp:1013-1027` binds each bin's resolve shader, pushes `ResolvePushData`, and calls `dispatchIndirect` on that slot's `dispatch`;
- the resolve shaders are created without `INDIRECT_BINDABLE`.

Off reuses it unchanged, so `ShaderObjects::isResolvePerBin = isGpuAvResolvePerBinRequested() || !isDgcEnabled`. The comments that call it "TEMPORARY GPU-AV only" change to "no-DGC and GPU-AV path".

## 3. Steps

### DT1: switch and device (`vk_shader_objects.{hpp,cpp}`, `vk_device.cpp`)

- `[[nodiscard]] bool isDgcRequested()`, next to `isGpuAvResolvePerBinRequested`:
  - `ENGINE_DGC` unset or `1` → true; `0` → false;
  - any other value logs an error and keeps DGC.
- `ShaderObjects::isDgcEnabled`; the startup log reads `Command path: DGC` or `Command path: plain indirect (ENGINE_DGC=0)`.
- Enable `PhysicalDeviceVulkan11Features::shaderDrawParameters` (the SPIR-V `DrawParameters` capability behind `BuiltIn DrawIndex`) and `PhysicalDeviceVulkan12Features::drawIndirectCount` in the `ENGINE_FEATURE` table. Both are reported by the 2060 Super and the 5070.
- `meshletsPerSequence` (`vk_meshlet_culling.cpp:374`) also bounds sequences by `maxDrawIndirectCount`.

### DT2: shaders

- `mesh_raster.slang`:
  - `rasterMeshlet` takes the `MeshPushData` as a parameter;
  - `meshMain` / `meshAlphaMain` pass the global `push`;
  - new `meshIndirectMain` / `meshAlphaIndirectMain` take `uint drawIndex : SV_DrawIndex` (Slang → `BuiltIn DrawIndex`, allowed in `MeshEXT` by VK_EXT_mesh_shader) and build the push of §1.
  - Same file and push block, so no new pass in `CMakeLists.txt`.
  - Check by a SPIR-V disassembly diff that the DGC entries change only in how the push is passed.
- `classify.slang`: only the comment on `binArgsPerBinMain`.

### DT3: shader objects

- When off, `createMeshShaders` builds `visRaster` / `visRasterAlpha` from the indirect mesh entries. The fragment shaders are the same.
- `createResolveShaders` already drops `eIndirectBindable` under `isResolvePerBin`.
- `createCullShaders` already picks `binArgsPerBinMain` under `isResolvePerBin`.

### DT4: `DeviceGeneratedCommands` (`vk_dgc.{hpp,cpp}`)

It keeps owning the sequence buffer, because the indirect draw reads it too.

- `init()` when off: no layouts, no state command buffers, no resolve objects. The log reads `DGC disabled: sequence buffers only`.
- `allocatePreprocessBuffer` is skipped when off.
- `prepareFrame` also takes the counters `vk::Buffer`, because the count draw takes a buffer and an offset.
- `void recordIndirectDraw(const vk::raii::CommandBuffer& cmd, CullPhase phase, RasterBin group, const MeshPushData& push) const`:
  - returns when `binMaxSequences[group] == 0`;
  - binds the mesh state and pushes the data;
  - records the draw of §1.
- `recordPreprocess`, `recordExecute` and `recordResolve*` throw `std::logic_error` when off. A missed branch fails loudly instead of recording DGC.
- The class comment states both modes.

### DT5: push data (`vk_meshlet_culling.{hpp,cpp}`)

`[[nodiscard]] MeshPushData indirectDrawPush(CullPhase, RasterBin, vk::DeviceAddress frameRoot) const`. It returns `{frameRoot, views[phase].binVisibleOffset[bin], perSequence}` of the recorded frame: the same numbers `writeCullView` hands to `buildMeshSequences`, kept in one place.

### DT6: frame recording (`vk_renderer.cpp`)

- **Raster (`:858-926`):**
  - off: no `dgc.recordPreprocess`;
  - `recordIndirectDraw` with `culling.indirectDrawPush(...)` in place of each `recordExecute`, opaque first as now;
  - the `EarlyPreprocess` / `LatePreprocess` timer scopes stay and read ≈0, so `[Bench]` rows match between modes.
- **Resolve (`:1013`):** already branches on `isResolvePerBin`; nothing changes beyond the comment.
- **Barriers:** the cull and classify barriers keep `eCommandPreprocessEXT` in both modes. Preprocess stages are valid while the extension is enabled, and the barriers stay mode-independent.

### DT7: checks and bench (`check_harness.cpp`)

- The bench keeps refusing `ENGINE_GPUAV_RESOLVE_PER_BIN`. It accepts `ENGINE_DGC=0`, the measured no-DGC path.
- `[Bench]` gets a header line `command path: DGC|plain indirect`, so every result names its mode.

## 4. Verification and measurement

Run from a checks tree (`windows-clang-release-checks`), with validation off for the runners and benches.

**Correctness**

| Check | Mode | Pass |
|---|---|---|
| Build | OFF tree and checks tree | no new warnings |
| `mesh_raster.spv` | DGC entries | disassembly unchanged apart from the push parameter |
| `ENGINE_CULL_CHECK=1` | `ENGINE_DGC=0` | 16/16 |
| `ENGINE_RT_CHECK=1` | `ENGINE_DGC=0` | 32/32 |
| `ENGINE_STABILITY_CHECK=1` | `=1`, then `=0` | runner lines identical from the subsystem column on |
| `ENGINE_GI_CHECK=1` | `ENGINE_DGC=0` | 8/8 (the resolve's per-bin path, once more) |
| Many sequences | `=0`, UI "Meshlets / sequence" = 64, Sponza | image matches `=1` at the same value; `sequenceCount` > 1 per bin |
| Validation | 300 frames per mode, vkconfig layers on, GPU-AV off | no new messages |
| GPU-AV | `=0`, GPU-AV on, 100 frames | runs: no `INDIRECT_BINDABLE` shader exists (C-E1-3). This is input for §5, not a pass gate |

**Performance**

- **Bench:** `ENGINE_BENCH_FRAMES=6000`, validation off. Alternate `ENGINE_DGC=1` and `=0` over 2 rounds (noise is ±5%).
- **Scenes:**
  - Sponza (default);
  - `ENGINE_BENCH_FIELD=32` (instance-heavy cull, most visible meshlets);
  - Bistro (`ENGINE_MODEL=models/bistro/bistro.gltf`), which bins three resolve shaders.
- **GPUs:** RTX 5070, then the 2060 Super (the floor decides ties).

| Measure | Source |
|---|---|
| Raster command cost | `EarlyPreprocess + LatePreprocess + EarlyDraw + EarlyAlphaDraw + LateDraw + LateAlphaDraw` |
| Resolve | `Resolve` scope |
| Frame | GPU frame total; CPU frame split (`[Bench]`) |
| CPU recording | Tracy `DgcPreprocess`, `DrawCalls`, resolve zones |
| Memory | `GPU/DgcPreprocess` + resolve preprocess bytes (Tracy memory plot), which off does not allocate |

The results go in a table in [dgc_comparison.md](../../../reference/dgc_comparison.md) §7.1, with driver version and resolution.

## 5. Choosing: stay or move

The user decides from §4. The spec only fixes what each outcome changes.

**What weighs beyond the timings:**
- planned DGC uses (dgc_comparison §9): per-material raster execution sets and compute work. Without DGC they become per-bin binds and draws, as §1 / §2 do today;
- GPU-AV on the resolve (C-E1-3): it works without DGC and stays blocked with it;
- code: `vk_dgc.cpp` (~470 lines), state command buffers, the preprocess barriers.

| Outcome | Changes |
|---|---|
| **Stay** | `ENGINE_DGC=0` stays as the measuring switch. §7.1 records the numbers. The memory "DGC is the GPU-driven path" gets the measured reason |
| **Move** | a follow-up spec: plain indirect becomes the only path, `vk_dgc.*` shrinks to the sequence buffer, the extension leaves the required set, `INDIRECT_BINDABLE` and the DGC barriers go. Updates: engine spec §2 extension table, §8.2, §9.2, §13, §16; C-E1-3 closes; the DGC memory is rewritten |

## 6. Docs to update with the implementation

- `docs/settings.md`: the `ENGINE_DGC` row in the env table.
- `docs/engine_spec.md` §8.2 / §9.2: "ENGINE_DGC=0: plain indirect for measurement (spec/features/planned/dgc_toggle.md); DGC is the default".
- `spec/considerations.md` C-E1-3: the per-bin path is now also the no-DGC path.
- `reference/dgc_comparison.md` §7.1: link here, add the results.

## 7. As built (2026-11-26)

**Differences from the steps:**
- `CMakeLists.txt`: the `mesh_raster.slang` pass lists the two new entries (`ENTRIES` is explicit). It is still one pass.
- `Renderer::recordRasterBin(cmd, phase, bin)` holds the DGC / indirect branch, so `recordPhase` keeps one call per bin.
- `DeviceGeneratedCommands` stores the count buffer as `sequenceCountBuffer`. With DGC off, its preprocess allocation is skipped in `ensureFrameCapacity`.
- `meshletsPerSequence` bounds sequences by the min of `maxIndirectSequenceCount` and `maxDrawIndirectCount`.

**Verification:** RTX 5070, release checks tree, validation off unless stated.

| Check | Result |
|---|---|
| Build, OFF tree and checks tree | clean; `ctest` 7/7 and 8/8 |
| `mesh_raster.spv` DGC entries | same instructions; only the push loads move before the builtin loads, and one `IAdd` swaps its operands. `spirv-val` passes |
| `ENGINE_DGC=0`, `ENGINE_CULL_CHECK` | 16 of 16 |
| `ENGINE_DGC=0`, `ENGINE_RT_CHECK` | 32 of 32 |
| `ENGINE_DGC=0`, `ENGINE_GI_CHECK` | 14 of 14 |
| `ENGINE_STABILITY_CHECK`, `=1` vs `=0` | 30 runner lines identical |
| Many sequences | a temporary build with `maxMeshletsPerSequence = 64` (164 sequences per slot), then reverted. Stability lines identical to the default DGC run in both modes; RT 32 of 32 with `=0` |
| Core + sync + best practices, 300 frames per mode | the same messages in both modes, and only known ones: `SYNC-HAZARD-WRITE-AFTER-WRITE` ×2 (NGX, C-H1-2), `WorkgroupSize` ×3, `vkDestroyDevice-05137` (NRC, C-G3-3) |
| GPU-AV, `=0`, 100 bench frames | runs to the end. Only `vkDestroyDevice-05137` and the layer's settings notes: no C-E1-3 crash without `INDIRECT_BINDABLE` shaders |

**Perf, RTX 5070** (2026-11-26, driver 621.40, release checks tree, validation off):

- **Setup:** `ENGINE_BENCH_FRAMES=6000` (50 windows of 120 frames after 240 warm-up frames). Render 853×480, display 1280×720 (DLSS-RR). Order per scene: on, off, off, on.
- **Values:** mean ms of the two rounds; "off − on" is plain minus DGC.
- **Raster commands:** `Early/Late DGC preprocess` + `Early/Late mesh/alpha draw`.

| Scene (visible meshlets) | Raster commands on → off | Preprocess | Draws | Resolve on → off | GPU total on → off |
|---|---|---|---|---|---|
| Sponza (566) | 0.057 → 0.022 | −0.022 | −0.013 | 0.094 → 0.075 (−20%) | 2.848 → 2.799 (−0.049, −1.7%) |
| Field 32×32 (1501) | 0.058 → 0.030 | −0.022 | −0.006 | 0.093 → 0.075 (−19%) | 2.862 → 2.822 (−0.040, −1.4%) |
| Bistro startup view, outside the café (203) | 0.113 → 0.067 | −0.036 | −0.010 | 0.096 → 0.069 (−28%) | 3.010 → 2.964 (−0.046, −1.5%) |

**Reading:**
- Every scene and every round agrees. Off is faster in each scope, and the scope deltas are well above the round-to-round spread (≤0.004 ms on the raster scopes).
- The GPU total gain, 0.04–0.05 ms, is under the ±5% total-noise rule. It still repeats in all 6 pairs, and it matches the sum of the scope deltas.
- **Without DGC:**
  - the preprocess (0.02–0.04 ms per frame) is gone;
  - the draws themselves run 15–40% faster (0.006–0.013 ms);
  - the resolve drops by about a fifth, as its execute was costlier than three indirect dispatches.
- CPU recording (`record` in `[Bench]` CPU frame) shows no difference: 0.28–0.32 ms in both modes. `drawFrame` (2.7–6.0 ms) is present pacing, not work.
- At 853×480 the absolute costs are small. Raster and resolve shrink with resolution only partly, so the ~0.05 ms gap does not grow with pixels.

**Not run yet:**
- the 2060 Super;
- a higher render resolution (e.g. a 2560×1369 window);
- many-sequence executes (`Meshlets / sequence`), where DGC's per-sequence cost could differ.

**Default flipped (2026-11-26, user, after the 5070 table):**
- `ENGINE_DGC` unset or `0`: plain indirect. `1`: DGC.
- Logs: `Command path: plain indirect` / `Command path: DGC (ENGINE_DGC=1)`.
- Checked in both values: `ENGINE_CULL_CHECK` 16 of 16.
- Both paths stay. Removing DGC code (§5 "Move") waits for the 2060 Super numbers and an explicit decision.
