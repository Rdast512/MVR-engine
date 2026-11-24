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

// NrcCache's divergence detector (m8 I16 C-I16-5), per wave so it needs no scale: waves with traced light count,
// diverged ones answer non-finite or above kNrcDivergedRatio times it. One atomic per wave and counter
void countNrcHealth(GpuNrcConstants constants, bool isCounted, bool isFinite, float cachedLuminance,
                    float tracedLuminance)
{
    const float cachedSum = WaveActiveSum(isCounted && isFinite ? cachedLuminance : 0.0);
    const float tracedSum = WaveActiveSum(isCounted ? tracedLuminance : 0.0);
    const bool hasNonFinite = WaveActiveAnyTrue(isCounted && !isFinite);
    if (constants.health == 0 || !WaveIsFirstLane() || !(tracedSum > 0.0 || hasNonFinite))
    {
        return;
    }
    uint* health = bdaPtr<uint>(constants.health);
    InterlockedAdd(health[kNrcHealthQueried], 1u);
    const float ratio = tracedSum > 0.0 ? cachedSum / tracedSum : 0.0;
    if (hasNonFinite || ratio > kNrcDivergedRatio)
    {
        InterlockedAdd(health[kNrcHealthDiverged], 1u);
    }
    // positive floats order like their bits
    InterlockedMax(health[kNrcHealthMaxRatio], asuint(ratio));
}

[shader("compute")]
[numthreads(kResolveTile, kResolveTile, 1)]
void nrcResolveMain(uint3 id : SV_DispatchThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    rtxdiBridgeInit(root);
    if (any(id.xy >= gRtxdiBridge.renderSize))
    {
        return;
    }
    const GpuNrcConstants nrcConstants = loadNrcConstants(root);
    NrcQueryAnswer answer;
    const bool isQueried = nrcLoadQueryAnswer(nrcConstants, id.xy, answer);
    const float3 cached = isQueried ? nrcPathRadiance(answer) : float3(0.0, 0.0, 0.0);
    const GpuRtxdiConstants constants = loadRtxdiConstants(root);
    const uint2 reservoirPos = RTXDI_PixelPosToReservoirPos(id.xy, constants.runtime.activeCheckerboardField);
    RTXDI_GIReservoir reservoir =
        RTXDI_LoadGIReservoir(constants.giReservoirBuffer, reservoirPos, kGiInitialReservoirArray);
    const bool isValid = RTXDI_IsValidGIReservoir(reservoir);
    // before the early return: the wave ops need every lane that has a reservoir or not
    countNrcHealth(nrcConstants, isQueried && isValid, isNrcAnswerFinite(answer), luminance(cached),
                   isValid ? luminance(reservoir.radiance) : 0.0);
    if (!isValid)
    {
        return;
    }
    bool shouldStore = false;
    if (any(cached > 0.0))
    {
        const float ao = tex2D(gRtxdiBridge.current.sampled[kGuideDiffuseAlbedo]).Load(int3(int2(id.xy), 0)).a;
        const float3 added = cached * ao;
        if (any(added > 0.0))
        {
            reservoir.radiance += added;
            shouldStore = true;
        }
    }
    // m8 I2 F13: indirect kept X2's true position while an NRC query was pending; if radiance is still zero after
    // resolve, replace it with the far miss placeholder so dark reservoirs keep Jacobian ~1 during reuse
    if (all(reservoir.radiance <= 0.0))
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
