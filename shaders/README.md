### What each layer owns

| Path | Task |
|---|---|
| `types.slang` | Shared POD + `GpuFrameRoot` (camera, instances, model slices, primitives, materials, lights, TLAS) |
| `core/frame` | Push data → `GpuFrameRoot` → instance, model, primitive, material |
| `core/binding` | `tex2D(handle)`, `samplerOf`, BDA load, bounds-checked `CheckedBuffer<T>` |
| `core/culling` | Frustum, Hi-Z and sub-pixel tests |
| `geometry/*` | Meshlet, vertex and triangle fetch; triangle cull; vis-buffer payload; surface interpolation |
| `material/*` | Bindless texture fetch + PBR/BRDF |
| `lighting/lights` | `evalLight`: KHR_lights_punctual radiance, range window, spot cone |
| `lighting/rtxdi_bridge`, `lighting/path_shading` | RTXDI application bridge (textual); hit shading and one sampled light at a path vertex |
| `gi/nrc_bridge` | NRC SDK HLSL over BDA (textual), `GpuNrcConstants` mirror |
| `raytracing/ser`, `passes/rt_ser_check` | SER trace (`traceShaded`) and its check: shelved, not built (considerations C-G1-3) |
| `passes/*` | Compile units, one dispatch or RT pipeline each |

Files land with the milestone that fills them ([docs/engine_spec.md §16.2](../docs/engine_spec.md#162-milestones)); no empty stubs.

### GPU order

The frame order is [docs/engine_spec.md §5](../docs/engine_spec.md#5-frame-overview).

Library files are Slang modules: a pass writes `import core.frame;` for each module it uses, and imports are not transitive. Exports are `public`, including every member of a shared struct. Vendor HLSL headers are `#include`d only from pass and bridge files. Fill bodies next; do not add descriptor-set bindings — keep handles + BDA only.