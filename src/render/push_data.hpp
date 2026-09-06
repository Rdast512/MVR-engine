#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vulkan/vulkan.hpp>

struct alignas(8) SlangHandle {
    uint32_t resourceIndex;
    uint32_t samplerIndex;
};

struct MeshPushData {
    vk::DeviceAddress cameraAddress;
    vk::DeviceAddress objectUbAddress;
    vk::DeviceAddress vertices;
    vk::DeviceAddress meshlets;
    vk::DeviceAddress meshletVertices;
    vk::DeviceAddress meshletTriangles;
    uint32_t firstMeshlet;
    uint32_t meshletCount;
    SlangHandle texture;
    SlangHandle samplerHandle;
};

static_assert(sizeof(SlangHandle) == sizeof(uint32_t) * 2,
              "Descriptor handle push layout must be uint2");
static_assert(sizeof(SlangHandle) == 8);

// MeshPushData must match shaders/types.slang MeshPushData (72 bytes).
static_assert(std::is_trivially_copyable_v<MeshPushData>);
static_assert(offsetof(MeshPushData, cameraAddress) == 0);
static_assert(offsetof(MeshPushData, objectUbAddress) == 8);
static_assert(offsetof(MeshPushData, vertices) == 16);
static_assert(offsetof(MeshPushData, meshlets) == 24);
static_assert(offsetof(MeshPushData, meshletVertices) == 32);
static_assert(offsetof(MeshPushData, meshletTriangles) == 40);
static_assert(offsetof(MeshPushData, firstMeshlet) == 48);
static_assert(offsetof(MeshPushData, meshletCount) == 52);
static_assert(offsetof(MeshPushData, texture) == 56);
static_assert(offsetof(MeshPushData, samplerHandle) == 64);
static_assert(sizeof(MeshPushData) == 72);
