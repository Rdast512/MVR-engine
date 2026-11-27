#include "vk_rtxdi.hpp"
#include "push_data.hpp"
#include "static_headers/logger.hpp"
#include "util/debug.hpp"
#include "util/vk_tracy.hpp"

#include <Rtxdi/ImportanceSamplingContext.h>
#include <Rtxdi/LightSampling/RISBufferSegmentAllocator.h>
#include <Rtxdi/PT/ReSTIRPT.h>
#include <Rtxdi/RtxdiUtils.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <format>
#include <numeric>
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
// Settings::localLightSamples is indexed by the mode
static_assert(std::to_underlying(ReSTIRDI_LocalLightSamplingMode::Uniform) == 0);
static_assert(std::to_underlying(ReSTIRDI_LocalLightSamplingMode::Power_RIS) == 1);
static_assert(std::to_underlying(ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS) == 2);
static_assert(RtxdiContext::kLocalLightSamplingModeCount == 3);

namespace
{
    // RTXDI's default; a power of two, the shaders mask with neighborOffsetMask
    constexpr uint32_t kNeighborOffsetCount = 8192;
    // the table layout and the spatial pass's walk must match; 3.2's default
    constexpr ReSTIRDI_SpatialSamplingMode kNeighborSamplingMode =
        ReSTIRDI_SpatialSamplingMode::StratifiedLowDiscrepancy;
    // FillNeighborOffsetBuffer writes RG8_SNORM pairs; RTXDI_NEIGHBOR_OFFSETS_BUFFER reads them as float2
    constexpr float kSnormScale = 1.0f / 127.0f;

    // R32_FLOAT, as the sample's LocalLightPdf: flux sums need the range
    constexpr vk::Format kLightPdfFormat = vk::Format::eR32Sfloat;
    // match passes/light_pdf_mips.slang and passes/rtxdi_presample.slang (RTXDI_PRESAMPLING_GROUP_SIZE and the
    // ReGIR build group of the RTXDI sample)
    constexpr uint32_t kLightPdfMipsTile = 8;
    constexpr uint32_t kPresampleGroupSize = 256;
    constexpr uint32_t kRegirBuildGroupSize = 256;
    // RTXDI_RIS_BUFFER entry, and the two uint4 of its compact light
    constexpr vk::DeviceSize kRisEntryBytes = 2 * sizeof(uint32_t);
    constexpr vk::DeviceSize kRisLightDataBytes = 8 * sizeof(uint32_t);

    constexpr uint32_t divideRoundUp(uint32_t value, uint32_t divisor) { return (value + divisor - 1) / divisor; }

    struct NeighborOffset {
        float x;
        float y;
    };

    std::vector<NeighborOffset> makeNeighborOffsets(ReSTIRDI_SpatialSamplingMode mode)
    {
        std::vector<uint8_t> packed(static_cast<size_t>(kNeighborOffsetCount) * 2);
        rtxdi::FillNeighborOffsetBuffer(packed.data(), kNeighborOffsetCount, mode);
        std::vector<NeighborOffset> offsets(kNeighborOffsetCount);
        for (uint32_t i = 0; i < kNeighborOffsetCount; ++i) {
            offsets[i] = {.x = static_cast<float>(static_cast<int8_t>(packed[i * 2])) * kSnormScale,
                          .y = static_cast<float>(static_cast<int8_t>(packed[i * 2 + 1])) * kSnormScale};
        }
        return offsets;
    }

    // FillReGIRConstants of the RTXDI sample (LightingPasses.cpp); the center is set per frame
    ReGIR_Parameters makeRegirConstants(const rtxdi::ReGIRContext& regir)
    {
        const rtxdi::ReGIRStaticParameters staticParams = regir.GetReGIRStaticParameters();
        const rtxdi::ReGIRDynamicParameters dynamicParams = regir.GetReGIRDynamicParameters();
        const rtxdi::ReGIROnionCalculatedParameters onion = regir.GetReGIROnionCalculatedParameters();
        ReGIR_Parameters params{};
        params.gridParams.cellsX = staticParams.gridParameters.gridSize.x;
        params.gridParams.cellsY = staticParams.gridParameters.gridSize.y;
        params.gridParams.cellsZ = staticParams.gridParameters.gridSize.z;
        params.commonParams.numRegirBuildSamples = dynamicParams.regirNumBuildSamples;
        params.commonParams.risBufferOffset = regir.GetReGIRCellOffset();
        params.commonParams.lightsPerCell = staticParams.lightsPerCell;
        // the onion works with radii, the cell size is a diameter
        params.commonParams.cellSize = staticParams.mode == rtxdi::ReGIRMode::Onion
            ? dynamicParams.regirCellSize * 0.5f
            : dynamicParams.regirCellSize;
        params.commonParams.localLightSamplingFallbackMode = std::to_underlying(dynamicParams.fallbackSamplingMode);
        params.commonParams.localLightPresamplingMode = std::to_underlying(dynamicParams.presamplingMode);
        params.commonParams.samplingJitter = std::max(0.0f, dynamicParams.regirSamplingJitter * 2.0f);
        params.onionParams.cubicRootFactor = onion.regirOnionCubicRootFactor;
        params.onionParams.linearFactor = onion.regirOnionLinearFactor;
        params.onionParams.numLayerGroups = static_cast<uint32_t>(onion.regirOnionLayers.size());
        assert(onion.regirOnionLayers.size() <= RTXDI_ONION_MAX_LAYER_GROUPS);
        for (size_t group = 0; group < onion.regirOnionLayers.size(); ++group) {
            params.onionParams.layers[group] = onion.regirOnionLayers[group];
            params.onionParams.layers[group].innerRadius *= params.commonParams.cellSize;
            params.onionParams.layers[group].outerRadius *= params.commonParams.cellSize;
        }
        assert(onion.regirOnionRings.size() <= RTXDI_ONION_MAX_RINGS);
        std::ranges::copy(onion.regirOnionRings, params.onionParams.rings);
        return params;
    }

    constexpr std::string_view localLightSamplingName(ReSTIRDI_LocalLightSamplingMode mode)
    {
        switch (mode) {
        case ReSTIRDI_LocalLightSamplingMode::Power_RIS:
            return "Power_RIS";
        case ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS:
            return "ReGIR_RIS";
        default:
            return "uniform";
        }
    }

    void setDecorrelation(RtxdiContext::GiSettings& settings, const RTXDI_PTDecorrelationParameters& params)
    {
        settings.decorrelationMode = params.decorrelationMode;
        settings.decorrelationFactor = params.decorrelationFactor;
        settings.decorrelationStagnancyExponent = params.decorrelationStagnancyExponent;
        settings.decorrelationEmaFactor = params.decorrelationEmaFactor;
        settings.isFireflyReplacementEnabled = params.fireflyReplacementFilterEnable != 0;
        settings.fireflyReplacementStrength = params.fireflyReplacementFilterStrength;
        settings.isFireflyReplacementBounded = params.fireflyReplacementBiasReduction != 0;
        settings.fireflyReplacementMultiplyBound = params.fireflyReplacementMultiplyBound;
    }

    [[nodiscard]] RTXDI_PTDecorrelationParameters decorrelationParameters(const RtxdiContext::GiSettings& settings)
    {
        return RTXDI_PTDecorrelationParameters{
            .decorrelationFactor = settings.decorrelationFactor,
            .decorrelationStagnancyExponent = settings.decorrelationStagnancyExponent,
            .decorrelationEmaFactor = settings.decorrelationEmaFactor,
            .decorrelationMode = settings.decorrelationMode,
            .fireflyReplacementFilterEnable = settings.isFireflyReplacementEnabled ? 1u : 0u,
            .fireflyReplacementFilterStrength = settings.fireflyReplacementStrength,
            .fireflyReplacementBiasReduction = settings.isFireflyReplacementBounded ? 1u : 0u,
            .fireflyReplacementMultiplyBound = settings.fireflyReplacementMultiplyBound,
        };
    }
} // namespace

RtxdiContext::RtxdiContext(const Device& device, const VkAllocator& allocator, const FrameSync& frameSync,
                           DescriptorManager& descriptorManager, const ShaderObjects& shaderObjects,
                           const WorldLights& worldLights) :
    device(device), allocator(allocator), frameSync(frameSync), descriptorManager(descriptorManager),
    shaderObjects(shaderObjects), worldLights(worldLights)
{
    // two tables, as NVIDIA's FullSample (RtxdiResources.cpp:209-214, m8 I2 F16): LowDiscrepancy for GI's linear
    // spatial walk, StratifiedLowDiscrepancy for DI's stratified walk
    const std::vector<NeighborOffset> giOffsets = makeNeighborOffsets(ReSTIRDI_SpatialSamplingMode::LowDiscrepancy);
    const vk::DeviceSize offsetBytes = giOffsets.size() * sizeof(NeighborOffset);
    neighborOffsets = createHostBuffer(offsetBytes, allocator.allocator, device.vkdevice, device.queueFamilyIndices,
                                       "RtxdiNeighborOffsets", "RtxdiNeighborOffsets");
    std::memcpy(neighborOffsets.mapped, giOffsets.data(), offsetBytes);
    const std::vector<NeighborOffset> diOffsets = makeNeighborOffsets(kNeighborSamplingMode);
    diNeighborOffsets = createHostBuffer(offsetBytes, allocator.allocator, device.vkdevice, device.queueFamilyIndices,
                                         "RtxdiDiNeighborOffsets", "RtxdiDiNeighborOffsets");
    std::memcpy(diNeighborOffsets.mapped, diOffsets.data(), offsetBytes);
    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        constants[slot] = createHostBuffer(sizeof(GpuRtxdiConstants), allocator.allocator, device.vkdevice,
                                           device.queueFamilyIndices, std::format("RtxdiConstants_{}", slot),
                                           "RtxdiConstants");
    }
    const vk::PhysicalDeviceLimits& limits = device.capabilities.properties2.properties.limits;
    const auto queueFamilies = device.physicalDevice.getQueueFamilyProperties2();
    if (limits.timestampPeriod > 0.0f &&
        queueFamilies[device.computeIndex].queueFamilyProperties.timestampValidBits != 0) {
        lightPrepTimestampPeriodNs = limits.timestampPeriod;
        lightPrepTimestamps = vk::raii::QueryPool(
            device.vkdevice, vk::QueryPoolCreateInfo{
                                 .queryType = vk::QueryType::eTimestamp,
                                 .queryCount = kLightPrepQueriesPerFrame * MAX_FRAMES_IN_FLIGHT,
                             });
    }
    if (const char* text = std::getenv("ENGINE_DI_SAMPLING"); text != nullptr) {
        const std::string_view mode(text);
        if (mode == "regir") {
            settings.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS;
        } else if (mode == "power_ris") {
            settings.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Power_RIS;
        } else if (mode == "uniform") {
            settings.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Uniform;
        } else {
            logError(std::format("ENGINE_DI_SAMPLING='{}' is unknown; ignored", mode), "Rtxdi");
        }
    }
}

// the descriptor manager may already be gone; only the PDF image is released
RtxdiContext::~RtxdiContext()
{
    destroyReservoirs();
    destroyVmaBuffer(allocator.allocator, risBuffer, risMemory);
    destroyVmaBuffer(allocator.allocator, risLightData, risLightDataMemory);
    destroyVmaImage(allocator.allocator, lightPdf, lightPdfMemory);
    destroyHostBuffer(allocator.allocator, neighborOffsets, "RtxdiNeighborOffsets");
    destroyHostBuffer(allocator.allocator, diNeighborOffsets, "RtxdiDiNeighborOffsets");
    for (HostBuffer& slot : constants) {
        destroyHostBuffer(allocator.allocator, slot, "RtxdiConstants");
    }
}

rtxdi::ReSTIRDIContext& RtxdiContext::restirDi() const { return importanceSampling->GetReSTIRDIContext(); }

rtxdi::ReSTIRGIContext& RtxdiContext::restirGi() const { return importanceSampling->GetReSTIRGIContext(); }

void RtxdiContext::destroyReservoirs()
{
    releaseDiReservoirs();
    releaseGiReservoirs();
}

void RtxdiContext::ensureDiReservoirs()
{
    if (reservoirs != nullptr) {
        return;
    }
    const RTXDI_ReservoirBufferParameters params = restirDi().GetReservoirBufferParameters();
    reservoirCount = params.reservoirArrayPitch * rtxdi::c_NumReSTIRDIReservoirBuffers;
    const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(reservoirCount) * sizeof(RTXDI_PackedDIReservoir);
    createBuffer(bytes, vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eDeviceLocal, reservoirs, reservoirMemory, allocator.allocator,
                 device.vkdevice, device.queueFamilyIndices, "RtxdiReservoirs");
    reservoirAddress = device.vkdevice.getBufferAddress({.buffer = *reservoirs});
    logInfo(std::format("ReSTIR DI reservoirs: {} buffers of {} ({:.1f} MiB)", rtxdi::c_NumReSTIRDIReservoirBuffers,
                        params.reservoirArrayPitch, static_cast<double>(bytes) / (1024.0 * 1024.0)),
            "Rtxdi");
}

void RtxdiContext::releaseDiReservoirs()
{
    if (reservoirs == nullptr) {
        return;
    }
    destroyVmaBuffer(allocator.allocator, reservoirs, reservoirMemory);
    reservoirAddress = 0;
    reservoirCount = 0;
    logInfo("ReSTIR DI reservoirs retired", "Rtxdi");
}

void RtxdiContext::ensureGiReservoirs()
{
    if (giReservoirs != nullptr) {
        return;
    }
    const RTXDI_ReservoirBufferParameters params = restirGi().GetReservoirBufferParameters();
    giReservoirCount = params.reservoirArrayPitch * kGiReservoirArrays;
    const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(giReservoirCount) * sizeof(RTXDI_PackedGIReservoir);
    createBuffer(bytes, vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eDeviceLocal, giReservoirs, giReservoirMemory, allocator.allocator,
                 device.vkdevice, device.queueFamilyIndices, "RtxdiGiReservoirs");
    giReservoirAddress = device.vkdevice.getBufferAddress({.buffer = *giReservoirs});
    logInfo(std::format("ReSTIR GI reservoirs: {} buffers of {} ({:.1f} MiB), {}", kGiReservoirArrays,
                        params.reservoirArrayPitch, static_cast<double>(bytes) / (1024.0 * 1024.0),
                        isGiReuseApplied ? "temporal and spatial" : "no reuse"),
            "Rtxdi");
}

void RtxdiContext::releaseGiReservoirs()
{
    if (giReservoirs == nullptr) {
        return;
    }
    destroyVmaBuffer(allocator.allocator, giReservoirs, giReservoirMemory);
    giReservoirAddress = 0;
    giReservoirCount = 0;
    logInfo("ReSTIR GI reservoirs retired", "Rtxdi");
}

void RtxdiContext::createRisBuffers()
{
    const uint32_t entryCount = importanceSampling->GetRISBufferSegmentAllocator().GetTotalSizeInElements();
    if (entryCount == risEntryCount) {
        return;
    }
    destroyVmaBuffer(allocator.allocator, risBuffer, risMemory);
    destroyVmaBuffer(allocator.allocator, risLightData, risLightDataMemory);
    risEntryCount = entryCount;
    constexpr vk::BufferUsageFlags2 kUsage =
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress;
    const vk::DeviceSize risBytes = entryCount * kRisEntryBytes;
    const vk::DeviceSize lightDataBytes = entryCount * kRisLightDataBytes;
    createBuffer(risBytes, kUsage, vk::MemoryPropertyFlagBits::eDeviceLocal, risBuffer, risMemory,
                 allocator.allocator, device.vkdevice, device.queueFamilyIndices, "RtxdiRisBuffer");
    createBuffer(lightDataBytes, kUsage, vk::MemoryPropertyFlagBits::eDeviceLocal, risLightData,
                 risLightDataMemory, allocator.allocator, device.vkdevice, device.queueFamilyIndices,
                 "RtxdiRisLightData");
    risAddress = device.vkdevice.getBufferAddress({.buffer = *risBuffer});
    risLightDataAddress = device.vkdevice.getBufferAddress({.buffer = *risLightData});
    const RTXDI_RISBufferSegmentParameters& localTiles = importanceSampling->GetLocalLightRISBufferSegmentParams();
    const rtxdi::ReGIRContext& regir = importanceSampling->GetReGIRContext();
    logInfo(std::format("RIS buffers: {} entries ({:.1f} + {:.1f} MiB): {} local light tiles of {}, ReGIR {} cells "
                        "of {} at {}",
                        entryCount, static_cast<double>(risBytes) / (1024.0 * 1024.0),
                        static_cast<double>(lightDataBytes) / (1024.0 * 1024.0), localTiles.tileCount,
                        localTiles.tileSize, regir.GetReGIROnionCalculatedParameters().regirOnionCells,
                        regir.GetReGIRStaticParameters().lightsPerCell, regir.GetReGIRCellOffset()),
            "Rtxdi");
}

void RtxdiContext::destroyLightPdf()
{
    for (const uint32_t index : lightPdfMipIndices) {
        descriptorManager.freeImageDescriptor(index);
    }
    lightPdfMipIndices.clear();
    if (lightPdfIndex != kNoneIndex) {
        descriptorManager.freeImageDescriptor(lightPdfIndex);
        lightPdfIndex = kNoneIndex;
    }
    destroyVmaImage(allocator.allocator, lightPdf, lightPdfMemory);
    lightPdfSide = 0;
    isLightPdfLayoutPending = false;
}

void RtxdiContext::createLightPdf(uint32_t localLightCount)
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipCount = 0;
    // powers of two: a scene that loads lights one model at a time reallocates rarely
    rtxdi::ComputePdfTextureSize(std::bit_ceil(localLightCount), width, height, mipCount);
    // square (width >= height): every 2x2 footprint of RTXDI's mip walk and of the mip build lies inside the texture,
    // and the top mip times the texel count is the total power, as RAB_EvaluateLocalLightSourcePdf assumes
    lightPdfSide = width;
    const bool isShared = device.queueFamilyIndices.size() > 1;
    const vk::ImageCreateInfo imageInfo{
        .imageType = vk::ImageType::e2D,
        .format = kLightPdfFormat,
        .extent = {lightPdfSide, lightPdfSide, 1},
        .mipLevels = mipCount,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        // written on async compute, read by the graphics queue's ray passes every frame: no ownership transfers
        .sharingMode = isShared ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive,
        .queueFamilyIndexCount = isShared ? static_cast<uint32_t>(device.queueFamilyIndices.size()) : 0u,
        .pQueueFamilyIndices = isShared ? device.queueFamilyIndices.data() : nullptr,
    };
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    allocator.allocateImage(imageInfo, allocInfo, lightPdf, lightPdfMemory, "RtxdiLightPdfMemory");
    setDebugName(device.vkdevice, lightPdf, "RtxdiLightPdf");

    const auto pdfView = [this](uint32_t baseMip, uint32_t levels) {
        return vk::ImageViewCreateInfo{.image = *lightPdf,
                                       .viewType = vk::ImageViewType::e2D,
                                       .format = kLightPdfFormat,
                                       .subresourceRange = {vk::ImageAspectFlagBits::eColor, baseMip, levels, 0, 1}};
    };
    lightPdfIndex = descriptorManager.writeImageDescriptor(pdfView(0, mipCount), vk::DescriptorType::eSampledImage,
                                                           vk::ImageLayout::eGeneral);
    lightPdfMipIndices.reserve(mipCount);
    for (uint32_t mip = 0; mip < mipCount; ++mip) {
        lightPdfMipIndices.push_back(descriptorManager.writeImageDescriptor(
            pdfView(mip, 1), vk::DescriptorType::eStorageImage, vk::ImageLayout::eGeneral));
    }
    isLightPdfLayoutPending = true;
    TracyMessageL("RTXDI light PDF grown");
    logInfo(std::format("light PDF: {0}x{0}, {1} mips for {2} local lights", lightPdfSide, mipCount, localLightCount),
            "Rtxdi");
}

void RtxdiContext::recreate(vk::Extent2D renderExtent)
{
    importanceSampling = std::make_unique<rtxdi::ImportanceSamplingContext>(
        rtxdi::ImportanceSamplingContext_StaticParameters{
            .NeighborOffsetCount = kNeighborOffsetCount,
            .renderWidth = renderExtent.width,
            .renderHeight = renderExtent.height,
            .CheckerboardSamplingMode = rtxdi::CheckerboardMode::Off,
        });
    regirConstants = makeRegirConstants(importanceSampling->GetReGIRContext());
    configure();

    // M8.3 PERF-D2: DI and GI reservoirs are allocated lazily in prepareFrame only while RestirDi / RestirGi runs
    destroyReservoirs();
    lastDiReservoirsFrame = 0;
    lastGiReservoirsFrame = 0;

    configureGi(isGiReuseApplied);
    createRisBuffers();

    logInfo(std::format("RTXDI {}x{}: {} ({}), {} local light sampling, {} neighbor offsets (reservoirs lazy)",
                        renderExtent.width, renderExtent.height, directModeLabel(directMode),
                        directResamplingModeLabel(directResampling),
                        localLightSamplingName(restirDi().GetInitialSamplingParameters().localLightSamplingMode),
                        kNeighborOffsetCount),
            "Rtxdi");
}

RtxdiContext::Settings RtxdiContext::nvidiaPreset(QualityPreset preset)
{
    const RTXDI_DIInitialSamplingParameters initial = rtxdi::GetDefaultReSTIRDIInitialSamplingParams();
    const RTXDI_DITemporalResamplingParameters temporal = rtxdi::GetDefaultReSTIRDITemporalResamplingParams();
    const RTXDI_DISpatialResamplingParameters spatial = rtxdi::GetDefaultReSTIRDISpatialResamplingParams();
    const RTXDI_BoilingFilterParameters boiling = rtxdi::GetDefaultReSTIRDIBoilingFilterParams();
    const RTXDI_ShadingParameters shading = rtxdi::GetDefaultReSTIRDIShadingParams();
    Settings settings;
    settings.localLightSamplingMode = initial.localLightSamplingMode;
    settings.localLightSamples.fill(initial.numLocalLightSamples);
    settings.infiniteLightSamples = initial.numInfiniteLightSamples;
    settings.brdfSamples = initial.numBrdfSamples;
    settings.isInitialVisibilityEnabled = initial.enableInitialVisibility != 0;
    settings.maxHistoryLength = temporal.maxHistoryLength;
    settings.temporalBiasCorrection = temporal.biasCorrectionMode;
    settings.isVisibilityShortcutEnabled = temporal.enableVisibilityShortcut != 0;
    settings.isFallbackSamplingEnabled = temporal.enableFallbackSampling != 0;
    settings.isPermutationSamplingEnabled = temporal.enablePermutationSampling != 0;
    settings.spatialSamples = spatial.numSamples;
    settings.disocclusionBoostSamples = spatial.numDisocclusionBoostSamples;
    settings.disocclusionHistoryLength = spatial.targetHistoryLength;
    settings.samplingRadius = spatial.samplingRadius;
    settings.spatialBiasCorrection = spatial.biasCorrectionMode;
    settings.isMaterialSimilarityTestEnabled = spatial.enableMaterialSimilarityTest != 0;
    settings.depthThreshold = spatial.depthThreshold;
    settings.normalThreshold = spatial.normalThreshold;
    settings.isBoilingFilterEnabled = boiling.enableBoilingFilter != 0;
    settings.boilingFilterStrength = boiling.boilingFilterStrength;
    settings.isFinalVisibilityReused = shading.reuseFinalVisibility != 0;

    // RTXDI Samples/FullSample UserInterface.cpp UIData::ApplyPreset. Skipped, no engine field: checkerboard rendering
    // (Fast), lightingSettings.enableGradients and enablePrevTLAS. brdfptParams.enableSecondaryResampling and its
    // spatial parameters are GI rows (GiSettings::isSecondaryResamplingEnabled, path_shading.slang)
    const auto apply = [&settings](QualityPreset applied)
    {
        switch (applied) {
        case QualityPreset::Fast:
            settings.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Power_RIS;
            settings.localLightSamples = {4, 4, 4};
            settings.brdfSamples = 0;
            settings.infiniteLightSamples = 1;
            settings.isVisibilityShortcutEnabled = true;
            settings.isBoilingFilterEnabled = true;
            settings.boilingFilterStrength = 0.2f;
            settings.temporalBiasCorrection = ReSTIRDI_TemporalBiasCorrectionMode::Off;
            settings.spatialBiasCorrection = ReSTIRDI_SpatialBiasCorrectionMode::Off;
            settings.spatialSamples = 1;
            settings.disocclusionBoostSamples = 2;
            settings.isFinalVisibilityReused = true;
            break;
        case QualityPreset::Medium:
            settings.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS;
            settings.localLightSamples = {8, 8, 8};
            settings.brdfSamples = 1;
            settings.infiniteLightSamples = 1;
            settings.isVisibilityShortcutEnabled = true;
            settings.isBoilingFilterEnabled = true;
            settings.boilingFilterStrength = 0.2f;
            settings.temporalBiasCorrection = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced;
            settings.spatialBiasCorrection = ReSTIRDI_SpatialBiasCorrectionMode::Basic;
            settings.spatialSamples = 1;
            settings.disocclusionBoostSamples = 8;
            settings.isFinalVisibilityReused = true;
            break;
        case QualityPreset::Unbiased:
            settings.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Uniform;
            settings.localLightSamples = {8, 8, 16};
            settings.brdfSamples = 1;
            settings.infiniteLightSamples = 1;
            settings.isVisibilityShortcutEnabled = false;
            settings.isBoilingFilterEnabled = false;
            settings.boilingFilterStrength = 0.0f;
            settings.temporalBiasCorrection = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced;
            settings.spatialBiasCorrection = ReSTIRDI_SpatialBiasCorrectionMode::Raytraced;
            settings.spatialSamples = 1;
            settings.disocclusionBoostSamples = 8;
            settings.isFinalVisibilityReused = false;
            break;
        case QualityPreset::Ultra:
            settings.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS;
            settings.localLightSamples = {16, 16, 16};
            settings.brdfSamples = 1;
            settings.infiniteLightSamples = 1;
            settings.isVisibilityShortcutEnabled = false;
            settings.isBoilingFilterEnabled = false;
            settings.boilingFilterStrength = 0.0f;
            settings.temporalBiasCorrection = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced;
            settings.spatialBiasCorrection = ReSTIRDI_SpatialBiasCorrectionMode::Raytraced;
            settings.spatialSamples = 4;
            settings.disocclusionBoostSamples = 16;
            settings.isFinalVisibilityReused = false;
            break;
        case QualityPreset::Reference:
            // resampling None is isReference, which also overrides the candidate counts
            settings.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Uniform;
            settings.localLightSamples = {16, 16, 0};
            settings.brdfSamples = 1;
            settings.infiniteLightSamples = 1;
            settings.isBoilingFilterEnabled = false;
            settings.boilingFilterStrength = 0.0f;
            break;
        case QualityPreset::Count:
            break;
        }
    };
    // the sample starts in Medium, and a preset leaves the fields it does not set where they were
    apply(QualityPreset::Medium);
    if (preset != QualityPreset::Medium) {
        apply(preset);
    }
    // NormalizeDISpatialSampleCount: the stratified walk takes neighbors in pairs, 2 to 32
    constexpr uint32_t kMaxSpatialSamples = 32;
    for (uint32_t* count : {&settings.spatialSamples, &settings.disocclusionBoostSamples}) {
        *count = (std::clamp(*count, 2u, kMaxSpatialSamples) + 1u) & ~1u;
    }
    return settings;
}

RtxdiContext::Settings RtxdiContext::nvidiaSettings() { return nvidiaPreset(QualityPreset::Medium); }

RtxdiContext::Settings RtxdiContext::defaultSettings() { return nvidiaSettings(); }

RtxdiContext::Settings RtxdiContext::tunedSettings()
{
    // M8 I6 tuning over NVIDIA's Medium preset (spec/fixesM1-6/i6_restir_di_tuning.md):
    // - Power_RIS removes ReGIR's async presample stall on multi-light scenes (F12) and its PDF mismatch with BRDF
    //   candidates
    // - Fresh final visibility (isFinalVisibilityReused = false, O10) eliminates shadow-edge light leak and keeps
    //   reservoir age at 0 so the temporal visibility shortcut fires every frame
    // - Ray-traced disocclusion boost (8 samples while M < 4, F3)
    // - Two spatial samples (m8 I15, F22): without spatial reuse DLSS-RR keeps per-pixel sticky DI noise as confetti in
    //   emissive-lit interiors (café Lit flicker 79% / 70% -> 33% / 37%), +0.25 ms on the sunlit courtyard
    // - Permutation sampling without the boiling filter (m8 I16, F28): permutation decorrelates the DI input DLSS-RR
    //   sees (lag-1 0.65 -> 0.04 in the café); the boiling filter dropped the extra variance as "boiling" and darkened
    //   the café's dim tiles by 8-10%, so it goes with it
    Settings settings = nvidiaSettings();
    settings.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Power_RIS;
    settings.isPermutationSamplingEnabled = true;
    settings.isBoilingFilterEnabled = false;
    settings.spatialSamples = 2;
    // - The sun outside RIS (m8 I16 F28): spatial reuse turned the exact delta sun into 21% raw noise in the courtyard
    settings.areInfiniteLightsShadedExactly = true;
    settings.disocclusionHistoryLength = 4;
    // - Four frames of history (m8 I16 pops): with permutation a lucky sample walks between neighbors for up to 20
    //   frames, which DLSS-RR shows as local light pops; at 4 the café's pops are 38% fewer and 40% dimmer, Lit std
    //   -13% / -21%, Bistro street flicker 27% -> 6%, DI bias unchanged
    settings.maxHistoryLength = 4;
    // - Per-pixel light tiles with 32 local candidates (m8 I16 §7.9): a 16x16 tile's shared RIS tile made its lucky
    //   draws light the tile at once (pops); per pixel, the 32 candidates are independent across pixels. Café Lit std
    //   3.36 / 3.93% -> 2.76 / 2.95%, dining pops 56% -> 27%, DI bias unchanged, +0.54 ms on Bistro street
    settings.areLightTilesPerPixel = true;
    settings.localLightSamples.fill(32);
    settings.spatialBiasCorrection = ReSTIRDI_SpatialBiasCorrectionMode::Raytraced;
    settings.isFinalVisibilityReused = false;
    return settings;
}
RtxdiContext::Settings RtxdiContext::convergedSettings()
{
    Settings converged = tunedSettings();
    converged.brdfSamples = 0;
    // ReGIR's cells select by volume weight but BRDF candidates are weighed against the power PDF, as in the RTXDI
    // sample: a structured +0.19% on EmissiveStrengthTest. Power_RIS weighs both by the same PDF
    converged.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Power_RIS;
    converged.temporalBiasCorrection = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced;
    converged.isVisibilityShortcutEnabled = false;
    converged.isFallbackSamplingEnabled = false;
    converged.isPermutationSamplingEnabled = false;
    converged.spatialSamples = 0;
    converged.disocclusionHistoryLength = 0;
    // the reference converges through history, not through the denoiser
    converged.maxHistoryLength = nvidiaSettings().maxHistoryLength;
    converged.spatialBiasCorrection = ReSTIRDI_SpatialBiasCorrectionMode::Raytraced;
    converged.isBoilingFilterEnabled = false;
    converged.isFinalVisibilityReused = false;
    return converged;
}

RtxdiContext::Settings RtxdiContext::targetRisSettings()
{
    Settings s = nvidiaSettings();
    s.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Power_RIS;
    s.localLightSamples.fill(16);
    s.infiniteLightSamples = 1;
    s.brdfSamples = 0;
    s.isInitialVisibilityEnabled = false;
    s.areInfiniteLightsShadedExactly = true;
    s.areLightTilesPerPixel = true;
    return s;
}

RtxdiContext::Settings RtxdiContext::targetTemporalSettings()
{
    Settings s = nvidiaSettings();
    s.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Power_RIS;
    s.localLightSamples.fill(8);
    s.infiniteLightSamples = 1;
    s.brdfSamples = 0;
    s.maxHistoryLength = 6;
    s.temporalBiasCorrection = ReSTIRDI_TemporalBiasCorrectionMode::Basic;
    s.areInfiniteLightsShadedExactly = true;
    s.areLightTilesPerPixel = true;
    s.isFinalVisibilityReused = false;
    return s;
}

RtxdiContext::Settings RtxdiContext::analogUniformRisSettings()
{
    // Matches RTXPT IntroSample (IntroRTXDIBridge.hlsli:76-96) and RTXGI IndirectDiffuse (Pathtracer.hlsl:35)
    Settings s = nvidiaSettings();
    s.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Uniform;
    s.localLightSamples.fill(8);
    s.infiniteLightSamples = 1;
    s.brdfSamples = 0;
    s.isInitialVisibilityEnabled = false;
    s.areInfiniteLightsShadedExactly = false;
    s.areLightTilesPerPixel = false;
    return s;
}

RtxdiContext::GiSettings RtxdiContext::nvidiaGiSettings()
{
    const RTXDI_GITemporalResamplingParameters temporal = rtxdi::GetDefaultReSTIRGITemporalResamplingParams();
    const RTXDI_GISpatialResamplingParameters spatial = rtxdi::GetDefaultReSTIRGISpatialResamplingParams();
    const RTXDI_BoilingFilterParameters boiling = rtxdi::GetDefaultReSTIRGIBoilingFilterParams();
    const RTXDI_GIFinalShadingParameters shading = rtxdi::GetDefaultReSTIRGIFinalShadingParams();
    // RTXDI Samples/FullSample UserInterface.cpp: the library defaults, which no preset changes
    GiSettings settings{
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
        // the sample's BRDF ray tracing default (LightingPasses.cpp), not a library parameter
        .isSecondaryRoughnessFloorEnabled = true,
    };
    // ReSTIR GI has no decorrelation or history reduction: ReSTIR PT's parameters without DLSS-RR, both off
    setDecorrelation(settings, rtxdi::GetDefaultReSTIRPTDecorrelationParams(false));
    const RTXDI_PTTemporalResamplingParameters ptTemporal = rtxdi::GetDefaultReSTIRPTTemporalResamplingParams(false);
    settings.isDuplicationHistoryReductionEnabled = ptTemporal.duplicationBasedHistoryReduction != 0;
    settings.historyReductionStrength = ptTemporal.historyReductionStrength;
    return settings;
}

RtxdiContext::GiSettings RtxdiContext::defaultGiSettings() { return nvidiaGiSettings(); }

RtxdiContext::GiSettings RtxdiContext::tunedGiSettings()
{
    // Basic spatial correction darkens occluded places: -27% / -9% against the one-bounce ground truth in ReSTIR GI
    // mode (considerations C-G5-2, spec/fixesM1-6/m8_optimization_and_fixes.md O1). The temporal pass's Basic
    // correction does not; rays there cost 0.06 ms for nothing
    GiSettings settings = nvidiaGiSettings();
    settings.spatialBiasCorrection = RTXDI_GIBiasCorrectionMode::Raytraced;
    // tuned for DLSS-RR: ReSTIR PT's DLSS-RR profile, which the RTXDI sample applies whenever
    // DLSS-RR runs (O11). Stagnancy decorrelation and firefly replacement take the boiling filter's place, with 20
    // frames of history; without the replacement those three let fireflies through (O1)
    const RTXDI_PTTemporalResamplingParameters temporal = rtxdi::GetDefaultReSTIRPTTemporalResamplingParams(true);
    settings.maxHistoryLength = temporal.maxHistoryLength;
    settings.isDuplicationHistoryReductionEnabled = temporal.duplicationBasedHistoryReduction != 0;
    settings.historyReductionStrength = temporal.historyReductionStrength;
    // the profile turns age-based rejection off; m8 I15 (F23) keeps it at the profile's 30 frames: unlimited age let
    // stagnant samples brighten the café's dim tiles by +31% against one bounce (+11% at 30), and it halves Sponza
    // gallery Lit flicker (39% -> 21%); courtyard reads -1.8% (I5 saw -2.4% with two spatial samples)
    settings.maxReservoirAge = temporal.maxReservoirAge;
    settings.isBoilingFilterEnabled = rtxdi::GetDefaultReSTIRPTBoilingFilterParams(true).enableBoilingFilter != 0;
    setDecorrelation(settings, rtxdi::GetDefaultReSTIRPTDecorrelationParams(true));
    // two PT parts darken GI against the one-bounce ground truth (gallery / courtyard, O11 / I5): duplication history
    // reduction (-5.6% / -7.6%, sample-correlated M cap) and the fresh-swap bound (-7.1% / -3.9%, clamps 1/pdf against
    // small resampled weightSum); without both and with PT's 1 spatial sample (I5, -0.23 ms ray-traced spatial cost),
    // shipped GI stays within +-1.5% on both poses while improving DLSS-RR stability
    settings.spatialSamples = rtxdi::GetDefaultReSTIRPTSpatialResamplingParams(true).numSpatialSamples;
    settings.isDuplicationHistoryReductionEnabled = false;
    settings.isFireflyReplacementBounded = false;
    settings.isFullTileBoilingFilterEnabled = true;
    // m8 I16 phase 2c: the sun outside X2's RIS takes the courtyard's dim tiles from -7.0% to -0.7% against eight
    // bounces at no measured cost; noise unchanged
    settings.areInfiniteLightsShadedExactly = true;
    return settings;
}

RtxdiContext::GiSettings RtxdiContext::convergedGiSettings()
{
    GiSettings converged = nvidiaGiSettings();
    converged.temporalBiasCorrection = RTXDI_GIBiasCorrectionMode::Raytraced;
    converged.spatialBiasCorrection = RTXDI_GIBiasCorrectionMode::Raytraced;
    // keeping only reservoirs whose sample is young keeps the ones a bright candidate won recently: +5.6% on the
    // gallery at RTXDI's 30 frames (considerations C-G5-1)
    converged.maxReservoirAge = kGiUnlimitedSampleAge;
    // a zero-motion sample without the surface tests: locally biased, usually darker (ReSTIRGIParameters.h)
    converged.isFallbackSamplingEnabled = false;
    converged.isPermutationSamplingEnabled = false;
    converged.isBoilingFilterEnabled = false;
    // darkens or brightens glossy X2 against the traced BRDF
    converged.isSecondaryRoughnessFloorEnabled = false;
    return converged;
}

void RtxdiContext::applyDenoiserCompatDefaults(GiSettings& settings, bool isDlssRr)
{
    if (isDlssRr) {
        // the shipped values: NVIDIA's DLSS-RR preset changes ReSTIR PT only, so with DLSS-RR ReSTIR GI is the
        // library's
        const GiSettings defaults = defaultGiSettings();
        settings.maxHistoryLength = defaults.maxHistoryLength;
        settings.maxReservoirAge = defaults.maxReservoirAge;
        settings.isDuplicationHistoryReductionEnabled = defaults.isDuplicationHistoryReductionEnabled;
        setDecorrelation(settings, decorrelationParameters(defaults));
        settings.isBoilingFilterEnabled = defaults.isBoilingFilterEnabled;
        settings.boilingFilterStrength = defaults.boilingFilterStrength;
        return;
    }
    const RTXDI_PTTemporalResamplingParameters temporal = rtxdi::GetDefaultReSTIRPTTemporalResamplingParams(false);
    settings.maxHistoryLength = temporal.maxHistoryLength;
    settings.maxReservoirAge = temporal.enableAgeBasedRejection != 0 ? temporal.maxReservoirAge : kGiUnlimitedSampleAge;
    settings.isDuplicationHistoryReductionEnabled = temporal.duplicationBasedHistoryReduction != 0;
    setDecorrelation(settings, rtxdi::GetDefaultReSTIRPTDecorrelationParams(false));
    const RTXDI_BoilingFilterParameters boiling = rtxdi::GetDefaultReSTIRPTBoilingFilterParams(false);
    settings.isBoilingFilterEnabled = boiling.enableBoilingFilter != 0;
    settings.boilingFilterStrength = boiling.boilingFilterStrength;
}

RtxdiContext::AppliedParameters RtxdiContext::getAppliedParameters() const
{
    const rtxdi::ReGIRContext& regir = importanceSampling->GetReGIRContext();
    const rtxdi::ReSTIRDIContext& context = restirDi();
    return AppliedParameters{
        .regirStatic = regir.GetReGIRStaticParameters(),
        .regirDynamic = regir.GetReGIRDynamicParameters(),
        .regirLightSlotCount = regir.GetReGIRLightSlotCount(),
        .initialSampling = context.GetInitialSamplingParameters(),
        .temporalResampling = context.GetTemporalResamplingParameters(),
        .spatialResampling = context.GetSpatialResamplingParameters(),
        .shading = context.GetShadingParameters(),
    };
}

bool RtxdiContext::isReferenceRequested()
{
    const char* text = std::getenv("ENGINE_RTXDI_REFERENCE");
    return text != nullptr && std::string_view(text) == "1";
}

void RtxdiContext::configure()
{
    rtxdi::ReSTIRDIContext& context = restirDi();
    const bool isRef = isDirectReference();
    const bool isSpatialOnly =
        directMode == DirectMode::RestirDi && directResampling == DirectResamplingMode::Spatial;
    const bool isTemporalOnly =
        directMode == DirectMode::RestirDi && directResampling == DirectResamplingMode::Temporal;
    context.SetResamplingMode(directMode != DirectMode::RestirDi || isRef ? rtxdi::ReSTIRDI_ResamplingMode::None
                              : isSpatialOnly                             ? rtxdi::ReSTIRDI_ResamplingMode::Spatial
                              : isTemporalOnly                            ? rtxdi::ReSTIRDI_ResamplingMode::Temporal
                                                                          : rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial);
    RTXDI_DIInitialSamplingParameters initialSampling = rtxdi::GetDefaultReSTIRDIInitialSamplingParams();
    // Power_RIS: unbiased, and its 32 candidates converge the lamp's 5,985 bulb triangles in 1,024 frames, which
    // uniform selection does not (+0.3% residual at 8,192 frames)
    initialSampling.localLightSamplingMode =
        isRef ? ReSTIRDI_LocalLightSamplingMode::Power_RIS : settings.localLightSamplingMode;
    initialSampling.numBrdfSamples = isRef ? 0u : settings.brdfSamples;
    initialSampling.numLocalLightSamples = isRef ? kReferenceLocalLightSamples : settings.activeLocalLightSamples();
    initialSampling.numInfiniteLightSamples =
        isRef ? kReferenceInfiniteLightSamples : settings.infiniteLightSamples;
    // M8.1 BUG-C1 / M8.3: StreamingRis and Reference shade directly without reuse, so initial visibility stays off
    initialSampling.enableInitialVisibility =
        (directMode == DirectMode::RestirDi && !isRef && settings.isInitialVisibilityEnabled) ? 1u : 0u;
    // RAB_TraceRayForLocalLight traces from the offset brdfRayOrigin stashed in RAB_SurfaceImportanceSampleBrdf (I14)
    initialSampling.brdfRayMinT = 0.0f;
    context.SetInitialSamplingParameters(initialSampling);

    RTXDI_DITemporalResamplingParameters temporal = context.GetTemporalResamplingParameters();
    temporal.maxHistoryLength = settings.maxHistoryLength;
    temporal.biasCorrectionMode = settings.temporalBiasCorrection;
    temporal.depthThreshold = settings.depthThreshold;
    temporal.normalThreshold = settings.normalThreshold;
    temporal.enableVisibilityShortcut = settings.isVisibilityShortcutEnabled ? 1u : 0u;
    temporal.enableFallbackSampling = settings.isFallbackSamplingEnabled ? 1u : 0u;
    // SetFrameIndex rehashes uniformRandomNumber either way: the fallback sample permutes with it
    temporal.enablePermutationSampling = settings.isPermutationSamplingEnabled ? 1u : 0u;
    context.SetTemporalResamplingParameters(temporal);

    // M8.1 BUG-C2: StratifiedLowDiscrepancy walks pairs of quadrants; normalize non-zero counts to even [2, 32]
    constexpr auto normalizeSpatialCount = [](uint32_t count) -> uint32_t {
        constexpr uint32_t kMaxSpatialSamples = 32;
        return count == 0u ? 0u : (std::clamp(count, 2u, kMaxSpatialSamples) + 1u) & ~1u;
    };
    RTXDI_DISpatialResamplingParameters spatial = context.GetSpatialResamplingParameters();
    spatial.numSamples = normalizeSpatialCount(settings.spatialSamples);
    spatial.numDisocclusionBoostSamples = normalizeSpatialCount(settings.disocclusionBoostSamples);
    spatial.samplingRadius = settings.samplingRadius;
    spatial.biasCorrectionMode = settings.spatialBiasCorrection;
    spatial.depthThreshold = settings.depthThreshold;
    spatial.normalThreshold = settings.normalThreshold;
    spatial.enableMaterialSimilarityTest = settings.isMaterialSimilarityTestEnabled ? 1u : 0u;
    // without temporal reuse every reservoir has M == 1, which would trigger the disocclusion boost on every pixel
    spatial.targetHistoryLength = isSpatialOnly ? 0u : settings.disocclusionHistoryLength;
    spatial.neighborOffsetSamplingMode = kNeighborSamplingMode;
    context.SetSpatialResamplingParameters(spatial);

    context.SetBoilingFilterParameters(RTXDI_BoilingFilterParameters{
        .enableBoilingFilter = settings.isBoilingFilterEnabled ? 1u : 0u,
        .boilingFilterStrength = settings.boilingFilterStrength,
        .pad1 = 0,
        .pad2 = 0,
    });

    RTXDI_ShadingParameters shading = context.GetShadingParameters();
    shading.reuseFinalVisibility = settings.isFinalVisibilityReused ? 1u : 0u;
    context.SetShadingParameters(shading);
    appliedDirectMode = directMode;
    appliedDirectResampling = directResampling;
    appliedSettings = settings;
}

void RtxdiContext::configureGi(bool isGiReuse)
{
    rtxdi::ReSTIRGIContext& giContext = restirGi();
    giContext.SetResamplingMode(isGiReuse ? rtxdi::ReSTIRGI_ResamplingMode::TemporalAndSpatial
                                          : rtxdi::ReSTIRGI_ResamplingMode::None);
    RTXDI_GITemporalResamplingParameters temporal = giContext.GetTemporalResamplingParameters();
    temporal.maxHistoryLength = giSettings.maxHistoryLength;
    temporal.maxReservoirAge = giSettings.maxReservoirAge;
    temporal.biasCorrectionMode = giSettings.temporalBiasCorrection;
    temporal.enableFallbackSampling = giSettings.isFallbackSamplingEnabled ? 1u : 0u;
    temporal.enablePermutationSampling = giSettings.isPermutationSamplingEnabled ? 1u : 0u;
    temporal.depthThreshold = giSettings.depthThreshold;
    temporal.normalThreshold = giSettings.normalThreshold;
    giContext.SetTemporalResamplingParameters(temporal);

    RTXDI_GISpatialResamplingParameters spatial = giContext.GetSpatialResamplingParameters();
    spatial.numSamples = giSettings.spatialSamples;
    spatial.samplingRadius = giSettings.samplingRadius;
    spatial.biasCorrectionMode = giSettings.spatialBiasCorrection;
    spatial.depthThreshold = giSettings.depthThreshold;
    spatial.normalThreshold = giSettings.normalThreshold;
    giContext.SetSpatialResamplingParameters(spatial);

    giContext.SetBoilingFilterParameters(RTXDI_BoilingFilterParameters{
        .enableBoilingFilter = giSettings.isBoilingFilterEnabled ? 1u : 0u,
        .boilingFilterStrength = giSettings.boilingFilterStrength,
        .pad1 = 0,
        .pad2 = 0,
    });
    giContext.SetFinalShadingParameters(RTXDI_GIFinalShadingParameters{
        .enableFinalVisibility = giSettings.isFinalVisibilityEnabled ? 1u : 0u,
        .enableFinalMIS = giSettings.isFinalMisEnabled ? 1u : 0u,
        .pad1 = 0,
        .pad2 = 0,
    });
    isGiReuseApplied = isGiReuse;
    appliedGiSettings = giSettings;
}

void RtxdiContext::prepareFrame(uint32_t frameSlot, uint32_t frameIndex, bool isGiActive,
                                const glm::vec3& cameraPosition, uint64_t lastSubmittedFrame)
{
    collectLightPrepResults(frameSlot);
    rtxdi::ReSTIRDIContext& context = restirDi();
    rtxdi::ReSTIRGIContext& giContext = restirGi();
    // advance before changing resampling mode so SetResamplingMode rotates from last frame's output buffer
    const uint32_t rtxdiFrame = frameIndexOverride.value_or(frameIndex);
    context.SetFrameIndex(rtxdiFrame);
    giContext.SetFrameIndex(rtxdiFrame);
    if (directMode != appliedDirectMode || directResampling != appliedDirectResampling) {
        configure();
        logInfo(std::format("Direct lighting: {} ({})", directModeLabel(directMode),
                            directResamplingModeLabel(directResampling)),
                "Rtxdi");
    } else if (settings != appliedSettings) {
        configure();
    }
    const bool isGiReuse = isGiActive && !isGiReference;
    if (isGiReuse != isGiReuseApplied) {
        configureGi(isGiReuse);
        logInfo(std::format("ReSTIR GI: {}", isGiReuse ? "temporal and spatial" : "no reuse"), "Rtxdi");
    } else if (giSettings != appliedGiSettings) {
        configureGi(isGiReuse);
    }
    // M8.3 PERF-D2: lazily allocate DI/GI reservoirs only while RestirDi / RestirGi runs; retire once no in-flight
    // frame can reference them (zero waitIdle)
    const bool needsDiReservoirs = directMode == DirectMode::RestirDi;
    if (needsDiReservoirs) {
        ensureDiReservoirs();
        lastDiReservoirsFrame = frameIndex;
    } else if (reservoirs != nullptr && frameIndex >= lastDiReservoirsFrame + MAX_FRAMES_IN_FLIGHT) {
        releaseDiReservoirs();
    }
    if (isGiActive) {
        ensureGiReservoirs();
        lastGiReservoirsFrame = frameIndex;
    } else if (giReservoirs != nullptr && frameIndex >= lastGiReservoirsFrame + MAX_FRAMES_IN_FLIGHT) {
        releaseGiReservoirs();
    }
    // a settings change resets ReSTIR GI history, so the temporal pass never reads a map this flag did not write
    isGiDuplicationMapApplied = isGiReuseApplied && giSettings.isDuplicationHistoryReductionEnabled;
    const RTXDI_GIBufferIndices giBufferIndices = giContext.GetBufferIndices();
    // rtxdi_gi_shade reads neighbors from the temporal output and writes next frame's temporal input
    assert(giContext.GetResamplingMode() == rtxdi::ReSTIRGI_ResamplingMode::None ||
           (giBufferIndices.spatialResamplingInputBufferIndex == giBufferIndices.temporalResamplingOutputBufferIndex &&
            giBufferIndices.spatialResamplingOutputBufferIndex == giBufferIndices.temporalResamplingInputBufferIndex &&
            giBufferIndices.finalShadingInputBufferIndex == giBufferIndices.spatialResamplingOutputBufferIndex));
    const RTXDI_DIBufferIndices bufferIndices = context.GetBufferIndices();
    // rtxdi_temporal writes over its own pixel's initial reservoir, and its early-out leaves that as the output
    assert(bufferIndices.temporalResamplingOutputBufferIndex == bufferIndices.initialSamplingOutputBufferIndex);
    // rtxdi_shade reads neighbors from one buffer and writes the other; without spatial reuse it reads and writes its
    // own pixel only
    assert(bufferIndices.spatialResamplingInputBufferIndex != bufferIndices.shadingInputBufferIndex ||
           (std::to_underlying(context.GetResamplingMode()) &
            std::to_underlying(rtxdi::ReSTIRDI_ResamplingMode::Spatial)) == 0);

    const uint32_t localLightCount = worldLights.localLightCount();
    if (localLightCount > lightPdfSide * lightPdfSide) {
        ZoneScopedN("RtxdiContext::growLightPdf");
        // rare: only a local light count above every earlier one
        if (lastSubmittedFrame > 0 && lightPdf != nullptr) {
            const vk::SemaphoreWaitInfo waitInfo{
                .semaphoreCount = 1, .pSemaphores = &*frameSync.rayTimeline, .pValues = &lastSubmittedFrame};
            while (vk::Result::eTimeout == device.vkdevice.waitSemaphores(waitInfo, UINT64_MAX))
                ;
        }
        destroyLightPdf();
        createLightPdf(localLightCount);
    }

    // the RTXDI sample centers ReGIR on the camera (SceneRenderer::UpdateReGIRContextFromUI)
    rtxdi::ReGIRContext& regir = importanceSampling->GetReGIRContext();
    rtxdi::ReGIRDynamicParameters regirDynamic = regir.GetReGIRDynamicParameters();
    regirDynamic.center = {.x = cameraPosition.x, .y = cameraPosition.y, .z = cameraPosition.z};
    regir.SetDynamicParameters(regirDynamic);
    regirConstants.commonParams.centerX = cameraPosition.x;
    regirConstants.commonParams.centerY = cameraPosition.y;
    regirConstants.commonParams.centerZ = cameraPosition.z;

    // point, spot and triangle lights lead the world light rows (WorldLights), the directional rows and the sun end
    // them; RAB_TranslateLightIndex is the identity
    const RTXDI_LightBufferParameters lightBuffer{
        .localLightBufferRegion = {.firstLightIndex = 0, .numLights = localLightCount, .pad1 = 0, .pad2 = 0},
        .infiniteLightBufferRegion = {.firstLightIndex = worldLights.firstInfiniteLight(),
                                      .numLights = worldLights.infiniteLightCount(),
                                      .pad1 = 0,
                                      .pad2 = 0},
        // RTXDI tells a BRDF hit on the environment by index alone (InitialSampling.hlsli), so 0 would give local
        // light 0 the environment's MIS weight
        .environmentLightParams = {.lightPresent = 0, .lightIndex = RTXDI_INVALID_LIGHT_INDEX, .pad1 = 0, .pad2 = 0},
    };
    importanceSampling->SetLightBufferParams(lightBuffer);

    const GpuRtxdiConstants data{
        .runtime = context.GetRuntimeParams(),
        .lightBuffer = lightBuffer,
        .reservoirBuffer = context.GetReservoirBufferParameters(),
        .bufferIndices = bufferIndices,
        .initialSampling = context.GetInitialSamplingParameters(),
        .temporalResampling = context.GetTemporalResamplingParameters(),
        .boilingFilter = context.GetBoilingFilterParameters(),
        .spatialResampling = context.GetSpatialResamplingParameters(),
        .shading = context.GetShadingParameters(),
        .reservoirs = needsDiReservoirs ? reservoirAddress : 0,
        .neighborOffsets = neighborOffsets.address,
        .resamplingMode = std::to_underlying(context.GetResamplingMode()),
        .reservoirCount = needsDiReservoirs ? reservoirCount : 0,
        .giReservoirBuffer = giContext.GetReservoirBufferParameters(),
        .giBufferIndices = giBufferIndices,
        .giReservoirs = isGiActive ? giReservoirAddress : 0,
        .giReservoirCount = isGiActive ? giReservoirCount : 0,
        .giResamplingMode = std::to_underlying(giContext.GetResamplingMode()),
        .giTemporalResampling = giContext.GetTemporalResamplingParameters(),
        .giBoilingFilter = giContext.GetBoilingFilterParameters(),
        .giSpatialResampling = giContext.GetSpatialResamplingParameters(),
        .giFinalShading = giContext.GetFinalShadingParameters(),
        .giReferenceBounces = giReferenceBounces,
        .giFlags = (isSecondaryResamplingEffective() ? GiFlag::SecondaryResampling : 0u) |
                   (giSettings.isFireflyClampEnabled ? GiFlag::FireflyClamp : 0u) |
                   // without reuse the 1-sample estimate stays unbiased
                   (giSettings.isSecondaryRoughnessFloorEnabled && isGiReuseApplied ? GiFlag::SecondaryRoughnessFloor
                                                                                    : 0u) |
                   // without age-based rejection, as ReSTIR PT's DLSS-RR profile
                   (giSettings.maxReservoirAge >= kGiUnlimitedSampleAge ? GiFlag::SpatialAgeReset : 0u) |
                   (isGiDuplicationMapApplied ? GiFlag::DuplicationHistoryReduction : 0u) |
                   (giSettings.isFullTileBoilingFilterEnabled ? GiFlag::FullTileBoilingFilter : 0u) |
                   (arePathInfiniteLightsExact() ? GiFlag::ExactInfiniteLights : 0u),
        .localLightRisBuffer = importanceSampling->GetLocalLightRISBufferSegmentParams(),
        .environmentLightRisBuffer = importanceSampling->GetEnvironmentLightRISBufferSegmentParams(),
        .regir = regirConstants,
        .risBuffer = risAddress,
        .risLightData = risLightDataAddress,
        .risEntryCount = risEntryCount,
        .lightPdf = lightPdfIndex,
        .lightPdfSize = {.width = lightPdfSide, .height = lightPdfSide},
        .giDecorrelation = decorrelationParameters(giSettings),
        .giHistoryReductionStrength = giSettings.historyReductionStrength,
        .diFlags = (areDiInfiniteLightsExact() ? DiFlag::ExactInfiniteLights : 0u) |
            (settings.areLightTilesPerPixel ? DiFlag::PerPixelLightTiles : 0u),
        .diNeighborOffsets = diNeighborOffsets.address,
        .pathLocalLightSamples = pathLocalLightSamples(),
        .pad0 = 0,
    };
    std::memcpy(constants[frameSlot].mapped, &data, sizeof(data));
}

uint32_t RtxdiContext::pathLocalLightSamples() const
{
    const uint32_t primary = restirDi().GetInitialSamplingParameters().numLocalLightSamples;
    const bool isCapped =
        giReferenceBounces == 0 && !isSecondaryResamplingEffective() && giSettings.pathLocalLightSamples > 0;
    return isCapped ? std::min(primary, giSettings.pathLocalLightSamples) : primary;
}

void RtxdiContext::recordLightPreparation(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot,
                                          vk::DeviceAddress frameRoot)
{
    ZoneScopedN("RtxdiContext::recordLightPreparation");
    const uint32_t firstQuery = frameSlot * kLightPrepQueriesPerFrame;
    const bool isTimed = lightPrepTimestampPeriodNs > 0.0f;
    isLightPrepTimed[frameSlot] = isTimed;
    if (isTimed) {
        cmd.resetQueryPool(*lightPrepTimestamps, firstQuery, kLightPrepQueriesPerFrame);
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *lightPrepTimestamps, firstQuery);
    }
    if (isLightPdfLayoutPending) {
        // a new PDF holds nothing yet: light_world writes every mip 0 texel, the mips pass every other texel
        const vk::ImageMemoryBarrier2 layoutBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eNone,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = *lightPdf,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, vk::RemainingMipLevels, 0, 1},
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &layoutBarrier});
        isLightPdfLayoutPending = false;
    }
    const bool hasLightPdf = !lightPdfMipIndices.empty();
    worldLights.recordUpdate(cmd, frameSlot,
                             LightPdfTarget{.mip0 = hasLightPdf ? lightPdfMipIndices.front() : kNoneIndex,
                                            .texelCount = lightPdfSide * lightPdfSide});
    if (isTimed) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *lightPrepTimestamps, firstQuery + 1);
    }

    // Uniform reads neither the PDF nor the tiles (RAB_EvaluateLocalLightSourcePdf is 1 / count then)
    const bool isUniform = restirDi().GetInitialSamplingParameters().localLightSamplingMode ==
        ReSTIRDI_LocalLightSamplingMode::Uniform;
    if (isUniform || worldLights.localLightCount() == 0) {
        if (isTimed) {
            cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *lightPrepTimestamps, firstQuery + 2);
            cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *lightPrepTimestamps, firstQuery + 3);
        }
        return;
    }
    // each pass reads what the one before wrote: PDF levels, world rows, then the local light tiles
    const vk::MemoryBarrier2 passBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderSampledRead,
    };
    const vk::DependencyInfo passDependency{.memoryBarrierCount = 1, .pMemoryBarriers = &passBarrier};
    cmd.pipelineBarrier2(passDependency);

    ShaderObjects::bindComputeShader(cmd, shaderObjects.frameShaders.lightPdfMips);
    for (uint32_t mip = 1; mip < lightPdfMipIndices.size(); ++mip) {
        const uint32_t side = std::max(lightPdfSide >> mip, 1u);
        const LightPdfMipsPushData push{.srcImage = lightPdfMipIndices[mip - 1],
                                        .dstImage = lightPdfMipIndices[mip],
                                        .dstSize = {.width = side, .height = side}};
        cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
        cmd.dispatch(divideRoundUp(side, kLightPdfMipsTile), divideRoundUp(side, kLightPdfMipsTile), 1);
        cmd.pipelineBarrier2(passDependency);
    }
    if (isTimed) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *lightPrepTimestamps, firstQuery + 2);
    }

    const FramePushData push{.frameRoot = frameRoot};
    if (importanceSampling->IsLocalLightPowerRISEnabled()) {
        const RTXDI_RISBufferSegmentParameters& tiles = importanceSampling->GetLocalLightRISBufferSegmentParams();
        ShaderObjects::bindComputeShader(cmd, shaderObjects.frameShaders.rtxdiPresample);
        cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
        cmd.dispatch(divideRoundUp(tiles.tileSize, kPresampleGroupSize), tiles.tileCount, 1);
        cmd.pipelineBarrier2(passDependency);
    }
    if (importanceSampling->IsReGIREnabled()) {
        ShaderObjects::bindComputeShader(cmd, shaderObjects.frameShaders.rtxdiPresampleRegir);
        cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
        cmd.dispatch(divideRoundUp(importanceSampling->GetReGIRContext().GetReGIRLightSlotCount(),
                                   kRegirBuildGroupSize),
                     1, 1);
    }
    if (isTimed) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *lightPrepTimestamps, firstQuery + 3);
    }
}

void RtxdiContext::skipLightPreparation(uint32_t frameSlot) noexcept
{
    isLightPrepTimed[frameSlot] = false;
    lastLightPrepStartTick = 0;
    lastLightPrepEndTick = 0;
    lightPrepHistories[0][lightPrepHistoryHead] = 0.0f;
    lightPrepHistories[1][lightPrepHistoryHead] = 0.0f;
    lightPrepHistories[2][lightPrepHistoryHead] = 0.0f;
    lightPrepHistoryHead = (lightPrepHistoryHead + 1) % kLightPrepHistorySize;
    lightPrepHistoryCount = std::min(lightPrepHistoryCount + 1, kLightPrepHistorySize);
}

void RtxdiContext::collectLightPrepResults(uint32_t frameSlot)
{
    if (!isLightPrepTimed[frameSlot]) {
        return;
    }
    isLightPrepTimed[frameSlot] = false;
    const auto [result, ticks] = lightPrepTimestamps.getResults<uint64_t>(
        frameSlot * kLightPrepQueriesPerFrame, kLightPrepQueriesPerFrame,
        kLightPrepQueriesPerFrame * sizeof(uint64_t), sizeof(uint64_t), vk::QueryResultFlagBits::e64);
    if (result != vk::Result::eSuccess) {
        return;
    }
    const auto toMs = [this](uint64_t begin, uint64_t end) {
        return end > begin ? static_cast<float>(end - begin) * lightPrepTimestampPeriodNs * 1e-6f : 0.0f;
    };
    lastLightPrepStartTick = ticks[0];
    lastLightPrepEndTick = ticks[3];
    lightPrepHistories[0][lightPrepHistoryHead] = toMs(ticks[0], ticks[1]);
    lightPrepHistories[1][lightPrepHistoryHead] = toMs(ticks[1], ticks[2]);
    lightPrepHistories[2][lightPrepHistoryHead] = toMs(ticks[2], ticks[3]);
    TracyPlot("Light world (async)", static_cast<double>(lightPrepHistories[0][lightPrepHistoryHead]));
    TracyPlot("Light PDF mips (async)", static_cast<double>(lightPrepHistories[1][lightPrepHistoryHead]));
    TracyPlot("Light presample (async)", static_cast<double>(lightPrepHistories[2][lightPrepHistoryHead]));
    lightPrepHistoryHead = (lightPrepHistoryHead + 1) % kLightPrepHistorySize;
    lightPrepHistoryCount = std::min(lightPrepHistoryCount + 1, kLightPrepHistorySize);
}

RtxdiContext::LightPrepStats RtxdiContext::getLightPrepStats() const noexcept
{
    const auto average = [this](const std::array<float, kLightPrepHistorySize>& samples) {
        return lightPrepHistoryCount == 0
            ? 0.0f
            : std::reduce(samples.begin(), samples.end(), 0.0f) / static_cast<float>(lightPrepHistoryCount);
    };
    const float world = average(lightPrepHistories[0]);
    const float mips = average(lightPrepHistories[1]);
    const float presample = average(lightPrepHistories[2]);
    return LightPrepStats{
        .worldLightsMs = world,
        .pdfMipsMs = mips,
        .presampleMs = presample,
        .totalMs = world + mips + presample,
        .lastStartTick = lastLightPrepStartTick,
        .lastEndTick = lastLightPrepEndTick,
    };
}
