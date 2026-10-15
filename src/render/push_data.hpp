#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vulkan/vulkan.hpp>

// one DGC sequence: everything else is reached through the frame root
struct MeshPushData {
    vk::DeviceAddress frameRoot;
    uint32_t instanceIndex;
    uint32_t primitiveIndex;
    // first meshlet of this batch, relative to the primitive
    uint32_t meshletOffset;
    uint32_t meshletCount;
};

// MeshPushData must match shaders/types.slang MeshPushData (24 bytes).
static_assert(std::is_trivially_copyable_v<MeshPushData>);
static_assert(offsetof(MeshPushData, frameRoot) == 0);
static_assert(offsetof(MeshPushData, instanceIndex) == 8);
static_assert(offsetof(MeshPushData, primitiveIndex) == 12);
static_assert(offsetof(MeshPushData, meshletOffset) == 16);
static_assert(offsetof(MeshPushData, meshletCount) == 20);
static_assert(sizeof(MeshPushData) == 24);
