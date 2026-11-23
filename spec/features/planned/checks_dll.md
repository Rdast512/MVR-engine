# Feature: Checks DLL

**Date:** 2026-11-23 · **Status:** spec; CK1–CK7 open · **Milestone:** tooling, independent of the M8 steps
**Implements:** every engine check, the bench and the settings lab move out of `engine_runtime` and `engine_render` into one shared library, `engine_checks`, built only with `-DENGINE_BUILD_CHECKS=ON`
**Related:** [engine_ui_split.md](../done/engine_ui_split.md) (same pattern: `Engine` owns one object behind a compile switch), [full_testing.md](./full_testing.md) §0 (checks vs lab), [docs/streaming_server_spec.md](../../../docs/streaming_server_spec.md) §"engine_stream" (a separate SHARED library that `engine_runtime` owns one object of)

About 8,100 lines of test code ship inside the renderer today: eight runners and their helpers in `src/core/` (4,250 lines), the settings lab in `src/lab/` (3,860 lines), two check passes and a display readback in `Renderer`, their GPU structs in `types.hpp` / `push_data.hpp` / `types.slang`, and a branch per runner in `Engine::initialize` and `Engine::run`. A shipped build carries all of it.

This spec moves the code. Afterwards no file under `src/core/`, `src/render/`, `src/scene/` or `src/ui/` names a runner, and a default build compiles none of it. Every check gives the same output lines as before.

---

## 1. What is test code

### 1.1 Moves to `engine_checks`

| Piece | Now | After |
|---|---|---|
| Runners | `src/core/engine_{bench,motion_test,guide_capture,cull_check,ray_check,gi_check,temporal_check}.{hpp,cpp}` | `src/checks/`, same file names |
| Check helpers | `src/core/{check_poses.hpp,check_images.*,convergence_metrics.*,gi_settings_sets.hpp}` | `src/checks/` |
| Settings lab | `src/lab/*` | `src/checks/lab/` |
| Check passes | `Renderer::{enableMotionCheck,enableRayCheck,getLastMotionCheck,getLastRayCheck,recordMotionCheck,recordRayCheck}`, `motionCheckStats`, `rayCheckStats` | `src/checks/check_passes.{hpp,cpp}` (§3) |
| Display capture | `Renderer::{requestDisplayCapture,getLastDisplayCapture,recordDisplayCapture}`, `displayCaptures*`; only runners call it | `check_passes` (§3) |
| Check shader objects | `ShaderObjects::FrameShaders::{motionCheck,rayCheck}` | owned by `CheckPasses` |
| GPU structs | `GpuMotionCheckStats`, `GpuRayCheckStats`, `GpuSerCheckStats` (`core/types.hpp`); `MotionCheckPushData`, `RayCheckPushData`, `SerCheckPushData` (`render/push_data.hpp`); their mirrors in `shaders/types.slang` | `src/checks/check_types.hpp` + `shaders/checks/check_types.slang` |
| Check shaders | `shaders/passes/{motion_check,rt_check,rt_ser_check}.slang` | `shaders/checks/`; `rt_ser_check` stays unbuilt (C-G1-3) |
| Runner wiring | `Engine`: seven `std::optional<Engine*>` members, `applyMotionTestRequest`, `applyGuideCaptureRequest`, `applyLabRequest`, `applyEngineConfig`, the startup model / pose / sun / jitter choices per runner, `kGiCheckBenchFrames` | `CheckHarness` (§2); `Engine` keeps one member and one request path |
| `ENGINE_CONFIG` | `Engine::applyEngineConfig` over the lab registry | `CheckHarness` |
| UI "Copy config" | `engine_ui.cpp` calls `formatLabConfig(captureLabConfig(...))` | the same lines under `#if ENGINE_ENABLE_CHECKS` |
| Env variables | `ENGINE_BENCH_*`, `ENGINE_MOTION_TEST`, `ENGINE_GUIDE_CAPTURE`, `ENGINE_CULL_CHECK`, `ENGINE_RT_CHECK`, `ENGINE_GI_CHECK*`, `ENGINE_STABILITY_CHECK`, `ENGINE_LIGHT_CHANGE_CHECK`, `ENGINE_SCREEN_EDGE_CHECK`, `ENGINE_CHECK_SCENE`, `ENGINE_CHECK_PNG`, `ENGINE_LAB`, `ENGINE_CONFIG` | read only inside `engine_checks` |
| CPU test | `tests/lab_registry_test.cpp` (links `engine_runtime`) | links `engine_checks`; built only with the flag |

The bench moves too: it is a scripted runner, it shares the check poses, and the GI check drives its frames through it. Bench numbers therefore need a checks build. That build records the same frame as a default build (§3.3), so they stay comparable with every baseline.

### 1.2 Stays in the engine

These are engine state or debug switches that the interactive engine and the UI use as well. Runners set them; they are not test code.

| Piece | Why it stays |
|---|---|
| `Renderer::fixedFrameDeltaMs`, `RtxdiContext::frameIndexOverride`, `Camera::jitterPhaseOverride` | determinism inputs of the frame; one optional each |
| `RtxdiContext::{nvidia,converged}{,Gi}Settings`, `isReference`, `giReferenceBounces` | the UI's presets and reference modes |
| `ENGINE_WINDOW_SIZE`, `ENGINE_FULLSCREEN`, `ENGINE_MODEL`, `ENGINE_INDIRECT_MODE` (incl. `cycle`), `ENGINE_JITTER`, `ENGINE_DI_SAMPLING`, `ENGINE_RTXDI_REFERENCE`, `ENGINE_PTLAS_*`, `ENGINE_NRC_*`, `ENGINE_OMM_DEBUG_IMAGES`, `ENGINE_GPUAV_RESOLVE_PER_BIN`, `ENGINE_LOG_DEBUG` | renderer and window configuration, used without any runner |
| `Engine::replaceModels`, `loadModel`, `spawnInstanceField` | scene actions; the harness asks for them (§2.3) |
| `tests/*` other than `lab_registry_test` | separate executables already; they never link the engine DLLs into the shipped app |

---

## 2. Library and interface

### 2.1 Target

```cmake
option(ENGINE_BUILD_CHECKS "engine checks, bench and settings lab (engine_checks.dll)" OFF)
```

| Item | Rule |
|---|---|
| Target | `engine_checks SHARED`, sources from `src/checks/`, `ENGINE_LOG_DLL="checks"`, PCH, mimalloc new/delete, unity build and Linux rpath like the other `engine_*` libs |
| Links | `PUBLIC engine_build_config engine_core engine_render engine_scene engine_util ThirdParty::all`. It never includes `core/vk_engine.hpp`: no runner depends on `Engine` today, so the dependency is one way |
| Engine side | `engine_runtime` links `engine_checks PRIVATE` only when the option is on, and gets `ENGINE_ENABLE_CHECKS=$<BOOL:${ENGINE_BUILD_CHECKS}>` as a PRIVATE definition, always 0 or 1 |
| Shaders | `engine_add_slang_pass` takes the output dir from the source path, so `checks/motion_check.slang` lands in `shaders/checks/`. The two `engine_add_slang_pass(checks/...)` lines sit inside `if (ENGINE_BUILD_CHECKS)` |
| Tests | `lab_registry_test` inside `if (ENGINE_BUILD_CHECKS)`; `layout_test` keeps checking the check structs in every build, since it reads sources (§4) |
| Presets | a `-checks` variant of `windows-clang-release` and `windows-clang-debug`, inheriting them with `ENGINE_BUILD_CHECKS=ON` and their own `binaryDir`. The check and bench workflows use these trees |

Exports follow the other engine DLLs (MinGW auto-export, no `dllexport` macros).

### 2.2 `CheckHarness`

`src/checks/check_harness.hpp` is the only checks header `engine_runtime` includes. The runners stay behind a pimpl, so `vk_engine.cpp` does not compile their headers.

```cpp
// engine_checks.dll: every runner of one process, the check passes and ENGINE_CONFIG
class CheckHarness
{
public:
    // nullptr when no runner variable and no ENGINE_CONFIG is set. Throws on the refusals Engine throws today
    // (ENGINE_LAB with a check or bench variable; bench with ENGINE_GPUAV_RESOLVE_PER_BIN outside the GI check)
    [[nodiscard]] static std::unique_ptr<CheckHarness> fromEnvironment();
    ~CheckHarness();

    // a runner owns the camera, the scene and the frame pace: uncapped present, no camera input, no unfocused pacing.
    // false when only ENGINE_CONFIG is set
    [[nodiscard]] bool isAutomated() const noexcept;
    // the runner's default when ENGINE_JITTER is unset; the motion test, guide capture and GI check compare pixels
    [[nodiscard]] bool isJitterEnabledByDefault() const noexcept;
    // today's precedence: check scene > ENGINE_MODEL > bench model > MODEL_PATH
    [[nodiscard]] std::filesystem::path chooseStartupModel(const std::optional<std::filesystem::path>& engineModel,
                                                           const std::filesystem::path& defaultModel) const;

    // after the renderer exists: startup pose and sun, ENGINE_CONFIG, check passes, frame hook. The returned request
    // carries the bench's instance field
    [[nodiscard]] CheckRequest attach(const CheckHostContext& host);
    void onInstanceFieldSpawned(EntityId firstEntity, uint32_t count);

    // before drawFrame. Sets renderer and camera state itself; returns what only Engine's scene actions can do
    [[nodiscard]] CheckRequest beginFrame();
    // after drawFrame, not minimized; waits for the device itself when a runner reads this frame. true: quit
    [[nodiscard]] bool endFrame();
    // after the loop's waitIdle: summaries, then the verdict of every runner
    [[nodiscard]] bool finish();

private:
    struct Runners;
    std::unique_ptr<Runners> runners;
};
```

`CheckHostContext` holds references to `Device`, `VkAllocator`, `ShaderObjects`, `Scene`, `AssetBuffers`, `Renderer` and `Camera`, the way `UiContext` does for `EngineUi`. The harness never owns them.

### 2.3 `CheckRequest`

Only actions that change the scene or the window go back to `Engine`. Everything else (sun, indirect mode, glass, exposure, RTXDI frame index, jitter phase, culling settings, debug view, lab configs, the bench field motion, captures) the harness sets directly, as `applyGuideCaptureRequest` does today from inside `Engine`.

```cpp
struct CheckRequest
{
    struct Load { std::filesystem::path model; glm::vec3 position; };
    struct InstanceField { glm::vec3 center; int side; float spacing; };

    // loaded at the origin, then every older entity is unloaded (guide capture, lab)
    std::optional<std::filesystem::path> replaceModels;
    // adds an entity (motion test)
    std::optional<Load> load;
    // arrives as SDL_EVENT_WINDOW_RESIZED in a later poll (motion test)
    std::optional<glm::ivec2> windowSize;
    // startup only (bench ENGINE_BENCH_FIELD); Engine reports the spawned range with onInstanceFieldSpawned
    std::optional<InstanceField> instanceField;
};
```

`Engine::applyCheckRequest` replaces `applyMotionTestRequest`, `applyGuideCaptureRequest` and `applyLabRequest`. A failed load is logged under `"Checks"`, as the three log it today; a failed instance field still throws at startup.

### 2.4 `Engine` after the move

```cpp
#if ENGINE_ENABLE_CHECKS
    // after renderer: owns check passes registered with it, destroyed before it
    std::unique_ptr<CheckHarness> checks;
#endif
```

| Site | Change |
|---|---|
| `initialize` | `checks = CheckHarness::fromEnvironment()` where the runners are created now; `swapChain->isUncapped`, camera jitter default and the startup model read it; `attach` after `rebuildSwapchainResources`; a startup instance field goes through `applyCheckRequest`. A build without checks logs once that it has none |
| `run` | `isCameraInputEnabled` and the unfocused-pacing test use `!(checks && checks->isAutomated())`; one `beginFrame` before and one `endFrame` after `drawFrame`; `finish()` gives the return value |
| `cleanup` | `checks.reset()` after the UI, before `renderer` |

`#if ENGINE_ENABLE_CHECKS` guards the include and member in the header and one block each in `initialize`, `run` and `cleanup`, next to the existing `#if ENGINE_ENABLE_IMGUI` ones. `vk_engine.cpp` loses about 250 lines.

**Order kept from today:**

- runners run in the current order inside `beginFrame` / `endFrame` (motion, guide, cull, GI, temporal, lab, ray; bench field step last), so a combined run (ray check beside another runner) is unchanged;
- a runner reads the scene (`localBounds[0]`) before `Engine` applies its swap;
- the bench's `onFrame` moves from after `FrameMark` into `endFrame`. Nothing between the two points changes what it reads (GPU timer, culling, RT, light-prep, CPU and glass stats), and automated runs skip unfocused pacing, so its numbers are the same.

---

## 3. Renderer side: one frame hook

### 3.1 Interface

`src/render/frame_hook.hpp`, in `engine_render`. It is the renderer's only extension point for this feature and names no check.

```cpp
struct FrameHookContext
{
    const vk::raii::CommandBuffer& cmd;
    uint32_t frameSlot;
    vk::DeviceAddress frameRoot;
    vk::Extent2D renderExtent;
    // the image present reads, in eGeneral: post at display extent when DLSS-RR presents, else the display image
    vk::Image presentedImage;
    vk::Extent2D presentedExtent;
};

class FrameHook
{
public:
    virtual ~FrameHook() = default;
    // after the debug view and its guide barrier, before present; reads what the debug view reads
    virtual void recordAfterDebugView(const FrameHookContext& context) = 0;
    // traces rays in recordAfterDebugView: the frame's ray-timeline signal moves to the post-acquire submit
    [[nodiscard]] virtual bool tracesRays() const noexcept = 0;
};
```

`Renderer::setFrameHook(FrameHook*)`, non-owning, set once before the first frame. The recording site becomes:

```cpp
if (frameHook != nullptr) {
    frameHook->recordAfterDebugView(context);
}
gpuTimer.end(*cmd, GpuTimerScope::FrameHook);
```

and `isPreDlssRaySignal` reads `!(frameHook && frameHook->tracesRays())` instead of `rayCheckStats[currentFrame].mapped == nullptr`.

`GpuTimerScope::SelfTests` is renamed `FrameHook`; its label stays `"Self-tests"` so bench lines and scripts keep their keys. The presented-image choice moves out of `recordDisplayCapture` into `Renderer::getPresentedImage()`, which fills the context.

### 3.2 `CheckPasses`

`src/checks/check_passes.{hpp,cpp}` implements `FrameHook` with the three bodies that leave `Renderer` unchanged: motion check dispatch + host barrier, ray check dispatch + host barrier, display capture copy + barriers. It owns the per-slot stats buffers, the capture buffers and the two compute shader objects, and remembers the slot it recorded last (today `lastRecordedFrame` serves this). `tracesRays()` is true while the ray check is enabled.

Shader objects: `ShaderObjects::createComputeShader` becomes public, with an overload taking SPIR-V bytes. `CheckPasses` reads `ENGINE_SHADER_DIR/checks/*.spv` and calls it, so the check shaders get the same descriptor-heap flag and debug-rays specialization as every pass. `kFullscreenTile` moves from `vk_renderer.cpp` into a render header both use.

Push data: `check_types.hpp` asserts each check push block against `kMaxPushDataSize`, which the device limit check at startup already covers. `push_data.hpp` drops the three check structs from its max.

### 3.3 Cost when nothing runs

A checks build without a runner variable creates no harness and installs no hook. With a runner, the hook adds one indirect call per frame; with the bench alone, `recordAfterDebugView` records nothing and `tracesRays()` is false, so the command stream and submit split equal a default build's.

---

## 4. Shaders and layout test

- `shaders/checks/check_types.slang` (`module check_types;`, imports `types`) takes `MotionCheckPushData`, `RayCheckPushData`, `SerCheckPushData` and the three `Gpu*CheckStats`. The check shaders import it.
- `layout_test` reflects `check_types` next to `types` and includes `checks/check_types.hpp`. It reads sources and headers only, so it runs in every build.
- `shaders/README.md` lists `checks/` as a compile-unit folder beside `passes/`.

---

## 5. Steps

| Step | Work | Done when |
|---|---|---|
| CK1 | option, `engine_checks` target, `ENGINE_ENABLE_CHECKS`, presets; `git mv` of §1.1 files into `src/checks/` and `src/checks/lab/`, include paths fixed | builds ON and OFF; history follows the moved files |
| CK2 | `FrameHook`, `getPresentedImage`, `CheckPasses`; check members and methods leave `Renderer` and `ShaderObjects` | `vk_renderer.{hpp,cpp}` and `vk_shader_objects.*` name no check |
| CK3 | `check_types.hpp` / `check_types.slang`, shaders moved, `layout_test` extended | `layout_test` passes in both builds |
| CK4 | `CheckHarness`, `CheckRequest`, `Engine` wiring (§2.4) | `vk_engine.{hpp,cpp}` include only `check_harness.hpp`, inside the guard |
| CK5 | `ENGINE_CONFIG` into the harness; UI "Copy config" under the guard | `engine_ui.cpp` includes the lab only with the flag |
| CK6 | `lab_registry_test` gated and relinked | `ctest` passes ON and OFF |
| CK7 | verification (§6); considerations entries and runner docs that name `src/core/engine_*` / `src/lab/` paths updated. Older step specs keep their paths: they record what was built then | §6 table all green |

---

## 6. Verification

| Check | Pass |
|---|---|
| OFF build: no test code | `llvm-nm` on `engine_runtime.dll`, `engine_render.dll`, `engine.exe` finds none of `EngineGiCheck`, `EngineLab`, `EngineBench`, `CheckPasses`, `MotionCheck`, `RayCheck`, `DisplayCapture`; no `engine_checks.dll`, no `shaders/checks/` in the build tree |
| Source | `rg -l 'EngineBench\|EngineLab\|Engine\w*Check\|EngineGuideCapture\|EngineMotionTest\|CheckPasses\|check_poses\|settings_registry' src/core src/render src/scene src/ui` lists only `engine_ui.cpp` (the guarded "Copy config") |
| Both builds | debug and release clean with `-Wall -Wextra -Wpedantic`; `ctest` green |
| Runners unchanged (ON, release) | against output from HEAD before CK1, same build type: temporal checks byte-identical (fixed clock, content seeds); cull check bit-identical 16/16; GI check 8/8 on the five seeds; motion test, RT check and guide capture pass with the same counts; `ENGINE_CHECK_SCENE=bistro_cafe` and `=bistro` runs match |
| Bench unchanged | `ENGINE_BENCH_FRAMES` on Sponza and the instance field, ON build vs HEAD before CK1 (`build/h`, .spv unchanged): every scope within ±5%, vkconfig and validation off |
| Validation | one motion test, one RT check and one guide capture run clean under core + sync + best practices |
| Interactive | OFF build starts, renders, UI opens; ON build without variables behaves the same and logs no harness |
