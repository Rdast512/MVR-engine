#include "vk_world_lights.hpp"
#include "../util/vk_tracy.hpp"
#include "../util/vk_utils.hpp"
#include "push_data.hpp"

#include <bit>
#include <cmath>
#include <format>

#include <glm/trigonometric.hpp>

namespace
{
    // matches passes/light_world.slang
    constexpr uint32_t kLightWorldGroupSize = 64;

    constexpr uint32_t divideRoundUp(uint32_t value, uint32_t divisor) { return (value + divisor - 1) / divisor; }
} // namespace

WorldLights::Sun WorldLights::defaultSun()
{
    return Sun{
        .isEnabled = true,
        // along Sponza's long courtyard: a sun across it is blocked by the opposite wing up to ~55 degrees
        .azimuthDegrees = 20.0f,
        .elevationDegrees = 65.0f,
        // FP16 radiance without pre-exposure (considerations C-H2-1); auto exposure makes the scale irrelevant
        .illuminance = 10.0f,
        .color = glm::vec3(1.0f),
    };
}

uint32_t WorldLights::lightCount() const
{
    return assets.uploadedCount(AssetBuffer::Lights) + triangleLightCount() + (sun.isEnabled ? 1u : 0u);
}

uint32_t WorldLights::localLightCount() const
{
    return punctualLocalLightCount() + (areTriangleLightsSampled ? triangleLightCount() : 0u);
}

uint32_t WorldLights::punctualLocalLightCount() const { return assets.localLightCount(); }

uint32_t WorldLights::triangleLightCount() const { return rtScene.triangleLightCount(); }

uint32_t WorldLights::firstInfiniteLight() const { return punctualLocalLightCount() + triangleLightCount(); }

uint32_t WorldLights::infiniteLightCount() const { return lightCount() - firstInfiniteLight(); }

WorldLights::WorldLights(const Device& device, const VkAllocator& allocator, const FrameSync& frameSync,
                         const AssetBuffers& assets, const FrameData& frameData, const RayTracingScene& rtScene,
                         const ShaderObjects& shaderObjects) :
    device(device), allocator(allocator), frameSync(frameSync), assets(assets), frameData(frameData),
    rtScene(rtScene), shaderObjects(shaderObjects)
{
}

WorldLights::~WorldLights()
{
    for (SlotBuffer& slot : slots) {
        destroySlot(slot);
    }
}

void WorldLights::destroySlot(SlotBuffer& slot) const
{
    destroyVmaBuffer(allocator.allocator, slot.buffer, slot.memory);
    slot.address = 0;
    slot.capacity = 0;
}

void WorldLights::prepareFrame(uint32_t frameSlot, uint64_t lastSubmittedFrame)
{
    const uint32_t count = lightCount();
    SlotBuffer& slot = slots[frameSlot];
    if (count <= slot.capacity) {
        return;
    }
    ZoneScopedN("WorldLights::grow");
    // rare: only a light count above every earlier one
    if (lastSubmittedFrame > 0 && slot.memory != nullptr) {
        const vk::SemaphoreWaitInfo waitInfo{
            .semaphoreCount = 1, .pSemaphores = &*frameSync.rayTimeline, .pValues = &lastSubmittedFrame};
        while (vk::Result::eTimeout == device.vkdevice.waitSemaphores(waitInfo, UINT64_MAX))
            ;
    }
    destroySlot(slot);
    // powers of two: a scene that loads lights one model at a time reallocates rarely
    slot.capacity = std::bit_ceil(count);
    createBuffer(slot.capacity * sizeof(GpuLight),
                 vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eDeviceLocal, slot.buffer, slot.memory, allocator.allocator,
                 device.vkdevice, device.queueFamilyIndices, std::format("WorldLights_{}", frameSlot));
    slot.address = device.vkdevice.getBufferAddress({.buffer = *slot.buffer});
}

void WorldLights::recordUpdate(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot) const
{
    // the frame root carries the same count
    const uint32_t count = lightCount();
    if (count == 0) {
        return;
    }
    const float azimuth = glm::radians(sun.azimuthDegrees);
    const float elevation = glm::radians(sun.elevationDegrees);
    const glm::vec3 towardSun{std::cos(elevation) * std::cos(azimuth), std::sin(elevation),
                              std::cos(elevation) * std::sin(azimuth)};
    // readers wait for the compute timeline, which covers these writes. The slot's earlier readers are done: its own
    // frame by the slot fence, the next frame (previous-frame rows) by this submit's ray timeline wait
    const LightWorldPushData push{
        .frameRoot = frameData.frameRootAddress(frameSlot),
        .sunDirection = -towardSun,
        .sunIlluminance = sun.illuminance,
        .sunColor = sun.color,
        .assetLightCount = assets.uploadedCount(AssetBuffer::Lights),
        .triangleLightTasks = rtScene.triangleLightTasksAddress(frameSlot),
        .triangleLightTaskCount = rtScene.triangleLightTaskCount(),
        .pad0 = 0,
    };
    ShaderObjects::bindComputeShader(cmd, shaderObjects.frameShaders.lightWorld);
    cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
    cmd.dispatch(divideRoundUp(count, kLightWorldGroupSize), 1, 1);
}
