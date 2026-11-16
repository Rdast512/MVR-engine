# M0 · A1 — GPU layouts, materials and device capabilities

**Status** — Built. A1 landed on 2026-11-08. Layout reflection followed on 2026-11-13.\
**Implements** — PBR A1 and §4.2. Engine spec §6.2 and §16.2.\
**Used by** — M1 resolve, M2 hit decode, M3 lighting, M4 G1, M6 OMM and M8 decompression.\
[PBR plan](../pbr_implementation_plan.md) · [Engine spec](../../docs/engine_spec.md)

## Shared layout

| Contract | Current size | Rule |
|---|---:|---|
| `GpuFrameRoot` | 192 B | One device address gives passes access to scene tables and frame data. `targets` is at byte 128. |
| `GpuTargets` | 200 B | Per-frame-slot block written by `RenderTargets`. Holds image and sampler heap indices, guide sets and sizes. Slang reads it through `loadTargets(root)`. |
| `GpuRayInstance` | 8 B | Two 32-bit indices, `nodeInstanceIndex` and `primitiveRow`. M2 populates the table. |
| `GpuMaterial` | 240 B | `GpuPrimitive::materialId` indexes the shared material table. Core shading reads one row. |
| `GpuLight` | 64 B | `GpuLightType` and Slang `LightType` agree on Directional = 0, Point = 1 and Spot = 2. |

Slang `AlphaMode`, material flag constants and `kFrameFlagHistoryReset` mirror
the host encodings. Keep C++ size and offset assertions and Slang layout
reflection in sync when a later milestone adds a field.
[C++ types](../../src/core/types.hpp) · [Slang types](../../shaders/types.slang) · [Layout test](../../tests/layout_test.cpp)

## Material packing

- `AssetBuffers` folds `KHR_materials_emissive_strength` into the uploaded
  `emissiveFactor`. The CPU material store retains authored values.
- The loader applies `KHR_texture_transform.texCoord` overrides to the slot's
  UV selection. Packing stores `T × R × S` as a 2×3 matrix for each transformed
  slot. With v increasing down, the rotation rows are `[c, s]` and `[-s, c]`.
- Flag bits 4–8 enable the matrices in base-color, metallic-roughness, normal,
  occlusion and emissive order. Identity slots skip the matrix read. The
  sampler transforms the UVs and gradients, and the normal slot also
  transforms the tangent frame.
- `GpuMaterialExtension` has a 724 B Slang mirror and a row loader in the current
  source. Core shading reads `GpuMaterial`. Extension shading belongs to the
  M7 material bins.

[Asset loader](../../src/core/assets_loader.cpp) · [GPU packing](../../src/core/vk_asset_buffers.cpp) · [Material sampler](../../shaders/material/pbr.slang) · [Frame helpers](../../shaders/core/frame.slang)

## Device properties

`Device` caches opacity-micromap and invocation-reorder properties in
`HardwareCapabilities`. Startup logs report the SER reorder hint and maximum
SBT record index, and the decompression method mask and maximum indirect count.
M4 G1, M6 and M8 own the uses of these queried values.
[Device initialization](../../src/core/vk_device.cpp) · [M4 G1](../m4/g1_ser_rt_pipeline.md)

## Verification

The host assertions guard layout at compile time. `ctest -R '^layout$'` checks
reflected shader sizes and field offsets. PBR B1 records the later M3 guide
checks for `TextureTransformTest` and `TextureTransformMultiTest`.
[Layout test registration](../../tests/CMakeLists.txt) · [PBR plan](../pbr_implementation_plan.md)

## Log

| Date | Change |
|---|---|
| 2026-11-08 | A1 material and frame contracts landed |
| 2026-11-13 | Slang reflection added under AU5 and AU11 |
| 2026-11-16 | Documented current layouts and later milestone consumers |
