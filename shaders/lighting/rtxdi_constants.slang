#ifndef ENGINE_LIGHTING_RTXDI_CONSTANTS_SLANG
#define ENGINE_LIGHTING_RTXDI_CONSTANTS_SLANG

// textual: RTXDI's parameter structs come from its C header, so this mirror cannot live in the types module.
// Matches GpuRtxdiConstants in src/render/vk_rtxdi.hpp; tests/layout_test.cpp checks it
#include "Rtxdi/DI/ReSTIRDIParameters.h"
#include "Rtxdi/GI/ReSTIRGIParameters.h"
#include "Rtxdi/LightSampling/RISBufferSegmentParameters.h"
#include "Rtxdi/PT/ReSTIRPTParameters.h"
#include "Rtxdi/ReGIR/ReGIRParameters.h"

// GpuFrameRoot.rtxdiConstants (PBR plan F1)
struct GpuRtxdiConstants
{
    RTXDI_RuntimeParameters runtime;
    RTXDI_LightBufferParameters lightBuffer;
    RTXDI_ReservoirBufferParameters reservoirBuffer;
    RTXDI_DIBufferIndices bufferIndices;
    RTXDI_DIInitialSamplingParameters initialSampling;
    RTXDI_DITemporalResamplingParameters temporalResampling;
    RTXDI_BoilingFilterParameters boilingFilter;
    RTXDI_DISpatialResamplingParameters spatialResampling;
    RTXDI_ShadingParameters shading;
    uint64_t reservoirs;      // RTXDI_PackedDIReservoir[c_NumReSTIRDIReservoirBuffers * reservoirArrayPitch]
    uint64_t neighborOffsets; // float2[neighborOffsetMask + 1]
    uint resamplingMode;      // rtxdi::ReSTIRDI_ResamplingMode: bit 0 temporal, bit 1 spatial; 0 is the reference
    uint reservoirCount;      // rows behind reservoirs
    // ReSTIR GI: initial reservoirs from passes/indirect.slang (M4 G2), resampled by passes/rtxdi_gi_*.slang (G5)
    RTXDI_ReservoirBufferParameters giReservoirBuffer;
    RTXDI_GIBufferIndices giBufferIndices;
    uint64_t giReservoirs;    // RTXDI_PackedGIReservoir[kGiReservoirArrays * reservoirArrayPitch]
    uint giReservoirCount;    // rows behind giReservoirs
    uint giResamplingMode;    // rtxdi::ReSTIRGI_ResamplingMode: bit 0 temporal, bit 1 spatial; 0 is the reference
    RTXDI_GITemporalResamplingParameters giTemporalResampling;
    RTXDI_BoilingFilterParameters giBoilingFilter;
    RTXDI_GISpatialResamplingParameters giSpatialResampling;
    RTXDI_GIFinalShadingParameters giFinalShading;
    uint giReferenceBounces;  // bounces traced after X2 instead of the NRC cache term; 0: the cache term
    uint giFlags;             // kGiFlag* bits
    // local light presampling (spec/m7/f1_light_presampling.md): passes/rtxdi_presample.slang fills the tiles and the
    // cells, RTXDI's initial sampling reads them. No environment light: its segment is never read
    RTXDI_RISBufferSegmentParameters localLightRisBuffer;
    RTXDI_RISBufferSegmentParameters environmentLightRisBuffer;
    ReGIR_Parameters regir;
    uint64_t risBuffer;       // uint2[risEntryCount], RTXDI_RIS_BUFFER
    uint64_t risLightData;    // uint4[2 * risEntryCount], the compact light of each entry
    uint risEntryCount;
    uint lightPdf;            // resource heap: sampled, full mip chain; kNoneIndex without local lights
    uint2 lightPdfSize;       // mip 0, square
    // ReSTIR PT's DLSS-RR decorrelation, applied to ReSTIR GI by passes/rtxdi_gi_shade.slang (m8 O11)
    RTXDI_PTDecorrelationParameters giDecorrelation;
    float giHistoryReductionStrength; // ReSTIR PT's historyReductionStrength, with kGiFlagDuplicationHistoryReduction
    uint diFlags;             // kDiFlag* bits
    uint64_t diNeighborOffsets; // float2[neighborOffsetMask + 1], StratifiedLowDiscrepancy for DI (m8 I2 F16)
};

// GiFlag in src/render/vk_rtxdi.hpp
static const uint kGiFlagSecondaryResampling = 1u << 0;
static const uint kGiFlagFireflyClamp = 1u << 1;
static const uint kGiFlagSecondaryRoughnessFloor = 1u << 2;
static const uint kGiFlagSpatialAgeReset = 1u << 3;
static const uint kGiFlagDuplicationHistoryReduction = 1u << 4;
static const uint kGiFlagFullTileBoilingFilter = 1u << 5;
static const uint kGiFlagExactInfiniteLights = 1u << 6;

// DiFlag in src/render/vk_rtxdi.hpp
static const uint kDiFlagExactInfiniteLights = 1u << 0;
static const uint kDiFlagPerPixelLightTiles = 1u << 1;

// RIS picks nothing when the only lights are directional ones shaded exactly (m8 I16 F28): Sponza's sun.
// Mirrors Renderer::recordDirectLight's host gate
bool hasRisLightCandidates(GpuRtxdiConstants constants, bool areInfiniteLightsExact)
{
    return constants.lightBuffer.localLightBufferRegion.numLights > 0 ||
           constants.lightBuffer.environmentLightParams.lightPresent != 0 ||
           (!areInfiniteLightsExact && constants.lightBuffer.infiniteLightBufferRegion.numLights > 0);
}

// RtxdiContext::kGiInitialReservoirArray: the indirect pass's initial samples, after RTXDI's two arrays
static const uint kGiInitialReservoirArray = 2;

#endif
