import types;
import core.binding;
import core.frame;
import lighting.glass;

static const uint kRtxdiTile = 8; // matches src/render/vk_renderer.cpp

// before the RTXDI headers: the firefly test's groupshared arrays are sized by the group
#define RTXDI_ENABLE_BOILING_FILTER
#define RTXDI_BOILING_FILTER_GROUP_SIZE 8 // kRtxdiTile

#include "lighting/rtxdi_bridge.slang"
#include "Rtxdi/GI/SpatialResampling.hlsli"
#include "Rtxdi/Utils/BoilingFilter.hlsli"
#include "Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli"

// ReSTIR GI spatial resampling and final shading (spec/m4/g5_restir_gi.md), the RTXDI sample's GI/SpatialResampling.hlsl
// and GI/FinalShading.hlsl fused as rtxdi_shade fuses DI's: this pixel's temporal reservoir against this frame's
// neighbors, then indirectRadiance = evalBrdf · Lo(X2) · NdotL · W with a reconnection ray and the glossy MIS against
// the pixel's initial sample. Neighbors are read from the temporal output; the result goes to the spatial output,
// next frame's temporal input. The reference (resampling None) shades the initial sample alone.
// With giDecorrelation (m8 O11), ReSTIR PT's DLSS-RR decorrelation from RTXDI's PT/Decorrelation.hlsli and
// PT/DuplicationMap.hlsli, ported to GI reservoirs: final shading may shade the initial sample instead, the stored
// reservoir keeps its history

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

// full-tile firefly test (m8 I2 F15): averages weight across all in-bounds lanes of the 8x8 group instead of nonzero
// lanes only
groupshared float gFullTileWeights[2];
groupshared uint gFullTileCounts[2];

bool fullTileBoilingFilter(uint2 localIndex, float filterStrength, float weight, bool isInBounds)
{
    const float multiplier = 10.0 / clamp(filterStrength, 1e-6, 1.0) - 9.0;
    const float waveWeight = WaveActiveSum(isInBounds ? weight : 0.0);
    const uint waveCount = WaveActiveCountBits(isInBounds);
    const uint linearThread = localIndex.x + localIndex.y * kRtxdiTile;
    const uint waveIndex = linearThread / WaveGetLaneCount();
    if (WaveIsFirstLane())
    {
        gFullTileWeights[waveIndex] = waveWeight;
        gFullTileCounts[waveIndex] = waveCount;
    }
    GroupMemoryBarrierWithGroupSync();
    const uint waveCountTotal = (kRtxdiTile * kRtxdiTile + WaveGetLaneCount() - 1) / WaveGetLaneCount();
    if (linearThread < waveCountTotal)
    {
        const float totalWeight = WaveActiveSum(gFullTileWeights[linearThread]);
        const uint totalCount = WaveActiveSum(gFullTileCounts[linearThread]);
        if (linearThread == 0)
        {
            gFullTileWeights[0] = totalCount > 0 ? totalWeight / float(totalCount) : 0.0;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    return isInBounds && weight > gFullTileWeights[0] * multiplier;
}

// the RTXDI sample's FinalShading.hlsl
static const float kMisRoughness = 0.3;
static const float kMaxBrdfValue = 1.0e4;

// RTXDI's per-pass seeds stop at 31; NRC's update pass takes 41 and 42, the indirect pass 43
static const uint kGiDecorrelationSeed = 44;
// RTXDI_PTStoreDuplicationInputs: stagnancy = age / 40
static const float kStagnancyAgeScale = 1.0 / 40.0;
// RTXDI_PT_SMOOTHED_DUPMAP_FILTER_STEP: the 5x5 previous-frame box, dilated
static const int kStagnancyFilterStep = 2;

// RTXDI_PTComputeSmoothedDuplicationMap, with the sample's ComputeSmoothedDuplicationMap.hlsl reprojection: this
// frame's stagnancy blended into last frame's 5x5 dilated average around the reprojected pixel
float smoothStagnancy(GpuFrameRoot root, GpuTargets targets, uint2 pixel, float current, float emaFactor)
{
    if (!hasIndirectHistory(root))
    {
        return current;
    }
    const float3 motion = tex2D(gRtxdiBridge.current.sampled[kGuideMotion]).Load(int3(pixel, 0)).xyz;
    const int2 previousPixel = int2(round(float2(pixel) + motion.xy));
    const int2 size = int2(gRtxdiBridge.renderSize);
    if (any(previousPixel < 0) || any(previousPixel >= size))
    {
        return current;
    }
    const Texture2D previousMap = tex2D(targets.giStagnancyPrevious);
    float sum = 0.0;
    float count = 0.0;
    const int radius = 2 * kStagnancyFilterStep;
    if (all(previousPixel >= radius) && all(previousPixel < size - radius))
    {
        [unroll]
        for (int dy = -2; dy <= 2; ++dy)
        {
            [unroll]
            for (int dx = -2; dx <= 2; ++dx)
            {
                sum += previousMap.Load(int3(previousPixel + int2(dx, dy) * kStagnancyFilterStep, 0)).x;
            }
        }
        count = 25.0;
    }
    else
    {
        [unroll]
        for (int dy = -2; dy <= 2; ++dy)
        {
            [unroll]
            for (int dx = -2; dx <= 2; ++dx)
            {
                const int2 tap = previousPixel + int2(dx, dy) * kStagnancyFilterStep;
                if (all(tap >= 0) && all(tap < size))
                {
                    sum += previousMap.Load(int3(tap, 0)).x;
                    count += 1.0;
                }
            }
        }
    }
    // the center tap is in bounds
    return lerp(sum / count, current, saturate(emaFactor));
}

// RTXDI_PTApplyDecorrelation's probability: Uniform, or Stagnancy-scaled; a detected firefly is always replaced
float decorrelationProbability(RTXDI_PTDecorrelationParameters params, float smoothedStagnancy, bool isFirefly)
{
    float probability = 0.0;
    if (params.decorrelationMode == RTXDI_PT_DECORRELATION_MODE_UNIFORM)
    {
        probability = saturate(params.decorrelationFactor);
    }
    else if (params.decorrelationMode == RTXDI_PT_DECORRELATION_MODE_STAGNANCY)
    {
        const float stagnancyScale = pow(saturate(smoothedStagnancy), max(0.0, params.decorrelationStagnancyExponent));
        probability = saturate(4.0 * saturate(params.decorrelationFactor) * stagnancyScale);
    }
    return params.fireflyReplacementFilterEnable != 0 && isFirefly ? 1.0 : probability;
}

// the initial sample's MIS share at a direction: high where the true lobe is far sharper than one roughened to
// kMisRoughness, where reuse from neighbors and history is least reliable
float glossyMisWeight(RAB_Surface surface, float3 samplePosition)
{
    RAB_Surface roughened = surface;
    roughened.roughness = max(roughened.roughness, kMisRoughness);
    const float3 unit = float3(1.0, 1.0, 1.0);
    const float3 roughBrdf = clamp(shadeGiSample(roughened, samplePosition, unit), 1.0e-4, kMaxBrdfValue);
    const float3 trueBrdf = clamp(shadeGiSample(surface, samplePosition, unit), 0.0, kMaxBrdfValue);
    const float weight = saturate(luminance(trueBrdf) / luminance(trueBrdf + roughBrdf));
    return weight * weight * weight;
}

// m8 I13 §3: the glass on the primary-to-X2 segment, as the reconnection ray sees it; targets and the resampling
// stay glass-free
float3 reconnectionTransmittance(GpuFrameRoot root, RAB_Surface surface, float3 samplePosition)
{
    if ((root.glassFlags & kGlassFlagShadowTransmittance) == 0)
    {
        return float3(1.0, 1.0, 1.0);
    }
    const float3 toSample = samplePosition - surface.position;
    const float distance = length(toSample);
    if (distance <= 0.0)
    {
        return float3(1.0, 1.0, 1.0);
    }
    const float3 l = toSample / distance;
    return glassTransmittance(root, rasterRayOrigin(surface, l), l, distance * (1.0 - kReconnectionShortening));
}

// the shadow transmittance's uniform switch: without it the shading keeps its pre-I13 expressions, so a scene without
// glass reads bit for bit as before (folding a 1.0 factor in changes the driver's FMA contraction)
bool isGiShadowTransmitted(GpuFrameRoot root)
{
    return (root.glassFlags & kGlassFlagShadowTransmittance) != 0;
}

float3 shadeFinalReservoir(GpuFrameRoot root, RAB_Surface surface, RTXDI_GIReservoir reservoir,
                           RTXDI_GIReservoir initial, RTXDI_GIFinalShadingParameters params)
{
    if (!RTXDI_IsValidGIReservoir(reservoir))
    {
        return float3(0.0, 0.0, 0.0);
    }
    float3 radiance = shadeGiSample(surface, reservoir.position, reservoir.radiance) * reservoir.weightSum;
    // the sample may be a neighbor's or last frame's: test opaque occlusion before walking glass layers
    if (params.enableFinalVisibility != 0 && any(radiance > 0.0) &&
        !isReconnectionVisible(surface, reservoir.position))
    {
        radiance = float3(0.0, 0.0, 0.0);
    }
    if (isGiShadowTransmitted(root) && any(radiance > 0.0))
    {
        radiance *= reconnectionTransmittance(root, surface, reservoir.position);
    }
    if (params.enableFinalMIS == 0)
    {
        return radiance;
    }
    radiance *= 1.0 - glossyMisWeight(surface, reservoir.position);
    // traced from this surface this frame: visible by construction
    if (RTXDI_IsValidGIReservoir(initial))
    {
        if (isGiShadowTransmitted(root))
        {
            const float3 initialRadiance = shadeGiSample(surface, initial.position, initial.radiance) * initial.weightSum;
            if (any(initialRadiance > 0.0))
            {
                radiance += initialRadiance * reconnectionTransmittance(root, surface, initial.position) *
                            glossyMisWeight(surface, initial.position);
            }
        }
        else
        {
            radiance += shadeGiSample(surface, initial.position, initial.radiance) * initial.weightSum *
                        glossyMisWeight(surface, initial.position);
        }
    }
    return radiance;
}

// lowbias32 integer hash
uint hashUint(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

// RTXDI_PTStoreDuplicationInputs' sample id: a GI sample is its X2, so equal positions are the same sample. 0: none
uint duplicationSampleId(RTXDI_GIReservoir reservoir)
{
    if (reservoir.weightSum == 0.0)
    {
        return 0u;
    }
    const uint3 bits = asuint(reservoir.position);
    return max(hashUint(bits.x ^ hashUint(bits.y ^ hashUint(bits.z))), 1u);
}

// the initial sample alone: the reference's estimate, and a decorrelated pixel's. The MIS of a sample with itself is
// the identity, and it was traced from this surface this frame, so it needs no visibility ray
float3 shadeInitialSample(GpuFrameRoot root, RAB_Surface surface, RTXDI_GIReservoir initial)
{
    if (!RTXDI_IsValidGIReservoir(initial))
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 radiance = shadeGiSample(surface, initial.position, initial.radiance) * initial.weightSum;
    return isGiShadowTransmitted(root) && any(radiance > 0.0)
        ? radiance * reconnectionTransmittance(root, surface, initial.position)
        : radiance;
}

[shader("compute")]
[numthreads(kRtxdiTile, kRtxdiTile, 1)]
void rtxdiGiShadeMain(uint3 id : SV_DispatchThreadID, uint3 localId : SV_GroupThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    rtxdiBridgeInit(root);
    // no early return before the firefly test: it synchronizes the group. Out of bounds the surface is invalid and
    // the reservoirs stay empty, so padding lanes never add to the group threshold
    const bool isInBounds = all(id.xy < gRtxdiBridge.renderSize);
    const GpuTargets targets = bdaPtr<GpuTargets>(root.targets)[0];
    const GpuRtxdiConstants constants = loadRtxdiConstants(root);
    const RTXDI_PTDecorrelationParameters decorrelation = constants.giDecorrelation;
    const RAB_Surface surface = RAB_GetGBufferSurface(int2(id.xy), false);
    const uint2 reservoirPosition = RTXDI_PixelPosToReservoirPos(id.xy, constants.runtime.activeCheckerboardField);
    RTXDI_GIReservoir initial = RTXDI_EmptyGIReservoir();
    if (isInBounds)
    {
        initial = RTXDI_LoadGIReservoir(constants.giReservoirBuffer, reservoirPosition, kGiInitialReservoirArray);
    }

    float3 radiance = float3(0.0, 0.0, 0.0);
    // uniform
    if (constants.giResamplingMode == 0)
    {
        // the unbiased 1-sample estimate the reuse converges to
        radiance = shadeInitialSample(root, surface, initial);
    }
    else
    {
        const uint inputArray = constants.giBufferIndices.spatialResamplingInputBufferIndex;
        RTXDI_GIReservoir reservoir = RTXDI_EmptyGIReservoir();
        if (isInBounds)
        {
            reservoir = RTXDI_LoadGIReservoir(constants.giReservoirBuffer, reservoirPosition, inputArray);
        }
        if ((constants.giResamplingMode & 2u) != 0 && RAB_IsSurfaceValid(surface))
        {
            const float3 temporalPosition = reservoir.position;
            RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(id.xy, constants.runtime.frameIndex,
                                                                   RTXDI_GI_SPATIAL_RESAMPLING_RANDOM_SEED);
            reservoir = RTXDI_GISpatialResampling(id.xy, surface, inputArray, reservoir, rng, constants.runtime,
                                                  constants.giReservoirBuffer, constants.giSpatialResampling);
            // RTXDI_GISpatialResampling does not report which sample won; a neighbor's has another X2
            if ((constants.giFlags & kGiFlagSpatialAgeReset) != 0 && any(reservoir.position != temporalPosition))
            {
                reservoir.age = 0;
            }
        }
        if (isInBounds)
        {
            RTXDI_StoreGIReservoir(reservoir, constants.giReservoirBuffer, reservoirPosition,
                                   constants.giBufferIndices.spatialResamplingOutputBufferIndex);
            // the final reservoirs are next frame's temporal input: passes/rtxdi_gi_duplication.slang counts them
            if ((constants.giFlags & kGiFlagDuplicationHistoryReduction) != 0)
            {
                storageTex2D<uint, kImageFormatR32ui>(targets.giSampleIdsStorage)[id.xy] =
                    duplicationSampleId(reservoir);
            }
        }
        // uniform: RTXDI_PTApplyDecorrelation's gate. Temporal reuse runs whenever reuse does
        const bool isDecorrelated = decorrelation.decorrelationMode != RTXDI_PT_DECORRELATION_MODE_NONE &&
            decorrelation.decorrelationFactor > 0.0 && (constants.giResamplingMode & 1u) != 0;
        bool isFirefly = false;
        if (isDecorrelated && decorrelation.fireflyReplacementFilterEnable != 0)
        {
            // RTXDI_PTDetectDecorrelationFireflies: the reservoir's target luminance times its weight
            const float fireflyWeight = RTXDI_IsValidGIReservoir(reservoir)
                ? RAB_GetGISampleTargetPdfForSurface(reservoir.position, reservoir.radiance, surface) *
                    reservoir.weightSum
                : 0.0;
            isFirefly = (constants.giFlags & kGiFlagFullTileBoilingFilter) != 0
                ? fullTileBoilingFilter(localId.xy, decorrelation.fireflyReplacementFilterStrength, fireflyWeight,
                                        isInBounds)
                : RTXDI_BoilingFilterInternal(localId.xy, decorrelation.fireflyReplacementFilterStrength,
                                              fireflyWeight);
        }
        // m8 I2 F14: GI's Combine uses <=, so zero-weight reservoirs inherit and increment age (unlike PT's <); keep
        // stagnancy at 0 when the reservoir carries no sample weight
        const float stagnancy =
            reservoir.weightSum > 0.0 ? min(float(reservoir.age) * kStagnancyAgeScale, 1.0) : 0.0;
        float smoothedStagnancy = 0.0;
        if (isDecorrelated && decorrelation.decorrelationMode == RTXDI_PT_DECORRELATION_MODE_STAGNANCY && isInBounds)
        {
            smoothedStagnancy = smoothStagnancy(root, targets, id.xy, stagnancy, decorrelation.decorrelationEmaFactor);
            storageTex2D<float, kImageFormatR32f>(targets.giStagnancyStorage)[id.xy] = smoothedStagnancy;
        }
        const float probability =
            isDecorrelated ? decorrelationProbability(decorrelation, smoothedStagnancy, isFirefly) : 0.0;
        bool isReplaced = false;
        if (probability > 0.0)
        {
            RTXDI_RandomSamplerState rng =
                RTXDI_InitRandomSampler(id.xy, constants.runtime.frameIndex, kGiDecorrelationSeed);
            isReplaced = RTXDI_GetNextRandom(rng) < probability;
        }
        if (isReplaced)
        {
            // a fresh final sample won this frame: its swap keeps at most the bound times its weight; never clamp to
            // 0 when spatial visibility zeroed the final reservoir (m8 I2 F14)
            if (decorrelation.fireflyReplacementBiasReduction != 0 && stagnancy == 0.0 && reservoir.weightSum > 0.0)
            {
                initial.weightSum =
                    min(decorrelation.fireflyReplacementMultiplyBound * reservoir.weightSum, initial.weightSum);
            }
            radiance = shadeInitialSample(root, surface, initial);
        }
        else
        {
            radiance = shadeFinalReservoir(root, surface, reservoir, initial, constants.giFinalShading);
        }
    }
    if (!isInBounds)
    {
        return;
    }
    // RenderTargets::kIndirectRadianceFormat
    storageTex2D<float4, kImageFormatRgba16f>(targets.indirectRadianceStorage)[id.xy] = float4(radiance, 1.0);
}
