#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
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
    Primitives,       // GpuPrimitive[], parallel to GeometryStore::primitiveDraws
    Count
};
inline constexpr size_t kAssetBufferCount = std::to_underlying(AssetBuffer::Count);

// [first, first + count) in elements
struct ElementRange {
    uint32_t first = 0;
    uint32_t count = 0;

    [[nodiscard]] uint32_t end() const noexcept { return first + count; }
};

// One model's contiguous slice of every asset buffer and catalog array; unload erases the slices
// and shifts everything after them (docs/model_unload_ui_spec.md).
// GPU data that points into other GPU data (meshlet vertex ids, meshlet offsets, indices) is relative to
// the model's slice, so compaction is a plain copy; draws add the slice base to the buffer address.
struct ModelAllocation {
    std::array<ElementRange, kAssetBufferCount> gpu;
    ElementRange primitives;   // GeometryStore::primitiveDraws
    ElementRange morphTargets; // GeometryStore::morphTargets
    ElementRange morphWeights; // GeometryStore::morphWeights
    ElementRange auxBlobs;     // GeometryStore::auxBlobs
};

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
// Shaders reach everything through the per-frame GpuFrameRoot: instances (host-visible, one buffer per frame slot),
// the GpuModel slice table and the shared asset buffers. The CPU keeps their addresses, not their contents.
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
    // grows/recreates the per-frame GpuObjectUB arrays to fit entityCount entries
    void ensureInstanceCapacity(uint32_t entityCount);
    void createUniformBuffers();
    void createColorResources();
    // append load scratch to device-local SSBOs as one model, then drop CPU bulk arrays.
    // scratch without a new entity (failed load) is discarded
    void flushGpuAssets();
    // drops an uploaded model: compacts every asset buffer (exact-fit realloc), rebases the catalog,
    // erases the entity. Textures are the caller's (TextureManager::release). GPU must be idle.
    void eraseModel(EntityId id);
    void createCameraBuffers(Camera& camera);
    void setSwapChainImageCount(uint32_t count) { swapChainImageCount = count; createSyncObjects(); }

    // per-frame Tracy plots for resource usage
    void tracyPlotResources() const;

    [[nodiscard]] vk::DeviceAddress frameRootAddress(uint32_t frameSlot) const noexcept
    {
        return frameRoots[frameSlot].address;
    }
    [[nodiscard]] const DeviceBuffer& asset(AssetBuffer id) const noexcept
    {
        return assetBuffers[std::to_underlying(id)];
    }
    // appends are exact, so used bytes / stride is the uploaded element count
    [[nodiscard]] uint32_t uploadedCount(AssetBuffer id) const noexcept;
    // address of the entity's slice of an asset buffer
    [[nodiscard]] vk::DeviceAddress modelAssetAddress(AssetBuffer id, EntityId entityId) const noexcept;
    // bytes the entity occupies across all asset buffers
    [[nodiscard]] vk::DeviceSize modelGpuBytes(EntityId entityId) const noexcept;
    // {used, capacity} summed over all asset buffers
    [[nodiscard]] std::pair<vk::DeviceSize, vk::DeviceSize> assetTotals() const noexcept;

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
    // indexed by EntityId, parallel to objectStorage
    std::vector<ModelAllocation> modelAllocations;

    // one GpuObjectUB[capacity] buffer per frame-in-flight, rewritten by the CPU every frame
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> instanceUbos;
    // GpuObjectUB slots per frame buffer, may exceed entity count
    uint32_t instanceCapacity = 0;
    // one GpuFrameRoot per frame-in-flight, rewritten every frame
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> frameRoots;
    // GpuModel[modelAllocations.size()], rebuilt after every flush and erase (GPU idle)
    HostBuffer modelTable;
    std::array<vk::DeviceAddress, MAX_FRAMES_IN_FLIGHT> cameraAddresses{};

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
    // persistently mapped host-visible storage buffer with a device address
    void createHostBuffer(HostBuffer& target, vk::DeviceSize size, const std::string& debugName,
                          const char* tracyName) const;
    void destroyHostBuffer(HostBuffer& target, const char* tracyName) const;
    void rebuildModelTable();
    void writeFrameRoot(uint32_t frameSlot);
    // scratch PrimitiveDraws as model-relative GpuPrimitives; call before remapScratchOffsets
    [[nodiscard]] std::vector<GpuPrimitive> packScratchPrimitives() const;
    void destroyAssetBuffer(AssetBuffer id);
    // device-local storage buffer with the asset usage flags; not yet installed
    void createAssetBuffer(AssetBuffer id, vk::DeviceSize capacity, vk::raii::Buffer& buffer,
                           VmaAllocation& memory) const;
    // replaces the asset buffer (destroying the old one) with an already-filled buffer
    void installAssetBuffer(AssetBuffer id, vk::raii::Buffer&& buffer, VmaAllocation memory, vk::DeviceSize usedBytes,
                            vk::DeviceSize capacityBytes);
    void appendDeviceLocal(AssetBuffer id, std::span<const std::byte> src);
    [[nodiscard]] std::span<const std::byte> scratchBytes(AssetBuffer id, std::span<const GpuLight> lights,
                                                          std::span<const GpuPrimitive> primitives) const;
    // removes the model's byte ranges from every asset buffer in one submit
    void compactAssetBuffers(const ModelAllocation& removed);
    // shifts every catalog reference past the removed slices and erases the slices
    void rebaseCatalog(EntityId id, const ModelAllocation& removed);
    // failed load: nothing reaches the GPU and the catalog returns to the last flush
    void discardScratch();
    void createAttachment(AttachmentImage& target, const AttachmentDesc& desc);
    void destroyAttachment(AttachmentImage& target, const char* tracyName);
    void remapScratchOffsets();
    [[nodiscard]] std::vector<GpuLight> packScratchLights() const;
    [[nodiscard]] vk::raii::CommandBuffer& oneTimeTransferCmd();
    [[nodiscard]] const vk::raii::Queue& oneTimeTransferQueue() const noexcept;
};
