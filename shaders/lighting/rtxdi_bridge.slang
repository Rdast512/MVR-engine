#ifndef ENGINE_LIGHTING_RTXDI_BRIDGE_SLANG
#define ENGINE_LIGHTING_RTXDI_BRIDGE_SLANG

// RTXDI application bridge (PBR plan F2): the resource macros and the RAB_* set the pinned RTXDI 3.2 DI and GI headers
// call (GI: M4 G2, G5).
// Textual, once per compile unit, before any Rtxdi/DI header; call rtxdiBridgeInit before the first RAB_* or RTXDI_*
// call. Light and BRDF math stays in lighting.lights and material.brdf: the bridge only composes them.
//
// Surfaces come from the guide sets: current or previous by frame parity, so a previous-frame surface is last frame's
// resolve output (position guide, normals, albedos, camera position). Lights are E2's world rows, last frame's for
// previous-frame loads; light index i is row i in both frames (RAB_TranslateLightIndex is the identity), and a load or
// unload resets history. Point and spot rows are deltas; triangle rows (spec/m7/e2_emissive_triangles.md) take a
// uniform area sample, and a BRDF ray names the triangle light it hits.
//
// Local lights are presampled (spec/m7/f1_light_presampling.md): RtxdiContext's RIS buffer holds the Power_RIS tiles
// and ReGIR's cells, each entry with its compact light (lighting.compact_lights), and the local light PDF weighs a BRDF
// candidate against the light candidates.
//
// Not built yet: the environment light. lightPresent stays 0, so its two functions are never reached

import types;
import core.binding;
import core.frame;
import core.math;
import lighting.compact_lights;
import lighting.lights;
import lighting.triangle_lights;
import geometry.hit;
import material.brdf;
import material.pbr;
import raytracing.trace;

// before any RTXDI header. ReGIR's layout is chosen by macro (GetRegirMacro in the RTXDI sample); RtxdiContext keeps
// the SDK's static parameters, whose mode is Onion
#define RTXDI_ENABLE_PRESAMPLING 1
#define RTXDI_REGIR_MODE RTXDI_REGIR_ONION

// RTXDI's buffers live in RtxdiContext's allocations (src/render/vk_rtxdi.cpp). Neighbor offsets are always indexed
// through neighborOffsetMask, so a plain BDA pointer serves
#define RTXDI_LIGHT_RESERVOIR_BUFFER gRtxdiBridge.reservoirs
#define RTXDI_GI_RESERVOIR_BUFFER gRtxdiBridge.giReservoirs
#define RTXDI_NEIGHBOR_OFFSETS_BUFFER bdaPtr<float2>(gRtxdiBridge.neighborOffsets)
#define RTXDI_RIS_BUFFER gRtxdiBridge.risBuffer

// RTXDI's HLSL converts uint to bool implicitly (E30081): units that include this file compile with
// RTXDI_SLANG_FLAGS (CMakeLists.txt). #pragma warning cannot scope it, Slang 2026.20 ignores it once a unit imports
#include "lighting/rtxdi_constants.slang"
#include "Rtxdi/RtxdiParameters.h"
#include "Rtxdi/Utils/Math.hlsli"
#include "Rtxdi/Utils/RandomSamplerState.hlsli"

// RTXDI's ReGIR functions take ReGIR_Parameters by value and index the onion's layers and rings dynamically. Loaded
// through BDA, the whole 1.3 KB struct is converted to its logical layout and every thread keeps it in local memory:
// 1.4 ms ReGIR build and 1.2 ms initial sampling at 5,990 local lights (RTX 5070). The view takes its place in RTXDI's
// signatures and reads each field through the pointer, as the RTXDI sample's constant buffer does
struct RegirOnionLayersView
{
    GpuRtxdiConstants* constants;
    __subscript(int index)->ReGIR_OnionLayerGroup { get { return constants->regir.onionParams.layers[index]; } }
    __subscript(uint index)->ReGIR_OnionLayerGroup { get { return constants->regir.onionParams.layers[index]; } }
};

struct RegirOnionRingsView
{
    GpuRtxdiConstants* constants;
    __subscript(int index)->ReGIR_OnionRing { get { return constants->regir.onionParams.rings[index]; } }
    __subscript(uint index)->ReGIR_OnionRing { get { return constants->regir.onionParams.rings[index]; } }
};

struct RegirOnionView
{
    GpuRtxdiConstants* constants;
    property layers : RegirOnionLayersView { get { return RegirOnionLayersView(constants); } }
    property rings : RegirOnionRingsView { get { return RegirOnionRingsView(constants); } }
    property numLayerGroups : uint { get { return constants->regir.onionParams.numLayerGroups; } }
    property cubicRootFactor : float { get { return constants->regir.onionParams.cubicRootFactor; } }
    property linearFactor : float { get { return constants->regir.onionParams.linearFactor; } }
};

struct RegirParametersView
{
    GpuRtxdiConstants* constants;
    property commonParams : ReGIR_CommonParameters { get { return constants->regir.commonParams; } }
    property gridParams : ReGIR_GridParameters { get { return constants->regir.gridParams; } }
    property onionParams : RegirOnionView { get { return RegirOnionView(constants); } }
};

// every RTXDI header after this point; ReGIRParameters.h, already included, keeps the real struct for the constants
#define ReGIR_Parameters RegirParametersView

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
    RayScene rays;
    uint64_t worldLights;
    uint64_t prevWorldLights;
    uint lightCount;
    CheckedBuffer<RTXDI_PackedDIReservoir> reservoirs;
    CheckedBuffer<RTXDI_PackedGIReservoir> giReservoirs;
    uint64_t neighborOffsets;
    uint localLightCount;
    TriangleLightTable triangleLights;
    ReSTIRDI_LocalLightSamplingMode localLightSamplingMode;
    CheckedBuffer<uint2> risBuffer;
    // two rows per RIS entry
    CheckedBuffer<uint4> risLightData;
    // resource heap; kNoneIndex while there is no local light
    uint lightPdf;
    uint lightPdfSide;
    // GpuRtxdiConstants, for regirParameters
    uint64_t rtxdiConstants;
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
    gRtxdiBridge.rays = loadRayScene(root);
    gRtxdiBridge.worldLights = root.worldLights;
    gRtxdiBridge.prevWorldLights = root.prevWorldLights;
    gRtxdiBridge.lightCount = root.lightCount;
    gRtxdiBridge.reservoirs.address = constants[0].reservoirs;
    gRtxdiBridge.reservoirs.count = constants[0].reservoirCount;
    gRtxdiBridge.giReservoirs.address = constants[0].giReservoirs;
    gRtxdiBridge.giReservoirs.count = constants[0].giReservoirCount;
    gRtxdiBridge.neighborOffsets = constants[0].neighborOffsets;
    gRtxdiBridge.localLightCount = constants[0].lightBuffer.localLightBufferRegion.numLights;
    gRtxdiBridge.triangleLights = loadTriangleLightTable(root);
    gRtxdiBridge.localLightSamplingMode = constants[0].initialSampling.localLightSamplingMode;
    gRtxdiBridge.risBuffer.address = constants[0].risBuffer;
    gRtxdiBridge.risBuffer.count = constants[0].risEntryCount;
    gRtxdiBridge.risLightData.address = constants[0].risLightData;
    gRtxdiBridge.risLightData.count = constants[0].risEntryCount * 2;
    gRtxdiBridge.lightPdf = constants[0].lightPdf;
    gRtxdiBridge.lightPdfSide = constants[0].lightPdfSize.x;
    gRtxdiBridge.rtxdiConstants = root.rtxdiConstants;
}

// ReGIR's parameters for RTXDI's sampling functions, read in place
RegirParametersView regirParameters()
{
    return RegirParametersView(bdaPtr<GpuRtxdiConstants>(gRtxdiBridge.rtxdiConstants));
}

// the local light PDF; only while there is a local light (lightPdf is a heap index then)
Texture2D lightPdfTexture()
{
    return tex2D(gRtxdiBridge.lightPdf);
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
// lights: E2's world rows; point, spot and directional through evalLight, triangles through lighting.triangle_lights
//

typealias RAB_LightInfo = GpuLight;

// a light at the surface it was sampled for; RTXDI always scores a sample against that same surface
struct RAB_LightSample
{
    float3 l;        // unit, toward the light
    float distance;  // visibility tMax; kInfiniteDistance for directional
    // incident, before NdotL; an area sample's is radiance / solidAnglePdf, so the shading and the target function
    // weigh it as the RTXDI sample's bridge does
    float3 radiance;
    float3 position; // world position for local lights; unused when distance >= kInfiniteDistance
    float solidAnglePdf; // area samples; 0 for deltas and for an area sample that carries no light
};

// an area sample's visibility ray ends this share of the distance short of the light, which lies on a triangle the
// ray would otherwise hit
static const float kLightSampleShortening = 1.0e-3;

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

// a presampled RIS entry's light (lighting.compact_lights); the entry was stored this frame
RAB_LightInfo RAB_LoadCompactLightInfo(uint linearIndex)
{
    return unpackCompactLight(gRtxdiBridge.risLightData[linearIndex * 2],
                              gRtxdiBridge.risLightData[linearIndex * 2 + 1]);
}

// false: the light keeps no compact form (spot), RTXDI loads its world row instead
bool RAB_StoreCompactLightInfo(uint linearIndex, RAB_LightInfo light)
{
    uint4 first;
    uint4 second;
    if (!packCompactLight(light, first, second))
    {
        return false;
    }
    gRtxdiBridge.risLightData[linearIndex * 2] = first;
    gRtxdiBridge.risLightData[linearIndex * 2 + 1] = second;
    return true;
}

// ReGIR's cell weight of a light for every surface in the sphere; infinite lights take no part in it
float RAB_GetLightTargetPdfForVolume(RAB_LightInfo light, float3 volumeCenter, float volumeRadius)
{
    if (light.type == LightType.Triangle)
    {
        return triangleLightWeightForVolume(asTriangleLight(light), volumeCenter, volumeRadius);
    }
    if (light.type == LightType.Directional)
    {
        return 0.0;
    }
    return punctualLightWeightForVolume(light, volumeCenter, volumeRadius);
}

// light rows keep their index across frames until a load or unload, which resets history
int RAB_TranslateLightIndex(uint lightIndex, bool currentToPrevious)
{
    return int(lightIndex);
}

// triangles: the uniform area sample uv maps to; punctual lights are deltas, uv is unused
RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo light, RAB_Surface surface, float2 uv)
{
    RAB_LightSample sample;
    if (light.type == LightType.Triangle)
    {
        const TriangleLightSample area = sampleTriangleLight(asTriangleLight(light), surface.position, uv);
        sample.l = area.l;
        sample.distance = area.distance * (1.0 - kLightSampleShortening);
        sample.radiance = area.solidAnglePdf > 0.0 ? area.radiance / area.solidAnglePdf : float3(0.0, 0.0, 0.0);
        sample.position = area.position;
        sample.solidAnglePdf = area.solidAnglePdf;
        return sample;
    }
    const LightSample evaluated = evalLight(light, surface.position);
    sample.l = evaluated.l;
    sample.distance = evaluated.distance;
    sample.radiance = evaluated.radiance;
    sample.position = light.position;
    sample.solidAnglePdf = 0.0;
    return sample;
}

// no BRDF ray can hit a delta light, so RTXDI skips light/BRDF MIS for these; an area sample without light has no
// solid-angle pdf to blend
bool RAB_IsAnalyticLightSample(RAB_LightSample sample)
{
    return sample.solidAnglePdf <= 0.0;
}

float RAB_LightSampleSolidAnglePdf(RAB_LightSample sample)
{
    return sample.solidAnglePdf;
}

void RAB_GetLightDirDistance(RAB_Surface surface, RAB_LightSample sample, out float3 direction, out float distance)
{
    direction = sample.l;
    distance = sample.distance;
}

// selection pdf of a local light hit by a BRDF ray, the light candidates' MIS weight against it. Presampled modes: its
// power over the total, as the RTXDI sample (the PDF texel over the top mip's mean times the texel count). Uniform:
// 1 / count, which the sample would take from the PDF too, biasing the MIS weights of a uniform light candidate
float RAB_EvaluateLocalLightSourcePdf(uint lightIndex)
{
    if (lightIndex >= gRtxdiBridge.localLightCount)
    {
        return 0.0;
    }
    if (gRtxdiBridge.localLightSamplingMode == ReSTIRDI_LocalLightSamplingMode_UNIFORM)
    {
        return 1.0 / float(gRtxdiBridge.localLightCount);
    }
    const Texture2D pdf = lightPdfTexture();
    const float power = pdf.Load(int3(RTXDI_LinearIndexToZCurve(lightIndex), 0)).r;
    const uint side = gRtxdiBridge.lightPdfSide;
    const float totalPower = pdf.Load(int3(0, 0, firstbithigh(side))).r * float(side * side);
    return totalPower > 0.0 ? power / totalPower : 0.0;
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
    return traceVisibility(gRtxdiBridge.rays, rasterRayOrigin(surface, sample.l), sample.l, 0.0, sample.distance);
}

// no previous TLAS: trace from previousSurface through this frame's TLAS. Under permutation sampling the temporal
// surface is another pixel, and a ray from currentSurface tests the wrong domain (m8 O2, §2.7)
bool RAB_GetTemporalConservativeVisibility(RAB_Surface currentSurface, RAB_Surface previousSurface,
                                           RAB_LightSample sample)
{
    if (!RAB_IsSurfaceValid(previousSurface))
    {
        return false;
    }
    if (sample.distance >= kInfiniteDistance)
    {
        return traceVisibility(gRtxdiBridge.rays, rasterRayOrigin(previousSurface, sample.l), sample.l, 0.0,
                               kInfiniteDistance);
    }
    const float3 toLight = sample.position - previousSurface.position;
    const float distance = length(toLight);
    if (distance <= 0.0)
    {
        return true;
    }
    const float3 l = toLight / distance;
    const float shortening = sample.solidAnglePdf > 0.0 ? kLightSampleShortening : 0.0;
    return traceVisibility(gRtxdiBridge.rays, rasterRayOrigin(previousSurface, l), l, 0.0,
                           distance * (1.0 - shortening));
}

// BRDF rays (BrdfRayTracing in the RTXDI sample): the closest hit names its triangle light, and randXY replays the hit
// point through RAB_SamplePolymorphicLight
bool RAB_TraceRayForLocalLight(float3 origin, float3 direction, float tMin, float tMax, out uint lightIndex,
                               out float2 randXY)
{
    lightIndex = RTXDI_InvalidLightIndex;
    randXY = float2(0.0, 0.0);
    // no hit can name a light: the first hit answers whether anything blocks the ray, at a fraction of the cost
    if (gRtxdiBridge.triangleLights.lightCount == 0)
    {
        return !traceVisibility(gRtxdiBridge.rays, origin, direction, tMin, tMax);
    }
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = tMin;
    ray.TMax = tMax;
    RayHit hit;
    if (!traceClosest(gRtxdiBridge.rays, ray, hit))
    {
        return false;
    }
    lightIndex = hitLightIndex(gRtxdiBridge.triangleLights, hit);
    if (lightIndex == kNoneIndex)
    {
        lightIndex = RTXDI_InvalidLightIndex;
    }
    else
    {
        const float2 b = hit.barycentrics;
        randXY = triangleRandFromBarycentrics(float3(1.0 - b.x - b.y, b.x, b.y));
    }
    return true;
}

//
// ReSTIR GI (spec/m4/g5_restir_gi.md): a sample is a secondary surface X2 with its outgoing radiance toward the
// surface that traced it
//

// evalBrdf · Lo(X2) · NdotL toward the sample: the GI target function's color, and the final shading
float3 shadeGiSample(RAB_Surface surface, float3 samplePosition, float3 sampleRadiance)
{
    if (!RAB_IsSurfaceValid(surface) || !surface.isLit)
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 toSample = samplePosition - surface.position;
    const float distanceSq = dot(toSample, toSample);
    if (distanceSq <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 l = toSample * rsqrt(distanceSq);
    const float nDotL = dot(surface.normal, l);
    // match the initial bounce's geometric hemisphere check so reused samples cannot leak from behind normal-mapped triangles
    if (nDotL <= 0.0 || dot(surface.geoNormal, l) <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }
    return evalBrdf(surface.normal, surface.diffuseAlbedo, surface.specularF0, surface.roughness, surface.viewDir, l) *
           sampleRadiance * nDotL;
}

float RAB_GetGISampleTargetPdfForSurface(float3 samplePosition, float3 sampleRadiance, RAB_Surface surface)
{
    return luminance(shadeGiSample(surface, samplePosition, sampleRadiance));
}

// the RTXDI sample's bridge (RAB_SpatialHelpers.hlsli): a solid-angle ratio past 10x rejects the reuse, past 3x it is
// clamped. The clamp is a bias the GI check measures
bool RAB_ValidateGISampleWithJacobian(inout float jacobian)
{
    if (jacobian > 10.0 || jacobian < 0.1)
    {
        return false;
    }
    jacobian = clamp(jacobian, 1.0 / 3.0, 3.0);
    return true;
}

// the ray ends this share of the distance short of X2, which lies on the triangle that traced it
static const float kReconnectionShortening = 1.0e-3;

// from a guide surface to a sample point
bool isReconnectionVisible(RAB_Surface surface, float3 samplePosition)
{
    if (!RAB_IsSurfaceValid(surface))
    {
        return false;
    }
    const float3 toSample = samplePosition - surface.position;
    const float distance = length(toSample);
    if (distance <= 0.0)
    {
        return false;
    }
    const float3 l = toSample / distance;
    if (dot(surface.geoNormal, l) <= 0.0)
    {
        return false;
    }
    return traceVisibility(gRtxdiBridge.rays, rasterRayOrigin(surface, l), l, 0.0,
                           distance * (1.0 - kReconnectionShortening));
}

bool RAB_GetConservativeVisibility(RAB_Surface surface, float3 samplePosition)
{
    return isReconnectionVisible(surface, samplePosition);
}

// no previous TLAS: the current surface through this frame's TLAS, as the RTXDI sample without enablePrevTLAS
bool RAB_GetTemporalConservativeVisibility(RAB_Surface currentSurface, RAB_Surface previousSurface,
                                           float3 samplePosition)
{
    return isReconnectionVisible(currentSurface, samplePosition);
}

#endif
