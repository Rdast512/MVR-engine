#pragma once
#include <array>
#include <cstdint>
#include <optional>
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
    MorphTargets,     // GpuMorphTarget[], model-relative delta offsets
    MorphWeights,     // float[], glTF default weights
    Materials,        // shared rows, MaterialStore ids; not sliced per model
    PbrExt,           // parallel to Materials
    Lights,           // per entity, rewritten from LightStore::uploaded; not sliced per model
    Primitives,       // GpuPrimitive[], parallel to GeometryStore::primitiveDraws
    Nodes,            // GpuNode[], parallel to GeometryStore::nodeDraws
    Count
};
inline constexpr size_t kAssetBufferCount = std::to_underlying(AssetBuffer::Count);

// [first, first + count) in elements
struct ElementRange {
    uint32_t first = 0;
    uint32_t count = 0;

    [[nodiscard]] uint32_t end() const noexcept { return first + count; }
};

// buffers holding one contiguous slice per model; the others are shared (Materials, PbrExt) or per entity (Lights)
[[nodiscard]] constexpr bool isModelSliced(AssetBuffer id) noexcept
{
    return id != AssetBuffer::Materials && id != AssetBuffer::PbrExt && id != AssetBuffer::Lights;
}

// One uploaded model, shared by every entity loaded from the same file. Owns a contiguous slice of every
// model-sliced asset buffer and catalog array; erasing its last entity drops the slices and shifts everything after
// them (docs/model_unload_ui_spec.md).
// GPU data that points into other GPU data (meshlet vertex ids, meshlet offsets, indices) is relative to
// the model's slice, so compaction is a plain copy; draws add the slice base to the buffer address.
struct ModelAllocation {
    std::array<ElementRange, kAssetBufferCount> gpu; // empty for buffers that are not model-sliced
    ElementRange primitives;   // GeometryStore::primitiveDraws
    ElementRange morphTargets; // GeometryStore::morphTargets
    ElementRange morphWeights; // GeometryStore::morphWeights
    ElementRange auxBlobs;     // GeometryStore::auxBlobs
    ElementRange nodes;        // GeometryStore::nodeDraws
    // resolved model file (AssetsLoader::resolveModelPath), the cache key
    std::string key;
    // union over the model's primitives, copied into every instance
    MeshletDraw meshletDraw;
    // union of the node spheres, model space
    glm::vec4 localBounds{0.0f};
    // model space, instanceIndex unset; every instance appends its own copy
    std::vector<GpuLight> lights;
    // one reference each, dropped with the model
    std::vector<uint32_t> materialRefs; // MaterialStore rows
    std::vector<uint32_t> textureRefs;  // TextureManager heap indices
    // entities drawing the model
    uint32_t refCount = 0;
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
    [[nodiscard]] std::optional<ModelId> findModel(std::string_view key) const;
    // new entity drawing an uploaded model; nothing is loaded or uploaded except its light rows
    EntityId instantiateModel(ModelId modelId, const Transform& transform);
    // erases the entity; its model goes with its last entity (compaction, material release).
    // returns the texture references the caller must drop (TextureManager::release). GPU must be idle.
    [[nodiscard]] std::vector<uint32_t> eraseEntity(EntityId id);
    void createCameraBuffers(Camera& camera);
    void setSwapChainImageCount(uint32_t count) { swapChainImageCount = count; createSyncObjects(); }

    // per-frame Tracy plots for resource usage
    void tracyPlotResources() const;
    // count rows of this frame slot's draw-record buffer, grown if needed (the slot must not be in flight);
    // the frame root written afterwards points at it
    [[nodiscard]] GpuDrawRecord* mapDrawRecords(uint32_t frameSlot, uint32_t count);

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
    // address of the model's slice of an asset buffer
    [[nodiscard]] vk::DeviceAddress modelAssetAddress(AssetBuffer id, ModelId modelId) const noexcept;
    // bytes the model's slices occupy, shared by all its entities
    [[nodiscard]] vk::DeviceSize modelGpuBytes(ModelId modelId) const noexcept;
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
    // indexed by ModelId (ObjectStorage::modelIds)
    std::vector<ModelAllocation> modelAllocations;

    // one GpuObjectUB[capacity] buffer per frame-in-flight, rewritten by the CPU every frame
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> instanceUbos;
    // GpuObjectUB slots per frame buffer, may exceed entity count
    uint32_t instanceCapacity = 0;
    // GpuNodeInstance[nodeInstanceCapacity] per frame slot: one row per entity x node, rewritten every frame
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> nodeInstanceBuffers;
    uint32_t nodeInstanceCapacity = 0;
    // first node-instance row per entity (entity-major); rebuilt whenever entities or models change
    std::vector<uint32_t> entityNodeInstanceFirst;
    uint32_t nodeInstanceCount = 0;
    // GpuDrawRecord[] per frame slot, filled by mapDrawRecords callers
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> drawRecordBuffers;
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> drawRecordCapacity{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> drawRecordCount{};
    // one GpuFrameRoot per frame-in-flight, rewritten every frame
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> frameRoots;
    // GpuModel[modelTableCapacity], rows rewritten in place after every flush and erase (GPU idle)
    HostBuffer modelTable;
    uint32_t modelTableCapacity = 0;
    std::array<vk::DeviceAddress, MAX_FRAMES_IN_FLIGHT> cameraAddresses{};

private:
    // bytes written at dstOffset of an asset buffer; the buffer grows (x2) when the write passes its capacity
    struct AssetUpload {
        AssetBuffer id;
        std::span<const std::byte> bytes;
        vk::DeviceSize dstOffset;
    };
    // load rows packed from scratch into their GPU form
    struct PackedScratch {
        std::vector<GpuPrimitive> primitives;
        std::vector<GpuNode> nodes;
        std::vector<GpuMorphTarget> morphTargets;
        // MaterialStore::dirtyRows, sorted, with their packed rows
        std::vector<uint32_t> materialRows;
        std::vector<GpuMaterial> materials;
        std::vector<GpuMaterialExtension> extensions;
    };
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
    // scratch NodeDraws as model-relative GpuNodes
    [[nodiscard]] std::vector<GpuNode> packScratchNodes() const;
    // entity-major node-instance rows; grows the per-frame buffers (GPU idle)
    void rebuildNodeInstanceLayout();
    void writeNodeInstances(uint32_t frameSlot);
    void destroyAssetBuffer(AssetBuffer id);
    // device-local storage buffer with the asset usage flags; not yet installed
    void createAssetBuffer(AssetBuffer id, vk::DeviceSize capacity, vk::raii::Buffer& buffer,
                           VmaAllocation& memory) const;
    // replaces the asset buffer (destroying the old one) with an already-filled buffer
    void installAssetBuffer(AssetBuffer id, vk::raii::Buffer&& buffer, VmaAllocation memory, vk::DeviceSize usedBytes,
                            vk::DeviceSize capacityBytes);
    // one staging buffer and one submit + idle for all uploads
    void uploadAssetBuffers(std::span<const AssetUpload> uploads);
    [[nodiscard]] std::span<const std::byte> scratchBytes(AssetBuffer id, const PackedScratch& packed) const;
    // dirty material rows, one upload per run of consecutive ids
    void appendMaterialUploads(PackedScratch& packed, std::vector<AssetUpload>& uploads) const;
    // links the entity to the model and appends its light rows; call uploadLights after
    void attachEntity(EntityId entity, ModelId modelId);
    // rewrites the light buffer from LightStore::uploaded
    void uploadLights();
    // drops a model nothing draws; returns its texture references
    [[nodiscard]] std::vector<uint32_t> eraseModel(ModelId modelId);
    // removes the model's byte ranges from every asset buffer in one submit
    void compactAssetBuffers(const ModelAllocation& removed);
    // shifts every catalog reference past the removed slices and erases the slices and the model
    void rebaseCatalog(ModelId modelId, const ModelAllocation& removed);
    // failed load: nothing reaches the GPU and the catalog returns to the last flush
    void discardScratch();
    void createAttachment(AttachmentImage& target, const AttachmentDesc& desc);
    void destroyAttachment(AttachmentImage& target, const char* tracyName);
    void remapScratchOffsets();
    // model-space light template; attachEntity sets the owner per instance
    [[nodiscard]] std::vector<GpuLight> packScratchLights() const;
    [[nodiscard]] vk::raii::CommandBuffer& oneTimeTransferCmd();
    [[nodiscard]] const vk::raii::Queue& oneTimeTransferQueue() const noexcept;
};
