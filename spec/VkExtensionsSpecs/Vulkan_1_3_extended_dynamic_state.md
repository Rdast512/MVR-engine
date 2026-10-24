# Vulkan 1.3 Extended Dynamic State (EDS1 + EDS2 promotion) — Implementation Spec

**Status in this repo: PROMOTED TO CORE (Vulkan 1.3) from `VK_EXT_extended_dynamic_state` + `VK_EXT_extended_dynamic_state2`. Core `vkCmdSet*` for EDS1/EDS2 states are available without those device extensions. Repo still chains `VkPhysicalDeviceExtendedDynamicStateFeaturesEXT{ .extendedDynamicState = true }` (EDS1 feature struct) and lists `VK_EXT_extended_dynamic_state3` as a separate device extension (see [VK_EXT_extended_dynamic_state3.md](./VK_EXT_extended_dynamic_state3.md)). Do NOT re-enable EDS1/EDS2 as device extensions.**

Official sources:

- [VK_EXT_extended_dynamic_state](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_extended_dynamic_state.html) (promoted → 1.3)
- [VK_EXT_extended_dynamic_state2](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_extended_dynamic_state2.html) (promoted → 1.3)
- [VK_EXT_extended_dynamic_state3](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_extended_dynamic_state3.html) (**not** promoted; remains extension)
- Core dynamic state commands under [Command Buffer](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html) / pipeline dynamic state chapter
- [VkDynamicState](https://docs.vulkan.org/refpages/latest/refpages/source/VkDynamicState.html)

> Note: EDS1/EDS2 are **core Vulkan 1.3**. **Never** add `VK_EXT_extended_dynamic_state` or `VK_EXT_extended_dynamic_state2` to `requiredDeviceExtension`. EDS3 stays an extension and has its own doc.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| EDS1/EDS2 as device extensions | `requiredDeviceExtension` | **Absent by design** (comment under 1.3 promotions) |
| EDS1 feature struct | `src/core/vk_device.cpp` feature chain | `ExtendedDynamicStateFeaturesEXT{ .extendedDynamicState = true }` still chained |
| EDS2 feature struct | feature chain | **Not** present (`extendedDynamicState2*` bits) |
| EDS3 device extension | `vk_device.hpp` | **Listed** — see EDS3 doc |
| Pipeline dynamic states | `src/render/vk_pipeline.cpp` | `eViewport`, `eScissor` only |
| Record-time setters | `src/render/vk_renderer.cpp` | `setViewport`, `setScissor` (core 1.0 dynamic states) |
| Core EDS1/EDS2 setters | cull/front/topology/depth*/stencil* etc. | **Not used** yet |

### Already wired

```cpp
// src/core/vk_device.hpp comment — must NOT list EDS1/EDS2 extensions:
//   Promoted to 1.3: ... EXT_extended_dynamic_state, EXT_extended_dynamic_state2, ...

// src/core/vk_device.cpp
// vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT
{.extendedDynamicState = true},

// src/render/vk_pipeline.cpp
std::vector dynamicStates = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};

// src/render/vk_renderer.cpp
commandBuffers[currentFrame].setViewport(0, vk::Viewport(...));
commandBuffers[currentFrame].setScissor(0, vk::Rect2D(...));
```

### Not wired yet

- Pipeline `pDynamicState` expansion for core EDS1/EDS2 states actually needed  
- Recording core `setCullMode`, `setDepthTestEnable`, `setPrimitiveTopology`, etc.  
- Removing redundant EDS1 feature struct if fully relying on core 1.3 (optional cleanup)  
- EDS3 feature struct + selective bits (separate doc)

---

## 2. What was promoted

EDS1 and EDS2 are **core Vulkan 1.3**. This engine must not list `VK_EXT_extended_dynamic_state` or `_2`. The only dynamic states consumed today are classic viewport and scissor, set in `vk_renderer.cpp` inside the dynamic-rendering instance (`beginRendering` / MSAA resolve into the swapchain / `endRendering`). Rasterizer and depth state remain baked in `vk_pipeline.cpp`.

EDS3 is a **separate device extension** with limited use — listed, but its feature struct is not fully chained and no `vkCmdSet*EXT` is recorded. Promoting more state to dynamic should start with core EDS1/2 (`setCullMode`, depth test/write, topology, …) before touching EDS3.

### 2.1 From EDS1 → core 1.3 (representative)

| Dynamic state | Core command (no EXT suffix required) |
| --- | --- |
| Cull mode | `vkCmdSetCullMode` |
| Front face | `vkCmdSetFrontFace` |
| Primitive topology | `vkCmdSetPrimitiveTopology` |
| Viewport with count | `vkCmdSetViewportWithCount` |
| Scissor with count | `vkCmdSetScissorWithCount` |
| Depth test enable | `vkCmdSetDepthTestEnable` |
| Depth write enable | `vkCmdSetDepthWriteEnable` |
| Depth compare op | `vkCmdSetDepthCompareOp` |
| Depth bounds test enable | `vkCmdSetDepthBoundsTestEnable` |
| Stencil test enable | `vkCmdSetStencilTestEnable` |
| Stencil op | `vkCmdSetStencilOp` |

### 2.2 From EDS2 → core 1.3 (representative)

| Dynamic state | Core command |
| --- | --- |
| Depth bias enable | `vkCmdSetDepthBiasEnable` |
| Primitive restart enable | `vkCmdSetPrimitiveRestartEnable` |
| Rasterizer discard enable | `vkCmdSetRasterizerDiscardEnable` |
| Patch control points | `vkCmdSetPatchControlPoints` (with feature considerations) |
| Logic op | `vkCmdSetLogicOp` (logic op enable may still interact with EDS3 / features) |

Exact feature gating for a few EDS2 bits historically used `VkPhysicalDeviceExtendedDynamicState2FeaturesEXT`; under core 1.3 the corresponding dynamic states are available as core. Prefer core entry points and core `VkDynamicState` enumerants.

### 2.3 Still extension: EDS3

**Not promoted.** Remains `VK_EXT_extended_dynamic_state3` with many `vkCmdSet*EXT` commands (polygon mode, rasterization samples, color blend enables/equations, etc.). Documented in [VK_EXT_extended_dynamic_state3.md](./VK_EXT_extended_dynamic_state3.md).

| Category | Core 1.3 (EDS1/2) | Still EDS3 (extension) |
| --- | --- | --- |
| Viewport / scissor | Core (including with-count from EDS1) | — |
| Cull / front face / topology | Core | — |
| Depth test/write/compare/bounds enable | Core | Depth clamp enable, clip control extras → EDS3 |
| Stencil test enable / op | Core | — |
| Depth bias **enable** | Core | Full bias factors may already be core dynamic; advanced line/poly → EDS3 |
| Polygon mode | — | `setPolygonModeEXT` |
| Rasterization samples / sample mask | — | EDS3 |
| Color blend enable / equation / write mask | — | EDS3 |
| Alpha to coverage / logic op enable | partial / EDS3 | EDS3 for several |

---

## 3. Structures and commands

### 3.1 Do not re-enable EDS1/EDS2 extensions

```cpp
// WRONG on this 1.3+/1.4 target
requiredDeviceExtension.push_back(vk::EXTExtendedDynamicStateExtensionName);
requiredDeviceExtension.push_back(vk::EXTExtendedDynamicState2ExtensionName);
```

```cpp
// RIGHT — core commands after pipeline declares dynamic states
cmd.setCullMode(vk::CullModeFlagBits::eBack);
cmd.setFrontFace(vk::FrontFace::eCounterClockwise);
cmd.setDepthTestEnable(vk::True);
cmd.setDepthWriteEnable(vk::True);
cmd.setDepthCompareOp(vk::CompareOp::eLess);
```

### 3.2 Pipeline dynamic state array

```cpp
std::vector<vk::DynamicState> dynamicStates = {
    vk::DynamicState::eViewport,
    vk::DynamicState::eScissor,
    // Core EDS1 examples when needed:
    // vk::DynamicState::eCullMode,
    // vk::DynamicState::eFrontFace,
    // vk::DynamicState::ePrimitiveTopology,
    // vk::DynamicState::eDepthTestEnable,
    // vk::DynamicState::eDepthWriteEnable,
    // vk::DynamicState::eDepthCompareOp,
    // vk::DynamicState::eDepthBoundsTestEnable,
    // vk::DynamicState::eStencilTestEnable,
    // vk::DynamicState::eStencilOp,
    // Core EDS2 examples when needed:
    // vk::DynamicState::eDepthBiasEnable,
    // vk::DynamicState::ePrimitiveRestartEnable,
    // vk::DynamicState::eRasterizerDiscardEnable,
};
```

Every state listed **must** be set before draws that depend on it (or inherit if the spec allows for that state — prefer explicit set).

### 3.3 Redundant EDS1 feature struct in this repo

The chain still includes:

```cpp
vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT{.extendedDynamicState = true}
```

On a pure core 1.3 path this is largely **redundant** for states already core. It is **not** a substitute for listing EDS1 as an extension (and the extension is correctly **not** listed). Optional cleanup:

- Leave as-is for compatibility with any remaining EDS1-named paths, **or**
- Remove the struct from the chain if validation and drivers are happy with core-only dynamic state usage

Do **not** interpret this struct as permission to re-add the EDS1 extension name.

### 3.4 Relationship to shader objects

`VK_EXT_shader_object` (enabled in this repo) typically expects a large set of dynamic states (core + often EDS3). When consuming shader objects for graphics, expand dynamic state coverage deliberately; many remaining gaps are EDS3, not EDS1/2.

---

## Limits

No extra numeric Required Limits for promoted EDS1/EDS2. Making a state dynamic does **not** raise vertex-input or viewport counts. Do not invent numeric minima.

| Topic | Constraint |
| --- | --- |
| Viewport / scissor counts | Still `maxViewports` (and related core scissor limits) |
| Vertex input | Still `maxVertexInputBindings`, `maxVertexInputAttributes`, etc. |
| Core dynamic states | Cull mode, front face, primitive topology, viewport/scissor with count, depth test/write/compare, stencil op, depth bias enable, primitive restart, rasterizer discard, … |

Query core `VkPhysicalDeviceLimits` as usual. Official: [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html).

---

## 4. Where to implement in this codebase

### 4.1 Device bootstrap

| File | Action |
| --- | --- |
| `src/core/vk_device.hpp` | Keep EDS1/EDS2 **out** of extension list; keep EDS3 listed only if still required |
| `src/core/vk_device.cpp` | Prefer core usage; optional remove EDS1 feature struct; **do not** add EDS2 extension |

### 4.2 Pipeline — expand only when needed

**File:** `src/render/vk_pipeline.cpp`

Today raster/depth state is **baked** into the pipeline (`cullMode = eBack`, depth test on, etc.). That is valid.

Make a state dynamic only if:

- Multiple materials need different cull/depth without pipeline churn, or  
- Shader objects / GPL require it, or  
- Debug toggles (wireframe needs EDS3 polygon mode, not EDS1)

### 4.3 Renderer record path

**File:** `src/render/vk_renderer.cpp`

After `bindPipeline`, set every dynamic state declared by the pipeline. Pattern:

```cpp
cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline.graphicsPipeline);
cmd.setViewport(0, viewport);
cmd.setScissor(0, scissor);
// if declared dynamic:
// cmd.setCullMode(vk::CullModeFlagBits::eBack);
// cmd.setDepthTestEnable(vk::True);
// ...
```

### 4.4 EDS3 remains separate

Do not fold EDS3 enablement into this core doc’s “always on” story. Feature bits and `set*EXT` live under [VK_EXT_extended_dynamic_state3.md](./VK_EXT_extended_dynamic_state3.md).

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep EDS1/EDS2 **off** the extension list | `vk_device.hpp` | Spec-correct for 1.3+ |
| P0 | Keep using core viewport/scissor | `vk_pipeline.cpp`, `vk_renderer.cpp` | Already correct |
| P1 | Document core vs EDS3 split for contributors | this file + EDS3 doc | Avoid re-adding EDS1/2 |
| P2 | Promote high-churn states (cull/depth) to core dynamic if pipeline count hurts | pipeline + renderer | Optional optimization |
| P2 | Optional remove redundant EDS1 feature struct | `vk_device.cpp` | Chain hygiene |
| P3 | Shader-object dynamic state matrix | shader object path | When that path is consumed |

### Implementation recipe

1. **Do not re-add EDS1/EDS2 extension strings.** Core 1.3 commands are enough. The chained `ExtendedDynamicStateFeaturesEXT{ .extendedDynamicState = true }` is optional/redundant — do not treat it as permission to list the extension.
2. **Keep the current consumed path.** Dynamic rendering + Synchronization2 already wrap draws. Viewport/scissor are set after `bindPipeline` inside `beginRendering` (MSAA color resolves into the swapchain). That is the correct baseline.
3. **Expand core dynamic states only when pipeline churn hurts.** Add `eCullMode` / depth enables / topology to `pDynamicState` and call the core `set*` commands in `vk_renderer.cpp`. Do not mark every state dynamic.
4. **Leave EDS3 as the extension it is.** Limited use today. Feature bits are not fully chained. Wireframe (`setPolygonModeEXT`) and dynamic blend live in [VK_EXT_extended_dynamic_state3.md](./VK_EXT_extended_dynamic_state3.md).
5. **Unrelated enablement.** `dynamicRenderingLocalRead` is on but unused. FSR is enabled, properties in `HardwareCapabilities::fragmentShadingRate`, unused. Advanced blend properties cached; `advancedBlendCoherentOperations` **explicitly false**. ASTC HDR **false** (GPU-gated). None of these require EDS1/2 work.

---

## 6. Concrete integration sketch (optional cull dynamic)

```cpp
// vk_pipeline.cpp
std::vector dynamicStates = {
    vk::DynamicState::eViewport,
    vk::DynamicState::eScissor,
    vk::DynamicState::eCullMode,
};

// still provide a default in rasterizer create info for non-dynamic fields;
// cullMode is ignored at draw if dynamic — set at record time.

// vk_renderer.cpp after bindPipeline
cmd.setCullMode(vk::CullModeFlagBits::eBack); // or eNone for double-sided
cmd.setViewport(0, viewport);
cmd.setScissor(0, scissor);
```

No device extension changes required.

---

## 7. Validation / test checklist

1. Device create succeeds **without** `VK_EXT_extended_dynamic_state` / `_2` in the extension list.  
2. Declaring `eCullMode` (etc.) as dynamic and calling core `setCullMode` validates cleanly.  
3. Missing set for a declared dynamic state produces validation errors — fix by setting before draw.  
4. EDS3 commands still require EDS3 extension + feature bits (do not assume core).  
5. Shader object path (when used): verify required dynamic states against the shader object spec.  
6. Removing EDS1 feature struct (if done): re-validate device create and draws.

---

## 8. Shader interaction

**Indirect.** Core 1.3 dynamic state lets host change cull mode, depth test, topology, etc. without pipeline variants. Shader modules stay the same; mesh stages ignore classic input-assembly topology.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Vulkan 1.3 core | Promotion home for EDS1/EDS2 |
| `VK_EXT_extended_dynamic_state3` | Additional dynamic states; separate enablement |
| `VK_EXT_shader_object` | Often needs core dynamic states + EDS3 |
| Dynamic rendering | Orthogonal; both used together on the main path |
| Maintenance10 NULL sample mask | Only when using EDS3/sample mask setters |
| Graphics pipeline library | Dynamic state interactions if GPL enabled later |

---

## 10. Non-goals / do not do

- Do **not** re-add `VK_EXT_extended_dynamic_state` or `VK_EXT_extended_dynamic_state2` to `requiredDeviceExtension`.  
- Do **not** recommend enabling EDS1/EDS2 extensions “for completeness” on 1.3+.  
- Do **not** confuse EDS3 `set*EXT` with core EDS1/EDS2 commands.  
- Do **not** mark every pipeline state dynamic without a need (CPU cost + validation surface).  
- Do **not** treat the chained EDS1 feature struct as a reason to keep the EDS1 extension name.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| EDS1/EDS2 status | **Promoted to core Vulkan 1.3** |
| Device extensions listed? | **No** for EDS1/EDS2 (correct); **EDS3 yes** (separate) |
| Consumed core EDS1/EDS2 setters? | **No** beyond classic viewport/scissor |
| Redundant chain entry? | EDS1 `extendedDynamicState = true` still present |
| Still valid? | **Yes** — use core `vkCmdSet*` for EDS1/EDS2 states |
| Highest-value next steps | Keep core-only; expand dynamic states only when needed; optional chain cleanup; use EDS3 doc for non-promoted states |
