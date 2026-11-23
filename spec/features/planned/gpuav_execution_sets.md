# Feature: GPU-AV with the resolve's execution set (C-E1-3)

**Date:** 2026-11-22 · **Status:** dropped 2026-11-22 (user): the local patched layer is not pursued. The analysis in §0 stays as reference for an upstream report. VVL #13244 (heap shader-object crash in `DescriptorHeapPass`, fixed for 1.4.363) is a different bug · **Milestone:** M8 tooling; unblocks GPU-AV for I10 (MG-P2, MG-P4, MG-P5, MG-S1) and every later shader step
**Fixes:** considerations C-E1-3 ([considerations.md](../../considerations.md)): GPU-AV turns itself off at the three `INDIRECT_BINDABLE` resolve shaders, and the process then crashes in frame 1
**Touches:** [m7/e1_material_binning.md](../../m7/e1_material_binning.md) (the execution set), `src/render/vk_shader_objects.cpp:131`, `src/render/vk_dgc.cpp:163-236`, `src/render/vk_renderer.cpp:43-46`
**Reference:** Vulkan-ValidationLayers at tags `vulkan-sdk-1.4.363.0` and `.1` (both match the installed SDK 1.4.363.0 byte for byte in the lines below) and `main` at `c70feb6` (2026-11-20), all with the same code:
- `layers/gpuav/instrumentation/gpuav_shader_instrumentor.cpp`:
  - `PreCallRecordCreateShadersEXT` (main :664-760): the check at :700-705, heap mode at :733-740;
  - `AddDescriptorHeapMappings` (:1406);
  - `push_data_offset_` (:197);
  - `InternalError` (:2084-2097);
  - `NeedPipelineCreationShaderInstrumentation` (:1249-1257), the pipeline variant of the check;
- `layers/gpuav/core/gpuav_record.cpp:874` `PreCallRecordCmdExecuteGeneratedCommandsEXT`.

No upstream issue covers this; the search was run on 2026-11-22. The nearest is #13704, which is about GPU-AV's vertex-attribute-fetch check with DGC indexed draws. The engine has no vertex input, so #13704 does not apply.

---

## 0. Why the layer refuses, and why the reason does not apply here

`PreCallRecordCreateShadersEXT` calls `InternalError` for every shader object created with `VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT` (main :700-705). The call is unconditional. It runs before the selective-instrumentation test, so regex or `pNext` selection cannot exclude a shader (C-E1-3's finding, confirmed in the source).

The message gives the reason: GPU-AV binds its own data as `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC` in an extra descriptor set. `VUID-VkIndirectExecutionSetPipelineInfoEXT-initialPipeline-11019` forbids dynamic buffers in a layout used by an execution set (comment at :1249-1251).

That reason holds only in **set-layout mode**. The engine's shaders are descriptor-heap shaders: `VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT`, mandatory, per the memory rule "descriptor heaps mandatory". For those, the same function takes the heap branch (:733-740), and `AddDescriptorHeapMappings` (:1406) maps GPU-AV's bindings as follows:

- source: `VK_DESCRIPTOR_MAPPING_SOURCE_INDIRECT_ADDRESS_EXT`;
- address: read from push data at `push_data_offset_ = maxPushDataSize - 8` (:197).

No descriptor set and no dynamic buffer is involved, so VUID-11019 cannot be hit. The check is simply older than heap mode.

**The crash has a second cause:**

- `InternalError` sets `aborted_` and releases the GPU-AV object (:2093-2096);
- `PreCallRecordCreateShadersEXT` does not `continue` after the error;
- it goes on to instrument the shader and marks `chassis_state.is_modified`.

What dereferences what after that is not pinned down. Two candidates:

1. the chassis records the shader creation on the released object;
2. the instrumented resolve runs without GPU-AV ever pushing its address, and reads whatever is at `maxPushDataSize - 8`.

V1 settles it. The 2026-11-20 log says "segfaults in frame 1", a CPU fault, which points at candidate 1.

## 1. Decision

**Fix the layer, not the engine.** The engine's command path stays exactly as shipped: DGC execution set, `INDIRECT_BINDABLE` heap shaders. GPU-AV then validates the real resolve.

- The per-bin `vkCmdDispatchIndirect` build of 2026-11-20 is kept only as the fallback (§4). It does not validate the shipped path.
- Engine spec §9.2 forbids it as the command path. Making it a switch is the user's call.

| Part | What | Lands in |
|---|---|---|
| Layer patch | (a) skip the execution-set error for descriptor-heap shaders; (b) `continue` (skip instrumentation) after any `InternalError` in shader creation | `tools/vvl/0001-gpuav-heap-indirect-bindable.patch` |
| Layer build | the SDK's tag plus the patch, built locally | `tools/vvl/build_vvl.ps1` → `build/vvl-1.4.363.0/` (not committed) |
| Engine | keep GPU-AV's 8 push bytes free: startup check `kMaxPushDataBytes + 8 <= maxPushDataSize` | `vk_renderer.cpp:43-46` |
| Upstream | issue + PR text (§5) | submitted by the user |

## 2. The patch

Against `vulkan-sdk-1.4.363.0`, in `PreCallRecordCreateShadersEXT`:

```cpp
// heap shaders read GPU-AV's data through an indirect-address mapping, not a dynamic SSBO, so VUID-11019 cannot apply
const bool isHeapShader = (new_create_info.flags & VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT) != 0;
if ((new_create_info.flags & VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT) && !isHeapShader) {
    InternalError(device, create_info_loc, "Unable to instrument shader using VkIndirectExecutionSetEXT validly, ...");
    continue;  // GPU-AV is released: never hand the driver an instrumented shader it will not feed
}
```

- **Shader objects only.** The pipeline variant (:1249-1257) gets the same treatment only for pipelines created with `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT`. The engine has no indirect-bindable pipelines, so that part is for upstream only and is not tested here.
- **Execution-set consistency.** Every shader of one execution set must agree on its interface. An instrumented shader gains the same `kTotalBindings` mappings at the same push offset as every other instrumented shader. All three bins are instrumented together, so the set stays consistent.
- **Selective instrumentation off** (the default), so the three bins can never be split into instrumented and uninstrumented.

## 3. Steps

| Step | Work | Done when |
|---|---|---|
| **V1 · Locate the crash** | build the unpatched tag as RelWithDebInfo; run the engine under vkconfig with GPU-AV on and a debugger attached | the faulting frame is named: §0 candidate 1, candidate 2 or something else. §0 is corrected if it is something else |
| **V2 · Patch and build** | `build_vvl.ps1`: clone the tag into `build/vvl-src` (outside the repo tree's sources), apply the patch, build with `-DUPDATE_DEPS=ON`, and write `VkLayer_khronos_validation.{dll,json}` to `build/vvl-1.4.363.0/`. Point vkconfig's layer paths at that directory and select it for the profile. When vkconfig is off, use `VK_ADD_LAYER_PATH` (memory note on per-process layers) | the engine starts with GPU-AV on. No `UNASSIGNED-GPU-Assisted-Validation` internal error. The layer reports itself as the local build (`vkEnumerateInstanceLayerProperties` implementation version, logged at startup) |
| **V3 · Prove the execution set is instrumented** | a temporary, reverted edit: `resolveUnlitMain` reads one `uint` past its tile list. This needs an unlit pixel: `ENGINE_GUIDE_CAPTURE` on `UnlitTest`. Check two things: (1) GPU-AV reports the OOB with the shader's debug name `ResolveUnlit`, and attributes it to the `vkCmdExecuteGeneratedCommandsEXT` of the resolve; (2) the other bins report nothing | the injected error is reported once per frame, with the right shader. Without the edit the same run is clean. If nothing is reported, the push address did not reach the shader through DGC: go to §4 |
| **V4 · Full GPU-AV pass** | core + sync + best practices + GPU-AV, debug and release, on: `ENGINE_GUIDE_CAPTURE`, `ENGINE_MOTION_TEST`, `ENGINE_CULL_CHECK`, `ENGINE_RT_CHECK`, `ENGINE_GI_CHECK`, the café runners (`ENGINE_CHECK_SCENE=bistro_cafe`), and startup, resize and a scene switch | every message is either fixed, or logged as its own consideration with a stamp. The first full GPU-AV run since E1 may find real bugs; they belong to their owning steps, not to this spec |
| **V5 · Engine push-byte rule** | the startup check reserves GPU-AV's last 8 push bytes, with a comment naming `push_data_offset_` | builds; current blocks are far below it (`ResolvePushData` 16 B, the largest block is checked at startup) |
| **V6 · Records** | C-E1-3 → resolved section, with the local-layer route and its exit. Memory `validation-via-vkconfig` updated. E1 spec status line updated. The `ctest` layout tests are unchanged | done |

Exit for the local layer: an SDK release whose layer creates indirect-bindable heap shaders without the internal error. Then V3's injection is re-run once on the stock layer, and `tools/vvl/` is removed.

## 4. Fallback: per-bin dispatch under GPU-AV (only if V3 fails)

If GPU-AV's push address does not survive the DGC execute, V3 reports nothing. The cause would be the driver or layer dropping push data the sequence's `PUSH_DATA` token did not write. In that case the layer patch is not enough.

The 2026-11-20 build then becomes a switch, `ENGINE_GPUAV_RESOLVE_PER_BIN=1`:

- the bin shaders are created without `INDIRECT_BINDABLE`;
- there is no execution set;
- one `vkCmdDispatchIndirect` per bin, from `ResolveDgcSequence::dispatch` at offset 24 of the same sequence buffer.

The switch is off by default. The checks and bench log it, and bench mode refuses it. This needs the user's approval, since it contradicts engine spec §9.2 for the duration of a GPU-AV run.

## 5. Upstream (user submits)

Issue for KhronosGroup/Vulkan-ValidationLayers, with no personal or project details:

- **Title:** GPU-AV disables itself for `VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT` descriptor-heap shader objects, then crashes.
- **Body:**
  - The check in `PreCallRecordCreateShadersEXT` exists for VUID-11019 (no dynamic buffers in execution-set layouts). Heap-mode instrumentation uses `INDIRECT_ADDRESS` mappings from push data, so the VUID cannot apply.
  - After `InternalError`, instrumentation continues on a released validation object, which crashes the process.
  - Repro: a compute shader object with `DESCRIPTOR_HEAP | INDIRECT_BINDABLE` in a `SHADER_OBJECTS` execution set, executed through `vkCmdExecuteGeneratedCommandsEXT` with an `EXECUTION_SET` + `PUSH_DATA` + `DISPATCH` layout.
  - Attach the §2 patch and V3's result.

## 6. Done when

- [ ] V1: crash site named
- [ ] V2: patched layer built and selected; no internal error at startup
- [ ] V3: an injected OOB inside the execution set reported with the right shader; clean without it
- [ ] V4: every check runner run under GPU-AV; messages fixed or logged
- [ ] V5, V6: push-byte rule in; C-E1-3 resolved, memory and E1 status updated
- [ ] §5 issue text ready for the user
