# M0 · Contracts

**Status** — Built. RTX 2060 Super verification remains open (C-M0-1).\
**Reviewed** — 2026-11-16 against the working tree at `9ae42eac2`.

The engine spec §16.2 owns milestone acceptance. PBR Phase A defines the
shading contracts, and Considerations tracks open issues and their exits.
[Engine spec](../../docs/engine_spec.md) · [PBR plan](../pbr_implementation_plan.md) · [Considerations](../considerations.md)

| Task | Contract | State |
|---|---|---|
| [A1 · GPU layouts and capabilities](a1_gpu_contracts.md) | Shared C++ and Slang layouts, material packing, target bindings and startup properties | Built |
| [A2 · Slang modules and frame data](a2_slang_modules.md) | Module imports, build dependencies, frame-root writes and layout reflection | Built |
| [A3 · Cleanup and audit follow-ups](a3_cleanup.md) | Obsolete shader removal and related audit fixes | Built |
| [Animation removal](animation_removal.md) | Static base-pose geometry for raster, culling and RT | Built |
| [A4 · Static geometry and vertex attributes](a4_shading_attributes.md) | Base-pose meshes, generated flat normals and compatible tangents | Built and checked on RTX 5070 |
| [A10 · Culling and baseline](a10_culling_baseline.md) | Bounded GPU lists and baseline measurements | Lists built. RTX 5070 baseline recorded. RTX 2060 Super open. |

M1 uses the target block and final vertex streams for the visibility resolve.
M2 supplies the RT scene addresses. M3 supplies jitter, world lights, RTXDI
constants and the exposure image. M4 uses the SER properties and owns
`cacheConstants`. M6 uses the OMM properties, M7 owns shading from the material
extension rows, and M9 uses the decompression properties.
[Visibility buffer](../features/visibility_buffer.md) · [M4 SER](../m4/g1_ser_rt_pipeline.md) · [M4 NRC](../m4/g3_nrc.md)

C-M0-1 owns the closure evidence. C-M0-5 tracks the optional GPU scene update,
and C-M0-7 tracks the Debug `gltf_ktx2` failure.
[Considerations](../considerations.md)
