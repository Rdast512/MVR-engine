#include "engine_gi_check.hpp"
#include "../static_headers/logger.hpp"
#include "convergence_metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <string_view>
#include <utility>

bool EngineGiCheck::isRequested()
{
    const char* text = std::getenv("ENGINE_GI_CHECK");
    return text != nullptr && std::string_view(text) == "1";
}

void EngineGiCheck::applyPhase(RtxdiContext& rtxdi) const
{
    switch (phase) {
    case Phase::GroundTruth:
    case Phase::NoiseFloor:
        rtxdi.giReferenceBounces = RtxdiContext::kGiGroundTruthBounces;
        rtxdi.isGiReference = true;
        rtxdi.giSettings = RtxdiContext::convergedGiSettings();
        break;
    case Phase::Reuse:
        rtxdi.giReferenceBounces = RtxdiContext::kGiGroundTruthBounces;
        rtxdi.isGiReference = false;
        rtxdi.giSettings = RtxdiContext::convergedGiSettings();
        break;
    case Phase::Pipeline:
        rtxdi.giReferenceBounces = 0;
        rtxdi.isGiReference = false;
        rtxdi.giSettings = RtxdiContext::convergedGiSettings();
        break;
    case Phase::Defaults:
        rtxdi.giReferenceBounces = 0;
        rtxdi.isGiReference = false;
        rtxdi.giSettings = RtxdiContext::defaultGiSettings();
        break;
    case Phase::Count:
        break;
    }
}

EngineGiCheck::Request EngineGiCheck::beginFrame(Camera& camera, RtxdiContext& rtxdi, DebugViewSettings& debugView)
{
    Request request;
    if (isFinished || frame < kStartupFrames) {
        return request;
    }
    debugView.view = DebugView::IndirectRadiance;
    if (phaseFrame == 0) {
        const Pose& pose = kPoses[poseIndex];
        if (phase == Phase::GroundTruth && !pose.isStartup) {
            camera.lookAt(pose.position, pose.target);
        }
        applyPhase(rtxdi);
    }
    isCaptureRequested = phaseFrame >= kPhases[std::to_underlying(phase)].warmupFrames;
    request.shouldCapture = isCaptureRequested;
    return request;
}

bool EngineGiCheck::endFrame(const std::optional<Renderer::DisplayCapture>& capture)
{
    if (isFinished) {
        return true;
    }
    if (frame < kStartupFrames) {
        ++frame;
        return false;
    }
    ++frame;
    if (isCaptureRequested) {
        if (!capture || (!sum.empty() && capture->extent != extent)) {
            isCaptureMissing = true;
        } else {
            const std::vector<float> luminance = decodeLuminance(*capture);
            if (sum.empty()) {
                extent = capture->extent;
                sum.assign(luminance.size(), 0.0);
                squaredSum.assign(luminance.size(), 0.0);
            }
            const bool isGroundTruth = phase == Phase::GroundTruth;
            for (size_t texel = 0; texel < luminance.size(); ++texel) {
                const double value = luminance[texel];
                const double error = isGroundTruth ? value : value - groundTruth[texel];
                sum[texel] += value;
                squaredSum[texel] += error * error;
            }
            ++capturedFrames;
        }
    }
    const PhaseInfo& info = kPhases[std::to_underlying(phase)];
    if (++phaseFrame < info.warmupFrames + info.frames) {
        return false;
    }
    return finishPhase();
}

bool EngineGiCheck::evaluatePhase() const
{
    const PhaseInfo& info = kPhases[std::to_underlying(phase)];
    const std::string label = std::format("ReSTIR GI convergence, {}, {}", kPoses[poseIndex].name, info.name);
    if (isCaptureMissing || capturedFrames == 0 || groundTruth.size() != sum.size()) {
        logError(std::format("FAIL {}: capture missing or resized", label), "GiCheck");
        return false;
    }
    const double frames = capturedFrames;
    std::vector<double> mean(sum.size());
    double meanTotal = 0.0;
    double groundTruthTotal = 0.0;
    double rmsTotal = 0.0;
    for (size_t texel = 0; texel < sum.size(); ++texel) {
        mean[texel] = sum[texel] / frames;
        meanTotal += mean[texel];
        groundTruthTotal += groundTruth[texel];
        rmsTotal += std::sqrt(squaredSum[texel] / frames);
    }
    if (groundTruthTotal <= 0.0) {
        logError(std::format("FAIL {}: the ground truth is black", label), "GiCheck");
        return false;
    }
    const TileErrors tiles = compareTiles(mean, groundTruth, extent, kTileSize);
    const double bias = (meanTotal - groundTruthTotal) / groundTruthTotal;
    const std::string detail = std::format(
        "{} frames after {}: bias {:+.2f}%, {}x{} tiles median {:.2f}% / p90 {:.2f}% / max {:.2f}% ({} tiles), "
        "per-frame RMS error {:.1f}% against the ground truth's 1-sample noise {:.1f}%",
        capturedFrames, info.warmupFrames, bias * 100.0, kTileSize, kTileSize, tiles.median * 100.0,
        tiles.p90 * 100.0, tiles.max * 100.0, tiles.count, rmsTotal / groundTruthTotal * 100.0,
        groundTruthNoise * 100.0);
    if (!isGated()) {
        logInfo(std::format("MEASURED {} ({})", label, detail), "GiCheck");
        return true;
    }
    const bool isPassed = std::abs(bias) <= info.maxBias && tiles.p90 <= info.maxTileP90;
    if (isPassed) {
        logInfo(std::format("PASS {} ({}; limits {:.1f}% / {:.1f}%)", label, detail, info.maxBias * 100.0,
                            info.maxTileP90 * 100.0),
                "GiCheck");
    } else {
        logError(std::format("FAIL {} ({}; limits {:.1f}% / {:.1f}%)", label, detail, info.maxBias * 100.0,
                             info.maxTileP90 * 100.0),
                 "GiCheck");
    }
    return isPassed;
}

bool EngineGiCheck::finishPhase()
{
    if (phase == Phase::GroundTruth) {
        const double frames = capturedFrames;
        groundTruth.assign(sum.size(), 0.0);
        double total = 0.0;
        double noise = 0.0;
        for (size_t texel = 0; texel < sum.size() && frames > 0.0; ++texel) {
            const double mean = sum[texel] / frames;
            groundTruth[texel] = mean;
            total += mean;
            noise += std::sqrt(std::max(squaredSum[texel] / frames - mean * mean, 0.0));
        }
        groundTruthNoise = total > 0.0 ? noise / total : 0.0;
        logInfo(std::format("ReSTIR GI ground truth, {}: {} frames, {} bounces after X2, summed luminance {:.4g}, "
                            "1-sample noise {:.1f}%",
                            kPoses[poseIndex].name, capturedFrames, RtxdiContext::kGiGroundTruthBounces, total,
                            groundTruthNoise * 100.0),
                "GiCheck");
    } else {
        const bool isPassed = evaluatePhase();
        if (isGated()) {
            ++checkCount;
            passedCount += isPassed ? 1u : 0u;
        }
    }
    sum.clear();
    squaredSum.clear();
    capturedFrames = 0;
    phaseFrame = 0;
    isCaptureMissing = false;
    phase = static_cast<Phase>(std::to_underlying(phase) + 1);
    if (phase != Phase::Count) {
        return false;
    }
    phase = Phase::GroundTruth;
    if (++poseIndex < kPoses.size()) {
        return false;
    }
    const std::string summary = std::format("ReSTIR GI convergence: {} of {} passed", passedCount, checkCount);
    if (passedCount == checkCount) {
        logInfo(summary, "GiCheck");
    } else {
        logError(summary, "GiCheck");
    }
    isFinished = true;
    return true;
}
