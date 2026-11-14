#include "vk_world_lights.hpp"
#include "../util/vk_tracy.hpp"
#include "../util/vk_utils.hpp"
#include "push_data.hpp"

#include <bit>
#include <format>

namespace
{
    // matches passes/light_world.slang
    constexpr uint32_t kLightWorldGroupSize = 64;

    constexpr uint32_t divideRoundUp(uint32_t value, uint32_t divisor) { return (value + divisor - 1) / divisor; }
} // namespace

WorldLights::WorldLights(const Device& device, const VkAllocator& allocator, const AssetBuffers& assets,
                         const FrameData& frameData, const ShaderObjects& shaderObjects)
    : device(device), allocator(allocator), assets(assets), frameData(frameData), shaderObjects(shaderObjects)
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

void WorldLights::prepareFrame(uint32_t frameSlot)
{
    const uint32_t count = assets.uploadedCount(AssetBuffer::Lights);
    SlotBuffer& slot = slots[frameSlot];
    if (count <= slot.capacity) {
        return;
    }
    ZoneScopedN("WorldLights::grow");
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
    const uint32_t count = assets.uploadedCount(AssetBuffer::Lights);
    if (count == 0) {
        return;
    }
    // readers wait for the compute timeline, which covers these writes; the slot's previous readers finished before
    // the CPU reached this slot again
    const FramePushData push{.frameRoot = frameData.frameRootAddress(frameSlot)};
    ShaderObjects::bindComputeShader(cmd, shaderObjects.frameShaders.lightWorld);
    cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
    cmd.dispatch(divideRoundUp(count, kLightWorldGroupSize), 1, 1);
}
