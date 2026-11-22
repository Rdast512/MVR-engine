#include "vk_nrc.hpp"
#include "../util/vk_tracy.hpp"
#include "push_data.hpp"
#include "vk_shader_objects.hpp"
#include "static_headers/logger.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
    constexpr std::string_view kSubsystem = "NRC";

    // the RTXGI IndirectDiffuse sample's values (spec/m4/g3_nrc.md §1)
    constexpr uint32_t kMaxPathVertices = 8;
    constexpr float kSmallestResolvableFeatureSize = 0.01f;
    constexpr float kMaxExpectedAverageRadiance = 1.0f;
    constexpr float kTerminationHeuristicThreshold = 0.01f;
    constexpr float kPrimarySegmentsToTrainOn = 0.02f;
    // scene bounds grow by this share of their extent: geometry on the boundary must stay inside
    constexpr float kBoundsMargin = 0.01f;

    void checkNrc(nrc::Status status, std::string_view what)
    {
        if (status != nrc::Status::OK) {
            throw std::runtime_error(std::format("NRC {} failed: status {}", what, static_cast<int>(status)));
        }
    }

    void logNrc(const char* message, nrc::LogLevel level)
    {
        // the SDK ends its messages with newlines
        std::string_view text(message);
        while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
            text.remove_suffix(1);
        }
        // QueryAndTrain is the only Vulkan inference entry point; with trainTheCache = false (NP2 idle or Train NRC
        // off) NRC 0.16 logs this every frame, flooding the log file and adding CPU I/O
        if (text.contains("trainTheCache is `false`")) {
            return;
        }
        if (level == nrc::LogLevel::Error || level == nrc::LogLevel::Warning) {
            logError(text, kSubsystem);
        } else if (level == nrc::LogLevel::Info) {
            logInfo(text, kSubsystem);
        } else {
            logDebug(text, kSubsystem);
        }
    }

    [[nodiscard]] bool isValid(const BoundingBox& box)
    {
        return glm::all(glm::lessThanEqual(box.bboxMin, box.bboxMax)) &&
               glm::any(glm::lessThan(box.bboxMin, box.bboxMax));
    }

    // matches shaders/passes/nrc_update.slang kUpdateTile
    constexpr uint32_t kUpdateTile = 8;

    constexpr uint32_t divideRoundUp(uint32_t value, uint32_t divisor) { return (value + divisor - 1) / divisor; }

    // NRC's kernels are CU launches (VK_NVX_binary_import) with no documented stage or access, so the NRC calls are
    // fenced off from everything around them
    void nrcBoundary(const vk::raii::CommandBuffer& cmd)
    {
        const vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
    }
} // namespace

NrcCache::NrcCache(const Device& device, const VkAllocator& allocator, const NrcLibrary& library) :
    device(device), allocator(allocator), api(library.api()), driverHeap(device, "Nrc")
{
    nrc::GlobalSettings globalSettings{};
    globalSettings.loggerFn = logNrc;
    // the engine allocates the SDK's buffers: SDK-allocated ones are unnamed dedicated blocks without a priority, and
    // Configure / Destroy leak them (considerations C-G3-3)
    globalSettings.enableGPUMemoryAllocation = false;
    // allocates DebugTrainingRadiance (~0.25 MiB at 853x480, VMA-owned) for SDK Resolve debug views (ND5)
    globalSettings.enableDebugBuffers = true;
    globalSettings.maxNumFramesInFlight = MAX_FRAMES_IN_FLIGHT;
    checkNrc(api.initialize(globalSettings), "Initialize");
    checkNrc(api.createContext(*device.vkdevice, *device.physicalDevice, *device.instance, context), "Context::Create");

    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        constants[slot] = createHostBuffer(sizeof(GpuNrcConstants), allocator.allocator, device.vkdevice,
                                           device.queueFamilyIndices, std::format("NrcConstants_{}", slot), nullptr);
    }
    if (const char* idleEnv = std::getenv("ENGINE_NRC_IDLE_FRAMES"); idleEnv != nullptr && *idleEnv != '\0') {
        uint32_t frames = 0;
        const std::string_view view(idleEnv);
        const auto [ptr, ec] = std::from_chars(view.data(), view.data() + view.size(), frames);
        if (ec == std::errc{} && ptr == view.data() + view.size()) {
            settings.idleTrainingFrames = frames;
            logInfo(std::format("NRC idle training frames (NP2): {}", frames), kSubsystem);
        } else {
            logError(std::format("ENGINE_NRC_IDLE_FRAMES='{}' invalid; ignored", view), kSubsystem);
        }
    }
    lastIsTrainingEnabled = settings.isTrainingEnabled;
    lastTrainingIterations = settings.trainingIterations;
    lastIdleTrainingFrames = settings.idleTrainingFrames;
}

NrcCache::~NrcCache()
{
    destroyDebugResolveTarget();
    for (HostBuffer& slot : constants) {
        destroyHostBuffer(allocator.allocator, slot, nullptr);
    }
    if (context != nullptr) {
        api.destroyContext(*context);
    }
    api.shutdown();
    destroySdkBuffers();
}

void NrcCache::recreate(vk::Extent2D extent)
{
    renderExtent = extent;
    const bool isFrameLargeEnough = extent.width <= contextSettings.frameDimensions.x &&
                                    extent.height <= contextSettings.frameDimensions.y;
    if (isValid(configuredBounds) && !(isConfigured && isFrameLargeEnough)) {
        configure();
    }
    if (isConfigured && isDebugResolveActive()) {
        ensureDebugResolveTarget();
    }
}

void NrcCache::prepareFrame(uint32_t frameSlot, const BoundingBox& sceneBounds, bool isPaused, bool isSceneChanged)
{
    if (isPaused) {
        wasPaused = true;
        isTrainingFrame = false;
        std::memset(constants[frameSlot].mapped, 0, sizeof(GpuNrcConstants));
        return;
    }
    const bool isNewScene = isValid(sceneBounds) &&
                            (sceneBounds.bboxMin != configuredBounds.bboxMin ||
                             sceneBounds.bboxMax != configuredBounds.bboxMax);
    if (isNewScene) {
        // Configure may reallocate the buffers every frame in flight uses; a layout change is a load. No requestReset:
        // NRC 0.16 keeps the weights with it (measured, considerations C-G3-5); the network adapts to the new bounds
        device.vkdevice.waitIdle();
        configuredBounds = sceneBounds;
        configure();
        if (isDebugResolveActive()) {
            ensureDebugResolveTarget();
        }
    } else {
        if (!isConfigured) {
            isTrainingFrame = false;
            std::memset(constants[frameSlot].mapped, 0, sizeof(GpuNrcConstants));
            return;
        }
        if (isDebugResolveActive() &&
            (!*debugResolveImage || debugResolveExtent.width != contextSettings.frameDimensions.x ||
             debugResolveExtent.height != contextSettings.frameDimensions.y)) {
            device.vkdevice.waitIdle();
            ensureDebugResolveTarget();
        }
    }
    const bool shouldReloadTraining = isNewScene || wasPaused || isSceneChanged ||
        (settings.isTrainingEnabled && !lastIsTrainingEnabled) ||
        settings.trainingIterations != lastTrainingIterations ||
        settings.idleTrainingFrames != lastIdleTrainingFrames;
    if (shouldReloadTraining) {
        trainingFramesRemaining = settings.idleTrainingFrames;
    }
    wasPaused = false;
    lastIsTrainingEnabled = settings.isTrainingEnabled;
    lastTrainingIterations = settings.trainingIterations;
    lastIdleTrainingFrames = settings.idleTrainingFrames;
    isTrainingFrame = settings.isTrainingEnabled && (settings.idleTrainingFrames == 0 || trainingFramesRemaining > 0);
}

void NrcCache::configure()
{
    ZoneScopedN("NrcCache::configure");
    const glm::vec3 margin = (configuredBounds.bboxMax - configuredBounds.bboxMin) * kBoundsMargin + glm::vec3(1e-3f);
    const glm::vec3 boundsMin = configuredBounds.bboxMin - margin;
    const glm::vec3 boundsMax = configuredBounds.bboxMax + margin;
    contextSettings.learnIrradiance = true;
    // direct light at the query vertex is sampled explicitly (engine spec §11.3): the cache holds the indirect part
    contextSettings.includeDirectLighting = false;
    contextSettings.sceneBoundsMin = {boundsMin.x, boundsMin.y, boundsMin.z};
    contextSettings.sceneBoundsMax = {boundsMax.x, boundsMax.y, boundsMax.z};
    contextSettings.smallestResolvableFeatureSize = kSmallestResolvableFeatureSize;
    // grow-only: a smaller render extent queries inside the configured frame, so shrinking never reconfigures
    contextSettings.frameDimensions = {std::max(renderExtent.width, contextSettings.frameDimensions.x),
                                       std::max(renderExtent.height, contextSettings.frameDimensions.y)};
    // sized for the SDK's default iteration count, so the UI can raise the per-frame budget without a reconfigure
    contextSettings.trainingDimensions = api.computeIdealTrainingDimensions(contextSettings.frameDimensions, 0, 0.0f);
    contextSettings.samplesPerPixel = 1;
    contextSettings.maxPathVertices = kMaxPathVertices;
    allocateSdkBuffers();
    checkNrc(api.configure(context, contextSettings, &sdkBufferInfo, nullptr), "Configure");

    nrc::BuffersAllocationInfo allocation{};
    checkNrc(api.getBuffersAllocationInfo(contextSettings, allocation), "GetBuffersAllocationInfo");
    const nrc::vulkan::Buffers& buffers = sdkBufferInfo;
    const auto rows = [&](nrc::BufferIdx index, size_t elementSize)
    {
        const nrc::AllocationInfo& info = allocation[index];
        if (info.elementSize != elementSize || buffers[index].deviceAddress == 0) {
            throw std::runtime_error(std::format("NRC buffer {}: element size {} (bridge {}), address {:#x}",
                                                 static_cast<int>(index), info.elementSize, elementSize,
                                                 buffers[index].deviceAddress));
        }
        return static_cast<uint32_t>(info.elementCount);
    };
    bufferRows = GpuNrcConstants{
        .nrc = {},
        .queryPathInfo = buffers[nrc::BufferIdx::QueryPathInfo].deviceAddress,
        .trainingPathInfo = buffers[nrc::BufferIdx::TrainingPathInfo].deviceAddress,
        .trainingPathVertices = buffers[nrc::BufferIdx::TrainingPathVertices].deviceAddress,
        .queryRadianceParams = buffers[nrc::BufferIdx::QueryRadianceParams].deviceAddress,
        .queryRadiance = buffers[nrc::BufferIdx::QueryRadiance].deviceAddress,
        .counters = buffers[nrc::BufferIdx::Counter].deviceAddress,
        .queryPathInfoCount = rows(nrc::BufferIdx::QueryPathInfo, sizeof(NrcPackedQueryPathInfo)),
        .trainingPathInfoCount = rows(nrc::BufferIdx::TrainingPathInfo, sizeof(NrcPackedTrainingPathInfo)),
        .trainingPathVertexCount = rows(nrc::BufferIdx::TrainingPathVertices, sizeof(NrcPackedPathVertex)),
        .queryRadianceCount = rows(nrc::BufferIdx::QueryRadianceParams, sizeof(NrcRadianceParams)),
    };
    // the query results are float3, one per query record
    if (rows(nrc::BufferIdx::QueryRadiance, 3 * sizeof(float)) < bufferRows.queryRadianceCount) {
        throw std::runtime_error("NRC QueryRadiance has fewer rows than QueryRadianceParams");
    }
    rows(nrc::BufferIdx::Counter, sizeof(uint32_t));
    isConfigured = true;

    size_t bytes = 0;
    for (const nrc::vulkan::BufferInfo& buffer : buffers.buffers) {
        bytes += buffer.allocatedSize;
    }
    logInfo(std::format("configured frame {}x{}, training {}x{}, bounds ({:.1f}, {:.1f}, {:.1f})-({:.1f}, {:.1f}, {:.1f}), "
                        "{:.1f} MiB",
                        contextSettings.frameDimensions.x, contextSettings.frameDimensions.y,
                        contextSettings.trainingDimensions.x,
                        contextSettings.trainingDimensions.y, boundsMin.x, boundsMin.y, boundsMin.z, boundsMax.x,
                        boundsMax.y, boundsMax.z, static_cast<double>(bytes) / (1024.0 * 1024.0)),
            kSubsystem);
}

void NrcCache::allocateSdkBuffers()
{
    nrc::BuffersAllocationInfo allocation{};
    checkNrc(api.getBuffersAllocationInfo(contextSettings, allocation), "GetBuffersAllocationInfo");
    for (size_t index = 0; index < sdkBuffers.size(); ++index) {
        const nrc::AllocationInfo& info = allocation.allocationInfo[index];
        SdkBuffer& target = sdkBuffers[index];
        const vk::DeviceSize size = static_cast<vk::DeviceSize>(info.elementCount) * info.elementSize;
        if (size == target.size) {
            continue;
        }
        destroyVmaBuffer(allocator.allocator, target.buffer, target.memory);
        target.size = size;
        sdkBufferInfo.buffers[index] = {};
        // DebugTrainingPathInfo without enableDebugBuffers
        if (size == 0) {
            continue;
        }
        const auto usage = static_cast<vk::BufferUsageFlags2>(api.getBufferUsageFlags(context, info)) |
                           vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress;
        createBuffer(size, usage, vk::MemoryPropertyFlagBits::eDeviceLocal, target.buffer, target.memory,
                     allocator.allocator, device.vkdevice, device.queueFamilyIndices,
                     std::format("Nrc{}", info.debugName != nullptr ? std::string(info.debugName) : std::to_string(index)));
        sdkBufferInfo.buffers[index] = nrc::vulkan::BufferInfo{
            .resource = *target.buffer,
            .allocatedSize = size,
            .allocatedOffset = 0,
            .memory = VK_NULL_HANDLE,
            .deviceAddress = device.vkdevice.getBufferAddress({.buffer = *target.buffer}),
        };
    }
}

void NrcCache::destroySdkBuffers()
{
    for (SdkBuffer& target : sdkBuffers) {
        destroyVmaBuffer(allocator.allocator, target.buffer, target.memory);
        target.size = 0;
    }
    sdkBufferInfo = {};
}

void NrcCache::ensureDebugResolveTarget()
{
    const vk::Extent2D targetExtent{contextSettings.frameDimensions.x, contextSettings.frameDimensions.y};
    if (targetExtent.width == 0 || targetExtent.height == 0 ||
        (*debugResolveImage && debugResolveExtent == targetExtent)) {
        return;
    }
    destroyDebugResolveTarget();
    constexpr vk::Format kDebugResolveFormat = vk::Format::eR32G32B32A32Sfloat;
    allocator.createImage2D(targetExtent.width, targetExtent.height, 1, vk::SampleCountFlagBits::e1,
                            kDebugResolveFormat,
                            vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc, debugResolveImage,
                            debugResolveMemory, "NrcDebugResolveMemory");
    setDebugName(device.vkdevice, debugResolveImage, "NrcDebugResolve");
    debugResolveView = vk::raii::ImageView(device.vkdevice,
                                           vk::ImageViewCreateInfo{
                                               .image = *debugResolveImage,
                                               .viewType = vk::ImageViewType::e2D,
                                               .format = kDebugResolveFormat,
                                               .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                                                    .levelCount = 1,
                                                                    .layerCount = 1},
                                           });
    setDebugName(device.vkdevice, debugResolveView, "NrcDebugResolveView");
    debugResolveExtent = targetExtent;
    isDebugResolveInitialized = false;
}

void NrcCache::destroyDebugResolveTarget()
{
    debugResolveView = nullptr;
    destroyVmaImage(allocator.allocator, debugResolveImage, debugResolveMemory);
    debugResolveExtent = {};
    isDebugResolveInitialized = false;
}

nrc::FrameSettings NrcCache::frameSettings() const
{
    nrc::FrameSettings frame{};
    frame.maxExpectedAverageRadianceValue = kMaxExpectedAverageRadiance;
    frame.terminationHeuristicThreshold = kTerminationHeuristicThreshold;
    frame.trainingTerminationHeuristicThreshold = kTerminationHeuristicThreshold;
    frame.trainTheCache = isTrainingFrame;
    frame.proportionPrimarySegmentsToTrainOn = kPrimarySegmentsToTrainOn;
    frame.skipDeltaVertices = true;
    frame.numTrainingIterations = std::max(settings.trainingIterations, 1u);
    frame.resolveMode = settings.resolveMode;
    // NP2: anneal Adam learning rate over the final <= 128 frames of the idle countdown so the frozen snapshot
    // settles into the mean instead of locking in a noisy 1e-2 mini-batch bounce (NrcCommon.h: [0.0001, 0.01])
    constexpr float kBaseLearningRate = 1e-2f;
    constexpr float kFinalLearningRate = 5e-4f;
    constexpr uint32_t kMaxAnnealFrames = 128;
    if (isTrainingFrame && settings.idleTrainingFrames >= 4) {
        const uint32_t annealFrames = std::max(std::min(settings.idleTrainingFrames / 2, kMaxAnnealFrames), 1u);
        if (trainingFramesRemaining <= annealFrames) {
            const float progress = 1.0f - static_cast<float>(trainingFramesRemaining - 1) / static_cast<float>(annealFrames);
            frame.learningRate = kBaseLearningRate * std::pow(kFinalLearningRate / kBaseLearningRate, progress);
        }
    }
    // from the render extent: a grown-only frame must not train more paths than the image has
    const nrc_uint2 used = api.computeIdealTrainingDimensions({renderExtent.width, renderExtent.height},
                                                              frame.numTrainingIterations, 0.0f);
    frame.usedTrainingDimensions = {std::min(used.x, contextSettings.trainingDimensions.x),
                                    std::min(used.y, contextSettings.trainingDimensions.y)};
    return frame;
}

void NrcCache::recordBeginFrame(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot, bool isCacheView)
{
    const nrc::FrameSettings frame = frameSettings();
    if (isTrainingFrame && settings.idleTrainingFrames > 0 && trainingFramesRemaining > 0) {
        --trainingFramesRemaining;
    }
    usedTrainingExtent = vk::Extent2D{frame.usedTrainingDimensions.x, frame.usedTrainingDimensions.y};
    // the previous frame's resolve and debug-view reads -> BeginFrame's counter reset
    nrcBoundary(cmd);
    driverHeap.bind(cmd);
    checkNrc(api.beginFrame(context, *cmd, frame), "BeginFrame");
    nrcBoundary(cmd);

    GpuNrcConstants slotConstants = bufferRows;
    checkNrc(api.populateShaderConstants(context, slotConstants.nrc), "PopulateShaderConstants");
    slotConstants.flags = isCacheView ? kNrcFlagCacheView : 0u;
    slotConstants.trainingLaunch = usedTrainingExtent.width | (usedTrainingExtent.height << 16);
    // host-coherent; the slot is not in flight
    std::memcpy(constants[frameSlot].mapped, &slotConstants, sizeof(slotConstants));
}

void NrcCache::recordUpdate(const vk::raii::CommandBuffer& cmd, vk::DeviceAddress frameRoot,
                            const vk::raii::ShaderEXT& updateShader) const
{
    if (!isTrainingFrame) {
        return;
    }
    ShaderObjects::bindComputeShader(cmd, updateShader);
    const FramePushData push{.frameRoot = frameRoot};
    cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
    cmd.dispatch(divideRoundUp(usedTrainingExtent.width, kUpdateTile),
                 divideRoundUp(usedTrainingExtent.height, kUpdateTile), 1);
}

void NrcCache::recordTrain(const vk::raii::CommandBuffer& cmd)
{
    // update and query path writes -> QueryAndTrain's reads
    nrcBoundary(cmd);
    driverHeap.bind(cmd);
    // no loss: on Vulkan the SDK's loss readback fails inside tiny-cuda-nn (cudaMemcpyAsync device-to-host "operation
    // not supported", status InternalError; considerations C-G3-4)
    checkNrc(api.queryAndTrain(context, *cmd, nullptr), "QueryAndTrain");
    // query radiance -> the resolve and debug view
    nrcBoundary(cmd);
}

bool NrcCache::isDebugResolveActive() const noexcept
{
    return settings.resolveMode != NrcResolveMode::AddQueryResultToOutput;
}

void NrcCache::recordDebugResolve(const vk::raii::CommandBuffer& cmd, vk::Image indirectRadiance, vk::Extent2D extent)
{
    if (!*debugResolveImage) {
        return;
    }
    constexpr vk::ImageSubresourceRange kColorRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1};
    if (!isDebugResolveInitialized) {
        const vk::ImageMemoryBarrier2 toGeneral{
            .srcStageMask = vk::PipelineStageFlagBits2::eNone,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = *debugResolveImage,
            .subresourceRange = kColorRange,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toGeneral});
        isDebugResolveInitialized = true;
    } else {
        const vk::ImageMemoryBarrier2 reuseBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eBlit,
            .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = *debugResolveImage,
            .subresourceRange = kColorRange,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &reuseBarrier});
    }
    nrcBoundary(cmd);
    driverHeap.bind(cmd);
    checkNrc(api.resolve(context, *cmd, *debugResolveView), "Resolve");

    const std::array preBlitBarriers{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eBlit,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = *debugResolveImage,
            .subresourceRange = kColorRange,
        },
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eBlit,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = indirectRadiance,
            .subresourceRange = kColorRange,
        },
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<uint32_t>(preBlitBarriers.size()),
        .pImageMemoryBarriers = preBlitBarriers.data(),
    });
    const vk::Extent2D copyExtent{std::min(extent.width, debugResolveExtent.width),
                                  std::min(extent.height, debugResolveExtent.height)};
    const vk::ImageBlit2 region{
        .srcSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
        .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                                 vk::Offset3D{static_cast<int32_t>(copyExtent.width),
                                              static_cast<int32_t>(copyExtent.height), 1}},
        .dstSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
        .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                                 vk::Offset3D{static_cast<int32_t>(copyExtent.width),
                                              static_cast<int32_t>(copyExtent.height), 1}},
    };
    cmd.blitImage2(vk::BlitImageInfo2{
        .srcImage = *debugResolveImage,
        .srcImageLayout = vk::ImageLayout::eGeneral,
        .dstImage = indirectRadiance,
        .dstImageLayout = vk::ImageLayout::eGeneral,
        .regionCount = 1,
        .pRegions = &region,
        .filter = vk::Filter::eNearest,
    });
    const vk::ImageMemoryBarrier2 postBlitBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eBlit,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = indirectRadiance,
        .subresourceRange = kColorRange,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &postBlitBarrier});
    nrcBoundary(cmd);
}

const char* NrcCache::resolveModeComboString() const
{
    return api.getImGuiResolveModeComboString();
}

void NrcCache::endFrame(const vk::raii::Queue& queue)
{
    checkNrc(api.endFrame(context, *queue), "EndFrame");
}
