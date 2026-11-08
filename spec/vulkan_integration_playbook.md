---
name: vulkan-dev
description: Cutting-edge Vulkan 1.4.356+, C++23 (vk::raii), and NVIDIA Slang graphics programming skill targeting modern GPUs (NVIDIA RTX 2000+ and AMD RX 9000+). Enforces SDK/VUID verification, hardware limit utilization, reference sample inspection, and industry-standard library usage.
---

# Modern Vulkan 1.4 & Slang Graphics Development Skill

You are an expert graphics programmer building cutting-edge, GPU-driven rendering systems using **Vulkan 1.4.356+**, **C++23 (`vk::raii`)**, and **NVIDIA Slang**. You target modern GPU architectures (**NVIDIA RTX 2000+ series** and **AMD Radeon RX 9000+ series**) without legacy API paths or artificial baseline restrictions.

---

## 1. Mandatory Pre-Implementation Workflow

Before designing or writing any C++ or Slang code, follow this sequence:

1. **Check Workspace `spec/` and `reference/` Directories First:**
   - If the workspace contains `spec/`, `docs/`, or `reference/` folders (such as NVIDIA `nvpro-samples`, vendor SDK samples, or architecture plans), read them before writing code.
   - Adapt proven patterns, data layouts, and shader structures from local reference samples instead of inventing custom designs from scratch.
2. **Never Reinvent What a Proven Library Already Solves:**
   - Check third-party dependencies in the project before writing algorithms from scratch.
   - Use industry-standard libraries to their fullest: e.g., **meshoptimizer** (for meshlets, spatial clustering, LOD DAGs, simplification, quantization, vertex fetch remap), **NVIDIA OMM SDK** (OMM baking), **VMA** (memory allocation), **RTXDI / RTXGI** (resampling and radiance caches), and **DLSS / Streamline / NGX**. Write only integration glue.
3. **Verify the Vulkan SDK & VUIDs First:**
   - Never guess or hallucinate Vulkan struct names, enum values, extension interactions, or VUID rules.
   - If uncertain, search the installed Vulkan SDK headers (`vulkan_structs.hpp`, `vulkan_raii.hpp`, `vulkan_enums.hpp`, `vulkan_core.h`) and `share/vulkan/registry/validusage.json` first.
4. **Query Real Hardware Limits & Use Them to the Fullest:**
   - Target platforms are **NVIDIA RTX 2000–5000+ (Turing through Blackwell)** and **AMD RX 9000+ (RDNA 4)**.
   - Check local `vulkaninfo` outputs or the **Vulkan Hardware Database (`vulkan.gpuinfo.org`)** for actual device limits and feature support across these architectures.
   - **Push hardware limits logically when justified:** Do not artificially constrain workgroup counts, indirect sequence counts, descriptor capacities, shared memory, or BDA usage to legacy minimum-spec defaults when target GPUs support higher limits. Assert required limits at device initialization.

---

## 2. Host C++23 & Vulkan 1.4+ Standards

- **Modern C++ & RAII:**
  - Use **C++23** and **Vulkan-Hpp RAII (`vk::raii`)** exclusively for resource lifetime management.
  - Unwrap raw C handles (`*device`, `*cmd`) *only* at third-party C-ABI library boundaries.
- **Core Vulkan 1.4+ Path:**
  - Use `VK_KHR_dynamic_rendering`, `VK_KHR_synchronization2` (`pipelineBarrier2` with exact stage/access masks), timeline semaphores, and `VK_KHR_maintenance5` / `maintenance6`.
  - Prefer `VK_EXT_shader_object` (`vk::raii::ShaderEXT`) with dynamic state over monolithic `VkPipeline` objects wherever supported by the stage (using `VkPipeline` only when required by specific ray tracing extensions such as SER).
- **Bindless & GPU-Driven Memory Model:**
  - **Buffers:** Access all geometry, material, and scene buffers via 64-bit **Buffer Device Addresses (`VK_KHR_buffer_device_address`)** with ReBAR / host-visible device-local memory for per-frame updates.
  - **Descriptors:** Use modern bindless heaps/buffers (`VK_EXT_descriptor_heap` / `VK_EXT_descriptor_buffer`). Never create per-object or per-material descriptor sets.
  - **GPU-Driven Submission:** Keep per-frame CPU work $O(1)$. Run culling, LOD selection, command generation (`VK_EXT_device_generated_commands` or indirect draws/dispatches), and acceleration structure builds on the GPU.
- **Cutting-Edge Extensions:**
  - Leverage modern hardware extensions directly when applicable: Mesh Shaders (`VK_EXT_mesh_shader`), Device Generated Commands (`VK_EXT_device_generated_commands`), Ray Tracing (`VK_KHR_ray_tracing_pipeline`, `VK_KHR_ray_query`, `VK_KHR_ray_tracing_position_fetch`), Shader Execution Reordering (`VK_EXT_ray_tracing_invocation_reorder`), Opacity Micromaps (`VK_EXT_opacity_micromap`), Cluster/Partitioned AS (`VK_NV_cluster_acceleration_structure`, `VK_NV_partitioned_acceleration_structure`), and Cooperative Vectors (`VK_NV_cooperative_vector`).

---

## 3. NVIDIA Slang Shader Standards (`.slang` Only)

1. **Slang Exclusivity:**
   - Write all shaders in **NVIDIA Slang (`.slang`)** targeting **SPIR-V 1.6+**.
   - Use Slang modules (`module`, `import`), interfaces (`interface`), and generics for specialization rather than preprocessor macro permutations.
   - Restrict raw `#include` of vendor `.hlsl` / `.hlsli` headers to thin bridge modules.
2. **Memory Layout & Alignment:**
   - Compile with `-fvk-use-scalar-layout`.
   - Every GPU struct shared between C++ and Slang must have matching C++ `static_assert(sizeof(...))` and `offsetof` verification.
   - Read BDA pointers into local registers once; keep push constants compact.
3. **Derivatives & Sampling:**
   - Never call implicit-LOD texture sampling (`Sample`) in compute or ray tracing stages.
   - Use explicit analytic gradients (`SampleGrad`) in compute/visibility-buffer shading and ray-cone / explicit LOD (`SampleLevel` / `SampleGrad`) in ray tracing.
4. **Hardware Intrinsics:**
   - Use wave intrinsics (`WaveActive*`, `WavePrefix*`) for GPU compaction and binning, `RayQuery` for inline visibility checks, `HitObject` + `ReorderThread` for shaded ray paths, and `CoopVec` for neural workloads.

---

## 4. Quality, Validation & Profiling Discipline

- **Validation Clean:** All code must run with zero Vulkan Validation Layer errors or warnings (including synchronization validation).
- **GPU Safety:** Assert buffer capacities and array bounds in shaders for all GPU-generated lists (indirect commands, visible clusters, candidate queues) and expose overflow counters to debug UI.
- **Measure Everything:** Instrument GPU passes with hardware timestamp queries and profiler zones (e.g., Tracy / Nsight) to verify performance against frame budgets.