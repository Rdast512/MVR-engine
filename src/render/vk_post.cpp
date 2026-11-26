#include "vk_post.hpp"
#include "push_data.hpp"
#include "vk_shader_objects.hpp"

#include <format>

namespace
{
    constexpr uint32_t kPostHistogramTile = 16; // matches shaders/passes/exposure.slang
    constexpr uint32_t kPostTonemapTile = 8;       // matches shaders/passes/post.slang

    constexpr uint32_t postGroupCount(uint32_t value, uint32_t divisor) { return (value + divisor - 1) / divisor; }

    constexpr vk::DeviceSize kHistogramBytes = kExposureHistogramBins * sizeof(uint64_t);

    // compute writes -> the next pass's reads and writes
    void postComputeBarrier(const vk::raii::CommandBuffer& cmd)
    {
        const vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageRead |
                vk::AccessFlagBits2::eShaderStorageWrite,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
    }
} // namespace

PostProcess::PostProcess(const Device& device, const VkAllocator& allocator, const ShaderObjects& shaderObjects) :
    allocator(allocator), shaderObjects(shaderObjects)
{
    createBuffer(kHistogramBytes,
                 vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress |
                     vk::BufferUsageFlagBits2::eTransferDst,
                 vk::MemoryPropertyFlagBits::eDeviceLocal, histogram, histogramMemory, allocator.allocator,
                 device.vkdevice, device.queueFamilyIndices, "ExposureHistogram");
    histogramAddress = device.vkdevice.getBufferAddress({.buffer = *histogram});
    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        readbacks[slot] = createCounterReadback(sizeof(GpuExposureReadback), allocator.allocator, device.vkdevice,
                                                device.queueFamilyIndices, std::format("ExposureReadback_{}", slot));
    }
}

PostProcess::~PostProcess()
{
    destroyVmaBuffer(allocator.allocator, histogram, histogramMemory);
    for (CounterReadback& readback : readbacks) {
        destroyCounterReadback(allocator.allocator, readback);
    }
}

void PostProcess::record(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot, vk::DeviceAddress frameRoot,
                         uint32_t input, vk::Extent2D displayExtent, float deltaSeconds)
{
    uint32_t flags = settings.isAutoExposure ? ExposureFlag::Auto : 0u;
    if (isSnapPending) {
        flags |= ExposureFlag::Snap;
        isSnapPending = false;
    }
    const ExposurePushData exposurePush{
        .frameRoot = frameRoot,
        .histogram = histogramAddress,
        .readback = readbacks[frameSlot].address,
        .deltaSeconds = deltaSeconds,
        .compensationEv = settings.compensationEv,
        .manualEv = settings.manualEv,
        .minEv = settings.minEv,
        .maxEv = settings.maxEv,
        .adaptationSpeedUp = settings.adaptationSpeedUp,
        .adaptationSpeedDown = settings.adaptationSpeedDown,
        .flags = flags,
        .input = input,
        .pad0 = 0,
    };
    const vk::PushDataInfoEXT exposurePushInfo{.offset = 0,
                                               .data = {.address = &exposurePush, .size = sizeof(exposurePush)}};

    if (!isHistogramZeroed) {
        cmd.fillMemoryKHR({.address = histogramAddress, .size = kHistogramBytes}, kBoundStorageAddress, 0);
        const vk::MemoryBarrier2 fillBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eClear,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &fillBarrier});
        isHistogramZeroed = true;
    }
    // the input's writes and the last exposure pass's histogram clear -> the histogram
    postComputeBarrier(cmd);
    ShaderObjects::bindComputeShader(cmd, shaderObjects.frameShaders.exposureHistogram);
    cmd.pushDataEXT(exposurePushInfo);
    cmd.dispatch(postGroupCount(displayExtent.width, kPostHistogramTile), postGroupCount(displayExtent.height, kPostHistogramTile),
                 1);
    postComputeBarrier(cmd);
    ShaderObjects::bindComputeShader(cmd, shaderObjects.frameShaders.exposureAdapt);
    cmd.pushDataEXT(exposurePushInfo);
    cmd.dispatch(1, 1, 1);
    postComputeBarrier(cmd);

    const PostPushData postPush{.frameRoot = frameRoot, .input = input, .pad0 = 0};
    ShaderObjects::bindComputeShader(cmd, shaderObjects.frameShaders.post);
    cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &postPush, .size = sizeof(postPush)}});
    cmd.dispatch(postGroupCount(displayExtent.width, kPostTonemapTile), postGroupCount(displayExtent.height, kPostTonemapTile), 1);

    recordCounterReadback(cmd, readbacks[frameSlot]);
    isReadbackPending[frameSlot] = true;
}

void PostProcess::readBack(uint32_t frameSlot)
{
    if (isReadbackPending[frameSlot]) {
        lastExposure = *static_cast<const GpuExposureReadback*>(readbacks[frameSlot].readback.mapped);
        isReadbackPending[frameSlot] = false;
    }
}
