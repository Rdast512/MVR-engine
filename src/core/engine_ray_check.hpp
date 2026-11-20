#pragma once

#include "types.hpp"

#include <cstdint>

// ray-tracing check (PBR plan C3): every frame, passes/rt_check.slang traces each pixel's primary ray and compares it
// with the vis buffer. Passive: the camera and the scene move as another runner scripts them (ENGINE_CULL_CHECK poses,
// ENGINE_GUIDE_CAPTURE model swaps, ENGINE_MOTION_TEST motion); alone it checks the startup view for a fixed number of
// frames. ENGINE_RT_CHECK=1 enables it; logSummary reports the run when the engine quits. The SER comparison
// (passes/rt_ser_check.slang) is shelved with SER (considerations C-G1-3)
class EngineRayCheck
{
public:
    [[nodiscard]] static bool isRequested();

    // after the frame's fence; true once a standalone run has checked all its frames
    [[nodiscard]] bool endFrame(const GpuRayCheckStats& stats, bool isStandalone);
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
    // misses, id mismatches and empty hits next to an alpha cut (spec/m6/d4_ray_contract.md): logged, not failures
    uint64_t alphaEdges = 0;
    float maxNormalError = 0.0f;
    float maxGeoNormalError = 0.0f;
    float maxPositionError = 0.0f;
    float maxMipError = 0.0f;
    float maxMaterialNormalError = 0.0f;
    float maxHitNormalError = 0.0f;
    float maxHitMaterialNormalError = 0.0f;
};
