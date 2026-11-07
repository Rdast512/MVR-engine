#pragma once

#include "../Constants.h"
#include "core/types.hpp"
#include "core/vk_descriptors.hpp"
#include "core/vk_device.hpp"
#include "core/vk_resource_manager.hpp"

#include <array>
#include <vulkan/vulkan_raii.hpp>

// render-resolution targets of the visibility-buffer path (spec/features/visibility_buffer.md §3.6): the R32_UINT
// vis buffer, two guide sets by frame parity and the display image, plus one GpuTargets block per frame slot.
// Guides and the display image stay in eGeneral; the vis buffer is a color attachment while rastering and
// eShaderReadOnlyOptimal after
class RenderTargets
{
public:
    static constexpr vk::Format kVisBufferFormat = vk::Format::eR32Uint;
    static constexpr vk::Format kDisplayFormat = vk::Format::eR16G16B16A16Sfloat;

    RenderTargets(const Device& device, ResourceManager& resourceManager, DescriptorManager& descriptorManager);
    ~RenderTargets();

    RenderTargets(const RenderTargets&) = delete;
    RenderTargets& operator=(const RenderTargets&) = delete;
    RenderTargets(RenderTargets&&) = delete;
    RenderTargets& operator=(RenderTargets&&) = delete;

    // device idle; after ResourceManager::createDepthResources at the same extent
    void recreate(vk::Extent2D renderExtent);
    // writes the slot's GpuTargets with the guide set of frameNumber's parity and hands its address to the frame root
    void prepareFrame(uint32_t frameSlot, uint32_t frameNumber);

    [[nodiscard]] vk::Extent2D getExtent() const noexcept { return extent; }
    [[nodiscard]] vk::Image getVisBufferImage() const noexcept { return *visBuffer.image; }
    [[nodiscard]] vk::ImageView getVisBufferView() const noexcept { return *visBufferView; }

private:
    struct Target
    {
        vk::raii::Image image = nullptr;
        VmaAllocation memory = nullptr;
        uint32_t storageIndex = kNoneIndex;
        uint32_t sampledIndex = kNoneIndex;
    };

    void checkFormatSupport() const;
    void createTarget(Target& target, vk::Format format, vk::ImageUsageFlags usage, vk::ImageLayout layout,
                      const char* debugName);
    void destroyTargets();

    const Device& device;
    ResourceManager& resourceManager;
    DescriptorManager& descriptorManager;

    vk::Extent2D extent{};
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
