#include "engine_gi_check.hpp"
#include "../static_headers/logger.hpp"
#include "check_images.hpp"
#include "convergence_metrics.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <string_view>
#include <system_error>
#include <utility>

bool EngineGiCheck::isRequested()
{
    const char* text = std::getenv("ENGINE_GI_CHECK");
    return text != nullptr && std::string_view(text) == "1";
}

uint32_t EngineGiCheck::readRunSeed()
{
    const char* text = std::getenv("ENGINE_GI_CHECK_SEED");
    if (text == nullptr) {
        return 0;
    }
    uint32_t seed = 0;
    const std::string_view value(text);
    if (std::from_chars(value.data(), value.data() + value.size(), seed).ec != std::errc{}) {
        logError(std::format("ENGINE_GI_CHECK_SEED='{}' is not a number; seed 0", value), "GiCheck");
        return 0;
    }
    logInfo(std::format("ReSTIR GI convergence: run seed {}", seed), "GiCheck");
    return seed;
}

void EngineGiCheck::applyPhase(RtxdiContext& rtxdi, IndirectMode& indirectMode) const
{
    const PhaseInfo& info = kPhases[std::to_underlying(phase)];
    indirectMode = info.mode;
    rtxdi.giReferenceBounces = info.referenceBounces;
    rtxdi.isGiReference = info.isGiReference;
    rtxdi.giSettings = giSettingsFor(info.settings);
}

EngineGiCheck::Request EngineGiCheck::beginFrame(Camera& camera, RtxdiContext& rtxdi, IndirectMode& indirectMode,
                                                 DebugViewSettings& debugView)
{
    Request request;
    if (isFinished) {
        rtxdi.frameIndexOverride.reset();
        return request;
    }
    if (frame < kStartupFrames) {
        return request;
    }
    debugView.view = DebugView::IndirectRadiance;
    if (phaseFrame == 0) {
        const CheckPose& pose = poses()[poseIndex];
        if (phase == Phase::GroundTruth && !pose.isStartup) {
            camera.lookAt(pose.position, pose.target);
        }
        applyPhase(rtxdi, indirectMode);
    }
    rtxdi.frameIndexOverride = runSeedBase + static_cast<uint32_t>(poseIndex) * kPoseSeedStride +
        static_cast<uint32_t>(std::to_underlying(phase)) * kPhaseSeedStride + phaseFrame;
    isCaptureRequested = phaseFrame >= kPhases[std::to_underlying(phase)].warmupFrames;
    request.shouldCapture = isCaptureRequested;
    return request;
}

bool EngineGiCheck::endFrame(const std::optional<DisplayCapture>& capture)
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
            const DecodedFrame decoded = decodeFrame(*capture);
            if (sum.empty()) {
                extent = capture->extent;
                sum.assign(decoded.luminance.size(), 0.0);
                squaredSum.assign(decoded.luminance.size(), 0.0);
            }
            const bool isGroundTruth = kPhases[std::to_underlying(phase)].role == Role::GroundTruth;
            for (size_t texel = 0; texel < decoded.luminance.size(); ++texel) {
                const double value = decoded.luminance[texel];
                const double error = isGroundTruth ? value : value - groundTruth[texel];
                sum[texel] += value;
                squaredSum[texel] += error * error;
            }
            for (size_t channel = 0; channel < 3; ++channel) {
                rgbSum[channel] += decoded.rgbTotal[channel];
            }
            nonFinitePixels += decoded.nonFinitePixels;
            negativePixels += decoded.negativePixels;
            ++capturedFrames;
        }
    }
    const PhaseInfo& info = kPhases[std::to_underlying(phase)];
    if (++phaseFrame < info.warmupFrames + info.frames) {
        return false;
    }
    return finishPhase();
}

bool EngineGiCheck::evaluatePhase()
{
    const PhaseInfo& info = kPhases[std::to_underlying(phase)];
    const std::string label = std::format("ReSTIR GI convergence, {}, {}", poses()[poseIndex].name, info.name);
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
    const std::array<double, 3> meanRgb{rgbSum[0] / frames, rgbSum[1] / frames, rgbSum[2] / frames};
    const std::array<double, 3> rgbBias = computeChannelBias(meanRgb, groundTruthRgb, groundTruthTotal);
    const double maxRgbBias = std::max({std::abs(rgbBias[0]), std::abs(rgbBias[1]), std::abs(rgbBias[2])});
    const TileErrors tiles = compareTiles(mean, groundTruth, extent, kTileSize);
    if (isCheckPngRequested()) {
        writeCheckLuminancePng(std::format("gi {} mean", label), extent, mean, pngScale);
        writeCheckErrorPng(std::format("gi {} error", label), extent, mean, groundTruth, pngScale);
    }
    const double bias = (meanTotal - groundTruthTotal) / groundTruthTotal;
    const uint64_t totalNonFinite = nonFinitePixels + groundTruthNonFinite;
    const uint64_t totalNegative = negativePixels + groundTruthNegative;
    const std::string detail = std::format(
        "{} frames after {}: bias {:+.2f}% (RGB {:+.2f}/{:+.2f}/{:+.2f}%), {}x{} tiles median {:.2f}% / p90 {:.2f}% / "
        "max {:.2f}% ({} tiles, dark leak {:.3f}% in {} tiles, dim bias {:+.2f}% in {} tiles), per-frame RMS error {:.1f}% "
        "against 1-sample noise {:.1f}%, nonFinite {} neg {}",
        capturedFrames, info.warmupFrames, bias * 100.0, rgbBias[0] * 100.0, rgbBias[1] * 100.0, rgbBias[2] * 100.0,
        kTileSize, kTileSize, tiles.median * 100.0, tiles.p90 * 100.0, tiles.max * 100.0, tiles.count,
        tiles.zeroRefLeak * 100.0, tiles.zeroRefLeakTiles, tiles.dimBias * 100.0, tiles.dimTiles,
        rmsTotal / groundTruthTotal * 100.0,
        groundTruthNoise * 100.0, totalNonFinite, totalNegative);
    if (!isGated()) {
        logInfo(std::format("MEASURED {} ({})", label, detail), "GiCheck");
        return true;
    }
    if (info.role == Role::NoiseFloor) {
        noiseFloorTileP90 = tiles.p90;
    }
    const double tileLimit = info.role == Role::Compared
        ? std::max(info.maxTileP90, kNoiseFloorTileFactor * noiseFloorTileP90)
        : info.maxTileP90;
    const double rgbLimit = maxBias() * kRgbBiasFactor;
    const bool isPassed = totalNonFinite == 0 && totalNegative == 0 && std::abs(bias) <= maxBias() &&
        maxRgbBias <= rgbLimit && tiles.zeroRefLeak <= maxBias() &&
        (tileLimit <= 0.0 || tiles.p90 <= tileLimit);
    const std::string limits = tileLimit > 0.0
        ? std::format("limits {:.1f}% / RGB {:.2f}% / p90 {:.1f}%", maxBias() * 100.0, rgbLimit * 100.0,
                      tileLimit * 100.0)
        : std::format("limits {:.1f}% / RGB {:.2f}%, tiles not gated", maxBias() * 100.0, rgbLimit * 100.0);
    if (isPassed) {
        logInfo(std::format("PASS {} ({}; {})", label, detail, limits), "GiCheck");
    } else {
        logError(std::format("FAIL {} ({}; {})", label, detail, limits), "GiCheck");
    }
    return isPassed;
}

bool EngineGiCheck::finishPhase()
{
    const PhaseInfo& info = kPhases[std::to_underlying(phase)];
    if (info.role == Role::GroundTruth) {
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
        for (size_t channel = 0; channel < 3; ++channel) {
            groundTruthRgb[channel] = frames > 0.0 ? rgbSum[channel] / frames : 0.0;
        }
        groundTruthNonFinite = nonFinitePixels;
        groundTruthNegative = negativePixels;
        groundTruthNoise = total > 0.0 ? noise / total : 0.0;
        pngScale = checkPngScale(groundTruth);
        if (isCheckPngRequested()) {
            writeCheckLuminancePng(std::format("gi ReSTIR GI convergence, {}, {}", poses()[poseIndex].name, info.name),
                                   extent, groundTruth, pngScale);
        }
        noiseFloorTileP90 = 0.0;
        logInfo(std::format("ReSTIR GI {}, {}: {} frames, {} bounces after X2, summed luminance {:.4g}, 1-sample "
                            "noise {:.1f}%, nonFinite {} neg {}",
                            info.name, poses()[poseIndex].name, capturedFrames, info.referenceBounces, total,
                            groundTruthNoise * 100.0, groundTruthNonFinite, groundTruthNegative),
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
    rgbSum = {0.0, 0.0, 0.0};
    nonFinitePixels = 0;
    negativePixels = 0;
    capturedFrames = 0;
    phaseFrame = 0;
    isCaptureMissing = false;
    phase = static_cast<Phase>(std::to_underlying(phase) + 1);
    if (phase != Phase::Count) {
        return false;
    }
    phase = Phase::GroundTruth;
    if (++poseIndex < poses().size()) {
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
