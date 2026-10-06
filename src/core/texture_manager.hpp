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
#include <memory>
#include <optional>
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
    struct StbFree {
        void operator()(unsigned char* pixels) const;
    };
    // stb RGBA8 pixels, freed on scope exit even when the upload throws
    using StbPixels = std::unique_ptr<unsigned char, StbFree>;

    // CPU-only decode result; pixels is null when decoding failed
    struct DecodedImage {
        StbPixels pixels;
        int width = 0;
        int height = 0;

        [[nodiscard]] std::span<const uint8_t> rgba() const
        {
            return {pixels.get(), static_cast<size_t>(width) * static_cast<size_t>(height) * 4u};
        }
    };

    explicit TextureManager(Device &deviceWrapper, const VkAllocator &allocator, DescriptorManager &descriptorManager);
    ~TextureManager();

    void init();

    // Format-detecting texture loader.
    // Inspects the file extension and routes to the KTX or
    // stb (PNG/etc.) pipeline accordingly.
    [[nodiscard]] uint32_t loadTexture(std::string texturePath, TextureColorSpace colorSpace = TextureColorSpace::Srgb);
    [[nodiscard]] uint32_t loadTextureFromMemory(std::string cacheKey, std::span<const uint8_t> bytes,
                                                 std::string_view mime, TextureColorSpace colorSpace);
    [[nodiscard]] uint32_t loadTextureFromPixels(std::string cacheKey, std::span<const uint8_t> rgba, uint32_t width,
                                                 uint32_t height, TextureColorSpace colorSpace);
    [[nodiscard]] uint32_t getOrCreateSampler(int32_t minFilter, int32_t magFilter, int32_t wrapS, int32_t wrapT);
    [[nodiscard]] bool isCached(std::string cacheKey, TextureColorSpace colorSpace) const;

    // Thread-safe decoders (no Vulkan); upload the result with loadTextureFromPixels
    [[nodiscard]] static DecodedImage decodeRgba8(std::span<const uint8_t> bytes);
    [[nodiscard]] static DecodedImage decodeRgba8File(const std::string& path);
    // RGBA8 bytes a decode would produce, read from the image header only; 0 when unreadable
    [[nodiscard]] static size_t decodedRgba8Bytes(std::span<const uint8_t> bytes);
    [[nodiscard]] static size_t decodedRgba8FileBytes(const std::string& path);
    [[nodiscard]] static bool isKtxPath(std::string_view path);

    // Uploads between begin and flush share one command buffer and one submit + fence wait.
    // Heap indices returned in between are valid, but the images are not sampleable until flush.
    void beginUploadBatch();
    void flushUploadBatch();

    // Stable handles / cached data — direct access
    Device &deviceWrapper;
    const VkAllocator &allocator;
    DescriptorManager &descriptorManager;
    const vk::raii::PhysicalDevice &physicalDevice;
    const vk::raii::Device &device;
    const vk::raii::Queue &graphicsQueue;
    uint32_t graphicsQueueFamilyIndex;

    std::unordered_map<std::string, TextureAsset> loadedTextures;
    vk::raii::CommandPool commandPool = nullptr;

private:
    // Resolve a relative path against the current working directory
    [[nodiscard]] std::string resolvePath(std::string_view path);
    [[nodiscard]] std::optional<uint32_t> cachedHeapIndex(const std::string &key) const;

    void generateMipmaps(vk::raii::CommandBuffer &commandBuffer, vk::raii::Image &image, int32_t texWidth,
                         int32_t texHeight, uint32_t mipLevels);

    [[nodiscard]] uint32_t uploadKtx(const std::string &path);
    // origin names the source in the decode error (file / mime type)
    [[nodiscard]] uint32_t uploadDecoded(const std::string &cacheKey, StbPixels pixels, int texWidth, int texHeight,
                                         TextureColorSpace colorSpace, std::string_view origin);
    [[nodiscard]] uint32_t uploadRgba8(const std::string& cacheKey, const void* pixels, int texWidth, int texHeight,
                                       vk::Format format);
    std::unordered_map<uint64_t, uint32_t> samplerKeyToIndex;
    // validated once in init() for both RGBA8 formats
    vk::ImageLayout hostCopyDstLayout = vk::ImageLayout::eUndefined;
    std::optional<vk::raii::CommandBuffer> uploadBatch;

    // libktx owns these images and their device memory (not VMA); destructed after the views
    std::vector<ktxVulkanTexture> ktxTextures;
    std::optional<ktxVulkanDeviceInfo> ktxDeviceInfo;
};
