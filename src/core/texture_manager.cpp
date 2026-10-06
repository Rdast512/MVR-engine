#include "texture_manager.hpp"



// Construct a TextureManager which holds Vulkan device/queue handles and
// creates a command pool for short-lived graphics commands.
TextureManager::TextureManager(Device& deviceWrapper, const VkAllocator& allocator, DescriptorManager &descriptorManager) :
    deviceWrapper(deviceWrapper), allocator(allocator), descriptorManager(descriptorManager),
    physicalDevice(deviceWrapper.physicalDevice), device(deviceWrapper.vkdevice),
    graphicsQueue(deviceWrapper.graphicsQueue), graphicsQueueFamilyIndex(deviceWrapper.graphicsIndex)
{
    log_info("Constructor started", "TextureManager");
    vk::CommandPoolCreateInfo poolInfo{.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                       .queueFamilyIndex = graphicsQueueFamilyIndex};
    commandPool = vk::raii::CommandPool(device, poolInfo);
}

std::string TextureManager::resolvePath(std::string_view path)
{
    std::filesystem::path fsPath(path);

    // If the path is already absolute, return it as-is
    if (fsPath.is_absolute()) {
        return fsPath.string();
    }

    // CWD, not the exe dir: assets are addressed relative to the launch directory
    std::filesystem::path resolved = std::filesystem::current_path() / fsPath;

    if (std::filesystem::exists(resolved)) {
        log_info(std::format("Resolved path: {} -> {}", path, resolved.string()), "TextureManager");
        return resolved.string();
    }

    // If not found relative to CWD, log warning and return original
    // (let the loader try and fail with a more informative error)
    log_info(std::format("Path not found relative to CWD: {}; trying original", path), "TextureManager");
    return std::string(path);
}

// Views go before their images; KTX images are owned by libktx, the rest by VMA.
TextureManager::~TextureManager()
{
    ZoneScopedN("TextureManager::~TextureManager");
    log_info("Destructor called", "TextureManager");

    for (auto& [key, asset] : loadedTextures) {
        asset.textureImageView = nullptr;
        destroyVmaImage(allocator.allocator, asset.textureImage, asset.textureImageMemory, "GPU/Textures");
    }
    loadedTextures.clear();
    for (ktxVulkanTexture& texture : ktxTextures) {
        ktxVulkanTexture_Destruct(&texture, *device, nullptr);
    }
    if (ktxDeviceInfo) {
        ktxVulkanDeviceInfo_Destruct(&*ktxDeviceInfo);
    }
    log_info("Resources destroyed", "TextureManager");
}

// ── format-detecting texture loader ─────────────────────────

namespace {

enum class TextureFormat { Ktx, Png, Unknown };

// Detect a texture file format from its filename extension (KTX vs PNG/etc).
TextureFormat detectFormat(std::string_view path)
{
    const auto dot = path.rfind('.');
    if (dot == std::string_view::npos) return TextureFormat::Unknown;

    const std::string_view ext = path.substr(dot);
    if (ext == ".ktx" || ext == ".ktx2") return TextureFormat::Ktx;
    if (ext == ".png"  || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp") return TextureFormat::Png;
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
        physicalDevice.getImageFormatProperties2<vk::ImageFormatProperties2,
                                                 vk::HostImageCopyDevicePerformanceQuery>(formatInfo);
    const auto& perf = perfChain.get<vk::HostImageCopyDevicePerformanceQuery>();
    log_info(std::format("hostImageCopy perf ({}): optimalDeviceAccess={} identicalMemoryLayout={}",
                         vk::to_string(format), static_cast<bool>(perf.optimalDeviceAccess),
                         static_cast<bool>(perf.identicalMemoryLayout)),
             "TextureManager");
    if (!perf.optimalDeviceAccess) {
        log_info("HOST_TRANSFER may be slower to sample than a non-host-transfer image", "TextureManager");
    }

    // mip chain is built with linear blits
    const vk::FormatProperties formatProperties = physicalDevice.getFormatProperties2(format).formatProperties;
    if (!(formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImageFilterLinear)) {
        throw std::runtime_error("Texture image format does not support linear blitting!");
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
    case TG3_TEXTURE_WRAP_CLAMP_TO_EDGE:
        return vk::SamplerAddressMode::eClampToEdge;
    case TG3_TEXTURE_WRAP_MIRRORED_REPEAT:
        return vk::SamplerAddressMode::eMirroredRepeat;
    default:
        return vk::SamplerAddressMode::eRepeat;
    }
}

vk::SamplerCreateInfo samplerInfoFromGltf(int32_t minFilter, int32_t magFilter, int32_t wrapS, int32_t wrapT,
                                          float maxSamplerAnisotropy)
{
    const vk::Filter mag = magFilter == TG3_TEXTURE_FILTER_NEAREST ? vk::Filter::eNearest : vk::Filter::eLinear;
    vk::Filter min = vk::Filter::eLinear;
    vk::SamplerMipmapMode mip = vk::SamplerMipmapMode::eLinear;
    float maxLod = vk::LodClampNone;
    switch (minFilter) {
    case TG3_TEXTURE_FILTER_NEAREST:
        min = vk::Filter::eNearest;
        mip = vk::SamplerMipmapMode::eNearest;
        maxLod = 0.25f;
        break;
    case TG3_TEXTURE_FILTER_LINEAR:
        min = vk::Filter::eLinear;
        mip = vk::SamplerMipmapMode::eNearest;
        maxLod = 0.25f;
        break;
    case TG3_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST:
        min = vk::Filter::eNearest;
        mip = vk::SamplerMipmapMode::eNearest;
        break;
    case TG3_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST:
        min = vk::Filter::eLinear;
        mip = vk::SamplerMipmapMode::eNearest;
        break;
    case TG3_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR:
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
    const bool anisotropy = mag == vk::Filter::eLinear && min == vk::Filter::eLinear &&
                            mip == vk::SamplerMipmapMode::eLinear;

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
    // descriptor manager's default sampler serves the glTF default (trilinear, repeat)
    samplerKeyToIndex[packSamplerKey(TG3_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR, TG3_TEXTURE_FILTER_LINEAR,
                                     TG3_TEXTURE_WRAP_REPEAT, TG3_TEXTURE_WRAP_REPEAT)] =
        descriptorManager.getSamplerDescriptorIndex();

    for (const TextureColorSpace colorSpace : {TextureColorSpace::Srgb, TextureColorSpace::Linear}) {
        hostCopyDstLayout = stbHostCopyDstLayout(physicalDevice, deviceWrapper.capabilities, rgbaFormat(colorSpace));
    }

    ktxDeviceInfo.emplace();
    if (ktxVulkanDeviceInfo_Construct(&*ktxDeviceInfo, *physicalDevice, *device, *graphicsQueue, *commandPool,
                                      nullptr) != KTX_SUCCESS) {
        ktxDeviceInfo.reset();
        throw std::runtime_error("ktxVulkanDeviceInfo_Construct failed");
    }
    log_info("Initialized", "TextureManager");
}

std::optional<uint32_t> TextureManager::cachedHeapIndex(const std::string& key) const
{
    const auto it = loadedTextures.find(key);
    if (it == loadedTextures.end()) {
        return std::nullopt;
    }
    return it->second.descriptorHeapIndex;
}

bool TextureManager::isCached(std::string cacheKey, TextureColorSpace colorSpace) const
{
    cacheKey += colorSpaceSuffix(colorSpace);
    return loadedTextures.contains(cacheKey);
}

void TextureManager::StbFree::operator()(unsigned char* pixels) const
{
    stbi_image_free(pixels);
}

TextureManager::DecodedImage TextureManager::decodeRgba8(std::span<const uint8_t> bytes)
{
    ZoneScopedN("TextureManager::decodeRgba8");
    DecodedImage image{};
    int channels = 0;
    image.pixels.reset(stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &image.width,
                                             &image.height, &channels, STBI_rgb_alpha));
    return image;
}

TextureManager::DecodedImage TextureManager::decodeRgba8File(const std::string& path)
{
    ZoneScopedN("TextureManager::decodeRgba8File");
    DecodedImage image{};
    int channels = 0;
    image.pixels.reset(stbi_load(path.c_str(), &image.width, &image.height, &channels, STBI_rgb_alpha));
    return image;
}

bool TextureManager::isKtxPath(std::string_view path)
{
    return detectFormat(path) == TextureFormat::Ktx;
}

// High-level texture loader that chooses between KTX (fast GPU upload)
// and a PNG/STB fallback. Caches loaded textures and returns a descriptor.
uint32_t TextureManager::loadTexture(std::string texturePath, TextureColorSpace colorSpace)
{
    ZoneScopedN("TextureManager::loadTexture");
    const std::string path = resolvePath(texturePath);
    const TextureFormat fmt = detectFormat(path);
    // KTX carries its own format, so both color spaces share one entry
    const std::string cacheKey = fmt == TextureFormat::Ktx ? path : path + colorSpaceSuffix(colorSpace);
    if (const auto cached = cachedHeapIndex(cacheKey)) {
        log_info(std::format("Texture already loaded: {}", cacheKey), "TextureManager");
        return *cached;
    }
    log_info(std::format("loadTexture: {} → {}", path,
                         fmt == TextureFormat::Ktx ? "KTX/KTX2" :
                         fmt == TextureFormat::Png ? "PNG/STB" : "Unknown"), "TextureManager");

    if (fmt == TextureFormat::Ktx) {
        return uploadKtx(path);
    }

    DecodedImage image = decodeRgba8File(path);
    return uploadDecoded(cacheKey, std::move(image.pixels), image.width, image.height, colorSpace, "file");
}

uint32_t TextureManager::loadTextureFromMemory(std::string cacheKey, std::span<const uint8_t> bytes,
                                               std::string_view mime, TextureColorSpace colorSpace)
{
    ZoneScopedN("TextureManager::loadTextureFromMemory");
    cacheKey += colorSpaceSuffix(colorSpace);
    if (const auto cached = cachedHeapIndex(cacheKey)) {
        return *cached;
    }
    if (bytes.empty()) {
        throw std::runtime_error("Empty texture blob: " + cacheKey);
    }

    DecodedImage image = decodeRgba8(bytes);
    return uploadDecoded(cacheKey, std::move(image.pixels), image.width, image.height, colorSpace, mime);
}

uint32_t TextureManager::loadTextureFromPixels(std::string cacheKey, std::span<const uint8_t> rgba, uint32_t width,
                                               uint32_t height, TextureColorSpace colorSpace)
{
    ZoneScopedN("TextureManager::loadTextureFromPixels");
    cacheKey += colorSpaceSuffix(colorSpace);
    if (const auto cached = cachedHeapIndex(cacheKey)) {
        return *cached;
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
    // glTF: undefined filters are implementation-defined, undefined wrap is REPEAT
    if (minFilter < 0) {
        minFilter = TG3_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR;
    }
    if (magFilter < 0) {
        magFilter = TG3_TEXTURE_FILTER_LINEAR;
    }
    if (wrapS == 0) {
        wrapS = TG3_TEXTURE_WRAP_REPEAT;
    }
    if (wrapT == 0) {
        wrapT = TG3_TEXTURE_WRAP_REPEAT;
    }

    const uint64_t key = packSamplerKey(minFilter, magFilter, wrapS, wrapT);
    if (const auto it = samplerKeyToIndex.find(key); it != samplerKeyToIndex.end()) {
        return it->second;
    }

    const auto maxAniso = descriptorManager.capabilities.properties2.properties.limits.maxSamplerAnisotropy;
    const vk::SamplerCreateInfo samplerInfo = samplerInfoFromGltf(minFilter, magFilter, wrapS, wrapT, maxAniso);
    const uint32_t heapIndex = descriptorManager.writeSamplerDescriptor(samplerInfo);
    samplerKeyToIndex[key] = heapIndex;
    return heapIndex;
}

uint32_t TextureManager::uploadKtx(const std::string& path)
{
    ZoneScopedN("TextureManager::uploadKtx");
    // KTX1 vs KTX2 detected from the file header
    ktxTexture* kTexture = nullptr;
    KTX_error_code result =
        ktxTexture_CreateFromNamedFile(path.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &kTexture);
    if (result != KTX_SUCCESS || !kTexture) {
        throw std::runtime_error("Failed to load KTX texture: " + path);
    }

    // libktx creates the VkImage + VkDeviceMemory; the CPU copy is no longer needed after upload
    ktxVulkanTexture vkTex{};
    result = ktxTexture_VkUpload(kTexture, &*ktxDeviceInfo, &vkTex);
    ktxTexture_Destroy(kTexture);
    if (result != KTX_SUCCESS) {
        throw std::runtime_error("Failed to upload KTX texture to GPU: " + path);
    }
    ktxTextures.push_back(vkTex);

    log_info(std::format("KTX texture uploaded: {}×{}, {} mips, format={}", vkTex.width, vkTex.height,
                         vkTex.levelCount, static_cast<uint32_t>(vkTex.imageFormat)),
             "TextureManager");

    // non-owning view: the image stays with ktxTextures
    TextureAsset asset{};
    vk::ImageViewCreateInfo const viewInfo{
        .image       = vk::Image(vkTex.image),
        .viewType    = static_cast<vk::ImageViewType>(vkTex.viewType),
        .format      = static_cast<vk::Format>(vkTex.imageFormat),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, vkTex.levelCount, 0, 1}};
    asset.textureImageView = vk::raii::ImageView(device, viewInfo);

    descriptorManager.writeImageDescriptor(asset, viewInfo);
    const uint32_t heapIndex = asset.descriptorHeapIndex;
    loadedTextures.insert_or_assign(path, std::move(asset));
    return heapIndex;
}

uint32_t TextureManager::uploadDecoded(const std::string& cacheKey, StbPixels pixels, int texWidth, int texHeight,
                                       TextureColorSpace colorSpace, std::string_view origin)
{
    if (!pixels) {
        throw std::runtime_error(std::format("Failed to decode texture '{}' from {}", cacheKey, origin));
    }
    return uploadRgba8(cacheKey, pixels.get(), texWidth, texHeight, rgbaFormat(colorSpace));
}

uint32_t TextureManager::uploadRgba8(const std::string& cacheKey, const void* pixels, int texWidth, int texHeight,
                                     vk::Format format)
{
    const vk::ImageLayout hostDstLayout = hostCopyDstLayout;

    vk::DeviceSize imageSize =
        static_cast<vk::DeviceSize>(texWidth) * static_cast<vk::DeviceSize>(texHeight) * 4;
    const uint32_t mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(texWidth, texHeight)))) + 1;

    TextureAsset asset{};
    allocator.createImage2D(static_cast<uint32_t>(texWidth), static_cast<uint32_t>(texHeight), mipLevels,
                            vk::SampleCountFlagBits::e1, format,
                            vk::ImageUsageFlagBits::eHostTransfer | vk::ImageUsageFlagBits::eTransferSrc |
                                vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                            asset.textureImage, asset.textureImageMemory, "TextureImageMemory");
    setDebugName(device, asset.textureImage, "TextureImage");
    const size_t texBytes = static_cast<size_t>(imageSize) + static_cast<size_t>(imageSize) / 3u;
    tracyResourceAlloc(static_cast<VkImage>(*asset.textureImage), texBytes, "GPU/Textures");
#ifdef TRACY_ENABLE
    {
        const std::string texMsg =
            std::format("Texture '{}' {}x{} mips={}", cacheKey, texWidth, texHeight, mipLevels);
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

    // host-written level 0 → TransferDst, then the blit chain; outside a batch this texture is its own batch
    const bool ownsBatch = !uploadBatch;
    if (ownsBatch) {
        beginUploadBatch();
    }
    vk::raii::CommandBuffer& commandBuffer = *uploadBatch;
    transitionImageLayout(&commandBuffer, *asset.textureImage, hostDstLayout, vk::ImageLayout::eTransferDstOptimal,
                          {vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, 1}, VK_QUEUE_FAMILY_IGNORED,
                          VK_QUEUE_FAMILY_IGNORED, vk::PipelineStageFlagBits2::eHost,
                          vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eHostWrite,
                          vk::AccessFlagBits2::eTransferWrite);
    generateMipmaps(commandBuffer, asset.textureImage, texWidth, texHeight, mipLevels);
    if (ownsBatch) {
        flushUploadBatch();
    }

    vk::ImageViewCreateInfo viewInfo{
        .image = asset.textureImage,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, 1}};
    asset.textureImageView = vk::raii::ImageView(device, viewInfo);

    descriptorManager.writeImageDescriptor(asset, viewInfo);
    const uint32_t heapIndex = asset.descriptorHeapIndex;
    loadedTextures.insert_or_assign(cacheKey, std::move(asset));

    log_info(std::format("STB texture loaded: {}×{}, {} mips (hostImageCopy) key={}", texWidth, texHeight, mipLevels,
                         cacheKey),
             "TextureManager");
    return heapIndex;
}

void TextureManager::beginUploadBatch()
{
    ZoneScopedN("TextureManager::beginUploadBatch");
    if (uploadBatch) {
        throw std::runtime_error("Texture upload batch already open");
    }
    const vk::CommandBufferAllocateInfo allocInfo{
        .commandPool = commandPool, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1};
    auto commandBuffers = device.allocateCommandBuffers(allocInfo);
    uploadBatch.emplace(std::move(commandBuffers[0]));
    uploadBatch->begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
}

// One submit for every texture recorded since beginUploadBatch; waits on a fence, not the whole queue.
void TextureManager::flushUploadBatch()
{
    ZoneScopedN("TextureManager::flushUploadBatch");
    if (!uploadBatch) {
        return;
    }
    // close the batch first so a failed submit cannot leave it half-open
    vk::raii::CommandBuffer commandBuffer = std::move(*uploadBatch);
    uploadBatch.reset();
    commandBuffer.end();

    const vk::raii::Fence fence(device, vk::FenceCreateInfo{});
    const vk::CommandBufferSubmitInfo commandBufferInfo{.commandBuffer = *commandBuffer};
    const vk::SubmitInfo2 submitInfo{.commandBufferInfoCount = 1, .pCommandBufferInfos = &commandBufferInfo};
    graphicsQueue.submit2(submitInfo, *fence);
    if (device.waitForFences({*fence}, vk::True, std::numeric_limits<uint64_t>::max()) != vk::Result::eSuccess) {
        throw std::runtime_error("Texture upload fence wait failed");
    }
}


// Generate mipmaps on the GPU by successively blitting between mip levels.
// Records into commandBuffer; all levels must be in TransferDst on entry.
// Layout strategy (avoids BestPractices-PipelineBarrier-readToReadBarrier):
//   - Each level is written as TransferDst (base copy or blit destination).
//   - Only promote TransferDst → TransferSrc when that level is about to be a blit source
//     (write→read — required and not a BP read-to-read).
//   - After the chain finishes, levels [0, last) sit in TransferSrc; the last level stays
//     TransferDst. Transition last with TransferDst → ShaderReadOnly (write→read). For the
//     already-read levels, one barrier covers TransferSrc → ShaderReadOnly (layout change;
//     availability of the original TransferWrite was established by the earlier Dst→Src
//     barriers + transfer execution dependency). No per-mip TransferSrc→ShaderRead in the loop.
// Linear-blit support for both RGBA8 formats is checked once in init().
void TextureManager::generateMipmaps(vk::raii::CommandBuffer& commandBuffer, vk::raii::Image& image,
                                     int32_t texWidth, int32_t texHeight, uint32_t mipLevelsIn)
{
    ZoneScopedN("TextureManager::generateMipmaps");

    int32_t mipWidth = texWidth;
    int32_t mipHeight = texHeight;

    for (uint32_t i = 1; i < mipLevelsIn; i++)
    {
        // Previous level: last write was TransferWrite in TransferDst — make it a blit source.
        const vk::ImageMemoryBarrier2 toSrc{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
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

        // Leave level i-1 in TransferSrc; level i remains TransferDst for the next iteration
        // (or for the final write→shader-read barrier when i is last).

        if (mipWidth > 1)
            mipWidth /= 2;
        if (mipHeight > 1)
            mipHeight /= 2;
    }

    // Final layout: all mips → ShaderReadOnlyOptimal.
    // - Levels that were blit sources: TransferSrc (read layout) → ShaderReadOnly.
    // - Last level never needed as a source: TransferDst (write) → ShaderReadOnly.
    if (mipLevelsIn == 1) {
        const vk::ImageMemoryBarrier2 toSampled{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
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
            // Already-used blit sources [0, last).
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
            // Last mip: still TransferDst after final blit destination write.
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
}
