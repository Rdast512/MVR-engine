#ifndef ENGINE_LIGHTING_RTXDI_CONSTANTS_SLANG
#define ENGINE_LIGHTING_RTXDI_CONSTANTS_SLANG

// textual: RTXDI's parameter structs come from its C header, so this mirror cannot live in the types module.
// Matches GpuRtxdiConstants in src/render/vk_rtxdi.hpp; tests/layout_test.cpp checks it
#include "Rtxdi/DI/ReSTIRDIParameters.h"

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
};

#endif
