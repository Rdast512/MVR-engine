#pragma once
#include <optional>
#include <string_view>
#include <vulkan/vulkan_raii.hpp>
#include "../core/types.hpp"
#include "Constants.h"
#include "fmt/chrono.h"
#include "geometry_store.hpp"
#include "material_store.hpp"
#include "object_storage.hpp"
#include "scene/vk_camera.hpp"
#include "vk_allocator.hpp"
#include "vk_device.hpp"

// manages GPU buffers, images, and descriptors
class ResourceManager
{
public:
    ResourceManager(const Device& deviceWrapper, const VkAllocator& allocator, GeometryStore& geometryStore,
                    MaterialStore& materialStore, ObjectStorage& objectStorage);
    ~ResourceManager();

    void init();
    void createSyncObjects();
    void updateUniformBuffers(uint32_t currentImage);
    void createCommandPool();
    void createCommandBuffers();
    void createDepthResources();
    void createVertexBuffer();
    void createMeshBuffers();
    void createIndirectBuffer();
    // grow instance buffers to entity capacity
    void ensureInstanceCapacity(uint32_t entityCount);
    void createUniformBuffers();
    void createColorResources();
    void recreateObjectsBuffers();
    void createCameraBuffers(Camera& camera);
    void setSwapChainImageCount(uint32_t count)
    {
        swapChainImageCount = count;
        createSyncObjects();
    }

    // plot tracy resource metrics
    void tracyPlotResources() const;

    [[nodiscard]] vk::DeviceAddress instanceUboAddress(uint32_t frameSlot, EntityId entityId) const noexcept;


    void createImage(uint32_t width, uint32_t height, uint32_t mipLevels, vk::SampleCountFlagBits samples,
                     vk::Format format, vk::ImageTiling tiling, vk::ImageUsageFlags usage,
                     vk::MemoryPropertyFlags properties, vk::raii::Image& image, VmaAllocation& imageMemory,
                     std::string_view memoryDebugBaseName = "ResourceImageMemory");
    vk::raii::ImageView createImageView(vk::raii::Image& image, vk::Format format, vk::ImageAspectFlags aspectFlags,
                                        uint32_t mipLevels);
    void copyBuffer(vk::raii::Buffer& srcBuffer, vk::raii::Buffer& dstBuffer, vk::DeviceSize size);
    uint32_t findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties);

    vk::Format findSupportedFormat(const std::vector<vk::Format>& candidates, vk::ImageTiling tiling,
                                   vk::FormatFeatureFlags features);
    vk::Format findDepthFormat();
    void copyBufferToImage(const vk::raii::Buffer& buffer, vk::raii::Image& image, uint32_t width, uint32_t height);
    static bool hasStencilComponent(vk::Format format);
    void generateMipmaps(vk::raii::Image& image, vk::Format imageFormat, int32_t texWidth, int32_t texHeight,
                         uint32_t mipLevels);
    static void endCommandBuffer(vk::raii::CommandBuffer& commandBuffer, const vk::raii::Queue& queue);
    void updateSwapChainExtent(vk::Extent2D newExtent);
    void updateSwapChainImageFormat(vk::Format newFormat) { swapChainImageFormat = newFormat; }

    const Device& deviceWrapper;
    const VkAllocator& allocator;
    const vk::raii::PhysicalDevice& physicalDevice;
    const vk::raii::Device& device;
    const std::vector<uint32_t>& queueFamilyIndices;
    const vk::raii::Queue& graphicsQueue;
    const vk::raii::Queue& transferQueue;
    const HardwareCapabilities hardwareCapabilities;
    ObjectStorage& objectStorage;
    GeometryStore& geometryStore;
    MaterialStore& materialStore;
    uint32_t graphicsIndex;
    uint32_t transferIndex;
    vk::SampleCountFlagBits msaaSamples;
    vk::Extent2D swapChainExtent{};
    const std::vector<GpuVertex>& vertices;
    const std::vector<GpuMeshletDesc>& meshlets;
    const std::vector<uint32_t>& meshletVertices;
    const std::vector<uint8_t>& meshletTriangles;
    uint32_t swapChainImageCount = 0;
    vk::Format swapChainImageFormat = vk::Format::eUndefined;

    // synchronization primitives
    std::vector<vk::raii::Semaphore> presentCompleteSemaphore;
    std::vector<vk::raii::Semaphore> renderFinishedSemaphore;
    std::vector<vk::raii::Fence> inFlightFences;
    vk::raii::Image depthImage = nullptr;
    VmaAllocation depthImageMemory = nullptr;
    vk::raii::ImageView depthImageView = nullptr;
    vk::raii::CommandPool commandPool = nullptr;
    vk::raii::CommandPool transferCommandPool = nullptr;
    std::vector<vk::raii::CommandBuffer> commandBuffers;
    std::vector<vk::raii::CommandBuffer> transferCommandBuffer;
    vk::raii::Buffer vertexBuffer = nullptr;
    VmaAllocation vertexBufferMemory = nullptr;
    vk::raii::Buffer stagingBuffer = nullptr;
    VmaAllocation stagingBufferMemory = nullptr;
    vk::raii::Buffer indirectBuffer = nullptr;
    VmaAllocation indirectBufferMemory = nullptr;
    vk::raii::Image colorImage = nullptr;
    VmaAllocation colorImageMemory = nullptr;
    vk::raii::ImageView colorImageView = nullptr;

    // device-local meshlet buffers accessed via BDA
    vk::raii::Buffer meshletBuffer = nullptr; // GpuMeshletDesc[]
    VmaAllocation meshletBufferMemory = nullptr;
    vk::raii::Buffer meshletVertexBuffer = nullptr; // uint32_t[] remap
    VmaAllocation meshletVertexBufferMemory = nullptr;
    vk::raii::Buffer meshletTriangleBuffer = nullptr; // uint8_t[] local corners
    VmaAllocation meshletTriangleBufferMemory = nullptr;

    // cached device addresses
    vk::DeviceAddress vertexBufferAddress = 0;
    vk::DeviceAddress meshletBufferAddress = 0;
    vk::DeviceAddress meshletVertexBufferAddress = 0;
    vk::DeviceAddress meshletTriangleBufferAddress = 0;
    vk::DeviceAddress indirectBufferAddress = 0;

    // per-frame instance UBO buffers
    std::array<vk::raii::Buffer, MAX_FRAMES_IN_FLIGHT> instanceUboBuffers = {nullptr, nullptr};
    std::array<VmaAllocation, MAX_FRAMES_IN_FLIGHT> instanceUboMemory = {nullptr, nullptr};
    std::array<void*, MAX_FRAMES_IN_FLIGHT> instanceUboMapped = {nullptr, nullptr};
    std::array<vk::DeviceAddress, MAX_FRAMES_IN_FLIGHT> instanceUboBaseAddresses = {0, 0};
    uint32_t instanceCapacity = 0;

private:
    void destroyInstanceUboBuffers();
    // Track last-known sizes for Tracy free/realloc pairing.
    vk::DeviceSize trackedVertexBytes = 0;
    vk::DeviceSize trackedMeshletBytes = 0;
    vk::DeviceSize trackedMeshletVertexBytes = 0;
    vk::DeviceSize trackedMeshletTriangleBytes = 0;
    vk::DeviceSize trackedColorBytes = 0;
    vk::DeviceSize trackedDepthBytes = 0;
    std::array<vk::DeviceSize, MAX_FRAMES_IN_FLIGHT> trackedInstanceUboBytes = {0, 0};
};
