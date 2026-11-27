#pragma once
#include "../Constants.h"
#include "core/vk_allocator.hpp"
#include "core/vk_descriptors.hpp"
#include "core/vk_device.hpp"
#include "core/vk_frame_sync.hpp"
#include "util/vk_utils.hpp"
#include "pipeline_modes.hpp"
#include "vk_shader_objects.hpp"
#include "vk_world_lights.hpp"

#include <Rtxdi/DI/ReSTIRDIParameters.h>
#include <Rtxdi/GI/ReSTIRGIParameters.h>
#include <Rtxdi/LightSampling/RISBufferSegmentParameters.h>
#include <Rtxdi/PT/ReSTIRPTParameters.h>
#include <Rtxdi/ReGIR/ReGIR.h>
#include <Rtxdi/ReGIR/ReGIRParameters.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/vec3.hpp>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

namespace rtxdi
{
    class ImportanceSamplingContext;
    class ReSTIRDIContext;
    class ReSTIRGIContext;
}

// GpuFrameRoot.rtxdiConstants: every RTXDI parameter block of the frame plus the buffers RTXDI's resampling functions
// read. Scalar layout, 16-byte blocks in RTXDI's own field order. Slang mirror: shaders/lighting/rtxdi_constants.slang
struct GpuRtxdiConstants {
    RTXDI_RuntimeParameters runtime;
    RTXDI_LightBufferParameters lightBuffer;
    RTXDI_ReservoirBufferParameters reservoirBuffer;
    RTXDI_DIBufferIndices bufferIndices;
    RTXDI_DIInitialSamplingParameters initialSampling;
    RTXDI_DITemporalResamplingParameters temporalResampling;
    RTXDI_BoilingFilterParameters boilingFilter;
    RTXDI_DISpatialResamplingParameters spatialResampling;
    RTXDI_ShadingParameters shading;
    // RTXDI_PackedDIReservoir[c_NumReSTIRDIReservoirBuffers * reservoirArrayPitch]
    vk::DeviceAddress reservoirs;
    // float2[neighborOffsetCount], RTXDI_NEIGHBOR_OFFSETS_BUFFER (LowDiscrepancy for GI; m8 I2 F16)
    vk::DeviceAddress neighborOffsets;
    // rtxdi::ReSTIRDI_ResamplingMode, tested as bits by the passes: bit 0 temporal, bit 1 spatial
    uint32_t resamplingMode;
    // RTXDI_PackedDIReservoir rows behind reservoirs; the bridge drops accesses past it
    uint32_t reservoirCount;
    // ReSTIR GI (spec/m4/g2_indirect_pass.md, g5_restir_gi.md): the indirect pass writes initial reservoirs, the GI
    // passes resample them
    RTXDI_ReservoirBufferParameters giReservoirBuffer;
    RTXDI_GIBufferIndices giBufferIndices;
    // RTXDI_PackedGIReservoir[kGiReservoirArrays * reservoirArrayPitch]: RTXDI's two arrays, then the initial samples
    vk::DeviceAddress giReservoirs;
    // RTXDI_PackedGIReservoir rows behind giReservoirs; the bridge drops accesses past it
    uint32_t giReservoirCount;
    // rtxdi::ReSTIRGI_ResamplingMode: TemporalAndSpatial (3) or None (0, the reference); bit 0 temporal, bit 1 spatial
    uint32_t giResamplingMode;
    RTXDI_GITemporalResamplingParameters giTemporalResampling;
    RTXDI_BoilingFilterParameters giBoilingFilter;
    RTXDI_GISpatialResamplingParameters giSpatialResampling;
    RTXDI_GIFinalShadingParameters giFinalShading;
    // bounces traced after X2 instead of the NRC cache term; 0: the cache term (spec §14 ground truth)
    uint32_t giReferenceBounces;
    // GiFlag bits
    uint32_t giFlags;
    // local light presampling (spec/m7/f1_light_presampling.md): Power_RIS tiles, the environment segment RTXDI's
    // initial sampling takes (no environment light: never read), ReGIR's cells
    RTXDI_RISBufferSegmentParameters localLightRisBuffer;
    RTXDI_RISBufferSegmentParameters environmentLightRisBuffer;
    ReGIR_Parameters regir;
    // uint2[risEntryCount], RTXDI_RIS_BUFFER: light index | compact bit, inverse source pdf
    vk::DeviceAddress risBuffer;
    // uint4[2 * risEntryCount]: the compact light of each RIS entry (RAB_StoreCompactLightInfo)
    vk::DeviceAddress risLightData;
    uint32_t risEntryCount;
    // resource heap: sampled view of the local light PDF, full mip chain; kNoneIndex without local lights
    uint32_t lightPdf;
    // mip 0, square
    vk::Extent2D lightPdfSize;
    // ReSTIR PT's DLSS-RR decorrelation, applied to ReSTIR GI's final shading (m8 O11)
    RTXDI_PTDecorrelationParameters giDecorrelation;
    // RTXDI_PTTemporalResamplingParameters::historyReductionStrength, with GiFlag::DuplicationHistoryReduction
    float giHistoryReductionStrength;
    // DiFlag bits
    uint32_t diFlags;
    // float2[neighborOffsetCount], RTXDI_DI_NEIGHBOR_OFFSETS_BUFFER (StratifiedLowDiscrepancy for DI; m8 I2 F16)
    vk::DeviceAddress diNeighborOffsets;
};
// GpuRtxdiConstants::giFlags; mirrored in shaders/lighting/rtxdi_constants.slang
namespace GiFlag
{
    inline constexpr uint32_t SecondaryResampling = 1u << 0;
    inline constexpr uint32_t FireflyClamp = 1u << 1;
    inline constexpr uint32_t SecondaryRoughnessFloor = 1u << 2;
    // a neighbor's sample won the spatial reuse: its age restarts, so stagnancy measures temporal reuse only (RTXDI
    // PT SpatialResampling.hlsl without age-based rejection)
    inline constexpr uint32_t SpatialAgeReset = 1u << 3;
    // the temporal pass caps history where last frame's final sample was shared by many pixels; the shade pass writes
    // the sample ids and rtxdi_gi_duplication counts them (ReSTIR PT's duplication-based history reduction)
    inline constexpr uint32_t DuplicationHistoryReduction = 1u << 4;
    // the firefly replacement test averages its 8x8 tile weight over all in-bounds lanes instead of nonzero lanes only;
    // the temporal boiling filter keeps NVIDIA's (m8 I2 F15)
    inline constexpr uint32_t FullTileBoilingFilter = 1u << 5;
    // path vertices shade directional lights outside RIS, one shadow ray each (m8 I16 phase 2c)
    inline constexpr uint32_t ExactInfiniteLights = 1u << 6;
} // namespace GiFlag
// GpuRtxdiConstants::diFlags; mirrored in shaders/lighting/rtxdi_constants.slang
namespace DiFlag
{
    // Settings::areInfiniteLightsShadedExactly
    inline constexpr uint32_t ExactInfiniteLights = 1u << 0;
    // Settings::areLightTilesPerPixel
    inline constexpr uint32_t PerPixelLightTiles = 1u << 1;
} // namespace DiFlag
static_assert(sizeof(ReGIR_Parameters) == 1296);
static_assert(sizeof(RTXDI_PTDecorrelationParameters) == 32);
static_assert(sizeof(GpuRtxdiConstants) == 1904);
static_assert(offsetof(GpuRtxdiConstants, giDecorrelation) == 1856);
static_assert(offsetof(GpuRtxdiConstants, diNeighborOffsets) == 1896);
static_assert(offsetof(GpuRtxdiConstants, reservoirs) == 304);
static_assert(offsetof(GpuRtxdiConstants, giReservoirBuffer) == 328);
static_assert(offsetof(GpuRtxdiConstants, giReservoirs) == 376);
static_assert(offsetof(GpuRtxdiConstants, giTemporalResampling) == 392);
static_assert(offsetof(GpuRtxdiConstants, giFinalShading) == 472);
static_assert(offsetof(GpuRtxdiConstants, regir) == 528);
static_assert(offsetof(GpuRtxdiConstants, risBuffer) == 1824);
static_assert(offsetof(GpuRtxdiConstants, lightPdfSize) == 1848);

// RTXDI host side of ReSTIR DI (PBR plan F1), ReSTIR GI (M4 G2, G5) and local light presampling (M7 F1): an
// ImportanceSamplingContext at render resolution, as the RTXDI sample's SceneRenderer, with the reservoir buffers, the
// neighbor offsets, the RIS buffers its segment allocator sizes, the local light PDF and one constants block per frame
// slot. Lights come from WorldLights in RTXDI's region order
class RtxdiContext
{
public:
    // ReSTIRDI_LocalLightSamplingMode values index Settings::localLightSamples
    static constexpr size_t kLocalLightSamplingModeCount = 3;

    // GI reservoir arrays: RTXDI's two, then the indirect pass's initial samples, which the temporal pass would
    // overwrite in place and the final MIS still reads (spec/m4/g5_restir_gi.md §1)
    static constexpr uint32_t kGiReservoirArrays = 3;
    static constexpr uint32_t kGiInitialReservoirArray = 2;
    // the ground truth's bounce count after X2 (engine spec §14); 0.5^8 of the energy is left on Sponza's albedo
    static constexpr uint32_t kGiGroundTruthBounces = 8;
    // GiSettings::maxReservoirAge that never discards: the packed age saturates at 255 and the temporal pass scales
    // the limit by [0.5, 1)
    static constexpr uint32_t kGiUnlimitedSampleAge = 1024;

    // runtime ReSTIR DI parameters, applied by the next prepareFrame. The reference mode overrides the candidate
    // counts and turns reuse off
    struct Settings
    {
        // Uniform, Power_RIS (tiles presampled by power) or ReGIR_RIS (tiles per world-space cell); the primary
        // surface and every path vertex sample local lights the same way
        ReSTIRDI_LocalLightSamplingMode localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Uniform;
        // candidates per mode, as the RTXDI sample keeps them; only the active mode's count is used
        std::array<uint32_t, kLocalLightSamplingModeCount> localLightSamples{};
        uint32_t infiniteLightSamples = 0;
        // one ray each; a hit on an emissive triangle names its light
        uint32_t brdfSamples = 0;
        // one shadow ray per pixel in the initial pass; occluded candidates are dropped before reuse
        bool isInitialVisibilityEnabled = false;
        // directional (delta) lights leave the RIS candidates and are shaded exactly in the shade pass, one shadow ray
        // each: reuse made the sun stochastic (m8 I16 F28). Path vertices follow GiSettings::areInfiniteLightsShadedExactly
        bool areInfiniteLightsShadedExactly = false;
        // every pixel picks its own RIS tile / ReGIR cell jitter instead of sharing its 16x16 screen tile's
        // (RTXDI_TILE_SIZE_IN_PIXELS); also at path vertices and behind glass
        bool areLightTilesPerPixel = false;

        uint32_t maxHistoryLength = 0;
        ReSTIRDI_TemporalBiasCorrectionMode temporalBiasCorrection = ReSTIRDI_TemporalBiasCorrectionMode::Basic;
        bool isVisibilityShortcutEnabled = false;
        // a failed surface match tries one zero-motion sample without the similarity tests (RTXDI 3.2)
        bool isFallbackSamplingEnabled = false;
        // history from a permuted pixel: decorrelates frames for a denoiser, raises frame-to-frame change
        bool isPermutationSamplingEnabled = false;

        // even: the stratified neighbor walk covers quadrants in pairs
        uint32_t spatialSamples = 0;
        // spatial samples while the reservoir's history is shorter than disocclusionHistoryLength; 0 never boosts
        uint32_t disocclusionBoostSamples = 0;
        uint32_t disocclusionHistoryLength = 0;
        float samplingRadius = 0.0f;
        ReSTIRDI_SpatialBiasCorrectionMode spatialBiasCorrection = ReSTIRDI_SpatialBiasCorrectionMode::Basic;
        bool isMaterialSimilarityTestEnabled = false;

        // shared by the temporal and spatial surface tests
        float depthThreshold = 0.0f;
        float normalThreshold = 0.0f;
        // after temporal resampling
        bool isBoilingFilterEnabled = false;
        float boilingFilterStrength = 0.0f;
        // the shade pass skips its ray for a sample tested at most finalVisibilityMaxAge frames ago, within
        // finalVisibilityMaxDistance px (RTXDI's 4 / 16)
        bool isFinalVisibilityReused = false;

        [[nodiscard]] uint32_t& activeLocalLightSamples()
        {
            return localLightSamples[std::to_underlying(localLightSamplingMode)];
        }
        [[nodiscard]] uint32_t activeLocalLightSamples() const
        {
            return localLightSamples[std::to_underlying(localLightSamplingMode)];
        }

        bool operator==(const Settings&) const = default;
    };
    // runtime ReSTIR GI parameters, applied by the next prepareFrame; no reuse (isGiReference, NRC mode) overrides the
    // resampling mode
    struct GiSettings
    {
        uint32_t maxHistoryLength = 0;
        // samples older than this are not reused; the temporal pass scales it per pixel by [0.5, 1)
        uint32_t maxReservoirAge = 0;
        RTXDI_GIBiasCorrectionMode temporalBiasCorrection = RTXDI_GIBiasCorrectionMode::Basic;
        bool isFallbackSamplingEnabled = false;
        bool isPermutationSamplingEnabled = false;

        uint32_t spatialSamples = 0;
        float samplingRadius = 0.0f;
        RTXDI_GIBiasCorrectionMode spatialBiasCorrection = RTXDI_GIBiasCorrectionMode::Basic;

        // shared by the temporal and spatial surface tests
        float depthThreshold = 0.0f;
        float normalThreshold = 0.0f;
        bool isBoilingFilterEnabled = false;
        float boilingFilterStrength = 0.0f;
        // firefly replacement only: average the 8x8 tile weight over all in-bounds lanes, not nonzero lanes (m8 I2 F15)
        bool isFullTileBoilingFilterEnabled = false;
        // a reconnection ray from the primary to the final sample
        bool isFinalVisibilityEnabled = false;
        // the final sample and the pixel's initial sample, weighted by how glossy their BRDF lobes are
        bool isFinalMisEnabled = false;
        // X2's light resampled from this frame's DI reservoirs where X2 is on screen (RTXDI sample
        // enableSecondaryResampling; spec/features/m8_optimization_and_fixes.md O2)
        bool isSecondaryResamplingEnabled = false;
        // X2's radiance clamped in display units (RTXDI sample c_MaxIndirectRadiance; O6); biased
        bool isFireflyClampEnabled = false;
        // X2 shaded with roughness at least 0.5 while GI reuse runs (RTXDI sample minSecondaryRoughness; O9): keeps
        // view-dependent glossy outliers out of the non-directional reservoirs; biased on glossy X2
        bool isSecondaryRoughnessFloorEnabled = false;
        // path vertices (X2, NRC training, glass) shade directional lights exactly instead of as RIS candidates (m8
        // I16 phase 2c). Secondary resampling overrides it with Settings::areInfiniteLightsShadedExactly: X2's
        // candidates and the reused DI reservoirs must hold the same lights (C-I16-4)
        bool areInfiniteLightsShadedExactly = false;

        // DLSS-RR prefers temporally independent samples (RTXDI 3.1, ReSTIR PT's DLSS-RR profile; O11): final shading
        // shades the pixel's initial sample instead of the reused one with probability decorrelationFactor. Stagnancy
        // mode scales it by 4 · stagnancy^exponent, stagnancy = min(age / 40, 1) blended into last frame's 5x5
        // average by emaFactor. The stored reservoirs keep their history
        RTXDI_PTDecorrelationMode decorrelationMode = RTXDI_PTDecorrelationMode::None;
        float decorrelationFactor = 0.0f;
        float decorrelationStagnancyExponent = 0.0f;
        float decorrelationEmaFactor = 0.0f;
        // a final reservoir far brighter than its 8x8 group is always swapped: the boiling filter's test, replacing
        // the sample instead of discarding it
        bool isFireflyReplacementEnabled = false;
        float fireflyReplacementStrength = 0.0f;
        // a swap where the final sample is fresh keeps at most fireflyReplacementMultiplyBound times its weight
        bool isFireflyReplacementBounded = false;
        float fireflyReplacementMultiplyBound = 0.0f;
        // sample impoverishment: where many of the 17x17 neighbors shared last frame's final sample, temporal reuse
        // keeps fewer frames of history, down to 1; strength shapes how fast the cap falls with the shared share
        bool isDuplicationHistoryReductionEnabled = false;
        float historyReductionStrength = 0.0f;

        bool operator==(const GiSettings&) const = default;
    };

    // the RTXDI sample's QualityPreset, without (Custom)
    enum class QualityPreset : uint8_t
    {
        Fast,
        Medium,
        Unbiased,
        Ultra,
        // pairs with isReference: resampling None
        Reference,
        Count,
    };

    // UIData::ApplyPreset over the sample's start state, the Medium preset (spec/features/planned/
    // nvidia_settings_layout.md §3); fields the engine lacks are skipped
    [[nodiscard]] static Settings nvidiaPreset(QualityPreset preset);
    // the RTXDI sample's default Medium preset (spec/features/done/rtxdi_3_2_update.md §3.3)
    [[nodiscard]] static Settings nvidiaSettings();
    // shipped since 2026-11-25: NVIDIA's Medium preset, so the engine starts as the sample does
    [[nodiscard]] static Settings defaultSettings();
    // the m8 I6-I16 tuning for DLSS-RR in NRC + ReSTIR GI mode: Power_RIS, exact sun, permutation, 2 spatial samples
    // (spec/fixesM1-6/i6_restir_di_tuning.md, i16_denoiser_friendly_noise.md)
    [[nodiscard]] static Settings tunedSettings();
    // the RTXDI sample's ReSTIR GI settings: the library defaults, temporal and spatial (spec/m4/g5_restir_gi.md §1)
    [[nodiscard]] static GiSettings nvidiaGiSettings();
    // shipped since 2026-11-25: NVIDIA's (the sample's DLSS-RR preset leaves ReSTIR GI alone)
    [[nodiscard]] static GiSettings defaultGiSettings();
    // NVIDIA's with ray-traced spatial bias correction (spec/fixesM1-6/m8_optimization_and_fixes.md O1) and ReSTIR PT's
    // DLSS-RR profile (O11); what the GI check gates
    [[nodiscard]] static GiSettings tunedGiSettings();
    // the GI check's ReSTIR GI configuration (spec/m4/g5_restir_gi.md §3): bias correction with rays; no sample age
    // limit, fallback, permutation or boiling filter
    [[nodiscard]] static GiSettings convergedGiSettings();
    // the configuration the guide capture's convergence check holds to its bounds (§6.1): reuse that stays unbiased
    // on a static camera, no neighbors past the disocclusion boost
    [[nodiscard]] static Settings convergedSettings();
    // M8.3 demo-analog & Engine Target DI settings presets (spec/m8/m8_3_pipeline_toggles.md §6.1)
    [[nodiscard]] static Settings targetRisSettings();
    [[nodiscard]] static Settings targetTemporalSettings();
    [[nodiscard]] static Settings analogUniformRisSettings();
    // UIData::ApplyDenoiserCompatDefaults: the GI fields that depend on the denoiser. Off takes ReSTIR PT's non-DLSS-RR
    // profile, DLSS-RR the shipped values, which are the engine's DLSS-RR profile (I5, I15)
    static void applyDenoiserCompatDefaults(GiSettings& settings, bool isDlssRr);

    // RTXDI parameter blocks as the last prepareFrame applied them, for the settings window's fixed rows
    struct AppliedParameters
    {
        rtxdi::ReGIRStaticParameters regirStatic;
        rtxdi::ReGIRDynamicParameters regirDynamic;
        uint32_t regirLightSlotCount;
        RTXDI_DIInitialSamplingParameters initialSampling;
        RTXDI_DITemporalResamplingParameters temporalResampling;
        RTXDI_DISpatialResamplingParameters spatialResampling;
        RTXDI_ShadingParameters shading;
    };
    [[nodiscard]] AppliedParameters getAppliedParameters() const;

    RtxdiContext(const Device& device, const VkAllocator& allocator, const FrameSync& frameSync,
                 DescriptorManager& descriptorManager, const ShaderObjects& shaderObjects,
                 const WorldLights& worldLights);
    ~RtxdiContext();

    RtxdiContext(const RtxdiContext&) = delete;
    RtxdiContext& operator=(const RtxdiContext&) = delete;
    RtxdiContext(RtxdiContext&&) = delete;
    RtxdiContext& operator=(RtxdiContext&&) = delete;

    // ENGINE_RTXDI_REFERENCE=1: start in the reference configuration
    [[nodiscard]] static bool isReferenceRequested();

    // device idle: contexts and reservoirs at the new render resolution; reservoir contents are undefined, so the
    // first frame after it must reset history (resize arrives as a camera cut)
    void recreate(vk::Extent2D renderExtent);
    // the slot is not in flight, after WorldLights::prepareFrame: advances the context to frameIndex, applies
    // directMode/directResampling, lazily allocates/retires DI and GI reservoirs, centers ReGIR on cameraPosition,
    // grows the light PDF to the local light count and writes the slot's constants. isGiActive: true only when
    // IndirectBounceMode::RestirGi runs (RawPt and Off retire GI reservoirs). lastSubmittedFrame: the frame number
    // submitted last, whose ray passes read the PDF; growing waits for them
    void prepareFrame(uint32_t frameSlot, uint32_t frameIndex, bool isGiActive, const glm::vec3& cameraPosition,
                      uint64_t lastSubmittedFrame);
    // async compute, heaps bound, the slot's frame root written: light_world (world rows, PDF mip 0), then the PDF
    // mips and the presampling passes the applied local light sampling mode reads. The ray passes see every write
    // through the compute timeline
    // rolling 120-frame averages of the async-compute light preparation passes (C-F1-5)
    struct LightPrepStats
    {
        float worldLightsMs = 0.0f;
        float pdfMipsMs = 0.0f;
        float presampleMs = 0.0f;
        float totalMs = 0.0f;
        // raw GPU clock ticks of the most recently collected compute submit (F9 overlap)
        uint64_t lastStartTick = 0;
        uint64_t lastEndTick = 0;
    };

    void recordLightPreparation(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot, vk::DeviceAddress frameRoot);
    // called when no lighting consumer runs this frame: avoids collecting stale query-pool timestamps and records 0 ms
    void skipLightPreparation(uint32_t frameSlot) noexcept;
    [[nodiscard]] LightPrepStats getLightPrepStats() const noexcept;
    [[nodiscard]] DirectMode getAppliedDirectMode() const noexcept { return appliedDirectMode; }
    [[nodiscard]] DirectResamplingMode getAppliedDirectResampling() const noexcept { return appliedDirectResampling; }
    [[nodiscard]] bool isDirectReference() const noexcept
    {
        return directMode == DirectMode::RestirDi && directResampling == DirectResamplingMode::Reference;
    }
    [[nodiscard]] bool isDirectReferenceApplied() const noexcept
    {
        return appliedDirectMode == DirectMode::RestirDi &&
            appliedDirectResampling == DirectResamplingMode::Reference;
    }
    // true while settings has edits not yet applied by prepareFrame (M8.2 BUG-UI-6)
    [[nodiscard]] bool isSettingsDirty() const noexcept { return settings != appliedSettings; }
    // ReSTIR DI temporal resampling pass runs in RestirDi Temporal and TemporalAndSpatial modes
    [[nodiscard]] bool isTemporalReuseEnabled() const noexcept
    {
        return appliedDirectMode == DirectMode::RestirDi &&
            (appliedDirectResampling == DirectResamplingMode::Temporal ||
             appliedDirectResampling == DirectResamplingMode::TemporalAndSpatial);
    }
    // primary DI shades directional lights outside RIS; off in reference mode so kReferenceInfiniteLightSamples runs
    [[nodiscard]] bool areDiInfiniteLightsExact() const noexcept
    {
        return !isDirectReference() && settings.areInfiniteLightsShadedExactly;
    }
    // secondary DI resampling (O2) reads DI reservoirs and is forced off when DI reservoirs are absent
    [[nodiscard]] bool isSecondaryResamplingEffective() const noexcept
    {
        return directMode == DirectMode::RestirDi && giSettings.isSecondaryResamplingEnabled;
    }
    // path vertices shade directional lights outside RIS; secondary resampling follows primary DI (C-I16-4)
    [[nodiscard]] bool arePathInfiniteLightsExact() const noexcept
    {
        return isSecondaryResamplingEffective() ? areDiInfiniteLightsExact() : giSettings.areInfiniteLightsShadedExactly;
    }
    // ReSTIR GI temporal and spatial reuse as applied by the last prepareFrame
    [[nodiscard]] bool isGiReuseEnabled() const noexcept { return isGiReuseApplied; }
    // rtxdi_gi_shade reads and writes giStagnancy only while ReSTIR GI reuse runs in Stagnancy decorrelation mode
    [[nodiscard]] bool isGiStagnancyEnabled() const noexcept
    {
        return isGiReuseApplied && giSettings.decorrelationMode == RTXDI_PTDecorrelationMode::Stagnancy;
    }
    // rtxdi_gi_duplication runs after the GI shade pass, as written by the last prepareFrame
    [[nodiscard]] bool isGiDuplicationMapEnabled() const noexcept { return isGiDuplicationMapApplied; }
    [[nodiscard]] bool hasDiReservoirs() const noexcept { return reservoirs != nullptr; }
    [[nodiscard]] bool hasGiReservoirs() const noexcept { return giReservoirs != nullptr; }

    // GpuRtxdiConstants of the slot
    [[nodiscard]] vk::DeviceAddress constantsAddress(uint32_t frameSlot) const noexcept
    {
        return constants[frameSlot].address;
    }

    // Stage 10 direct lighting mode and ReSTIR DI resampling mode (spec/m8/m8_3_pipeline_toggles.md §2); read by the
    // next prepareFrame
    DirectMode directMode = kDefaultDirectMode;
    DirectResamplingMode directResampling = kDefaultDirectResamplingMode;
    Settings settings = defaultSettings();
    // ReSTIR GI without reuse: the pixel's initial sample is shaded alone, the unbiased 1-sample estimate
    bool isGiReference = false;
    GiSettings giSettings = defaultGiSettings();
    // > 0: the indirect pass traces this many bounces after X2 instead of querying NRC, which the renderer pauses
    uint32_t giReferenceBounces = 0;
    // deterministic per-phase RNG seed override for self-test runners (the GI check); nullopt uses
    // Renderer::frameNumber
    std::optional<uint32_t> frameIndexOverride;

private:
    static constexpr uint32_t kReferenceLocalLightSamples = 32;
    static constexpr uint32_t kReferenceInfiniteLightSamples = 8;
    static constexpr size_t kLightPrepHistorySize = 120;
    // start, after light_world, after light_pdf_mips, after presample (Power_RIS / ReGIR)
    static constexpr uint32_t kLightPrepQueriesPerFrame = 4;

    void collectLightPrepResults(uint32_t frameSlot);

    // the ReSTIR DI and GI contexts importanceSampling owns
    [[nodiscard]] rtxdi::ReSTIRDIContext& restirDi() const;
    [[nodiscard]] rtxdi::ReSTIRGIContext& restirGi() const;
    // resampling mode of directMode/directResampling and every parameter block of settings
    void configure();
    // resampling mode and every parameter block of giSettings
    void configureGi(bool isGiReuse);
    void destroyReservoirs();
    void ensureDiReservoirs();
    void releaseDiReservoirs();
    void ensureGiReservoirs();
    void releaseGiReservoirs();
    // the segment allocator's total, once: it does not depend on the render size
    void createRisBuffers();
    // the square power of two rtxdi::ComputePdfTextureSize gives for localLightCount, full mip chain, a storage view
    // per mip; no frame may still read the old one
    void createLightPdf(uint32_t localLightCount);
    void destroyLightPdf();

    const Device& device;
    const VkAllocator& allocator;
    const FrameSync& frameSync;
    DescriptorManager& descriptorManager;
    const ShaderObjects& shaderObjects;
    const WorldLights& worldLights;
    std::unique_ptr<rtxdi::ImportanceSamplingContext> importanceSampling;
    DirectMode appliedDirectMode = kDefaultDirectMode;
    DirectResamplingMode appliedDirectResampling = kDefaultDirectResamplingMode;
    Settings appliedSettings;
    bool isGiReuseApplied = false;
    bool isGiDuplicationMapApplied = false;
    GiSettings appliedGiSettings;
    vk::raii::Buffer reservoirs = nullptr;
    VmaAllocation reservoirMemory = nullptr;
    vk::DeviceAddress reservoirAddress = 0;
    uint32_t reservoirCount = 0;
    uint32_t lastDiReservoirsFrame = 0;
    vk::raii::Buffer giReservoirs = nullptr;
    VmaAllocation giReservoirMemory = nullptr;
    vk::DeviceAddress giReservoirAddress = 0;
    uint32_t giReservoirCount = 0;
    uint32_t lastGiReservoirsFrame = 0;
    HostBuffer neighborOffsets;
    HostBuffer diNeighborOffsets;
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> constants;

    // one copy: the compute submit that rewrites them waits for the previous frame's ray passes (C-F2-1)
    vk::raii::Buffer risBuffer = nullptr;
    VmaAllocation risMemory = nullptr;
    vk::DeviceAddress risAddress = 0;
    vk::raii::Buffer risLightData = nullptr;
    VmaAllocation risLightDataMemory = nullptr;
    vk::DeviceAddress risLightDataAddress = 0;
    uint32_t risEntryCount = 0;
    // ReGIR's cells as the shaders read them; only the center changes per frame
    ReGIR_Parameters regirConstants{};

    // compute-queue timestamps for recordLightPreparation (C-F1-5)
    vk::raii::QueryPool lightPrepTimestamps = nullptr;
    float lightPrepTimestampPeriodNs = 0.0f;
    std::array<bool, MAX_FRAMES_IN_FLIGHT> isLightPrepTimed{};
    std::array<std::array<float, kLightPrepHistorySize>, 3> lightPrepHistories{};
    uint64_t lastLightPrepStartTick = 0;
    uint64_t lastLightPrepEndTick = 0;
    size_t lightPrepHistoryHead = 0;
    size_t lightPrepHistoryCount = 0;

    // R32_SFLOAT, eGeneral, concurrent between the queue families: written on async compute, read by presampling and
    // by the ray passes (RAB_EvaluateLocalLightSourcePdf). One copy, like the RIS buffers
    vk::raii::Image lightPdf = nullptr;
    VmaAllocation lightPdfMemory = nullptr;
    uint32_t lightPdfSide = 0;
    // resource heap: the full chain, sampled
    uint32_t lightPdfIndex = kNoneIndex;
    // resource heap: a storage view per mip
    std::vector<uint32_t> lightPdfMipIndices;
    // created since the last recordLightPreparation, still in eUndefined
    bool isLightPdfLayoutPending = false;
};
