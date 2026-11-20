#include "engine_guide_capture.hpp"
#include "../Constants.h"
#include "../static_headers/logger.hpp"
#include "assets_loader.hpp"
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
    const bool isConvergenceShot = shotIndex == kConvergenceShot;
    if (isConvergenceShot && !heldRtxdiSettings) {
        heldRtxdiSettings = std::exchange(rtxdi, RtxdiContext::convergedSettings());
    } else if (!isConvergenceShot && heldRtxdiSettings) {
        rtxdi = *heldRtxdiSettings;
        heldRtxdiSettings.reset();
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
    if (shotFrame == 1 && shot.pose) {
        camera.lookAt(shot.pose->position, shot.pose->target);
    } else if (shotFrame == 1 && modelBounds) {
        // the bounding sphere fills the vertical field of view
        const glm::vec3 center(*modelBounds);
        const float halfFov = glm::radians(camera.getFovDegrees()) * 0.5f;
        const float distance = kFramingMargin * modelBounds->w / std::sin(halfFov);
        camera.lookAt(center + distance * glm::normalize(shot.viewDirection), center);
    }
    request.sun = shot.sun;
    debugView.view = isConvergenceRunning ? DebugView::DirectLighting : kViews[viewIndex].view;
    request.indirectMode = isConvergenceRunning ? IndirectMode::NrcRestirGi : kViews[viewIndex].mode;
    request.shouldCapture = shotFrame >= kSettleFrames && modelBounds.has_value() &&
        (!isConvergenceRunning || convergence->frame >= kConvergenceWarmupFrames);
    return request;
}

bool EngineGuideCapture::endFrame(const std::optional<Renderer::DisplayCapture>& capture)
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
    if (!capture) {
        logError(std::format("{}: no model to frame, shot skipped", kShots[shotIndex].name), "GuideCapture");
        return finishShot();
    }
    if (writePng(*capture)) {
        ++writtenCount;
    }
    keepNormals(*capture);
    if (shotIndex == kConvergenceShot && kViews[viewIndex].view == DebugView::ShadowedLights) {
        convergence.emplace();
        convergence->extent = capture->extent;
        convergence->reference = decodeLuminance(*capture);
    }
    if (++viewIndex < kViews.size()) {
        return false;
    }
    if (shotIndex == kConvergenceShot && convergence) {
        isConvergenceRunning = true;
        return false;
    }
    return finishShot();
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
    const size_t passed = evaluateNormalChecks();
    const std::string summary = std::format("Normal checks: {} of {} passed", passed, kNormalChecks.size());
    if (passed == kNormalChecks.size()) {
        logInfo(summary, "GuideCapture");
    } else {
        logError(summary, "GuideCapture");
    }
    if (!isConvergencePassed) {
        logError("Convergence check: not run", "GuideCapture");
    } else if (*isConvergencePassed) {
        logInfo("Convergence check: passed", "GuideCapture");
    } else {
        logError("Convergence check: FAILED", "GuideCapture");
    }
    return true;
}

void EngineGuideCapture::keepNormals(const Renderer::DisplayCapture& capture)
{
    if (kViews[viewIndex].mode != IndirectMode::NrcRestirGi) {
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

bool EngineGuideCapture::advanceConvergence(const std::optional<Renderer::DisplayCapture>& capture)
{
    ConvergenceRun& run = *convergence;
    if (run.frame++ < kConvergenceWarmupFrames) {
        return false;
    }
    if (!capture || capture->extent != run.extent) {
        logError("FAIL ReSTIR DI convergence: capture missing or resized", "GuideCapture");
        isConvergenceRunning = false;
        isConvergencePassed = false;
        return true;
    }
    std::vector<float> frame = decodeLuminance(*capture);
    if (run.sum.empty()) {
        run.sum.assign(frame.size(), 0.0);
    }
    for (size_t texel = 0; texel < frame.size(); ++texel) {
        run.sum[texel] += frame[texel];
        run.absoluteErrorSum += std::abs(frame[texel] - run.reference[texel]);
        if (!run.previous.empty()) {
            run.changeSum += std::abs(frame[texel] - run.previous[texel]);
        }
    }
    run.previous = std::move(frame);
    if (++run.capturedFrames < kConvergenceFrames) {
        return false;
    }
    isConvergenceRunning = false;
    isConvergencePassed = evaluateConvergence();
    return true;
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
        logError("FAIL ReSTIR DI convergence: the reference is black", "GuideCapture");
        return false;
    }

    const TileErrors tiles = compareTiles(mean, reference, run.extent, kConvergenceTileSize);
    const double bias = (meanTotal - referenceTotal) / referenceTotal;
    const bool isPassed = std::abs(bias) <= kMaxConvergenceBias && tiles.p90 <= kMaxConvergenceTileP90;
    const std::string detail = std::format(
        "{}, {} frames after {}: bias {:+.3f}%, {}x{} tiles median {:.2f}% / p90 {:.2f}% / max {:.2f}% ({} tiles), "
        "per-frame error {:.2f}%, frame-to-frame change {:.2f}%",
        kShots[kConvergenceShot].name, run.capturedFrames, kConvergenceWarmupFrames, bias * 100.0, kConvergenceTileSize,
        kConvergenceTileSize, tiles.median * 100.0, tiles.p90 * 100.0, tiles.max * 100.0, tiles.count,
        run.absoluteErrorSum / frames / referenceTotal * 100.0,
        run.changeSum / (frames - 1.0) / referenceTotal * 100.0);
    if (isPassed) {
        logInfo(std::format("PASS ReSTIR DI convergence ({})", detail), "GuideCapture");
    } else {
        logError(std::format("FAIL ReSTIR DI convergence ({})", detail), "GuideCapture");
    }
    return isPassed;
}

bool EngineGuideCapture::writePng(const Renderer::DisplayCapture& capture) const
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
    const std::string modeSuffix = shotView.mode == IndirectMode::NrcRestirGi
        ? std::string()
        : std::format("_{}", indirectModeInfo(shotView.mode).id);
    const std::filesystem::path file =
        directory / std::format("{}_{}{}.png", shot.name, fileStem(debugViewInfo(shotView.view).name), modeSuffix);
    const int stride = static_cast<int>(capture.extent.width) * 4;
    if (stbi_write_png(file.string().c_str(), static_cast<int>(capture.extent.width),
                       static_cast<int>(capture.extent.height), 4, pixels.data(), stride) == 0) {
        logError(std::format("could not write {}", file.string()), "GuideCapture");
        return false;
    }
    return true;
}
