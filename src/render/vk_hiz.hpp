#pragma once

#include "core/types.hpp"
#include "core/vk_descriptors.hpp"
#include "core/vk_allocator.hpp"
#include "core/vk_device.hpp"
#include "core/vk_frame_sync.hpp"
#include "vk_render_targets.hpp"
#include "vk_shader_objects.hpp"

#include <vector>
#include <vulkan/vulkan_raii.hpp>

// far-depth pyramid of the depth attachment (reverse Z: each texel holds the minimum depth of its footprint).
// R32_SFLOAT, previous power of two of the depth size, full mip chain, always in eGeneral
class HiZPyramid
{
public:
    HiZPyramid(const Device& device, const VkAllocator& allocator, FrameSync& frameSync,
               DescriptorManager& descriptorManager, const ShaderObjects& shaderObjects);
    ~HiZPyramid();

    HiZPyramid(const HiZPyramid&) = delete;
    HiZPyramid& operator=(const HiZPyramid&) = delete;
    HiZPyramid(HiZPyramid&&) = delete;
    HiZPyramid& operator=(HiZPyramid&&) = delete;

    // after RenderTargets::recreate, with the device idle; sized from and reads its depth attachment
    void recreate(const RenderTargets& targets);
    // outside a render pass, heaps bound, depth in eDepthStencilAttachmentOptimal after the raster pass;
    // leaves depth in eShaderReadOnlyOptimal. isFrozen: only that transition, the pyramid keeps its content
    void recordBuild(const vk::raii::CommandBuffer& cmd, bool isFrozen = false);

    [[nodiscard]] uint32_t pyramidHeapIndex() const noexcept { return pyramidIndex; }
    [[nodiscard]] uint32_t farSamplerHeapIndex() const noexcept { return farSamplerIndex; }
    [[nodiscard]] vk::Extent2D size() const noexcept { return extent; }
    [[nodiscard]] uint32_t mipCount() const noexcept { return static_cast<uint32_t>(mipIndices.size()); }
    // false from recreate until the first recorded build: the image holds no depth yet
    [[nodiscard]] bool hasContent() const noexcept { return isBuilt; }

private:
    void freeDescriptors();
    [[nodiscard]] vk::Extent2D mipExtent(uint32_t mip) const;

    const Device& device;
    const VkAllocator& allocator;
    FrameSync& frameSync;
    DescriptorManager& descriptorManager;
    const ShaderObjects& shaderObjects;

    vk::raii::Image image = nullptr;
    VmaAllocation memory = nullptr;
    // owned by RenderTargets; replaced only by recreate
    vk::Image depthImage;
    vk::Extent2D depthExtent{};
    vk::Extent2D extent{};

    // resource heap indices
    uint32_t depthIndex = kNoneIndex;
    // full chain, sampled through farSamplerIndex
    uint32_t pyramidIndex = kNoneIndex;
    // storage view per mip
    std::vector<uint32_t> mipIndices;
    // sampler heap index; samplers are never freed
    uint32_t farSamplerIndex = 0;
    bool isBuilt = false;
};
