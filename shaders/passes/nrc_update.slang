import types;
import core.binding;
import core.frame;
import geometry.hit;
import material.brdf;
import material.pbr;
import raytracing.trace;

#include "lighting/rtxdi_bridge.slang"
#include "lighting/path_shading.slang"
// NRC_UPDATE=1 (CMakeLists.txt)
#include "gi/nrc_bridge.slang"

// NRC update pass (spec/m4/g3_nrc.md): sparse training paths from jittered G-buffer pixels, as the RTXGI
// IndirectDiffuse sample's TraceNrcIndirectDiffuseRays in update mode. The primary surface is vertex 0; every vertex
// adds its emission and one RIS-sampled light, samples the full BRDF and traces a ray query, so the network learns
// each vertex's whole outgoing radiance. One thread per training path over the used training dimensions
// (GpuNrcConstants.trainingLaunch). Compute with traceClosest while SER is shelved (considerations C-G1-3)

static const uint kUpdateTile = 8;

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

// RTXDI's per-pass seeds stop at 31 (RandomSamplerPerPassSeeds.hlsli)
static const uint kNrcUpdatePathSeed = 41;
static const uint kNrcUpdateLightSeed = 42;

static const float kMissDistance = 1.0e5;

// the sample's SelectSparseGBufferPixel: launch cell + a random offset, scaled to the render size
uint2 selectTrainingPixel(uint2 launch, uint2 launchSize, uint2 renderSize, inout RTXDI_RandomSamplerState rng)
{
    const float2 offset = float2(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
    const float2 scale = float2(max(renderSize, uint2(1, 1))) / float2(max(launchSize, uint2(1, 1)));
    return min(uint2((float2(launch) + offset) * scale), max(renderSize, uint2(1, 1)) - 1);
}

NrcSurfaceAttributes attributesOf(NrcConstants constants, RAB_Surface surface)
{
    return nrcSurfaceAttributes(constants, surface.position, surface.normal, surface.viewDir, surface.roughness,
                                surface.diffuseAlbedo, surface.specularF0);
}

// the next segment from a lit vertex; false: absorbed
bool scatter<Origin : IRayOrigin>(RAB_Surface surface, Origin origin, inout RTXDI_RandomSamplerState rng,
                                   inout float3 throughput, inout NrcPathState path, inout RayDesc ray)
{
    if (!surface.isLit)
    {
        NrcSetDebugPathTerminationReason(path, NrcDebugPathTerminationReason::BRDFAbsorption);
        return false;
    }
    const float3 u = float3(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
    const BrdfSample brdf =
        sampleBrdf(surface.normal, surface.diffuseAlbedo, surface.specularF0, surface.roughness, surface.viewDir, u);
    if (brdf.pdf <= 0.0)
    {
        NrcSetDebugPathTerminationReason(path, NrcDebugPathTerminationReason::BRDFAbsorption);
        return false;
    }
    throughput *= brdf.weight;
    // the heuristic grows the path spread at the next hit from this pdf
    NrcSetBrdfPdf(path, brdf.pdf);
    ray.Origin = origin.spawn(brdf.l);
    ray.Direction = brdf.l;
    return true;
}

[shader("compute")]
[numthreads(kUpdateTile, kUpdateTile, 1)]
void nrcUpdateMain(uint3 id : SV_DispatchThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    const GpuNrcConstants cache = loadNrcConstants(root);
    const uint2 launchSize = uint2(cache.trainingLaunch & 0xFFFFu, cache.trainingLaunch >> 16);
    const uint2 launch = id.xy;
    if (any(launch >= launchSize))
    {
        return;
    }
    rtxdiBridgeInit(root);
    const NrcConstants nrcConstants = nrcBridgeInit(root);
    const GpuRtxdiConstants rtxdiConstants = loadRtxdiConstants(root);
    const uint frameIndex = root.frameIndex;
    // the training launch is the path's identity; the light RNG is coherent per launch tile like ReSTIR DI's
    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(launch, frameIndex, kNrcUpdatePathSeed);
    RTXDI_RandomSamplerState lightRng = RTXDI_InitRandomSampler(launch, frameIndex, kNrcUpdateLightSeed);
    RTXDI_RandomSamplerState coherentRng =
        RTXDI_InitRandomSampler(launch / RTXDI_TILE_SIZE_IN_PIXELS, frameIndex, kNrcUpdateLightSeed);
    const uint2 pixel = selectTrainingPixel(launch, launchSize, gRtxdiBridge.renderSize, rng);
    const uint maxPathVertices = nrcConstants.maxPathVertices;

    const NrcBuffers buffers = {};
    const NrcContext nrc = NrcCreateContext(nrcConstants, buffers, launch);
    NrcPathState path = NrcCreatePathState(nrcConstants, RTXDI_GetNextRandom(rng));
    float3 throughput = float3(1.0, 1.0, 1.0);
    float3 radiance = float3(0.0, 0.0, 0.0);
    RayDesc ray;
    ray.Origin = float3(0.0, 0.0, 0.0);
    ray.Direction = float3(0.0, 0.0, 1.0);
    ray.TMin = 0.0;
    ray.TMax = kMissDistance;
    bool isActive = false;
    // camera to the current ray origin: the texture ray cone grows over it
    float pathLength = 0.0;

    const RAB_Surface primary = RAB_GetGBufferSurface(int2(pixel), false);
    if (!RAB_IsSurfaceValid(primary))
    {
        NrcUpdateOnMiss(path);
    }
    else
    {
        pathLength = length(primary.position - gRtxdiBridge.cameraPos);
        const NrcProgressState progress =
            NrcUpdateOnHit(nrc, path, attributesOf(nrcConstants, primary), pathLength, 0, throughput, radiance);
        if (progress != NrcProgressState::TerminateImmediately)
        {
            // the root's own light only reaches the network; the image takes the primary's direct light from ReSTIR DI
            RasterOrigin origin;
            origin.surface = primary;
            const float3 emissive =
                tex2D(gRtxdiBridge.current.sampled[kGuideEmissive]).Load(int3(int2(pixel), 0)).rgb;
            radiance += throughput * (emissive + directLightAt(primary, origin, rtxdiConstants, lightRng, coherentRng));
            if (progress == NrcProgressState::Continue)
            {
                isActive = scatter(primary, origin, rng, throughput, path, ray);
            }
        }
    }

    for (uint bounce = 1; isActive && bounce < maxPathVertices; ++bounce)
    {
        RayHit hit;
        if (!traceClosest(root, ray, hit))
        {
            NrcUpdateOnMiss(path);
            break;
        }
        const PathHit vertex = shadeRayHit(root, hit, ray, pathLength + hit.t);
        pathLength += hit.t;
        const NrcProgressState progress =
            NrcUpdateOnHit(nrc, path, attributesOf(nrcConstants, vertex.surface), hit.t, bounce, throughput, radiance);
        if (progress == NrcProgressState::TerminateImmediately)
        {
            break;
        }
        HitOrigin origin;
        origin.hit = hit;
        radiance += throughput *
                    (vertex.emissive + directLightAt(vertex.surface, origin, rtxdiConstants, lightRng, coherentRng));
        // without cached direct light, NRC ends a path after the direct light of its last vertex
        if (progress == NrcProgressState::TerminateAfterDirectLighting)
        {
            break;
        }
        if (bounce == maxPathVertices - 1)
        {
            NrcSetDebugPathTerminationReason(path, NrcDebugPathTerminationReason::MaxPathVertices);
            break;
        }
        isActive = scatter(vertex.surface, origin, rng, throughput, path, ray);
    }

    NrcWriteFinalPathInfo(nrc, path, throughput, radiance);
}
