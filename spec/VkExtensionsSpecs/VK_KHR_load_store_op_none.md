# VK_KHR_load_store_op_none — Implementation Spec

**Status in this repo: PROMOTED TO CORE (Vulkan 1.3). Always available on the project’s 1.3+/1.4 device — no feature bit and not listed as a device extension. Engine currently uses `AttachmentLoadOp::eClear` / `AttachmentStoreOp::eStore` / `eDontCare` in dynamic rendering; `eNone` is available and preferred when an attachment is neither loaded nor stored.**

Official sources:

- [VK_KHR_load_store_op_none](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_load_store_op_none.html)
- [VkAttachmentLoadOp](https://docs.vulkan.org/refpages/latest/refpages/source/VkAttachmentLoadOp.html) (`VK_ATTACHMENT_LOAD_OP_NONE`)
- [VkAttachmentStoreOp](https://docs.vulkan.org/refpages/latest/refpages/source/VkAttachmentStoreOp.html) (`VK_ATTACHMENT_STORE_OP_NONE`)

> Note: Promoted to **Vulkan 1.3 core**. **Never** add `VK_KHR_load_store_op_none` to `requiredDeviceExtension`. Enumerants are core (`VK_ATTACHMENT_LOAD_OP_NONE`, `VK_ATTACHMENT_STORE_OP_NONE` / Vulkan-Hpp `eNone`).

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` | **Absent by design** (comment lists under 1.3 promotions) |
| Feature bit | `Vulkan13Features` | **None** (ops are always available in 1.3+) |
| Frame attachments | `src/render/vk_renderer.cpp` | Color: `loadOp=eClear`, `storeOp=eStore`; Depth: `loadOp=eClear`, `storeOp=eDontCare` |
| `eNone` usage | engine | **Not used yet** |

### Current dynamic rendering ops

```cpp
// src/render/vk_renderer.cpp
vk::RenderingAttachmentInfo colorAttachmentInfo = {
    // ...
    .loadOp = vk::AttachmentLoadOp::eClear,
    .storeOp = vk::AttachmentStoreOp::eStore,
    .clearValue = clearColor};

vk::RenderingAttachmentInfo depthAttachmentInfo = {
    // ...
    .loadOp = vk::AttachmentLoadOp::eClear,
    .storeOp = vk::AttachmentStoreOp::eDontCare,
    .clearValue = clearDepth};
```

---

## 2. What the extension provides

Adds **NONE** load and store operations meaning: the attachment contents are **not accessed** for load or store by the render pass instance.

`DONT_CARE` means “implementation may store or discard”; it is a weak statement when the pass truly does not access an attachment. `LOAD_OP_NONE` / `STORE_OP_NONE` mean **no access**. The historical extension is **promoted to Vulkan 1.3 core** and is always available on this 1.4 device — **no feature bit**, **no device-extension string**.

This does **not** add structs, commands, or limits. It does **not** replace barriers or layouts. It does **not** make NONE safe on attachments that will be loaded or sampled later. Current `vk_renderer.cpp` still uses Clear/Store/DontCare; `eNone` is available but unused.


| Op | Meaning | When correct |
| --- | --- | --- |
| `LOAD_OP_NONE` | Do not load; contents **undefined** unless written | Attachment will be fully written; previous contents must not matter; no clear |
| `STORE_OP_NONE` | Do not store; contents after the pass are **undefined** for that attachment | Attachment not sampled/presented later from that image/view |
| `LOAD_OP_CLEAR` | Clear to `clearValue` | First use of a transient target each frame (current color/depth) |
| `LOAD_OP_LOAD` | Preserve previous contents | Accumulation, UI overlay on existing color, etc. |
| `STORE_OP_STORE` | Write out attachment | Later reads, present, or next pass loads |
| `STORE_OP_DONT_CARE` | Driver may discard; contents undefined after | OK when not read later; slightly weaker statement than NONE on some impls historically |

**Still valid.** Use `NONE` to express “no access” more precisely than `DONT_CARE` where the attachment is intentionally untouched for load or store.

Difference vs `DONT_CARE` (practical guidance):

- **`DONT_CARE`**: “I do not care what happens to the data” — implementation may store or discard.  
- **`NONE`**: “There is no store/load access” — use when the attachment is not needed as input/output of the pass for that op (tile memory / bandwidth friendly intent).

For MSAA images that exist only to feed a resolve, **store NONE/DONT_CARE on the MSAA surface** is typically correct if nothing reads the MSAA contents after the pass.

---

## 3. Structures and commands

### 3.1 Enumerants (core 1.3+)

```cpp
// Load
VK_ATTACHMENT_LOAD_OP_NONE  // vk::AttachmentLoadOp::eNone

// Store
VK_ATTACHMENT_STORE_OP_NONE // vk::AttachmentStoreOp::eNone
```

Used inside:

- `VkRenderingAttachmentInfo::{loadOp,storeOp}` (dynamic rendering — **this engine**)
- Classic `VkAttachmentDescription` / `VkAttachmentDescription2` (not used on main path)

### 3.2 No feature struct

No `VkPhysicalDevice*Features` bit. Availability is core 1.3+.

### 3.3 Valid usage highlights

- If `loadOp == NONE`, `clearValue` is ignored (do not rely on clear).  
- If the attachment will be read as input / sampled later, do **not** use `storeOp == NONE` (or `DONT_CARE`) on the view you still need.  
- Resolve targets: store ops apply per attachment; MSAA source vs resolve destination are separate `RenderingAttachmentInfo` fields — set ops on the attachment being written, not only the resolve view.  
- Depth currently uses `DONT_CARE` store after a full clear+write — `NONE` is also appropriate if depth is not consumed after the pass.

---

## Limits

There is **no property struct** and **no limits**. `LOAD_OP_NONE` / `STORE_OP_NONE` are always available at Vulkan **1.3+**.

| Limit / property | Spec-required (when feature supported) | Constraint | Engine implication |
| --- | --- | --- | --- |
| `VK_ATTACHMENT_LOAD_OP_NONE` / `VK_ATTACHMENT_STORE_OP_NONE` | always available at **1.3+** | no feature bit | Use `vk::AttachmentLoadOp::eNone` / `AttachmentStoreOp::eNone` in `RenderingAttachmentInfo` |
| Numeric / property limits | **none** | — | Do not add a `HardwareCapabilities` field |
| Related core (color/depth size) | `maxImageDimension2D` min **4096** (core) / **8192** (1.4/roadmap) | query `properties2.limits` | Attachment extent still bounded by core image limits |

Sources: [limits](https://docs.vulkan.org/spec/latest/chapters/limits.html), [Required Limits](https://docs.vulkan.org/refpages/latest/refpages/source/Required_Limits.html).

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap — DONE (implicit)

No extension string, no feature bit.

### 4.2 Primary consumer — dynamic rendering attachments

**File:** `src/render/vk_renderer.cpp` → `recordCommandBuffer`

| Attachment | Today | Recommended when |
| --- | --- | --- |
| MSAA color | Clear + **Store** | Keep **Clear**. Change store to **`eDontCare` or `eNone`** if only the **resolve** (swapchain) is consumed and MSAA color is never read |
| Resolve (swapchain) | Implicit via resolve fields | Not a separate attachment info storeOp for the resolve image in the same way — the resolve writes the swapchain image as side effect of resolveMode |
| Depth | Clear + DontCare | Keep Clear; **`eNone` or keep `eDontCare`** if depth is never sampled later |

Illustrative tightening for MSAA color when the engine never samples the MSAA buffer after the pass:

```cpp
vk::RenderingAttachmentInfo colorAttachmentInfo = {
    .imageView = resourceManager.colorImageView,
    .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
    .resolveMode = vk::ResolveModeFlagBits::eAverage,
    .resolveImageView = swapChain.swapChainImageViews[imageIndex],
    .resolveImageLayout = vk::ImageLayout::eColorAttachmentOptimal,
    .loadOp = vk::AttachmentLoadOp::eClear,
    .storeOp = vk::AttachmentStoreOp::eNone, // or eDontCare — MSAA contents unused after resolve
    .clearValue = clearColor};

vk::RenderingAttachmentInfo depthAttachmentInfo = {
    .imageView = resourceManager.depthImageView,
    .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
    .loadOp = vk::AttachmentLoadOp::eClear,
    .storeOp = vk::AttachmentStoreOp::eNone,
    .clearValue = clearDepth};
```

### 4.3 When NOT to use NONE

- **Load NONE** while expecting previous frame contents (e.g. accumulation buffer).  
- **Store NONE** on an attachment that will be `LOAD`ed next or sampled as a texture.  
- **Load NONE** when you still pass a clear and expect a clear — use `CLEAR` instead.  
- Swapchain image as a **color attachment without resolve** that will be presented: needs a real store (or resolve write) producing defined contents for present.

### 4.4 Future passes

| Pass type | Load | Store |
| --- | --- | --- |
| Full-screen clear + draw, no later read | Clear | None / DontCare |
| Load history buffer | Load | Store |
| Depth prepass, depth used later | Clear/Load | **Store** |
| Depth prepass, unused after | Clear | None |
| Sideband attachment not touched this pass | **None** | **None** |

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep ops available as core | — | Already true |
| P1 | MSAA color `storeOp` → `eNone` or `eDontCare` | `vk_renderer.cpp` | Avoid useless MSAA stores when only resolve matters |
| P1 | Depth `storeOp` → `eNone` (optional) | `vk_renderer.cpp` | Same if depth never sampled |
| P2 | Audit new passes for NONE vs STORE | future render graph | Bandwidth / correctness |
| P3 | Document per-pass attachment matrix | render docs | Multi-pass readiness |

### Implementation recipe

1. **Enable** — nothing. Core 1.3+ / API 1.4. Never list `VK_KHR_load_store_op_none` in `requiredDeviceExtension` (`vk_device.hpp` 1.3 promoted block). No `Vulkan13Features` bit.
2. **Query** — no properties. Availability is implied by the API version already selected in `vk_device.cpp`.
3. **Cache** — no `HardwareCapabilities` field.
4. **Gate** — choose ops from the pass’s real access: `eNone` only when the attachment is not loaded/stored; keep `eClear` when a clear is required; keep `eStore` if a later pass samples or presents that view.
5. **Record** — in `vk_renderer.cpp` `recordCommandBuffer`, consider MSAA color `storeOp → eNone` (resolve writes the swapchain) and depth `storeOp → eNone` while depth is never sampled. Flip back to `eStore` if SSR / depth effects land.

---

## 6. Concrete integration sketch

```cpp
// Forward MSAA path: clear targets; do not preserve MSAA/depth after resolve
colorAttachmentInfo.loadOp = vk::AttachmentLoadOp::eClear;
colorAttachmentInfo.storeOp = vk::AttachmentStoreOp::eNone;

depthAttachmentInfo.loadOp = vk::AttachmentLoadOp::eClear;
depthAttachmentInfo.storeOp = vk::AttachmentStoreOp::eNone;
```

If a later feature samples the depth or MSAA color (SSR, depth-aware effects), flip the corresponding store back to `eStore` and ensure layout/usage allow sampling.

---

## 7. Validation / test checklist

1. No extension string for load_store_op_none.  
2. Validation accepts `eNone` on dynamic rendering attachments.  
3. Visual: full-screen clear color still correct after store op change (resolve must still run).  
4. No later pass loads an attachment that used store NONE.  
5. If depth sampling is added: store becomes STORE; image usage includes sampled/input as needed.  
6. ImGui still draws correctly inside the same rendering instance.

---

## 8. Shader interaction

**Indirect.** Attachment load/store ops can be NONE; fragment shaders must not assume previous contents if load op is none/dont-care. Keep dynamic rendering attachment ops consistent with FS expectations.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Dynamic rendering | Ops set on `RenderingAttachmentInfo` |
| MSAA resolve | Store ops on MSAA attachment vs resolve destination — tune MSAA store independently |
| Maintenance10 resolve TF | Orthogonal to load/store ops |
| Synchronization2 | Barriers still required for layouts; NONE does not replace barriers |
| Render graph / multi-pass | Store/load pairing across passes is the main correctness concern |

---

## 10. Non-goals / do not do

- Do **not** list `VK_KHR_load_store_op_none` as a device extension.  
- Do **not** use `LOAD_OP_NONE` when a clear is required.  
- Do **not** use `STORE_OP_NONE` on the only copy of data needed later.  
- Do **not** assume NONE and DONT_CARE are identical on every implementation — prefer NONE when “no access” is the real intent.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| How available? | **Core 1.3+** (always on for this project) |
| Device extension / feature bit? | **Neither** |
| Consumed as `eNone`? | **Not yet** — Clear/Store/DontCare today |
| Still valid? | **Yes** |
| Highest-value next steps | MSAA color (and depth) store → `eNone`/`eDontCare` when unused after the pass |
