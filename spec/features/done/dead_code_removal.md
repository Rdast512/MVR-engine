# Feature: Dead scaffolding removal

**Date:** 2026-11-12 · **Status:** built; debug + release build clean, ctest 5/5 in both; runtime check pending · **Milestone:** before M2 ([engine_spec.md](../../../docs/engine_spec.md) §16.2)
**Implements:** engine spec §20.2 AU7
**Related:** engine spec §17 (the PBR plan A3 row this reverses), [shader_architecture_plan.md](../../shader_architecture_plan.md) (owner of shader file status)

Zero-byte sources, stub shaders and unused types stay in the tree from earlier plans. Some are compiled, some declare module names that collide, and the stubs describe designs §10–§11 have since replaced. AU7 deletes them; a file returns with the milestone that fills it.

No behavior changes. The binary renders the same frame.

---

## 1. Inventory

Checked against the working tree on 2026-11-12.

### 1.1 C++ sources

| File | Size | Referenced by | Action |
|---|---|---|---|
| `src/render/vk_pbr.cpp`, `.hpp` | 0 B | `src/render/CMakeLists.txt` (`.cpp` compiled) | delete files and list entry |
| `src/render/vk_postprocessing.cpp`, `.hpp` | 0 B | `src/render/CMakeLists.txt` | delete files and list entry |
| `src/scene/vk_mesh.cpp`, `.hpp` | 0 B | `src/scene/CMakeLists.txt` | delete files and list entry |
| `src/util/maths.cpp` | 0 B | `src/util/CMakeLists.txt` | delete file and list entry |
| `src/util/vk_shaders.cpp`, `.hpp` | 0 B | `src/util/CMakeLists.txt` | delete files and list entry |

No source includes any of the headers.

### 1.2 Unused code

| Item | Location | Referenced by | Action |
|---|---|---|---|
| `GpuUniformBufferObject` | `types.hpp` | nothing; no Slang mirror | delete |
| `EngineSettings` | `types.hpp` | only `EngineContext::settings` | delete |
| `EngineContext` | `types.hpp` | nothing | delete (orphaned by `EngineSettings`) |
| `DescriptorBindingMode` | `types.hpp` | `DescriptorManager::descriptorBindingMode` | delete |
| `DescriptorManager::descriptorBindingMode` | `vk_descriptors.hpp` | `ShaderObjects::usesDescriptorHeaps`, guard in `DeviceGeneratedCommands::init` | delete; heaps are the only mode |
| `DescriptorManager::descriptorSetLayout`, `descriptorPool`, `descriptorSets` | `vk_descriptors.hpp` | `ShaderObjects` legacy branch only; never created | delete |
| `ShaderObjects::usesDescriptorHeaps` and the set-layout branches in `createComputeShader` / `createMeshShaderPair` | `vk_shader_objects.*` | itself | delete; always `eDescriptorHeap`, no set layouts |
| `intersectSize` | `shaders/core/culling.slang` | nothing (the engine uses `isSubPixel`) | delete |

The legacy-sets branch would dereference a null set layout if it ever ran, so it is not a working fallback. The DGC guard tested a value that can no longer differ; heap support stays asserted at device creation (§20.1, device creation row).

### 1.3 Shaders

None of these is compiled by an `engine_add_slang_pass` or by a test.

| File | Content | Imported by | Action |
|---|---|---|---|
| `base.slang` | imports only | nothing | delete |
| `base/mesh.slang` | imports only; declares `module mesh` like `geometry/mesh.slang` | nothing | delete with the `base/` directory |
| `raytracing/hit.slang`, `raytracing/trace.slang` | imports only | `passes/pathtrace` | delete |
| `raytracing/payload.slang` | `RtPayload` struct | `hit`, `trace`, `pathtrace` (all deleted) | delete (orphaned) |
| `passes/composite.slang` | imports only | nothing | delete |
| `passes/pathtrace.slang`, `pathtrace_query.slang`, `pathtrace_update.slang` | imports and SHaRC defines only | each other | delete |
| `passes/sharc_resolve.slang` | include only | nothing | delete |
| `gi/rtxgi.slang` | `GI_CACHE_MODE` define, `kGiCache*` constants | `passes/pathtrace` | delete |
| `postprocessing/dlss.slang` | `DlssFrameContract` struct | `passes/composite` | delete (orphaned; [shader_architecture_plan.md](../../shader_architecture_plan.md) already marks it `[delete]`) |

`gi/sharc.slang` stays: `tests/shaders/sharc_roundtrip.slang` includes it. The `raytracing/` and `postprocessing/` directories become empty and go; M2–M4 recreate `raytracing/`.

`payload` and `dlss` are not named in AU7. They are included because the deletions leave them without an importer and their contents are superseded: the hit record of §10.1 differs from `RtPayload`, and jitter and exposure live in `GpuFrameRoot`.

### 1.4 Comments that name deleted files

| Location | Fix |
|---|---|
| `types.hpp`, comment on `GiCache` | drop the `rtxgi.slang` reference; the values are the host's choice until `IRadianceCache` (§11.4, AU5 in M5) |
| `vk_device.cpp`, SHaRC feature enable | drop "(GI_CACHE_MODE both)" |

---

## 2. Out of scope

| Item | Where it goes |
|---|---|
| `CULLING_REVERSED_Z` and the forward-Z branch | AU5 |
| `GI_CACHE_MODE` → `IRadianceCache` | AU5, M5 |
| `nrc_library` placement | AU9, M5 |
| Plans that name future files (`pbr_implementation_plan.md`, `VkExtensionsSpecs/*`, `streaming_server_spec.md`) | unchanged; they describe files to be created |
| `GiCache` enum and NRC/SHaRC device selection | kept; used at device creation |

---

## 3. Steps

| # | Step | Files |
|---|---|---|
| DC1 | Delete the zero-byte C++ files and their CMake list entries | §1.1 |
| DC2 | Delete the unused types and the descriptor-set mode; shader objects always use descriptor heaps | `types.hpp`, `vk_descriptors.hpp`, `vk_shader_objects.*`, `vk_dgc.cpp`, `vk_device.cpp` |
| DC3 | Delete the stub shaders and `intersectSize` | §1.3, `core/culling.slang` |
| DC4 | Documents: `shaders/README.md` layer table matches the tree; [shader_architecture_plan.md](../../shader_architecture_plan.md) marks deleted stubs `[new]` at their milestone; engine spec §17 row, §16.2, §20.2 AU7 status, changelog | docs |

---

## 4. Checks

| Check | Pass when |
|---|---|
| Build | `windows-clang-debug` and `windows-clang-release` configure and build with no new warnings |
| Tests | `ctest` passes in both builds (SHaRC round trip still compiles `gi/sharc.slang`) |
| References | a search for every deleted name in `src/`, `shaders/`, `tests/` and the CMake files finds nothing |
| Runtime | the engine starts, loads a model and renders the same image; validation clean (core + sync + best practices) |
