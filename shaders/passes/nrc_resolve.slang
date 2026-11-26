import types;
import core.binding;
import core.frame;

#include "lighting/rtxdi_bridge.slang"
#include "Rtxdi/GI/Reservoir.hlsli"
#include "gi/nrc_bridge.slang"

// NRC custom resolve (NRC guide step 7, spec/m4/g3_nrc.md): after QueryAndTrain, the cache's answer for each pixel's
// query at X2 joins the radiance of the pixel's initial ReSTIR GI reservoir. The reservoir weight is 1/pdf and does not
// depend on radiance, so the add keeps it valid (G2 §1). Material AO of the primary applies to this cache term only
// (engine spec §11.6)

static const uint kResolveTile = 8; // matches src/render/vk_renderer.cpp kFullscreenTile

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

// a wave's cache answers against the radiance traced at the same X2 vertices: a healthy cache adds indirect light of
// the traced light's order, a diverged one orders of magnitude more (C-I16-5: 700x poisoned, ~1e6x measured)
static const float kNrcDivergedRatio = 50.0;
// absolute per-lane ceiling when a wave has no direct/emissive light at X2 (50x c_MaxIndirectRadiance, well below
// C-I16-5's ~3000 mean)
static const float kNrcUntracedDivergedLuminance = 500.0;
// matches indirect.slang kMaxIndirectRadiance (M8.1 BUG-E6)
static const float kMaxIndirectRadiance = 10.0;

// NrcCache's divergence detector (m8 I16 C-I16-5): waves with traced light compare scale-free against tracedSum;
// shadowed waves (tracedSum == 0) count only when non-finite or above kNrcUntracedDivergedLuminance so healthy dark
// waves do not dilute the queried share. Call in uniform control flow
void countNrcHealth(GpuNrcConstants constants, bool isCounted, bool isFinite, float cachedLuminance,
                    float tracedLuminance)
{
    const bool isLaneFinite = isFinite && !isnan(cachedLuminance) && !isinf(cachedLuminance);
    const uint queriedLanes = WaveActiveCountBits(isCounted);
    const float cachedSum = WaveActiveSum(isCounted && isLaneFinite ? cachedLuminance : 0.0);
    const float tracedSum = WaveActiveSum(isCounted ? tracedLuminance : 0.0);
    const bool hasNonFinite =
        WaveActiveAnyTrue(isCounted && !isLaneFinite) || isnan(cachedSum) || isinf(cachedSum);
    const bool hasUntracedDivergence = queriedLanes > 0 && tracedSum <= 0.0 &&
        (hasNonFinite || cachedSum > float(queriedLanes) * kNrcUntracedDivergedLuminance);
    if (constants.health == 0 || !WaveIsFirstLane() || !(tracedSum > 0.0 || hasUntracedDivergence))
    {
        return;
    }
    GpuNrcHealth* health = bdaPtr<GpuNrcHealth>(constants.health);
    InterlockedAdd(health.queried, 1u);
    const float ratio = tracedSum > 0.0 ? cachedSum / tracedSum : cachedSum / float(queriedLanes);
    if (hasNonFinite || hasUntracedDivergence || ratio > kNrcDivergedRatio)
    {
        InterlockedAdd(health.diverged, 1u);
    }
    // positive floats order like their bits; guard NaN from poisoning InterlockedMax
    const float orderedRatio = isnan(ratio) ? 0.0 : max(ratio, 0.0);
    InterlockedMax(health.maxRatioBits, asuint(orderedRatio));
}

[shader("compute")]
[numthreads(kResolveTile, kResolveTile, 1)]
void nrcResolveMain(uint3 id : SV_DispatchThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    rtxdiBridgeInit(root);
    const bool isInBounds = all(id.xy < gRtxdiBridge.renderSize);
    const GpuNrcConstants nrcConstants = loadNrcConstants(root);
    NrcQueryAnswer answer;
    answer.radiance = float3(0.0, 0.0, 0.0);
    answer.prefixThroughput = float3(0.0, 0.0, 0.0);
    const bool isQueried = isInBounds && nrcLoadQueryAnswer(nrcConstants, id.xy, answer);
    const bool isAnswerFinite = isNrcAnswerFinite(answer);
    const float3 cached = isQueried && isAnswerFinite ? nrcPathRadiance(answer) : float3(0.0, 0.0, 0.0);
    const GpuRtxdiConstants constants = loadRtxdiConstants(root);
    uint2 reservoirPos = uint2(0, 0);
    RTXDI_GIReservoir reservoir = RTXDI_EmptyGIReservoir();
    if (isInBounds)
    {
        reservoirPos = RTXDI_PixelPosToReservoirPos(id.xy, constants.runtime.activeCheckerboardField);
        reservoir = RTXDI_LoadGIReservoir(constants.giReservoirBuffer, reservoirPos, kGiInitialReservoirArray);
    }
    const bool isValid = isInBounds && RTXDI_IsValidGIReservoir(reservoir);
    // before any early return: wave ops require uniform control flow across the tile
    countNrcHealth(nrcConstants, isQueried && isValid, isAnswerFinite, luminance(cached),
                   isValid ? luminance(reservoir.radiance) : 0.0);
    if (!isValid)
    {
        return;
    }
    bool shouldStore = false;
    // a non-finite answer would poison the reservoir and every pixel that reuses it
    if (any(cached > 0.0))
    {
        // M8.1 BUG-E2: apply primary pixel AO only when ReSTIR GI reuse is off so pixel A's AO never leaks into
        // shared reservoirs reused at pixel B
        const float ao = constants.giResamplingMode == 0
            ? tex2D(gRtxdiBridge.current.sampled[kGuideDiffuseAlbedo]).Load(int3(int2(id.xy), 0)).a
            : 1.0;
        const float3 added = cached * ao;
        if (any(added > 0.0) && isFiniteFloat3(added) && isFiniteFloat3(reservoir.radiance + added))
        {
            reservoir.radiance += added;
            // M8.1 BUG-E6: enforce kGiFlagFireflyClamp after adding cached multi-bounce radiance
            if ((constants.giFlags & kGiFlagFireflyClamp) != 0)
            {
                const float totalLuminance = luminance(reservoir.radiance);
                if (totalLuminance > kMaxIndirectRadiance)
                {
                    reservoir.radiance *= kMaxIndirectRadiance / totalLuminance;
                }
            }
            shouldStore = true;
        }
    }
    // m8 I2 F13 + M8.1 BUG-D6: indirect kept X2's true position while an NRC query was pending; if radiance is still
    // below LogLuv's 1e-6 floor after resolve, replace it with the far miss placeholder so dark reservoirs keep
    // Jacobian ~1 during reuse
    if (luminance(reservoir.radiance) < 1.0e-6)
    {
        const RAB_Surface primary = RAB_GetGBufferSurface(int2(id.xy), false);
        if (RAB_IsSurfaceValid(primary))
        {
            const float3 toSample = reservoir.position - primary.position;
            const float distSq = dot(toSample, toSample);
            if (distSq > 0.0 && distSq < 0.25 * kMissDistance * kMissDistance)
            {
                const float3 dir = toSample * rsqrt(distSq);
                reservoir.position = primary.position + dir * kMissDistance;
                reservoir.normal = -dir;
                shouldStore = true;
            }
        }
    }
    if (shouldStore)
    {
        RTXDI_StoreGIReservoir(reservoir, constants.giReservoirBuffer, reservoirPos, kGiInitialReservoirArray);
    }
}
