# VK_KHR_vulkan_memory_model — Implementation Spec

**Status in this repo: PROMOTED TO CORE (Vulkan 1.2), ENABLED via `Vulkan12Features::vulkanMemoryModel` + `vulkanMemoryModelDeviceScope = true`. Still required modern SPIR-V baseline — not replaced.**

Official sources:

- [VK_KHR_vulkan_memory_model](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_vulkan_memory_model.html) (historical extension name)
- [VkPhysicalDeviceVulkan12Features](https://docs.vulkan.org/spec/latest/chapters/features.html) (`vulkanMemoryModel*`)
- [Vulkan Memory Model](https://docs.vulkan.org/spec/latest/chapters/memorymodel.html)
- SPIR-V: `VulkanMemoryModel` capability / related execution modes

> **PROMOTED TO CORE — do not list as a device extension.** With API version 1.2+, enable only through `VkPhysicalDeviceVulkan12Features`. This project targets Vulkan 1.4; do **not** put `VK_KHR_vulkan_memory_model` in `requiredDeviceExtension`.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` | **Must not list** — promoted |
| Feature enable | `src/core/vk_device.cpp` → `Vulkan12Features` | `vulkanMemoryModel = true`, `vulkanMemoryModelDeviceScope = true` |
| Availability bit | same struct | `vulkanMemoryModelAvailabilityVisibilityChains` **not** forced (optional) |
| Property query | — | No dedicated properties struct |
| Capability cache | `types.hpp` | No separate field (feature-only via Vulkan12) |
| API usage | shaders / SPIR-V toolchain | Baseline for modern memory model; shaders must match |

### Already wired

```cpp
// src/core/vk_device.cpp — Vulkan12Features
.vulkanMemoryModel = true,
.vulkanMemoryModelDeviceScope = true,
```

### Not separately wired

- Explicit SPIR-V module audit for `VulkanMemoryModel` capability (toolchain responsibility)
- `vulkanMemoryModelAvailabilityVisibilityChains` (enable only if shaders need it)

---

## 2. What core provides

Defines the **Vulkan memory model** for shader memory ordering, atomics, barriers, and availability/visibility semantics — the modern replacement for the older SPIR-V “GLSL450” memory model assumptions.

Pre-1.2 shaders typically assumed GLSL450, which is not a portable contract for device-scope atomics or availability/visibility. The model is **promoted to Vulkan 1.2 core** and is enabled here via `Vulkan12Features::vulkanMemoryModel` + `vulkanMemoryModelDeviceScope` — **not** via a device-extension string.

This does **not** add host commands, property structs, or numeric limits. It does **not** replace `synchronization2` (command-side sync). Device-scope vs availability-visibility-chains are **features**, not limits. Disabling the bits breaks modern SPIR-V load/validation.


| Feature bit | Meaning | Engine relevance |
| --- | --- | --- |
| `vulkanMemoryModel` | Master switch: SPIR-V Vulkan memory model allowed | **Required** modern baseline |
| `vulkanMemoryModelDeviceScope` | Device-scope memory semantics in SPIR-V | Needed for device-wide atomics / scopes |
| `vulkanMemoryModelAvailabilityVisibilityChains` | Makes availability/visibility chain semantics available | Optional; enable if shaders use those ops |

**Still required modern baseline — not replaced.** RT, mesh, compute atomics, descriptor heaps, and concurrent resource access all assume a coherent memory model story. Toolchains (glslang/dxc/slang) typically emit Vulkan memory model SPIR-V for contemporary targets.

This is largely **baseline plumbing** once features are on: no new draw/dispatch entry points, but **wrong disablement breaks shader load / validation**.

---

## 3. Structures and commands

### 3.1 Features (Vulkan 1.2)

```cpp
// Part of VkPhysicalDeviceVulkan12Features
VkBool32 vulkanMemoryModel;
VkBool32 vulkanMemoryModelDeviceScope;
VkBool32 vulkanMemoryModelAvailabilityVisibilityChains;
```

**Already enabled:** first two bits. No new Vulkan commands.

### 3.2 SPIR-V side (shader toolchain)

Modules compiled for the Vulkan memory model typically declare:

- Capability: `VulkanMemoryModel` (and related)
- Memory model instruction: `OpMemoryModel Logical Vulkan` (wording per SPIR-V revision)

If the device feature is false but modules require the capability, pipeline creation / shader module use fails validation.

---

## Limits

There is **no dedicated property struct** and **no numeric Required Limits** row. Availability vs device-scope is a **feature**, not a limit.

| Limit / property | Spec-required (when feature supported) | Constraint | Engine implication |
| --- | --- | --- | --- |
| `vulkanMemoryModel` | feature must be **true** for Vulkan-memory-model modules | enabled in `vk_device.cpp` | Required modern SPIR-V baseline |
| `vulkanMemoryModelDeviceScope` | feature; **not** implied by the master bit | enabled in this repo | Needed for device-scope atomics / barriers |
| `vulkanMemoryModelAvailabilityVisibilityChains` | optional feature | **not** forced here | Enable only if shaders use chain ops |
| Numeric limits | **none** | — | Do not invent a property cache |

Sources: [limits](https://docs.vulkan.org/spec/latest/chapters/limits.html), [Required Limits](https://docs.vulkan.org/refpages/latest/refpages/source/Required_Limits.html).

---

## 4. Where this applies in the codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.cpp` | Keep `vulkanMemoryModel` + `vulkanMemoryModelDeviceScope` true |
| `src/core/vk_device.hpp` | Keep extension string **out** of required list |

### 4.2 Shader / pipeline path — baseline

**Files:** shader build scripts, `src/render/vk_pipeline.cpp`, any SPIR-V load path

- Ensure toolchain targets Vulkan 1.2+ memory model (aligned with `VK_API_VERSION_1_4` / SPIR-V 1.4+ — see `VK_KHR_spirv_1_4.md`)
- No runtime toggle; if shaders need availability/visibility chains, enable the third feature bit at device create

### 4.3 Concurrent GPU work

Memory model features underwrite correct interpretation of:

- Storage buffer atomics (GPU-driven / DGC paths)
- Cross-workgroup / device-scope ops
- Future RT and mesh shader memory interactions

No direct C++ call sites beyond feature enable.

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep both feature bits true | `vk_device.cpp` | Required modern baseline |
| P0 | Never re-list as device extension | `vk_device.hpp` | Promoted to 1.2 |
| P1 | Confirm shader toolchain emits Vulkan memory model | shader build | Match enabled features |
| P2 | Enable availability/visibility chains only if needed | `vk_device.cpp` | Optional bit |

### Implementation recipe

1. **Enable** — keep `.vulkanMemoryModel = true` and `.vulkanMemoryModelDeviceScope = true` on `Vulkan12Features` in `vk_device.cpp`. Never list `VK_KHR_vulkan_memory_model` in `requiredDeviceExtension` (`vk_device.hpp`).
2. **Query** — no properties struct. Features are requested on the device-create chain; the driver rejects create if unsupported.
3. **Cache** — no `HardwareCapabilities` field (feature-only). Do not add a fake limits cache.
4. **Gate** — leave both bits on. Optionally set `vulkanMemoryModelAvailabilityVisibilityChains` only if the toolchain emits those ops. Pipeline create (`vk_pipeline.cpp`) fails validation if modules declare `VulkanMemoryModel` while the feature is off.
5. **Record** — no command-buffer step. Confirm slang/dxc/glslang emit `OpMemoryModel Logical Vulkan` for `shaders/base/shader.slang` and `shaders/base/mesh.slang`.

---

## 6. Integration sketch

No command-buffer integration. Device create only:

```cpp
// Vulkan12Features fragment
.vulkanMemoryModel = true,
.vulkanMemoryModelDeviceScope = true,
// .vulkanMemoryModelAvailabilityVisibilityChains = true, // only if required
```

Shader compile (illustrative toolchain flags — exact flags depend on compiler):

```text
# Target SPIR-V with Vulkan memory model for Vulkan 1.2+ clients
# (slang / dxc / glslang options as used by this repo's shader pipeline)
```

---

## 7. Validation / test checklist

1. Device creates with memory model bits true and **no** extension string.
2. Pipeline/shader modules that declare Vulkan memory model validate cleanly.
3. Turning bits off (debug experiment only) should fail modern shader loads — confirms dependency.
4. Atomics / barriers in compute or future GPU-driven paths have no memory-model validation errors.

---

## 8. Shader interaction

**Direct — defines how shader loads, stores, atomics, and barriers order and become visible.** Features `vulkanMemoryModel` + `vulkanMemoryModelDeviceScope` (Vulkan 1.2 core) allow SPIR-V modules that declare the **Vulkan** memory model instead of legacy GLSL450 assumptions.

### SPIR-V side

Typical modern modules:

- Capability `VulkanMemoryModel` (and device-scope related bits when used)
- `OpMemoryModel Logical Vulkan` (wording per SPIR-V revision)

If shaders use device-scope atomics / barriers, `vulkanMemoryModelDeviceScope` must be true (enabled in this project). Optional `vulkanMemoryModelAvailabilityVisibilityChains` only if shaders use those chain ops.

### Why mesh / BDA / heaps care

| Pattern | Memory-model relevance |
| --- | --- |
| Cooperative mesh loads into output verts | Workgroup shared visibility within mesh WG |
| BDA loads of UBOs written by CPU | Host write → GPU visibility via barriers/queue submit; shader reads must not race host writes without sync |
| Future GPU heap updates | Heap write availability + `*_HEAP_READ` access scopes |
| RT / compute atomics | Device-scope atomics need device-scope memory model |

### Engine status

Features **enabled**. Toolchain (Slang → SPIR-V) should emit Vulkan memory model modules for the Vulkan target; no hand-written memory-model assembly. Do **not** disable these bits — validation will reject modern SPIR-V.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Vulkan 1.2+ core | Promotion home |
| SPIR-V 1.4+ / shader toolchain | Typical modern emit path |
| Descriptor heaps / untyped pointers | Rely on correct memory semantics |
| Synchronization2 (1.3) | Host/cmd sync model; complementary to shader memory model |
| Device-generated commands / mesh / RT | Heavier concurrent memory use |

---

## 10. Non-goals / do not do

- Do **not** list `VK_KHR_vulkan_memory_model` as a device extension on 1.2+.
- Do not disable the feature to “simplify” — modern shaders depend on it.
- Do not confuse this with `synchronization2` (command-side sync) — both are needed, different layers.
- Do not treat this as app-level plumbing to re-implement; it is a **feature + SPIR-V** contract.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Promotion | **Vulkan 1.2 core** |
| Enable path | `vulkanMemoryModel` + `vulkanMemoryModelDeviceScope` on `Vulkan12Features` |
| Consumed? | **Baseline** — no direct draw calls; required for modern SPIR-V |
| Still modern? | **Yes** — not replaced |
| Device extension string? | **Never** on this 1.4 target |
