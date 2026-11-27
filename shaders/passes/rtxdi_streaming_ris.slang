import types;
import core.binding;
import core.frame;
import lighting.glass;

static const uint kRtxdiTile = 8; // matches src/render/vk_renderer.cpp

#include "lighting/rtxdi_bridge.slang"
#include "lighting/rtxdi_shading.slang"
#include "Rtxdi/DI/InitialSampling.hlsli"
#include "Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli"

// Fused single-pass Streaming RIS direct lighting (spec/m8/m8_3_pipeline_toggles.md §3.2, M8.1 PERF-C1, modeled on
// reference/RTXPT-main/Rtxpt/Shaders/IntroSample/RTXDIStreamingRISLightingPass.hlsl): draws candidates with the user's
// RtxdiContext::Settings (decoupled from Reference overrides), evaluates the winning sample directly in registers with
// one final visibility ray and glass transmittance, adds exact directional lights when enabled, and writes
// directRadiance with zero DI reservoir traffic.

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

[shader("compute")]
[numthreads(kRtxdiTile, kRtxdiTile, 1)]
void rtxdiStreamingRisMain(uint3 id : SV_DispatchThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    rtxdiBridgeInit(root);
    const uint2 pixel = id.xy;
    // RenderTargets::kDirectRadianceFormat
    const uint output = bdaPtr<GpuTargets>(root.targets)[0].directRadianceStorage;
    if (gRtxdiBridge.lightCount == 0)
    {
        if (all(pixel < gRtxdiBridge.renderSize))
        {
            storageTex2D<float4, kImageFormatRgba16f>(output)[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        }
        return;
    }
    if (any(pixel >= gRtxdiBridge.renderSize))
    {
        return;
    }
    const GpuRtxdiConstants constants = loadRtxdiConstants(root);
    const RAB_Surface surface = RAB_GetGBufferSurface(int2(pixel), false);
    const bool isInfiniteExact = (constants.diFlags & kDiFlagExactInfiniteLights) != 0;

    float3 radiance = float3(0.0, 0.0, 0.0);
    if (RAB_IsSurfaceValid(surface) && surface.isLit)
    {
        if (hasRisLightCandidates(constants, isInfiniteExact))
        {
            const uint frameIndex = constants.runtime.frameIndex;
            RTXDI_RandomSamplerState rng =
                RTXDI_InitRandomSampler(pixel, frameIndex, RTXDI_DI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED);
            RTXDI_RandomSamplerState coherentRng =
                initCoherentRandomSampler(constants, pixel, frameIndex, RTXDI_DI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED);
            RTXDI_DIInitialSamplingParameters sampling = constants.initialSampling;
            // Streaming RIS traces its single final visibility ray below instead of an initial candidate shadow ray
            sampling.enableInitialVisibility = 0;
            // m8 I6 O8: without emissive triangles or an environment light no BRDF ray can hit a light
            if (gRtxdiBridge.triangleLights.lightCount == 0 &&
                constants.lightBuffer.environmentLightParams.lightPresent == 0)
            {
                sampling.numBrdfSamples = 0;
            }
            if (isInfiniteExact)
            {
                sampling.numInfiniteLightSamples = 0;
            }
            RAB_LightSample lightSample = RAB_EmptyLightSample();
            const RTXDI_DIReservoir reservoir = RTXDI_SampleLightsForSurface(
                rng, coherentRng, surface, sampling, constants.lightBuffer, constants.localLightRisBuffer,
                constants.environmentLightRisBuffer, regirParameters(), lightSample);
            if (RTXDI_IsValidDIReservoir(reservoir))
            {
                const float invPdf = RTXDI_GetDIReservoirInvPdf(reservoir);
                if (invPdf > 0.0)
                {
                    const float3 shade = shadeLightSample(surface, lightSample);
                    if (any(shade > 0.0) && RAB_GetConservativeVisibility(surface, lightSample))
                    {
                        radiance = shade * invPdf;
                        if (any(radiance > 0.0))
                        {
                            radiance *= glassTransmittance(root, surface, lightSample);
                        }
                    }
                }
            }
        }
        if (isInfiniteExact)
        {
            radiance += shadeInfiniteLightsExactly(root, surface, constants.lightBuffer.infiniteLightBufferRegion);
        }
    }
    storageTex2D<float4, kImageFormatRgba16f>(output)[pixel] = float4(radiance, 1.0);
}
