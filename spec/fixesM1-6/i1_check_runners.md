# M8 · I1: check runners

**Status:** built 2026-11-21 · **Milestone:** M8 ([README](./README.md))
**Implements:** findings F1 (spread), F2, F3 ([m8_optimization_and_fixes.md](./m8_optimization_and_fixes.md) §4); opens F20
**Depends on:** the GI check (`ENGINE_GI_CHECK`, M4 G5 / G7), `Renderer::requestDisplayCapture`, `RtxdiContext::frameIndexOverride` (F1 seeding)
**Considerations:** none opened; F2's first-run artifact is closed here; C-O11-1's "no stability runner" is answered
**Reference:** NVIDIA's samples ship no stability or settling runner: the RTXDI FullSample and RTXGI samples are judged on screen. The metrics follow the scratch probe of findings §2.5 (`o1_stab_probe.py`) and the GI check's conventions (`convergence_metrics.hpp`).

Every later M8 step is judged by these runners, so they come first. Nothing here changes the frame or the shipped settings.

## Scope

| Item | Before | After I1 |
|---|---|---|
| GI-check spread | one realization per phase (F1 seeding); the courtyard traced-path gate read 0.90–1.51% against 1.5% across code changes | `ENGINE_GI_CHECK_SEED=n` picks another realization of the whole run; five seeds measured, limits per pose set from the spread |
| Stability | scratch patch, first process run abnormal, lit-pixel cutoff not recorded (F2) | `ENGINE_STABILITY_CHECK=1`: `Lit` and `Indirect radiance` temporal noise per settings set and pose |
| Light change | none (F3) | `ENGINE_LIGHT_CHANGE_CHECK=1`: frames until `Indirect radiance` settles after a sun move and a sun toggle |
| Screen edges | none (F3) | `ENGINE_SCREEN_EDGE_CHECK=1`: frames until the band a pan or a backward dolly reveals converges, in `Lit`, `Direct lighting` and `Indirect radiance` |

## Decisions

| Decision | Rule | Why |
|---|---|---|
| One class for three runners | `EngineTemporalCheck` (`src/core/engine_temporal_check.*`) with a `Kind`; each kind builds its step list at construction; one kind per run (a second env var is logged and ignored); not combinable with the other scene runners | the three share poses, settings sets, seeding, capture handling and the frame clock |
| Settings sets | `src/core/gi_settings_sets.hpp`: `GiSettingsSet` and `giSettingsFor(set)`, also used by the GI check in place of its private enum. New sets: shipped without decorrelation, shipped without firefly replacement, shipped with NVIDIA's age limit | one definition of "the set before O11" and of the O11 parts |
| Poses | the GI check's: gallery (startup camera, recorded on the first runner frame through `Camera::getForward`) and courtyard (`lookAt({6, 2.5, 1.5}, {-10, 1, -1})`) | these numbers and the GI check's share their views |
| Indirect mode | ReSTIR GI (shipped); DI keeps its shipped settings | what ships |
| Fixed frame clock | `Renderer::fixedFrameDeltaMs = 1000 / 60` while a runner runs: exposure adaptation and DLSS-RR's `InFrameTimeDeltaInMsec` read it instead of the wall clock | auto exposure adapts by wall-clock seconds (`PostProcess::record`), so the exposure after N frames depended on the process's frame times: F2's first-run artifact |
| Step start | a camera cut (`lookAt`: ReSTIR DI / GI and DLSS-RR history reset) and an exposure snap (`PostProcess::requestSnap`) | each step starts from nothing |
| Seeding | `frameIndexOverride = stepSeed(step) + stepFrame`, the seed an even hash of what the step is (kind, pose, set, view, sun states, motion, pose choice); `Camera::jitterPhaseOverride = stepFrame` | a step's frames depend on nothing before it: adding, removing or reordering steps changes no other step's numbers (checked: adding a set to the screen-edge runner left every existing line byte-identical). As first built, the seed came from the step index and the jitter from the global frame, so one step's length moved every later step's realization |
| Jitter | on (the engine default; only the GI check turns it off) | `Lit` is judged as DLSS-RR receives it |
| Settling | non-overlapping 8-frame blocks, within max(2%, 4σ of the block-mean noise of a static reference); `settled after N`: frames before the run of passing blocks that lasts to the end | a sliding window's many overlapping tests trip on noise alone; the gallery's frame mean is too noisy for a fixed 2% |
| Gating | none: every line is `MEASURED`. A runner fails only on a missing capture or non-finite pixels | baselines change with every M8 fix; steps compare against §5 |

## 1. GI-check spread (F1)

`ENGINE_GI_CHECK_SEED=n` adds `n · kPoseSeedStride · poses` to every frame index override, so seed `n` is another realization of every phase; seed 0 is the run before I1. Five seeds (0–4) of the full check measured each gated phase's spread. Each limit holds on all five seeds with a margin of at least half the observed range; `PhaseInfo::maxBias` is now per pose.

## 2. Stability runner (F2)

Per pose × settings set × view, one step: 300 warm-up frames (5 s at the fixed clock), then 480 captured frames of a static camera.

- **Sets:** shipped, shipped without decorrelation, shipped without firefly replacement, shipped without the DLSS-RR profile (the set before O11), shipped with NVIDIA's age limit, NVIDIA, converged.
- **Views:** `Lit` (DLSS-RR, exposure and tonemap; display resolution; what the user sees) and `Indirect radiance` (ReSTIR GI output; render resolution; linear; unjittered).
- **Metrics,** per pixel over the captured frames, luminance:
  - `mean`;
  - `temporal std`: Σ per-pixel std / Σ per-pixel mean;
  - `flicker`: the share of lit pixels whose std / mean > 10%;
  - `lit px`: on `Lit`, mean > 1e-3 in display units (about sRGB code 3, the darkest visible); on `Indirect radiance`, mean > 5% of the frame's mean; logged with its share;
  - `f2f`: mean |frame-to-frame change| / mean;
  - `largest 8-frame swing`: the 8-frame block mean farthest from the step's mean, which flags bright or dark events.
- 120 captured frames (the first build) missed F20's transients, which start after ~400 static frames.

## 3. Light-change runner (F3)

Per pose × settings set × event, on `Indirect radiance`:

1. **Reference:** the new light state from a cut, 256 warm-up, 256 captured: the converged mean and its 8-frame block noise.
2. **Change:** the old state from a cut, 512 frames (the last 64 captured as the old mean), then the new state at once and 512 captured frames.

- **Events:** sun move (azimuth +30°, 20° → 50°; no history reset, so stale samples have to die out) and sun toggle (off → on; the light-set change resets history, so this times a fresh ramp-up).
- **Sets:** shipped, shipped with NVIDIA's age limit (30 frames), NVIDIA, converged.
- **Metrics:** `settled after N` (§ Decisions), the 8-frame trailing mean's error at 1, 4, 16, 64 and 256 frames after the change, the old and new means.

## 4. Screen-edge runner (F3)

Per pose × motion:

1. **Depth:** one `Depth` view frame at the motion's start pose and one at the pose (render resolution; view depth decoded from `depthShade`). The CPU builds the masks of the pose's view: `band`, pixels whose world point is off screen at the start; `disoccluded`, on screen but behind a closer surface there (view depth > 1.02 × that surface + 2 cm); `interior`, the rest.
2. Per settings set × view, a **reference** step at the pose (cut, 256 warm-up, 256 captured: the first 128 average into the reference image, the last 128 measure each region's static error and block noise against it), then a **move** step: the start pose (cut, 256 warm-up), the pose at once without a cut, 256 captured frames.

- **Motions:** pan (start 10° turned) and backward dolly (start 1 m ahead). The motion ends at the pose: backing 1 m away from a pose walked into columns (min depth 0.43 m), and the space ahead of a pose is inside its frustum, which is clear.
- **Exposure:** the reference step's converged exposure is pinned (manual EV) for its move step, so `Lit` compares at one exposure.
- **Sets:** shipped, shipped with NVIDIA's age limit, NVIDIA (GI); **views:** `Lit`, `Direct lighting`, `Indirect radiance`.
- **Metrics,** per revealed region of at least 1% of the frame: `mean error` |Σ L − Σ ref| / Σ ref and `pixel error` Σ |L − ref| / Σ ref. A region has settled when its 8-frame mean error is within its static block noise and its pixel error is at most 1.25 × its own static error; errors at frames 1, 4, 16, 64 and 256. A region black in the reference (the gallery's `Direct lighting` band: no sun reaches it) is logged as such.

## 5. Results

RTX 5070, release, no layers, 1280×720 window (853×480 render, DLSS-RR Quality), 2026-11-21. Run times: GI check ~3.5 min per seed, stability ~2.5 min, light change ~1.6 min, screen edge ~2.5 min.

**Repeatability.** Each runner repeats byte for byte across process runs, the first after a build included (stability and screen edge checked twice each). F2's first-run artifact is gone with the fixed clock.

**GI-check spread** (bias against the ground truth, seeds 0–4):

| Phase | Gallery | Courtyard | Limit (gallery / courtyard) |
|---|---|---|---|
| traced paths, noise floor | −1.94 … +0.22% | −0.24 … +0.08% | 3.0% / 1.0% (was 1.0%) |
| traced paths, ReSTIR GI (converged) | −1.12 … +0.66% | +0.97 … +1.25% | 2.0% / 1.5% (was 1.5%) |
| one bounce, noise floor | −0.17 … +0.19% | +0.05 … +0.41% | 1.0% |
| ReSTIR GI mode (converged) | −0.31 … +0.27% | +0.51 … +0.85% | 1.5% |
| ReSTIR GI mode (shipped), measured | −1.60 … −0.75% | −1.01 … −0.46% | not gated |
| ReSTIR GI mode (NVIDIA), measured | −25.2 … −24.2% | −8.6 … −8.1% | not gated |

Seed 4 failed the gallery traced noise floor at the old 1% (−1.94%): the noise floor and the reuse phase moved together by ~1.5 pp, and one tile read 65% off, which points to a firefly in a 2048-frame multi-bounce estimate (785% 1-sample noise). The courtyard traced-path gate holds the old 1.5% with a consistent +1.1% bias. With the new limits all five seeds pass 8 of 8.

**Stability** (`Lit`, 480 frames; temporal std / flicker of lit px / largest 8-frame swing):

| Set | Gallery | Courtyard |
|---|---|---|
| shipped | 6.14% / 48.7% / 1.16% | **24.65% / 91.0% / 22.57%** (F20) |
| shipped without decorrelation | 11.47% / 74.8% / 1.75% | 25.30% / 91.2% / 13.74% |
| shipped without firefly replacement | 6.51% / 51.1% / 1.40% | 23.25% / 82.4% / 13.89% |
| shipped without DLSS-RR profile | 10.16% / 68.1% / 1.54% | 1.55% / 17.2% / 0.19% |
| shipped + NVIDIA age limit | 4.87% / 17.3% / 1.23% | 1.29% / 7.7% / 0.16% |
| NVIDIA | 7.71% / 44.0% / 1.36% | 1.44% / 14.6% / 0.15% |
| converged | 8.29% / 56.8% / 1.10% | 4.55% / 32.2% / 5.54% |

Lit px: gallery 9.5–15% (mostly below sRGB code 3), courtyard 57%, read 59–100% where an event brightens the frame. `Indirect radiance` temporal std: gallery 187–655%, courtyard 98–288%; decorrelation doubles the raw per-frame noise by design (shipped 204% vs 98% without it), and no set shows a swing above 5% there: the events are only in the jittered, DLSS-RR `Lit`.

**Light change** (courtyard; the gallery is noise-limited, 5–7% tolerance):

| Set | Sun move: settled after / error at 16 frames | Sun toggle: settled after |
|---|---|---|
| shipped | 400 / +28.2% | 24 |
| shipped + NVIDIA age limit | 16 / +2.7% | 8 |
| NVIDIA | 32 / +21.2% | 0 |
| converged | 32 / +26.0% | 344 |

The shipped and converged sets have no age limit; their late settle times come from F20's events after the light has settled (shipped at 64 frames: +1.0%).

**Screen edges** (frames until the revealed band settles; `> 256`: not within the record):

| Pose, motion | View | shipped | shipped + age limit | NVIDIA |
|---|---|---|---|---|
| gallery, pan 10° (band 17%) | `Lit` | > 256 | > 256 (px 10.3% vs static 6.3% at 256) | 96 |
| | `Indirect radiance` | 8 | 0 | 0 |
| gallery, dolly 1 m (band 53%) | `Lit` | > 256 | > 256 | 80 |
| | `Indirect radiance` | 8 | 8 | 16 |
| courtyard, pan 10° (band 18%) | `Lit` | unreadable (F20 in the reference) | > 256 (7.9% vs 5.8%) | > 256 |
| | `Direct lighting` | 0 | 56 | 48 |
| | `Indirect radiance` | 8 | 0 | 8 |
| courtyard, dolly 1 m (band 49%, disoccluded 8.5%) | `Lit` | unreadable (F20) | > 256 (1.8% vs 1.4%) | 248 |
| | `Direct lighting` | 0 | 0 | 0 |
| | `Indirect radiance` | 16 | 8 | 8 |

ReSTIR GI and DI recover a revealed band within 0–56 frames; DLSS-RR's `Lit` keeps 1.3–1.6× its static pixel error there for more than 256 frames (4 s) in most cases. That is the screen-edge question I5 and I6 inherit (F3).

**Validation:** core + sync + best practices on one full run of each runner, GPU-AV off (unusable since M7 E1, C-E1-3): only the known third-party messages, identical in all three runs: NGX's two `SYNC-HAZARD-WRITE-AFTER-WRITE` and three `SpirvDeprecated_WorkgroupSize` (C-H1-2), and the NRC SDK's leaked objects at device destruction (`VUID-vkDestroyDevice-device-05137`, C-G3-3).

## 6. Done when

- [x] each runner repeats its numbers exactly across two process runs, the first after a build included;
- [x] the baseline of the shipped set (O11) on both poses is recorded here and in the README baseline, replacing the scratch probe's courtyard numbers;
- [x] the GI-check limits are set from the five-seed spread;
- [x] the runners and their numbers are documented in memory; validation runs on one run of each.
