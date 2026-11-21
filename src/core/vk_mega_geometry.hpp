#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <vulkan/vulkan_raii.hpp>
#include "object_storage.hpp"
#include "omm_baker.hpp"
#include "types.hpp"
#include "vk_allocator.hpp"
#include "vk_device.hpp"
#include "vk_frame_sync.hpp"

// Ray-tracing geometry built once per model at load (engine spec §10.2, PBR plan C1): one CLAS per meshlet and one
// cluster BLAS per mesh primitive, grouped per model and indexed like AssetBuffers::models(). A cluster BLAS refers to
// its CLAS by address, so both stay in the model's own allocations and never move: unloading a model leaves every
// other model's structures as they are. A baked primitive also gets one opacity micromap, built before its CLAS and
// referenced by address from each of its clusters (spec/m6/d2_micromap_clas.md).
class MegaGeometry {
public:
    // one model's meshlets as AssetBuffers uploads them: meshlet rows and vertex ids are model-relative
    struct ModelGeometry {
        std::span<const GpuVertex> vertices;
        std::span<const GpuMeshletDesc> meshlets;
        std::span<const uint32_t> meshletVertices;
        std::span<const uint8_t> meshletTriangles;
        std::span<const GpuPrimitive> primitives;
        // parallel to primitives, or empty when nothing was baked
        std::span<const OmmBake> ommBakes;
    };

    MegaGeometry(const Device& device, const VkAllocator& allocator, FrameSync& frameSync);

    MegaGeometry(const MegaGeometry&) = delete;
    MegaGeometry& operator=(const MegaGeometry&) = delete;
    MegaGeometry(MegaGeometry&&) = delete;
    MegaGeometry& operator=(MegaGeometry&&) = delete;
    ~MegaGeometry() = default;

    // appends the structures of the next model; the GPU must be idle
    void buildModel(const ModelGeometry& geometry);
    // the GPU must be idle
    void eraseModel(ModelId modelId);

    // per model-relative primitive row; 0 for a primitive without meshlets
    [[nodiscard]] std::span<const vk::DeviceAddress> blasAddresses(ModelId modelId) const
    {
        return models[modelId].blasAddresses;
    }
    // per model-relative primitive row; 1 when the primitive's clusters carry a micromap
    [[nodiscard]] std::span<const uint8_t> micromapFlags(ModelId modelId) const
    {
        return models[modelId].micromapFlags;
    }
    // CLAS, BLAS and micromap bytes of every model
    [[nodiscard]] vk::DeviceSize totalBytes() const noexcept;

private:
    // VMA buffer freed with its owner; address and mapped point at the aligned buffer start (MG-B1)
    struct OwnedBuffer {
        VmaAllocator allocator = nullptr;
        vk::raii::Buffer buffer = nullptr;
        VmaAllocation memory = nullptr;
        vk::DeviceAddress address = 0;
        std::byte* mapped = nullptr;
        vk::DeviceSize bytes = 0;

        OwnedBuffer() = default;
        ~OwnedBuffer() { reset(); }
        OwnedBuffer(const OwnedBuffer&) = delete;
        OwnedBuffer& operator=(const OwnedBuffer&) = delete;
        OwnedBuffer(OwnedBuffer&& other) noexcept;
        OwnedBuffer& operator=(OwnedBuffer&& other) noexcept;

        void reset() noexcept;
    };

    struct ModelStructures {
        OwnedBuffer clas;
        OwnedBuffer blas;
        std::vector<vk::DeviceAddress> blasAddresses;
        // declared before micromaps: members are destroyed in reverse order, the micromaps first
        OwnedBuffer micromapStorage;
        std::vector<vk::raii::MicromapEXT> micromaps;
        std::vector<uint8_t> micromapFlags;
    };

    enum class BufferMemory : uint8_t {
        DeviceLocal,
        HostWrite,
        HostReadback,
    };

    [[nodiscard]] OwnedBuffer createOwnedBuffer(vk::DeviceSize bytes, vk::DeviceSize alignment,
                                                vk::BufferUsageFlags2 usage, BufferMemory memoryMode,
                                                std::string_view debugName) const;
    // one submit on the build queue, waited on
    template <typename Record>
    void submit(Record&& record);

    const vk::raii::Device& device;
    const std::vector<uint32_t>& queueFamilyIndices;
    const vk::PhysicalDeviceClusterAccelerationStructurePropertiesNV& limits;
    // micromap build scratch alignment
    vk::DeviceSize scratchOffsetAlignment;
    const VkAllocator& allocator;
    FrameSync& frameSync;
    // indexed by ModelId
    std::vector<ModelStructures> models;
};
