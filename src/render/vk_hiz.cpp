#include "vk_hiz.hpp"

#include "push_data.hpp"
#include "util/debug.hpp"
#include "util/vk_tracy.hpp"
#include "util/vk_utils.hpp"

#include <algorithm>
#include <bit>
#include <format>
#include <stdexcept>

namespace
{
    constexpr const char* kTracyName = "GPU/HiZPyramid";
    constexpr vk::Format kFormat = vk::Format::eR32Sfloat;
    // matches shaders/passes/hiz_build.slang
    constexpr uint32_t kGroupSize = 8;

    constexpr uint32_t divideRoundUp(uint32_t value, uint32_t divisor) { return (value + divisor - 1) / divisor; }

    void checkFormatSupport(const Device& device)
    {
        // storage writes go through an Unknown-format image type (allowed per format at api 1.3+)
        constexpr vk::FormatFeatureFlags2 required = vk::FormatFeatureFlagBits2::eStorageImage |
            vk::FormatFeatureFlagBits2::eStorageWriteWithoutFormat | vk::FormatFeatureFlagBits2::eSampledImage |
            vk::FormatFeatureFlagBits2::eSampledImageFilterLinear |
            vk::FormatFeatureFlagBits2::eSampledImageFilterMinmax;
        const auto properties =
            device.physicalDevice.getFormatProperties2<vk::FormatProperties2, vk::FormatProperties3>(kFormat);
        if ((properties.get<vk::FormatProperties3>().optimalTilingFeatures & required) != required) {
            throw std::runtime_error(std::format("hi-z: {} lacks storage / max-reduction support",
                                                 vk::to_string(kFormat)));
        }
    }
} // namespace

HiZPyramid::HiZPyramid(const Device& device, ResourceManager& resourceManager, DescriptorManager& descriptorManager,
                       const ShaderObjects& shaderObjects) :
    device(device), resourceManager(resourceManager), descriptorManager(descriptorManager),
    shaderObjects(shaderObjects)
{
    checkFormatSupport(device);
    const vk::SamplerReductionModeCreateInfo reduction{.reductionMode = vk::SamplerReductionMode::eMax};
    maxSamplerIndex = descriptorManager.writeSamplerDescriptor(vk::SamplerCreateInfo{
        .pNext = &reduction,
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxLod = vk::LodClampNone,
    });
}

// the descriptor manager may already be gone; only the image is released
HiZPyramid::~HiZPyramid() { destroyVmaImage(resourceManager.allocator.allocator, image, memory, kTracyName); }

void HiZPyramid::freeDescriptors()
{
    for (const uint32_t index : mipIndices) {
        descriptorManager.freeImageDescriptor(index);
    }
    mipIndices.clear();
    for (uint32_t* index : {&pyramidIndex, &depthIndex}) {
        if (*index != kNoneIndex) {
            descriptorManager.freeImageDescriptor(*index);
            *index = kNoneIndex;
        }
    }
}

void HiZPyramid::recreate()
{
    freeDescriptors();
    destroyVmaImage(resourceManager.allocator.allocator, image, memory, kTracyName);
    isBuilt = false;

    depthExtent = resourceManager.swapChainExtent;
    extent = {std::bit_floor(depthExtent.width), std::bit_floor(depthExtent.height)};
    const auto mipCount = static_cast<uint32_t>(std::bit_width(std::max(extent.width, extent.height)));
    resourceManager.allocator.createImage2D(extent.width, extent.height, mipCount, vk::SampleCountFlagBits::e1,
                                            kFormat,
                                            vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                                            image, memory, "HiZPyramidMemory");
    setDebugName(device.vkdevice, image, "HiZPyramid");
    // full chain is 4/3 of mip 0
    const size_t approxBytes = size_t{extent.width} * extent.height * sizeof(float) * 4 / 3;
    tracyResourceAlloc(static_cast<VkImage>(*image), approxBytes, kTracyName);

    vk::raii::CommandBuffer& cmd = resourceManager.commandBuffers[0];
    cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    transitionImageLayout(&cmd, *image, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                          {vk::ImageAspectFlagBits::eColor, 0, mipCount, 0, 1}, VK_QUEUE_FAMILY_IGNORED,
                          VK_QUEUE_FAMILY_IGNORED, vk::PipelineStageFlagBits2::eTopOfPipe,
                          vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eNone,
                          vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eShaderSampledRead);
    submitAndWait(cmd, resourceManager.graphicsQueue);

    const auto pyramidView = [this](uint32_t baseMip, uint32_t levels) {
        return vk::ImageViewCreateInfo{.image = *image,
                                       .viewType = vk::ImageViewType::e2D,
                                       .format = kFormat,
                                       .subresourceRange = {vk::ImageAspectFlagBits::eColor, baseMip, levels, 0, 1}};
    };
    pyramidIndex = descriptorManager.writeImageDescriptor(pyramidView(0, mipCount), vk::DescriptorType::eSampledImage,
                                                          vk::ImageLayout::eGeneral);
    mipIndices.reserve(mipCount);
    for (uint32_t mip = 0; mip < mipCount; ++mip) {
        mipIndices.push_back(descriptorManager.writeImageDescriptor(
            pyramidView(mip, 1), vk::DescriptorType::eStorageImage, vk::ImageLayout::eGeneral));
    }
    depthIndex = descriptorManager.writeImageDescriptor(
        vk::ImageViewCreateInfo{.image = *resourceManager.depthAttachment.image,
                                .viewType = vk::ImageViewType::e2D,
                                .format = resourceManager.findDepthFormat(),
                                .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}},
        vk::DescriptorType::eSampledImage, vk::ImageLayout::eShaderReadOnlyOptimal);
}

vk::Extent2D HiZPyramid::mipExtent(uint32_t mip) const
{
    return {std::max(extent.width >> mip, 1u), std::max(extent.height >> mip, 1u)};
}

void HiZPyramid::recordBuild(const vk::raii::CommandBuffer& cmd, bool isFrozen)
{
    ZoneScopedN("HiZPyramid::recordBuild");
    // depth writes -> mip 0 reads; the previous build's pyramid accesses -> this build's writes
    const vk::ImageMemoryBarrier2 depthBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
            vk::PipelineStageFlagBits2::eLateFragmentTests,
        .srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
        .oldLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = *resourceManager.depthAttachment.image,
        .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
    };
    // fragment: the Hi-Z debug view samples it in the late raster pass
    const vk::MemoryBarrier2 previousBuildBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                            .pMemoryBarriers = &previousBuildBarrier,
                                            .imageMemoryBarrierCount = 1,
                                            .pImageMemoryBarriers = &depthBarrier});
    // the next raster pass expects depth in eShaderReadOnlyOptimal either way
    if (isFrozen) {
        return;
    }
    // single queue: every later submission sees this build
    isBuilt = true;

    HiZPushData push{
        .srcImage = depthIndex,
        .dstImage = mipIndices[0],
        .maxSampler = maxSamplerIndex,
        .srcMip = 0,
        .srcSize = depthExtent,
        .dstSize = extent,
    };
    ShaderObjects::bindComputeShader(cmd, shaderObjects.hizShaders.fromDepth);
    cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
    cmd.dispatch(divideRoundUp(extent.width, kGroupSize), divideRoundUp(extent.height, kGroupSize), 1);

    // mip n-1 written -> mip n samples it; the pyramid stays eGeneral, so a global barrier suffices
    const vk::MemoryBarrier2 mipBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
    };
    ShaderObjects::bindComputeShader(cmd, shaderObjects.hizShaders.downsample);
    push.srcImage = pyramidIndex;
    for (uint32_t mip = 1; mip < mipIndices.size(); ++mip) {
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &mipBarrier});
        push.dstImage = mipIndices[mip];
        push.srcMip = mip - 1;
        push.srcSize = mipExtent(mip - 1);
        push.dstSize = mipExtent(mip);
        cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
        cmd.dispatch(divideRoundUp(push.dstSize.width, kGroupSize), divideRoundUp(push.dstSize.height, kGroupSize),
                     1);
    }
}
