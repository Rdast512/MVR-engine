#pragma once
#include "../Constants.h"
#include "core/vk_allocator.hpp"
#include "core/vk_asset_buffers.hpp"
#include "core/vk_device.hpp"
#include "core/vk_frame_data.hpp"
#include "vk_shader_objects.hpp"

#include <array>
#include <cstdint>
#include <vulkan/vulkan_raii.hpp>

// World-space light rows of each frame slot (PBR plan E2): every frame, passes/light_world.slang places the
// model-space rows of the light asset buffer with their entity's matrix, so lights follow their entity with no CPU
// loop. Rows keep the asset buffer's order, point and spot before directional (RTXDI's local and infinite regions).
// Recorded into the slot's async compute command buffer, before the TLAS build
class WorldLights
{
public:
    WorldLights(const Device& device, const VkAllocator& allocator, const AssetBuffers& assets,
                const FrameData& frameData, const ShaderObjects& shaderObjects);
    ~WorldLights();

    WorldLights(const WorldLights&) = delete;
    WorldLights& operator=(const WorldLights&) = delete;
    WorldLights(WorldLights&&) = delete;
    WorldLights& operator=(WorldLights&&) = delete;

    // the slot is not in flight: grows its buffer to the uploaded light count
    void prepareFrame(uint32_t frameSlot);
    // compute queue, heaps bound. The slot's frame root must be written before submit
    void recordUpdate(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot) const;

    // GpuLight[lightCount]; 0 while the scene has no light
    [[nodiscard]] vk::DeviceAddress address(uint32_t frameSlot) const noexcept { return slots[frameSlot].address; }

private:
    struct SlotBuffer
    {
        vk::raii::Buffer buffer = nullptr;
        VmaAllocation memory = nullptr;
        vk::DeviceAddress address = 0;
        uint32_t capacity = 0;
    };

    void destroySlot(SlotBuffer& slot) const;

    const Device& device;
    const VkAllocator& allocator;
    const AssetBuffers& assets;
    const FrameData& frameData;
    const ShaderObjects& shaderObjects;
    std::array<SlotBuffer, MAX_FRAMES_IN_FLIGHT> slots;
};
