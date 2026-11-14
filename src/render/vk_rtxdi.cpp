#include "vk_rtxdi.hpp"
#include "static_headers/logger.hpp"

#include <Rtxdi/DI/ReSTIRDI.h>
#include <Rtxdi/RtxdiUtils.h>

#include <cstdlib>
#include <cstring>
#include <format>
#include <string_view>
#include <vector>

namespace
{
    // RTXDI's default; a power of two, the shaders mask with neighborOffsetMask
    constexpr uint32_t kNeighborOffsetCount = 8192;
    // FillNeighborOffsetBuffer writes RG8_SNORM pairs; RTXDI_NEIGHBOR_OFFSETS_BUFFER reads them as float2
    constexpr float kSnormScale = 1.0f / 127.0f;

    struct NeighborOffset {
        float x;
        float y;
    };

    std::vector<NeighborOffset> makeNeighborOffsets()
    {
        std::vector<uint8_t> packed(static_cast<size_t>(kNeighborOffsetCount) * 2);
        rtxdi::FillNeighborOffsetBuffer(packed.data(), kNeighborOffsetCount);
        std::vector<NeighborOffset> offsets(kNeighborOffsetCount);
        for (uint32_t i = 0; i < kNeighborOffsetCount; ++i) {
            offsets[i] = {.x = static_cast<float>(static_cast<int8_t>(packed[i * 2])) * kSnormScale,
                          .y = static_cast<float>(static_cast<int8_t>(packed[i * 2 + 1])) * kSnormScale};
        }
        return offsets;
    }
} // namespace

RtxdiContext::RtxdiContext(const Device& device, const VkAllocator& allocator, const AssetBuffers& assets)
    : device(device), allocator(allocator), assets(assets)
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

    logInfo(std::format("ReSTIR DI {}x{}: {} reservoir buffers of {} ({:.1f} MiB), {}, uniform local light sampling, {} "
                        "neighbor offsets",
                        renderExtent.width, renderExtent.height, rtxdi::c_NumReSTIRDIReservoirBuffers,
                        reservoirParams.reservoirArrayPitch, static_cast<double>(reservoirBytes) / (1024.0 * 1024.0),
                        isReference ? "reference (no reuse)" : "fused spatiotemporal", kNeighborOffsetCount),
            "Rtxdi");
}

RtxdiContext::Settings RtxdiContext::defaultSettings()
{
    const RTXDI_DIInitialSamplingParameters initial = rtxdi::GetDefaultReSTIRDIInitialSamplingParams();
    const RTXDI_DISpatioTemporalResamplingParameters spatioTemporal =
        rtxdi::GetDefaultReSTIRDISpatioTemporalResamplingParams();
    const RTXDI_BoilingFilterParameters boiling = rtxdi::GetDefaultReSTIRDIBoilingFilterParams();
    return Settings{
        .localLightSamples = initial.numLocalLightSamples,
        .infiniteLightSamples = initial.numInfiniteLightSamples,
        .isInitialVisibilityEnabled = initial.enableInitialVisibility != 0,
        .spatialSamples = spatioTemporal.numSamples,
        .disocclusionBoostSamples = spatioTemporal.numDisocclusionBoostSamples,
        .samplingRadius = spatioTemporal.samplingRadius,
        .maxHistoryLength = spatioTemporal.maxHistoryLength,
        .depthThreshold = spatioTemporal.depthThreshold,
        .normalThreshold = spatioTemporal.normalThreshold,
        // RTXDI 3.1's fused Basic path skips naive neighbors (discountNaiveSamples) after marking them for its
        // normalization, the temporal one included, so their M stays 1 and keeps darkening: 35% on
        // LightsPunctualLamp. Pairwise MIS discounts only spatial neighbors and matched the reference within 0.5%
        // (plan F3)
        .biasCorrection = ReSTIRDI_SpatioTemporalBiasCorrectionMode::Pairwise,
        .isVisibilityShortcutEnabled = spatioTemporal.enableVisibilityShortcut != 0,
        .isMaterialSimilarityTestEnabled = spatioTemporal.enableMaterialSimilarityTest != 0,
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
    // the library default is TemporalAndSpatial; the plan runs one fused pass (§2 #12). Both modes give the shading
    // input the initial sampling output's index, so switching keeps the reservoir rotation intact
    context->SetResamplingMode(isReference ? rtxdi::ReSTIRDI_ResamplingMode::None
                                           : rtxdi::ReSTIRDI_ResamplingMode::FusedSpatiotemporal);
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

    RTXDI_DISpatioTemporalResamplingParameters spatioTemporal = context->GetSpatioTemporalResamplingParameters();
    spatioTemporal.numSamples = settings.spatialSamples;
    spatioTemporal.numDisocclusionBoostSamples = settings.disocclusionBoostSamples;
    spatioTemporal.samplingRadius = settings.samplingRadius;
    spatioTemporal.maxHistoryLength = settings.maxHistoryLength;
    spatioTemporal.depthThreshold = settings.depthThreshold;
    spatioTemporal.normalThreshold = settings.normalThreshold;
    spatioTemporal.biasCorrectionMode = settings.biasCorrection;
    spatioTemporal.enableVisibilityShortcut = settings.isVisibilityShortcutEnabled ? 1u : 0u;
    spatioTemporal.enableMaterialSimilarityTest = settings.isMaterialSimilarityTestEnabled ? 1u : 0u;
    // permutation decorrelates frames for denoisers, but the frame-to-frame change grew from 0.8% to 20.5% of the
    // signal (lamp, static camera); revisit with DLSS-RR (H1). Turning it on also needs this setter every frame:
    // SetFrameIndex rehashes only the temporal pass's uniformRandomNumber, so the fused one would stay 0
    spatioTemporal.enablePermutationSampling = 0;
    context->SetSpatioTemporalResamplingParameters(spatioTemporal);

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
        logInfo(std::format("ReSTIR DI: {}", isReference ? "reference (no reuse)" : "fused spatiotemporal"), "Rtxdi");
    } else if (settings != appliedSettings) {
        configure();
    }
    // rotates the reservoir buffer indices and the frame's random number
    context->SetFrameIndex(frameIndex);

    // local lights lead the world light rows (AssetBuffers::uploadLights); RAB_TranslateLightIndex is the identity
    const uint32_t lightCount = assets.uploadedCount(AssetBuffer::Lights);
    const uint32_t localLightCount = assets.localLightCount();
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
        .bufferIndices = context->GetBufferIndices(),
        .initialSampling = context->GetInitialSamplingParameters(),
        .temporalResampling = context->GetTemporalResamplingParameters(),
        .boilingFilter = context->GetBoilingFilterParameters(),
        .spatialResampling = context->GetSpatialResamplingParameters(),
        .spatioTemporalResampling = context->GetSpatioTemporalResamplingParameters(),
        .shading = context->GetShadingParameters(),
        .reservoirs = reservoirAddress,
        .neighborOffsets = neighborOffsets.address,
        .isFusedResampling =
            context->GetResamplingMode() == rtxdi::ReSTIRDI_ResamplingMode::FusedSpatiotemporal ? 1u : 0u,
        .reservoirCount = reservoirCount,
    };
    std::memcpy(constants[frameSlot].mapped, &data, sizeof(data));
}
