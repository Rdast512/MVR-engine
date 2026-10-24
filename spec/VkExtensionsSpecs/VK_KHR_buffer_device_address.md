# VK_KHR_buffer_device_address — Implementation Spec

**Status in this repo: PROMOTED TO CORE (Vulkan 1.2), ENABLED via `Vulkan12Features::bufferDeviceAddress = true`, CONSUMED.**

Official sources:

- [VK_KHR_buffer_device_address](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_buffer_device_address.html) (historical extension name)
- [VkPhysicalDeviceVulkan12Features::bufferDeviceAddress](https://docs.vulkan.org/spec/latest/chapters/features.html)
- [vkGetBufferDeviceAddress](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetBufferDeviceAddress.html)
- [VkBufferDeviceAddressInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkBufferDeviceAddressInfo.html)

> **PROMOTED TO CORE — do not list as a device extension.** With API version 1.2+, enable only through `VkPhysicalDeviceVulkan12Features`. This project targets Vulkan 1.4; the string `VK_KHR_buffer_device_address` must **not** appear in `requiredDeviceExtension`.

---

## 1. Current enablement audit

| Layer | Location | State |
| --- | --- | --- |
| Device extension list | `src/core/vk_device.hpp` | **Must not list** — promoted; called out in the 1.2 promoted block comment |
| Feature enable | `src/core/vk_device.cpp` → `Vulkan12Features` | `bufferDeviceAddress = true` |
| Capture / multi-device | same feature struct | **Not enabled** (`bufferDeviceAddressCaptureReplay`, `bufferDeviceAddressMultiDevice` left false) |
| VMA | `src/core/vk_allocator.hpp` | `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` |
| Buffer usage | resource / descriptor paths | `eShaderDeviceAddress` on camera UBO, object UBO, descriptor heaps |
| API usage | `vk_resource_manager.cpp`, `vk_descriptors.cpp` | `getBufferAddress` / `vkGetBufferDeviceAddress` |

### Already wired

```cpp
// src/core/vk_device.cpp — Vulkan12Features
.bufferDeviceAddress = true,

// src/core/vk_allocator.hpp
allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT | /* ... */;
allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_4;

// src/core/vk_resource_manager.cpp (camera + object UBOs)
createBuffer(..., eUniformBuffer | eStorageBuffer | eShaderDeviceAddress, ...);
camera.cameraBufferAddresses[i] = device.getBufferAddress({.buffer = *camera.cameraBuffers[i]});
obj.uboAddresses[i] = device.getBufferAddress({.buffer = *obj.uniformBuffers[i]});

// src/core/vk_descriptors.cpp (descriptor heaps)
// eShaderDeviceAddress | eDescriptorHeapEXT, then:
VkBufferDeviceAddressInfo info = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = *heapBuffer };
VkDeviceAddress addr = vkGetBufferDeviceAddress(*device, &info);
```

### Related features (do not enable unless needed)

| Feature bit | Enable? | Notes |
| --- | --- | --- |
| `bufferDeviceAddress` | **Yes** | Main switch — **required** for this engine |
| `bufferDeviceAddressCaptureReplay` | No | Capture/replay tools only |
| `bufferDeviceAddressMultiDevice` | No | Multi-device / device-group only |

---

## 2. What core provides

Stable 64-bit GPU virtual addresses for buffers. Shaders and device-side structures reference memory by address rather than only by descriptor binding.

| Capability | API / bit | Engine relevance |
| --- | --- | --- |
| Buffer device address | `vkGetBufferDeviceAddress` | Camera/object UBO addresses; heap bind ranges |
| Buffer usage | `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` | Required on every addressed buffer |
| Memory allocation | memory must support device address (VMA BDA flag) | Allocator create bit |
| SPIR-V | `PhysicalStorageBuffer` / buffer-reference style access | Untyped pointers, RT, bindless stacks |

**Still the modern foundation — not replaced.** Descriptor heaps, acceleration structures, untyped pointer stacks, and device-address command paths all build on BDA.

A `VkDeviceAddress` is a stable 64-bit GPU virtual address for a buffer’s bound memory. Shaders in `shaders/base/shader.slang` and `shaders/base/mesh.slang` `reinterpret<T*>` camera, object, and meshlet tables from addresses pushed via `pushDataEXT`. Heap bind ranges are also BDAs (`vk_descriptors.cpp`).

There is no dedicated BDA property struct. Alignment is the buffer’s memory-offset alignment. Capture/replay address bits stay off unless a tool needs them.

---

## 3. Structures and commands

### 3.1 Feature (enable at device create via Vulkan 1.2)

```cpp
// Part of VkPhysicalDeviceVulkan12Features (core 1.2+)
VkBool32 bufferDeviceAddress;
VkBool32 bufferDeviceAddressCaptureReplay; // leave false unless capture tools
VkBool32 bufferDeviceAddressMultiDevice;   // leave false unless device groups
```

**Already enabled** (`bufferDeviceAddress` only). Keep enabling via `Vulkan12Features` only — never re-add the extension name for 1.4 targets.

### 3.2 Query address

```cpp
typedef struct VkBufferDeviceAddressInfo {
    VkStructureType    sType; // VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO
    const void*        pNext;
    VkBuffer           buffer;
} VkBufferDeviceAddressInfo;

VkDeviceAddress vkGetBufferDeviceAddress(
    VkDevice                                    device,
    const VkBufferDeviceAddressInfo*            pInfo);
```

Vulkan-Hpp (already used):

```cpp
device.getBufferAddress(vk::BufferDeviceAddressInfo{ .buffer = *buffer });
```

Valid usage highlights:

- Buffer must have been created with `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`
- Bound memory must have been allocated with device-address support
- Feature `bufferDeviceAddress` must be enabled
- Returned address is stable for the buffer’s lifetime (until destroy)

---

## Limits

No dedicated BDA property struct and nothing extra in `HardwareCapabilities`. Addresses are **64-bit**.

| Topic | Rule |
| --- | --- |
| Alignment | The buffer device address follows the buffer’s **memory offset alignment** |
| Usage | Buffer must be created with `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` |
| Memory | Allocation must support device addresses (VMA `BUFFER_DEVICE_ADDRESS` create flag) |
| Capture / replay | Extra create bits only if `bufferDeviceAddressCaptureReplay` is on (it is **not**, typically) |

Do not invent extra numeric limits. See [Required Limits](https://docs.vulkan.org/spec/latest/chapters/limits.html) for related core buffer/memory alignments.

---

## 4. Where this is used in the codebase

### 4.1 Device bootstrap — DONE

| File | Action |
| --- | --- |
| `src/core/vk_device.cpp` | Keep `Vulkan12Features::bufferDeviceAddress = true` |
| `src/core/vk_device.hpp` | Keep **out** of `requiredDeviceExtension` (promoted comment already documents this) |
| `src/core/vk_allocator.hpp` | Keep `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` |

### 4.2 Resource manager — CONSUMED

**File:** `src/core/vk_resource_manager.cpp`

- Camera UBOs and per-object UBOs: usage includes `eShaderDeviceAddress`
- Addresses stored in `cameraBufferAddresses[]` / `uboAddresses[]` for push / heap / shader consumption

### 4.3 Descriptor heaps — CONSUMED

**File:** `src/core/vk_descriptors.cpp`

- Resource and sampler heap buffers use `eShaderDeviceAddress`
- `vkGetBufferDeviceAddress` fills `BindHeapInfoEXT::heapRange.address`

### 4.4 Downstream stacks that require BDA

| Stack | Why BDA is required |
| --- | --- |
| `VK_EXT_descriptor_heap` | Heap buffer GPU addresses |
| `VK_KHR_acceleration_structure` | Device addresses for AS build inputs / handles |
| `VK_KHR_shader_untyped_pointers` | Physical storage / untyped address model |
| `VK_KHR_device_address_commands` | Address-based command consumption |

---

## 5. Recommended implementation order

| Priority | Change | File(s) | Why |
| --- | --- | --- | --- |
| P0 | Keep feature + VMA BDA as-is | `vk_device.cpp`, `vk_allocator.hpp` | Already correct and consumed |
| P0 | Never re-list as device extension | `vk_device.hpp` | Spec: promoted into 1.2 core |
| P1 | Ensure every new addressed buffer sets `eShaderDeviceAddress` | resource / RT / heap helpers | Missing usage bit → validation failure |
| P2 | Optional: log addresses at create (debug builds) | resource / descriptor managers | Debuggability for bindless/RT |

### Implementation recipe

1. Keep `Vulkan12Features::bufferDeviceAddress = true` and VMA `BUFFER_DEVICE_ADDRESS`. Do **not** list `VK_KHR_buffer_device_address` as a device extension.
2. Every addressed buffer needs `eShaderDeviceAddress`: camera/object UBOs (`vk_resource_manager.cpp`), heap buffers (`vk_descriptors.cpp`), meshlet/vertex tables.
3. Query once after bind via `Device::getBufferAddress` (or the existing C `vkGetBufferDeviceAddress` in heap setup). Store in `cameraBufferAddresses[]` / `uboAddresses[]` / heap `BindHeapInfoEXT.heapRange.address`.
4. Push those 64-bit values through `pushDataEXT`. Shaders (`shaders/base/shader.slang`, `shaders/base/mesh.slang`) `reinterpret<T*>` the addresses. Heap descriptors stay heap handles, not raw BDA loads of descriptor bits.
5. Leave `bufferDeviceAddressCaptureReplay` / `multiDevice` false. Alignment is the buffer’s memory offset alignment — no extra BDA property cache.

---

## 6. Integration sketch (addressed buffer)

```cpp
// Create
createBuffer(
    size,
    vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eShaderDeviceAddress,
    /* memory props */,
    buffer, allocation, /* ... */);

// Query once after create/bind
const vk::DeviceAddress addr =
    device.getBufferAddress(vk::BufferDeviceAddressInfo{ .buffer = *buffer });

// Push to shaders / heap bind ranges / AS geometry as needed
```

Do not invent a second address-query path; prefer `Device::getBufferAddress` (Hpp) consistently, or document why C API is used (as in heap code today).

---

## 7. Validation / test checklist

1. Device creates with Vulkan 1.4 + `bufferDeviceAddress = true` and **no** `VK_KHR_buffer_device_address` in the extension list.
2. VMA allocator created with `BUFFER_DEVICE_ADDRESS` bit.
3. Camera/object UBO and heap buffers: usage includes `SHADER_DEVICE_ADDRESS`; `getBufferAddress` returns non-zero.
4. No validation errors on `vkGetBufferDeviceAddress`.
5. Heap bind + push path still works with stored addresses.
6. Future AS builds use addresses from BDA-enabled buffers only.

---

## 8. Shader interaction

**Direct — shaders load/store through 64-bit GPU addresses.** Host obtains `VkDeviceAddress` via `vkGetBufferDeviceAddress`; shaders treat those values as pointers into `PhysicalStorageBuffer` memory (`SPV_KHR_physical_storage_buffer`).

### SPIR-V / language model

| Concept | Detail |
| --- | --- |
| Capability | `PhysicalStorageBufferAddresses` (and related) |
| Storage class | `PhysicalStorageBuffer` |
| Alignment | Loads/stores must honor declared alignment (scalar block layout common for tight structs) |
| Null | Address `0` is reserved as null |

Slang patterns used in this engine:

```slang
// Address in push constants (classic path — shaders/base/shader.slang)
struct PushData {
    uint64_t cameraAddress;
    uint64_t objectUBddress;
    DescriptorHandle<Texture2D> texture;
    DescriptorHandle<SamplerState> samplerHandle;
};
[[vk::push_constant]] ConstantBuffer<PushData> push;

ObjectUB* object = reinterpret<ObjectUB*>(push.objectUBddress);
CameraData* camera = reinterpret<CameraData*>(push.cameraAddress);
float4x4 model = object->modelMatrix;

// Mesh path also BDAs geometry tables (MeshPushData.vertices / meshlets / …)
Vertex* vertexTable = reinterpret<Vertex*>(push.vertices);
```

Equivalent GLSL uses `layout(buffer_reference)` / `GL_EXT_buffer_reference2`. Slang may also emit `vk::BufferPointer<T>` (`.Get()` dereference) depending on dialect.

### What the host must guarantee for shaders

1. Buffer usage includes `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`.
2. Memory allocation supports device addresses (VMA `BUFFER_DEVICE_ADDRESS` create flag).
3. Feature `bufferDeviceAddress` enabled (this project: `Vulkan12Features`).
4. Address pushed or stored is the base of the buffer (or base + intentional byte offset); lifetime ≥ last use in flight.
5. CPU layout of `CameraData` / `ObjectUB` / `Vertex` / `MeshletDesc` **matches** shader struct packing (scalar / std430 agreement).

### Engine consumers

| Data | Host | Shader |
| --- | --- | --- |
| Camera UBO (× frames in flight) | `cameraBufferAddresses[i]` | `push.cameraAddress` |
| Object UBO (× FIF) | `uboAddresses[i]` | `push.objectUB*` |
| Descriptor heap buffers | heap bind range address | Heaps (not raw BDA load of descriptors) |
| Meshlet / vertex tables (static) | single BDA each | `MeshPushData` geometry fields |

**Frames-in-flight:** only buffers the CPU rewrites each frame need multiple addresses. Static geometry BDAs are single values shared across frames.

Cross-refs: [VK_EXT_descriptor_heap](./VK_EXT_descriptor_heap.md), [VK_KHR_shader_untyped_pointers](./VK_KHR_shader_untyped_pointers.md), [VK_EXT_mesh_shader](./VK_EXT_mesh_shader.md), RT / AS docs.

## 9. Dependencies and interactions

| Dependency / sibling | Interaction |
| --- | --- |
| Vulkan 1.2+ core | Promotion home; features via `Vulkan12Features` |
| VMA | `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` required when any buffer uses BDA |
| `VK_EXT_descriptor_heap` | Heap ranges are device addresses |
| Acceleration structure / RT | Build inputs and scratch often addressed by BDA |
| `VK_KHR_shader_untyped_pointers` | Address-based untyped loads/stores |
| `bufferDeviceAddressCaptureReplay` | Only if GPU capture tools need stable replay addresses |

---

## 10. Non-goals / do not do

- Do **not** add `VK_KHR_buffer_device_address` to `requiredDeviceExtension` when targeting 1.2+.
- Do **not** enable `captureReplay` or `multiDevice` without a concrete tool/multi-GPU need.
- Do not call `getBufferAddress` on buffers without `eShaderDeviceAddress` usage.
- Do not treat descriptor heaps or untyped pointers as replacements for BDA — they **depend** on it.

---

## 11. Summary

| Question | Answer |
| --- | --- |
| Promotion | **Vulkan 1.2 core** |
| Enable path | `Vulkan12Features::bufferDeviceAddress = true` only |
| Consumed? | **Yes** — UBO addresses, descriptor heaps, foundation for RT / untyped / address commands |
| Still modern? | **Yes** — not replaced; still the address foundation |
| Device extension string? | **Never** on this 1.4 target |
