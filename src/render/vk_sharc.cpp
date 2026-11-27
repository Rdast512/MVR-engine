#include "vk_sharc.hpp"
#include "../util/vk_tracy.hpp"
#include "push_data.hpp"
#include "static_headers/logger.hpp"
#include "vk_shader_objects.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace
{
    constexpr const char* kSharcTracyName = "GPU/SharcCache";
    constexpr uint32_t kSharcUpdateTile = 8;
    constexpr uint32_t kSharcResolveGroupSize = 256;
    constexpr vk::DeviceSize kHashEntryBytes = sizeof(uint64_t);
    constexpr vk::DeviceSize kAccumulationBytes = 16; // uint4 (SharcAccumulationData, SHARC_ENABLE_SH_ENCODING=0)
    constexpr vk::DeviceSize kResolvedBytes = 16;     // float16_t4 + 2x uint (SharcPackedData)
    static_assert(kHashEntryBytes + kAccumulationBytes + kResolvedBytes == SharcCache::kBytesPerEntry);

    constexpr uint32_t divideRoundUp(uint32_t value, uint32_t divisor) noexcept
    {
        return (value + divisor - 1u) / divisor;
    }

    bool isCacheEncodingChanged(const SharcCache::Settings& a, const SharcCache::Settings& b) noexcept
    {
        return a.isMaterialDemodulationEnabled != b.isMaterialDemodulationEnabled || a.sceneScale != b.sceneScale;
    }

    bool isLightingSettingsChanged(SharcCache::Settings a, const SharcCache::Settings& b) noexcept
    {
        a.debugMode = b.debugMode;
        return a != b;
    }
} // namespace

SharcCache::SharcCache(const Device& device, const VkAllocator& allocator) : device(device), allocator(allocator)
{
    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        constants[slot] =
            createHostBuffer(sizeof(GpuSharcConstants), allocator.allocator, device.vkdevice, device.queueFamilyIndices,
                             std::format("SharcConstants_{}", slot), nullptr);
        *static_cast<GpuSharcConstants*>(constants[slot].mapped) = {};
        counters[slot] = createCounterReadback(sizeof(GpuSharcCounters), allocator.allocator, device.vkdevice,
                                               device.queueFamilyIndices, std::format("SharcCounters_{}", slot));
    }
}

SharcCache::~SharcCache()
{
    for (CacheBufferSet& retired : retiredBuffers) {
        destroyBufferSet(retired);
    }
    retiredBuffers.clear();
    destroyBufferSet(activeBuffers);
    allocatedCapacity = 0;
    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        destroyCounterReadback(allocator.allocator, counters[slot]);
        destroyHostBuffer(allocator.allocator, constants[slot], nullptr);
    }
}

void SharcCache::allocateBuffers(uint32_t capacity)
{
    ZoneScopedN("SharcCache::allocateBuffers");
    const auto createCacheBuffer = [&](vk::DeviceSize bytes, std::string_view name) {
        CacheBuffer out;
        out.size = bytes;
        createBuffer(bytes,
                     vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress |
                         vk::BufferUsageFlagBits2::eTransferDst,
                     vk::MemoryPropertyFlagBits::eDeviceLocal, out.buffer, out.memory, allocator.allocator,
                     device.vkdevice, device.queueFamilyIndices, name);
        out.address = device.vkdevice.getBufferAddress({.buffer = *out.buffer});
        tracyResourceAlloc(static_cast<VkBuffer>(*out.buffer), static_cast<size_t>(bytes), kSharcTracyName);
        return out;
    };

    const auto count = static_cast<vk::DeviceSize>(capacity);
    CacheBufferSet next;
    try {
        next.hashEntries = createCacheBuffer(count * kHashEntryBytes, "SharcHashEntries");
        next.accumulation = createCacheBuffer(count * kAccumulationBytes, "SharcAccumulation");
        next.resolved = createCacheBuffer(count * kResolvedBytes, "SharcResolved");
    } catch (...) {
        next.capacity = capacity;
        destroyBufferSet(next);
        throw;
    }
    next.capacity = capacity;

    if (activeBuffers.capacity != 0) {
        retiredBuffers.push_back(std::move(activeBuffers));
        activeBuffers = {};
    }
    activeBuffers = std::move(next);
    allocatedCapacity = capacity;
    isClearPending = true;

    const double totalMiB = static_cast<double>(count * kBytesPerEntry) / (1024.0 * 1024.0);
    logInfo(std::format("SHaRC buffers allocated: {} entries ({:.1f} MiB)", capacity, totalMiB), "SHaRC");
}

void SharcCache::destroyBufferSet(CacheBufferSet& set)
{
    if (set.capacity == 0) {
        return;
    }
    destroyVmaBuffer(allocator.allocator, set.hashEntries.buffer, set.hashEntries.memory, kSharcTracyName);
    destroyVmaBuffer(allocator.allocator, set.accumulation.buffer, set.accumulation.memory, kSharcTracyName);
    destroyVmaBuffer(allocator.allocator, set.resolved.buffer, set.resolved.memory, kSharcTracyName);
    set = {};
}

void SharcCache::drainRetiredBuffers(uint32_t frameNumber)
{
    auto it = retiredBuffers.begin();
    while (it != retiredBuffers.end()) {
        if (frameNumber >= it->lastUsedFrame + MAX_FRAMES_IN_FLIGHT) {
            destroyBufferSet(*it);
            it = retiredBuffers.erase(it);
        } else {
            ++it;
        }
    }
}

bool SharcCache::prepareFrame(uint32_t frameSlot, uint32_t frameNumber, bool isActive, vk::Extent2D renderExtent,
                              const glm::vec3& cameraPos, bool isSceneChanged)
{
    if (isCountersRecorded[frameSlot]) {
        lastCounters = *static_cast<const GpuSharcCounters*>(counters[frameSlot].readback.mapped);
        isCountersRecorded[frameSlot] = false;
    }
    drainRetiredBuffers(frameNumber);

    if (!isActive) {
        isFrameActive = false;
        lastCounters = {};
        if (activeBuffers.capacity != 0 && frameNumber >= activeBuffers.lastUsedFrame + MAX_FRAMES_IN_FLIGHT) {
            const double freedMiB = static_cast<double>(getAllocatedBytes()) / (1024.0 * 1024.0);
            destroyBufferSet(activeBuffers);
            allocatedCapacity = 0;
            hasAnchorCamera = false;
            hasAppliedSettings = false;
            logInfo(std::format("SHaRC buffers retired ({:.1f} MiB freed)", freedMiB), "SHaRC");
        }
        return false;
    }

    const uint32_t clampedShift = std::clamp(settings.capacityShift, kMinCapacityShift, kMaxCapacityShift);
    const uint32_t targetCapacity = 1u << clampedShift;
    const uint32_t downscale = std::clamp(settings.downscaleFactor, kMinDownscaleFactor, kMaxDownscaleFactor);
    const uint32_t accumFrames =
        std::clamp(settings.accumulationFrameNum, kMinAccumulationFrames, kMaxAccumulationFrames);
    const uint32_t staleFrames = std::clamp(settings.staleFrameNumMax, kMinStaleFrames, kMaxStaleFrames);
    const uint32_t updateBounces = std::clamp(settings.maxUpdateBounces, kMinUpdateBounces, kMaxUpdateBounces);
    const uint32_t queryBounces = std::clamp(settings.maxQueryBounces, kMinQueryBounces, kMaxQueryBounces);
    const float sceneScale = std::clamp(settings.sceneScale, kMinSceneScale, kMaxSceneScale);
    const float minRoughness = std::clamp(settings.minRoughnessOverride, 0.0f, 1.0f);
    const float fireflyClamp = std::max(settings.fireflyClamp, 0.0f);

    const bool isAllocationNeeded = activeBuffers.capacity != targetCapacity;
    if (isAllocationNeeded) {
        allocateBuffers(targetCapacity);
    }

    const bool isEncodingChanged = hasAppliedSettings && isCacheEncodingChanged(settings, appliedSettings);
    // Turning resolve back on after update ran un-resolved would merge wrapped uint32 accumulation sums into history
    const bool isResolveReenabled = hasAppliedSettings && !appliedSettings.isResolveEnabled && settings.isResolveEnabled;
    const bool isLightingChanged = hasAppliedSettings && isLightingSettingsChanged(settings, appliedSettings);
    if (isEncodingChanged || isResolveReenabled || isSceneChanged || isClearRequested ||
        settings.isContinuousClearEnabled) {
        isClearPending = true;
        isClearRequested = false;
    }

    if (!hasAnchorCamera || isClearPending) {
        anchorCameraPos = cameraPos;
        prevAnchorCameraPos = cameraPos;
        hasAnchorCamera = true;
    } else if (settings.isUpdateViewCamera && settings.isUpdateEnabled) {
        prevAnchorCameraPos = anchorCameraPos;
        anchorCameraPos = cameraPos;
    } else {
        prevAnchorCameraPos = anchorCameraPos;
    }

    updateLaunchExtent = {
        std::max(divideRoundUp(renderExtent.width, downscale), 1u),
        std::max(divideRoundUp(renderExtent.height, downscale), 1u),
    };
    const uint32_t debugModeBits =
        (static_cast<uint32_t>(std::to_underlying(settings.debugMode)) << SharcFlag::DebugModeShift) &
        SharcFlag::DebugModeMask;
    const uint32_t flags = (settings.isMaterialDemodulationEnabled ? SharcFlag::MaterialDemodulation : 0u) |
        debugModeBits;

    *static_cast<GpuSharcConstants*>(constants[frameSlot].mapped) = GpuSharcConstants{
        .hashEntries = activeBuffers.hashEntries.address,
        .accumulation = activeBuffers.accumulation.address,
        .resolved = activeBuffers.resolved.address,
        .cameraPos = anchorCameraPos,
        .sceneScale = sceneScale,
        .prevCameraPos = prevAnchorCameraPos,
        .radianceScale = kDefaultRadianceScale,
        .capacity = activeBuffers.capacity,
        .downscaleFactor = downscale,
        .accumulationFrameNum = accumFrames,
        .staleFrameNumMax = staleFrames,
        .maxUpdateBounces = updateBounces,
        .maxQueryBounces = queryBounces,
        .minRoughnessOverride = minRoughness,
        .fireflyClamp = fireflyClamp,
        .flags = flags,
        .updateLaunch = (updateLaunchExtent.width & 0xFFFFu) | (updateLaunchExtent.height << 16u),
        .counters = counters[frameSlot].address,
    };

    activeBuffers.lastUsedFrame = frameNumber;
    isFrameActive = true;
    appliedSettings = settings;
    hasAppliedSettings = true;

    return isAllocationNeeded || isClearPending || isLightingChanged;
}

void SharcCache::recordUpdate(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot, vk::DeviceAddress frameRoot,
                              const vk::raii::ShaderEXT& updateShader)
{
    if (!isFrameActive || activeBuffers.capacity == 0) {
        return;
    }
    ZoneScopedN("SharcCache::recordUpdate");

    // rayStartBarrier (vk_renderer.cpp) already orders previous frame's compute work before this frame's compute
    // dispatches; only vkCmdFillBuffer (eClear) needs an explicit compute -> transfer-write barrier when clearing
    if (isClearPending) {
        const vk::MemoryBarrier2 preClearBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eClear,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &preClearBarrier});
        cmd.fillBuffer(*activeBuffers.hashEntries.buffer, 0, vk::WholeSize, 0);
        cmd.fillBuffer(*activeBuffers.accumulation.buffer, 0, vk::WholeSize, 0);
        cmd.fillBuffer(*activeBuffers.resolved.buffer, 0, vk::WholeSize, 0);
        isClearPending = false;
        const vk::MemoryBarrier2 clearBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eClear,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &clearBarrier});
    }

    // Per-slot counters (host-waited by slot fence); resetBarrier only waits on eClear, never stalling DirectLight
    recordCounterReset(cmd, counters[frameSlot]);

    if (settings.isUpdateEnabled) {
        const FramePushData push{.frameRoot = frameRoot};
        ShaderObjects::bindComputeShader(cmd, updateShader);
        cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
        cmd.dispatch(divideRoundUp(updateLaunchExtent.width, kSharcUpdateTile),
                     divideRoundUp(updateLaunchExtent.height, kSharcUpdateTile), 1);
    }
}

void SharcCache::recordResolve(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot, vk::DeviceAddress frameRoot,
                               const vk::raii::ShaderEXT& resolveShader)
{
    if (!isFrameActive || activeBuffers.capacity == 0) {
        return;
    }
    ZoneScopedN("SharcCache::recordResolve");

    if (settings.isResolveEnabled) {
        // Orders sharc_update's atomic hashEntries/accumulation writes and resolved resampling reads before resolve
        const vk::MemoryBarrier2 updateToResolve{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &updateToResolve});

        const FramePushData push{.frameRoot = frameRoot};
        ShaderObjects::bindComputeShader(cmd, resolveShader);
        cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
        cmd.dispatch(activeBuffers.capacity / kSharcResolveGroupSize, 1, 1);
    }

    recordCounterReadback(cmd, counters[frameSlot]);
    isCountersRecorded[frameSlot] = true;

    if (settings.isResolveEnabled || settings.isUpdateEnabled) {
        // Orders hashEntries and resolved writes before indirect.slang and debug_view.slang queries
        const vk::MemoryBarrier2 resolveToQuery{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &resolveToQuery});
    }
}
