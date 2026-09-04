#include "texture_manager.hpp"


TextureManager::TextureManager(Device& deviceWrapper, const VkAllocator& allocator,
                               DescriptorManager& descriptorManager) :
    deviceWrapper(deviceWrapper), physicalDevice(deviceWrapper.physicalDevice), device(deviceWrapper.vkdevice),
    graphicsQueue(deviceWrapper.graphicsQueue), transferQueue(deviceWrapper.transferQueue),
    graphicsQueueFamilyIndex(deviceWrapper.graphicsIndex), transferQueueFamilyIndex(deviceWrapper.transferIndex),
    allocator(allocator), descriptorManager(descriptorManager)
{
    log_info("Constructor started", "TextureManager");
    vk::CommandPoolCreateInfo poolInfo{.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                       .queueFamilyIndex = graphicsQueueFamilyIndex};
    commandPool = vk::raii::CommandPool(device, poolInfo);
}

std::string TextureManager::resolvePath(std::string_view path)
{
    std::filesystem::path fsPath(path);

    if (fsPath.is_absolute()) {
        return fsPath.string();
    }

    // resolve relative to cwd
    std::filesystem::path resolved = std::filesystem::current_path() / fsPath;

    if (std::filesystem::exists(resolved)) {
        log_info(std::format("Resolved path: {} -> {}", path, resolved.string()), "TextureManager");
        return resolved.string();
    }

    log_info(std::format("Path not found relative to CWD: {}; trying original", path), "TextureManager");
    return std::string(path);
}

TextureManager::~TextureManager()
{
    ZoneScopedN("TextureManager::~TextureManager");
    log_info("Destructor called", "TextureManager");

    for (auto& [path, asset] : loadedTextures) {
        if (asset.textureImageMemory != nullptr) {
            VkImage raw = asset.textureImage.release();
            tracyResourceFree(raw, "GPU/Textures");
            vmaDestroyImage(allocator.allocator, raw, asset.textureImageMemory);
            asset.textureImageMemory = nullptr;
        }
    }
    loadedTextures.clear();
    log_info("Resources destroyed", "TextureManager");
}

// ── format-detecting texture loader ─────────────────────────

namespace
{

    enum class TextureFormat
    {
        Ktx,
        Png,
        Unknown
    };

    TextureFormat detectFormat(std::string_view path)
    {
        const auto dot = path.rfind('.');
        if (dot == std::string_view::npos)
            return TextureFormat::Unknown;

        const std::string_view ext = path.substr(dot);
        if (ext == ".ktx" || ext == ".ktx2")
            return TextureFormat::Ktx;
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp")
            return TextureFormat::Png;
        return TextureFormat::Unknown;
    }

    bool containsImageLayout(const std::vector<vk::ImageLayout>& layouts, vk::ImageLayout layout)
    {
        return std::ranges::find(layouts, layout) != layouts.end();
    }

    // HOST_TRANSFER makes GENERAL as cheap as OPTIMAL for host copies / later sample.
    // other host-transition dests (TRANSFER_DST, SHADER_READ_ONLY) add no benefit.
    vk::ImageLayout stbHostCopyDstLayout(const vk::raii::PhysicalDevice& physicalDevice,
                                         const HardwareCapabilities& capabilities, vk::Format format)
    {
        const auto formatChain =
            physicalDevice.getFormatProperties2<vk::FormatProperties2, vk::FormatProperties3>(format);
        if (!(formatChain.get<vk::FormatProperties3>().optimalTilingFeatures &
              vk::FormatFeatureFlagBits2::eHostImageTransfer)) {
            throw std::runtime_error("RGBA8 format lacks HOST_IMAGE_TRANSFER");
        }

        if (!containsImageLayout(capabilities.hostImageCopyDstLayouts, vk::ImageLayout::eGeneral)) {
            throw std::runtime_error("host image copy dest layouts missing GENERAL");
        }

        const vk::PhysicalDeviceImageFormatInfo2 formatInfo{
            .format = format,
            .type = vk::ImageType::e2D,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eHostTransfer | vk::ImageUsageFlagBits::eTransferSrc |
                vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
        };
        const auto perfChain =
            physicalDevice
                .getImageFormatProperties2<vk::ImageFormatProperties2, vk::HostImageCopyDevicePerformanceQuery>(
                    formatInfo);
        const auto& perf = perfChain.get<vk::HostImageCopyDevicePerformanceQuery>();
        static bool loggedPerf = false;
        if (!loggedPerf) {
            log_info(std::format("hostImageCopy perf: optimalDeviceAccess={} identicalMemoryLayout={}",
                                 static_cast<bool>(perf.optimalDeviceAccess),
                                 static_cast<bool>(perf.identicalMemoryLayout)),
                     "TextureManager");
            if (!perf.optimalDeviceAccess) {
                log_info("HOST_TRANSFER may be slower to sample than a non-host-transfer image", "TextureManager");
            }
            loggedPerf = true;
        }

        return vk::ImageLayout::eGeneral;
    }

    vk::Format rgbaFormat(TextureColorSpace colorSpace)
    {
        return colorSpace == TextureColorSpace::Srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
    }

    const char* colorSpaceSuffix(TextureColorSpace colorSpace)
    {
        return colorSpace == TextureColorSpace::Srgb ? "|srgb" : "|linear";
    }

    uint64_t packSamplerKey(int32_t minFilter, int32_t magFilter, int32_t wrapS, int32_t wrapT)
    {
        const auto u16 = [](int32_t v) { return static_cast<uint16_t>(v); };
        return static_cast<uint64_t>(u16(minFilter)) | (static_cast<uint64_t>(u16(magFilter)) << 16) |
            (static_cast<uint64_t>(u16(wrapS)) << 32) | (static_cast<uint64_t>(u16(wrapT)) << 48);
    }

    vk::SamplerAddressMode wrapToVk(int32_t wrap)
    {
        switch (wrap) {
        case 33071:
            return vk::SamplerAddressMode::eClampToEdge;
        case 33648:
            return vk::SamplerAddressMode::eMirroredRepeat;
        default:
            return vk::SamplerAddressMode::eRepeat;
        }
    }

    vk::SamplerCreateInfo samplerInfoFromGltf(int32_t minFilter, int32_t magFilter, int32_t wrapS, int32_t wrapT,
                                              float maxSamplerAnisotropy)
    {
        const vk::Filter mag = magFilter == 9728 ? vk::Filter::eNearest : vk::Filter::eLinear;
        vk::Filter min = vk::Filter::eLinear;
        vk::SamplerMipmapMode mip = vk::SamplerMipmapMode::eLinear;
        float maxLod = vk::LodClampNone;
        switch (minFilter) {
        case 9728:
            min = vk::Filter::eNearest;
            mip = vk::SamplerMipmapMode::eNearest;
            maxLod = 0.25f;
            break;
        case 9729:
            min = vk::Filter::eLinear;
            mip = vk::SamplerMipmapMode::eNearest;
            maxLod = 0.25f;
            break;
        case 9984:
            min = vk::Filter::eNearest;
            mip = vk::SamplerMipmapMode::eNearest;
            break;
        case 9985:
            min = vk::Filter::eLinear;
            mip = vk::SamplerMipmapMode::eNearest;
            break;
        case 9986:
            min = vk::Filter::eNearest;
            mip = vk::SamplerMipmapMode::eLinear;
            break;
        default:
            min = vk::Filter::eLinear;
            mip = vk::SamplerMipmapMode::eLinear;
            break;
        }

        const vk::SamplerAddressMode addressS = wrapToVk(wrapS);
        const vk::SamplerAddressMode addressT = wrapToVk(wrapT);
        const bool anisotropy =
            mag == vk::Filter::eLinear && min == vk::Filter::eLinear && mip == vk::SamplerMipmapMode::eLinear;

        return vk::SamplerCreateInfo{
            .magFilter = mag,
            .minFilter = min,
            .mipmapMode = mip,
            .addressModeU = addressS,
            .addressModeV = addressT,
            .addressModeW = vk::SamplerAddressMode::eRepeat,
            .mipLodBias = 0.0f,
            .anisotropyEnable = anisotropy ? vk::True : vk::False,
            .maxAnisotropy = anisotropy ? maxSamplerAnisotropy : 1.0f,
            .compareEnable = vk::False,
            .compareOp = vk::CompareOp::eAlways,
            .minLod = 0.0f,
            .maxLod = maxLod,
        };
    }

} // anonymous namespace

void TextureManager::init()
{
    ZoneScopedN("TextureManager::init");
    log_info("init() started", "TextureManager");
    const SamplerDesc def{
        .minFilter = 9987,
        .magFilter = 9729,
        .wrapS = 10497,
        .wrapT = 10497,
        .heapIndex = descriptorManager.getSamplerDescriptorIndex(),
    };
    samplers.push_back(def);
    samplerKeyToIndex[packSamplerKey(def.minFilter, def.magFilter, def.wrapS, def.wrapT)] = def.heapIndex;
    log_info("Initialized", "TextureManager");
}

uint32_t TextureManager::loadTexture(std::string texturePath)
{
    return loadTexture(std::move(texturePath), TextureColorSpace::Srgb);
}

uint32_t TextureManager::loadTexture(std::string texturePath, TextureColorSpace colorSpace)
{
    ZoneScopedN("TextureManager::loadTexture");
    const std::string path = resolvePath(texturePath);
    const std::string cacheKey = path + colorSpaceSuffix(colorSpace);
    log_info(std::format("loadTexture() started for {}", path), "TextureManager");
    if (loadedTextures.find(cacheKey) != loadedTextures.end()) {
        log_info(std::format("Texture already loaded: {}", cacheKey), "TextureManager");
        return loadedTextures[cacheKey].descriptorHeapIndex;
    }
    if (loadedTextures.find(path) != loadedTextures.end() && detectFormat(path) == TextureFormat::Ktx) {
        return loadedTextures[path].descriptorHeapIndex;
    }

    const TextureFormat fmt = detectFormat(path);
    log_info(std::format("loadTexture: {} → {}", path,
                         fmt == TextureFormat::Ktx       ? "KTX/KTX2"
                             : fmt == TextureFormat::Png ? "PNG/STB"
                                                         : "Unknown"),
             "TextureManager");

    // ── KTX / KTX2 path ──────────────────────────────────
    if (fmt == TextureFormat::Ktx) {
        // initialize KTX device info
        ktxVulkanDeviceInfo vdi{};
        const KTX_error_code ctorRes =
            ktxVulkanDeviceInfo_Construct(&vdi, *physicalDevice, *device, *graphicsQueue, *commandPool,
                                          nullptr); // VkAllocationCallbacks

        if (ctorRes != KTX_SUCCESS) {
            throw std::runtime_error("ktxVulkanDeviceInfo_Construct failed");
        }

        // load KTX file
        ktxTexture* kTexture = nullptr;
        KTX_error_code result =
            ktxTexture_CreateFromNamedFile(path.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &kTexture);

        if (result != KTX_SUCCESS || !kTexture) {
            ktxVulkanDeviceInfo_Destruct(&vdi);
            throw std::runtime_error("Failed to load KTX texture: " + path);
        }

        // upload to GPU
        ktxVulkanTexture vkTex{};
        result = ktxTexture_VkUpload(kTexture, &vdi, &vkTex);

        ktxTexture_Destroy(kTexture);
        ktxVulkanDeviceInfo_Destruct(&vdi);

        if (result != KTX_SUCCESS) {
            throw std::runtime_error("Failed to upload KTX texture to GPU: " + path);
        }

        const VkFormat vkFormat = vkTex.imageFormat;
        const uint32_t width = vkTex.width;
        const uint32_t height = vkTex.height;
        const uint32_t levels = vkTex.levelCount;

        log_info(std::format("KTX texture uploaded: {}×{}, {} mips, format={}", width, height, levels,
                             static_cast<uint32_t>(vkFormat)),
                 "TextureManager");

        // non-owning image view wrapper
        TextureAsset asset{};
        vk::ImageViewCreateInfo const viewInfo{.image = vk::Image(vkTex.image), // non-owning wrapper
                                               .viewType = static_cast<vk::ImageViewType>(vkTex.viewType),
                                               .format = static_cast<vk::Format>(vkFormat),
                                               .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, levels, 0, 1}};
        asset.textureImageView = vk::raii::ImageView(device, viewInfo);

        descriptorManager.writeImageDescriptor(asset, viewInfo);

        // cache loaded texture asset
        loadedTextures[path] = std::move(asset);
        return loadedTextures[path].descriptorHeapIndex;
    }

    // ── PNG / STB ────────────────────────────────────────
    {
        int texWidth = 0;
        int texHeight = 0;
        int texChannels = 0;
        stbi_uc const* pixels = stbi_load(path.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
        if (!pixels) {
            throw std::runtime_error("Failed to load texture via stb: " + path);
        }
        const uint32_t heapIndex = uploadRgba8(cacheKey, pixels, texWidth, texHeight, rgbaFormat(colorSpace));
        stbi_image_free(const_cast<stbi_uc*>(pixels));
        return heapIndex;
    }
}

uint32_t TextureManager::loadTextureFromMemory(std::string cacheKey, std::span<const uint8_t> bytes,
                                               std::string_view mime, TextureColorSpace colorSpace)
{
    ZoneScopedN("TextureManager::loadTextureFromMemory");
    cacheKey += colorSpaceSuffix(colorSpace);
    if (loadedTextures.find(cacheKey) != loadedTextures.end()) {
        return loadedTextures[cacheKey].descriptorHeapIndex;
    }
    if (bytes.empty()) {
        throw std::runtime_error("Empty texture blob: " + cacheKey);
    }

    int texWidth = 0;
    int texHeight = 0;
    int texChannels = 0;
    stbi_uc const* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &texWidth, &texHeight,
                                                  &texChannels, STBI_rgb_alpha);
    if (!pixels) {
        throw std::runtime_error(std::format("Failed to decode texture blob '{}' mime='{}'", cacheKey, mime));
    }
    const uint32_t heapIndex = uploadRgba8(cacheKey, pixels, texWidth, texHeight, rgbaFormat(colorSpace));
    stbi_image_free(const_cast<stbi_uc*>(pixels));
    return heapIndex;
}

uint32_t TextureManager::loadTextureFromPixels(std::string cacheKey, std::span<const uint8_t> rgba, uint32_t width,
                                               uint32_t height, TextureColorSpace colorSpace)
{
    ZoneScopedN("TextureManager::loadTextureFromPixels");
    cacheKey += colorSpaceSuffix(colorSpace);
    if (loadedTextures.find(cacheKey) != loadedTextures.end()) {
        return loadedTextures[cacheKey].descriptorHeapIndex;
    }
    const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    if (rgba.size() < expected) {
        throw std::runtime_error("Pixel blob too small: " + cacheKey);
    }
    return uploadRgba8(cacheKey, rgba.data(), static_cast<int>(width), static_cast<int>(height),
                       rgbaFormat(colorSpace));
}

uint32_t TextureManager::getOrCreateSampler(int32_t minFilter, int32_t magFilter, int32_t wrapS, int32_t wrapT)
{
    if (minFilter < 0) {
        minFilter = 9987;
    }
    if (magFilter < 0) {
        magFilter = 9729;
    }
    if (wrapS == 0) {
        wrapS = 10497;
    }
    if (wrapT == 0) {
        wrapT = 10497;
    }

    const uint64_t key = packSamplerKey(minFilter, magFilter, wrapS, wrapT);
    if (const auto it = samplerKeyToIndex.find(key); it != samplerKeyToIndex.end()) {
        return it->second;
    }

    const auto maxAniso = descriptorManager.capabilities.properties2.properties.limits.maxSamplerAnisotropy;
    const vk::SamplerCreateInfo samplerInfo = samplerInfoFromGltf(minFilter, magFilter, wrapS, wrapT, maxAniso);
    const uint32_t heapIndex = descriptorManager.writeSamplerDescriptor(samplerInfo);
    samplers.push_back(SamplerDesc{
        .minFilter = minFilter,
        .magFilter = magFilter,
        .wrapS = wrapS,
        .wrapT = wrapT,
        .heapIndex = heapIndex,
    });
    samplerKeyToIndex[key] = heapIndex;
    return heapIndex;
}

uint32_t TextureManager::uploadRgba8(const std::string& cacheKey, const void* pixels, int texWidth, int texHeight,
                                     vk::Format format)
{
    const vk::ImageLayout hostDstLayout = stbHostCopyDstLayout(physicalDevice, deviceWrapper.capabilities, format);

    vk::DeviceSize imageSize = static_cast<vk::DeviceSize>(texWidth) * static_cast<vk::DeviceSize>(texHeight) * 4;
    mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(texWidth, texHeight)))) + 1;

    TextureAsset asset{};
    createImage(
        static_cast<uint32_t>(texWidth), static_cast<uint32_t>(texHeight), mipLevels, format, vk::ImageTiling::eOptimal,
        vk::ImageUsageFlagBits::eHostTransfer | vk::ImageUsageFlagBits::eTransferSrc |
            vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
        vk::MemoryPropertyFlagBits::eDeviceLocal, asset.textureImage, asset.textureImageMemory, "TextureImageMemory");
    setDebugName(device, asset.textureImage, "TextureImage");
    const size_t texBytes = static_cast<size_t>(imageSize) + static_cast<size_t>(imageSize) / 3u;
    tracyResourceAlloc(static_cast<VkImage>(*asset.textureImage), texBytes, "GPU/Textures");
#ifdef TRACY_ENABLE
    {
        const std::string texMsg = std::format("Texture '{}' {}x{} mips={}", cacheKey, texWidth, texHeight, mipLevels);
        TracyMessage(texMsg.c_str(), texMsg.size());
    }
    TracyPlot("Vulkan/HostImageCopyBytes", static_cast<double>(imageSize));
#endif

    {
        ZoneScopedN("TextureManager::copyMemoryToImage");
        const vk::HostImageLayoutTransitionInfo hostTransition{
            .image = *asset.textureImage,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = hostDstLayout,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, 1},
        };
        device.transitionImageLayout({hostTransition});

        const vk::MemoryToImageCopy region{
            .pHostPointer = pixels,
            .memoryRowLength = 0,
            .memoryImageHeight = 0,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {static_cast<uint32_t>(texWidth), static_cast<uint32_t>(texHeight), 1},
        };
        const vk::CopyMemoryToImageInfo copyInfo{
            .dstImage = *asset.textureImage,
            .dstImageLayout = hostDstLayout,
            .regionCount = 1,
            .pRegions = &region,
        };
        device.copyMemoryToImage(copyInfo);
    }

    {
        auto cmdBuffer = beginSingleTimeCommands(graphicsQueue);
        transitionImageLayout(&cmdBuffer, *asset.textureImage, mipLevels, hostDstLayout,
                              vk::ImageLayout::eTransferDstOptimal, {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
                              VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, vk::PipelineStageFlagBits2::eHost,
                              vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eHostWrite,
                              vk::AccessFlagBits2::eTransferWrite);
        endSingleTimeCommands(cmdBuffer, graphicsQueue);
    }

    generateMipmaps(asset.textureImage, format, texWidth, texHeight, mipLevels);

    vk::ImageViewCreateInfo viewInfo{.image = asset.textureImage,
                                     .viewType = vk::ImageViewType::e2D,
                                     .format = format,
                                     .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, 1}};
    asset.textureImageView = vk::raii::ImageView(device, viewInfo);

    descriptorManager.writeImageDescriptor(asset, viewInfo);
    loadedTextures[cacheKey] = std::move(asset);

    log_info(std::format("STB texture loaded: {}×{}, {} mips (hostImageCopy) key={}", texWidth, texHeight, mipLevels,
                         cacheKey),
             "TextureManager");
    return loadedTextures[cacheKey].descriptorHeapIndex;
}

// Create an image with the requested properties and allocate GPU memory
// for it via VMA. Returns the vk::ImageCreateInfo used (for callers that
// need it).
vk::ImageCreateInfo TextureManager::createImage(uint32_t width, uint32_t height, uint32_t mipLevelsIn,
                                                vk::Format format, vk::ImageTiling tiling, vk::ImageUsageFlags usage,
                                                vk::MemoryPropertyFlags properties, vk::raii::Image& image,
                                                VmaAllocation& imageMemory, std::string_view memoryDebugBaseName)
{
    ZoneScopedN("TextureManager::createImage");
    log_info("createImage() started", "TextureManager");
    const bool needsConcurrent =
        (usage & vk::ImageUsageFlagBits::eTransferSrc || usage & vk::ImageUsageFlagBits::eTransferDst) &&
        transferQueueFamilyIndex != UINT32_MAX && transferQueueFamilyIndex != graphicsQueueFamilyIndex;

    uint32_t families[2] = {graphicsQueueFamilyIndex, transferQueueFamilyIndex};

    vk::ImageCreateInfo const imageInfo{.imageType = vk::ImageType::e2D,
                                        .format = format,
                                        .extent = {width, height, 1},
                                        .mipLevels = mipLevelsIn,
                                        .arrayLayers = 1,
                                        .samples = vk::SampleCountFlagBits::e1,
                                        .tiling = tiling,
                                        .usage = usage,
                                        .sharingMode = needsConcurrent ? vk::SharingMode::eConcurrent
                                                                       : vk::SharingMode::eExclusive,
                                        .queueFamilyIndexCount = needsConcurrent ? 2u : 0u,
                                        .pQueueFamilyIndices = needsConcurrent ? families : nullptr};
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (properties & vk::MemoryPropertyFlagBits::eHostVisible) {
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        allocInfo.priority = 0.25f;
    } else {
        // Sampled textures / GPU images: high keep priority under memory pressure.
        allocInfo.priority = 0.9f;
    }

    allocator.alocateImage(imageInfo, allocInfo, image, imageMemory, memoryDebugBaseName);
    return imageInfo;
}

// begin one-time command buffer
vk::raii::CommandBuffer TextureManager::beginSingleTimeCommands(const vk::raii::Queue& queue)
{
    ZoneScopedN("TextureManager::beginSingleTimeCommands");
    log_info("beginSingleTimeCommands() started", "TextureManager");
    vk::CommandBufferAllocateInfo allocInfo{
        .commandPool = commandPool, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1};
    auto commandBuffers = device.allocateCommandBuffers(allocInfo);
    vk::raii::CommandBuffer commandBuffer = std::move(commandBuffers[0]);
    vk::CommandBufferBeginInfo beginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit};
    commandBuffer.begin(beginInfo);
    return commandBuffer;
}

// end command buffer and wait idle
void TextureManager::endSingleTimeCommands(vk::raii::CommandBuffer& commandBuffer, const vk::raii::Queue& queue)
{
    ZoneScopedN("TextureManager::endSingleTimeCommands");
    log_info("endSingleTimeCommands() started", "TextureManager");
    commandBuffer.end();
    const vk::CommandBufferSubmitInfo commandBufferInfo{.commandBuffer = *commandBuffer};
    const vk::SubmitInfo2 submitInfo{.commandBufferInfoCount = 1, .pCommandBufferInfos = &commandBufferInfo};
    queue.submit2(submitInfo, nullptr);
    queue.waitIdle();
}


// generate mipmaps via iterative blit
void TextureManager::generateMipmaps(vk::raii::Image& image, vk::Format imageFormat, int32_t texWidth,
                                     int32_t texHeight, uint32_t mipLevelsIn)
{
    ZoneScopedN("TextureManager::generateMipmaps");
    log_info("generateMipmaps() started", "TextureManager");
    vk::FormatProperties formatProperties = physicalDevice.getFormatProperties2(imageFormat).formatProperties;
    if (!(formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImageFilterLinear)) {
        throw std::runtime_error("Texture image format does not support linear blitting!");
    }

    auto commandBuffer = beginSingleTimeCommands(graphicsQueue);

    int32_t mipWidth = texWidth;
    int32_t mipHeight = texHeight;

    for (uint32_t i = 1; i < mipLevelsIn; i++) {
        // transition previous level to blit source
        const vk::ImageMemoryBarrier2 toSrc{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                                            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                                            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                                            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                                            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                                            .image = image,
                                            .subresourceRange = {vk::ImageAspectFlagBits::eColor, i - 1, 1, 0, 1}};
        const vk::DependencyInfo toSrcDep{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toSrc};
        commandBuffer.pipelineBarrier2(toSrcDep);

        vk::ImageBlit blit{};
        blit.srcSubresource = {vk::ImageAspectFlagBits::eColor, i - 1, 0, 1};
        blit.srcOffsets[0] = vk::Offset3D(0, 0, 0);
        blit.srcOffsets[1] = vk::Offset3D(mipWidth, mipHeight, 1);
        blit.dstSubresource = {vk::ImageAspectFlagBits::eColor, i, 0, 1};
        blit.dstOffsets[0] = vk::Offset3D(0, 0, 0);
        blit.dstOffsets[1] = vk::Offset3D(mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1);

        commandBuffer.blitImage(image, vk::ImageLayout::eTransferSrcOptimal, image,
                                vk::ImageLayout::eTransferDstOptimal, {blit}, vk::Filter::eLinear);

        if (mipWidth > 1)
            mipWidth /= 2;
        if (mipHeight > 1)
            mipHeight /= 2;
    }

    // transition all mips to shader read
    if (mipLevelsIn == 1) {
        const vk::ImageMemoryBarrier2 toSampled{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                                .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                                                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                                                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                                                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                                .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                                                .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                                                .image = image,
                                                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
        const vk::DependencyInfo dep{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toSampled};
        commandBuffer.pipelineBarrier2(dep);
    } else {
        const vk::ImageMemoryBarrier2 barriers[2] = {
            {.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
             .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
             .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
             .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
             .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
             .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
             .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
             .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
             .image = image,
             .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, mipLevelsIn - 1, 0, 1}},
            {.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
             .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
             .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
             .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
             .oldLayout = vk::ImageLayout::eTransferDstOptimal,
             .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
             .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
             .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
             .image = image,
             .subresourceRange = {vk::ImageAspectFlagBits::eColor, mipLevelsIn - 1, 1, 0, 1}},
        };
        const vk::DependencyInfo dep{.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = barriers};
        commandBuffer.pipelineBarrier2(dep);
    }

    endSingleTimeCommands(commandBuffer, graphicsQueue);
}
