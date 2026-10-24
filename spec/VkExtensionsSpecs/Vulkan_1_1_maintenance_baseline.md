# Vulkan 1.1 maintenance baseline (KHR_maintenance1 + KHR_maintenance2) — Implementation Spec

**Status in this repo: PROMOTED TO CORE (Vulkan 1.1), ALWAYS AVAILABLE at 1.1+; no feature flags. Always-on baseline plumbing — not something to re-enable as extensions. NOT replaced by later maintenance (cumulative).**

Official sources:

- [VK_KHR_maintenance1](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_maintenance1.html) (historical)
- [VK_KHR_maintenance2](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_maintenance2.html) (historical)
- Vulkan 1.1 core feature set / changelog for maintenance1–2 promotions

> **PROMOTED TO CORE — do not list as device extensions.** `vk_device.hpp` already notes: *Promoted to 1.1: Maintenance1–3, …*. This project targets Vulkan 1.4; never put `VK_KHR_maintenance1` or `VK_KHR_maintenance2` in `requiredDeviceExtension`.

> Combined short spec: little direct app action beyond targeting 1.1+ and using core APIs. Maintenance3 (limits) is documented separately in `VK_KHR_maintenance3.md`.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` | **Must not list** maintenance1/2 |
| Feature enable | — | **None** — always available at 1.1+ |
| API version | instance / device / VMA | **1.4** implies full 1.1 maintenance baseline |
| Explicit property cache | `types.hpp` | No dedicated maintenance1/2 property structs required for normal use |
| App-level re-enable | — | **Do not** |

There is nothing to set in `Vulkan11Features` for maintenance1/2 specifically — they are core behavior once the API version is ≥ 1.1.

---

## 2. What the baseline provides

Small, cumulative correctness and flexibility fixes from the first two maintenance extensions. Examples of the **class** of fixes (not an exhaustive re-statement of the old extension text):

Vulkan 1.0 left several image-view, render-pass, and secondary-inheritance edge cases missing or under-specified. Maintenance1+2 closed those gaps and were **promoted wholesale into Vulkan 1.1** — always on at 1.1+, **no feature bits**, **no device-extension strings**. This repo targets Vulkan 1.4, so the baseline is implied by API version.

This baseline does **not** add dedicated property structs, enable flags, or new draw/dispatch commands. It does **not** subsume later maintenance (3–10); those remain separate, cumulative docs. App work is “target 1.1+ and use core APIs,” not “implement an extension.”


| Area (illustrative) | Why it still matters |
| --- | --- |
| 2D array image views / layer handling refinements | Texture arrays, atlases, bindless views |
| Render-pass / layout flexibility fixes | Older RP edge cases; dynamic rendering reduces some pressure |
| Command buffer / secondary inheritance clarifications | Nested recording patterns |
| Miscellaneous 1.0 gaps closed in 1.1 | Portable behavior across vendors |

**Cumulative, not replaced:** Maintenance4/5/6/7/… add *more* fixes and optional features. They do **not** mean “turn off” or “forget” 1.1 maintenance. Each generation builds on the previous core baseline.

**App action:** almost none — **always-on baseline**. Do not re-enable as extensions; do not treat as optional device features.

---

## 3. Structures and commands

No dedicated enable structs for maintenance1/2 in modern core use.

Relevant posture:

```cpp
// Instance / device: Vulkan 1.1+ (this repo: 1.4)
// Do NOT:
//   requiredDeviceExtension.push_back(VK_KHR_MAINTENANCE1_EXTENSION_NAME);
//   requiredDeviceExtension.push_back(VK_KHR_MAINTENANCE2_EXTENSION_NAME);
```

Later optional maintenance (7–10 in this engine) **do** use extension names + feature bits because they are not yet core on all target paths — that is intentional and separate from 1.1 baseline.

---

## Limits

There is **no dedicated property struct** for maintenance1+2. Behavioral and numeric floors live in core `VkPhysicalDeviceLimits` (`HardwareCapabilities::properties2.limits`) and in maintenance3 (see `VK_KHR_maintenance3.md`).

| Limit / property | Spec-required (when feature supported) | Constraint | Engine implication |
| --- | --- | --- | --- |
| `maxImageDimension2D` | min **4096** (core); **8192** on 1.4/roadmap devices | implementation-dependent above floor — query `properties2.limits` | `pickPhysicalDevice()` already scores on this (`vk_device.cpp`) |
| `maxPushConstantsSize` | min **128** bytes | implementation-dependent above floor | Do not assume more than 128 without querying |
| `maxBoundDescriptorSets` | min **4** | heap path is less set-bound | Legacy set layouts must stay within the queried count |
| `maxMemoryAllocationCount` | min **4096** | implementation-dependent above floor | VMA / device alloc planning |
| `minUniformBufferOffsetAlignment` / `minStorageBufferOffsetAlignment` | power of two | implementation-dependent — query `properties2.limits` | Do not hardcode |
| `nonCoherentAtomSize` | power of two | implementation-dependent — query | Host-visible flush/invalidate granularity |
| maintenance3 (`maxPerSetDescriptors`, `maxMemoryAllocationSize`) | see `VK_KHR_maintenance3.md` | cached separately | Not part of maintenance1+2 |

Sources: [limits](https://docs.vulkan.org/spec/latest/chapters/limits.html), [Required Limits](https://docs.vulkan.org/refpages/latest/refpages/source/Required_Limits.html).

---

## 4. Where this applies in the codebase

| Area | Implication |
| --- | --- |
| `vk_device.hpp` promoted comment | Source of truth: maintenance1–3 not listed |
| Texture / image view creation | Rely on 1.1+ view rules without extension branches |
| Any `#ifdef` on maintenance1/2 | Should not exist for 1.4-only engine |
| Docs / checklists | Treat as permanent core floor |

No renderer feature work is required to “implement maintenance1/2.”

---

## 5. Recommended implementation order

| Priority | Change | Why |
| --- | --- | --- |
| P0 | Keep API ≥ 1.1 (already 1.4); never list maintenance1/2 | Correct baseline |
| P0 | Do not add feature toggles for maintenance1/2 | No flags |
| P1 | Keep separate docs for maintenance3+ that have properties/features | Actionable later maintenance |

### Implementation recipe

1. **Enable** — keep instance/device/VMA at Vulkan 1.4 (`vk_device.cpp` `apiVersion = vk::ApiVersion14`, `vk_allocator.hpp` `VK_API_VERSION_1_4`). Do **not** push `VK_KHR_MAINTENANCE1_EXTENSION_NAME` or `VK_KHR_MAINTENANCE2_EXTENSION_NAME` into `requiredDeviceExtension` (`vk_device.hpp` “Promoted to 1.1” comment).
2. **Query** — no maintenance1/2 property query. Core limits already arrive via `physicalDevice.getProperties2` → `HardwareCapabilities::properties2` (`vk_device.cpp`).
3. **Cache** — do not add a dedicated `HardwareCapabilities` field. Use `properties2.limits` and `maintenance3`.
4. **Gate** — no feature toggle and no `#ifdef` on maintenance1/2. Image-view / render-pass / inheritance paths use 1.1+ core rules.
5. **Record** — no extra command-buffer work. Later optional maintenance (7–10) is enabled and recorded separately.

---

## 6. Integration sketch

N/A beyond API version selection already done:

```cpp
.applicationInfo = { .apiVersion = vk::ApiVersion14 }
// VMA: VK_API_VERSION_1_4
```

---

## 7. Validation / test checklist

1. Device extension list contains neither `VK_KHR_maintenance1` nor `VK_KHR_maintenance2`.
2. No code paths branch on “is maintenance1 available?” for 1.4 builds.
3. Image views / arrays work without extension-specific create flags from the old KHR names.

---

## 8. Shader interaction

**Indirect baseline (Vulkan 1.1 maintenance).** No dedicated shader stages. Enables a modern core baseline that later shader features assume. See [VK_KHR_shader_draw_parameters](./VK_KHR_shader_draw_parameters.md) for the main 1.1 shader-facing feature.

## 9. Dependencies and interactions

| Sibling | Interaction |
| --- | --- |
| `VK_KHR_maintenance3` (1.1) | Same generation; has **properties** — see dedicated doc |
| Maintenance4 (1.3 feature) | Cumulative; enabled via `Vulkan13Features::maintenance4` in this repo |
| Maintenance5/6 (1.4) | Cumulative; via `Vulkan14Features` |
| Maintenance7–10 | Still extensions in this project; not substitutes for 1.1 baseline |

---

## 10. Non-goals / do not do

- Do **not** re-enable maintenance1/2 as device extension strings on 1.1+.
- Do not document them as “recommended modern enable path” via extensions.
- Do not claim later maintenance **replaces** 1.1 maintenance — it is **cumulative**.
- Do not invent `Vulkan11Features` bits for maintenance1/2 that do not exist as app toggles.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Promotion | **Vulkan 1.1 core** (always available at 1.1+) |
| Enable path | API version only — **no feature flags** |
| Consumed? | **Always-on baseline** — little app action |
| Still modern? | Yes as **floor**; later maintenance is additive |
| Device extension strings? | **Never** on this 1.4 target |
| Replaced by later maintenance? | **No — cumulative** |
