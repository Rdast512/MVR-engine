#pragma once

#include "../Constants.h"
#include "core/types.hpp"
#include "core/vk_descriptors.hpp"
#include "core/vk_allocator.hpp"
#include "core/vk_device.hpp"
#include "core/vk_frame_sync.hpp"
#include "util/vk_utils.hpp"

#include <array>
#include <vulkan/vulkan_raii.hpp>

// render-resolution targets of the visibility-buffer path (spec/features/visibility_buffer.md §3.6): the depth
// attachment, the R32_UINT vis buffer, two guide sets by frame parity, the direct radiance, the noisy color and the
// debug display image, plus the display-resolution DLSS-RR output and one GpuTargets block per frame slot.
// Every storage target stays in eGeneral; the vis buffer is a color attachment while rastering and
// eShaderReadOnlyOptimal after
class RenderTargets
{
public:
    static constexpr vk::Format kVisBufferFormat = vk::Format::eR32Uint;
    static constexpr vk::Format kDisplayFormat = vk::Format::eR16G16B16A16Sfloat;
    // linear HDR, written by passes/rtxdi_shade.slang
    static constexpr vk::Format kDirectRadianceFormat = vk::Format::eR16G16B16A16Sfloat;
    // linear HDR, written by passes/rtxdi_gi_shade.slang
    static constexpr vk::Format kIndirectRadianceFormat = vk::Format::eR16G16B16A16Sfloat;
    // linear HDR: passes/combine.slang's output and the DLSS-RR input and output
    static constexpr vk::Format kNoisyColorFormat = vk::Format::eR16G16B16A16Sfloat;
    static constexpr vk::Format kUpscaledFormat = vk::Format::eR16G16B16A16Sfloat;
    static constexpr vk::Format kPostFormat = vk::Format::eR16G16B16A16Sfloat;
    static constexpr vk::Format kExposureFormat = vk::Format::eR32Sfloat;

    // an image with the view NGX needs; the descriptor heap does not keep views
    struct ImageRef
    {
        vk::Image image;
        vk::ImageView view;
        vk::Format format = vk::Format::eUndefined;
    };

    RenderTargets(const Device& device, const VkAllocator& allocator, FrameSync& frameSync,
                  DescriptorManager& descriptorManager);
    ~RenderTargets();

    RenderTargets(const RenderTargets&) = delete;
    RenderTargets& operator=(const RenderTargets&) = delete;
    RenderTargets(RenderTargets&&) = delete;
    RenderTargets& operator=(RenderTargets&&) = delete;

    // device idle; depth first, every target at renderExtent except the DLSS-RR output at displayExtent
    void recreate(vk::Extent2D renderExtent, vk::Extent2D displayExtent);
    // writes the slot's GpuTargets with the guide set of frameNumber's parity; isUpscaledPresented: present reads the
    // DLSS-RR output instead of the display image
    void prepareFrame(uint32_t frameSlot, uint32_t frameNumber, bool isUpscaledPresented);

    // depth, vis buffer, guides and every culling size: the DLSS render resolution
    [[nodiscard]] vk::Extent2D getExtent() const noexcept { return extent; }
    [[nodiscard]] vk::Extent2D getDisplayExtent() const noexcept { return displayExtent; }
    // guide of the set frameNumber's parity selects
    [[nodiscard]] ImageRef guide(uint32_t frameNumber, GuideImage image) const;
    [[nodiscard]] ImageRef noisyColor() const { return imageRef(noisyColorTarget, kNoisyColorFormat); }
    // eShaderReadOnlyOptimal after the late hi-z build
    [[nodiscard]] ImageRef depth() const
    {
        return ImageRef{.image = *depthAttachment.image, .view = *depthAttachment.view, .format = depthFormat};
    }
    // display resolution, eGeneral
    [[nodiscard]] ImageRef upscaled() const { return imageRef(upscaledTarget, kUpscaledFormat); }
    // display resolution, eGeneral: tonemapped, what Lit presents
    [[nodiscard]] ImageRef post() const { return imageRef(postTarget, kPostFormat); }
    // 1x1, eGeneral: the exposure scale, 1 after each recreate
    [[nodiscard]] ImageRef exposure() const { return imageRef(exposureTarget, kExposureFormat); }
    // the slot's GpuTargets, for the frame root
    [[nodiscard]] vk::DeviceAddress targetsAddress(uint32_t frameSlot) const noexcept
    {
        return frameTargets[frameSlot].address;
    }
    [[nodiscard]] vk::Image getDepthImage() const noexcept { return *depthAttachment.image; }
    [[nodiscard]] vk::ImageView getDepthView() const noexcept { return *depthAttachment.view; }
    [[nodiscard]] vk::Format getDepthFormat() const noexcept { return depthFormat; }
    [[nodiscard]] vk::Image getVisBufferImage() const noexcept { return *visBuffer.image; }
    [[nodiscard]] vk::ImageView getVisBufferView() const noexcept { return *visBufferView; }
    // kDisplayFormat, eGeneral
    [[nodiscard]] vk::Image getDisplayImage() const noexcept { return *display.image; }

private:
    struct Target
    {
        vk::raii::Image image = nullptr;
        VmaAllocation memory = nullptr;
        // for NGX; destroyed before the image
        vk::raii::ImageView view = nullptr;
        uint32_t storageIndex = kNoneIndex;
        uint32_t sampledIndex = kNoneIndex;
    };

    // render-resolution target, recreated on resize
    struct AttachmentImage
    {
        vk::raii::Image image = nullptr;
        VmaAllocation memory = nullptr;
        vk::raii::ImageView view = nullptr;
    };
    struct AttachmentDesc
    {
        vk::Format format;
        vk::ImageUsageFlags usage;
        vk::ImageAspectFlags aspect;
        vk::ImageLayout layout;
        const char* debugName;
        const char* tracyName;
    };

    void checkFormatSupport() const;
    [[nodiscard]] vk::Format findDepthFormat() const;
    void createDepthResources();
    void createAttachment(AttachmentImage& target, const AttachmentDesc& desc);
    void destroyAttachment(AttachmentImage& target, const char* tracyName);
    void createTarget(Target& target, vk::Format format, vk::ImageUsageFlags usage, vk::ImageLayout layout,
                      const char* debugName, vk::Extent2D size);
    [[nodiscard]] static ImageRef imageRef(const Target& target, vk::Format format)
    {
        return ImageRef{.image = *target.image, .view = *target.view, .format = format};
    }
    void destroyTargets();

    const Device& device;
    const VkAllocator& allocator;
    FrameSync& frameSync;
    DescriptorManager& descriptorManager;

    vk::Extent2D extent{};
    vk::Extent2D displayExtent{};
    AttachmentImage depthAttachment;
    vk::Format depthFormat = vk::Format::eUndefined;
    Target visBuffer;
    vk::raii::ImageView visBufferView = nullptr;
    Target display;
    Target directRadiance;
    Target indirectRadiance;
    Target noisyColorTarget;
    Target upscaledTarget;
    Target postTarget;
    Target exposureTarget;
    std::array<std::array<Target, kGuideImageCount>, kGuideSetCount> guideSets;
    // sampled depth for the resolve and debug views
    uint32_t depthIndex = kNoneIndex;
    // sampler heap index; samplers are never freed
    uint32_t linearSamplerIndex = 0;
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> frameTargets;
};
