#include "check_passes.hpp"
#include "core/vk_allocator.hpp"
#include "core/vk_device.hpp"
#include "render/vk_shader_objects.hpp"

#include <format>
#include <stdexcept>

namespace
{
    constexpr uint32_t divideRoundUp(uint32_t value, uint32_t divisor) { return (value + divisor - 1) / divisor; }

    // MotionCheckPushData and RayCheckPushData share this layout
    struct StatsPushData
    {
        vk::DeviceAddress frameRoot;
        vk::DeviceAddress stats;
    };
    static_assert(sizeof(StatsPushData) == sizeof(MotionCheckPushData) &&
                  sizeof(StatsPushData) == sizeof(RayCheckPushData));
} // namespace

CheckPasses::CheckPasses(const Device& device, const VkAllocator& allocator, const ShaderObjects& shaderObjects)
    : device(device), allocator(allocator), shaderObjects(shaderObjects)
{
}

CheckPasses::~CheckPasses()
{
    for (HostBuffer& stats : motionCheckStats) {
        destroyHostBuffer(allocator.allocator, stats, nullptr);
    }
    for (HostBuffer& stats : rayCheckStats) {
        destroyHostBuffer(allocator.allocator, stats, nullptr);
    }
    for (HostBuffer& capture : displayCaptures) {
        destroyHostBuffer(allocator.allocator, capture, nullptr);
    }
}

void CheckPasses::enableMotionCheck()
{
    if (*motionCheckShader == nullptr) {
        motionCheckShader = shaderObjects.createComputeShader(readShaderSpirv("checks/motion_check.spv"),
                                                              "motionCheckMain", "MotionCheck");
    }
    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        if (motionCheckStats[slot].mapped == nullptr) {
            motionCheckStats[slot] = createHostBuffer(sizeof(GpuMotionCheckStats), allocator.allocator, device.vkdevice,
                                                      device.queueFamilyIndices,
                                                      std::format("MotionCheckStats_{}", slot), nullptr);
        }
    }
}

void CheckPasses::enableRayCheck()
{
    if (*rayCheckShader == nullptr) {
        rayCheckShader =
            shaderObjects.createComputeShader(readShaderSpirv("checks/rt_check.spv"), "rayCheckMain", "RayCheck");
    }
    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        if (rayCheckStats[slot].mapped == nullptr) {
            rayCheckStats[slot] = createHostBuffer(sizeof(GpuRayCheckStats), allocator.allocator, device.vkdevice,
                                                   device.queueFamilyIndices, std::format("RayCheckStats_{}", slot),
                                                   nullptr);
        }
    }
}

const GpuRayCheckStats& CheckPasses::getLastRayCheck() const
{
    if (rayCheckStats[lastRecordedSlot].mapped == nullptr) {
        throw std::logic_error("ray check read without enableRayCheck");
    }
    return *static_cast<const GpuRayCheckStats*>(rayCheckStats[lastRecordedSlot].mapped);
}

const GpuMotionCheckStats& CheckPasses::getLastMotionCheck() const
{
    if (motionCheckStats[lastRecordedSlot].mapped == nullptr) {
        throw std::logic_error("motion check read without enableMotionCheck");
    }
    return *static_cast<const GpuMotionCheckStats*>(motionCheckStats[lastRecordedSlot].mapped);
}

std::optional<DisplayCapture> CheckPasses::getLastDisplayCapture() const
{
    const std::optional<vk::Extent2D>& extent = displayCaptureExtents[lastRecordedSlot];
    if (!extent) {
        return std::nullopt;
    }
    const size_t halfCount = static_cast<size_t>(extent->width) * extent->height * 4;
    const auto* texels = static_cast<const uint16_t*>(displayCaptures[lastRecordedSlot].mapped);
    return DisplayCapture{.extent = *extent, .rgba16f = {texels, halfCount}};
}

void CheckPasses::recordAfterDebugView(const FrameHookContext& context)
{
    const uint32_t slot = context.frameSlot;
    if (void* stats = motionCheckStats[slot].mapped) {
        // host writes before the submit are visible to it
        *static_cast<GpuMotionCheckStats*>(stats) = {};
        recordStatsPass(context, motionCheckShader, motionCheckStats[slot].address);
    }
    if (void* stats = rayCheckStats[slot].mapped) {
        *static_cast<GpuRayCheckStats*>(stats) = {};
        recordStatsPass(context, rayCheckShader, rayCheckStats[slot].address);
    }
    displayCaptureExtents[slot].reset();
    if (isDisplayCapturePending) {
        recordDisplayCapture(context);
        isDisplayCapturePending = false;
    }
    lastRecordedSlot = slot;
}

void CheckPasses::recordStatsPass(const FrameHookContext& context, const vk::raii::ShaderEXT& shader,
                                  vk::DeviceAddress stats)
{
    const vk::raii::CommandBuffer& cmd = context.cmd;
    const StatsPushData push{.frameRoot = context.frameRoot, .stats = stats};
    ShaderObjects::bindComputeShader(cmd, shader);
    cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
    const vk::Extent2D extent = context.renderExtent;
    cmd.dispatch(divideRoundUp(extent.width, kFullscreenTile), divideRoundUp(extent.height, kFullscreenTile), 1);
    // a fence wait alone does not make device writes visible to the host
    const vk::MemoryBarrier2 readbackBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &readbackBarrier});
}

void CheckPasses::recordDisplayCapture(const FrameHookContext& context)
{
    constexpr vk::DeviceSize kTexelBytes = 8;
    const vk::raii::CommandBuffer& cmd = context.cmd;
    const uint32_t slot = context.frameSlot;
    const vk::Extent2D extent = context.presentedExtent;
    const vk::DeviceSize size = vk::DeviceSize{extent.width} * extent.height * kTexelBytes;
    HostBuffer& capture = displayCaptures[slot];
    if (displayCaptureCapacities[slot] < size) {
        destroyHostBuffer(allocator.allocator, capture, nullptr);
        // read back by the CPU: cached memory, not ReBAR
        createBuffer(size, vk::BufferUsageFlagBits2::eTransferDst,
                     vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent |
                         vk::MemoryPropertyFlagBits::eHostCached,
                     capture.buffer, capture.memory, allocator.allocator, device.vkdevice, device.queueFamilyIndices,
                     std::format("DisplayCapture_{}", slot), VMA_ALLOCATION_CREATE_MAPPED_BIT);
        VmaAllocationInfo allocationInfo{};
        vmaGetAllocationInfo(allocator.allocator, capture.memory, &allocationInfo);
        capture.mapped = allocationInfo.pMappedData;
        displayCaptureCapacities[slot] = size;
    }

    // debug view writes -> copy; the presented image stays in eGeneral
    const vk::MemoryBarrier2 copyBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &copyBarrier});
    const vk::BufferImageCopy2 region{
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {extent.width, extent.height, 1},
    };
    cmd.copyImageToBuffer2(vk::CopyImageToBufferInfo2{.srcImage = context.presentedImage,
                                                      .srcImageLayout = vk::ImageLayout::eGeneral,
                                                      .dstBuffer = *capture.buffer,
                                                      .regionCount = 1,
                                                      .pRegions = &region});
    const std::array<vk::MemoryBarrier2, 2> afterCopy{{
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        },
        // the next frame's debug view overwrites the copied texels: execution dependency only
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        },
    }};
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = static_cast<uint32_t>(afterCopy.size()),
                                            .pMemoryBarriers = afterCopy.data()});
    displayCaptureExtents[slot] = extent;
}
