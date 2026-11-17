#include "vk_indirect.hpp"
#include "push_data.hpp"
#include "vk_shader_objects.hpp"

namespace
{
    // matches shaders/passes/indirect.slang kIndirectTile
    constexpr uint32_t kIndirectTile = 8;

    constexpr uint32_t divideRoundUp(uint32_t value, uint32_t divisor) { return (value + divisor - 1) / divisor; }
} // namespace

void recordIndirectPass(const vk::raii::CommandBuffer& cmd, const vk::raii::ShaderEXT& shader,
                        vk::DeviceAddress frameRoot, vk::Extent2D extent)
{
    // the resolve's guide writes -> reads; earlier compute reads of the GI reservoirs -> this pass's stores
    const vk::MemoryBarrier2 inputBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageRead |
                         vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &inputBarrier});

    ShaderObjects::bindComputeShader(cmd, shader);
    const FramePushData push{.frameRoot = frameRoot};
    cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
    cmd.dispatch(divideRoundUp(extent.width, kIndirectTile), divideRoundUp(extent.height, kIndirectTile), 1);

    // GI reservoirs -> the compute passes that read them (debug view; G3 resolve, G5 resampling)
    const vk::MemoryBarrier2 outputBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &outputBarrier});
}
