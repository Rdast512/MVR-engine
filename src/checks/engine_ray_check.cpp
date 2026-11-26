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
                           "{} edge mismatches, {} depth ties, {} alpha edges; decode max error normal {:.4f} deg, geometric normal {:.4f} deg, "
                           "material normal {:.4f} deg; position {:.2e} px, mip {:.3f} ({} px over 0.5); at the hit's "
                           "barycentrics normal {:.4f} deg, material normal {:.4f} deg",
                           stats.interiorPixels, stats.misses, stats.idMismatches, stats.emptyHits, stats.selfHits,
                           stats.backFaceHits, stats.edgeMismatches, stats.depthTies, stats.alphaEdges,
                           fromBits(stats.maxNormalErrorBits),
                           fromBits(stats.maxGeoNormalErrorBits), fromBits(stats.maxMaterialNormalErrorBits),
                           fromBits(stats.maxPositionErrorBits), fromBits(stats.maxMipErrorBits), stats.mipOutliers,
                           fromBits(stats.maxHitNormalErrorBits), fromBits(stats.maxHitMaterialNormalErrorBits));
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

bool EngineRayCheck::endFrame(const GpuRayCheckStats& stats, bool isStandalone)
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
    interiorPixels += stats.interiorPixels;
    edgeMismatches += stats.edgeMismatches;
    edgePixels += stats.coveredPixels - stats.interiorPixels;
    backFaceHits += stats.backFaceHits;
    mipOutliers += stats.mipOutliers;
    depthTies += stats.depthTies;
    alphaEdges += stats.alphaEdges;
    maxNormalError = std::max(maxNormalError, fromBits(stats.maxNormalErrorBits));
    maxGeoNormalError = std::max(maxGeoNormalError, fromBits(stats.maxGeoNormalErrorBits));
    maxPositionError = std::max(maxPositionError, fromBits(stats.maxPositionErrorBits));
    maxMipError = std::max(maxMipError, fromBits(stats.maxMipErrorBits));
    maxMaterialNormalError = std::max(maxMaterialNormalError, fromBits(stats.maxMaterialNormalErrorBits));
    maxHitNormalError = std::max(maxHitNormalError, fromBits(stats.maxHitNormalErrorBits));
    maxHitMaterialNormalError = std::max(maxHitMaterialNormalError, fromBits(stats.maxHitMaterialNormalErrorBits));
    if (checkedFrames == 1) {
        logInfo(std::format("first checked frame: {}", describe(stats)), "RayCheck");
    }
    return isStandalone && checkedFrames >= kStandaloneFrames;
}

void EngineRayCheck::logSummary() const
{
    const auto ratio = [](uint64_t part, uint64_t whole) {
        return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
    };
    logInfo(std::format("{} interior px checked; {} back-face hits; {} depth ties; {} alpha edges; edge mismatches {:.3f}% of edge px; "
                        "decode max error normal {:.4f} deg, geometric normal {:.4f} deg, material normal {:.4f} deg; "
                        "position {:.2e} px, mip {:.3f} ({:.3f}% of interior px over 0.5); at the hit's barycentrics "
                        "normal {:.4f} deg, material normal {:.4f} deg",
                        interiorPixels, backFaceHits, depthTies, alphaEdges, ratio(edgeMismatches, edgePixels), maxNormalError,
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
}
