#pragma once

#include "vk_device.hpp"
#include "types.hpp"
#include "../Constants.h"
#include "vk_allocator.hpp"
#include "object_storage.hpp"
#include "../util/debug.hpp"
#include "../util/vk_tracy.hpp"
#include "../util/vk_utils.hpp"
#include "../static_headers/logger.hpp"
#include "vk_descriptors.hpp"
#include "ktxvulkan.h"
#include <filesystem>
#include <span>
#include <unordered_map>
#include <vector>

enum class TextureColorSpace : uint8_t
{
    Srgb = 0,
    Linear = 1,
};

// Loads textures into GPU images and registers SampledImage descriptors on the
// resource heap. Sampling state comes from the DescriptorManager sampler heap
// (not a VkSampler object).
class TextureManager {
public:
    explicit TextureManager(Device &deviceWrapper, const VkAllocator &allocator, DescriptorManager &descriptorManager);
    ~TextureManager();

    void init();

    // Format-detecting texture loader.
    // Inspects the file extension and routes to the KTX or
    // stb (PNG/etc.) pipeline accordingly.
    [[nodiscard]] uint32_t loadTexture(std::string texturePath);
    [[nodiscard]] uint32_t loadTexture(std::string texturePath, TextureColorSpace colorSpace);
    [[nodiscard]] uint32_t loadTextureFromMemory(std::string cacheKey, std::span<const uint8_t> bytes,
                                                 std::string_view mime, TextureColorSpace colorSpace);
    [[nodiscard]] uint32_t loadTextureFromPixels(std::string cacheKey, std::span<const uint8_t> rgba, uint32_t width,
                                                 uint32_t height, TextureColorSpace colorSpace);
    [[nodiscard]] uint32_t getOrCreateSampler(int32_t minFilter, int32_t magFilter, int32_t wrapS, int32_t wrapT);

    // Stable handles / cached data — direct access
    Device &deviceWrapper;
    const VkAllocator &allocator;
    DescriptorManager &descriptorManager;
    const vk::raii::PhysicalDevice &physicalDevice;
    const vk::raii::Device &device;
    const vk::raii::Queue &graphicsQueue;
    const vk::raii::Queue &transferQueue;
    uint32_t graphicsQueueFamilyIndex;
    uint32_t transferQueueFamilyIndex;

    std::unordered_map<std::string, TextureAsset> loadedTextures;
    std::vector<SamplerDesc> samplers;
    vk::raii::CommandPool commandPool = nullptr;
    vk::ImageViewCreateInfo textureImageViewCreateInfo;
    uint32_t mipLevels = 0;

private:
    // Resolve a path relative to the executable directory if it's a relative path
    [[nodiscard]] std::string resolvePath(std::string_view path);

    vk::ImageCreateInfo createImage(uint32_t width, uint32_t height, uint32_t mipLevelsIn, vk::Format format,
                     vk::ImageTiling tiling, vk::ImageUsageFlags usage, vk::MemoryPropertyFlags properties,
                     vk::raii::Image &image, VmaAllocation &imageMemory,
                     std::string_view memoryDebugBaseName = "TextureImageMemory");

    auto beginSingleTimeCommands(const vk::raii::Queue &queue) -> vk::raii::CommandBuffer;
    void endSingleTimeCommands(vk::raii::CommandBuffer &commandBuffer, const vk::raii::Queue &queue);
    void generateMipmaps(vk::raii::Image &image, vk::Format imageFormat, int32_t texWidth, int32_t texHeight,
                         uint32_t mipLevels);

    [[nodiscard]] uint32_t uploadRgba8(const std::string& cacheKey, const void* pixels, int texWidth, int texHeight,
                                       vk::Format format);
    std::unordered_map<uint64_t, uint32_t> samplerKeyToIndex;
};
