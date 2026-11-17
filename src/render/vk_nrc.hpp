#pragma once

#include "../Constants.h"
#include "core/types.hpp"
#include "core/vk_allocator.hpp"
#include "core/vk_device.hpp"
#include "nrc_library.hpp"
#include "util/vk_utils.hpp"
#include "vk_driver_heap.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vulkan/vulkan_raii.hpp>

// GpuNrcConstants.flags; matches shaders/gi/nrc_constants.slang
inline constexpr uint32_t kNrcFlagCacheView = 1u;

// GpuFrameRoot.cacheConstants (M4 G3): the SDK's constants and the device addresses and row counts of its buffers.
// Mirrored in shaders/gi/nrc_constants.slang; tests/layout_test.cpp checks it
struct GpuNrcConstants
{
    NrcConstants nrc;
    vk::DeviceAddress queryPathInfo = 0;
    vk::DeviceAddress trainingPathInfo = 0;
    vk::DeviceAddress trainingPathVertices = 0;
    vk::DeviceAddress queryRadianceParams = 0;
    // float3 per query, written by QueryAndTrain
    vk::DeviceAddress queryRadiance = 0;
    vk::DeviceAddress counters = 0;
    uint32_t queryPathInfoCount = 0;
    uint32_t trainingPathInfoCount = 0;
    uint32_t trainingPathVertexCount = 0;
    // rows of queryRadianceParams and queryRadiance
    uint32_t queryRadianceCount = 0;
    uint32_t flags = 0;
    // used training dimensions, width | height << 16: the update dispatch
    uint32_t trainingLaunch = 0;
};
static_assert(sizeof(NrcConstants) == 96);
static_assert(sizeof(GpuNrcConstants) == 168);
static_assert(offsetof(GpuNrcConstants, queryPathInfo) == 96);
static_assert(offsetof(GpuNrcConstants, queryPathInfoCount) == 144);

// NVIDIA Neural Radiance Cache (spec/m4/g3_nrc.md, engine spec §11.4): the SDK context at render resolution, its
// per-frame settings and constants, and the update (training) paths. The query paths are the indirect pass
// (IndirectPass), the resolve a frame compute shader; the renderer records them in between
class NrcCache
{
public:
    // read when a frame is recorded; the UI changes it between frames
    struct Settings
    {
        // FrameSettings::trainTheCache: off freezes the cache and skips the update paths
        bool isTrainingEnabled = true;
        // FrameSettings::numTrainingIterations, 16K training records each; the RTXGI sample trains once per frame
        uint32_t trainingIterations = 1;
    };

    NrcCache(const Device& device, const VkAllocator& allocator, const NrcLibrary& library);
    ~NrcCache();

    NrcCache(const NrcCache&) = delete;
    NrcCache& operator=(const NrcCache&) = delete;
    NrcCache(NrcCache&&) = delete;
    NrcCache& operator=(NrcCache&&) = delete;

    // device idle: the frame dimensions; configures once scene bounds exist
    void recreate(vk::Extent2D renderExtent);
    // the slot is not in flight. New scene bounds wait for the device and reconfigure. Without a configured cache the
    // slot's constants are zero: the query pass then makes no queries
    void prepareFrame(uint32_t frameSlot, const BoundingBox& sceneBounds);
    // false until a scene with ray instances is configured: the NRC passes are skipped
    [[nodiscard]] bool isActive() const noexcept { return isConfigured; }

    // ray command buffer: BeginFrame, then the slot's constants. Binds the driver's descriptor heap; the caller
    // rebinds the engine heaps. isCacheView: the query pass queries at the primary, the resolve is skipped
    void recordBeginFrame(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot, bool isCacheView);
    // heaps bound, after recordBeginFrame: passes/nrc_update.slang, one thread per training path
    void recordUpdate(const vk::raii::CommandBuffer& cmd, vk::DeviceAddress frameRoot,
                      const vk::raii::ShaderEXT& updateShader) const;
    // after the query pass: inference at the queries, then training. Binds the driver's descriptor heap; the caller
    // rebinds the engine heaps
    void recordTrain(const vk::raii::CommandBuffer& cmd);
    // after the submit that ran this frame's NRC commands, on the same queue
    void endFrame(const vk::raii::Queue& queue);

    [[nodiscard]] vk::DeviceAddress constantsAddress(uint32_t frameSlot) const noexcept
    {
        return constants[frameSlot].address;
    }
    [[nodiscard]] vk::Extent2D getTrainingExtent() const noexcept { return usedTrainingExtent; }

    Settings settings;

private:
    void configure();
    [[nodiscard]] nrc::FrameSettings frameSettings() const;

    const Device& device;
    const VkAllocator& allocator;
    const NrcApi& api;
    nrc::vulkan::Context* context = nullptr;
    DriverDescriptorHeap driverHeap;
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> constants;
    vk::Extent2D renderExtent{};
    BoundingBox configuredBounds{};
    nrc::ContextSettings contextSettings{};
    // GpuNrcConstants minus NrcConstants and flags: refreshed by configure
    GpuNrcConstants bufferRows{};
    vk::Extent2D usedTrainingExtent{};
    bool isConfigured = false;
};
