#pragma once

#include "../Constants.h"
#include "core/vk_allocator.hpp"
#include "core/vk_device.hpp"
#include "util/vk_utils.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>
#include <glm/vec3.hpp>
#include <vulkan/vulkan_raii.hpp>

// Debug visualization modes for DebugView::SharcCache (spec/m8/m8_4_sharc_cache.md §4.4)
enum class SharcDebugMode : uint8_t
{
    CachedRadiance = 0,
    ColoredHash,
    Occupancy,
    HashCollisions,
    VoxelSize,
    GridLevel,
    Count,
};
inline constexpr size_t kSharcDebugModeCount = std::to_underlying(SharcDebugMode::Count);

[[nodiscard]] constexpr const char* sharcDebugModeLabel(SharcDebugMode mode) noexcept
{
    switch (mode) {
    case SharcDebugMode::CachedRadiance:
        return "Cached radiance";
    case SharcDebugMode::ColoredHash:
        return "Colored hash";
    case SharcDebugMode::Occupancy:
        return "Occupancy";
    case SharcDebugMode::HashCollisions:
        return "Hash collisions";
    case SharcDebugMode::VoxelSize:
        return "Voxel size";
    case SharcDebugMode::GridLevel:
        return "Grid level";
    case SharcDebugMode::Count:
        return "";
    }
    return "";
}

// GpuSharcConstants::flags; matches shaders/gi/sharc_constants.slang
namespace SharcFlag
{
    inline constexpr uint32_t MaterialDemodulation = 1u << 0;
    inline constexpr uint32_t DebugModeShift = 4u;
    inline constexpr uint32_t DebugModeMask = 0x7u << DebugModeShift;
} // namespace SharcFlag

// Device-local counters copied to host-cached readback after sharc_resolve (mirrored in shaders/gi/sharc_constants.slang)
struct GpuSharcCounters
{
    uint32_t occupied = 0;       // valid non-empty hash-table entries after sharc_resolve
    uint32_t insertFailures = 0; // HashGridInsertEntry bucket overflows during sharc_update
};
static_assert(sizeof(GpuSharcCounters) == 8);
static_assert(offsetof(GpuSharcCounters, occupied) == 0 && offsetof(GpuSharcCounters, insertFailures) == 4);

// GpuFrameRoot::sharcConstants (M8.4); mirrored in shaders/gi/sharc_constants.slang, checked in tests/layout_test.cpp
struct GpuSharcConstants
{
    vk::DeviceAddress hashEntries = 0;
    vk::DeviceAddress accumulation = 0;
    vk::DeviceAddress resolved = 0;
    glm::vec3 cameraPos{0.0f};
    float sceneScale = 35.0f;
    glm::vec3 prevCameraPos{0.0f};
    float radianceScale = 1000.0f;
    uint32_t capacity = 1u << 22;
    uint32_t downscaleFactor = 4;
    uint32_t accumulationFrameNum = 32;
    uint32_t staleFrameNumMax = 64;
    uint32_t maxUpdateBounces = 4;
    uint32_t maxQueryBounces = 2;
    float minRoughnessOverride = 0.4f;
    float fireflyClamp = 0.0f;
    uint32_t flags = 0;
    // width | (height << 16) of the sparse sharc_update dispatch
    uint32_t updateLaunch = 0;
    vk::DeviceAddress counters = 0;
};
static_assert(sizeof(GpuSharcConstants) == 104);
static_assert(offsetof(GpuSharcConstants, hashEntries) == 0);
static_assert(offsetof(GpuSharcConstants, accumulation) == 8);
static_assert(offsetof(GpuSharcConstants, resolved) == 16);
static_assert(offsetof(GpuSharcConstants, cameraPos) == 24);
static_assert(offsetof(GpuSharcConstants, sceneScale) == 36);
static_assert(offsetof(GpuSharcConstants, prevCameraPos) == 40);
static_assert(offsetof(GpuSharcConstants, radianceScale) == 52);
static_assert(offsetof(GpuSharcConstants, capacity) == 56);
static_assert(offsetof(GpuSharcConstants, flags) == 88);
static_assert(offsetof(GpuSharcConstants, updateLaunch) == 92);
static_assert(offsetof(GpuSharcConstants, counters) == 96);

// Pure Vulkan 1.4 BDA Spatial Hash Radiance Cache (NVIDIA SHaRC v1.8.3, spec/m8/m8_4_sharc_cache.md).
// Lazily allocates 3 device-local buffers (40 B/voxel) on first active frame and retires them after
// MAX_FRAMES_IN_FLIGHT inactive frames or capacity changes with zero waitIdle.
class SharcCache
{
public:
    static constexpr uint32_t kMinCapacityShift = 20;
    static constexpr uint32_t kMaxCapacityShift = 23;
    static constexpr uint32_t kMinDownscaleFactor = 2;
    static constexpr uint32_t kMaxDownscaleFactor = 5;
    static constexpr uint32_t kMinAccumulationFrames = 1;
    static constexpr uint32_t kMaxAccumulationFrames = 256;
    static constexpr uint32_t kMinStaleFrames = 8;
    static constexpr uint32_t kMaxStaleFrames = 256;
    static constexpr uint32_t kMinUpdateBounces = 2;
    static constexpr uint32_t kMaxUpdateBounces = 8;
    static constexpr uint32_t kMinQueryBounces = 1;
    static constexpr uint32_t kMaxQueryBounces = 4;
    static constexpr float kMinSceneScale = 1.0f;
    static constexpr float kMaxSceneScale = 100.0f;
    static constexpr float kDefaultRadianceScale = 1000.0f;
    // 8 B (uint64_t hashKey) + 16 B (SharcAccumulationData) + 16 B (SharcPackedData)
    static constexpr vk::DeviceSize kBytesPerEntry = 40;

    struct Settings
    {
        bool isUpdateEnabled = true;
        bool isResolveEnabled = true;
        bool isUpdateViewCamera = true;
        bool isMaterialDemodulationEnabled = true;
        bool isContinuousClearEnabled = false;
        uint32_t downscaleFactor = 4;
        uint32_t capacityShift = 22;
        uint32_t accumulationFrameNum = 32;
        uint32_t staleFrameNumMax = 64;
        uint32_t maxUpdateBounces = 4;
        uint32_t maxQueryBounces = 2;
        float sceneScale = 35.0f;
        float minRoughnessOverride = 0.4f;
        // Optional scene-unit luminance clamp per update vertex (0 = off, matching gi.fireflyClamp; unlike RTXPT's
        // exposure-scaled FireflyFilterThreshold in Sample.cpp:2202)
        float fireflyClamp = 0.0f;
        SharcDebugMode debugMode = SharcDebugMode::CachedRadiance;

        bool operator==(const Settings&) const = default;
    };

    SharcCache(const Device& device, const VkAllocator& allocator);
    ~SharcCache();

    SharcCache(const SharcCache&) = delete;
    SharcCache& operator=(const SharcCache&) = delete;
    SharcCache(SharcCache&&) = delete;
    SharcCache& operator=(SharcCache&&) = delete;

    // after slot fence: reads back counters, drains retired allocations, lazily creates/resizes/retires cache buffers,
    // updates constants. Returns true when settings/clear/scene state changed so the renderer resets history
    [[nodiscard]] bool prepareFrame(uint32_t frameSlot, uint32_t frameNumber, bool isActive, vk::Extent2D renderExtent,
                                    const glm::vec3& cameraPos, bool isSceneChanged);

    // records optional vkCmdFillBuffer clear, counter reset, and sparse update dispatch
    void recordUpdate(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot, vk::DeviceAddress frameRoot,
                      const vk::raii::ShaderEXT& updateShader);
    // records update -> resolve barrier, resolve dispatch, counter readback, and resolve -> query barrier
    void recordResolve(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot, vk::DeviceAddress frameRoot,
                       const vk::raii::ShaderEXT& resolveShader);

    void requestClear() noexcept { isClearRequested = true; }

    [[nodiscard]] vk::DeviceAddress constantsAddress(uint32_t frameSlot) const noexcept
    {
        return isFrameActive ? constants[frameSlot].address : 0;
    }
    [[nodiscard]] bool isActiveThisFrame() const noexcept { return isFrameActive; }
    [[nodiscard]] uint32_t getAllocatedCapacity() const noexcept { return allocatedCapacity; }
    [[nodiscard]] vk::DeviceSize getAllocatedBytes() const noexcept
    {
        return static_cast<vk::DeviceSize>(allocatedCapacity) * kBytesPerEntry;
    }
    [[nodiscard]] vk::Extent2D getUpdateLaunchExtent() const noexcept { return updateLaunchExtent; }
    [[nodiscard]] GpuSharcCounters getLastCounters() const noexcept { return lastCounters; }

    Settings settings;

private:
    struct CacheBuffer
    {
        vk::raii::Buffer buffer = nullptr;
        VmaAllocation memory = nullptr;
        vk::DeviceAddress address = 0;
        vk::DeviceSize size = 0;
    };

    struct CacheBufferSet
    {
        CacheBuffer hashEntries;
        CacheBuffer accumulation;
        CacheBuffer resolved;
        uint32_t capacity = 0;
        uint32_t lastUsedFrame = 0;
    };

    void allocateBuffers(uint32_t capacity);
    void destroyBufferSet(CacheBufferSet& set);
    void drainRetiredBuffers(uint32_t frameNumber);

    const Device& device;
    const VkAllocator& allocator;
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> constants;
    std::array<CounterReadback, MAX_FRAMES_IN_FLIGHT> counters;
    std::array<bool, MAX_FRAMES_IN_FLIGHT> isCountersRecorded{};
    GpuSharcCounters lastCounters{};
    CacheBufferSet activeBuffers;
    std::vector<CacheBufferSet> retiredBuffers;
    uint32_t allocatedCapacity = 0;
    vk::Extent2D updateLaunchExtent{};
    glm::vec3 anchorCameraPos{0.0f};
    glm::vec3 prevAnchorCameraPos{0.0f};
    bool hasAnchorCamera = false;
    bool isFrameActive = false;
    bool isClearPending = false;
    bool isClearRequested = false;
    Settings appliedSettings{};
    bool hasAppliedSettings = false;
};
