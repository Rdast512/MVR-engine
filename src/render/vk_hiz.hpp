#pragma once

#include "core/types.hpp"
#include "core/vk_descriptors.hpp"
#include "core/vk_device.hpp"
#include "core/vk_resource_manager.hpp"
#include "vk_shader_objects.hpp"

#include <vector>
#include <vulkan/vulkan_raii.hpp>

// max-depth pyramid of the depth attachment (standard Z: each texel holds the farthest depth of its footprint).
// R32_SFLOAT, previous power of two of the depth size, full mip chain, always in eGeneral
class HiZPyramid
{
public:
    HiZPyramid(const Device& device, ResourceManager& resourceManager, DescriptorManager& descriptorManager,
               const ShaderObjects& shaderObjects);
    ~HiZPyramid();

    HiZPyramid(const HiZPyramid&) = delete;
    HiZPyramid& operator=(const HiZPyramid&) = delete;
    HiZPyramid(HiZPyramid&&) = delete;
    HiZPyramid& operator=(HiZPyramid&&) = delete;

    // after ResourceManager::createDepthResources, with the device idle
    void recreate();
    // outside a render pass, heaps bound, depth in eDepthStencilAttachmentOptimal after the raster pass;
    // leaves depth in eShaderReadOnlyOptimal. isFrozen: only that transition, the pyramid keeps its content
    void recordBuild(const vk::raii::CommandBuffer& cmd, bool isFrozen = false);

    [[nodiscard]] uint32_t pyramidHeapIndex() const noexcept { return pyramidIndex; }
    [[nodiscard]] uint32_t maxSamplerHeapIndex() const noexcept { return maxSamplerIndex; }
    [[nodiscard]] vk::Extent2D size() const noexcept { return extent; }
    [[nodiscard]] uint32_t mipCount() const noexcept { return static_cast<uint32_t>(mipIndices.size()); }
    // false from recreate until the first recorded build: the image holds no depth yet
    [[nodiscard]] bool hasContent() const noexcept { return isBuilt; }

private:
    void freeDescriptors();
    [[nodiscard]] vk::Extent2D mipExtent(uint32_t mip) const;

    const Device& device;
    ResourceManager& resourceManager;
    DescriptorManager& descriptorManager;
    const ShaderObjects& shaderObjects;

    vk::raii::Image image = nullptr;
    VmaAllocation memory = nullptr;
    vk::Extent2D depthExtent{};
    vk::Extent2D extent{};

    // resource heap indices
    uint32_t depthIndex = kNoneIndex;
    // full chain, sampled through maxSamplerIndex
    uint32_t pyramidIndex = kNoneIndex;
    // storage view per mip
    std::vector<uint32_t> mipIndices;
    // sampler heap index; samplers are never freed
    uint32_t maxSamplerIndex = 0;
    bool isBuilt = false;
};
