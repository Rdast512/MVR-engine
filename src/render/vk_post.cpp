#include "vk_post.hpp"
#include "push_data.hpp"
#include "vk_shader_objects.hpp"

#include <cstring>
#include <format>

namespace
{
    constexpr uint32_t kPostHistogramTile = 16; // matches shaders/passes/exposure.slang
    constexpr uint32_t kPostTonemapTile = 8;       // matches shaders/passes/post.slang

    constexpr uint32_t postGroupCount(uint32_t value, uint32_t divisor) { return (value + divisor - 1) / divisor; }

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
    // host-visible VRAM (ReBAR): zeroed once here, then cleared by the exposure pass
    constexpr auto histogramBytes = kExposureHistogramBins * sizeof(uint64_t);
    histogram = createHostBuffer(histogramBytes, allocator.allocator, device.vkdevice, device.queueFamilyIndices,
                                 "ExposureHistogram", nullptr);
    std::memset(histogram.mapped, 0, histogramBytes);
    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        readbacks[slot] = createHostBuffer(sizeof(GpuExposureReadback), allocator.allocator, device.vkdevice,
                                           device.queueFamilyIndices, std::format("ExposureReadback_{}", slot),
                                           nullptr);
    }
}

PostProcess::~PostProcess()
{
    destroyHostBuffer(allocator.allocator, histogram, nullptr);
    for (HostBuffer& readback : readbacks) {
        destroyHostBuffer(allocator.allocator, readback, nullptr);
    }
}

void PostProcess::record(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot, vk::DeviceAddress frameRoot,
                         vk::Extent2D displayExtent, float deltaSeconds)
{
    uint32_t flags = settings.isAutoExposure ? ExposureFlag::Auto : 0u;
    if (isSnapPending) {
        flags |= ExposureFlag::Snap;
        isSnapPending = false;
    }
    const ExposurePushData exposurePush{
        .frameRoot = frameRoot,
        .histogram = histogram.address,
        .readback = readbacks[frameSlot].address,
        .deltaSeconds = deltaSeconds,
        .compensationEv = settings.compensationEv,
        .manualEv = settings.manualEv,
        .minEv = settings.minEv,
        .maxEv = settings.maxEv,
        .adaptationSpeedUp = settings.adaptationSpeedUp,
        .adaptationSpeedDown = settings.adaptationSpeedDown,
        .flags = flags,
    };
    const vk::PushDataInfoEXT exposurePushInfo{.offset = 0,
                                               .data = {.address = &exposurePush, .size = sizeof(exposurePush)}};

    // DLSS-RR's output and the last exposure pass's histogram clear -> the histogram
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

    const FramePushData postPush{.frameRoot = frameRoot};
    ShaderObjects::bindComputeShader(cmd, shaderObjects.frameShaders.post);
    cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &postPush, .size = sizeof(postPush)}});
    cmd.dispatch(postGroupCount(displayExtent.width, kPostTonemapTile), postGroupCount(displayExtent.height, kPostTonemapTile), 1);

    // a fence wait alone does not make device writes visible to the host
    const vk::MemoryBarrier2 readbackBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &readbackBarrier});
    isReadbackPending[frameSlot] = true;
}

void PostProcess::readBack(uint32_t frameSlot)
{
    if (isReadbackPending[frameSlot]) {
        lastExposure = *static_cast<const GpuExposureReadback*>(readbacks[frameSlot].mapped);
        isReadbackPending[frameSlot] = false;
    }
}
