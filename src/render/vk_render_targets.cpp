#include "vk_render_targets.hpp"

#include "util/debug.hpp"
#include "util/vk_tracy.hpp"
#include "util/vk_utils.hpp"

#include <format>
#include <stdexcept>
#include <utility>

namespace
{
    // indexed by GuideImage (docs/engine_spec.md §9.3)
    constexpr std::array<vk::Format, kGuideImageCount> kGuideFormats = {
        vk::Format::eR16G16B16A16Sfloat, // NormalRoughness
        vk::Format::eR16G16Snorm, // GeoNormal
        vk::Format::eR8G8B8A8Unorm, // DiffuseAlbedo
        vk::Format::eR8G8B8A8Unorm, // SpecularF0
        vk::Format::eR8G8B8A8Unorm, // SpecAlbedo
        vk::Format::eB10G11R11UfloatPack32, // Emissive
        vk::Format::eR16G16Sfloat, // Motion
    };
    constexpr std::array<const char*, kGuideImageCount> kGuideNames = {
        "NormalRoughness", "GeoNormal", "DiffuseAlbedo", "SpecularF0", "SpecAlbedo", "Emissive", "Motion",
    };

    // the resolve writes through Unknown-format storage images; later passes sample
    constexpr vk::FormatFeatureFlags2 kStorageFeatures = vk::FormatFeatureFlagBits2::eStorageImage |
        vk::FormatFeatureFlagBits2::eStorageWriteWithoutFormat | vk::FormatFeatureFlagBits2::eSampledImage;
    constexpr vk::ImageUsageFlags kStorageUsage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;

    vk::ImageViewCreateInfo colorViewInfo(vk::Image image, vk::Format format)
    {
        return vk::ImageViewCreateInfo{.image = image,
                                       .viewType = vk::ImageViewType::e2D,
                                       .format = format,
                                       .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
    }
} // namespace

RenderTargets::RenderTargets(const Device& device, ResourceManager& resourceManager,
                             DescriptorManager& descriptorManager) :
    device(device), resourceManager(resourceManager), descriptorManager(descriptorManager)
{
    checkFormatSupport();
    linearSamplerIndex = descriptorManager.writeSamplerDescriptor(vk::SamplerCreateInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxLod = 0.0f,
    });
    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        // fixed size: not a Tracy memory pool
        resourceManager.createHostBuffer(frameTargets[slot], sizeof(GpuTargets), std::format("GpuTargets_{}", slot),
                                         nullptr);
    }
}

// the descriptor manager may already be gone; only memory is released
RenderTargets::~RenderTargets()
{
    const VmaAllocator allocator = resourceManager.allocator.allocator;
    visBufferView = nullptr;
    destroyVmaImage(allocator, visBuffer.image, visBuffer.memory);
    destroyVmaImage(allocator, display.image, display.memory);
    for (auto& guides : guideSets) {
        for (Target& guide : guides) {
            destroyVmaImage(allocator, guide.image, guide.memory);
        }
    }
    for (HostBuffer& buffer : frameTargets) {
        resourceManager.destroyHostBuffer(buffer, nullptr);
    }
}

void RenderTargets::checkFormatSupport() const
{
    const auto check = [this](vk::Format format, vk::FormatFeatureFlags2 required)
    {
        const auto properties =
            device.physicalDevice.getFormatProperties2<vk::FormatProperties2, vk::FormatProperties3>(format);
        if ((properties.get<vk::FormatProperties3>().optimalTilingFeatures & required) != required) {
            throw std::runtime_error(
                std::format("render targets: {} lacks {}", vk::to_string(format), vk::to_string(required)));
        }
    };
    // RG16_SNORM, RG16F and R11G11B10F storage need shaderStorageImageExtendedFormats, which every target has
    for (const vk::Format format : kGuideFormats) {
        check(format, kStorageFeatures);
    }
    check(kDisplayFormat, kStorageFeatures | vk::FormatFeatureFlagBits2::eSampledImageFilterLinear);
    check(kVisBufferFormat, vk::FormatFeatureFlagBits2::eColorAttachment | vk::FormatFeatureFlagBits2::eSampledImage);
}

void RenderTargets::createTarget(Target& target, vk::Format format, vk::ImageUsageFlags usage, vk::ImageLayout layout,
                                 const char* debugName)
{
    resourceManager.allocator.createImage2D(extent.width, extent.height, 1, vk::SampleCountFlagBits::e1, format, usage,
                                            target.image, target.memory, std::format("{}Memory", debugName));
    // recreated only on resize: not a Tracy memory pool
    setDebugName(device.vkdevice, target.image, debugName);
    const vk::ImageViewCreateInfo view = colorViewInfo(*target.image, format);
    if (usage & vk::ImageUsageFlagBits::eStorage) {
        target.storageIndex = descriptorManager.writeImageDescriptor(view, vk::DescriptorType::eStorageImage, layout);
    }
    target.sampledIndex = descriptorManager.writeImageDescriptor(view, vk::DescriptorType::eSampledImage, layout);
}

void RenderTargets::destroyTargets()
{
    const VmaAllocator allocator = resourceManager.allocator.allocator;
    const auto destroy = [&](Target& target)
    {
        for (uint32_t* index : {&target.storageIndex, &target.sampledIndex}) {
            if (*index != kNoneIndex) {
                descriptorManager.freeImageDescriptor(*index);
                *index = kNoneIndex;
            }
        }
        destroyVmaImage(allocator, target.image, target.memory);
    };
    visBufferView = nullptr;
    destroy(visBuffer);
    destroy(display);
    for (auto& guides : guideSets) {
        for (Target& guide : guides) {
            destroy(guide);
        }
    }
    if (depthIndex != kNoneIndex) {
        descriptorManager.freeImageDescriptor(depthIndex);
        depthIndex = kNoneIndex;
    }
}

void RenderTargets::recreate(vk::Extent2D renderExtent)
{
    ZoneScopedN("RenderTargets::recreate");
    destroyTargets();
    extent = renderExtent;

    createTarget(visBuffer, kVisBufferFormat,
                 vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
                 vk::ImageLayout::eShaderReadOnlyOptimal, "VisBuffer");
    visBufferView = vk::raii::ImageView(device.vkdevice, colorViewInfo(*visBuffer.image, kVisBufferFormat));
    setDebugName(device.vkdevice, visBufferView, "VisBufferView");
    createTarget(display, kDisplayFormat, kStorageUsage, vk::ImageLayout::eGeneral, "DisplayColor");
    for (uint32_t set = 0; set < kGuideSetCount; ++set) {
        for (size_t guide = 0; guide < kGuideImageCount; ++guide) {
            createTarget(guideSets[set][guide], kGuideFormats[guide], kStorageUsage, vk::ImageLayout::eGeneral,
                         std::format("Guide{}_{}", kGuideNames[guide], set).c_str());
        }
    }
    depthIndex = descriptorManager.writeImageDescriptor(
        vk::ImageViewCreateInfo{.image = *resourceManager.depthAttachment.image,
                                .viewType = vk::ImageViewType::e2D,
                                .format = resourceManager.findDepthFormat(),
                                .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}},
        vk::DescriptorType::eSampledImage, vk::ImageLayout::eShaderReadOnlyOptimal);

    vk::raii::CommandBuffer& cmd = resourceManager.commandBuffers[0];
    cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    // nothing to wait for: the device is idle and the content is undefined
    const auto initialize = [&cmd](vk::Image image, vk::ImageLayout layout, vk::AccessFlags2 access)
    {
        transitionImageLayout(&cmd, image, vk::ImageLayout::eUndefined, layout,
                              {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, vk::PipelineStageFlagBits2::eNone,
                              vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
                              vk::AccessFlagBits2::eNone, access);
    };
    constexpr vk::AccessFlags2 kStorageAccess =
        vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eShaderSampledRead;
    // the vis buffer is transitioned from eUndefined every frame (Renderer::recordCommandBuffer)
    initialize(*display.image, vk::ImageLayout::eGeneral, kStorageAccess);
    for (auto& guides : guideSets) {
        for (Target& guide : guides) {
            initialize(*guide.image, vk::ImageLayout::eGeneral, kStorageAccess);
        }
    }
    submitAndWait(cmd, resourceManager.graphicsQueue);
}

void RenderTargets::prepareFrame(uint32_t frameSlot, uint32_t frameNumber)
{
    const auto guideSet = [this](uint32_t set)
    {
        GpuGuideSet result;
        for (size_t guide = 0; guide < kGuideImageCount; ++guide) {
            result.storage[guide] = guideSets[set][guide].storageIndex;
            result.sampled[guide] = guideSets[set][guide].sampledIndex;
        }
        return result;
    };
    const uint32_t current = frameNumber % kGuideSetCount;
    // write-combined memory: one whole-struct store
    *static_cast<GpuTargets*>(frameTargets[frameSlot].mapped) = GpuTargets{
        .visBuffer = visBuffer.sampledIndex,
        .depth = depthIndex,
        .displayStorage = display.storageIndex,
        .displaySampled = display.sampledIndex,
        .linearSampler = linearSamplerIndex,
        .pad0 = 0,
        .renderSize = {extent.width, extent.height},
        .current = guideSet(current),
        .previous = guideSet((current + 1) % kGuideSetCount),
    };
    resourceManager.targetsAddresses[frameSlot] = frameTargets[frameSlot].address;
}
