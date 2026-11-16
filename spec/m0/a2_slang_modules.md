# M0 · A2 — Slang modules and frame data

**Status** — Built, 2026-11-08.\
**Implements** — PBR A2 and §4.1–4.5. Engine spec AU5 and AU11.\
[PBR plan](../pbr_implementation_plan.md) · [Engine spec](../../docs/engine_spec.md)

## Build and binding

- Shader libraries declare a plain module name such as `module binding`.
  Passes import by path, for example `import core.binding`. Vendor headers
  with required include order use textual includes at their integration points.
- `engine_add_slang_pass` builds SPIR-V 1.6 with scalar layout and column-major
  matrices. The Slang depfile tracks transitive imports, and the pass source
  is an explicit CMake dependency.
- Buffers use typed BDA pointers. Images and samplers use descriptor heap
  indices. Callers check `kNoneIndex` before constructing a heap handle.
  `storageTex2D` encodes the view format in `RWTexture2D<T, 0, format>`.
- Keep material texture indexing through `sampleSlot`. PBR plan §4.4 records
  missing `NonUniform` decoration on the pinned Slang 2026.20 probe. Recheck
  the emitted SPIR-V on compiler upgrades.

[Build rules](../../CMakeLists.txt) · [Binding helpers](../../shaders/core/binding.slang) · [Material sampler](../../shaders/material/pbr.slang)

## Frame-root ownership

`Renderer` passes the active slot's addresses and frame index to `FrameData`.
`FrameData::write` publishes the root for that slot. `RenderTargets` owns the
target block. M2 supplies `tlas` and `rayInstances`, and M3 supplies the RTXDI
and world-light addresses. `cacheConstants` remains zero pending M4.
`GpuFrameRoot.exposure` stays `1.0f`. H2 passes the exposure image to DLSS-RR.
Jitter lives in `GpuCameraData::jitterOffset`.
[Frame data](../../src/core/vk_frame_data.cpp) · [Renderer](../../src/render/vk_renderer.cpp) · [Render targets](../../src/render/vk_render_targets.cpp) · [Considerations, C-M0-6 and C-H2-1](../considerations.md)

## Toolchain decisions

| Probe | Consequence |
|---|---|
| Slang v2026.20 accepted a plain module identifier and a path in the import, typed pointers, typed storage handles and a 64-bit address converted to a ray-tracing AS. | Share shader functions through modules and bind scene buffers through the frame root. Vendor headers that depend on text order still use includes. |
| Command-line defines reach imported units. Source-file defines have lexical include scope. | Pass SDK flags at the compile unit and preserve ordering for SDK headers and bridge macros. |
| Applying a format annotation to a heap handle left the SPIR-V image type unknown. | Encode format in the storage image type and verify the enum mapping by reflection. |
| Slang depfiles include transitive dependencies. | Ninja rebuilds a pass when an imported module changes, without a manually maintained import list. |

The module probes above are historical. The current build pins v2026.22.
The compiler upgrade does not itself establish that the v2026.20
`NonUniform` observation has been resolved.
[Compiler pin](../../ThirdParty/CMakeLists.txt) · [A2 probes and §4.4](../pbr_implementation_plan.md)

## Slot update sequence

1. Wait for the frame slot before rewriting host-visible data. Prepare
   the active camera and target blocks and collect the current geometry,
   culling and lighting addresses.
2. Publish the slot's `GpuFrameRoot` in one write. Use per-pass push data
   to pass that address into the shader. Each later milestone owns the
   lifetime and readiness of the addresses it supplies.
3. A consumer loads the block it needs. Use typed pointers for tables and
   bindless handles for target views. A history reset makes the old world
   light address zero and disables reuse through the frame flag.

## Tasks and acceptance

| Task | State | Evidence |
|---|---|---|
| Convert library modules and pass imports | Done | Release SPIR-V comparison at bring-up. |
| Track imported sources in the build | Done | `engine_add_slang_pass` supplies Slang's depfile and CMake's `DEPFILE`. |
| Publish frame-root additions by slot | Done | Frame index and scalar exposure are written by `FrameData`. C2/C4 and F1/F2 now fill their addresses. |
| Check host and shader layouts | Done | AU5/AU11 use `layout` CTest to check field counts, offsets and format constants. |

## Verification

The PBR plan records byte-identical release SPIR-V across all eight original
passes during the module conversion. The separate `layout` CTest checks
reflected struct sizes and field offsets against C++ and checks the typed
storage-image format constants.
[PBR plan, A2](../pbr_implementation_plan.md) · [Layout test](../../tests/layout_test.cpp)

## Log

| Date | Change |
|---|---|
| 2026-11-08 | Module conversion and frame-root fields landed |
| 2026-11-13 | Typed storage images and reflected layout checks landed |
