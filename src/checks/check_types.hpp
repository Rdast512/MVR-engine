#pragma once

#include "render/push_data.hpp"

#include <cstddef>
#include <cstdint>

// GPU structs of the check passes; mirrored in shaders/checks/check_types.slang (layout_test)

// rt_ser_check pass, ENGINE_RT_CHECK only (spec/m4/g1_ser_rt_pipeline.md §4): the SER trace against traceClosest on
// the same primary ray, one per reorder mode; zeroed by the CPU per frame
struct GpuSerCheckStats
{
    uint32_t rays = 0;
    // traceClosest hits
    uint32_t hits = 0;
    // one of the two hits, the other misses
    uint32_t hitMismatches = 0;
    // another (ray instance, meshlet, triangle)
    uint32_t idMismatches = 0;
    // same ids, other barycentrics, hit distance, positions or transforms
    uint32_t dataMismatches = 0;
    // float bits
    uint32_t maxBarycentricErrorBits = 0;
    // float bits, relative to the hit distance
    uint32_t maxDistanceErrorBits = 0;
};
static_assert(sizeof(GpuSerCheckStats) == 28);
static_assert(offsetof(GpuSerCheckStats, maxDistanceErrorBits) == 24);

// motion_check pass, ENGINE_MOTION_TEST only (spec/features/done/motion_vectors.md §5); zeroed by the CPU per frame
struct GpuMotionCheckStats
{
    uint32_t pixels = 0;
    uint32_t emptyPixels = 0;
    // motion != 0
    uint32_t movingPixels = 0;
    // |motion - camera-only motion from depth| over tolerance; meaningful only while every entity is still
    uint32_t cameraMismatches = 0;
    // albedo differs from last frame's at the reprojected position, and at the same position
    uint32_t reprojectedErrors = 0;
    uint32_t stillErrors = 0;
    // reprojected outside last frame's image
    uint32_t offscreenPixels = 0;
    uint32_t frameFlags = 0;
    // float bits: non-negative floats order like their uint bits
    uint32_t maxMotionBits = 0;
    uint32_t maxCameraErrorBits = 0;
};
static_assert(sizeof(GpuMotionCheckStats) == 40);
static_assert(offsetof(GpuMotionCheckStats, maxMotionBits) == 32);

// rt_check pass, ENGINE_RT_CHECK only (plan C3): primary rays against the vis buffer; zeroed by the CPU per frame
struct GpuRayCheckStats
{
    uint32_t coveredPixels = 0;
    // covered, and the 4 neighbours hold the same payload: no triangle edge through the pixel
    uint32_t interiorPixels = 0;
    // interior, the primary ray hits nothing
    uint32_t misses = 0;
    // interior, the ray hits another (node instance, meshlet, triangle)
    uint32_t idMismatches = 0;
    // the ray hits a back face of a one-sided primitive, which raster culls: expected, counted apart
    uint32_t backFaceHits = 0;
    // covered but not interior: tie-breaking along triangle edges differs between raster and rays
    uint32_t edgeMismatches = 0;
    // empty pixel with empty neighbours, the ray hits something
    uint32_t emptyHits = 0;
    // a ray spawned from a matched hit with safeSpawnPoint hits its own triangle
    uint32_t selfHits = 0;
    // matched, ray-cone and raster uv footprints more than half a mip apart
    uint32_t mipOutliers = 0;
    // interior, the ray hits another triangle within intersection noise of the raster one: overlapping coplanar
    // triangles, where the depth test and the ray each pick one
    uint32_t depthTies = 0;
    // float bits: non-negative floats order like their uint bits. The decode at the resolve's barycentrics and
    // gradients, in degrees: any difference is the decode chain's
    uint32_t maxNormalErrorBits = 0;
    uint32_t maxGeoNormalErrorBits = 0;
    // pixels between the ray and raster intersections on screen
    uint32_t maxPositionErrorBits = 0;
    // |log2| of the ray-cone over the raster uv footprint, uv set 0
    uint32_t maxMipErrorBits = 0;
    uint32_t maxMaterialNormalErrorBits = 0;
    // the same at the ray's own barycentrics: intersection noise through steep normal maps, reported only
    uint32_t maxHitNormalErrorBits = 0;
    uint32_t maxHitMaterialNormalErrorBits = 0;
    // a miss, id mismatch or empty hit where an alpha-tested triangle's alpha crosses its threshold within one OMM
    // micro-triangle plus the raster footprint: the OMM and the per-texel test may disagree there (spec/m6/d4)
    uint32_t alphaEdges = 0;
};
static_assert(sizeof(GpuRayCheckStats) == 72);
static_assert(offsetof(GpuRayCheckStats, maxNormalErrorBits) == 40);
static_assert(offsetof(GpuRayCheckStats, alphaEdges) == 68);

// motion_check pass
struct MotionCheckPushData {
    vk::DeviceAddress frameRoot;
    // GpuMotionCheckStats
    vk::DeviceAddress stats;
};
static_assert(sizeof(MotionCheckPushData) == 16);

// rt_check pass
struct RayCheckPushData {
    vk::DeviceAddress frameRoot;
    // GpuRayCheckStats
    vk::DeviceAddress stats;
};
static_assert(sizeof(RayCheckPushData) == 16);

// rt_ser_check pass
struct SerCheckPushData {
    vk::DeviceAddress frameRoot;
    // GpuSerCheckStats of the pipeline's reorder mode
    vk::DeviceAddress stats;
};
static_assert(sizeof(SerCheckPushData) == 16);

// the renderer checks kMaxPushDataBytes against maxPushDataSize at startup; the check passes stay under it
static_assert(sizeof(MotionCheckPushData) <= kMaxPushDataBytes && sizeof(RayCheckPushData) <= kMaxPushDataBytes &&
              sizeof(SerCheckPushData) <= kMaxPushDataBytes);
