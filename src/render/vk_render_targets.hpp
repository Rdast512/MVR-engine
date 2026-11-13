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
// attachment, the R32_UINT vis buffer, two guide sets by frame parity and the display image, plus one GpuTargets
// block per frame slot.
// Guides and the display image stay in eGeneral; the vis buffer is a color attachment while rastering and
// eShaderReadOnlyOptimal after
class RenderTargets
{
public:
    static constexpr vk::Format kVisBufferFormat = vk::Format::eR32Uint;
    static constexpr vk::Format kDisplayFormat = vk::Format::eR16G16B16A16Sfloat;

    RenderTargets(const Device& device, const VkAllocator& allocator, FrameSync& frameSync,
                  DescriptorManager& descriptorManager);
    ~RenderTargets();

    RenderTargets(const RenderTargets&) = delete;
    RenderTargets& operator=(const RenderTargets&) = delete;
    RenderTargets(RenderTargets&&) = delete;
    RenderTargets& operator=(RenderTargets&&) = delete;

    // device idle; depth first, every target at renderExtent
    void recreate(vk::Extent2D renderExtent);
    // writes the slot's GpuTargets with the guide set of frameNumber's parity
    void prepareFrame(uint32_t frameSlot, uint32_t frameNumber);

    // depth, vis buffer, guides and every culling size; equal to the swapchain extent until upscaling (M3)
    [[nodiscard]] vk::Extent2D getExtent() const noexcept { return extent; }
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
                      const char* debugName);
    void destroyTargets();

    const Device& device;
    const VkAllocator& allocator;
    FrameSync& frameSync;
    DescriptorManager& descriptorManager;

    vk::Extent2D extent{};
    AttachmentImage depthAttachment;
    vk::Format depthFormat = vk::Format::eUndefined;
    Target visBuffer;
    vk::raii::ImageView visBufferView = nullptr;
    Target display;
    std::array<std::array<Target, kGuideImageCount>, kGuideSetCount> guideSets;
    // sampled depth for the resolve and debug views
    uint32_t depthIndex = kNoneIndex;
    // sampler heap index; samplers are never freed
    uint32_t linearSamplerIndex = 0;
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> frameTargets;
};
