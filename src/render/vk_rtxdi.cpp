#include "vk_rtxdi.hpp"
#include "static_headers/logger.hpp"

#include <Rtxdi/DI/ReSTIRDI.h>
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
    RTXDI_ShadingParameters shading = context->GetShadingParameters();
    // one final visibility ray per pixel (spec §11.2): rtxdi_shade never reuses the stored visibility
    shading.reuseFinalVisibility = 0;
    context->SetShadingParameters(shading);

    destroyReservoirs();
    const RTXDI_ReservoirBufferParameters reservoirParams = context->GetReservoirBufferParameters();
    reservoirCount = reservoirParams.reservoirArrayPitch * rtxdi::c_NumReSTIRDIReservoirBuffers;
    const vk::DeviceSize reservoirBytes = static_cast<vk::DeviceSize>(reservoirCount) * sizeof(RTXDI_PackedDIReservoir);
    createBuffer(reservoirBytes, vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eDeviceLocal, reservoirs, reservoirMemory, allocator.allocator,
                 device.vkdevice, device.queueFamilyIndices, "RtxdiReservoirs");
    reservoirAddress = device.vkdevice.getBufferAddress({.buffer = *reservoirs});

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
    return Settings{
        .localLightSamples = initial.numLocalLightSamples,
        .infiniteLightSamples = initial.numInfiniteLightSamples,
        .isInitialVisibilityEnabled = initial.enableInitialVisibility != 0,
        .maxHistoryLength = temporal.maxHistoryLength,
        .temporalBiasCorrection = temporal.biasCorrectionMode,
        .isVisibilityShortcutEnabled = temporal.enableVisibilityShortcut != 0,
        .isFallbackSamplingEnabled = temporal.enableFallbackSampling != 0,
        // 3.1's fused pass counted the temporal reservoir as its first spatial sample, so its default of 1 took
        // neighbors only on disocclusions. 3.2's 2 per frame feed neighbors back through the temporal history: on
        // LightsPunctualLamp tile p90 49.6% (Basic) and 21.7% (Pairwise) against 2.9% with neighbors only below
        // maxHistoryLength (spec/features/rtxdi_3_2_update.md §6)
        .spatialSamples = 0,
        .disocclusionBoostSamples = spatial.numDisocclusionBoostSamples,
        .samplingRadius = spatial.samplingRadius,
        // Basic weighs neighbors with mismatched BRDFs poorly: tile p90 6.3% against Pairwise's 2.9% (same capture)
        .spatialBiasCorrection = ReSTIRDI_SpatialBiasCorrectionMode::Pairwise,
        .isMaterialSimilarityTestEnabled = spatial.enableMaterialSimilarityTest != 0,
        .depthThreshold = spatial.depthThreshold,
        .normalThreshold = spatial.normalThreshold,
        .isBoilingFilterEnabled = boiling.enableBoilingFilter != 0,
        .boilingFilterStrength = boiling.boilingFilterStrength,
    };
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
    // a BRDF ray finds a light only on emissive triangles (M7); until then each one is a wasted ray per pixel
    initialSampling.numBrdfSamples = 0;
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
    // permutation decorrelates frames for denoisers, but the frame-to-frame change grew from 0.8% to 20.5% of the
    // signal (lamp, static camera; considerations C-F3-3). SetFrameIndex still rehashes uniformRandomNumber, which
    // the fallback sample permutes with
    temporal.enablePermutationSampling = 0;
    context->SetTemporalResamplingParameters(temporal);

    RTXDI_DISpatialResamplingParameters spatial = context->GetSpatialResamplingParameters();
    spatial.numSamples = settings.spatialSamples;
    spatial.numDisocclusionBoostSamples = settings.disocclusionBoostSamples;
    spatial.samplingRadius = settings.samplingRadius;
    spatial.biasCorrectionMode = settings.spatialBiasCorrection;
    spatial.depthThreshold = settings.depthThreshold;
    spatial.normalThreshold = settings.normalThreshold;
    spatial.enableMaterialSimilarityTest = settings.isMaterialSimilarityTestEnabled ? 1u : 0u;
    // the library's 0 never boosts; the fused pass boosted below maxHistoryLength
    spatial.targetHistoryLength = settings.maxHistoryLength;
    spatial.neighborOffsetSamplingMode = kNeighborSamplingMode;
    context->SetSpatialResamplingParameters(spatial);

    context->SetBoilingFilterParameters(RTXDI_BoilingFilterParameters{
        .enableBoilingFilter = settings.isBoilingFilterEnabled ? 1u : 0u,
        .boilingFilterStrength = settings.boilingFilterStrength,
        .pad1 = 0,
        .pad2 = 0,
    });
    isReferenceApplied = isReference;
    appliedSettings = settings;
}

void RtxdiContext::prepareFrame(uint32_t frameSlot, uint32_t frameIndex)
{
    if (isReference != isReferenceApplied) {
        configure();
        logInfo(std::format("ReSTIR DI: {}", isReference ? "reference (no reuse)" : "temporal and spatial"), "Rtxdi");
    } else if (settings != appliedSettings) {
        configure();
    }
    // rotates the reservoir buffer indices and the frame's random number
    context->SetFrameIndex(frameIndex);
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
    };
    std::memcpy(constants[frameSlot].mapped, &data, sizeof(data));
}
