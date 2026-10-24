# VK_KHR_spirv_1_4 — Implementation Spec

**Status in this repo: PROMOTED TO CORE (Vulkan 1.2), ALWAYS AVAILABLE at 1.2+. Still valid shader toolchain target — not replaced.**

Official sources:

- [VK_KHR_spirv_1_4](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_spirv_1_4.html) (historical extension name)
- [SPIR-V 1.4 specification](https://registry.khronos.org/SPIR-V/)
- Vulkan 1.2 core: SPIR-V 1.4 accepted without enabling a device extension

> **PROMOTED TO CORE — do not list as a device extension.** At Vulkan 1.2+, implementations accept SPIR-V 1.4 modules as part of core. This project targets Vulkan 1.4; do **not** put `VK_KHR_spirv_1_4` in `requiredDeviceExtension`.

> **No `Vulkan12Features` bit** for “spirv 1.4” — availability is implied by the API version. Higher SPIR-V versions may apply with 1.3/1.4; this doc records the 1.2 promotion baseline.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` | **Must not list** — promoted (1.2 comment block) |
| Instance / device API version | `vk_device.cpp`, `vk_allocator.hpp`, ImGui init | **Vulkan 1.4** |
| Feature enable | — | **N/A** (always available 1.2+) |
| Shader modules | pipeline / SPIR-V load path | Consume whatever SPIR-V the toolchain emits (should be ≥ 1.4-capable for modern features) |

### Already wired (API level)

```cpp
// instance create — apiVersion = vk::ApiVersion14
// VMA: vulkanApiVersion = VK_API_VERSION_1_4
// ImGui: ApiVersion = VK_API_VERSION_1_4
```

No separate spirv-1.4 feature struct or property cache.

---

## 2. What core provides

Permission to pass **SPIR-V 1.4** binaries to `vkCreateShaderModule` without the historical device extension. SPIR-V 1.4 is the baseline that unlocked several language features used by modern pipelines (including interactions later used by broader Vulkan 1.2+ shader ecosystem).

Vulkan 1.0/1.1 accepted only older SPIR-V revisions; several 1.2-era language features required 1.4. Historical `VK_KHR_spirv_1_4` is **promoted to Vulkan 1.2 core** and is always available at 1.2+ with **no feature bit**. This repo targets **Vulkan 1.4**, so SPIR-V 1.4 is the toolchain floor, not a runtime switch.

This does **not** add Vulkan structs, commands, or a property cache. It does **not** cap the module version at 1.4 — mesh, RT, heaps, and untyped pointers may need a higher SPIR-V env. It does **not** replace the Vulkan memory model (see `VK_KHR_vulkan_memory_model.md`).


| Item | Notes |
| --- | --- |
| SPIR-V 1.4 modules | Legal under Vulkan 1.2+ core |
| Toolchain target | Compilers should target at least 1.4 when using 1.2+ device features |
| Later SPIR-V | Vulkan 1.3/1.4 and extensions may require newer SPIR-V; 1.4 remains the 1.2 floor |

**Still valid** — this is **baseline toolchain plumbing**, not a runtime toggle. Not replaced by untyped pointers or mesh shaders; those assume a modern SPIR-V version stream.

---

## 3. Structures and commands

No new Vulkan structs or commands from the promotion.

Shader module create remains:

```cpp
vk::ShaderModuleCreateInfo info{
    .codeSize = spirvBytes,
    .pCode = spirvWords
};
device.createShaderModule(info);
```

Ensure `code` is valid SPIR-V for the advertised version and enabled capabilities/features.

---

## Limits

There is **no dedicated property struct** and no numeric Required Limits row. This is a **toolchain / API-version floor**.

| Limit / property | Spec-required (when feature supported) | Constraint | Engine implication |
| --- | --- | --- | --- |
| Accepted SPIR-V version | SPIR-V **1.4** legal at Vulkan **1.2+** without an extension string | higher SPIR-V allowed when API/features permit | This repo targets Vulkan **1.4**; compilers should emit ≥ 1.4 |
| Feature / property struct | **None** | availability implied by `apiVersion ≥ 1.2` | Do not invent a `Vulkan12Features` bit |
| Module capabilities | must match **enabled** device features | validation rejects excess caps | Gate shader paths on features already enabled in `vk_device.cpp` |

Sources: [limits](https://docs.vulkan.org/spec/latest/chapters/limits.html), [Required Limits](https://docs.vulkan.org/refpages/latest/refpages/source/Required_Limits.html).

---

## 4. Where this applies in the codebase

| Area | Action |
| --- | --- |
| Device API version | Keep 1.4 — implies ≥ 1.2 SPIR-V 1.4 acceptance |
| Extension list | Never add `VK_KHR_spirv_1_4` |
| Shader build | Target SPIR-V appropriate for enabled features (1.4 minimum floor for 1.2-era features; higher as needed for 1.3/1.4/ext) |
| Pipeline create | No special spirv-1.4 flag |

---

## 5. Recommended implementation order

| Priority | Change | Why |
| --- | --- | --- |
| P0 | Keep API 1.4; never list extension | Correct promoted posture |
| P1 | Align shader compiler `-fspv-target-env` / equivalent with device features | Avoid capability mismatches |
| P2 | Document per-feature SPIR-V requirements in other specs | e.g. untyped pointers, mesh |

### Implementation recipe

1. **Enable** — keep API 1.4 (`vk_device.cpp` instance create, `vk_allocator.hpp` `VK_API_VERSION_1_4`). Never list `VK_KHR_spirv_1_4` in `requiredDeviceExtension` (`vk_device.hpp` 1.2 promoted block).
2. **Query** — no property or feature query for “spirv 1.4”. Confirm `capabilities.properties2.properties.apiVersion` is 1.4 after device pick.
3. **Cache** — no `HardwareCapabilities` field. The toolchain target lives in the shader build, not a runtime cache.
4. **Gate** — `createShaderModule` has no spirv-1.4 flag. Avoid modules whose declared version or capabilities exceed enabled features (mesh, RT, BDA, …).
5. **Record** — no command-buffer step. Align slangc/dxc/glslang `-fspv-target-env` (or equivalent) with a Vulkan 1.2+ / SPIR-V 1.4 floor; raise the env only when a feature requires it (`shaders/base/shader.slang`, `shaders/base/mesh.slang`).

---

## 6. Integration sketch

No C++ enable chain entry. Toolchain only:

```text
# Example intent — exact flags depend on slang/dxc/glslang setup in-repo
# Target Vulkan 1.2+ / SPIR-V 1.4 or higher matching feature use
```

---

## 7. Validation / test checklist

1. No `VK_KHR_spirv_1_4` in device extensions.
2. Shader modules create without SPIR-V version validation errors.
3. Modules that need >1.4 still match Vulkan 1.4 / extension capabilities.

---

## 8. Shader interaction

**Direct toolchain baseline — every engine SPIR-V module is subject to the accepted SPIR-V version.** At Vulkan 1.2+, SPIR-V **1.4** is accepted without a device extension string. This project targets **Vulkan 1.4**, so modules may use SPIR-V features allowed by 1.4+ and by enabled extensions (mesh, RT, heaps, untyped pointers, etc.).

### What shaders / compilers must respect

| Item | Guidance |
| --- | --- |
| Module version | slangc / glslang target ≥ SPIR-V 1.4 for modern Vulkan 1.2+ features |
| Capabilities | Declared capabilities must match **enabled** device features (mesh, ray query, BDA, …) |
| Entry points | Multiple entry points per module allowed (SPIR-V 1.4+); engine currently separate files per path |
| Memory model | Prefer Vulkan memory model (see [VK_KHR_vulkan_memory_model](./VK_KHR_vulkan_memory_model.md)) |

### Engine shaders

| Source | Stages | Notes |
| --- | --- | --- |
| `shaders/base/shader.slang` | vertex + fragment | Classic path; BDA + DescriptorHandle |
| `shaders/base/mesh.slang` | mesh + fragment | Meshlet BDA path; requires mesh SPIR-V capabilities |

Pipeline create fails if SPIR-V version or capabilities exceed device API/features. No runtime “spirv 1.4” toggle — it is implied by API version.

## 9. Dependencies and interactions

| Sibling | Interaction |
| --- | --- |
| Vulkan 1.2+ API version | Core acceptance of SPIR-V 1.4 |
| `vulkanMemoryModel` | Modern memory model in SPIR-V |
| Mesh / RT / untyped pointers | Additional SPIR-V caps; higher env may be required |

---

## 10. Non-goals / do not do

- Do **not** re-enable as a device extension on 1.2+.
- Do not invent a fake feature bit in `Vulkan12Features` for spirv 1.4.
- Do not pin forever to 1.4 if newer SPIR-V is required by later features — treat 1.4 as the **1.2 floor**, not a ceiling.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Promotion | **Vulkan 1.2 core** (always available at 1.2+) |
| Enable path | API version ≥ 1.2; no feature flag |
| Consumed? | **Toolchain baseline** |
| Still modern? | **Yes** — SPIR-V floor for modern Vulkan |
| Device extension string? | **Never** on this 1.4 target |
