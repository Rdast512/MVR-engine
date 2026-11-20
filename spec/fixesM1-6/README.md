# M8 · Optimization and fixes

**Status** — Milestone planned on 2026-11-20. No step spec is written yet, and no step is built.\
**Scope** — Make the whole pipeline produce its best image per millisecond. That means fixing the measured bias and stability problems of the default lighting, closing or explaining the NRC bias, and landing the measured perf levers. Each step is backed by a check that keeps it fixed.
[Engine spec](../../docs/engine_spec.md) · [Findings](./m8_optimization_and_fixes.md) · [PBR plan](../pbr_implementation_plan.md) · [Considerations](../considerations.md)

Fixes landed ahead of the steps: [auto-exposure dark boiling](./auto_exposure_dark_boiling.md) (2026-11-20, finding F10).

Findings and candidate ids (O, Q, NP, F) refer to [m8_optimization_and_fixes.md](./m8_optimization_and_fixes.md). Each step gets its own spec in this folder (`i1_check_runners.md`, …) before its code, written after the step's reference is studied in `reference/`.

| Step | Contract | Findings |
|---|---|---|
| I1 · Check runners | **GI check:** seeded per phase, not per frame index, so adding a phase shifts nothing; the courtyard's converged gate judged over several seeds and its limit set from that spread. **Stability runner** `ENGINE_STABILITY_CHECK`: the static-camera `Lit` / `Indirect` temporal std, flickering px and mean per settings set, with the first-run artifact found or handled. **Light-change runner:** sun toggle and sun move, frames until `Indirect radiance` is within 2% of its new converged mean. **Screen-edge runner:** scripted pan and backward dolly, frames until the revealed band converges | F1, F2, F3 |
| I2 · Frame profile | A separate TLAS-wait span; per-scope tables in all three indirect modes; an Nsight GPU Trace of the default frame with the limiter of each pass (ray throughput, memory, occupancy); Tracy's CPU frame against the 2 ms budget. Output: a ranked list of levers with expected gains. Levers outside lighting (PTLAS minimum cost, O(entities) frame writes, async overlap) get their own step spec when they rank | F4, F7, F8, F9 |
| I3 · ReSTIR GI quality | `maxReservoirAge` decided with I1's light-change runner (the +8% gallery bias against settling time); the cost of ray-traced spatial correction (+0.15 ms) traded against spatial sample count and radius; the `Lit` stability of the shipped set held or improved; NVIDIA's set stays callable | O1 follow-up, C-G5-1, C-G5-2, C-R32-5 |
| I4 · ReSTIR DI tuning | Ray budget: each DI ray (candidate visibility, temporal correction, final visibility, BRDF candidate) measured for cost and for its effect on the convergence and stability runners; final visibility reuse at shadow edges; screen-edge convergence with fallback sampling. NVIDIA's `Medium` stays callable | F5, F6, O8, C-R32-1, C-R32-3, C-R32-4, C-M4-1 |
| I5 · NRC bias | Q1 and Q2, then Q3, then Q4 and Q5 (Q6 if the root still differs). Exit: the NRC GI-check phases are gated, or the cause is known and logged | C-G5-3, ND1, ND5, ND6 |
| I6 · NRC cost | NP1 rechecked per driver and reported to NVIDIA; NP2 train-on-change; NP3 and NP5 measured | C-G3-6, NP1–NP5 |
| I7 · Pass merges and traffic | O3: GI temporal reuse inside the indirect pass. O4: compact RTXDI surface loads (packed guide, position from depth) | O3, O4 |
| I8 · Floor tier | On the 2060 Super: every scope of I2; DLSS-RR cost and mode per tier; O7 checkerboard GI (and DI) if `Indirect` is over its floor budget; NP3 if NRC runs there | O7, C-G2-1, C-M0-1, C-F3-7, C-G3-2 |
| I9 · Many-light re-check (after M7) | With M7's emissive triangles and `Power_RIS`: O2 with ray-traced or visibility-aware resampling, O6, the BRDF candidate's value (O8 exit), and the I3 / I4 decisions re-run on a many-light pose | O2, O6, O8, C-R32-3 |

## Order

```
I1 → I2 → I3, I4, I7      (I3 / I4 / I7 independent; each ABBA-measured against I2's table)
I1 → I5 → I6
I2 → I8                   (needs the 2060 Super)
M7 + I3 + I4 → I9
```

I1 comes first because I3, I4 and I5 are decided by checks that do not exist yet or are not reproducible. I2 orders the perf steps by measured gain, so no lever is taken on expectation. Only I9 depends on M6 or M7. The other steps can start before M6 and M7; the milestone number gives order of completion, not of start.

## Rules

- **NVIDIA's sets stay callable.** Every tuned default keeps NVIDIA's set behind the `NVIDIA defaults` button and in a measured GI-check phase, for the cost and quality comparison with NVIDIA's samples (C-M4-2).
- **A change lands with its numbers:**
  - GI check and stability runner before and after;
  - an ABBA bench for perf (release, no layers, ±5% noise);
  - validation clean, with GPU-AV on for shader changes;
  - `ENGINE_RT_CHECK`, `ENGINE_CULL_CHECK`, `ENGINE_MOTION_TEST` and `ENGINE_GUIDE_CAPTURE` still pass.
- **A measured no-gain lever is closed with its numbers** in the findings document, as O1, O2 and O6 were.

## Frame and acceptance

M8 adds no frame pass. I7 removes one, and I1 adds runners only.

M8 is done when:

- the shipped ReSTIR GI phase is gated in the GI check on both poses, or its remaining bias is a choice documented with the light-change runner's numbers;
- the NRC phases are gated, or C-G5-3 has a known cause and stays a logged limitation of the comparison mode;
- the stability runner's `Lit` numbers on both poses are no worse than the 2026-11-20 baseline (courtyard 1.57% / 5.0% flickering px);
- every lever in I2's ranked list has either landed with its gain or been closed with its number, and the per-scope table is recorded before and after on the RTX 5070 (and the 2060 Super, I8);
- every runner passes, the GI check passes on every gated phase, and validation and GPU-AV are clean.

## Baseline (2026-11-20, RTX 5070, 1280×720 window (853×480 render, DLSS-RR Quality), release, no layers)

| Item | Value |
|---|---|
| GPU total, ReSTIR GI mode | 2.72–2.76 ms (DLSS-RR 1.10, `Indirect` 0.52, GI shade 0.41, GI temporal 0.11, DI 0.34) |
| GPU total, NRC + ReSTIR GI mode | 3.30 ms (2026-11-19) |
| Shipped ReSTIR GI bias vs one bounce, gallery / courtyard | +8.1% / +1.7% |
| NRC mode bias, gallery / courtyard | +10.5% / +8.3% |
| `Lit` stability, courtyard (temporal std / flickering px) | 1.57% / 5.0% |
| GI check | 8 of 8 gated phases pass with the O2 / O6 phases appended. The run before them read 7 of 8 (courtyard converged one-bounce at +1.51% against 1.5%), a realization shift (F1) |

## References

- `reference/RTXDI` (sample `fdbd9b3`), `RTXDI-Library` `b67f61c`: presets, `ShadeSecondarySurfaces.hlsl`, `GI/*.hlsl`, `Doc/RestirGI.md`, `ChangeLog.md`.
- `reference/RTXGI` (2.9.0): `Samples/IndirectDiffuse`, NRC SDK 0.16 `Include/NrcCommon.h`, `Docs/NrcGuide.md`.
- Nsight Graphics GPU Trace for I2; Tracy for the CPU frame.
