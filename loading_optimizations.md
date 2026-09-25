# Loading Optimizations

## Phase 1: Textures

**Target:** approximately `1.25 s` -> `0.2 s`

- **Decode images in parallel.** All images used by the materials are known before any upload starts. Decode them on a worker pool into an array indexed by image, then upload them on the main thread in order so texture heap indices remain stable.
	- **Where:** `parseGltfImages`, or a new pre-pass before `parseGltfMaterials`, in `assets_loader.cpp`.
	- Add upload-only entry points to `TextureManager` that accept already-decoded pixels.
	- `stb_image` is safe to call from several threads, except for `stbi_failure_reason`, which requires `STBI_THREAD_LOCAL`.
	- **Expected:** `734 ms` drops to roughly `734 ms / number of cores`.

- **Upload all textures with one submit.** Record every texture layout transition and mip-generation operation in one command buffer, then submit once and wait on a fence. This replaces 138 submits, each followed by `queue.waitIdle()`.
	- **Where:** `texture_manager.cpp`: `uploadRgba8`, `generateMipmaps`, and `endSingleTimeCommands`.
	- **Expected:** most of the `370 ms` spent on submit and mip-generation time goes away.

- **Perform invariant checks once.** Move the format checks in `stbHostCopyDstLayout` and the check at the top of `generateMipmaps` out of the per-texture path.

- **Longer term: use KTX2 with BC7 compression.** Prepare textures offline to remove decoding and mip generation from load time. This also cuts texture memory from about `370 MB` to about `90 MB`.

- **Fix the existing KTX path first.** It currently uploads each texture with its own queue wait and leaks every image (the TODO at `texture_manager.cpp:333`).

## Phase 2: Geometry

**Target:** approximately `203 ms` -> `40 ms`

- **Build meshlets in parallel.** Use one primitive per task; primitives do not depend on each other. Each task fills its own local arrays, and the main thread appends them in primitive order and applies the offsets. The result remains identical to today's output.
	- **Where:** split `GeometryStore::buildMeshletsForRange` into a pure build step and a step that appends into the store.

- **Reserve geometry memory upfront.** First add up the vertex and index counts from the accessors, then reserve all nine per-vertex arrays once. Today, `resizeVertices` grows every array for every primitive.

- **Reduce per-item logging.** Move per-item logging to debug level. Keep one summary line per model, plus errors.
	- Per-item logging includes every texture, primitive, material, attribute, and meshlet build.
	- This benefits every phase.

## Phase 3: GPU Upload and Runtime Loading

- **Upload geometry with one submit.** `flushGpuAssets` should use one staging buffer and one command buffer for all 16 buffers, with one fence wait. This currently takes only `20 ms` for Sponza, but matters when loading many small models.

- **Load models without freezing frames.** `Engine::loadObject` currently waits for the GPU to go idle and then loads everything on the main thread.

- **Move CPU work to a worker thread.** Parse, decode, and build meshlets on a worker thread, then commit the GPU upload between frames.

- **Split CPU and GPU work in the load path.** This depends on items 1 and 4 being separated into CPU and GPU parts, which puts the load-model paths in `vk_engine.cpp` and `assets_loader.cpp` in scope.