#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vulkan/vulkan.hpp>

// one DGC sequence: everything else is reached through the frame root
struct MeshPushData {
    vk::DeviceAddress frameRoot;
    // row of GpuFrameRoot::drawRecords: node instance, primitive, meshlet offset
    uint32_t drawIndex;
    // groups in this batch
    uint32_t meshletCount;
};

// MeshPushData must match shaders/types.slang MeshPushData (16 bytes).
static_assert(std::is_trivially_copyable_v<MeshPushData>);
static_assert(offsetof(MeshPushData, frameRoot) == 0);
static_assert(offsetof(MeshPushData, drawIndex) == 8);
static_assert(offsetof(MeshPushData, meshletCount) == 12);
static_assert(sizeof(MeshPushData) == 16);
