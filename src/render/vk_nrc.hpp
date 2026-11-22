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
        // NP2: train for this many frames after a camera, light, scene or mode change (annealing Adam LR over the
        // final <= 128 frames), then freeze until the next change. 0 trains every frame (continuous, NVIDIA's default)
        uint32_t idleTrainingFrames = 512;
        // FrameSettings::numTrainingIterations, 16K training records each; the RTXGI sample trains once per frame
        uint32_t trainingIterations = 1;
        // AddQueryResultToOutput uses the custom GI reservoir resolve (passes/nrc_resolve.slang); any other mode runs
        // the SDK's debug Resolve into indirectRadiance (ND5)
        NrcResolveMode resolveMode = NrcResolveMode::AddQueryResultToOutput;
    };

    NrcCache(const Device& device, const VkAllocator& allocator, const NrcLibrary& library);
    ~NrcCache();

    NrcCache(const NrcCache&) = delete;
    NrcCache& operator=(const NrcCache&) = delete;
    NrcCache(NrcCache&&) = delete;
    NrcCache& operator=(NrcCache&&) = delete;

    // device idle: the render extent. Reconfigures only when it outgrows the configured frame (and scene bounds
    // exist): every Configure reloads the SDK's network and leaks its kernels (considerations C-G3-3)
    void recreate(vk::Extent2D renderExtent);
    // the slot is not in flight. New scene bounds wait for the device and reconfigure. Without a configured cache, or
    // paused (no NRC work this frame), the slot's constants are zero: the query pass then makes no queries. A pause
    // keeps context, buffers and weights, and defers new bounds to the resume (one Configure).
    // isSceneChanged: true when camera, sun, light set, dynamic entities or layout changed (restarts NP2 countdown); an
    // indirect-mode switch does not, since NRC's training target is the same in both NRC modes
    void prepareFrame(uint32_t frameSlot, const BoundingBox& sceneBounds, bool isPaused, bool isSceneChanged);
    // false until a scene with ray instances is configured: the NRC passes are skipped
    [[nodiscard]] bool isActive() const noexcept { return isConfigured; }
    // true if this frame runs training paths and weight updates (false when frozen or NP2 idle)
    [[nodiscard]] bool isTrainingActive() const noexcept { return isTrainingFrame; }
    [[nodiscard]] uint32_t getTrainingFramesRemaining() const noexcept { return trainingFramesRemaining; }

    // ray command buffer: BeginFrame, then the slot's constants. Binds the driver's descriptor heap; the caller
    // rebinds the engine heaps. isCacheView: the query pass queries at the primary, the resolve is skipped
    void recordBeginFrame(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot, bool isCacheView);
    // heaps bound, after recordBeginFrame: passes/nrc_update.slang, one thread per training path
    void recordUpdate(const vk::raii::CommandBuffer& cmd, vk::DeviceAddress frameRoot,
                      const vk::raii::ShaderEXT& updateShader) const;
    // after the query pass: inference at the queries, then training. Binds the driver's descriptor heap; the caller
    // rebinds the engine heaps
    void recordTrain(const vk::raii::CommandBuffer& cmd);
    // SDK debug resolve (ND5): writes the active NrcResolveMode into an internal RGBA32F target (matching NRC's
    // Rgba32f storage image descriptor) and blits the renderExtent region into indirectRadiance
    [[nodiscard]] bool isDebugResolveActive() const noexcept;
    void recordDebugResolve(const vk::raii::CommandBuffer& cmd, vk::Image indirectRadiance, vk::Extent2D extent);
    [[nodiscard]] const char* resolveModeComboString() const;
    // after the submit that ran this frame's NRC commands, on the same queue
    void endFrame(const vk::raii::Queue& queue);

    [[nodiscard]] vk::DeviceAddress constantsAddress(uint32_t frameSlot) const noexcept
    {
        return constants[frameSlot].address;
    }
    [[nodiscard]] vk::Extent2D getTrainingExtent() const noexcept { return usedTrainingExtent; }

    Settings settings;

private:
    // one SDK buffer (nrc::BufferIdx), allocated by the engine: named, suballocated, prioritized, freed by us
    struct SdkBuffer
    {
        vk::raii::Buffer buffer = nullptr;
        VmaAllocation memory = nullptr;
        vk::DeviceSize size = 0;
    };

    void configure();
    // device idle: (re)allocates the SDK buffers whose size changed and fills sdkBufferInfo
    void allocateSdkBuffers();
    void destroySdkBuffers();
    void ensureDebugResolveTarget();
    void destroyDebugResolveTarget();
    [[nodiscard]] nrc::FrameSettings frameSettings() const;

    const Device& device;
    const VkAllocator& allocator;
    const NrcApi& api;
    nrc::vulkan::Context* context = nullptr;
    DriverDescriptorHeap driverHeap;
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> constants;
    std::array<SdkBuffer, static_cast<size_t>(nrc::BufferIdx::Count)> sdkBuffers;
    nrc::vulkan::Buffers sdkBufferInfo{};
    // lazily allocated RGBA32F target for NRC's built-in Resolve debug pass (0 bytes in normal rendering)
    vk::raii::Image debugResolveImage = nullptr;
    VmaAllocation debugResolveMemory = nullptr;
    vk::raii::ImageView debugResolveView = nullptr;
    vk::Extent2D debugResolveExtent{};
    bool isDebugResolveInitialized = false;
    // render extent of the current swapchain; the SDK frame (contextSettings.frameDimensions) only grows past it
    vk::Extent2D renderExtent{};
    BoundingBox configuredBounds{};
    nrc::ContextSettings contextSettings{};
    // GpuNrcConstants minus NrcConstants and flags: refreshed by configure
    GpuNrcConstants bufferRows{};
    vk::Extent2D usedTrainingExtent{};
    bool isConfigured = false;
    // NP2 adaptive training state
    uint32_t trainingFramesRemaining = 0;
    bool isTrainingFrame = true;
    bool wasPaused = true;
    bool lastIsTrainingEnabled = true;
    uint32_t lastTrainingIterations = 1;
    uint32_t lastIdleTrainingFrames = 0;
};
