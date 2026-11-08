### What each layer owns

| Path | Task |
|---|---|
| `types.slang` | Shared POD + `GpuFrameRoot` (camera, instances, model slices, primitives, materials, lights, TLAS) |
| `core/frame` | Push data → `GpuFrameRoot` → instance, model, primitive, material |
| `base.slang` | Your current raster: amplification / mesh / textured PBR fragment |
| `core/binding` | `tex2D(handle)`, `tlasOf`, BDA load |
| `geo/mesh` | Meshlet fetch/cull used by `base` |
| `material/*` | Bindless texture fetch + PBR/BRDF |
| `rt/*` | Payload, trace helpers, shared hit |
| `gi/rtxgi` | Cache mode switch (SHaRC / NRC / both) |
| `gi/sharc` | Hash / accum / resolved buffers + update/query/resolve |
| `post/dlss` | Jitter / exposure contract only — NGX is not Slang |
| `passes/*` | Compile units, one dispatch or RT pipeline each |

### GPU order

The frame order is [docs/engine_spec.md §5](../docs/engine_spec.md#5-frame-overview).

Library files are Slang modules: a pass writes `import core.frame;` for each module it uses, and imports are not transitive. Exports are `public`, including every member of a shared struct. Vendor HLSL headers are `#include`d only from pass and bridge files. Fill bodies next; do not add descriptor-set bindings — keep handles + BDA only.