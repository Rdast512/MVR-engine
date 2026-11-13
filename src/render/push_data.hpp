#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vulkan/vulkan.hpp>

// one DGC sequence: everything else is reached through the frame root
struct MeshPushData {
    vk::DeviceAddress frameRoot;
    // row of GpuFrameRoot::visibleMeshlets drawn by mesh workgroup 0
    uint32_t firstVisible;
    // groups in this batch
    uint32_t meshletCount;
};

// MeshPushData must match shaders/types.slang MeshPushData (16 bytes).
static_assert(std::is_trivially_copyable_v<MeshPushData>);
static_assert(offsetof(MeshPushData, frameRoot) == 0);
static_assert(offsetof(MeshPushData, firstVisible) == 8);
static_assert(offsetof(MeshPushData, meshletCount) == 12);
static_assert(sizeof(MeshPushData) == 16);

// written by buildMeshSequences (shaders/types.slang MeshDgcSequence); packed size is 28, the 32 B stride keeps
// the frame root BDA in pushData 8-byte aligned for every sequence
struct alignas(16) MeshDgcSequence
{
    MeshPushData pushData;
    vk::DrawMeshTasksIndirectCommandEXT draw;
};

static_assert(std::is_trivially_copyable_v<MeshDgcSequence>);
static_assert(offsetof(MeshDgcSequence, pushData) == 0);
static_assert(offsetof(MeshDgcSequence, draw) == sizeof(MeshPushData));
static_assert(sizeof(MeshDgcSequence) == 32);

// meshlet cull dispatches; must match shaders/types.slang CullPushData
struct CullPushData {
    vk::DeviceAddress frameRoot;
    // GpuCullView of the frame slot
    vk::DeviceAddress cullView;
};

static_assert(std::is_trivially_copyable_v<CullPushData>);
static_assert(sizeof(CullPushData) == 16);

// rt_instances pass; must match shaders/types.slang RtInstancesPushData
struct RtInstancesPushData {
    vk::DeviceAddress frameRoot;
    // uint64 BLAS address per ray instance, parallel to GpuFrameRoot::rayInstances
    vk::DeviceAddress blasAddresses;
    // VkAccelerationStructureInstanceKHR[count], the TLAS build input
    vk::DeviceAddress tlasInstances;
    uint32_t count;
    uint32_t pad0;
};

static_assert(std::is_trivially_copyable_v<RtInstancesPushData>);
static_assert(offsetof(RtInstancesPushData, count) == 24);
static_assert(sizeof(RtInstancesPushData) == 32);

// matches shaders/passes/hiz_build.slang
inline constexpr uint32_t kHiZLevelsPerDispatch = 3;

// resolve, debug view and present; must match shaders/types.slang FramePushData
struct FramePushData {
    vk::DeviceAddress frameRoot;
};

static_assert(sizeof(FramePushData) == 8);

// motion_check pass; must match shaders/types.slang MotionCheckPushData
struct MotionCheckPushData {
    vk::DeviceAddress frameRoot;
    // GpuMotionCheckStats
    vk::DeviceAddress stats;
};

static_assert(sizeof(MotionCheckPushData) == 16);

// rt_check pass; must match shaders/types.slang RayCheckPushData
struct RayCheckPushData {
    vk::DeviceAddress frameRoot;
    // GpuRayCheckStats
    vk::DeviceAddress stats;
};

static_assert(sizeof(RayCheckPushData) == 16);

// present pass; must match shaders/types.slang PresentPushData
struct PresentPushData {
    vk::DeviceAddress frameRoot;
    // swapchain extent
    vk::Extent2D displaySize;
};

static_assert(sizeof(PresentPushData) == 16);

// hi-z build dispatches; must match shaders/types.slang HiZPushData
struct HiZPushData {
    // resource heap: depth for mip 0, the pyramid's full-chain view otherwise
    uint32_t srcImage;
    // resource heap: storage views of the written mips, first levelCount valid
    std::array<uint32_t, kHiZLevelsPerDispatch> dstImages;
    // pyramid mip the first written level halves; unused from depth
    uint32_t srcMip;
    uint32_t isFromDepth;
    uint32_t levelCount;
    uint32_t pad0;
    vk::Extent2D srcSize;
    // first written level
    vk::Extent2D dstSize;
};

static_assert(std::is_trivially_copyable_v<HiZPushData>);
static_assert(offsetof(HiZPushData, srcSize) == 32);
static_assert(sizeof(HiZPushData) == 48);

// Hi-Z pyramid debug view; must match shaders/types.slang HiZViewPushData
struct HiZViewPushData {
    // resource heap, full mip chain
    uint32_t pyramid;
    uint32_t mip;
    float nearZ;
    float farZ;
    vk::Extent2D screenSize;
};

static_assert(std::is_trivially_copyable_v<HiZViewPushData>);
static_assert(sizeof(HiZViewPushData) == 24);

// largest push block of any pass; checked against maxPushDataSize at startup
inline constexpr size_t kMaxPushDataBytes =
    std::max({sizeof(MeshPushData), sizeof(CullPushData), sizeof(HiZPushData), sizeof(HiZViewPushData),
              sizeof(FramePushData), sizeof(PresentPushData), sizeof(RtInstancesPushData)});
