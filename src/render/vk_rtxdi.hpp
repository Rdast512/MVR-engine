#pragma once
#include "../Constants.h"
#include "core/vk_allocator.hpp"
#include "core/vk_descriptors.hpp"
#include "core/vk_device.hpp"
#include "core/vk_frame_sync.hpp"
#include "util/vk_utils.hpp"
#include "vk_shader_objects.hpp"
#include "vk_world_lights.hpp"

#include <Rtxdi/DI/ReSTIRDIParameters.h>
#include <Rtxdi/GI/ReSTIRGIParameters.h>
#include <Rtxdi/LightSampling/RISBufferSegmentParameters.h>
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
    // float2[neighborOffsetCount], RTXDI_NEIGHBOR_OFFSETS_BUFFER
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
};
// GpuRtxdiConstants::giFlags; mirrored in shaders/lighting/rtxdi_constants.slang
namespace GiFlag
{
    inline constexpr uint32_t SecondaryResampling = 1u << 0;
    inline constexpr uint32_t FireflyClamp = 1u << 1;
} // namespace GiFlag
static_assert(sizeof(ReGIR_Parameters) == 1296);
static_assert(sizeof(GpuRtxdiConstants) == 1856);
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
        // a reconnection ray from the primary to the final sample
        bool isFinalVisibilityEnabled = false;
        // the final sample and the pixel's initial sample, weighted by how glossy their BRDF lobes are
        bool isFinalMisEnabled = false;
        // X2's light resampled from this frame's DI reservoirs where X2 is on screen (RTXDI sample
        // enableSecondaryResampling; spec/features/m8_optimization_and_fixes.md O2)
        bool isSecondaryResamplingEnabled = false;
        // X2's radiance clamped in display units (RTXDI sample c_MaxIndirectRadiance; O6); biased
        bool isFireflyClampEnabled = false;

        bool operator==(const GiSettings&) const = default;
    };

    // the RTXDI sample's default Medium preset (spec/features/rtxdi_3_2_update.md §3.3)
    [[nodiscard]] static Settings defaultSettings();
    // the RTXDI sample's ReSTIR GI settings: the library defaults, temporal and spatial (spec/m4/g5_restir_gi.md §1)
    [[nodiscard]] static GiSettings nvidiaGiSettings();
    // shipped: NVIDIA's with ray-traced spatial bias correction (spec/features/m8_optimization_and_fixes.md O1)
    [[nodiscard]] static GiSettings defaultGiSettings();
    // the GI check's ReSTIR GI configuration (spec/m4/g5_restir_gi.md §3): bias correction with rays; no sample age
    // limit, fallback, permutation or boiling filter
    [[nodiscard]] static GiSettings convergedGiSettings();
    // the configuration the guide capture's convergence check holds to its bounds (§6.1): reuse that stays unbiased
    // on a static camera, no neighbors past the disocclusion boost
    [[nodiscard]] static Settings convergedSettings();

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
    // isReference, centers ReGIR on cameraPosition, grows the light PDF to the local light count and writes the slot's
    // constants. isGiReuseAllowed: false in the NRC indirect mode, which shades the initial samples as isGiReference.
    // lastSubmittedFrame: the frame number submitted last, whose ray passes read the PDF; growing waits for them
    void prepareFrame(uint32_t frameSlot, uint32_t frameIndex, bool isGiReuseAllowed, const glm::vec3& cameraPosition,
                      uint64_t lastSubmittedFrame);
    // async compute, heaps bound, the slot's frame root written: light_world (world rows, PDF mip 0), then the PDF
    // mips and the presampling passes the applied local light sampling mode reads. The ray passes see every write
    // through the compute timeline
    void recordLightPreparation(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot, vk::DeviceAddress frameRoot);
    // ReSTIR DI reference mode as applied by the last prepareFrame
    [[nodiscard]] bool isReferenceEnabled() const noexcept { return isReferenceApplied; }
    // ReSTIR GI temporal and spatial reuse as applied by the last prepareFrame
    [[nodiscard]] bool isGiReuseEnabled() const noexcept { return isGiReuseApplied; }

    // GpuRtxdiConstants of the slot
    [[nodiscard]] vk::DeviceAddress constantsAddress(uint32_t frameSlot) const noexcept
    {
        return constants[frameSlot].address;
    }

    // plan F3 ground truth: resampling None with kReference* initial candidates instead of temporal and spatial
    // reuse with the library's counts; read by the next prepareFrame
    bool isReference = false;
    Settings settings = defaultSettings();
    // ReSTIR GI without reuse: the pixel's initial sample is shaded alone, the unbiased 1-sample estimate
    bool isGiReference = false;
    GiSettings giSettings = defaultGiSettings();
    // > 0: the indirect pass traces this many bounces after X2 instead of querying NRC, which the renderer pauses
    uint32_t giReferenceBounces = 0;
    // deterministic per-phase RNG seed override for self-test runners (EngineGiCheck); nullopt uses Renderer::frameNumber
    std::optional<uint32_t> frameIndexOverride;

private:
    static constexpr uint32_t kReferenceLocalLightSamples = 32;
    static constexpr uint32_t kReferenceInfiniteLightSamples = 8;

    // the ReSTIR DI and GI contexts importanceSampling owns
    [[nodiscard]] rtxdi::ReSTIRDIContext& restirDi() const;
    [[nodiscard]] rtxdi::ReSTIRGIContext& restirGi() const;
    // resampling mode of isReference and every parameter block of settings
    void configure();
    // resampling mode and every parameter block of giSettings
    void configureGi(bool isGiReuse);
    void destroyReservoirs();
    void createGiReservoirs();
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
    bool isReferenceApplied = false;
    Settings appliedSettings;
    bool isGiReuseApplied = true;
    GiSettings appliedGiSettings;
    vk::raii::Buffer reservoirs = nullptr;
    VmaAllocation reservoirMemory = nullptr;
    vk::DeviceAddress reservoirAddress = 0;
    uint32_t reservoirCount = 0;
    vk::raii::Buffer giReservoirs = nullptr;
    VmaAllocation giReservoirMemory = nullptr;
    vk::DeviceAddress giReservoirAddress = 0;
    uint32_t giReservoirCount = 0;
    HostBuffer neighborOffsets;
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
