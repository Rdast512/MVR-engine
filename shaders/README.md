### What each layer owns

| Path | Task |
|---|---|
| `types.slang` | Shared POD + `FrameRoot` (camera, instances, clusters, materials, TLAS handle, BDA tables) |
| `base.slang` | Your current raster: amplification / mesh / textured PBR fragment |
| `core/binding` | `tex2D(handle)`, `tlasOf`, BDA load |
| `geo/mesh` | Meshlet fetch/cull used by `base` |
| `geo/mega_geometry` | Cluster LOD records, CLAS address table, `ClusterDraw` list |
| `geo/mega_tess` | Tile + fill verts before CLAS instantiate |
| `material/*` | Bindless texture fetch + PBR/BRDF |
| `rt/*` | Payload, trace helpers, shared hit (includes Mega cluster id) |
| `gi/rtxgi` | Cache mode switch (SHaRC / NRC / both) |
| `gi/sharc` | Hash / accum / resolved buffers + update/query/resolve |
| `gi/nrc` | Train/query records for the NRC runtime |
| `post/gbuffer` | DLSS-RR guide packing (color, depth, MV, albedo, spec albedo, n+rough, hitT) |
| `post/dlss` | Jitter / exposure contract only — NGX is not Slang |
| `passes/*` | Compile units, one dispatch or RT pipeline each |

### Intended GPU order

1. `mega_select` → optional `mega_tess` → **host CLAS/BLAS/TLAS**
2. `gbuffer` and/or `pathtrace` (primary + guides)
3. `pathtrace` with `SHARC_UPDATE` / `NRC_TRAIN`
4. `sharc_resolve` + `nrc_pack` → **host NRC infer**
5. `pathtrace` query path writes noisy HDR + guides
6. **host NGX DLSS-RR**
7. `composite`

`types.slang` and `base.slang` stay at the shader root so your current includes keep working. Fill bodies next; do not add descriptor-set bindings — keep handles + BDA only.