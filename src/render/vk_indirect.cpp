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
                        vk::DeviceAddress frameRoot, vk::Extent2D extent, bool needsInputBarrier)
{
    // M8.5 P2: rayStartBarrier already orders resolve guides and previous-frame buffers, nrc_update is independent of
    // indirect (NrcGuide.md), and sharc.recordResolve emits resolveToQuery. Only secondary DI resampling reads this
    // frame's DI reservoirs. Every downstream pass (nrc.recordTrain, recordIndirectResampling, combineBarrier,
    // recordDebugView) emits its own tight input barrier, so no unconditional output barrier is needed here.
    if (needsInputBarrier) {
        const vk::MemoryBarrier2 inputBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &inputBarrier});
    }

    ShaderObjects::bindComputeShader(cmd, shader);
    const FramePushData push{.frameRoot = frameRoot};
    cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
    cmd.dispatch(divideRoundUp(extent.width, kIndirectTile), divideRoundUp(extent.height, kIndirectTile), 1);
}
