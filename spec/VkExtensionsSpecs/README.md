# Vulkan Extension Implementation Specs

Per-extension implementation docs for this engine. Standard headings in each file:

1. Current enablement audit  
2. What the extension / core provides  
3. Structures and commands  
4. **Limits** — spec-required minima / maxima (from [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html#limits-minmax)), implementation-dependent properties, and engine implications  
5. Where to implement in this codebase  
6. Recommended implementation order (step-by-step recipe)  
7. Integration sketch  
8. Validation / test checklist  
9. **Shader interaction** — SPIR-V / Slang stages, builtins, push data, descriptors, barriers (or explicitly **None** / **Indirect**)  
10. Dependencies and interactions  
11. Non-goals  
12. Summary  

Status line + official sources sit above §1.

Older specs may still number **Where to implement** as §4; treat **Limits** as the heading that must exist after **Structures and commands**, even if later section numbers are not yet shifted.

### Shader interaction guide

When reading or extending a spec, use **§ Shader interaction** for anything that compiles to or binds programmable stages:

| Kind | Meaning | Examples |
| --- | --- | --- |
| **Direct** | New stages, builtins, storage classes, or required SPIR-V capabilities | mesh shaders, BDA, ray query/pipeline, descriptor heaps, draw parameters, untyped pointers, VMM, SPIR-V 1.4 |
| **Indirect** | Host/pipeline/sync/format setup that must stay compatible with existing shaders | multi-draw, DGC, dynamic rendering, sync2 stage masks, maintenance flags, ASTC formats |
| **None** | No shader-facing surface | swapchain, memory budget/priority, present timing, pure host copies |

Engine shader sources today: `shaders/base/mesh.slang` (mesh+FS, meshlet BDA + `DescriptorHandle`). Draw path is DGC `DRAW_MESH_TASKS` — see [VK_EXT_mesh_shader.md](./VK_EXT_mesh_shader.md).

The overview integration map remains [docs/vulkan_extensions_reference.md](../../docs/vulkan_extensions_reference.md).

**Two families of docs:**

1. **Device extensions** — still listed in `requiredDeviceExtension` (maintenance7–10, RT, mesh, heaps, …).  
2. **Promoted-to-core** — enabled only via `VkPhysicalDeviceVulkan1xFeatures` / always-available core; **never** re-listed as device extensions. Still valid on a modern 1.4 target (later maintenance is additive, not a replacement).

## Promoted-to-core (Vulkan 1.1–1.4)

| Feature / original extension | Core version | Impl doc | Status (high level) |
| --- | --- | --- | --- |
| maintenance1 + maintenance2 | 1.1 always | [Vulkan_1_1_maintenance_baseline.md](./Vulkan_1_1_maintenance_baseline.md) | Always on baseline |
| `VK_KHR_maintenance3` | 1.1 always | [VK_KHR_maintenance3.md](./VK_KHR_maintenance3.md) | Props cached |
| bind_memory2 + get_memory_requirements2 | 1.1 always | [VK_KHR_bind_and_memory_requirements2.md](./VK_KHR_bind_and_memory_requirements2.md) | VMA path |
| `VK_KHR_buffer_device_address` | 1.2 | [VK_KHR_buffer_device_address.md](./VK_KHR_buffer_device_address.md) | **Consumed** |
| `VK_EXT_descriptor_indexing` | 1.2 | [VK_EXT_descriptor_indexing.md](./VK_EXT_descriptor_indexing.md) | Enabled; coexists with descriptor heap |
| `VK_KHR_vulkan_memory_model` | 1.2 | [VK_KHR_vulkan_memory_model.md](./VK_KHR_vulkan_memory_model.md) | Enabled |
| `VK_KHR_depth_stencil_resolve` | 1.2 always | [VK_KHR_depth_stencil_resolve.md](./VK_KHR_depth_stencil_resolve.md) | Props cached |
| `VK_KHR_spirv_1_4` | 1.2 always | [VK_KHR_spirv_1_4.md](./VK_KHR_spirv_1_4.md) | Toolchain baseline |
| `VK_KHR_dynamic_rendering` | 1.3 | [VK_KHR_dynamic_rendering.md](./VK_KHR_dynamic_rendering.md) | **Consumed** |
| `VK_KHR_synchronization2` | 1.3 | [VK_KHR_synchronization2.md](./VK_KHR_synchronization2.md) | **Consumed** |
| `VK_KHR_copy_commands2` | 1.3 always | [VK_KHR_copy_commands2.md](./VK_KHR_copy_commands2.md) | Prefer `*2` copies |
| EDS1 + EDS2 → core dynamic state | 1.3 always | [Vulkan_1_3_extended_dynamic_state.md](./Vulkan_1_3_extended_dynamic_state.md) | Core states; EDS3 still extension |
| `VK_KHR_load_store_op_none` | 1.3 always | [VK_KHR_load_store_op_none.md](./VK_KHR_load_store_op_none.md) | Always available |
| `VK_KHR_map_memory2` | 1.3/1.4 core | [VK_KHR_map_memory2.md](./VK_KHR_map_memory2.md) | VMA / map path |
| `VK_KHR_maintenance4` | 1.3 | [VK_KHR_maintenance4.md](./VK_KHR_maintenance4.md) | Enabled + VMA |
| ASTC HDR | 1.3 optional | [VK_EXT_texture_compression_astc_hdr.md](./VK_EXT_texture_compression_astc_hdr.md) | Feature **false** (GPU-gated) |
| `VK_KHR_maintenance5` | 1.4 | [VK_KHR_maintenance5.md](./VK_KHR_maintenance5.md) | Consumed (inline SPIR-V / Flags2 / VMA) |
| `VK_KHR_maintenance6` | 1.4 | [VK_KHR_maintenance6.md](./VK_KHR_maintenance6.md) | Enabled, not consumed (props cached only) |
| `VK_KHR_global_priority` | 1.4 | [VK_KHR_global_priority.md](./VK_KHR_global_priority.md) | Query enabled, not used |
| `VK_EXT_host_image_copy` | 1.4 | [VK_EXT_host_image_copy.md](./VK_EXT_host_image_copy.md) | Consumed (STB host-only) |
| `VK_KHR_dynamic_rendering_local_read` | 1.4 | [VK_KHR_dynamic_rendering_local_read.md](./VK_KHR_dynamic_rendering_local_read.md) | Enabled, not consumed |

### Intentionally not given a full impl doc

| Item | Why |
| --- | --- |
| `VK_EXT_tooling_info` | Diagnostic-only core query; no engine integration surface beyond optional tooling |
| `VK_EXT_multi_draw` / `VK_EXT_shader_object` | Mesh + DGC + `VkPipeline` path; not enabled |
| `VK_KHR_shader_draw_parameters` / `VK_KHR_draw_indirect_count` | Classic vertex/index draws; core bits left false |
| Superseded exclusive 1.0-only paths | Prefer core `*2` / sync2 / dynamic rendering docs instead of teaching obsolete primary paths |
| Re-enable as device extension | Forbidden when the feature is already core for the requested API version |

## Device extensions (enabled list)

| Extension | Impl doc | Status (high level) |
| --- | --- | --- |
| `VK_KHR_swapchain` | [VK_KHR_swapchain.md](./VK_KHR_swapchain.md) | Fully consumed |
| `VK_KHR_swapchain_maintenance1` | [VK_KHR_swapchain_maintenance1.md](./VK_KHR_swapchain_maintenance1.md) | Partially consumed |
| `VK_KHR_present_mode_fifo_latest_ready` | [VK_KHR_present_mode_fifo_latest_ready.md](./VK_KHR_present_mode_fifo_latest_ready.md) | Partially consumed |
| `VK_KHR_present_id2` | [VK_KHR_present_id2.md](./VK_KHR_present_id2.md) | Dependency only |
| `VK_KHR_calibrated_timestamps` | [VK_KHR_calibrated_timestamps.md](./VK_KHR_calibrated_timestamps.md) | Dependency only |
| `VK_KHR_maintenance7` | [VK_KHR_maintenance7.md](./VK_KHR_maintenance7.md) | Enabled, not consumed (props cached only) |
| `VK_KHR_maintenance8` | [VK_KHR_maintenance8.md](./VK_KHR_maintenance8.md) | Enabled, not consumed |
| `VK_KHR_maintenance9` | [VK_KHR_maintenance9.md](./VK_KHR_maintenance9.md) | Partial (QFOT-mask log; feature false; m9 props cached only) |
| `VK_KHR_maintenance10` | [VK_KHR_maintenance10.md](./VK_KHR_maintenance10.md) | Enabled, not consumed (props cached only) |
| `VK_KHR_deferred_host_operations` | [VK_KHR_deferred_host_operations.md](./VK_KHR_deferred_host_operations.md) | AS dependency |
| `VK_KHR_acceleration_structure` | [VK_KHR_acceleration_structure.md](./VK_KHR_acceleration_structure.md) | Enabled, not consumed |
| `VK_KHR_ray_tracing_pipeline` | [VK_KHR_ray_tracing_pipeline.md](./VK_KHR_ray_tracing_pipeline.md) | Enabled, not consumed |
| `VK_KHR_ray_query` | [VK_KHR_ray_query.md](./VK_KHR_ray_query.md) | Enabled, not consumed |
| `VK_KHR_ray_tracing_maintenance1` | [VK_KHR_ray_tracing_maintenance1.md](./VK_KHR_ray_tracing_maintenance1.md) | Enabled, not consumed |
| `VK_KHR_fragment_shading_rate` | [VK_KHR_fragment_shading_rate.md](./VK_KHR_fragment_shading_rate.md) | Enabled, not consumed |
| `VK_KHR_copy_memory_indirect` | [VK_KHR_copy_memory_indirect.md](./VK_KHR_copy_memory_indirect.md) | Enabled, not consumed |
| `VK_KHR_shader_untyped_pointers` | [VK_KHR_shader_untyped_pointers.md](./VK_KHR_shader_untyped_pointers.md) | Enabled, not consumed |
| `VK_KHR_device_address_commands` | [VK_KHR_device_address_commands.md](./VK_KHR_device_address_commands.md) | Enabled, not consumed |
| `VK_EXT_descriptor_heap` | [VK_EXT_descriptor_heap.md](./VK_EXT_descriptor_heap.md) | Partially consumed (runtime-gated) |
| `VK_EXT_memory_budget` | [VK_EXT_memory_budget.md](./VK_EXT_memory_budget.md) | Partially consumed (VMA) |
| `VK_EXT_memory_priority` | [VK_EXT_memory_priority.md](./VK_EXT_memory_priority.md) | Partially consumed (VMA priority) |
| `VK_EXT_memory_decompression` | [VK_EXT_memory_decompression.md](./VK_EXT_memory_decompression.md) | Enabled, not consumed |
| `VK_EXT_pageable_device_local_memory` | [VK_EXT_pageable_device_local_memory.md](./VK_EXT_pageable_device_local_memory.md) | Enabled, not consumed |
| `VK_EXT_opacity_micromap` | [VK_EXT_opacity_micromap.md](./VK_EXT_opacity_micromap.md) | Enabled, not consumed |
| `VK_EXT_mesh_shader` | [VK_EXT_mesh_shader.md](./VK_EXT_mesh_shader.md) | Consumed (mesh pipeline + DGC DRAW_MESH_TASKS; no host drawMeshTasksEXT) |
| `VK_EXT_device_generated_commands` | [VK_EXT_device_generated_commands.md](./VK_EXT_device_generated_commands.md) | Consumed (mesh DGC only) |
| `VK_EXT_blend_operation_advanced` | [VK_EXT_blend_operation_advanced.md](./VK_EXT_blend_operation_advanced.md) | Listed; coherent ops off |
| `VK_EXT_present_timing` | [VK_EXT_present_timing.md](./VK_EXT_present_timing.md) | Extension listed; features not enabled |
| `VK_EXT_ray_tracing_invocation_reorder` | [VK_EXT_ray_tracing_invocation_reorder.md](./VK_EXT_ray_tracing_invocation_reorder.md) | Enabled, not consumed |
| `VK_EXT_extended_dynamic_state3` | [VK_EXT_extended_dynamic_state3.md](./VK_EXT_extended_dynamic_state3.md) | Extension listed; features/cmds unused |
| `VK_NV_cluster_acceleration_structure` | [VK_NV_cluster_acceleration_structure.md](./VK_NV_cluster_acceleration_structure.md) | Enabled, not consumed (vendor) |
| `VK_NV_partitioned_acceleration_structure` | [VK_NV_partitioned_acceleration_structure.md](./VK_NV_partitioned_acceleration_structure.md) | Enabled, not consumed (vendor) |

## How to add a new extension

1. Enable list + feature/property chains in `vk_device.*` / `types.hpp`.  
2. Copy [VK_KHR_maintenance10.md](./VK_KHR_maintenance10.md) as the template.  
3. Fill enablement audit from real code; structures from official Vulkan docs (prefer Context7 / docs.vulkan.org for current text).  
4. Always add **§ Shader interaction**: Direct / Indirect / None, with Slang/SPIR-V notes when Direct, and engine file anchors when content exists.  
5. Link the new file from [vulkan_extensions_reference.md](../../docs/vulkan_extensions_reference.md) and this README.
