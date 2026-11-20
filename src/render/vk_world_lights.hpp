#pragma once
#include "../Constants.h"
#include "core/types.hpp"
#include "core/vk_allocator.hpp"
#include "core/vk_asset_buffers.hpp"
#include "core/vk_device.hpp"
#include "core/vk_frame_data.hpp"
#include "core/vk_frame_sync.hpp"
#include "vk_rt_scene.hpp"
#include "vk_shader_objects.hpp"

#include <array>
#include <cstdint>
#include <glm/vec3.hpp>
#include <vulkan/vulkan_raii.hpp>

// the local light PDF's mip 0 as light_world writes it (RtxdiContext)
struct LightPdfTarget
{
    // resource heap: storage view of mip 0
    uint32_t mip0 = kNoneIndex;
    // 0: no PDF is written
    uint32_t texelCount = 0;
};

// World-space light rows of each frame slot (PBR plan E2): every frame, passes/light_world.slang places the
// model-space rows of the light asset buffer with their entity's matrix, so lights follow their entity with no CPU
// loop. Row order: the asset's point and spot rows, then one triangle light per triangle of every emissive ray
// instance (spec/m7/e2_emissive_triangles.md, built from the RT scene's tasks), then the asset's directional rows and
// the engine's sun (PBR plan E3). The first two are RTXDI's local region, the rest its infinite region. Recorded into
// the slot's async compute command buffer, before the TLAS build. The next frame reads a slot's rows as its
// previous-frame lights, so a slot is rewritten only after that frame's ray passes
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

    WorldLights(const Device& device, const VkAllocator& allocator, const FrameSync& frameSync,
                const AssetBuffers& assets, const FrameData& frameData, const RayTracingScene& rtScene,
                const ShaderObjects& shaderObjects);
    ~WorldLights();

    WorldLights(const WorldLights&) = delete;
    WorldLights& operator=(const WorldLights&) = delete;
    WorldLights(WorldLights&&) = delete;
    WorldLights& operator=(WorldLights&&) = delete;

    // the slot is not in flight, and the RT scene has followed the layout: grows its buffer to the light count.
    // lastSubmittedFrame: the frame number submitted last, which reads this slot as its previous frame; growing waits
    // for its ray passes
    void prepareFrame(uint32_t frameSlot, uint64_t lastSubmittedFrame);
    // compute queue, heaps bound. The slot's frame root must be written before submit. Also writes every texel of
    // the light PDF's mip 0: a local light's power at its Z-curve texel, 0 past the local lights
    void recordUpdate(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot, const LightPdfTarget& lightPdf) const;

    // GpuLight[lightCount()]; 0 while the scene has no light
    [[nodiscard]] vk::DeviceAddress address(uint32_t frameSlot) const noexcept { return slots[frameSlot].address; }
    // world rows: the asset's lights and the triangle lights, then the sun when enabled
    [[nodiscard]] uint32_t lightCount() const;
    // RTXDI's local region: the point and spot rows, then the triangle lights while they are sampled
    [[nodiscard]] uint32_t localLightCount() const;
    // point and spot rows: the world row of triangle light 0
    [[nodiscard]] uint32_t punctualLocalLightCount() const;
    [[nodiscard]] uint32_t triangleLightCount() const;
    // RTXDI's infinite region: the asset's directional rows and the sun, after the triangle rows
    [[nodiscard]] uint32_t firstInfiniteLight() const;
    [[nodiscard]] uint32_t infiniteLightCount() const;
    [[nodiscard]] bool hasLights() const { return lightCount() > 0; }

    // read by the next prepareFrame and recordUpdate
    Sun sun = defaultSun();
    // off: the triangle rows are still written, but RTXDI samples the punctual lights only and ray hits add the
    // triangles' emission (spec/m7/e2_emissive_triangles.md: the emission-rule comparison, the punctual convergence
    // check). A change resets lighting history
    bool areTriangleLightsSampled = true;

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
    const FrameSync& frameSync;
    const AssetBuffers& assets;
    const FrameData& frameData;
    const RayTracingScene& rtScene;
    const ShaderObjects& shaderObjects;
    std::array<SlotBuffer, MAX_FRAMES_IN_FLIGHT> slots;
};
