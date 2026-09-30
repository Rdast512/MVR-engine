#pragma once
#include <array>
#include <span>
#include <string_view>
#include <utility>
#include <vulkan/vulkan_raii.hpp>
#include "../core/types.hpp"
#include "Constants.h"
#include "geometry_store.hpp"
#include "light_store.hpp"
#include "material_store.hpp"
#include "object_storage.hpp"
#include "scene/vk_camera.hpp"
#include "vk_allocator.hpp"
#include "vk_device.hpp"

// Device-local SSBOs appended by flushGpuAssets, read by shaders via BDA (loaded once / rarely).
// Order defines upload order and the kAssetBufferInfo table rows.
enum class AssetBuffer : uint8_t {
    Vertices,
    Meshlets,         // GpuMeshletDesc[]
    MeshletVertices,  // uint32_t[] remap
    MeshletTriangles, // uint8_t[] local corners
    Normals,
    Tangents,
    Uv1,
    Joints,
    Weights,
    Indices,
    MorphPos,
    MorphNrm,
    MorphTan,
    Materials,
    PbrExt,
    Lights,
    Count
};
inline constexpr size_t kAssetBufferCount = std::to_underlying(AssetBuffer::Count);

// Growable device-local buffer; capacity may exceed used after grow-with-headroom
struct DeviceBuffer {
    vk::raii::Buffer buffer = nullptr;
    VmaAllocation memory = nullptr;
    vk::DeviceAddress address = 0;
    vk::DeviceSize usedBytes = 0;
    vk::DeviceSize capacityBytes = 0;
};

// Persistently mapped host-visible buffer (VMA_ALLOCATION_CREATE_MAPPED_BIT)
struct HostBuffer {
    vk::raii::Buffer buffer = nullptr;
    VmaAllocation memory = nullptr;
    void* mapped = nullptr;
    vk::DeviceAddress address = 0;
};

// Swapchain-sized render target, recreated on resize
struct AttachmentImage {
    vk::raii::Image image = nullptr;
    VmaAllocation memory = nullptr;
    vk::raii::ImageView view = nullptr;
};

// Manages GPU resources (buffers, images, command pools) using Device + Assets data.
// Instance GpuObjectUB data lives in a single host-visible buffer per frame slot (SoA-friendly).
// Geometry is mesh-shader only: vertex SSBO + meshlet tables via BDA (no index buffer).
class ResourceManager {
public:
    ResourceManager(const Device &deviceWrapper,
                    const VkAllocator &allocator,
                    GeometryStore &geometryStore,
                    MaterialStore &materialStore,
                    LightStore &lightStore,
                    ObjectStorage &objectStorage);
    ~ResourceManager();

    void init();
    void createSyncObjects();
    void updateUniformBuffers(uint32_t currentImage);
    void createCommandPool();
    void createCommandBuffers();
    void createDepthResources();
    // Grow/recreate the per-frame GpuObjectUB arrays so they fit at least entityCount entries.
    void ensureInstanceCapacity(uint32_t entityCount);
    void createUniformBuffers();
    void createColorResources();
    // append load scratch to device-local SSBOs, then drop CPU bulk arrays
    void flushGpuAssets();
    void createCameraBuffers(Camera& camera);
    void setSwapChainImageCount(uint32_t count) { swapChainImageCount = count; createSyncObjects(); }

    // Per-frame Tracy plots for geometry / mesh / entity resource usage.
    void tracyPlotResources() const;

    [[nodiscard]] vk::DeviceAddress instanceUboAddress(uint32_t frameSlot, EntityId entityId) const noexcept;
    [[nodiscard]] const DeviceBuffer& asset(AssetBuffer id) const noexcept
    {
        return assetBuffers[std::to_underlying(id)];
    }
    // appends are exact, so used bytes / stride is the uploaded element count
    [[nodiscard]] uint32_t uploadedCount(AssetBuffer id) const noexcept;

    vk::raii::ImageView createImageView(vk::raii::Image &image, vk::Format format, vk::ImageAspectFlags aspectFlags,
                                        uint32_t mipLevels);
    vk::Format findSupportedFormat(const std::vector<vk::Format> &candidates, vk::ImageTiling tiling,
                                   vk::FormatFeatureFlags features);
    vk::Format findDepthFormat();
    static bool hasStencilComponent(vk::Format format);
    void updateSwapChainExtent(vk::Extent2D newExtent);
    void updateSwapChainImageFormat(vk::Format newFormat) { swapChainImageFormat = newFormat; }

    const Device &deviceWrapper;
    const VkAllocator &allocator;
    const vk::raii::PhysicalDevice &physicalDevice;
    const vk::raii::Device &device;
    const std::vector<uint32_t> &queueFamilyIndices;
    const vk::raii::Queue &graphicsQueue;
    const vk::raii::Queue &transferQueue;
    ObjectStorage &objectStorage;
    GeometryStore &geometryStore;
    MaterialStore &materialStore;
    LightStore &lightStore;
    uint32_t graphicsIndex;
    uint32_t transferIndex;
    vk::SampleCountFlagBits msaaSamples;
    vk::Extent2D swapChainExtent{};
    uint32_t swapChainImageCount = 0;
    vk::Format swapChainImageFormat = vk::Format::eUndefined;

    // Acquire: one per frame-in-flight. Present signal: one per swapchain image.
    std::vector<vk::raii::Semaphore> presentCompleteSemaphore;
    std::vector<vk::raii::Semaphore> renderFinishedSemaphore;
    std::vector<vk::raii::Fence> inFlightFences;
    vk::raii::CommandPool commandPool = nullptr;
    vk::raii::CommandPool transferCommandPool = nullptr;
    std::vector<vk::raii::CommandBuffer> commandBuffers;
    std::vector<vk::raii::CommandBuffer> transferCommandBuffer;
    AttachmentImage colorAttachment;
    AttachmentImage depthAttachment;

    std::array<DeviceBuffer, kAssetBufferCount> assetBuffers;

    // One GpuObjectUB[capacity] buffer per frame-in-flight, rewritten by the CPU every frame.
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> instanceUbos;
    // Allocated instance GpuObjectUB slots per frame buffer (may be > entity count).
    uint32_t instanceCapacity = 0;

private:
    struct AttachmentDesc {
        vk::Format format;
        vk::ImageUsageFlags usage;
        vk::ImageAspectFlags viewAspect;
        // packed depth/stencil barriers need both aspects (VUID-VkImageMemoryBarrier2-image-03320)
        vk::ImageAspectFlags barrierAspect;
        vk::ImageLayout layout;
        const char* debugName;
        const char* tracyName;
    };

    void destroyInstanceUboBuffers();
    void destroyAssetBuffer(AssetBuffer id);
    void appendDeviceLocal(AssetBuffer id, std::span<const std::byte> src);
    [[nodiscard]] std::span<const std::byte> scratchBytes(AssetBuffer id, std::span<const GpuLight> lights) const;
    // {used, capacity} summed over all asset buffers
    [[nodiscard]] std::pair<vk::DeviceSize, vk::DeviceSize> assetTotals() const noexcept;
    void createAttachment(AttachmentImage& target, const AttachmentDesc& desc);
    void destroyAttachment(AttachmentImage& target, const char* tracyName);
    void remapScratchOffsets();
    [[nodiscard]] std::vector<GpuLight> packScratchLights() const;
    [[nodiscard]] vk::raii::CommandBuffer& oneTimeTransferCmd();
    [[nodiscard]] const vk::raii::Queue& oneTimeTransferQueue() const noexcept;
};
