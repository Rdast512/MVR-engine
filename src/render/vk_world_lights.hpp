#pragma once
#include "../Constants.h"
#include "core/vk_allocator.hpp"
#include "core/vk_asset_buffers.hpp"
#include "core/vk_device.hpp"
#include "core/vk_frame_data.hpp"
#include "vk_shader_objects.hpp"

#include <array>
#include <cstdint>
#include <glm/vec3.hpp>
#include <vulkan/vulkan_raii.hpp>

// World-space light rows of each frame slot (PBR plan E2): every frame, passes/light_world.slang places the
// model-space rows of the light asset buffer with their entity's matrix, so lights follow their entity with no CPU
// loop. Rows keep the asset buffer's order, point and spot before directional (RTXDI's local and infinite regions).
// The engine's sun (PBR plan E3) is one more directional row after them. Recorded into the slot's async compute
// command buffer, before the TLAS build
class WorldLights
{
public:
    // a directional light owned by the engine, not by an asset; glTF samples such as Sponza ship without lights
    struct Sun
    {
        bool isEnabled = true;
        // degrees from +X toward +Z, around +Y (up)
        float azimuthDegrees = 0.0f;
        // degrees above the horizon
        float elevationDegrees = 0.0f;
        // lux, as glTF directional intensity
        float illuminance = 0.0f;
        glm::vec3 color{1.0f};

        bool operator==(const Sun&) const = default;
    };
    [[nodiscard]] static Sun defaultSun();

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

    // GpuLight[lightCount()]; 0 while the scene has no light
    [[nodiscard]] vk::DeviceAddress address(uint32_t frameSlot) const noexcept { return slots[frameSlot].address; }
    // world rows: the asset's lights, then the sun when enabled
    [[nodiscard]] uint32_t lightCount() const;
    // point and spot rows lead; the sun is directional and never counts here
    [[nodiscard]] uint32_t localLightCount() const;
    [[nodiscard]] bool hasLights() const { return lightCount() > 0; }

    // read by the next prepareFrame and recordUpdate
    Sun sun = defaultSun();

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
