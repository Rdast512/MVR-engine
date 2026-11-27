#include "engine_guide_capture.hpp"
#include "../Constants.h"
#include "../static_headers/logger.hpp"
#include "core/assets_loader.hpp"
#include "convergence_metrics.hpp"

#include <SDL3/SDL_filesystem.h>
#include <glm/gtc/packing.hpp>
#include <json.hpp>
#include <stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <string_view>
#include <vector>

namespace
{
    // models/ and the glTF-Sample-Assets checkout gltf/ live there
    std::filesystem::path repositoryRoot() { return std::filesystem::path(ENGINE_MODELS_DIR).parent_path(); }

    // next to logs/engine.log
    std::filesystem::path outputDirectory()
    {
        const char* base = SDL_GetBasePath();
        return std::filesystem::path(base != nullptr ? base : "") / "logs" / "guides";
    }

    // the swapchain is sRGB: the PNG shows what the screen shows
    uint8_t encodeSrgb(float linear)
    {
        const float clamped = std::clamp(linear, 0.0f, 1.0f);
        const float encoded =
            clamped <= 0.0031308f ? clamped * 12.92f : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
        return static_cast<uint8_t>(std::lround(encoded * 255.0f));
    }

    std::string fileStem(std::string_view text)
    {
        std::string stem(text);
        std::ranges::transform(stem, stem.begin(), [](unsigned char character) -> char {
            return character == ' ' ? '_' : static_cast<char>(std::tolower(character));
        });
        return stem;
    }

    // the loader takes no '..' or absolute URIs: the variant folder gets copies of the referenced files
    bool copyReferencedFiles(const nlohmann::json& items, const std::filesystem::path& sourceDirectory,
                             const std::filesystem::path& variantDirectory)
    {
        for (const nlohmann::json& item : items) {
            if (!item.contains("uri")) {
                continue;
            }
            const std::string uri = item["uri"].get<std::string>();
            if (uri.starts_with("data:")) {
                continue;
            }
            const std::filesystem::path target = variantDirectory / uri;
            std::error_code errorCode;
            std::filesystem::create_directories(target.parent_path(), errorCode);
            std::filesystem::copy_file(sourceDirectory / uri, target, std::filesystem::copy_options::update_existing,
                                       errorCode);
            if (errorCode) {
                logError(std::format("could not copy {}: {}", uri, errorCode.message()), "GuideCapture");
                return false;
            }
        }
        return true;
    }

    // debug_view writes n * 0.5 + 0.5; empty pixels decode to a zero or non-unit vector
    glm::vec3 decodeNormal(std::span<const uint16_t> rgba16f, size_t texel)
    {
        const glm::vec3 encoded(glm::unpackHalf1x16(rgba16f[texel * 4]), glm::unpackHalf1x16(rgba16f[texel * 4 + 1]),
                                glm::unpackHalf1x16(rgba16f[texel * 4 + 2]));
        const glm::vec3 normal = encoded * 2.0f - 1.0f;
        constexpr float kUnitTolerance = 0.1f;
        return std::abs(glm::length(normal) - 1.0f) <= kUnitTolerance ? glm::normalize(normal) : glm::vec3(0.0f);
    }

    glm::vec4 decodeTexel(std::span<const uint16_t> rgba16f, size_t texel)
    {
        return {glm::unpackHalf1x16(rgba16f[texel * 4]), glm::unpackHalf1x16(rgba16f[texel * 4 + 1]),
                glm::unpackHalf1x16(rgba16f[texel * 4 + 2]), glm::unpackHalf1x16(rgba16f[texel * 4 + 3])};
    }

    float sumRgb(glm::vec3 color) { return color.r + color.g + color.b; }
} // namespace

bool EngineGuideCapture::isRequested()
{
    const char* text = std::getenv("ENGINE_GUIDE_CAPTURE");
    return text != nullptr && std::string_view(text) == "1";
}

std::optional<std::filesystem::path> EngineGuideCapture::prepareModel(const Shot& shot)
{
    const std::optional<std::filesystem::path> source = AssetsLoader::resolveModelPath(repositoryRoot() / shot.folder);
    if (!source || shot.variant == Variant::AsShipped) {
        return source;
    }
    if (source->extension() != ".gltf") {
        logError(std::format("{}: variants need a .gltf, not {}", shot.name, source->filename().string()),
                 "GuideCapture");
        return std::nullopt;
    }
    std::ifstream input(*source);
    nlohmann::json gltf = nlohmann::json::parse(input, nullptr, false);
    if (gltf.is_discarded()) {
        logError(std::format("{}: could not parse {}", shot.name, source->string()), "GuideCapture");
        return std::nullopt;
    }
    if (gltf.contains("meshes")) {
        for (nlohmann::json& mesh : gltf["meshes"]) {
            for (nlohmann::json& primitive : mesh["primitives"]) {
                primitive["attributes"].erase("TANGENT");
                if (shot.variant == Variant::NoNormals) {
                    primitive["attributes"].erase("NORMAL");
                }
            }
        }
    }
    const std::filesystem::path directory = outputDirectory() / "variants" / shot.name;
    std::error_code errorCode;
    std::filesystem::create_directories(directory, errorCode);
    for (const char* key : {"buffers", "images"}) {
        if (gltf.contains(key) && !copyReferencedFiles(gltf[key], source->parent_path(), directory)) {
            return std::nullopt;
        }
    }
    const std::filesystem::path file = directory / std::format("{}.gltf", shot.name);
    std::ofstream output(file);
    output << gltf.dump(1);
    if (!output) {
        logError(std::format("could not write {}", file.string()), "GuideCapture");
        return std::nullopt;
    }
    return file;
}

EngineGuideCapture::Request EngineGuideCapture::beginFrame(Camera& camera, DebugViewSettings& debugView,
                                                           RtxdiContext::Settings& rtxdi,
                                                           std::optional<glm::vec4> modelBounds)
{
    // the check gates the ReSTIR DI port, not the defaults: the NVIDIA defaults fail it (spec rtxdi_3_2_update §6.1)
    const bool isConverging = isConvergenceShot(shotIndex);
    if (isConverging && !heldRtxdiSettings) {
        heldRtxdiSettings = std::exchange(rtxdi, RtxdiContext::convergedSettings());
    } else if (!isConverging && heldRtxdiSettings) {
        rtxdi = *heldRtxdiSettings;
        heldRtxdiSettings.reset();
    }
    if (isConverging) {
        rtxdi.brdfSamples = findConvergenceShot(shotIndex)->brdfSamples;
    }
    Request request;
    if (shotIndex >= kShots.size()) {
        return request;
    }
    const Shot& shot = kShots[shotIndex];
    if (shotFrame == 0) {
        const std::pair<std::string, Variant> model{shot.folder, shot.variant};
        if (loadedModel != model) {
            request.model = prepareModel(shot);
            loadedModel = model;
        }
        return request;
    }
    if (shotFrame == 1) {
        frameShot(camera, shot, modelBounds);
    }
    request.sun = shot.sun;
    if (throughput) {
        if (throughput->frame == 0) {
            frameShot(camera, shot, modelBounds);
        }
        if (throughput->phase == ThroughputPhase::PassOff) {
            request.glass.isPassEnabled = false;
        }
        request.glass.isReflectionEnabled = false;
        request.glass.isDiffuseShareEnabled = false;
        request.glass.isEmissionEnabled = false;
        request.glass.isShadowTransmittanceEnabled = false;
        request.frameIndex = kThroughputFrameIndexBase + throughput->frame;
        request.jitterPhase = throughput->frame;
        request.manualEv = kThroughputManualEv;
        // NRC trains asynchronously to the frame; ReSTIR GI alone repeats
        request.indirectBounce = IndirectBounceMode::RestirGi;
        request.radianceCache = RadianceCacheMode::Off;
        debugView.view = throughput->phase == ThroughputPhase::Throughput ? DebugView::Glass : DebugView::DlssInput;
        debugView.showGlassReflection = false;
        request.shouldCapture = throughput->frame + 1 == kThroughputFrames && modelBounds.has_value();
        return request;
    }
    request.isRtxdiReference = isConvergenceRunning && convergence->isReferencePhase;
    request.areTriangleLightsSampled = !isConvergenceRunning || findConvergenceShot(shotIndex)->areTriangleLightsSampled;
    debugView.view = isConvergenceRunning ? DebugView::DirectLighting : kViews[viewIndex].view;
    const IndirectStagePair stages = kLegacyIndirectModes[isConvergenceRunning ? kNrcRestirGiIndex : kViews[viewIndex].modeIndex].stages;
    request.indirectBounce = stages.bounce;
    request.radianceCache = stages.cache;
    request.shouldCapture = shotFrame >= kSettleFrames && modelBounds.has_value() &&
        (!isConvergenceRunning || convergence->frame >= kConvergenceWarmupFrames);
    return request;
}

bool EngineGuideCapture::endFrame(const std::optional<DisplayCapture>& capture)
{
    if (shotIndex >= kShots.size()) {
        return true;
    }
    if (shotFrame < kSettleFrames) {
        ++shotFrame;
        return false;
    }
    if (isConvergenceRunning) {
        return advanceConvergence(capture) && finishShot();
    }
    if (throughput) {
        return advanceThroughput(capture) && finishShot();
    }
    if (!capture) {
        logError(std::format("{}: no model to frame, shot skipped", kShots[shotIndex].name), "GuideCapture");
        return finishShot();
    }
    if (writePng(*capture)) {
        ++writtenCount;
    }
    keepNormals(*capture);
    const std::optional<ConvergenceReference> reference = convergenceReference(shotIndex);
    if (reference == ConvergenceReference::ShadowedLights && kViews[viewIndex].view == DebugView::ShadowedLights) {
        const DecodedFrame decoded = decodeFrame(*capture);
        convergence.emplace();
        convergence->shotIndex = shotIndex;
        convergence->extent = capture->extent;
        convergence->reference = std::move(decoded.luminance);
        convergence->referenceRgb = decoded.rgbTotal;
        convergence->referenceNonFinite = decoded.nonFinitePixels;
        convergence->referenceNegative = decoded.negativePixels;
    }
    if (++viewIndex < kViews.size()) {
        return false;
    }
    if (reference == ConvergenceReference::RtxdiNone) {
        convergence.emplace();
        convergence->shotIndex = shotIndex;
        convergence->extent = capture->extent;
        convergence->isReferencePhase = true;
    }
    if (reference && convergence) {
        isConvergenceRunning = true;
        return false;
    }
    if (isThroughputShot(shotIndex)) {
        throughput.emplace();
        return false;
    }
    return finishShot();
}

void EngineGuideCapture::frameShot(Camera& camera, const Shot& shot, std::optional<glm::vec4> modelBounds)
{
    if (shot.pose) {
        camera.lookAt(shot.pose->position, shot.pose->target);
    } else if (modelBounds) {
        // the bounding sphere fills the vertical field of view
        const glm::vec3 center(*modelBounds);
        const float halfFov = glm::radians(camera.getFovDegrees()) * 0.5f;
        const float distance = kFramingMargin * modelBounds->w / std::sin(halfFov);
        camera.lookAt(center + distance * glm::normalize(shot.viewDirection), center);
    }
}

bool EngineGuideCapture::advanceThroughput(const std::optional<DisplayCapture>& capture)
{
    ThroughputRun& run = *throughput;
    if (++run.frame < kThroughputFrames) {
        return false;
    }
    const bool isFirstPhase = run.phase == ThroughputPhase::PassOff;
    if (!capture || (!isFirstPhase && capture->extent != run.extent)) {
        logError(std::format("FAIL glass throughput ({}): capture missing or resized", kShots[shotIndex].name),
                 "GuideCapture");
        ++throughputCheckCount;
        throughput.reset();
        return true;
    }
    switch (run.phase) {
    case ThroughputPhase::PassOff:
        run.extent = capture->extent;
        run.passOff.assign(capture->rgba16f.begin(), capture->rgba16f.end());
        run.phase = ThroughputPhase::Transmission;
        break;
    case ThroughputPhase::Transmission:
        run.transmission.assign(capture->rgba16f.begin(), capture->rgba16f.end());
        run.phase = ThroughputPhase::Throughput;
        break;
    case ThroughputPhase::Throughput: {
        const bool isPassed = evaluateThroughput(*capture);
        ++throughputCheckCount;
        throughputPassedCount += isPassed ? 1u : 0u;
        throughput.reset();
        return true;
    }
    }
    run.frame = 0;
    return false;
}

bool EngineGuideCapture::evaluateThroughput(const DisplayCapture& glass) const
{
    const ThroughputRun& run = *throughput;
    const size_t texelCount = static_cast<size_t>(run.extent.width) * run.extent.height;
    // pixels without glass must come through bit for bit
    uint64_t touchedPixels = 0;
    uint64_t nonFinitePixels = 0;
    std::array<uint64_t, 3> layerPixels{0, 0, 0};
    struct Compared
    {
        float expected;
        float error;
    };
    std::vector<Compared> compared;
    for (size_t texel = 0; texel < texelCount; ++texel) {
        const glm::vec4 throughputTexel = decodeTexel(glass.rgba16f, texel);
        const glm::vec3 throughputRgb(throughputTexel);
        const glm::vec3 passOff(decodeTexel(run.passOff, texel));
        const glm::vec3 transmission(decodeTexel(run.transmission, texel));
        if (!std::isfinite(sumRgb(passOff)) || !std::isfinite(sumRgb(transmission)) ||
            !std::isfinite(sumRgb(throughputRgb))) {
            ++nonFinitePixels;
            continue;
        }
        const auto layers = static_cast<uint32_t>(std::lround(throughputTexel.a));
        if (layers == 0) {
            const auto first = static_cast<std::ptrdiff_t>(texel * 4);
            const bool isTouched = !std::equal(run.passOff.begin() + first, run.passOff.begin() + first + 3,
                                               run.transmission.begin() + first);
            touchedPixels += isTouched ? 1u : 0u;
            continue;
        }
        ++layerPixels[std::min<uint32_t>(layers, 3) - 1];
        if (layers > 2) {
            continue;
        }
        const glm::vec3 expected = throughputRgb * passOff;
        const glm::vec3 error = glm::abs(transmission - expected);
        compared.push_back({sumRgb(expected), sumRgb(error)});
    }
    double expectedSum = 0.0;
    for (const Compared& pixel : compared) {
        expectedSum += pixel.expected;
    }
    const double darkLimit =
        compared.empty() ? 0.0 : kThroughputDarkShare * expectedSum / static_cast<double>(compared.size());
    std::vector<float> errors;
    errors.reserve(compared.size());
    for (const Compared& pixel : compared) {
        if (pixel.expected > darkLimit) {
            errors.push_back(pixel.error / pixel.expected);
        }
    }
    const std::string name = kShots[shotIndex].name;
    if (errors.size() < kMinThroughputPixels) {
        logError(std::format("FAIL glass throughput ({}): {} px behind one or two lit layers, {} needed", name,
                             errors.size(), kMinThroughputPixels),
                 "GuideCapture");
        return false;
    }
    std::ranges::sort(errors);
    const auto quantile = [&errors](double share) {
        return errors[static_cast<size_t>(share * static_cast<double>(errors.size() - 1))];
    };
    const double median = quantile(0.5);
    const double p90 = quantile(0.9);
    const bool isPassed = touchedPixels == 0 && nonFinitePixels == 0 && median <= kMaxThroughputMedianError &&
        p90 <= kMaxThroughputP90Error;
    const std::string detail = std::format(
        "{}: pass on / off against the pass's throughput over {} px ({} below {:.1f}% of the mean skipped): median "
        "{:.3f}% / p90 {:.3f}% / max {:.3f}%; layers 1 / 2 / 3+: {} / {} / {} px; {} px without glass changed, "
        "nonFinite {}",
        name, errors.size(), compared.size() - errors.size(), kThroughputDarkShare * 100.0, median * 100.0,
        p90 * 100.0, errors.back() * 100.0, layerPixels[0], layerPixels[1], layerPixels[2], touchedPixels,
        nonFinitePixels);
    if (isPassed) {
        logInfo(std::format("PASS glass throughput ({})", detail), "GuideCapture");
    } else {
        logError(std::format("FAIL glass throughput ({})", detail), "GuideCapture");
    }
    return isPassed;
}

bool EngineGuideCapture::finishShot()
{
    viewIndex = 0;
    shotFrame = 0;
    if (++shotIndex < kShots.size()) {
        return false;
    }
    logInfo(std::format("Guide capture: {} PNGs written to {}", writtenCount, outputDirectory().string()),
            "GuideCapture");
    normalPassedCount = evaluateNormalChecks();
    const std::string summary = std::format("Normal checks: {} of {} passed", normalPassedCount, kNormalChecks.size());
    if (normalPassedCount == kNormalChecks.size()) {
        logInfo(summary, "GuideCapture");
    } else {
        logError(summary, "GuideCapture");
    }
    const std::string convergenceSummary =
        std::format("Convergence checks: {} of {} passed", convergencePassedCount, kConvergenceShots.size());
    if (convergenceCheckCount == kConvergenceShots.size() && convergencePassedCount == kConvergenceShots.size()) {
        logInfo(convergenceSummary, "GuideCapture");
    } else {
        logError(convergenceSummary, "GuideCapture");
    }
    const std::string throughputSummary =
        std::format("Glass throughput checks: {} of {} passed", throughputPassedCount, kThroughputShots.size());
    if (throughputCheckCount == kThroughputShots.size() && throughputPassedCount == kThroughputShots.size()) {
        logInfo(throughputSummary, "GuideCapture");
    } else {
        logError(throughputSummary, "GuideCapture");
    }
    isFinished = true;
    return true;
}

void EngineGuideCapture::keepNormals(const DisplayCapture& capture)
{
    if (kViews[viewIndex].modeIndex != kNrcRestirGiIndex) {
        return;
    }
    const std::pair<size_t, DebugView> key{shotIndex, kViews[viewIndex].view};
    const bool isRead = std::ranges::any_of(kNormalChecks, [&](const NormalCheck& check) {
        return key == std::pair{check.shot, check.view} || key == std::pair{check.referenceShot, check.referenceView};
    });
    if (!isRead) {
        return;
    }
    const size_t texelCount = static_cast<size_t>(capture.extent.width) * capture.extent.height;
    std::vector<glm::vec3> normals(texelCount);
    for (size_t texel = 0; texel < texelCount; ++texel) {
        normals[texel] = decodeNormal(capture.rgba16f, texel);
    }
    keptNormals[key] = std::move(normals);
}

size_t EngineGuideCapture::evaluateNormalChecks() const
{
    size_t passed = 0;
    for (const NormalCheck& check : kNormalChecks) {
        const auto tested = keptNormals.find({check.shot, check.view});
        const auto reference = keptNormals.find({check.referenceShot, check.referenceView});
        if (tested == keptNormals.end() || reference == keptNormals.end() ||
            tested->second.size() != reference->second.size()) {
            logError(std::format("FAIL {}: capture missing", check.name), "GuideCapture");
            continue;
        }
        // both captures see the same triangles: a pixel empty in only one of them is a coverage change
        uint64_t coverageMismatches = 0;
        std::vector<float> angles;
        for (size_t texel = 0; texel < tested->second.size(); ++texel) {
            const glm::vec3 a = tested->second[texel];
            const glm::vec3 b = reference->second[texel];
            const bool hasA = a != glm::vec3(0.0f);
            const bool hasB = b != glm::vec3(0.0f);
            if (hasA != hasB) {
                ++coverageMismatches;
            } else if (hasA) {
                angles.push_back(glm::degrees(std::acos(std::clamp(glm::dot(a, b), -1.0f, 1.0f))));
            }
        }
        if (angles.empty()) {
            logError(std::format("FAIL {}: no pixels to compare", check.name), "GuideCapture");
            continue;
        }
        std::ranges::sort(angles);
        const float quantile = angles[static_cast<size_t>(kNormalAngleQuantile * static_cast<double>(angles.size() - 1))];
        double sum = 0.0;
        for (const float angle : angles) {
            sum += angle;
        }
        const std::string detail = std::format(
            "{} px, mean {:.3f} deg, {:.1f}% within {:.3f} deg, max {:.3f} deg, {} px differ in coverage", angles.size(),
            sum / static_cast<double>(angles.size()), kNormalAngleQuantile * 100.0, quantile, angles.back(),
            coverageMismatches);
        if (coverageMismatches == 0 && quantile <= kMaxNormalAngleDegrees) {
            ++passed;
            logInfo(std::format("PASS {}: {}", check.name, detail), "GuideCapture");
        } else {
            logError(std::format("FAIL {}: {}", check.name, detail), "GuideCapture");
        }
    }
    return passed;
}

bool EngineGuideCapture::advanceConvergence(const std::optional<DisplayCapture>& capture)
{
    ConvergenceRun& run = *convergence;
    if (run.frame++ < kConvergenceWarmupFrames) {
        return false;
    }
    if (capture && capture->extent == run.extent && run.isReferencePhase) {
        accumulateReference(*capture);
        return false;
    }
    if (!capture || capture->extent != run.extent) {
        logError(std::format("FAIL ReSTIR DI convergence ({}): capture missing or resized", kShots[run.shotIndex].name),
                 "GuideCapture");
        ++convergenceCheckCount;
        isConvergenceRunning = false;
        convergence.reset();
        return true;
    }
    DecodedFrame decoded = decodeFrame(*capture);
    if (run.sum.empty()) {
        run.sum.assign(decoded.luminance.size(), 0.0);
    }
    for (size_t texel = 0; texel < decoded.luminance.size(); ++texel) {
        const float value = decoded.luminance[texel];
        run.sum[texel] += value;
        run.absoluteErrorSum += std::abs(value - run.reference[texel]);
        if (!run.previous.empty()) {
            run.changeSum += std::abs(value - run.previous[texel]);
        }
    }
    for (size_t channel = 0; channel < 3; ++channel) {
        run.rgbSum[channel] += decoded.rgbTotal[channel];
    }
    run.nonFinitePixels += decoded.nonFinitePixels;
    run.negativePixels += decoded.negativePixels;
    run.previous = std::move(decoded.luminance);
    if (++run.capturedFrames < kConvergenceFrames) {
        return false;
    }
    const bool isPassed = evaluateConvergence();
    ++convergenceCheckCount;
    convergencePassedCount += isPassed ? 1u : 0u;
    isConvergenceRunning = false;
    convergence.reset();
    return true;
}

void EngineGuideCapture::accumulateReference(const DisplayCapture& capture)
{
    ConvergenceRun& run = *convergence;
    const DecodedFrame decoded = decodeFrame(capture);
    if (run.referenceSum.empty()) {
        run.referenceSum.assign(decoded.luminance.size(), 0.0);
    }
    for (size_t texel = 0; texel < decoded.luminance.size(); ++texel) {
        run.referenceSum[texel] += decoded.luminance[texel];
    }
    for (size_t channel = 0; channel < 3; ++channel) {
        run.referenceRgb[channel] += decoded.rgbTotal[channel];
    }
    run.referenceNonFinite += decoded.nonFinitePixels;
    run.referenceNegative += decoded.negativePixels;
    if (++run.capturedFrames < kReferenceFrames) {
        return;
    }
    // the mean is the reference; ReSTIR DI then warms up again with reuse on
    const double frames = run.capturedFrames;
    run.reference.resize(run.referenceSum.size());
    for (size_t texel = 0; texel < run.referenceSum.size(); ++texel) {
        run.reference[texel] = static_cast<float>(run.referenceSum[texel] / frames);
    }
    for (double& channel : run.referenceRgb) {
        channel /= frames;
    }
    run.referenceSum = {};
    run.isReferencePhase = false;
    run.capturedFrames = 0;
    run.frame = 0;
}

bool EngineGuideCapture::evaluateConvergence() const
{
    const ConvergenceRun& run = *convergence;
    const double frames = run.capturedFrames;
    std::vector<double> mean(run.sum.size());
    const std::vector<double> reference(run.reference.begin(), run.reference.end());
    double referenceTotal = 0.0;
    double meanTotal = 0.0;
    for (size_t texel = 0; texel < reference.size(); ++texel) {
        mean[texel] = run.sum[texel] / frames;
        referenceTotal += reference[texel];
        meanTotal += mean[texel];
    }
    if (referenceTotal <= 0.0) {
        logError(std::format("FAIL ReSTIR DI convergence ({}): the reference is black", kShots[run.shotIndex].name),
                 "GuideCapture");
        return false;
    }

    const std::array<double, 3> meanRgb{run.rgbSum[0] / frames, run.rgbSum[1] / frames, run.rgbSum[2] / frames};
    const std::array<double, 3> rgbBias = computeChannelBias(meanRgb, run.referenceRgb, referenceTotal);
    const double maxRgbBias = std::max({std::abs(rgbBias[0]), std::abs(rgbBias[1]), std::abs(rgbBias[2])});
    const TileErrors tiles = compareTiles(mean, reference, run.extent, kConvergenceTileSize);
    const double bias = (meanTotal - referenceTotal) / referenceTotal;
    const uint64_t totalNonFinite = run.nonFinitePixels + run.referenceNonFinite;
    const uint64_t totalNegative = run.negativePixels + run.referenceNegative;
    const bool isPassed = totalNonFinite == 0 && totalNegative == 0 && std::abs(bias) <= kMaxConvergenceBias &&
        maxRgbBias <= kMaxConvergenceBias && tiles.p90 <= kMaxConvergenceTileP90 &&
        tiles.zeroRefLeak <= kMaxConvergenceBias;
    const std::string detail = std::format(
        "{} against {}, {} frames after {}: bias {:+.3f}% (RGB {:+.3f}/{:+.3f}/{:+.3f}%), {}x{} tiles median {:.2f}% / p90 "
        "{:.2f}% / max {:.2f}% ({} tiles, dark leak {:.3f}% in {} tiles), per-frame error {:.2f}%, frame-to-frame "
        "change {:.2f}%, nonFinite {} neg {}",
        kShots[run.shotIndex].name,
        convergenceReference(run.shotIndex) == ConvergenceReference::RtxdiNone
            ? std::format("RTXDI None over {} frames", kReferenceFrames)
            : std::string("Shadowed lights"),
        run.capturedFrames, kConvergenceWarmupFrames, bias * 100.0, rgbBias[0] * 100.0,
        rgbBias[1] * 100.0, rgbBias[2] * 100.0, kConvergenceTileSize, kConvergenceTileSize, tiles.median * 100.0,
        tiles.p90 * 100.0, tiles.max * 100.0, tiles.count, tiles.zeroRefLeak * 100.0, tiles.zeroRefLeakTiles,
        run.absoluteErrorSum / frames / referenceTotal * 100.0,
        run.changeSum / (frames - 1.0) / referenceTotal * 100.0, totalNonFinite, totalNegative);
    if (isPassed) {
        logInfo(std::format("PASS ReSTIR DI convergence ({})", detail), "GuideCapture");
    } else {
        logError(std::format("FAIL ReSTIR DI convergence ({})", detail), "GuideCapture");
    }
    return isPassed;
}

bool EngineGuideCapture::writePng(const DisplayCapture& capture) const
{
    const size_t texelCount = static_cast<size_t>(capture.extent.width) * capture.extent.height;
    std::vector<uint8_t> pixels(texelCount * 4);
    for (size_t texel = 0; texel < texelCount; ++texel) {
        for (size_t channel = 0; channel < 3; ++channel) {
            pixels[texel * 4 + channel] = encodeSrgb(glm::unpackHalf1x16(capture.rgba16f[texel * 4 + channel]));
        }
        pixels[texel * 4 + 3] = 255;
    }

    const Shot& shot = kShots[shotIndex];
    const std::filesystem::path directory = outputDirectory();
    std::error_code errorCode;
    std::filesystem::create_directories(directory, errorCode);
    const ShotView& shotView = kViews[viewIndex];
    // NRC + ReSTIR GI keeps the file names of the runs before G7
    const std::string modeSuffix = shotView.modeIndex == kNrcRestirGiIndex
        ? std::string()
        : std::format("_{}", kLegacyIndirectModes[shotView.modeIndex].id);
    const std::filesystem::path file =
        directory / std::format("{}_{}{}.png", shot.name, fileStem(debugViewInfo(shotView.view).name), modeSuffix);
    const int stride = static_cast<int>(capture.extent.width) * 4;
    // fast zlib compression cuts 315-PNG write time without changing decoded pixels
    stbi_write_png_compression_level = 1;
    if (stbi_write_png(file.string().c_str(), static_cast<int>(capture.extent.width),
                       static_cast<int>(capture.extent.height), 4, pixels.data(), stride) == 0) {
        logError(std::format("could not write {}", file.string()), "GuideCapture");
        return false;
    }
    return true;
}
