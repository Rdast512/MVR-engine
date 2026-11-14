#pragma once
#include "../Constants.h"
#include "core/vk_allocator.hpp"
#include "vk_world_lights.hpp"
#include "core/vk_device.hpp"
#include "util/vk_utils.hpp"

#include <Rtxdi/DI/ReSTIRDIParameters.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vulkan/vulkan_raii.hpp>

namespace rtxdi
{
    class ReSTIRDIContext;
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
    RTXDI_DISpatioTemporalResamplingParameters spatioTemporalResampling;
    RTXDI_ShadingParameters shading;
    // RTXDI_PackedDIReservoir[c_NumReSTIRDIReservoirBuffers * reservoirArrayPitch]
    vk::DeviceAddress reservoirs;
    // float2[neighborOffsetCount], RTXDI_NEIGHBOR_OFFSETS_BUFFER
    vk::DeviceAddress neighborOffsets;
    // 1: FusedSpatiotemporal; 0: the reference (None), rtxdi_shade shades the initial sample
    uint32_t isFusedResampling;
    // RTXDI_PackedDIReservoir rows behind reservoirs; the bridge drops accesses past it
    uint32_t reservoirCount;
};
static_assert(sizeof(GpuRtxdiConstants) == 360);
static_assert(offsetof(GpuRtxdiConstants, reservoirs) == 336);

// RTXDI host side of ReSTIR DI (PBR plan F1): the ReSTIRDIContext at render resolution, the DI reservoir buffer, the
// neighbor offsets and one constants block per frame slot. Lights come from WorldLights in RTXDI's region order
class RtxdiContext
{
public:
    // runtime ReSTIR DI parameters, applied by the next prepareFrame. The fused pass reads the initial sampling,
    // spatiotemporal and boiling filter blocks; the reference mode overrides the candidate counts
    struct Settings
    {
        uint32_t localLightSamples = 0;
        uint32_t infiniteLightSamples = 0;
        // one shadow ray per pixel in the initial pass; occluded candidates are dropped before reuse
        bool isInitialVisibilityEnabled = false;
        uint32_t spatialSamples = 0;
        // spatial samples while the reservoir's history is shorter than maxHistoryLength
        uint32_t disocclusionBoostSamples = 0;
        float samplingRadius = 0.0f;
        uint32_t maxHistoryLength = 0;
        float depthThreshold = 0.0f;
        float normalThreshold = 0.0f;
        ReSTIRDI_SpatioTemporalBiasCorrectionMode biasCorrection = ReSTIRDI_SpatioTemporalBiasCorrectionMode::Pairwise;
        bool isVisibilityShortcutEnabled = false;
        bool isMaterialSimilarityTestEnabled = false;
        bool isBoilingFilterEnabled = false;
        float boilingFilterStrength = 0.0f;

        bool operator==(const Settings&) const = default;
    };
    // RTXDI's defaults with the engine's choices (plan F3): Pairwise MIS
    [[nodiscard]] static Settings defaultSettings();

    RtxdiContext(const Device& device, const VkAllocator& allocator, const WorldLights& worldLights);
    ~RtxdiContext();

    RtxdiContext(const RtxdiContext&) = delete;
    RtxdiContext& operator=(const RtxdiContext&) = delete;
    RtxdiContext(RtxdiContext&&) = delete;
    RtxdiContext& operator=(RtxdiContext&&) = delete;

    // ENGINE_RTXDI_REFERENCE=1: start in the reference configuration
    [[nodiscard]] static bool isReferenceRequested();

    // device idle: context and reservoirs at the new render resolution; reservoir contents are undefined, so the
    // first frame after it must reset history (resize arrives as a camera cut)
    void recreate(vk::Extent2D renderExtent);
    // the slot is not in flight: applies isReference, advances the context to frameIndex and writes the slot's
    // constants
    void prepareFrame(uint32_t frameSlot, uint32_t frameIndex);

    // GpuRtxdiConstants of the slot
    [[nodiscard]] vk::DeviceAddress constantsAddress(uint32_t frameSlot) const noexcept
    {
        return constants[frameSlot].address;
    }

    // plan F3 ground truth: resampling None with kReference* initial candidates instead of fused spatiotemporal
    // reuse with the library's counts; read by the next prepareFrame
    bool isReference = false;
    Settings settings = defaultSettings();

private:
    static constexpr uint32_t kReferenceLocalLightSamples = 32;
    static constexpr uint32_t kReferenceInfiniteLightSamples = 8;

    // resampling mode of isReference and every parameter block of settings
    void configure();
    void destroyReservoirs();

    const Device& device;
    const VkAllocator& allocator;
    const WorldLights& worldLights;
    std::unique_ptr<rtxdi::ReSTIRDIContext> context;
    bool isReferenceApplied = false;
    Settings appliedSettings;
    vk::raii::Buffer reservoirs = nullptr;
    VmaAllocation reservoirMemory = nullptr;
    vk::DeviceAddress reservoirAddress = 0;
    uint32_t reservoirCount = 0;
    HostBuffer neighborOffsets;
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> constants;
};
