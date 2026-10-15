#include "vk_descriptors.hpp"

#include "../Constants.h"
#include "../core/types.hpp"
#include "../util/debug.hpp"
#include "../util/vk_tracy.hpp"
#include "../util/vk_utils.hpp"
#include "logger.hpp"

[[nodiscard]] static constexpr vk::DeviceSize alignUp(vk::DeviceSize size, vk::DeviceSize alignment)
{
    return (size + alignment - 1) & ~(alignment - 1);
}

DescriptorManager::DescriptorManager(const vk::raii::Device& device, VmaAllocator allocator,
                                     const std::vector<uint32_t>& queueFamilyIndices,
                                     const HardwareCapabilities& capabilities) :
    device(device), allocator(allocator), queueFamilyIndices(queueFamilyIndices),
    capabilities(capabilities)
{
    minResourceHeapReservedRange = capabilities.descriptorHeap.minResourceHeapReservedRange;
    minSamplerHeapReservedRange = capabilities.descriptorHeap.minSamplerHeapReservedRange;

    bufferDescriptorSize = capabilities.descriptorHeap.bufferDescriptorSize;
    samplerDescriptorSize = capabilities.descriptorHeap.samplerDescriptorSize;
    imageDescriptorSize = capabilities.descriptorHeap.imageDescriptorSize;

    bufferDescriptorAlignment = capabilities.descriptorHeap.bufferDescriptorAlignment;
    samplerDescriptorAlignment = capabilities.descriptorHeap.samplerDescriptorAlignment;
    imageDescriptorAlignment = capabilities.descriptorHeap.imageDescriptorAlignment;
}

DescriptorManager::~DescriptorManager()
{
    ZoneScopedN("DescriptorManager::~DescriptorManager");
    if (mappedResourceHeapPtr != nullptr && resourceHeapMemory != nullptr) {
        vmaUnmapMemory(allocator, resourceHeapMemory);
        mappedResourceHeapPtr = nullptr;
    }
    if (mappedSamplerHeapPtr != nullptr && samplerHeapMemory != nullptr) {
        vmaUnmapMemory(allocator, samplerHeapMemory);
        mappedSamplerHeapPtr = nullptr;
    }

    destroyVmaBuffer(allocator, resourceHeapBuffer, resourceHeapMemory);
    destroyVmaBuffer(allocator, samplerHeapBuffer, samplerHeapMemory);
}

void DescriptorManager::init()
{
    ZoneScopedN("DescriptorManager::init");
    createHeaps();
    createHeapDescriptors();
}


void DescriptorManager::createHeaps()
{
    ZoneScopedN("DescriptorManager::createHeaps");

    if (samplerDescriptorSize == 0 || imageDescriptorSize == 0 || samplerDescriptorAlignment == 0 ||
        imageDescriptorAlignment == 0) {
        throw std::runtime_error("Descriptor heap properties are invalid for heap mode");
    }

    const vk::DeviceSize resourceHeapSize = capabilities.descriptorHeap.maxResourceHeapSize;
    logInfo(std::format("Creating resource descriptor heap: size={} ({:.1f} MiB) imageDesc={} reserve={}  "
                         "[host-visible mapped — counts as process RAM]",
                         resourceHeapSize, static_cast<double>(resourceHeapSize) / (1024.0 * 1024.0),
                         imageDescriptorSize, minResourceHeapReservedRange),
             "DescriptorManager");

    const vk::DeviceSize samplerHeapSize = capabilities.descriptorHeap.maxSamplerHeapSize;
    logInfo(std::format("Creating sampler descriptor heap: size={} ({:.1f} MiB) samplerDesc={} reserve={}  "
                         "[host-visible mapped — counts as process RAM]",
                         samplerHeapSize, static_cast<double>(samplerHeapSize) / (1024.0 * 1024.0),
                         samplerDescriptorSize, minSamplerHeapReservedRange),
             "DescriptorManager");

    createHeapBuffers(resourceHeapSize, samplerHeapSize);
}

void DescriptorManager::writeImageDescriptor(TextureAsset& textureAsset, const vk::ImageViewCreateInfo& imageViewCreateInfo)
{
    // size is the array stride; spec guarantees imageDescriptorAlignment <= imageDescriptorSize
    const bool reuseSlot = !freeImageSlots.empty();
    const vk::DeviceSize currentResOffset = reuseSlot
        ? static_cast<vk::DeviceSize>(freeImageSlots.back()) * imageDescriptorSize
        : alignUp(textureDescriptorOffset, imageDescriptorAlignment);
    if (reuseSlot) {
        freeImageSlots.pop_back();
    }
    const vk::ImageDescriptorInfoEXT descriptorImageInfo{
        .pView = &imageViewCreateInfo,
        .layout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::ResourceDescriptorInfoEXT imageInfo{
        .type = vk::DescriptorType::eSampledImage,
        .data = vk::ResourceDescriptorDataEXT{&descriptorImageInfo},
    };
    const vk::HostAddressRangeEXT imageWrite{
        .address = static_cast<uint8_t*>(mappedResourceHeapPtr) + currentResOffset,
        .size = imageDescriptorSize,
    };

    {
        ZoneScopedN("DescriptorManager::createHeapDescriptors::writeResourceDescriptorsEXT");
        device.writeResourceDescriptorsEXT(imageInfo, imageWrite);
    }

    // next texture needs a fresh slot
    if (!reuseSlot) {
        textureDescriptorOffset = alignUp(currentResOffset + imageDescriptorSize, imageDescriptorAlignment);
    }

    const uint32_t heapIndex = static_cast<uint32_t>(currentResOffset / imageDescriptorSize);
    textureAsset.descriptorHeapIndex = heapIndex;

    logInfo(std::format("Descriptor heap image write: offset={} size={} align={} index={} nextOffset={}",
                         currentResOffset, imageDescriptorSize, imageDescriptorAlignment, heapIndex,
                         textureDescriptorOffset),
             "DescriptorHeap");
}


void DescriptorManager::freeImageDescriptor(uint32_t heapIndex)
{
    freeImageSlots.push_back(heapIndex);
}

// default linear/anisotropic sampler, written straight into the sampler heap
void DescriptorManager::createHeapDescriptors()
{
    ZoneScopedN("DescriptorManager::createHeapDescriptors");

    const vk::DeviceSize currentSampOffset = alignUp(0, samplerDescriptorAlignment);
    samplerDescriptorOffset = currentSampOffset;

    const auto maxSamplerAnisotropy = capabilities.properties2.properties.limits.maxSamplerAnisotropy;

    // maxLod unclamped for full mip chains
    const vk::SamplerCreateInfo samplerInfo{.magFilter = vk::Filter::eLinear,
                                            .minFilter = vk::Filter::eLinear,
                                            .mipmapMode = vk::SamplerMipmapMode::eLinear,
                                            .addressModeU = vk::SamplerAddressMode::eRepeat,
                                            .addressModeV = vk::SamplerAddressMode::eRepeat,
                                            .addressModeW = vk::SamplerAddressMode::eRepeat,
                                            .mipLodBias = 0.0f,
                                            .anisotropyEnable = vk::True,
                                            .maxAnisotropy = maxSamplerAnisotropy,
                                            .compareEnable = vk::False,
                                            .compareOp = vk::CompareOp::eAlways,
                                            .minLod = 0.0f,
                                            .maxLod = vk::LodClampNone};
    const vk::HostAddressRangeEXT samplerWrite{
        .address = static_cast<uint8_t*>(mappedSamplerHeapPtr) + currentSampOffset,
        .size = samplerDescriptorSize,
    };
    {
        ZoneScopedN("DescriptorManager::createHeapDescriptors::writeSamplerDescriptorsEXT");
        device.writeSamplerDescriptorsEXT(samplerInfo, samplerWrite);
    }

    defaultSamplerHeapIndex = static_cast<uint32_t>(currentSampOffset / samplerDescriptorSize);
    nextSamplerDescriptorOffset = alignUp(currentSampOffset + samplerDescriptorSize, samplerDescriptorAlignment);

    logInfo(std::format("Descriptor heap sampler layout samplerDescSize={} "
                         "samplerAlign={} samplerIndex={}",
                         samplerDescriptorSize,
                         samplerDescriptorAlignment, getSamplerDescriptorIndex()), "DescriptorHeap");
}

uint32_t DescriptorManager::writeSamplerDescriptor(const vk::SamplerCreateInfo& samplerInfo)
{
    const vk::DeviceSize currentSampOffset = alignUp(nextSamplerDescriptorOffset, samplerDescriptorAlignment);
    const vk::HostAddressRangeEXT samplerWrite{
        .address = static_cast<uint8_t*>(mappedSamplerHeapPtr) + currentSampOffset,
        .size = samplerDescriptorSize,
    };
    {
        ZoneScopedN("DescriptorManager::writeSamplerDescriptor");
        device.writeSamplerDescriptorsEXT(samplerInfo, samplerWrite);
    }

    nextSamplerDescriptorOffset = alignUp(currentSampOffset + samplerDescriptorSize, samplerDescriptorAlignment);

    const uint32_t heapIndex = static_cast<uint32_t>(currentSampOffset / samplerDescriptorSize);
    logInfo(std::format("Descriptor heap sampler write: offset={} index={}", currentSampOffset, heapIndex),
             "DescriptorHeap");
    return heapIndex;
}

void DescriptorManager::createHeapBuffers(vk::DeviceSize resourceHeapSize, vk::DeviceSize samplerHeapSize)
{
    ZoneScopedN("DescriptorManager::createHeapBuffers");
    createBuffer(
        resourceHeapSize,
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress |
            vk::BufferUsageFlagBits2::eDescriptorHeapEXT,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, resourceHeapBuffer,
        resourceHeapMemory, allocator, device,
        queueFamilyIndices, "DescriptorHeapResourceMemory",
        VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);
    const VkResult resourceHeapMapResult =
        vmaMapMemory(allocator, resourceHeapMemory, &mappedResourceHeapPtr);
    if (resourceHeapMapResult != VK_SUCCESS || mappedResourceHeapPtr == nullptr) {
        throw std::runtime_error(std::format("Failed to map resource descriptor heap memory (VkResult={})",
                                             static_cast<int>(resourceHeapMapResult)));
    }
    setDebugName(device, resourceHeapBuffer, "DescriptorHeap_Resource");

    const VkBufferDeviceAddressInfo resourceHeapAddressInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .pNext = nullptr,
        .buffer = *resourceHeapBuffer,
    };
    const VkDeviceAddress resourceHeapAddress = vkGetBufferDeviceAddress(*device, &resourceHeapAddressInfo);
    resourceHeapInfo = vk::BindHeapInfoEXT{
        .heapRange = vk::DeviceAddressRangeEXT{.address = resourceHeapAddress, .size = resourceHeapSize},
        .reservedRangeOffset = resourceHeapSize - minResourceHeapReservedRange,
        .reservedRangeSize = minResourceHeapReservedRange,
    };
    logInfo(std::format("Resource heap GPU address=0x{:016x}, reservedOffset={}, reservedSize={}", resourceHeapAddress,
                         resourceHeapInfo.reservedRangeOffset, resourceHeapInfo.reservedRangeSize), "DescriptorManager");

    createBuffer(
        samplerHeapSize,
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress |
            vk::BufferUsageFlagBits2::eDescriptorHeapEXT,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, samplerHeapBuffer,
        samplerHeapMemory, allocator, device,
        queueFamilyIndices, "DescriptorHeapSamplerMemory",
        VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);
    const VkResult samplerHeapMapResult =
        vmaMapMemory(allocator, samplerHeapMemory, &mappedSamplerHeapPtr);
    if (samplerHeapMapResult != VK_SUCCESS || mappedSamplerHeapPtr == nullptr) {
        throw std::runtime_error(std::format("Failed to map sampler descriptor heap memory (VkResult={})",
                                             static_cast<int>(samplerHeapMapResult)));
    }
    setDebugName(device, samplerHeapBuffer, "DescriptorHeap_Sampler");

    const VkBufferDeviceAddressInfo samplerHeapAddressInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .pNext = nullptr,
        .buffer = *samplerHeapBuffer,
    };
    const VkDeviceAddress samplerHeapAddress = vkGetBufferDeviceAddress(*device, &samplerHeapAddressInfo);
    samplerHeapInfo = vk::BindHeapInfoEXT{
        .heapRange = vk::DeviceAddressRangeEXT{.address = samplerHeapAddress, .size = samplerHeapSize},
        .reservedRangeOffset = samplerHeapSize - minSamplerHeapReservedRange,
        .reservedRangeSize = minSamplerHeapReservedRange,
    };
    logInfo(std::format("Sampler heap GPU address=0x{:016x}, reservedOffset={}, reservedSize={}", samplerHeapAddress,
                         samplerHeapInfo.reservedRangeOffset, samplerHeapInfo.reservedRangeSize), "DescriptorManager");
}


uint32_t DescriptorManager::getTextureDescriptorIndex() const
{
    if (imageDescriptorSize == 0) {
        return 0;
    }
    return static_cast<uint32_t>(textureDescriptorOffset / imageDescriptorSize);
}

uint32_t DescriptorManager::getSamplerDescriptorIndex() const
{
    return defaultSamplerHeapIndex;
}
