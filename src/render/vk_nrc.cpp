#include "vk_nrc.hpp"
#include "../util/vk_tracy.hpp"
#include "push_data.hpp"
#include "vk_shader_objects.hpp"
#include "static_headers/logger.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string>

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
    // the SDK owns its buffers; the engine reads their addresses after each configure
    globalSettings.enableGPUMemoryAllocation = true;
    globalSettings.enableDebugBuffers = false;
    globalSettings.maxNumFramesInFlight = MAX_FRAMES_IN_FLIGHT;
    checkNrc(api.initialize(globalSettings), "Initialize");
    checkNrc(api.createContext(*device.vkdevice, *device.physicalDevice, *device.instance, context), "Context::Create");

    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        constants[slot] = createHostBuffer(sizeof(GpuNrcConstants), allocator.allocator, device.vkdevice,
                                           device.queueFamilyIndices, std::format("NrcConstants_{}", slot), nullptr);
    }
}

NrcCache::~NrcCache()
{
    for (HostBuffer& slot : constants) {
        destroyHostBuffer(allocator.allocator, slot, nullptr);
    }
    if (context != nullptr) {
        api.destroyContext(*context);
    }
    api.shutdown();
}

void NrcCache::recreate(vk::Extent2D extent)
{
    renderExtent = extent;
    if (isValid(configuredBounds)) {
        configure();
    }
}

void NrcCache::prepareFrame(uint32_t frameSlot, const BoundingBox& sceneBounds)
{
    const bool isNewScene = isValid(sceneBounds) &&
                            (sceneBounds.bboxMin != configuredBounds.bboxMin ||
                             sceneBounds.bboxMax != configuredBounds.bboxMax);
    if (!isNewScene) {
        if (!isConfigured) {
            std::memset(constants[frameSlot].mapped, 0, sizeof(GpuNrcConstants));
        }
        return;
    }
    // Configure may reallocate the buffers every frame in flight uses; a layout change is a load. No requestReset:
    // NRC 0.16 keeps the weights with it (measured, considerations C-G3-5); the network adapts to the new bounds
    device.vkdevice.waitIdle();
    configuredBounds = sceneBounds;
    configure();
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
    contextSettings.frameDimensions = {renderExtent.width, renderExtent.height};
    // sized for the SDK's default iteration count, so the UI can raise the per-frame budget without a reconfigure
    contextSettings.trainingDimensions = api.computeIdealTrainingDimensions(contextSettings.frameDimensions, 0, 0.0f);
    contextSettings.samplesPerPixel = 1;
    contextSettings.maxPathVertices = kMaxPathVertices;
    checkNrc(api.configure(context, contextSettings, nullptr, nullptr), "Configure");

    nrc::BuffersAllocationInfo allocation{};
    checkNrc(api.getBuffersAllocationInfo(contextSettings, allocation), "GetBuffersAllocationInfo");
    const nrc::vulkan::Buffers& buffers = *api.getBuffers(context);
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
    logInfo(std::format("configured {}x{}, training {}x{}, bounds ({:.1f}, {:.1f}, {:.1f})-({:.1f}, {:.1f}, {:.1f}), "
                        "{:.1f} MiB",
                        renderExtent.width, renderExtent.height, contextSettings.trainingDimensions.x,
                        contextSettings.trainingDimensions.y, boundsMin.x, boundsMin.y, boundsMin.z, boundsMax.x,
                        boundsMax.y, boundsMax.z, static_cast<double>(bytes) / (1024.0 * 1024.0)),
            kSubsystem);
}

nrc::FrameSettings NrcCache::frameSettings() const
{
    nrc::FrameSettings frame{};
    frame.maxExpectedAverageRadianceValue = kMaxExpectedAverageRadiance;
    frame.terminationHeuristicThreshold = kTerminationHeuristicThreshold;
    frame.trainingTerminationHeuristicThreshold = kTerminationHeuristicThreshold;
    frame.trainTheCache = settings.isTrainingEnabled;
    frame.proportionPrimarySegmentsToTrainOn = kPrimarySegmentsToTrainOn;
    frame.skipDeltaVertices = true;
    frame.numTrainingIterations = std::max(settings.trainingIterations, 1u);
    const nrc_uint2 used =
        api.computeIdealTrainingDimensions(contextSettings.frameDimensions, frame.numTrainingIterations, 0.0f);
    frame.usedTrainingDimensions = {std::min(used.x, contextSettings.trainingDimensions.x),
                                    std::min(used.y, contextSettings.trainingDimensions.y)};
    return frame;
}

void NrcCache::recordBeginFrame(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot, bool isCacheView)
{
    const nrc::FrameSettings frame = frameSettings();
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
    if (!settings.isTrainingEnabled) {
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

void NrcCache::endFrame(const vk::raii::Queue& queue)
{
    checkNrc(api.endFrame(context, *queue), "EndFrame");
}
