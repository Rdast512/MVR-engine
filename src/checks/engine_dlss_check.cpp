#include "engine_dlss_check.hpp"
#include "../static_headers/logger.hpp"
#include "check_images.hpp"
#include "convergence_metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <string_view>

bool EngineDlssCheck::isRequested()
{
    const char* text = std::getenv("ENGINE_DLSS_CHECK");
    return text != nullptr && std::string_view(text) == "1";
}

const char* EngineDlssCheck::variantName(Variant variant)
{
    switch (variant) {
    case Variant::Shipped:
        return "shipped";
    case Variant::JitterInverted:
        return "jitter inverted";
    case Variant::NormalInverted:
        return "normal inverted";
    case Variant::RoughnessSquared:
        return "roughness squared";
    case Variant::DepthNonInverted:
        return "depth non-inverted";
    case Variant::ProjYFlipped:
        return "proj Y-flip";
    }
    return "";
}

std::vector<EngineDlssCheck::Step> EngineDlssCheck::buildSteps()
{
    return {
        // clean-input static baseline (gated; 128 warmup lets DLSS-RR settle after switching from startup lighting)
        {Variant::Shipped, false, 128, 120},
        // static perturbation controls
        {Variant::JitterInverted, false, 64, 64},
        {Variant::NormalInverted, false, 64, 64},
        {Variant::RoughnessSquared, false, 64, 64},
        {Variant::DepthNonInverted, false, 64, 64},
        // moving slow-pan controls (including proj[1][1] Y-flip)
        {Variant::Shipped, true, 32, 48},
        {Variant::JitterInverted, true, 32, 48},
        {Variant::NormalInverted, true, 32, 48},
        {Variant::RoughnessSquared, true, 32, 48},
        {Variant::DepthNonInverted, true, 32, 48},
        {Variant::ProjYFlipped, true, 32, 48},
    };
}

EngineDlssCheck::EngineDlssCheck() : steps(buildSteps())
{
    logInfo(std::format("DLSS-RR check ({}): {} steps, results follow",
                        scene == CheckScene::BistroCafe ? "bistro_cafe" : "sponza", steps.size()),
            "DlssCheck");
}

void EngineDlssCheck::startStep(Camera& camera, Renderer& renderer)
{
    const Step& step = steps[stepIndex];
    const CheckPose& pose = checkPoses(scene)[0];
    if (pose.isStartup) {
        camera.lookAt(startupPos, startupTarget);
    } else {
        camera.lookAt(pose.position, pose.target);
    }
    renderer.debugView.view = DebugView::Lit;
    renderer.indirectBounce = IndirectBounceMode::RestirGi;
    renderer.radianceCache = RadianceCacheMode::Off;
    // keep glass off during clean synthetic radiance check so only DLSS-RR and its guides are exercised
    renderer.glass.isPassEnabled = false;
    renderer.getWorldLights().sun.isEnabled = scene != CheckScene::BistroCafe;
    PostProcess& post = renderer.getPost();
    post.settings.isAutoExposure = false;
    post.settings.manualEv = 0.0f;
    post.requestSnap();

    Dlss& dlss = renderer.getDlss();
    dlss.guides = {};
    dlss.debug = {};
    dlss.debug.isDepthInverted = step.variant != Variant::DepthNonInverted;
    uint32_t flags = FrameFlag::CleanDlssInput;
    switch (step.variant) {
    case Variant::Shipped:
    case Variant::DepthNonInverted:
        break;
    case Variant::JitterInverted:
        dlss.debug.isJitterInverted = true;
        break;
    case Variant::NormalInverted:
        flags |= FrameFlag::InvertDlssNormal;
        break;
    case Variant::RoughnessSquared:
        flags |= FrameFlag::SquareDlssRoughness;
        break;
    case Variant::ProjYFlipped:
        dlss.debug.isProjYFlipped = true;
        break;
    }
    renderer.extraFrameFlags = flags;
    renderer.resetDlssHistory();

    extent = {};
    capturedFrames = 0;
    nonFinitePixels = 0;
    isCaptureMissing = false;
    sum.clear();
    squaredSum.clear();
    previous.clear();
    frameChange = 0.0;
}

EngineDlssCheck::Request EngineDlssCheck::beginFrame(Camera& camera, Renderer& renderer)
{
    Request request;
    if (isFinished) {
        return request;
    }
    renderer.fixedFrameDeltaMs = frameDeltaMs;
    if (frame < kStartupFrames) {
        return request;
    }
    if (!hasStartupPose) {
        startupPos = camera.cameraData.cameraPos;
        startupTarget = startupPos - glm::vec3(camera.cameraData.invView[2]);
        hasStartupPose = true;
    }
    if (stepFrame == 0) {
        startStep(camera, renderer);
    }
    const Step& step = steps[stepIndex];
    if (step.isPanning && stepFrame >= step.warmupFrames) {
        camera.rotate(kPanYawDeltaPerFrame, 0.0f);
    }
    isCaptureRequested = stepFrame >= step.warmupFrames;
    request.shouldCapture = isCaptureRequested;
    return request;
}

void EngineDlssCheck::accumulate(const DisplayCapture& capture)
{
    const DecodedFrame decoded = decodeFrame(capture);
    nonFinitePixels += decoded.nonFinitePixels;
    if (sum.empty()) {
        extent = capture.extent;
        sum.assign(decoded.luminance.size(), 0.0);
        squaredSum.assign(decoded.luminance.size(), 0.0);
    }
    double diffSum = 0.0;
    for (size_t i = 0; i < decoded.luminance.size(); ++i) {
        const double value = decoded.luminance[i];
        sum[i] += value;
        squaredSum[i] += value * value;
        if (!previous.empty()) {
            diffSum += std::abs(value - static_cast<double>(previous[i]));
        }
    }
    if (!previous.empty()) {
        frameChange += diffSum / static_cast<double>(decoded.luminance.size());
    }
    previous = decoded.luminance;
    ++capturedFrames;
}

void EngineDlssCheck::finishStep()
{
    const Step& step = steps[stepIndex];
    const std::string label = std::format("{}, {}", step.isPanning ? "pan" : "static", variantName(step.variant));
    if (isCaptureMissing || capturedFrames < 2 || nonFinitePixels > 0) {
        ++failedCount;
        logError(std::format("FAIL DLSS-RR ({}): captured {} / {}, nonFinite {}", label, capturedFrames, step.frames,
                             nonFinitePixels),
                 "DlssCheck");
        return;
    }
    const double n = static_cast<double>(capturedFrames);
    const size_t pixels = sum.size();
    double meanTotal = 0.0;
    double devTotal = 0.0;
    size_t litCount = 0;
    size_t flickerCount = 0;
    for (size_t i = 0; i < pixels; ++i) {
        const double mean = sum[i] / n;
        const double stdDev = std::sqrt(std::max(0.0, squaredSum[i] / n - mean * mean));
        meanTotal += mean;
        devTotal += stdDev;
        if (mean > kLitDisplayCutoff) {
            ++litCount;
            if (stdDev > kFlickerCv * mean) {
                ++flickerCount;
            }
        }
    }
    const double frameMean = meanTotal / static_cast<double>(pixels);
    const double relStd = meanTotal > 0.0 ? devTotal / meanTotal : 0.0;
    const double flicker = litCount > 0 ? static_cast<double>(flickerCount) / static_cast<double>(litCount) : 0.0;
    const double f2f = frameMean > 0.0 ? (frameChange / (n - 1.0)) / frameMean : 0.0;

    if (!step.isPanning) {
        if (step.variant == Variant::Shipped) {
            shippedStaticStd = relStd;
            const bool isPass = relStd <= kMaxStaticCleanStd && flicker <= kMaxStaticCleanFlicker;
            if (!isPass) {
                ++failedCount;
            }
            const std::string line = std::format(
                "{} DLSS-RR ({}): mean {:.5g}, temporal std {:.3f}% (limit {:.2f}%), flicker {:.3f}% (limit {:.2f}%), "
                "f2f {:.3f}%, nonFinite 0",
                isPass ? "PASS" : "FAIL", label, frameMean, relStd * 100.0, kMaxStaticCleanStd * 100.0, flicker * 100.0,
                kMaxStaticCleanFlicker * 100.0, f2f * 100.0);
            if (isPass) {
                logInfo(line, "DlssCheck");
            } else {
                logError(line, "DlssCheck");
            }
        } else if (step.variant == Variant::JitterInverted) {
            // jitter sign control: inverted jitter must shimmer strictly more than shipped
            const bool isPass = relStd > shippedStaticStd;
            if (!isPass) {
                ++failedCount;
            }
            const std::string line = std::format(
                "{} DLSS-RR ({}): mean {:.5g}, temporal std {:.3f}% vs shipped {:.3f}%, flicker {:.3f}%, f2f {:.3f}%",
                isPass ? "PASS" : "FAIL", label, frameMean, relStd * 100.0, shippedStaticStd * 100.0, flicker * 100.0,
                f2f * 100.0);
            if (isPass) {
                logInfo(line, "DlssCheck");
            } else {
                logError(line, "DlssCheck");
            }
        } else {
            logInfo(std::format("MEASURED DLSS-RR ({}): mean {:.5g}, temporal std {:.3f}% vs shipped {:.3f}%, "
                                "flicker {:.3f}%, f2f {:.3f}%",
                                label, frameMean, relStd * 100.0, shippedStaticStd * 100.0, flicker * 100.0,
                                f2f * 100.0),
                    "DlssCheck");
        }
    } else {
        if (step.variant == Variant::Shipped) {
            shippedPanF2f = f2f;
        }
        logInfo(std::format("MEASURED DLSS-RR ({}): mean {:.5g}, f2f {:.3f}% (vs shipped {:.3f}%), nonFinite 0", label,
                            frameMean, f2f * 100.0, shippedPanF2f * 100.0),
                "DlssCheck");
    }
}

bool EngineDlssCheck::endFrame(const std::optional<DisplayCapture>& capture)
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
            accumulate(*capture);
        }
    }
    ++stepFrame;
    const Step& step = steps[stepIndex];
    if (stepFrame >= step.warmupFrames + step.frames) {
        finishStep();
        ++stepIndex;
        stepFrame = 0;
        if (stepIndex >= steps.size()) {
            isFinished = true;
            logInfo(std::format("DLSS-RR check: {} steps, {} failed", steps.size(), failedCount), "DlssCheck");
            return true;
        }
    }
    return false;
}
