#include "engine_ray_check.hpp"
#include "../static_headers/logger.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>

namespace
{
    [[nodiscard]] float fromBits(uint32_t bits) { return std::bit_cast<float>(bits); }

    [[nodiscard]] std::string describe(const GpuRayCheckStats& stats)
    {
        return std::format("{} interior px: {} misses, {} id mismatches, {} empty hits, {} self hits; {} back-face hits, "
                           "{} edge mismatches, {} depth ties; decode max error normal {:.4f} deg, geometric normal {:.4f} deg, "
                           "material normal {:.4f} deg; position {:.2e} px, mip {:.3f} ({} px over 0.5); at the hit's "
                           "barycentrics normal {:.4f} deg, material normal {:.4f} deg",
                           stats.interiorPixels, stats.misses, stats.idMismatches, stats.emptyHits, stats.selfHits,
                           stats.backFaceHits, stats.edgeMismatches, stats.depthTies,
                           fromBits(stats.maxNormalErrorBits),
                           fromBits(stats.maxGeoNormalErrorBits), fromBits(stats.maxMaterialNormalErrorBits),
                           fromBits(stats.maxPositionErrorBits), fromBits(stats.maxMipErrorBits), stats.mipOutliers,
                           fromBits(stats.maxHitNormalErrorBits), fromBits(stats.maxHitMaterialNormalErrorBits));
    }

    [[nodiscard]] std::string describe(const GpuSerCheckStats& stats, bool isReorderEnabled)
    {
        return std::format("SER reorder {}: {} rays, {} hits; {} hit mismatches, {} id mismatches, {} data mismatches; "
                           "max error barycentric {:.2e}, distance {:.2e} relative",
                           isReorderEnabled ? "on" : "off", stats.rays, stats.hits, stats.hitMismatches,
                           stats.idMismatches, stats.dataMismatches, fromBits(stats.maxBarycentricErrorBits),
                           fromBits(stats.maxDistanceErrorBits));
    }
} // namespace

bool EngineRayCheck::isRequested()
{
    const char* text = std::getenv("ENGINE_RT_CHECK");
    return text != nullptr && std::string_view(text) == "1";
}

bool EngineRayCheck::isPassing(const GpuRayCheckStats& stats)
{
    return stats.misses == 0 && stats.idMismatches == 0 && stats.emptyHits == 0 && stats.selfHits == 0 &&
        stats.mipOutliers == 0 &&
        fromBits(stats.maxNormalErrorBits) <= kMaxNormalErrorDegrees &&
        fromBits(stats.maxGeoNormalErrorBits) <= kMaxNormalErrorDegrees &&
        fromBits(stats.maxMaterialNormalErrorBits) <= kMaxNormalErrorDegrees &&
        fromBits(stats.maxPositionErrorBits) <= kMaxPositionError;
}

bool EngineRayCheck::isPassing(const GpuSerCheckStats& stats)
{
    return stats.rays > 0 && stats.hitMismatches == 0 && stats.idMismatches == 0 && stats.dataMismatches == 0;
}

bool EngineRayCheck::endFrame(const GpuRayCheckStats& stats, std::span<const GpuSerCheckStats, 2> serStats,
                              bool isStandalone)
{
    if (++frame <= kWarmupFrames) {
        return false;
    }
    ++checkedFrames;
    if (isPassing(stats)) {
        ++passedFrames;
    } else if (loggedFailures < kMaxLoggedFailures) {
        ++loggedFailures;
        logError(std::format("FAIL frame {}: {}", frame, describe(stats)), "RayCheck");
    }
    for (size_t mode = 0; mode < serStats.size(); ++mode) {
        const GpuSerCheckStats& ser = serStats[mode];
        SerTotals& totals = serTotals[mode];
        if (isPassing(ser)) {
            ++totals.passedFrames;
        } else if (loggedFailures < kMaxLoggedFailures) {
            ++loggedFailures;
            logError(std::format("FAIL frame {}: {}", frame, describe(ser, mode == 1)), "RayCheck");
        }
        totals.rays += ser.rays;
        totals.hits += ser.hits;
        totals.mismatches += ser.hitMismatches + ser.idMismatches + ser.dataMismatches;
        totals.maxBarycentricError = std::max(totals.maxBarycentricError, fromBits(ser.maxBarycentricErrorBits));
        totals.maxDistanceError = std::max(totals.maxDistanceError, fromBits(ser.maxDistanceErrorBits));
    }
    interiorPixels += stats.interiorPixels;
    edgeMismatches += stats.edgeMismatches;
    edgePixels += stats.coveredPixels - stats.interiorPixels;
    backFaceHits += stats.backFaceHits;
    mipOutliers += stats.mipOutliers;
    depthTies += stats.depthTies;
    maxNormalError = std::max(maxNormalError, fromBits(stats.maxNormalErrorBits));
    maxGeoNormalError = std::max(maxGeoNormalError, fromBits(stats.maxGeoNormalErrorBits));
    maxPositionError = std::max(maxPositionError, fromBits(stats.maxPositionErrorBits));
    maxMipError = std::max(maxMipError, fromBits(stats.maxMipErrorBits));
    maxMaterialNormalError = std::max(maxMaterialNormalError, fromBits(stats.maxMaterialNormalErrorBits));
    maxHitNormalError = std::max(maxHitNormalError, fromBits(stats.maxHitNormalErrorBits));
    maxHitMaterialNormalError = std::max(maxHitMaterialNormalError, fromBits(stats.maxHitMaterialNormalErrorBits));
    if (checkedFrames == 1) {
        logInfo(std::format("first checked frame: {}", describe(stats)), "RayCheck");
        logInfo(std::format("first checked frame: {}; {}", describe(serStats[0], false), describe(serStats[1], true)),
                "RayCheck");
    }
    return isStandalone && checkedFrames >= kStandaloneFrames;
}

void EngineRayCheck::logSummary() const
{
    const auto ratio = [](uint64_t part, uint64_t whole) {
        return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
    };
    logInfo(std::format("{} interior px checked; {} back-face hits; {} depth ties; edge mismatches {:.3f}% of edge px; "
                        "decode max error normal {:.4f} deg, geometric normal {:.4f} deg, material normal {:.4f} deg; "
                        "position {:.2e} px, mip {:.3f} ({:.3f}% of interior px over 0.5); at the hit's barycentrics "
                        "normal {:.4f} deg, material normal {:.4f} deg",
                        interiorPixels, backFaceHits, depthTies, ratio(edgeMismatches, edgePixels), maxNormalError,
                        maxGeoNormalError, maxMaterialNormalError, maxPositionError, maxMipError,
                        ratio(mipOutliers, interiorPixels), maxHitNormalError, maxHitMaterialNormalError),
            "RayCheck");
    const std::string summary =
        std::format("Ray check: {} of {} frames matched the vis buffer", passedFrames, checkedFrames);
    if (checkedFrames > 0 && passedFrames == checkedFrames) {
        logInfo(summary, "RayCheck");
    } else {
        logError(summary, "RayCheck");
    }
    for (size_t mode = 0; mode < serTotals.size(); ++mode) {
        const SerTotals& totals = serTotals[mode];
        const std::string serSummary = std::format(
            "SER check, reorder {}: {} of {} frames matched traceClosest; {} rays, {} hits, {} mismatches; max error "
            "barycentric {:.2e}, distance {:.2e} relative",
            mode == 1 ? "on" : "off", totals.passedFrames, checkedFrames, totals.rays, totals.hits, totals.mismatches,
            totals.maxBarycentricError, totals.maxDistanceError);
        if (checkedFrames > 0 && totals.passedFrames == checkedFrames) {
            logInfo(serSummary, "RayCheck");
        } else {
            logError(serSummary, "RayCheck");
        }
    }
}
