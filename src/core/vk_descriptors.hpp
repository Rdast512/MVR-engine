#pragma once
#include <vulkan/vulkan_raii.hpp>

#include "types.hpp"

class DescriptorManager
{
    void createHeaps();
    void createHeapDescriptors();
    void createHeapBuffers(vk::DeviceSize resourceHeapSize, vk::DeviceSize samplerHeapSize);
    vk::DeviceSize minResourceHeapReservedRange = 0;
    vk::DeviceSize minSamplerHeapReservedRange = 0;

    vk::DeviceSize bufferDescriptorSize = 0;
    vk::DeviceSize samplerDescriptorSize = 0;
    vk::DeviceSize imageDescriptorSize = 0;

    vk::DeviceSize bufferDescriptorAlignment = 0;
    vk::DeviceSize samplerDescriptorAlignment = 0;
    vk::DeviceSize imageDescriptorAlignment = 0;

public:
    DescriptorManager(const vk::raii::Device& device, VmaAllocator allocator,
                      const std::vector<uint32_t>& queueFamilyIndices,
                      const HardwareCapabilities& capabilities);

    ~DescriptorManager();


    void init();

    // Computed indices (not raw field passthrough)
    [[nodiscard]] auto getTextureDescriptorIndex() const -> uint32_t;
    [[nodiscard]] auto getSamplerDescriptorIndex() const -> uint32_t;
    void writeImageDescriptor(TextureAsset& textureAsset, const vk::ImageViewCreateInfo& imageViewCreateInfo);
    [[nodiscard]] uint32_t writeSamplerDescriptor(const vk::SamplerCreateInfo& samplerInfo);



    const vk::raii::Device& device;
    VmaAllocator allocator;
    const std::vector<uint32_t>& queueFamilyIndices;
    const HardwareCapabilities& capabilities;
    DescriptorBindingMode descriptorBindingMode = DescriptorBindingMode::DescriptorHeaps;


    vk::raii::Buffer resourceHeapBuffer = nullptr;
    vk::raii::Buffer samplerHeapBuffer = nullptr;

    VmaAllocation resourceHeapMemory = nullptr;
    VmaAllocation samplerHeapMemory = nullptr;

    void* mappedResourceHeapPtr = nullptr;
    void* mappedSamplerHeapPtr = nullptr;

    vk::BindHeapInfoEXT resourceHeapInfo{};
    vk::BindHeapInfoEXT samplerHeapInfo{};

    vk::DeviceSize textureDescriptorOffset = 0;
    vk::DeviceSize samplerDescriptorOffset = 0;
    vk::DeviceSize nextSamplerDescriptorOffset = 0;
    uint32_t defaultSamplerHeapIndex = 0;
    vk::raii::DescriptorSetLayout descriptorSetLayout = nullptr;
    vk::raii::DescriptorPool descriptorPool = nullptr;
    std::vector<vk::raii::DescriptorSet> descriptorSets;
};
