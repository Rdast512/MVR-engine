#pragma once

#include "types.hpp"

#include <array>
#include <cstdint>
#include <span>

// ray-tracing check (PBR plan C3): every frame, passes/rt_check.slang traces each pixel's primary ray and compares it
// with the vis buffer. Passive: the camera and the scene move as another runner scripts them (ENGINE_CULL_CHECK poses,
// ENGINE_GUIDE_CAPTURE model swaps, ENGINE_MOTION_TEST motion); alone it checks the startup view for a fixed number of
// frames. The same frames trace the primary rays through the SER pipeline too (passes/rt_ser_check.slang), reorder off
// and on, which must match traceClosest bit for bit (spec/m4/g1_ser_rt_pipeline.md §4). ENGINE_RT_CHECK=1 enables it;
// logSummary reports the run when the engine quits
class EngineRayCheck
{
public:
    [[nodiscard]] static bool isRequested();

    // after the frame's fence; true once a standalone run has checked all its frames
    // serStats indexed by isReorderEnabled
    [[nodiscard]] bool endFrame(const GpuRayCheckStats& stats, std::span<const GpuSerCheckStats, 2> serStats,
                                bool isStandalone);
    void logSummary() const;

private:
    // the startup flush and the first TLAS build
    static constexpr uint32_t kWarmupFrames = 2;
    static constexpr uint32_t kStandaloneFrames = 32;
    static constexpr uint32_t kMaxLoggedFailures = 16;
    // the decode at the resolve's exact barycentrics and gradients: same inputs, so the same bits (0.0000 measured)
    static constexpr float kMaxNormalErrorDegrees = 1e-4f;
    // ray and raster intersections of the same triangle, in pixels on screen. Hardware hit barycentrics move the
    // point up to 0.06 px on large Sponza triangles; a wrong pixel center or an unsubtracted jitter is 0.5 px off
    static constexpr float kMaxPositionError = 0.1f;

    [[nodiscard]] static bool isPassing(const GpuRayCheckStats& stats);
    [[nodiscard]] static bool isPassing(const GpuSerCheckStats& stats);

    // the SER run of one reorder mode over all checked frames
    struct SerTotals
    {
        uint64_t rays = 0;
        uint64_t hits = 0;
        uint64_t mismatches = 0;
        uint32_t passedFrames = 0;
        float maxBarycentricError = 0.0f;
        float maxDistanceError = 0.0f;
    };

    uint32_t frame = 0;
    uint32_t checkedFrames = 0;
    uint32_t passedFrames = 0;
    uint32_t loggedFailures = 0;
    uint64_t interiorPixels = 0;
    uint64_t edgeMismatches = 0;
    uint64_t edgePixels = 0;
    uint64_t backFaceHits = 0;
    uint64_t mipOutliers = 0;
    uint64_t depthTies = 0;
    float maxNormalError = 0.0f;
    float maxGeoNormalError = 0.0f;
    float maxPositionError = 0.0f;
    float maxMipError = 0.0f;
    float maxMaterialNormalError = 0.0f;
    float maxHitNormalError = 0.0f;
    float maxHitMaterialNormalError = 0.0f;
    std::array<SerTotals, 2> serTotals{};
};
