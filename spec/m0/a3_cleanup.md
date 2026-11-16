# M0 · A3 — Cleanup and audit follow-ups

**Status** — A3 built. Related audit fixes landed through 2026-11-15.\
**Implements** — PBR A3 and engine spec §17. Related audit work in §20.\
[PBR plan](../pbr_implementation_plan.md) · [Engine spec](../../docs/engine_spec.md)

## Scope

A3 removed the obsolete G-buffer shader and `evalLambert`. The RT pipeline,
SER and cluster AS device paths stayed available for later milestones.
AU7 removed empty shader and C++ scaffolding. Files return with their
implementation, as `raytracing/trace.slang` did in M3.
[Dead code removal](../features/dead_code_removal.md)

| Audit | As built |
|---|---|
| AU2, AU3 | `AssetBuffers`, `FrameData` and `FrameSync` own the former `ResourceManager` responsibilities. `EngineUi` owns ImGui and sends scene requests to `Engine`. |
| AU5, AU11 | Reverse Z is the sole culling convention. The layout test compares reflected Slang layouts and checks the typed storage-image formats. |
| AU6, C-M0-4 | `gltfTextureSlotColorSpace` is shared by the loader, its image-use scan and `gltf_ktx2`. Base color, emissive, specular color, sheen color and diffuse-transmission color slots are sRGB. Other slots are linear. |
| AU8, C-M0-3 | `engine_util` owns the whole-archive link of `ThirdPartyImpl` on Windows. `implementations.cpp` compiles tinygltf once. Coverage defaults to OFF for fresh configurations. |
| AU10, C-M0-2 | Resize and out-of-date or suboptimal swapchain results request recreation. `Engine::recreateSwapchain` services the request before building the next eligible frame. The renderer passes the old swapchain to creation, and the engine updates ImGui. |

[Resource split](../features/resource_manager_split.md) · [UI split](../features/engine_ui_split.md) · [Color-space policy](../../src/core/texture_color_space.hpp) · [Link ownership](../../src/util/CMakeLists.txt) · [Recreate path](../../src/core/vk_engine.cpp)

## Verification and open work

- C-M0-2 records one recreate for the scripted resize and 13 of 13 motion
  scenarios passing. C-M0-3 and C-M0-6 record the build and documentation fixes.
- C-M0-5 tracks the full per-entity writes in `FrameData::write`. AU4 is the
  optional GPU scene update. The loader split remains under M8.
- C-M0-7 records the Debug `gltf_ktx2` assertion and UBSan reports on
  `SheenCloth`. Release converted the same asset. The exit is a clean Debug
  conversion of the glTF samples under sanitizers.
- The remaining SHaRC cleanup belongs to M4 G3, under C-G3-1.

[Considerations](../considerations.md) · [M4 G3](../m4/g3_nrc.md)

## Log

| Date | Change |
|---|---|
| 2026-11-08 | A3 completed |
| 2026-11-12–15 | Audit cleanup, layout and build checks, and resize consolidation landed |
