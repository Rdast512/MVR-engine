#pragma once

#include "../Constants.h"
#include "core/types.hpp"
#include "core/vk_allocator.hpp"
#include "core/vk_descriptors.hpp"
#include "core/vk_device.hpp"
#include "core/vk_frame_sync.hpp"
#include "util/vk_utils.hpp"

#include <array>
#include <utility>
#include <vulkan/vulkan_raii.hpp>

// render-resolution targets of the visibility-buffer path (spec/features/done/visibility_buffer.md §3.6): the depth
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
    // MaterialBin per pixel, written by passes/classify.slang
    static constexpr vk::Format kBinIdsFormat = vk::Format::eR8Uint;
    // smoothed ReSTIR GI stagnancy, written by passes/rtxdi_gi_shade.slang and read the next frame
    static constexpr vk::Format kGiStagnancyFormat = vk::Format::eR32Sfloat;
    // ReSTIR GI duplication map inputs and counts (passes/rtxdi_gi_duplication.slang)
    static constexpr vk::Format kGiSampleIdsFormat = vk::Format::eR32Uint;
    static constexpr vk::Format kGiDuplicationFormat = vk::Format::eR32Sfloat;
    // DLSS-RR pInSpecularHitDistance, as reference/vk_gltf_renderer's eDlssSpecularHitDist
    static constexpr vk::Format kSpecularHitDistanceFormat = vk::Format::eR16Sfloat;
    // the settings lab's converged DLSS-RR input: sum and count (m8 I16)
    static constexpr vk::Format kDlssInputAccumulationFormat = vk::Format::eR32G32B32A32Sfloat;
    // the Accumulation AA mode's running mean: 32-bit floats keep the low bits of a 1,024-frame mean
    static constexpr vk::Format kAccumulatedFormat = vk::Format::eR32G32B32A32Sfloat;

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
    // allocates the RGBA32F input accumulation target on first lab use and schedules a zero clear for the next frame
    void ensureDlssInputAccumulation();
    void requestDlssInputAccumulationClear() noexcept { isDlssInputAccumulationClearPending = true; }
    // records any pending lazy-target transition and zero clear before the frame's compute passes
    void recordPendingInit(vk::raii::CommandBuffer& cmd);
    // writes the slot's GpuTargets with the guide set of frameNumber's parity; isUpscaledPresented: present reads the
    // DLSS-RR output instead of the display image; optional lab/guide storage views stay kNoneIndex when unused
    void prepareFrame(uint32_t frameSlot, uint32_t frameNumber, bool isUpscaledPresented,
                      bool isSpecularHitDistanceWritten = false, bool isDlssInputAccumulated = false);

    // depth, vis buffer, guides and every culling size: the DLSS render resolution
    [[nodiscard]] vk::Extent2D getExtent() const noexcept { return extent; }
    [[nodiscard]] vk::Extent2D getDisplayExtent() const noexcept { return displayExtent; }
    // guide of the set frameNumber's parity selects
    [[nodiscard]] ImageRef guide(uint32_t frameNumber, GuideImage image) const;
    [[nodiscard]] ImageRef noisyColor() const { return imageRef(noisyColorTarget, kNoisyColorFormat); }
    [[nodiscard]] ImageRef specularHitDistance() const
    {
        return imageRef(specularHitDistanceTarget, kSpecularHitDistanceFormat);
    }
    // DLSS-RR's albedo guide blended with the glass layers (m8 I13 §4 variant b): DiffuseAlbedo or SpecAlbedo, written
    // by passes/glass.slang; the shared guides stay the surface's, since RTXDI reads them again next frame
    [[nodiscard]] ImageRef glassAlbedo(GuideImage image) const;
    // eShaderReadOnlyOptimal after the late hi-z build
    [[nodiscard]] ImageRef depth() const
    {
        return ImageRef{.image = *depthAttachment.image, .view = *depthAttachment.view, .format = depthFormat};
    }
    // display resolution, eGeneral
    [[nodiscard]] ImageRef upscaled() const { return imageRef(upscaledTarget, kUpscaledFormat); }
    // resource heap, sampled: the post inputs
    [[nodiscard]] uint32_t upscaledSampledIndex() const noexcept { return upscaledTarget.sampledIndex; }
    [[nodiscard]] uint32_t noisyColorSampledIndex() const noexcept { return noisyColorTarget.sampledIndex; }

    // Accumulation AA (spec/features/planned/nvidia_settings_layout.md §4.4): display resolution, eGeneral, created
    // only while the mode runs. Creation is safe between frames: it writes free heap slots only
    void ensureAccumulated();
    // no frame in flight may still read it
    void releaseAccumulated();
    [[nodiscard]] bool hasAccumulated() const noexcept { return accumulatedTarget.image != nullptr; }
    // true once after ensureAccumulated created it: the next pass transitions it from eUndefined
    [[nodiscard]] bool takeAccumulatedLayoutPending() noexcept { return std::exchange(isAccumulatedLayoutPending, false); }
    [[nodiscard]] vk::Image getAccumulatedImage() const noexcept { return *accumulatedTarget.image; }
    [[nodiscard]] uint32_t accumulatedStorageIndex() const noexcept { return accumulatedTarget.storageIndex; }
    [[nodiscard]] uint32_t accumulatedSampledIndex() const noexcept { return accumulatedTarget.sampledIndex; }
    // display resolution, eGeneral: tonemapped, what Lit presents
    [[nodiscard]] ImageRef post() const { return imageRef(postTarget, kPostFormat); }
    // the slot's GpuTargets, for the frame root
    [[nodiscard]] vk::DeviceAddress targetsAddress(uint32_t frameSlot) const noexcept
    {
        return frameTargets[frameSlot].address;
    }
    [[nodiscard]] vk::Image getDepthImage() const noexcept { return *depthAttachment.image; }
    [[nodiscard]] vk::ImageView getDepthView() const noexcept { return *depthAttachment.view; }
    [[nodiscard]] vk::Image getIndirectRadianceImage() const noexcept { return *indirectRadiance.image; }
    [[nodiscard]] vk::Format getDepthFormat() const noexcept { return depthFormat; }
    [[nodiscard]] vk::Image getVisBufferImage() const noexcept { return *visBuffer.image; }
    [[nodiscard]] vk::ImageView getVisBufferView() const noexcept { return *visBufferView; }
    // kDisplayFormat, eGeneral
    [[nodiscard]] vk::Image getDisplayImage() const noexcept { return *display.image; }
    // GpuMaterialBins + tile lists; written and read within one frame's graphics work, so one copy serves both slots
    [[nodiscard]] vk::DeviceAddress materialBinsAddress() const noexcept { return materialBinsDeviceAddress; }
    // TEMPORARY (considerations C-E1-3): a bin's tile list, as passes/classify.slang tileList() places it
    [[nodiscard]] vk::DeviceAddress materialTileListAddress(MaterialBin bin) const noexcept;

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
                      const char* debugName, vk::Extent2D size, bool isSampledInHeap = true);
    [[nodiscard]] static ImageRef imageRef(const Target& target, vk::Format format)
    {
        return ImageRef{.image = *target.image, .view = *target.view, .format = format};
    }
    void destroyTargets();
    void createMaterialBins();

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
    // DLSS-only albedo guides with glass (m8 I13 §4 variant b)
    Target glassDiffuseAlbedo;
    Target glassSpecAlbedo;
    Target upscaledTarget;
    Target postTarget;
    Target exposureTarget;
    Target binIds;
    vk::raii::Buffer materialBins = nullptr;
    VmaAllocation materialBinsMemory = nullptr;
    vk::DeviceAddress materialBinsDeviceAddress = 0;
    std::array<std::array<Target, kGuideImageCount>, kGuideSetCount> guideSets;
    // by frame parity, as the guide sets
    std::array<Target, kGuideSetCount> giStagnancy;
    // one copy: each is read before the frame's next write to it
    Target giSampleIds;
    Target giDuplication;
    Target specularHitDistanceTarget;
    Target dlssInputAccumulation;
    Target accumulatedTarget;
    bool isAccumulatedLayoutPending = false;
    bool isDlssInputAccumulationInitialized = false;
    bool isDlssInputAccumulationClearPending = false;
    // sampled depth for the resolve and debug views
    uint32_t depthIndex = kNoneIndex;
    // sampler heap index; samplers are never freed
    uint32_t linearSamplerIndex = 0;
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> frameTargets;
};
