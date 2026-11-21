#include "vk_render_targets.hpp"

#include "../static_headers/logger.hpp"
#include "util/debug.hpp"
#include "util/vk_tracy.hpp"
#include "util/vk_utils.hpp"
#include "push_data.hpp"

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
        vk::Format::eR16G16B16A16Sfloat, // Motion
        vk::Format::eR32G32B32A32Sfloat, // Position
    };
    constexpr std::array<const char*, kGuideImageCount> kGuideNames = {
        "NormalRoughness", "GeoNormal", "DiffuseAlbedo", "SpecularF0", "SpecAlbedo", "Emissive", "Motion", "Position",
    };

    // the resolve writes through typed storage images (core/binding.slang storageTex2D); later passes sample
    constexpr vk::FormatFeatureFlags2 kStorageFeatures =
        vk::FormatFeatureFlagBits2::eStorageImage | vk::FormatFeatureFlagBits2::eSampledImage;
    constexpr vk::ImageUsageFlags kStorageUsage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;

    vk::ImageViewCreateInfo colorViewInfo(vk::Image image, vk::Format format)
    {
        return vk::ImageViewCreateInfo{.image = image,
                                       .viewType = vk::ImageViewType::e2D,
                                       .format = format,
                                       .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
    }
} // namespace

RenderTargets::RenderTargets(const Device& device, const VkAllocator& allocator, FrameSync& frameSync,
                             DescriptorManager& descriptorManager) :
    device(device), allocator(allocator), frameSync(frameSync), descriptorManager(descriptorManager)
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
        frameTargets[slot] = createHostBuffer(sizeof(GpuTargets), allocator.allocator, device.vkdevice,
                                              device.queueFamilyIndices, std::format("GpuTargets_{}", slot), nullptr);
    }
}

// the descriptor manager may already be gone; only memory is released
RenderTargets::~RenderTargets()
{
    const VmaAllocator vma = allocator.allocator;
    visBufferView = nullptr;
    const auto destroy = [vma](Target& target)
    {
        target.view = nullptr;
        destroyVmaImage(vma, target.image, target.memory);
    };
    destroy(visBuffer);
    destroy(display);
    destroy(directRadiance);
    destroy(indirectRadiance);
    destroy(noisyColorTarget);
    destroy(upscaledTarget);
    destroy(postTarget);
    destroy(exposureTarget);
    destroy(binIds);
    for (auto& guides : guideSets) {
        for (Target& guide : guides) {
            destroy(guide);
        }
    }
    for (Target& stagnancy : giStagnancy) {
        destroy(stagnancy);
    }
    destroy(giSampleIds);
    destroy(giDuplication);
    destroyVmaBuffer(vma, materialBins, materialBinsMemory);
    for (HostBuffer& buffer : frameTargets) {
        destroyHostBuffer(vma, buffer, nullptr);
    }
    destroyAttachment(depthAttachment, nullptr);
}

vk::Format RenderTargets::findDepthFormat() const
{
    logInfo("findDepthFormat() started", "RenderTargets");
    // hi-z reads depth as R32_SFLOAT; no stencil user. BestPractices-NVIDIA-CreateImage-Depth32Format (0xf00e92a8) is
    // accepted: reverse Z needs float depth for its precision
    constexpr std::array kCandidates{vk::Format::eD32Sfloat};
    return findSupportedFormat(device.physicalDevice, kCandidates, vk::ImageTiling::eOptimal,
                               vk::FormatFeatureFlagBits::eDepthStencilAttachment |
                                   vk::FormatFeatureFlagBits::eSampledImage);
}

void RenderTargets::destroyAttachment(AttachmentImage& target, const char* tracyName)
{
    // view references the image, release it first
    target.view = nullptr;
    destroyVmaImage(allocator.allocator, target.image, target.memory, tracyName);
}

void RenderTargets::createAttachment(AttachmentImage& target, const AttachmentDesc& desc)
{
    destroyAttachment(target, desc.tracyName);
    allocator.createImage2D(extent.width, extent.height, 1, vk::SampleCountFlagBits::e1, desc.format, desc.usage,
                            target.image, target.memory, std::format("{}Memory", desc.debugName));
    setDebugName(device.vkdevice, target.image, desc.debugName);
    // approx footprint: 4 B/pixel
    const vk::DeviceSize approxBytes = static_cast<vk::DeviceSize>(extent.width) * extent.height * 4u;
    tracyResourceAlloc(static_cast<VkImage>(*target.image), static_cast<size_t>(approxBytes), desc.tracyName);

    vk::raii::CommandBuffer& cmd = frameSync.commandBuffers[0];
    cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    transitionImageLayout(&cmd, target.image, vk::ImageLayout::eUndefined, desc.layout,
                          {.aspectMask = desc.aspect,
                           .baseMipLevel = 0,
                           .levelCount = 1,
                           .baseArrayLayer = 0,
                           .layerCount = 1});
    submitAndWait(cmd, device.graphicsQueue);
    target.view = createImageView(device.vkdevice, *target.image, desc.format, desc.aspect, 1);
}

void RenderTargets::createDepthResources()
{
    logInfo("createDepthResources() started", "RenderTargets");
    depthFormat = findDepthFormat();
    logInfo(std::format("Depth format selected: {}", vk::to_string(depthFormat)), "RenderTargets");

    // sampled by the hi-z build, the resolve and the debug views
    createAttachment(depthAttachment,
                     {.format = depthFormat,
                      .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled,
                      .aspect = vk::ImageAspectFlagBits::eDepth,
                      .layout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
                      .debugName = "DepthImage",
                      // recreated only on resize: not a Tracy memory pool
                      .tracyName = nullptr});
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
    // RG16_SNORM, RG16F and R11G11B10F: SPIR-V StorageImageExtendedFormats, core since 1.0 (no device feature)
    for (const vk::Format format : kGuideFormats) {
        check(format, kStorageFeatures);
    }
    check(kDisplayFormat, kStorageFeatures | vk::FormatFeatureFlagBits2::eSampledImageFilterLinear);
    check(kDirectRadianceFormat, kStorageFeatures);
    check(kIndirectRadianceFormat, kStorageFeatures);
    check(kNoisyColorFormat, kStorageFeatures);
    check(kUpscaledFormat, kStorageFeatures | vk::FormatFeatureFlagBits2::eSampledImageFilterLinear);
    check(kPostFormat, kStorageFeatures | vk::FormatFeatureFlagBits2::eSampledImageFilterLinear);
    check(kExposureFormat, kStorageFeatures);
    check(kBinIdsFormat, kStorageFeatures);
    check(kVisBufferFormat, vk::FormatFeatureFlagBits2::eColorAttachment | vk::FormatFeatureFlagBits2::eSampledImage);
}

void RenderTargets::createTarget(Target& target, vk::Format format, vk::ImageUsageFlags usage, vk::ImageLayout layout,
                                 const char* debugName, vk::Extent2D size)
{
    allocator.createImage2D(size.width, size.height, 1, vk::SampleCountFlagBits::e1, format, usage, target.image,
                            target.memory, std::format("{}Memory", debugName));
    // recreated only on resize: not a Tracy memory pool
    setDebugName(device.vkdevice, target.image, debugName);
    const vk::ImageViewCreateInfo view = colorViewInfo(*target.image, format);
    target.view = vk::raii::ImageView(device.vkdevice, view);
    setDebugName(device.vkdevice, target.view, std::format("{}View", debugName));
    if (usage & vk::ImageUsageFlagBits::eStorage) {
        target.storageIndex = descriptorManager.writeImageDescriptor(view, vk::DescriptorType::eStorageImage, layout);
    }
    target.sampledIndex = descriptorManager.writeImageDescriptor(view, vk::DescriptorType::eSampledImage, layout);
}

void RenderTargets::destroyTargets()
{
    const VmaAllocator vma = allocator.allocator;
    const auto destroy = [&](Target& target)
    {
        for (uint32_t* index : {&target.storageIndex, &target.sampledIndex}) {
            if (*index != kNoneIndex) {
                descriptorManager.freeImageDescriptor(*index);
                *index = kNoneIndex;
            }
        }
        target.view = nullptr;
        destroyVmaImage(vma, target.image, target.memory);
    };
    visBufferView = nullptr;
    destroy(visBuffer);
    destroy(display);
    destroy(directRadiance);
    destroy(indirectRadiance);
    destroy(noisyColorTarget);
    destroy(upscaledTarget);
    destroy(postTarget);
    destroy(exposureTarget);
    destroy(binIds);
    for (auto& guides : guideSets) {
        for (Target& guide : guides) {
            destroy(guide);
        }
    }
    for (Target& stagnancy : giStagnancy) {
        destroy(stagnancy);
    }
    destroy(giSampleIds);
    destroy(giDuplication);
    destroyVmaBuffer(vma, materialBins, materialBinsMemory);
    materialBinsDeviceAddress = 0;
    if (depthIndex != kNoneIndex) {
        descriptorManager.freeImageDescriptor(depthIndex);
        depthIndex = kNoneIndex;
    }
}

void RenderTargets::recreate(vk::Extent2D renderExtent, vk::Extent2D displaySize)
{
    ZoneScopedN("RenderTargets::recreate");
    destroyTargets();
    extent = renderExtent;
    displayExtent = displaySize;
    createDepthResources();

    createTarget(visBuffer, kVisBufferFormat,
                 vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
                 vk::ImageLayout::eShaderReadOnlyOptimal, "VisBuffer", extent);
    visBufferView = vk::raii::ImageView(device.vkdevice, colorViewInfo(*visBuffer.image, kVisBufferFormat));
    setDebugName(device.vkdevice, visBufferView, "VisBufferView");
    // transfer sources for the guide capture (Renderer::requestDisplayCapture)
    createTarget(display, kDisplayFormat, kStorageUsage | vk::ImageUsageFlagBits::eTransferSrc,
                 vk::ImageLayout::eGeneral, "DisplayColor", extent);
    createTarget(directRadiance, kDirectRadianceFormat, kStorageUsage, vk::ImageLayout::eGeneral, "DirectRadiance",
                 extent);
    createTarget(indirectRadiance, kIndirectRadianceFormat, kStorageUsage, vk::ImageLayout::eGeneral,
                 "IndirectRadiance", extent);
    createTarget(noisyColorTarget, kNoisyColorFormat, kStorageUsage, vk::ImageLayout::eGeneral, "NoisyColor", extent);
    createTarget(upscaledTarget, kUpscaledFormat, kStorageUsage | vk::ImageUsageFlagBits::eTransferSrc,
                 vk::ImageLayout::eGeneral, "DlssOutput", displayExtent);
    // transfer source for the Lit display capture
    createTarget(postTarget, kPostFormat, kStorageUsage | vk::ImageUsageFlagBits::eTransferSrc,
                 vk::ImageLayout::eGeneral, "PostColor", displayExtent);
    // transfer destination for its initial value
    createTarget(exposureTarget, kExposureFormat, kStorageUsage | vk::ImageUsageFlagBits::eTransferDst,
                 vk::ImageLayout::eGeneral, "Exposure", vk::Extent2D{1, 1});
    createTarget(binIds, kBinIdsFormat, kStorageUsage, vk::ImageLayout::eGeneral, "MaterialBinIds", extent);
    createTarget(giSampleIds, kGiSampleIdsFormat, kStorageUsage, vk::ImageLayout::eGeneral, "GiSampleIds", extent);
    createTarget(giDuplication, kGiDuplicationFormat, kStorageUsage, vk::ImageLayout::eGeneral, "GiDuplication",
                 extent);
    createMaterialBins();
    for (uint32_t set = 0; set < kGuideSetCount; ++set) {
        for (size_t guide = 0; guide < kGuideImageCount; ++guide) {
            createTarget(guideSets[set][guide], kGuideFormats[guide], kStorageUsage, vk::ImageLayout::eGeneral,
                         std::format("Guide{}_{}", kGuideNames[guide], set).c_str(), extent);
        }
        createTarget(giStagnancy[set], kGiStagnancyFormat, kStorageUsage, vk::ImageLayout::eGeneral,
                     std::format("GiStagnancy_{}", set).c_str(), extent);
    }
    depthIndex = descriptorManager.writeImageDescriptor(
        vk::ImageViewCreateInfo{.image = *depthAttachment.image,
                                .viewType = vk::ImageViewType::e2D,
                                .format = depthFormat,
                                .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}},
        vk::DescriptorType::eSampledImage, vk::ImageLayout::eShaderReadOnlyOptimal);

    vk::raii::CommandBuffer& cmd = frameSync.commandBuffers[0];
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
    // the vis buffer is transitioned from eUndefined every frame (Renderer::recordFrameCommands)
    initialize(*display.image, vk::ImageLayout::eGeneral, kStorageAccess);
    initialize(*directRadiance.image, vk::ImageLayout::eGeneral, kStorageAccess);
    initialize(*indirectRadiance.image, vk::ImageLayout::eGeneral, kStorageAccess);
    initialize(*noisyColorTarget.image, vk::ImageLayout::eGeneral, kStorageAccess);
    initialize(*upscaledTarget.image, vk::ImageLayout::eGeneral, kStorageAccess);
    initialize(*postTarget.image, vk::ImageLayout::eGeneral, kStorageAccess);
    initialize(*binIds.image, vk::ImageLayout::eGeneral, kStorageAccess);
    // initialize the adaptation history to unity
    transitionImageLayout(&cmd, *exposureTarget.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                          {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}, VK_QUEUE_FAMILY_IGNORED,
                          VK_QUEUE_FAMILY_IGNORED, vk::PipelineStageFlagBits2::eNone,
                          vk::PipelineStageFlagBits2::eClear, vk::AccessFlagBits2::eNone,
                          vk::AccessFlagBits2::eTransferWrite);
    cmd.clearColorImage(*exposureTarget.image, vk::ImageLayout::eTransferDstOptimal,
                        vk::ClearColorValue{std::array{1.0f, 1.0f, 1.0f, 1.0f}},
                        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
    transitionImageLayout(&cmd, *exposureTarget.image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eGeneral,
                          {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}, VK_QUEUE_FAMILY_IGNORED,
                          VK_QUEUE_FAMILY_IGNORED, vk::PipelineStageFlagBits2::eClear,
                          vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eTransferWrite,
                          kStorageAccess | vk::AccessFlagBits2::eShaderStorageRead);
    for (auto& guides : guideSets) {
        for (Target& guide : guides) {
            initialize(*guide.image, vk::ImageLayout::eGeneral, kStorageAccess);
        }
    }
    // read only with ReSTIR GI history, which a recreate resets
    for (Target& stagnancy : giStagnancy) {
        initialize(*stagnancy.image, vk::ImageLayout::eGeneral, kStorageAccess);
    }
    initialize(*giSampleIds.image, vk::ImageLayout::eGeneral, kStorageAccess);
    initialize(*giDuplication.image, vk::ImageLayout::eGeneral, kStorageAccess);
    submitAndWait(cmd, device.graphicsQueue);
}

// one list per bin with room for every render tile: a tile is appended at most once per bin, so nothing overflows
void RenderTargets::createMaterialBins()
{
    const uint32_t tilesX = (extent.width + kMaterialTileSize - 1) / kMaterialTileSize;
    const uint32_t tilesY = (extent.height + kMaterialTileSize - 1) / kMaterialTileSize;
    const uint32_t tileCount = tilesX * tilesY;
    // a bin launches one group per tile in x; tile coordinates are packed into 16 bits each
    const uint32_t maxGroups = device.capabilities.properties2.properties.limits.maxComputeWorkGroupCount[0];
    if (tileCount > maxGroups || tilesX > 0xffffu || tilesY > 0xffffu) {
        throw std::runtime_error(std::format("render targets: {}x{} resolve tiles exceed maxComputeWorkGroupCount[0] {} "
                                             "or 16-bit tile coordinates",
                                             tilesX, tilesY, maxGroups));
    }
    const vk::DeviceSize size =
        sizeof(GpuMaterialBins) + static_cast<vk::DeviceSize>(kMaterialBinCount) * tileCount * sizeof(uint32_t);
    // indirect: DGC sequences and sequenceCountAddress; transfer: the per-frame count fill
    createBuffer(size,
                 vk::BufferUsageFlagBits2::eIndirectBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress |
                     vk::BufferUsageFlagBits2::eTransferDst,
                 vk::MemoryPropertyFlagBits::eDeviceLocal, materialBins, materialBinsMemory, allocator.allocator,
                 device.vkdevice, device.queueFamilyIndices, "MaterialBins");
    materialBinsDeviceAddress = device.vkdevice.getBufferAddress({.buffer = *materialBins});
}

RenderTargets::ImageRef RenderTargets::guide(uint32_t frameNumber, GuideImage image) const
{
    const size_t index = std::to_underlying(image);
    return imageRef(guideSets[frameNumber % kGuideSetCount][index], kGuideFormats[index]);
}

void RenderTargets::prepareFrame(uint32_t frameSlot, uint32_t frameNumber, bool isPostPresented)
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
        .directRadianceStorage = directRadiance.storageIndex,
        .directRadianceSampled = directRadiance.sampledIndex,
        .indirectRadianceStorage = indirectRadiance.storageIndex,
        .indirectRadianceSampled = indirectRadiance.sampledIndex,
        .noisyColorStorage = noisyColorTarget.storageIndex,
        .noisyColorSampled = noisyColorTarget.sampledIndex,
        .presentSampled = isPostPresented ? postTarget.sampledIndex : display.sampledIndex,
        .upscaledSampled = upscaledTarget.sampledIndex,
        .postStorage = postTarget.storageIndex,
        .exposureStorage = exposureTarget.storageIndex,
        .displaySize = {displayExtent.width, displayExtent.height},
        .binIdsStorage = binIds.storageIndex,
        .binIdsSampled = binIds.sampledIndex,
        .giStagnancyStorage = giStagnancy[current].storageIndex,
        .giStagnancyPrevious = giStagnancy[(current + 1) % kGuideSetCount].sampledIndex,
        .giSampleIdsStorage = giSampleIds.storageIndex,
        .giDuplicationStorage = giDuplication.storageIndex,
    };
}
