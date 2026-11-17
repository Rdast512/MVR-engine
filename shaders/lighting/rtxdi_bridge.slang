#ifndef ENGINE_LIGHTING_RTXDI_BRIDGE_SLANG
#define ENGINE_LIGHTING_RTXDI_BRIDGE_SLANG

// RTXDI application bridge (PBR plan F2): the resource macros and the RAB_* set the pinned RTXDI 3.2 DI headers call,
// plus the GI reservoir buffer the indirect pass writes (M4 G2).
// Textual, once per compile unit, before any Rtxdi/DI header; call rtxdiBridgeInit before the first RAB_* or RTXDI_*
// call. Light and BRDF math stays in lighting.lights and material.brdf: the bridge only composes them.
//
// Surfaces come from the guide sets: current or previous by frame parity, so a previous-frame surface is last frame's
// resolve output (position guide, normals, albedos, camera position). Lights are E2's world rows, last frame's for
// previous-frame loads; light index i is row i in both frames (RAB_TranslateLightIndex is the identity), and a load or
// unload resets history.
//
// Not built yet:
//   - presampling (Power_RIS / ReGIR, M7): RAB_LoadCompactLightInfo, RAB_StoreCompactLightInfo, RIS buffer
//   - emissive triangles (M7): RAB_TraceRayForLocalLight never names a light; RAB_EvaluateLocalLightSourcePdf is the
//     uniform pdf
//   - environment light: lightPresent stays 0, so its two functions are never reached

import types;
import core.binding;
import core.frame;
import core.math;
import lighting.lights;
import geometry.hit;
import material.brdf;
import material.pbr;
import raytracing.trace;

// before any RTXDI header: RtxdiParameters.h defaults it to 1
#define RTXDI_ENABLE_PRESAMPLING 0

// RTXDI's buffers live in RtxdiContext's allocations (src/render/vk_rtxdi.cpp). Neighbor offsets are always indexed
// through neighborOffsetMask, so a plain BDA pointer serves
#define RTXDI_LIGHT_RESERVOIR_BUFFER gRtxdiBridge.reservoirs
#define RTXDI_GI_RESERVOIR_BUFFER gRtxdiBridge.giReservoirs
#define RTXDI_NEIGHBOR_OFFSETS_BUFFER bdaPtr<float2>(gRtxdiBridge.neighborOffsets)

// RTXDI's HLSL converts uint to bool implicitly (E30081): units that include this file compile with
// RTXDI_SLANG_FLAGS (CMakeLists.txt). #pragma warning cannot scope it, Slang 2026.20 ignores it once a unit imports
#include "lighting/rtxdi_constants.slang"
#include "Rtxdi/RtxdiParameters.h"
#include "Rtxdi/Utils/RandomSamplerState.hlsli"

// RTXDI reads its buffers as structured buffers under robust buffer access, where out-of-range reads return zeros.
// 3.1's fused pass relied on it: it loaded the reservoir of an off-screen back-projected pixel and light 0 of an empty
// result, also with no lights, and through BDA those reads faulted. 3.2 dropped that pass and guards the empty result,
// but the library still assumes robust buffers, so the reservoir accessors (CheckedBuffer) and RAB_LoadLightInfo check
// their bounds: zeros out of range, stores dropped

// uniform per dispatch; filled by rtxdiBridgeInit
struct RtxdiBridgeState
{
    GpuGuideSet current;
    GpuGuideSet previous;
    uint2 renderSize;
    float3 cameraPos;
    float3 prevCameraPos;
    uint64_t tlas;
    uint64_t worldLights;
    uint64_t prevWorldLights;
    uint lightCount;
    CheckedBuffer<RTXDI_PackedDIReservoir> reservoirs;
    CheckedBuffer<RTXDI_PackedGIReservoir> giReservoirs;
    uint64_t neighborOffsets;
    uint localLightCount;
};

static RtxdiBridgeState gRtxdiBridge;

void rtxdiBridgeInit(GpuFrameRoot root)
{
    const GpuTargets targets = loadTargets(root);
    GpuCameraData* camera = bdaPtr<GpuCameraData>(root.camera);
    GpuRtxdiConstants* constants = bdaPtr<GpuRtxdiConstants>(root.rtxdiConstants);
    gRtxdiBridge.current = targets.current;
    gRtxdiBridge.previous = targets.previous;
    gRtxdiBridge.renderSize = targets.renderSize;
    gRtxdiBridge.cameraPos = camera[0].cameraPos;
    gRtxdiBridge.prevCameraPos = camera[0].prevCameraPos;
    gRtxdiBridge.tlas = root.tlas;
    gRtxdiBridge.worldLights = root.worldLights;
    gRtxdiBridge.prevWorldLights = root.prevWorldLights;
    gRtxdiBridge.lightCount = root.lightCount;
    gRtxdiBridge.reservoirs.address = constants[0].reservoirs;
    gRtxdiBridge.reservoirs.count = constants[0].reservoirCount;
    gRtxdiBridge.giReservoirs.address = constants[0].giReservoirs;
    gRtxdiBridge.giReservoirs.count = constants[0].giReservoirCount;
    gRtxdiBridge.neighborOffsets = constants[0].neighborOffsets;
    gRtxdiBridge.localLightCount = constants[0].lightBuffer.localLightBufferRegion.numLights;
}

GpuRtxdiConstants loadRtxdiConstants(GpuFrameRoot root)
{
    return bdaPtr<GpuRtxdiConstants>(root.rtxdiConstants)[0];
}

//
// surface: one guide-set pixel
//

struct RAB_Surface
{
    float3 position;
    float viewDepth; // 0: no surface
    float3 normal;   // shading
    float roughness;
    float3 geoNormal;
    float3 viewDir; // unit, toward the camera of the surface's frame
    float3 diffuseAlbedo;
    float3 specularF0;
    bool isLit; // unlit (KHR_materials_unlit) surfaces take no light
};

struct RAB_Material
{
    float3 diffuseAlbedo;
    float3 specularF0;
    float roughness;
};

RAB_Surface RAB_EmptySurface()
{
    RAB_Surface surface = {};
    return surface;
}

RAB_Surface RAB_GetGBufferSurface(int2 pixel, bool previousFrame)
{
    RAB_Surface surface = RAB_EmptySurface();
    if (any(pixel < 0) || any(pixel >= int2(gRtxdiBridge.renderSize)))
    {
        return surface;
    }
    const GpuGuideSet set = previousFrame ? gRtxdiBridge.previous : gRtxdiBridge.current;
    const int3 texel = int3(pixel, 0);
    const float4 position = tex2D(set.sampled[kGuidePosition]).Load(texel);
    if (position.w <= 0.0)
    {
        return surface;
    }
    const float4 normalRoughness = tex2D(set.sampled[kGuideNormalRoughness]).Load(texel);
    const float4 specularF0 = tex2D(set.sampled[kGuideSpecularF0]).Load(texel);
    surface.position = position.xyz;
    surface.viewDepth = position.w;
    surface.normal = normalRoughness.xyz;
    surface.roughness = normalRoughness.w;
    surface.geoNormal = octDecode(tex2D(set.sampled[kGuideGeoNormal]).Load(texel).xy);
    surface.viewDir = normalize((previousFrame ? gRtxdiBridge.prevCameraPos : gRtxdiBridge.cameraPos) - position.xyz);
    surface.diffuseAlbedo = tex2D(set.sampled[kGuideDiffuseAlbedo]).Load(texel).rgb;
    surface.specularF0 = specularF0.rgb;
    surface.isLit = specularF0.a > 0.0;
    return surface;
}

// a ray hit (M4 G2): the decoded surface and its sampled material, seen from the ray. viewDepth holds the hit
// distance, only so the surface tests valid; no RTXDI temporal or spatial function receives a hit surface
RAB_Surface rayHitSurface(DecodedHit decoded, SampledMaterial material, float3 rayDirection, float hitDistance)
{
    const BrdfInputs brdf = fromMaterial(material);
    RAB_Surface surface;
    surface.position = decoded.surface.position;
    surface.viewDepth = hitDistance;
    surface.normal = material.n;
    surface.roughness = material.roughness;
    surface.geoNormal = decoded.surface.geoNormal;
    surface.viewDir = -rayDirection;
    surface.diffuseAlbedo = brdf.cDiff;
    // unlit: as the resolve, the weight takes the specular lobe and all light away
    surface.specularF0 = brdf.f0 * material.specularWeight;
    surface.isLit = material.specularWeight > 0.0;
    return surface;
}

bool RAB_IsSurfaceValid(RAB_Surface surface)
{
    return surface.viewDepth > 0.0;
}

float3 RAB_GetSurfaceWorldPos(RAB_Surface surface)
{
    return surface.position;
}

float3 RAB_GetSurfaceNormal(RAB_Surface surface)
{
    return surface.normal;
}

// view depth of the surface's own frame, the unit of motion.z
float RAB_GetSurfaceLinearDepth(RAB_Surface surface)
{
    return surface.viewDepth;
}

RAB_Material RAB_GetMaterial(RAB_Surface surface)
{
    RAB_Material material;
    material.diffuseAlbedo = surface.diffuseAlbedo;
    material.specularF0 = surface.specularF0;
    material.roughness = surface.roughness;
    return material;
}

// thresholds of the RTXDI sample application's bridge
static const float kMaterialRoughnessThreshold = 0.5;
static const float kMaterialReflectivityThreshold = 0.25;
static const float kMaterialAlbedoThreshold = 0.25;

bool RAB_AreMaterialsSimilar(RAB_Material a, RAB_Material b)
{
    if (!RTXDI_CompareRelativeDifference(a.roughness, b.roughness, kMaterialRoughnessThreshold))
    {
        return false;
    }
    if (abs(luminance(a.specularF0) - luminance(b.specularF0)) > kMaterialReflectivityThreshold)
    {
        return false;
    }
    return abs(luminance(a.diffuseAlbedo) - luminance(b.diffuseAlbedo)) <= kMaterialAlbedoThreshold;
}

int2 RAB_ClampSamplePositionIntoView(int2 pixel, bool previousFrame)
{
    return clamp(pixel, int2(0, 0), int2(gRtxdiBridge.renderSize) - 1);
}

//
// BRDF
//

float RAB_SurfaceEvaluateBrdfPdf(RAB_Surface surface, float3 direction)
{
    return pdfBrdf(surface.normal, surface.diffuseAlbedo, surface.specularF0, surface.roughness, surface.viewDir,
                   direction);
}

bool RAB_SurfaceImportanceSampleBrdf(RAB_Surface surface, inout RTXDI_RandomSamplerState rng, out float3 direction)
{
    const float3 u = float3(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
    const BrdfSample brdf =
        sampleBrdf(surface.normal, surface.diffuseAlbedo, surface.specularF0, surface.roughness, surface.viewDir, u);
    direction = brdf.l;
    return brdf.pdf > 0.0;
}

//
// lights: E2's world rows, punctual only until M7
//

typealias RAB_LightInfo = GpuLight;

// evalLight at the surface it was sampled for; RTXDI always scores a sample against that same surface
struct RAB_LightSample
{
    float3 l;        // unit, toward the light
    float distance;  // visibility tMax; kInfiniteDistance for directional
    float3 radiance; // incident, before NdotL
};

RAB_LightInfo RAB_EmptyLightInfo()
{
    RAB_LightInfo light = {};
    return light;
}

RAB_LightSample RAB_EmptyLightSample()
{
    RAB_LightSample sample = {};
    return sample;
}

// previousFrame: last frame's row, or this frame's after a history reset. Out of range: the empty light, which shades
// to 0 (see CheckedBuffer)
RAB_LightInfo RAB_LoadLightInfo(uint index, bool previousFrame)
{
    if (index >= gRtxdiBridge.lightCount)
    {
        return RAB_EmptyLightInfo();
    }
    const uint64_t rows =
        previousFrame && gRtxdiBridge.prevWorldLights != 0 ? gRtxdiBridge.prevWorldLights : gRtxdiBridge.worldLights;
    return bdaPtr<GpuLight>(rows)[index];
}

// light rows keep their index across frames until a load or unload, which resets history
int RAB_TranslateLightIndex(uint lightIndex, bool currentToPrevious)
{
    return int(lightIndex);
}

// punctual lights are deltas: uv is unused
RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo light, RAB_Surface surface, float2 uv)
{
    const LightSample evaluated = evalLight(light, surface.position);
    RAB_LightSample sample;
    sample.l = evaluated.l;
    sample.distance = evaluated.distance;
    sample.radiance = evaluated.radiance;
    return sample;
}

// no BRDF ray can hit a delta light, so RTXDI skips light/BRDF MIS for these
bool RAB_IsAnalyticLightSample(RAB_LightSample sample)
{
    return true;
}

float RAB_LightSampleSolidAnglePdf(RAB_LightSample sample)
{
    return 0.0;
}

void RAB_GetLightDirDistance(RAB_Surface surface, RAB_LightSample sample, out float3 direction, out float distance)
{
    direction = sample.l;
    distance = sample.distance;
}

// selection pdf of a local light hit by a BRDF ray: uniform until Power_RIS's PDF texture (M7)
float RAB_EvaluateLocalLightSourcePdf(uint lightIndex)
{
    return gRtxdiBridge.localLightCount > 0 ? 1.0 / float(gRtxdiBridge.localLightCount) : 0.0;
}

float2 RAB_GetEnvironmentMapRandXYFromDir(float3 direction)
{
    return float2(0.0, 0.0);
}

float RAB_EvaluateEnvironmentMapSamplingPdf(float3 direction)
{
    return 0.0;
}

// evalBrdf · L_i · NdotL: the target function's color, and the shading of a resampled sample
float3 shadeLightSample(RAB_Surface surface, RAB_LightSample sample)
{
    if (!RAB_IsSurfaceValid(surface) || !surface.isLit)
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float nDotL = dot(surface.normal, sample.l);
    if (nDotL <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }
    return evalBrdf(surface.normal, surface.diffuseAlbedo, surface.specularF0, surface.roughness, surface.viewDir,
                    sample.l) *
           sample.radiance * nDotL;
}

float RAB_GetLightSampleTargetPdfForSurface(RAB_LightSample sample, RAB_Surface surface)
{
    return luminance(shadeLightSample(surface, sample));
}

//
// visibility: traceVisibility from the guide surface
//

// raster surfaces carry no traced triangle, so safeSpawnPoint does not apply: the reference offset of
// spec/features/ray_tracing_mega_geometry.md §6.1 (offsetRay), along the geometric normal on the ray's side, scaled
// by the larger of the position's magnitude and its view depth (the resolve's reconstruction error grows with both)
static const float kRasterOriginScale = 1.0 / 65536.0;

float3 rasterRayOrigin(RAB_Surface surface, float3 direction)
{
    const float side = dot(surface.geoNormal, direction) < 0.0 ? -1.0 : 1.0;
    const float scale = kRasterOriginScale * max(length(surface.position), surface.viewDepth);
    return surface.position + surface.geoNormal * (side * scale);
}

bool RAB_GetConservativeVisibility(RAB_Surface surface, RAB_LightSample sample)
{
    if (!RAB_IsSurfaceValid(surface))
    {
        return false;
    }
    return traceVisibility(gRtxdiBridge.tlas, rasterRayOrigin(surface, sample.l), sample.l, 0.0, sample.distance);
}

// from last frame's surface through this frame's TLAS: there is no previous TLAS to trace
bool RAB_GetTemporalConservativeVisibility(RAB_Surface currentSurface, RAB_Surface previousSurface,
                                           RAB_LightSample sample)
{
    return RAB_GetConservativeVisibility(previousSurface, sample);
}

// BRDF rays: until emissive triangles (M7) no geometry is a light, so a hit never names one
bool RAB_TraceRayForLocalLight(float3 origin, float3 direction, float tMin, float tMax, out uint lightIndex,
                               out float2 randXY)
{
    lightIndex = RTXDI_InvalidLightIndex;
    randXY = float2(0.0, 0.0);
    return !traceVisibility(gRtxdiBridge.tlas, origin, direction, tMin, tMax);
}

#endif
