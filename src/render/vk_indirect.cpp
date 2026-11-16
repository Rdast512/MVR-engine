#include "vk_indirect.hpp"
#include "push_data.hpp"

IndirectPass::IndirectPass(const Device& device, const VkAllocator& allocator) :
    pipelines{RtPipeline(device, allocator, source(false)), RtPipeline(device, allocator, source(true))},
    isReorderAvailable(device.capabilities.isSerReordering)
{
}

RtPipeline::Source IndirectPass::source(bool isReorderEnabled)
{
    return RtPipeline::Source{.file = "indirect.spv",
                              .raygenEntry = "indirectRaygen",
                              .missEntry = "indirectMiss",
                              .closestHitEntry = "indirectClosestHit",
                              .debugName = isReorderEnabled ? "IndirectReorder" : "Indirect",
                              .isReorderEnabled = isReorderEnabled};
}

void IndirectPass::record(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot, vk::DeviceAddress frameRoot,
                          vk::Extent2D extent) const
{
    // the resolve's guide writes -> reads; earlier compute reads of the GI reservoirs -> this pass's stores
    const vk::MemoryBarrier2 inputBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eRayTracingShaderKHR,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageRead |
                         vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &inputBarrier});

    const RtPipeline& pipeline = pipelines[settings.isReorderEnabled && isReorderAvailable ? 1 : 0];
    pipeline.bind(cmd);
    const FramePushData push{.frameRoot = frameRoot};
    cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
    pipeline.traceRays(cmd, frameSlot, extent);

    // GI reservoirs -> the compute passes that read them (debug view; G3 resolve, G5 resampling)
    const vk::MemoryBarrier2 outputBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eRayTracingShaderKHR,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &outputBarrier});
}
