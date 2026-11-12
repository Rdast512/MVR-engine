# Feature: Engine and UI split

**Date:** 2026-11-11 · **Status:** UI1–UI4 built; builds clean (ImGui on/off, debug/release); §4 runtime checks pending · **Milestone:** before M2 ([engine_spec.md](../../docs/engine_spec.md) §16.2)
**Implements:** engine spec §20.2 AU3
**Related:** [resource_manager_split.md](./resource_manager_split.md) (AU2), engine spec §20.2 AU10 (one swapchain recreate path)

`Engine` ([vk_engine.hpp](../../src/core/vk_engine.hpp), [vk_engine.cpp](../../src/core/vk_engine.cpp), 1,514 lines) runs the application and also is the debug UI: about 900 of its lines are ImGui panels and their helpers, and half of its members are panel state. This spec splits it in two: `Engine` keeps the application, `EngineUi` gets everything ImGui.

This is a move of code. Panels look and behave the same afterwards.

---

## 1. Current layout of `vk_engine.cpp`

| Lines | Content | Goes to |
|---|---|---|
| 25–170 | panel helpers: `propertyRow`, `textureRow`, `drawMaterialExtensions` | `EngineUi` |
| 174–299 | `initialize`: SDL, window, object creation; ImGui context and backends in between | `Engine`; the ImGui part to `EngineUi` |
| 301–331 | `createImGuiDescriptorPool` | `EngineUi` |
| 333–488 | `run`: events, focus, pacing, camera input, frame | `Engine` |
| 490–505 | `setImGuiInputEnabled` | `EngineUi` |
| 507–1192 | `drawImGui` and the nine panels | `EngineUi` |
| 1194–1208 | `applyPendingModelActions` | `Engine` |
| 1210–1265 | `scanFolder` | `EngineUi` |
| 1267–1283 | `recreateSwapchain` | `Engine`; the ImGui lines to `EngineUi` |
| 1285–1447 | `loadSelectedModel`, `spawnInstanceField`, `clearInstanceField`, `unloadModel` | `Engine` |
| 1449–1514 | `cleanup` | `Engine`; ImGui shutdown to `EngineUi` |

---

## 2. Target

| Class | Files | Target | Owns |
|---|---|---|---|
| `Engine` | `src/core/vk_engine.{hpp,cpp}` | `engine_runtime` | window, the engine objects and their order, the event loop, focus and pacing, camera input, swapchain recreation, the scene actions |
| `EngineUi` | `src/ui/engine_ui.{hpp,cpp}` | `engine_runtime`, compiled only when `ENGINE_ENABLE_IMGUI` is on | the ImGui context, its descriptor pool and backends, every panel, all panel state |

Expected sizes: `vk_engine.cpp` about 600 lines, `engine_ui.cpp` about 950. One UI file is enough; split it by panel only if it passes about 1,200 lines.

### 2.1 What `EngineUi` reads

A struct of references handed over at construction. Panels read through it and never own engine objects.

```cpp
struct UiContext {
    SDL_Window* window;
    const Device& device;
    const SwapChain& swapChain;
    Scene& scene;
    const AssetBuffers& assets;
    const TextureManager& textures;
    Renderer& renderer;                 // debug view, culling settings and stats, GPU timer
    Camera& camera;
};
```

`Renderer` and `Camera` are non-const because panels edit their settings directly today (debug view, culling settings, teleport). Entity transforms are edited through `scene.objectStorage`. That stays.

### 2.2 What `EngineUi` asks for

Panels do not load, unload or spawn. They record a request, and `Engine` runs it after the ImGui frame. This is the rule the `hasPending*` flags implement today (no panel iterates storage that changes under it); the flags become one struct.

```cpp
struct UiRequests {
    struct Load { std::filesystem::path asset; glm::vec3 position; };
    struct InstanceField { glm::vec3 center; int side; float spacing; };

    std::optional<Load> load;
    std::optional<EntityId> unload;
    std::optional<InstanceField> spawnInstanceField;
    bool shouldClearInstanceField = false;
};
```

- `EngineUi::takeRequests()` returns the struct and clears it.
- `Engine::applyUiRequests` runs each request through the scene actions, which keep their `waitIdle` sequencing, and reports back with `EngineUi::setLoadStatus(text, isError)`.
- The scene actions take their inputs as parameters (`loadModel(path, position)`, `spawnInstanceField(center, side, spacing)`), not from UI state. They return their status line (`std::nullopt` when they did nothing) so they compile without ImGui; `applyUiRequests` forwards it.

### 2.3 Interface

| `EngineUi` function | Called from | Replaces |
|---|---|---|
| constructor | `Engine::initialize`, after `Renderer` | context creation, `createImGuiDescriptorPool`, backend init |
| destructor | `Engine::cleanup`, after `waitIdle` and before `Renderer` | backend shutdown, `DestroyContext`, pool clear |
| `processEvent(const SDL_Event&)` | event loop, only while the UI is open | the `ImGui_ImplSDL3_ProcessEvent` call |
| `setInputEnabled(bool)` | focus changes | `setImGuiInputEnabled`; also shows or hides the panels, which are open exactly while they take input |
| `isTextInputActive()` | `I` key handler | the `ImGui::GetIO().WantTextInput` read |
| `buildFrame(const FrameStats&)` | once per frame | `drawImGui`; `FrameStats` carries `fps` and `frameMs` |
| `takeRequests()` | after `buildFrame` | the `hasPending*` flags and `pendingUnload` |
| `setLoadStatus(...)` | scene actions | `loadStatus`, `isLoadStatusError` |
| `onSwapchainRecreated()` | `Engine::recreateSwapchain` | the ImGui image-count and format update |

`Engine` keeps `isUiOpen` (the `I` key) because it decides mouse capture and camera input. `Renderer` keeps the `ImGui_ImplVulkan_RenderDrawData` call in the present pass; it needs no `EngineUi` reference.

### 2.4 State that moves

`imguiDescriptorPool`, `imguiColorFormat`, `imguiDepthFormat`, `imguiPipelineRenderingInfo`, `discoveredAssets`, `selectedAssetIndex`, `hasScannedAssets`, `loadedModelPosition`, `teleportPosition`, `assetsPathInput`, `loadStatus`, `isLoadStatusError`, `transformTestEntity`, `instanceFieldSide`, `instanceFieldSpacing`.

`Engine` loses its public `scanFolder`; nothing outside the UI calls it.

### 2.5 Without ImGui

With `ENGINE_ENABLE_IMGUI` off, `engine_ui.cpp` is not compiled and `Engine` holds no `EngineUi`. The `#if` blocks in `vk_engine.cpp` shrink to the member, its construction and the five calls of §2.3. The runtime `enableImGui` flag is removed: it duplicates the macro.

---

## 3. Tasks

Each step builds, runs and passes §4 before the next starts.

| # | Task |
|---|---|
| UI1 | Create `EngineUi` with `UiContext`; move the helpers, the panels, `scanFolder` and the panel state of §2.4 as they are. `Engine` calls `buildFrame`; requests still go through temporary accessors to the old flags |
| UI2 | Introduce `UiRequests`, `takeRequests` and `setLoadStatus`; give the scene actions parameters; delete the `hasPending*` flags |
| UI3 | Move ImGui lifetime: context, descriptor pool, backend init and shutdown, `onSwapchainRecreated`, `processEvent`, `setInputEnabled` |
| UI4 | Remove `enableImGui` and the matching `Renderer` constructor argument (`imguiEnabled`, already unused there); reduce the `#if ENGINE_ENABLE_IMGUI` blocks in `vk_engine.cpp` to the member and its calls; add `src/ui` to the `engine_runtime` sources |

---

## 4. Verification

| Check | Pass when |
|---|---|
| Build | clean with ImGui on and with it off, debug and release presets, no new warnings |
| Panels | every panel opens and shows the same values as before: stats overlay, load, loaded models, assets, debug view, entity transform, camera, culling, instance field |
| Scene actions from the UI | scan a folder, load a model, load it again (shared instance), spawn and clear the instance field, unload: status line correct, no validation message |
| Input | `I` toggles the UI; mouse look works only with the UI closed; ImGui gets no mouse while the game has focus |
| Resize | drag-resize with the UI open: UI stays correct, no validation message |
| Shutdown | no validation message, no ImGui assert |
| Frame time | CPU frame time with the UI open within noise of the pre-split number |

---

## 5. Must not

- let `EngineUi` call a scene action, `waitIdle`, or anything that changes entity or model storage during `buildFrame`;
- give `EngineUi` ownership of an engine object, or keep a pointer to one outside `UiContext`;
- move the event loop, focus handling or camera input into the UI;
- restyle or reorganize panels while moving them;
- add an interface or base class for the UI: there is one implementation.
