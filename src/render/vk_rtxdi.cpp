#include "vk_rtxdi.hpp"
#include "static_headers/logger.hpp"

#include <Rtxdi/DI/ReSTIRDI.h>
#include <Rtxdi/GI/ReSTIRGI.h>
#include <Rtxdi/RtxdiUtils.h>

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

// the passes test GpuRtxdiConstants.resamplingMode as bits
static_assert(std::to_underlying(rtxdi::ReSTIRDI_ResamplingMode::None) == 0);
static_assert(std::to_underlying(rtxdi::ReSTIRDI_ResamplingMode::Temporal) == 1);
static_assert(std::to_underlying(rtxdi::ReSTIRDI_ResamplingMode::Spatial) == 2);
static_assert(std::to_underlying(rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial) == 3);
static_assert(std::to_underlying(rtxdi::ReSTIRGI_ResamplingMode::None) == 0);
static_assert(std::to_underlying(rtxdi::ReSTIRGI_ResamplingMode::TemporalAndSpatial) == 3);
// the initial samples follow RTXDI's arrays
static_assert(RtxdiContext::kGiInitialReservoirArray == rtxdi::c_NumReSTIRGIReservoirBuffers);
static_assert(RtxdiContext::kGiReservoirArrays == rtxdi::c_NumReSTIRGIReservoirBuffers + 1);

namespace
{
    // RTXDI's default; a power of two, the shaders mask with neighborOffsetMask
    constexpr uint32_t kNeighborOffsetCount = 8192;
    // the table layout and the spatial pass's walk must match; 3.2's default
    constexpr ReSTIRDI_SpatialSamplingMode kNeighborSamplingMode =
        ReSTIRDI_SpatialSamplingMode::StratifiedLowDiscrepancy;
    // FillNeighborOffsetBuffer writes RG8_SNORM pairs; RTXDI_NEIGHBOR_OFFSETS_BUFFER reads them as float2
    constexpr float kSnormScale = 1.0f / 127.0f;

    struct NeighborOffset {
        float x;
        float y;
    };

    std::vector<NeighborOffset> makeNeighborOffsets()
    {
        std::vector<uint8_t> packed(static_cast<size_t>(kNeighborOffsetCount) * 2);
        rtxdi::FillNeighborOffsetBuffer(packed.data(), kNeighborOffsetCount, kNeighborSamplingMode);
        std::vector<NeighborOffset> offsets(kNeighborOffsetCount);
        for (uint32_t i = 0; i < kNeighborOffsetCount; ++i) {
            offsets[i] = {.x = static_cast<float>(static_cast<int8_t>(packed[i * 2])) * kSnormScale,
                          .y = static_cast<float>(static_cast<int8_t>(packed[i * 2 + 1])) * kSnormScale};
        }
        return offsets;
    }
} // namespace

RtxdiContext::RtxdiContext(const Device& device, const VkAllocator& allocator, const WorldLights& worldLights)
    : device(device), allocator(allocator), worldLights(worldLights)
{
    const std::vector<NeighborOffset> offsets = makeNeighborOffsets();
    const vk::DeviceSize offsetBytes = offsets.size() * sizeof(NeighborOffset);
    neighborOffsets = createHostBuffer(offsetBytes, allocator.allocator, device.vkdevice, device.queueFamilyIndices,
                                       "RtxdiNeighborOffsets", "RtxdiNeighborOffsets");
    std::memcpy(neighborOffsets.mapped, offsets.data(), offsetBytes);
    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        constants[slot] = createHostBuffer(sizeof(GpuRtxdiConstants), allocator.allocator, device.vkdevice,
                                           device.queueFamilyIndices, std::format("RtxdiConstants_{}", slot),
                                           "RtxdiConstants");
    }
}

RtxdiContext::~RtxdiContext()
{
    destroyReservoirs();
    destroyHostBuffer(allocator.allocator, neighborOffsets, "RtxdiNeighborOffsets");
    for (HostBuffer& slot : constants) {
        destroyHostBuffer(allocator.allocator, slot, "RtxdiConstants");
    }
}

void RtxdiContext::destroyReservoirs()
{
    destroyVmaBuffer(allocator.allocator, reservoirs, reservoirMemory);
    reservoirAddress = 0;
    reservoirCount = 0;
    destroyVmaBuffer(allocator.allocator, giReservoirs, giReservoirMemory);
    giReservoirAddress = 0;
    giReservoirCount = 0;
}

void RtxdiContext::createGiReservoirs()
{
    const RTXDI_ReservoirBufferParameters params = giContext->GetReservoirBufferParameters();
    giReservoirCount = params.reservoirArrayPitch * kGiReservoirArrays;
    const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(giReservoirCount) * sizeof(RTXDI_PackedGIReservoir);
    createBuffer(bytes, vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eDeviceLocal, giReservoirs, giReservoirMemory, allocator.allocator,
                 device.vkdevice, device.queueFamilyIndices, "RtxdiGiReservoirs");
    giReservoirAddress = device.vkdevice.getBufferAddress({.buffer = *giReservoirs});
    logInfo(std::format("ReSTIR GI: {} reservoir buffers of {} ({:.1f} MiB), {}", kGiReservoirArrays,
                        params.reservoirArrayPitch, static_cast<double>(bytes) / (1024.0 * 1024.0),
                        isGiReuseApplied ? "temporal and spatial" : "no reuse"),
            "Rtxdi");
}

void RtxdiContext::recreate(vk::Extent2D renderExtent)
{
    context = std::make_unique<rtxdi::ReSTIRDIContext>(rtxdi::ReSTIRDIStaticParameters{
        .NeighborOffsetCount = kNeighborOffsetCount,
        .RenderWidth = renderExtent.width,
        .RenderHeight = renderExtent.height,
        .CheckerboardSamplingMode = rtxdi::CheckerboardMode::Off,
    });
    configure();

    destroyReservoirs();
    const RTXDI_ReservoirBufferParameters reservoirParams = context->GetReservoirBufferParameters();
    reservoirCount = reservoirParams.reservoirArrayPitch * rtxdi::c_NumReSTIRDIReservoirBuffers;
    const vk::DeviceSize reservoirBytes = static_cast<vk::DeviceSize>(reservoirCount) * sizeof(RTXDI_PackedDIReservoir);
    createBuffer(reservoirBytes, vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eDeviceLocal, reservoirs, reservoirMemory, allocator.allocator,
                 device.vkdevice, device.queueFamilyIndices, "RtxdiReservoirs");
    reservoirAddress = device.vkdevice.getBufferAddress({.buffer = *reservoirs});

    giContext = std::make_unique<rtxdi::ReSTIRGIContext>(rtxdi::ReSTIRGIStaticParameters{
        .RenderWidth = renderExtent.width,
        .RenderHeight = renderExtent.height,
        .CheckerboardSamplingMode = rtxdi::CheckerboardMode::Off,
    });
    configureGi(isGiReuseApplied);
    createGiReservoirs();

    logInfo(
        std::format("ReSTIR DI {}x{}: {} reservoir buffers of {} ({:.1f} MiB), {}, uniform local light sampling, {} "
                    "neighbor offsets",
                    renderExtent.width, renderExtent.height, rtxdi::c_NumReSTIRDIReservoirBuffers,
                    reservoirParams.reservoirArrayPitch, static_cast<double>(reservoirBytes) / (1024.0 * 1024.0),
                    isReference ? "reference (no reuse)" : "temporal and spatial", kNeighborOffsetCount),
        "Rtxdi");
}

RtxdiContext::Settings RtxdiContext::defaultSettings()
{
    const RTXDI_DIInitialSamplingParameters initial = rtxdi::GetDefaultReSTIRDIInitialSamplingParams();
    const RTXDI_DITemporalResamplingParameters temporal = rtxdi::GetDefaultReSTIRDITemporalResamplingParams();
    const RTXDI_DISpatialResamplingParameters spatial = rtxdi::GetDefaultReSTIRDISpatialResamplingParams();
    const RTXDI_BoilingFilterParameters boiling = rtxdi::GetDefaultReSTIRDIBoilingFilterParams();
    const RTXDI_ShadingParameters shading = rtxdi::GetDefaultReSTIRDIShadingParams();
    // RTXDI-Library Samples/FullSample UserInterface.cpp ApplyPreset(Medium) over the library defaults. Its ReGIR_RIS
    // local sampling needs presampling (M7): uniform, same candidate count
    return Settings{
        .localLightSamples = initial.numLocalLightSamples,
        .infiniteLightSamples = initial.numInfiniteLightSamples,
        .brdfSamples = initial.numBrdfSamples,
        .isInitialVisibilityEnabled = initial.enableInitialVisibility != 0,
        .maxHistoryLength = temporal.maxHistoryLength,
        .temporalBiasCorrection = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced,
        .isVisibilityShortcutEnabled = true,
        .isFallbackSamplingEnabled = temporal.enableFallbackSampling != 0,
        .isPermutationSamplingEnabled = temporal.enablePermutationSampling != 0,
        // the preset's 1, rounded up to even for the stratified walk (NormalizeDISpatialSampleCount)
        .spatialSamples = 2,
        .disocclusionBoostSamples = spatial.numDisocclusionBoostSamples,
        .disocclusionHistoryLength = spatial.targetHistoryLength,
        .samplingRadius = spatial.samplingRadius,
        .spatialBiasCorrection = ReSTIRDI_SpatialBiasCorrectionMode::Basic,
        .isMaterialSimilarityTestEnabled = spatial.enableMaterialSimilarityTest != 0,
        .depthThreshold = spatial.depthThreshold,
        .normalThreshold = spatial.normalThreshold,
        .isBoilingFilterEnabled = boiling.enableBoilingFilter != 0,
        .boilingFilterStrength = boiling.boilingFilterStrength,
        .isFinalVisibilityReused = shading.reuseFinalVisibility != 0,
    };
}

RtxdiContext::Settings RtxdiContext::convergedSettings()
{
    Settings converged = defaultSettings();
    converged.brdfSamples = 0;
    converged.temporalBiasCorrection = ReSTIRDI_TemporalBiasCorrectionMode::Basic;
    converged.isVisibilityShortcutEnabled = false;
    converged.isPermutationSamplingEnabled = false;
    converged.spatialSamples = 0;
    converged.disocclusionHistoryLength = converged.maxHistoryLength;
    converged.spatialBiasCorrection = ReSTIRDI_SpatialBiasCorrectionMode::Pairwise;
    converged.isFinalVisibilityReused = false;
    return converged;
}

RtxdiContext::GiSettings RtxdiContext::defaultGiSettings()
{
    const RTXDI_GITemporalResamplingParameters temporal = rtxdi::GetDefaultReSTIRGITemporalResamplingParams();
    const RTXDI_GISpatialResamplingParameters spatial = rtxdi::GetDefaultReSTIRGISpatialResamplingParams();
    const RTXDI_BoilingFilterParameters boiling = rtxdi::GetDefaultReSTIRGIBoilingFilterParams();
    const RTXDI_GIFinalShadingParameters shading = rtxdi::GetDefaultReSTIRGIFinalShadingParams();
    // RTXDI Samples/FullSample UserInterface.cpp: the library defaults, which no preset changes
    return GiSettings{
        .maxHistoryLength = temporal.maxHistoryLength,
        .maxReservoirAge = temporal.maxReservoirAge,
        .temporalBiasCorrection = temporal.biasCorrectionMode,
        .isFallbackSamplingEnabled = temporal.enableFallbackSampling != 0,
        .isPermutationSamplingEnabled = temporal.enablePermutationSampling != 0,
        .spatialSamples = spatial.numSamples,
        .samplingRadius = spatial.samplingRadius,
        .spatialBiasCorrection = spatial.biasCorrectionMode,
        .depthThreshold = spatial.depthThreshold,
        .normalThreshold = spatial.normalThreshold,
        .isBoilingFilterEnabled = boiling.enableBoilingFilter != 0,
        .boilingFilterStrength = boiling.boilingFilterStrength,
        .isFinalVisibilityEnabled = shading.enableFinalVisibility != 0,
        .isFinalMisEnabled = shading.enableFinalMIS != 0,
    };
}

RtxdiContext::GiSettings RtxdiContext::convergedGiSettings()
{
    GiSettings converged = defaultGiSettings();
    converged.temporalBiasCorrection = RTXDI_GIBiasCorrectionMode::Raytraced;
    converged.spatialBiasCorrection = RTXDI_GIBiasCorrectionMode::Raytraced;
    // keeping only reservoirs whose sample is young keeps the ones a bright candidate won recently: +5.6% on the
    // gallery at RTXDI's 30 frames (considerations C-G5-1)
    converged.maxReservoirAge = kGiUnlimitedSampleAge;
    // a zero-motion sample without the surface tests: locally biased, usually darker (ReSTIRGIParameters.h)
    converged.isFallbackSamplingEnabled = false;
    converged.isPermutationSamplingEnabled = false;
    converged.isBoilingFilterEnabled = false;
    return converged;
}

bool RtxdiContext::isReferenceRequested()
{
    const char* text = std::getenv("ENGINE_RTXDI_REFERENCE");
    return text != nullptr && std::string_view(text) == "1";
}

void RtxdiContext::configure()
{
    context->SetResamplingMode(isReference ? rtxdi::ReSTIRDI_ResamplingMode::None
                                           : rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial);
    RTXDI_DIInitialSamplingParameters initialSampling = rtxdi::GetDefaultReSTIRDIInitialSamplingParams();
    // Power_RIS needs the light PDF texture (M7)
    initialSampling.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Uniform;
    initialSampling.numBrdfSamples = isReference ? 0u : settings.brdfSamples;
    initialSampling.numLocalLightSamples = isReference ? kReferenceLocalLightSamples : settings.localLightSamples;
    initialSampling.numInfiniteLightSamples =
        isReference ? kReferenceInfiniteLightSamples : settings.infiniteLightSamples;
    initialSampling.enableInitialVisibility = settings.isInitialVisibilityEnabled ? 1u : 0u;
    context->SetInitialSamplingParameters(initialSampling);

    RTXDI_DITemporalResamplingParameters temporal = context->GetTemporalResamplingParameters();
    temporal.maxHistoryLength = settings.maxHistoryLength;
    temporal.biasCorrectionMode = settings.temporalBiasCorrection;
    temporal.depthThreshold = settings.depthThreshold;
    temporal.normalThreshold = settings.normalThreshold;
    temporal.enableVisibilityShortcut = settings.isVisibilityShortcutEnabled ? 1u : 0u;
    temporal.enableFallbackSampling = settings.isFallbackSamplingEnabled ? 1u : 0u;
    // SetFrameIndex rehashes uniformRandomNumber either way: the fallback sample permutes with it
    temporal.enablePermutationSampling = settings.isPermutationSamplingEnabled ? 1u : 0u;
    context->SetTemporalResamplingParameters(temporal);

    RTXDI_DISpatialResamplingParameters spatial = context->GetSpatialResamplingParameters();
    spatial.numSamples = settings.spatialSamples;
    spatial.numDisocclusionBoostSamples = settings.disocclusionBoostSamples;
    spatial.samplingRadius = settings.samplingRadius;
    spatial.biasCorrectionMode = settings.spatialBiasCorrection;
    spatial.depthThreshold = settings.depthThreshold;
    spatial.normalThreshold = settings.normalThreshold;
    spatial.enableMaterialSimilarityTest = settings.isMaterialSimilarityTestEnabled ? 1u : 0u;
    spatial.targetHistoryLength = settings.disocclusionHistoryLength;
    spatial.neighborOffsetSamplingMode = kNeighborSamplingMode;
    context->SetSpatialResamplingParameters(spatial);

    context->SetBoilingFilterParameters(RTXDI_BoilingFilterParameters{
        .enableBoilingFilter = settings.isBoilingFilterEnabled ? 1u : 0u,
        .boilingFilterStrength = settings.boilingFilterStrength,
        .pad1 = 0,
        .pad2 = 0,
    });

    RTXDI_ShadingParameters shading = context->GetShadingParameters();
    shading.reuseFinalVisibility = settings.isFinalVisibilityReused ? 1u : 0u;
    context->SetShadingParameters(shading);
    isReferenceApplied = isReference;
    appliedSettings = settings;
}

void RtxdiContext::configureGi(bool isGiReuse)
{
    giContext->SetResamplingMode(isGiReuse ? rtxdi::ReSTIRGI_ResamplingMode::TemporalAndSpatial
                                           : rtxdi::ReSTIRGI_ResamplingMode::None);
    RTXDI_GITemporalResamplingParameters temporal = giContext->GetTemporalResamplingParameters();
    temporal.maxHistoryLength = giSettings.maxHistoryLength;
    temporal.maxReservoirAge = giSettings.maxReservoirAge;
    temporal.biasCorrectionMode = giSettings.temporalBiasCorrection;
    temporal.enableFallbackSampling = giSettings.isFallbackSamplingEnabled ? 1u : 0u;
    temporal.enablePermutationSampling = giSettings.isPermutationSamplingEnabled ? 1u : 0u;
    temporal.depthThreshold = giSettings.depthThreshold;
    temporal.normalThreshold = giSettings.normalThreshold;
    giContext->SetTemporalResamplingParameters(temporal);

    RTXDI_GISpatialResamplingParameters spatial = giContext->GetSpatialResamplingParameters();
    spatial.numSamples = giSettings.spatialSamples;
    spatial.samplingRadius = giSettings.samplingRadius;
    spatial.biasCorrectionMode = giSettings.spatialBiasCorrection;
    spatial.depthThreshold = giSettings.depthThreshold;
    spatial.normalThreshold = giSettings.normalThreshold;
    giContext->SetSpatialResamplingParameters(spatial);

    giContext->SetBoilingFilterParameters(RTXDI_BoilingFilterParameters{
        .enableBoilingFilter = giSettings.isBoilingFilterEnabled ? 1u : 0u,
        .boilingFilterStrength = giSettings.boilingFilterStrength,
        .pad1 = 0,
        .pad2 = 0,
    });
    giContext->SetFinalShadingParameters(RTXDI_GIFinalShadingParameters{
        .enableFinalVisibility = giSettings.isFinalVisibilityEnabled ? 1u : 0u,
        .enableFinalMIS = giSettings.isFinalMisEnabled ? 1u : 0u,
        .pad1 = 0,
        .pad2 = 0,
    });
    isGiReuseApplied = isGiReuse;
    appliedGiSettings = giSettings;
}

void RtxdiContext::prepareFrame(uint32_t frameSlot, uint32_t frameIndex, bool isGiReuseAllowed)
{
    if (isReference != isReferenceApplied) {
        configure();
        logInfo(std::format("ReSTIR DI: {}", isReference ? "reference (no reuse)" : "temporal and spatial"), "Rtxdi");
    } else if (settings != appliedSettings) {
        configure();
    }
    const bool isGiReuse = isGiReuseAllowed && !isGiReference;
    if (isGiReuse != isGiReuseApplied) {
        configureGi(isGiReuse);
        logInfo(std::format("ReSTIR GI: {}", isGiReuse ? "temporal and spatial" : "no reuse"), "Rtxdi");
    } else if (giSettings != appliedGiSettings) {
        configureGi(isGiReuse);
    }
    // rotates the reservoir buffer indices and the frame's random number
    context->SetFrameIndex(frameIndex);
    giContext->SetFrameIndex(frameIndex);
    const RTXDI_GIBufferIndices giBufferIndices = giContext->GetBufferIndices();
    // rtxdi_gi_shade reads neighbors from the temporal output and writes next frame's temporal input
    assert(giContext->GetResamplingMode() == rtxdi::ReSTIRGI_ResamplingMode::None ||
           (giBufferIndices.spatialResamplingInputBufferIndex == giBufferIndices.temporalResamplingOutputBufferIndex &&
            giBufferIndices.spatialResamplingOutputBufferIndex == giBufferIndices.temporalResamplingInputBufferIndex &&
            giBufferIndices.finalShadingInputBufferIndex == giBufferIndices.spatialResamplingOutputBufferIndex));
    const RTXDI_DIBufferIndices bufferIndices = context->GetBufferIndices();
    // rtxdi_temporal writes over its own pixel's initial reservoir, and its early-out leaves that as the output
    assert(bufferIndices.temporalResamplingOutputBufferIndex == bufferIndices.initialSamplingOutputBufferIndex);
    // rtxdi_shade reads neighbors from one buffer and writes the other
    assert(bufferIndices.spatialResamplingInputBufferIndex != bufferIndices.shadingInputBufferIndex ||
           context->GetResamplingMode() == rtxdi::ReSTIRDI_ResamplingMode::None);

    // local lights lead the world light rows (AssetBuffers::uploadLights), the sun ends them;
    // RAB_TranslateLightIndex is the identity
    const uint32_t lightCount = worldLights.lightCount();
    const uint32_t localLightCount = worldLights.localLightCount();
    const RTXDI_LightBufferParameters lightBuffer{
        .localLightBufferRegion = {.firstLightIndex = 0, .numLights = localLightCount, .pad1 = 0, .pad2 = 0},
        .infiniteLightBufferRegion = {.firstLightIndex = localLightCount,
                                      .numLights = lightCount - localLightCount,
                                      .pad1 = 0,
                                      .pad2 = 0},
        .environmentLightParams = {.lightPresent = 0, .lightIndex = 0, .pad1 = 0, .pad2 = 0},
    };

    const GpuRtxdiConstants data{
        .runtime = context->GetRuntimeParams(),
        .lightBuffer = lightBuffer,
        .reservoirBuffer = context->GetReservoirBufferParameters(),
        .bufferIndices = bufferIndices,
        .initialSampling = context->GetInitialSamplingParameters(),
        .temporalResampling = context->GetTemporalResamplingParameters(),
        .boilingFilter = context->GetBoilingFilterParameters(),
        .spatialResampling = context->GetSpatialResamplingParameters(),
        .shading = context->GetShadingParameters(),
        .reservoirs = reservoirAddress,
        .neighborOffsets = neighborOffsets.address,
        .resamplingMode = std::to_underlying(context->GetResamplingMode()),
        .reservoirCount = reservoirCount,
        .giReservoirBuffer = giContext->GetReservoirBufferParameters(),
        .giBufferIndices = giBufferIndices,
        .giReservoirs = giReservoirAddress,
        .giReservoirCount = giReservoirCount,
        .giResamplingMode = std::to_underlying(giContext->GetResamplingMode()),
        .giTemporalResampling = giContext->GetTemporalResamplingParameters(),
        .giBoilingFilter = giContext->GetBoilingFilterParameters(),
        .giSpatialResampling = giContext->GetSpatialResamplingParameters(),
        .giFinalShading = giContext->GetFinalShadingParameters(),
        .giReferenceBounces = giReferenceBounces,
        .pad0 = 0,
    };
    std::memcpy(constants[frameSlot].mapped, &data, sizeof(data));
}
