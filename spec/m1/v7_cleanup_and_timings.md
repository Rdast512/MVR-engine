# M1 · V7 — Visibility cleanup and stage timing

**Status** — Cleanup done. RTX 5070 baseline recorded on 2026-11-13.
[Visibility-buffer V7](../features/visibility_buffer.md) · [M0 floor-tier gate](../m0/a10_culling_baseline.md)

## Decisions and instrumentation

The opaque visibility path needs clip positions and the primitive
payload. Geometry IDs, edges and guides come from compute debug passes.
Remove `mesh_debug`, `RasterVariant`, obsolete varyings and the fragment
barycentric feature requirement.

Maintain separate early and late scopes for culling, DGC preprocessing,
raster and Hi-Z. Time resolve separately. The bench aggregates matching
scopes for each reported stage. The acquire wait is GPU idle time and
is outside total GPU work.
[Shader state](../../src/render/vk_shader_objects.cpp) · [Renderer](../../src/render/vk_renderer.cpp) · [Bench harness](../../src/core/engine_bench.cpp)

## Tasks and recorded results

| Task | State and evidence |
|---|---|
| Remove obsolete raster and shader variants | Done. V5/V7 history and current raster and debug paths. |
| Record Release stage timings without layers | RTX 5070 done. Driver 621.01, 1280×720, 3,000 measured frames. |
| Record floor-tier timings and image | Open under M0 C-M0-1. |

| Mean RTX 5070 time | Sponza startup | + 128×128 bunny field |
|---|---:|---:|
| Cull + DGC preprocess + Hi-Z | 0.086 ms | 0.182 ms |
| Visibility raster | 0.031 ms | 0.032 ms |
| Resolve and guides | 0.147 ms | 0.139 ms |
| GPU total with debug, present and ImGui | 0.308 ms | 0.395 ms |

The separate cull check recorded 16/16 identical poses on reverse Z.
[Engine spec §2.3](../../docs/engine_spec.md) · [V8 A/B](v8_payload_comparison.md)
