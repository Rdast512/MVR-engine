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
#include "util/vk_utils.hpp"
#include "vk_allocator.hpp"
#include "vk_device.hpp"
#include "vk_frame_sync.hpp"

// Device-local SSBOs appended by AssetBuffers::flush, read by shaders via BDA (loaded once / rarely).
// Order defines upload order and the kAssetBufferInfo table rows.
enum class AssetBuffer : uint8_t {
    Vertices,
    Meshlets,         // GpuMeshletDesc[]
    MeshletBounds,    // GpuMeshletBounds[], parallel to Meshlets
    MeshletGroups,    // GpuMeshletGroup[], from the GeometryStore::meshletGroups catalog
    MeshletPrimitives, // uint[] parallel to Meshlets: model-relative primitive row
    MeshletVertices,  // uint32_t[] remap
    MeshletTriangles, // uint8_t[] local corners
    Normals,
    Tangents,
    Uv1,
    Indices,
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
    ElementRange auxBlobs;     // GeometryStore::auxBlobs
    ElementRange nodes;        // GeometryStore::nodeDraws
    ElementRange meshletGroups; // GeometryStore::meshletGroups
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

// The device-local asset buffers, the model catalog and the GpuModel slice table. Loads append one model per flush;
// unloads compact the slices. Every change bumps sceneGeneration, which FrameData::syncScene follows.
// The GPU must be idle around flush, instantiateModel and eraseEntity.
class AssetBuffers {
public:
    // uploads the startup model already in the load scratch, if any
    AssetBuffers(const Device& deviceWrapper, const VkAllocator& allocator, FrameSync& frameSync,
                 GeometryStore& geometryStore, MaterialStore& materialStore, LightStore& lightStore,
                 ObjectStorage& objectStorage);
    ~AssetBuffers();

    AssetBuffers(const AssetBuffers&) = delete;
    AssetBuffers& operator=(const AssetBuffers&) = delete;
    AssetBuffers(AssetBuffers&&) = delete;
    AssetBuffers& operator=(AssetBuffers&&) = delete;

    // append load scratch to device-local SSBOs as one model, then drop CPU bulk arrays.
    // scratch without a new entity (failed load) is discarded
    void flush();
    [[nodiscard]] std::optional<ModelId> findModel(std::string_view key) const;
    // new entity drawing an uploaded model; nothing is loaded or uploaded except its light rows
    EntityId instantiateModel(ModelId modelId, const Transform& transform)
    {
        return instantiateModel(modelId, std::span(&transform, 1));
    }
    // one entity per transform; returns the first id, the rest follow it
    EntityId instantiateModel(ModelId modelId, std::span<const Transform> transforms);
    // erases the entity; its model goes with its last entity (compaction, material release).
    // returns the texture references the caller must drop (TextureManager::release)
    [[nodiscard]] std::vector<uint32_t> eraseEntity(EntityId id);

    [[nodiscard]] const DeviceBuffer& asset(AssetBuffer id) const noexcept
    {
        return assetBuffers[std::to_underlying(id)];
    }
    // address of the model's slice of an asset buffer
    [[nodiscard]] vk::DeviceAddress modelAssetAddress(AssetBuffer id, ModelId modelId) const noexcept;
    // GpuModel[models().size()]
    [[nodiscard]] vk::DeviceAddress modelTableAddress() const noexcept { return modelTable.address; }
    // indexed by ModelId (ObjectStorage::modelIds)
    [[nodiscard]] std::span<const ModelAllocation> models() const noexcept { return modelAllocations; }
    // appends are exact, so used bytes / stride is the uploaded element count
    [[nodiscard]] uint32_t uploadedCount(AssetBuffer id) const noexcept;
    // bytes the model's slices occupy, shared by all its entities
    [[nodiscard]] vk::DeviceSize modelGpuBytes(ModelId modelId) const noexcept;
    // {used, capacity} summed over all asset buffers
    [[nodiscard]] std::pair<vk::DeviceSize, vk::DeviceSize> assetTotals() const noexcept;
    // bumped by every flush, instantiateModel and eraseEntity that changes entities or models
    [[nodiscard]] uint32_t sceneGeneration() const noexcept { return generation; }

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
        std::vector<uint32_t> meshletPrimitives;
        // MaterialStore::dirtyRows, sorted, with their packed rows
        std::vector<uint32_t> materialRows;
        std::vector<GpuMaterial> materials;
        std::vector<GpuMaterialExtension> extensions;
    };

    void rebuildModelTable();
    // scratch PrimitiveDraws as model-relative GpuPrimitives; call before remapScratchOffsets
    [[nodiscard]] std::vector<GpuPrimitive> packScratchPrimitives() const;
    // scratch NodeDraws as model-relative GpuNodes
    [[nodiscard]] std::vector<GpuNode> packScratchNodes() const;
    // model-relative primitive row of each scratch meshlet
    [[nodiscard]] std::vector<uint32_t> packScratchMeshletPrimitives() const;
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
    void remapScratchOffsets();
    // model-space light template; attachEntity sets the owner per instance
    [[nodiscard]] std::vector<GpuLight> packScratchLights() const;
    // Tracy plots of resource counts and bytes; called when resources change (flush, instance, erase)
    void tracyPlotResources() const;

    const VkAllocator& allocator;
    const vk::raii::Device& device;
    const std::vector<uint32_t>& queueFamilyIndices;
    FrameSync& frameSync;
    ObjectStorage& objectStorage;
    GeometryStore& geometryStore;
    MaterialStore& materialStore;
    LightStore& lightStore;

    std::array<DeviceBuffer, kAssetBufferCount> assetBuffers;
    // indexed by ModelId (ObjectStorage::modelIds)
    std::vector<ModelAllocation> modelAllocations;
    // GpuModel[modelTableCapacity], rows rewritten in place after every flush and erase (GPU idle)
    HostBuffer modelTable;
    uint32_t modelTableCapacity = 0;
    uint32_t generation = 0;
};
