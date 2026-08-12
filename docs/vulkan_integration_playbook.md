# Vulkan Extension Integration Playbook

## Goal

This playbook describes how Vulkan extensions should be integrated in this project at system level. It is intentionally architecture-first: not every path is fully implemented yet, but each path describes how it should be wired.

Use this with [docs/vulkan_extensions_reference.md](./vulkan_extensions_reference.md). The reference doc carries a source link for every extension row.

## Current Extension Tally

| Category                              | Count | Details                                                                                                                                                                                                                                                                                                                                   |
| ------------------------------------- | ----- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| KHR device extensions (enabled)       | 17    | swapchain, maintenance7-10, deferred_host_operations, acceleration_structure, ray_tracing_pipeline, fragment_shading_rate, ray_query, swapchain_maintenance1, ray_tracing_maintenance1, present_id2, calibrated_timestamps, present_mode_fifo_latest_ready, copy_memory_indirect, shader_untyped_pointers                                 |
| EXT device extensions (enabled)       | 11    | opacity_micromap, memory_budget, memory_priority, memory_decompression, descriptor_heap, blend_operation_advanced, mesh_shader, device_generated_commands, pageable_device_local_memory, present_timing, ray_tracing_invocation_reorder                                                                                                  |
| NV device extension (enabled)         | 1     | cluster_acceleration_structure                                                                                                                                                                                                                                                                                                            |
| Promoted-to-core (feature flags only) | 19    | maintenance1-6, bind_memory2, get_memory_requirements2, buffer_device_address, depth_stencil_resolve, spirv_1_4, descriptor_indexing, copy_commands2, extended_dynamic_state, extended_dynamic_state2, texture_compression_astc_hdr, tooling_info, global_priority, load_store_op_none, map_memory2, host_image_copy, vulkan_memory_model |
| Instance extensions                   | 6     | surface + platform WSI, debug_utils, display, surface_maintenance1, get_display_properties2, get_surface_capabilities2                                                                                                                                                                                                                    |
| Commented-out / disabled              | 10    | pipeline_binary, pipeline_library, graphics_pipeline_library, extended_dynamic_state (no-op in chain), texel_buffer_alignment (no-op in chain), descriptor_buffer (probe-only), multi_draw, shader_object, shader_draw_parameters (core bit false), draw_indirect_count (core bit false)                                                  |

> See [vulkan_extensions_reference.md](./vulkan_extensions_reference.md) for the full per-extension feature-flag, property-struct, and source-link mapping.

## Terminology Quick Reference

- Capability negotiation: Enumerating extension support and querying feature/property chains before `vkCreateDevice`.
- Capability cache: Storing queried limits/properties in a central struct for runtime use.
- Feature gating: Enabling code paths only when the required feature bits are true.
- Dual-path renderer: Keeping both legacy descriptor-set path and bindless descriptor-heap path.
- Optional vendor path: Extension path that must gracefully fall back on unsupported hardware.

## Usage Guides

### 1. Instance, WSI, and Debug Setup

Implementation anchor: [src/core/vk_device.cpp](../src/core/vk_device.cpp)

How to use:

- Treat `VK_KHR_surface` plus the platform surface extension selected by SDL as the baseline instance contract.
- Enable `VK_EXT_debug_utils` only when validation layers are on, and keep the debug messenger optional in release builds.
- Add `VK_KHR_display`, `VK_KHR_surface_maintenance1`, `VK_KHR_get_display_properties2`, and `VK_KHR_get_surface_capabilities2` when the display/surface capability workflow needs them.
- Query every required instance extension before `vkCreateInstance`; fail fast if the selected runtime mode cannot satisfy the WSI contract.
- Keep instance-time extension selection separate from device-time feature gating.

### 2. Device Selection and Promoted Core Features

Implementation anchor: [src/core/vk_device.cpp](../src/core/vk_device.cpp)

How to use:

- Score GPUs by device type and baseline feature support, then reject devices that miss mandatory requirements such as geometry shaders.
- Cache queue-family capabilities before logical-device creation so the renderer can make queue-placement decisions without re-querying.
- Keep all promoted features out of the device extension list: Maintenance1-6, bind_memory2, get_memory_requirements2, buffer_device_address, depth_stencil_resolve, spirv_1_4, descriptor_indexing, copy_commands2, extended_dynamic_state, extended_dynamic_state2, texture_compression_astc_hdr, tooling_info, global_priority, load_store_op_none, map_memory2, host_image_copy, and vulkan_memory_model.
- Treat `bufferDeviceAddress`, `descriptorIndexing`, and `vulkanMemoryModel` as foundational features for bindless resources and shader memory correctness.
- Build the core `Vulkan11Features` through `Vulkan14Features` structs inline in the device feature chain; do not duplicate promoted functionality as device extensions.

### 3. Descriptor Binding, Shader Compilation, and Bindless Resources

Implementation anchors:

- [src/core/vk_descriptors.cpp](../src/core/vk_descriptors.cpp)
- [src/render/vk_pipeline.cpp](../src/render/vk_pipeline.cpp)
- [src/render/vk_renderer.cpp](../src/render/vk_renderer.cpp)
- [CMakeLists.txt](../CMakeLists.txt)

How to use:

- Prefer `VK_EXT_descriptor_heap` when the runtime reports support, and treat it as a distinct binding model rather than a small tweak to descriptor sets.
- Keep the legacy descriptor-set path as the fallback so the renderer still works when heap mode is unavailable.
- Model heap usage explicitly: one sampler heap, one resource heap, heap offsets instead of implicit set/layout state, and push-data mappings for shader-facing constants.
- Keep `VK_KHR_shader_untyped_pointers` available for shader-side heap mapping and any BDA-oriented SPIR-V paths you compile.
- Use `VK_EXT_descriptor_indexing` via the promoted Vulkan 1.2 feature struct for non-uniform indexing and runtime descriptor arrays.
- Keep `VK_KHR_buffer_device_address` available when push data or shader records need to carry raw addresses.
- Treat `VK_EXT_descriptor_buffer` as probe-only and future-facing; do not make it the active binding model until the C++ path and shaders are migrated together.
- Compile shaders with the matching capability directives (`GL_EXT_descriptor_heap`, `GL_EXT_nonuniform_qualifier`, and any other explicit extension directives required by the active path).
- Keep push-data layout stable and shader-to-binding mapping explicit when using heap indexing.

### 4. GPU-Driven Geometry and Draw Submission

Implementation anchors:

- [src/render/vk_pipeline.cpp](../src/render/vk_pipeline.cpp)
- [src/render/vk_renderer.cpp](../src/render/vk_renderer.cpp)

How to use:

- Build graphics pipelines with dynamic rendering support so the render path does not depend on a rigid legacy render-pass shape.
- Use `VK_EXT_mesh_shader` for GPU-generated geometry when task/mesh support is available.
- Add `VK_EXT_multi_draw` before jumping to fully GPU-generated command streams; it is the lowest-risk way to reduce CPU submission overhead.
- Add `VK_EXT_device_generated_commands` when the draw/dispatch emission itself should move onto the GPU.
- Use `VK_KHR_copy_memory_indirect` for streaming and upload work that benefits from GPU-driven copy scheduling.
- Keep `VK_EXT_shader_object` as an optional future pipeline path; the stable default remains full pipeline objects.
- Keep descriptor resource binding aligned with the chosen descriptor mode, and do not let the GPU-driven path bypass the fallback CPU path.

### 5. Swapchain and Presentation Timing

Implementation anchors:

- [src/core/vk_swapchain.cpp](../src/core/vk_swapchain.cpp)
- [src/render/vk_renderer.cpp](../src/render/vk_renderer.cpp)

How to use:

- Make `VK_KHR_swapchain` the base present path and keep out-of-date/suboptimal handling robust.
- Use `VK_KHR_swapchain_maintenance1` to smooth swapchain maintenance behavior and keep the present path resilient.
- Correlate frame IDs and timelines with `VK_KHR_present_id2` and `VK_KHR_calibrated_timestamps`.
- Keep `VK_EXT_present_timing` disabled until telemetry and frame-pacing plumbing are wired end to end.
- Use `VK_KHR_present_mode_fifo_latest_ready` where supported when the low-latency FIFO policy matches the frame pacing goal.
- Expose presentation telemetry only as an optional debugging and pacing aid; it should never block the core present path.

### 6. Memory Budget, Priority, and Residency

Implementation anchors:

- [src/core/vk_allocator.hpp](../src/core/vk_allocator.hpp)
- [src/core/vk_descriptors.cpp](../src/core/vk_descriptors.cpp)

How to use:

- Poll `VK_EXT_memory_budget` before making residency or eviction decisions.
- Use `VK_EXT_memory_priority` to raise priority on critical resources only.
- Add `VK_EXT_pageable_device_local_memory` hooks when a device can evict or reclaim local memory.
- Use `VK_EXT_memory_decompression` for streaming paths that benefit from decompression-aware upload or staging.
- Keep `VK_KHR_bind_memory2`, `VK_KHR_get_memory_requirements2`, `VK_KHR_map_memory2`, and `VK_EXT_host_image_copy` in the promoted memory toolbox.
- Centralize allocator policy in the capability cache so memory decisions stay consistent across render, upload, and descriptor code.

### 7. Modern Rendering-State and Maintenance Features

Implementation anchors:

- [src/render/vk_pipeline.cpp](../src/render/vk_pipeline.cpp)
- [src/render/vk_renderer.cpp](../src/render/vk_renderer.cpp)
- [src/core/vk_device.cpp](../src/core/vk_device.cpp)

How to use:

- Use `VK_KHR_fragment_shading_rate` for variable-rate shading only when the queried properties and attachment rules match the intended pass.
- Keep `VK_KHR_maintenance7`, `VK_KHR_maintenance8`, `VK_KHR_maintenance9`, and `VK_KHR_maintenance10` feature bits and property queries in sync with the actual rendering behavior you depend on.
- Treat `VK_EXT_blend_operation_advanced` as an explicit opt-in path; the baseline build keeps it off.
- Keep `VK_EXT_extended_dynamic_state`, `VK_EXT_extended_dynamic_state2`, `VK_KHR_load_store_op_none`, and `VK_KHR_depth_stencil_resolve` in the promoted-core bucket rather than reintroducing them as device extensions.
- Use `VK_EXT_texture_compression_astc_hdr` as a format-support gate, not as a generic renderer toggle.
- Use `VK_EXT_tooling_info` for diagnostics and introspection, and `VK_KHR_global_priority` for queue-priority behavior, not pipeline behavior.
- Keep dynamic rendering, attachment resolves, and load/store behavior consistent across passes whenever a maintenance extension changes the rules.

### 8. Ray Tracing and Advanced Geometry

Primary extension stack:

- `VK_KHR_acceleration_structure`
- `VK_KHR_ray_tracing_pipeline`
- `VK_KHR_ray_query`
- `VK_KHR_ray_tracing_maintenance1`
- `VK_EXT_ray_tracing_invocation_reorder`
- `VK_EXT_opacity_micromap`
- `VK_NV_cluster_acceleration_structure`

How to use:

- Keep `VK_KHR_deferred_host_operations` enabled because acceleration-structure builds and copies depend on it.
- Build and manage BLAS/TLAS resources, scratch memory, and acceleration-structure lifetimes through `VK_KHR_acceleration_structure`.
- Add shader binding table management and ray-generation/hit/miss stage wiring through `VK_KHR_ray_tracing_pipeline`.
- Use `VK_KHR_ray_query` for inline hybrid passes when a full ray-tracing dispatch is more expensive than a query.
- Keep `VK_KHR_ray_tracing_maintenance1` in the path so indirect-trace and maintenance behaviors remain explicit.
- Integrate `VK_EXT_opacity_micromap` and `VK_EXT_ray_tracing_invocation_reorder` as optional quality/perf multipliers, not baseline requirements.
- Keep `VK_NV_cluster_acceleration_structure` vendor-gated and fold it into the same acceleration-structure management layer rather than creating a separate stack.
- Make the SBT layout, shader interface, and fallback path explicit so the renderer can switch between RT and non-RT modes cleanly.

### 9. Future, Disabled, and Vendor-Specific Tracks

How to use:

- Keep `VK_EXT_descriptor_buffer` in the future/probe-only bucket until the descriptor model, push-data mapping, and shaders are migrated together.
- Keep `VK_KHR_pipeline_binary`, `VK_KHR_pipeline_library`, and `VK_EXT_graphics_pipeline_library` behind explicit feature flags and a concrete pipeline-cache rollout plan.
- Keep `VK_EXT_texel_buffer_alignment` in the probe-only bucket until its alignment constraints are wired through the renderer and resource layout rules.
- Integrate placeholder vendor technologies only once their fallback behavior is fully defined and documented.
- Do not advertise a future path as active until both the C++ binding/pipeline code and the shader path agree on the same model.

## Placeholder Sections (Requested)

The following sections are intentionally short placeholders. They are future integration stubs and not implementation instructions yet.

### NVIDIA RTXGI (placeholder)

Status: TODO

- Intent: Integrate DDGI-style global illumination on top of KHR ray tracing stack.
- Entry points:
  - New module under `src/raytracing/rtxgi`.
  - Volume update pass integrated into renderer frame graph.
  - Probe lighting resources managed by allocator and descriptor path.
- Reference: [RTXGI-DDGI repository](https://github.com/NVIDIAGameWorks/RTXGI-DDGI)

### NVIDIA RTXDI (placeholder)

Status: TODO

- Intent: Add ReSTIR direct/indirect light sampling in ray-traced lighting stages.
- Entry points:
  - New module under `src/raytracing/rtxdi`.
  - Light reservoir buffers and temporal history resources.
  - Optional denoiser stage after RTXDI outputs.
- Reference: [RTXDI repository](https://github.com/NVIDIA-RTX/RTXDI)

### NVIDIA RTX Mega Geometry (placeholder)

Status: TODO

- Intent: Add very-large-geometry ray tracing acceleration integration path.
- Entry points:
  - New module under `src/raytracing/rtx_mega_geometry`.
  - Geometry streaming and acceleration structure update policy.
  - Integration with vendor-specific acceleration extension path.
- Source status: Official stable public technical reference link pending validation in this workspace.

### NVIDIA RTXMU (placeholder)

Status: TODO

- Intent: Reduce acceleration-structure memory footprint with compaction and suballocation utilities.
- Entry points:
  - AS memory manager layer under `src/raytracing/rtxmu`.
  - BLAS/TLAS build-compaction lifecycle integration.
  - Residency and garbage-collection hooks.
- Reference: [RTXMU repository](https://github.com/NVIDIA-RTX/RTXMU)

### NVIDIA DLSS Upscale (placeholder)

Status: TODO

- Intent: Add optional vendor upscaler path after main render and before final presentation.
- Entry points:
  - New module under `src/postprocessing/upscale_dlss`.
  - Motion vector, depth, exposure, and reactive-mask feed-in path.
  - Swapchain output and UI composition compatibility checks.
- Reference: [NVIDIA DLSS developer page](https://developer.nvidia.com/rtx/dlss)

### AMD FSR Upscale (placeholder)

Status: TODO

- Intent: Add cross-vendor temporal upscaling and optional frame-generation path.
- Entry points:
  - New module under `src/postprocessing/upscale_fsr`.
  - Motion vectors, depth, and mask generation integration.
  - Quality mode and frame pacing configuration in renderer settings.
- Reference: [AMD FidelityFX Super Resolution](https://gpuopen.com/fidelityfx-super-resolution-3/)

## Suggested Rollout Order

1. Stabilize extension-feature-property synchronization and source-link coverage.
2. Complete descriptor architecture path parity (heap and legacy).
3. Add present timing telemetry and frame pacing controls.
4. Land baseline KHR ray tracing path.
5. Integrate placeholder technologies one by one behind feature flags.

## Maintenance Rules

1. Any change in extension list, feature chain, or source-link mapping must update this playbook and [docs/vulkan_extensions_reference.md](./vulkan_extensions_reference.md).
2. Any new optional vendor path must have explicit fallback behavior documented.
3. Any shader capability flag change must be reflected in both CMake shader compile settings and device feature enablement.
4. Before adding a promoted extension to the device extension list, verify it is not already covered by a core `Vulkan1xFeatures` struct. Promoted-to-core extensions go in the Promoted-to-Core section of the reference doc, not the device extension tables.
5. The extension tally table at the top of this playbook must stay in sync with the actual `requiredDeviceExtension` list + promoted extensions.
