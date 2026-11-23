#include "engine_temporal_check.hpp"
#include "../static_headers/logger.hpp"
#include "check_images.hpp"
#include "convergence_metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

namespace
{
    constexpr std::array<std::pair<const char*, EngineTemporalCheck::Kind>, 4> kKindVariables = {{
        {"ENGINE_STABILITY_CHECK", EngineTemporalCheck::Kind::Stability},
        {"ENGINE_LIGHT_CHANGE_CHECK", EngineTemporalCheck::Kind::LightChange},
        {"ENGINE_SCREEN_EDGE_CHECK", EngineTemporalCheck::Kind::ScreenEdge},
        {"ENGINE_EMISSIVE_CHECK", EngineTemporalCheck::Kind::EmissiveRef},
    }};

    bool isVariableSet(const char* name)
    {
        const char* text = std::getenv(name);
        return text != nullptr && std::string_view(text) == "1";
    }

    // frames after the change before the run of passing blocks that lasts to the end; frames + 1 when the last block
    // fails. Non-overlapping blocks: a sliding window's many overlapping tests would trip on noise alone
    template <typename IsBlockPassed>
    uint32_t settledFrames(uint32_t frames, uint32_t block, const IsBlockPassed& isBlockPassed)
    {
        const uint32_t blocks = frames / block;
        for (uint32_t index = blocks; index-- > 0;) {
            if (!isBlockPassed(index * block)) {
                return index + 1 == blocks ? frames + 1 : (index + 1) * block;
            }
        }
        return blocks == 0 ? frames + 1 : 0;
    }

    std::string settledText(uint32_t settled, uint32_t frames)
    {
        return settled > frames ? std::format("> {}", frames) : std::format("{}", settled);
    }

    // relative std of the means of consecutive blocks of a static, converged sequence
    double blockNoise(std::span<const double> values, uint32_t block, double mean)
    {
        const size_t blocks = values.size() / block;
        if (blocks < 2 || mean <= 0.0) {
            return 0.0;
        }
        double squared = 0.0;
        for (size_t index = 0; index < blocks; ++index) {
            double total = 0.0;
            for (size_t frame = index * block; frame < (index + 1) * block; ++frame) {
                total += values[frame];
            }
            const double error = total / block - mean;
            squared += error * error;
        }
        return std::sqrt(squared / static_cast<double>(blocks - 1)) / mean;
    }
} // namespace

std::optional<EngineTemporalCheck::Kind> EngineTemporalCheck::requestedKind()
{
    std::optional<Kind> requested;
    for (const auto& [name, kind] : kKindVariables) {
        if (!isVariableSet(name)) {
            continue;
        }
        if (requested) {
            logError(std::format("{}=1 ignored: one temporal runner per run", name), "TemporalCheck");
        } else {
            requested = kind;
        }
    }
    return requested;
}

const char* EngineTemporalCheck::kindName(Kind kind)
{
    switch (kind) {
    case Kind::Stability:
        return "stability";
    case Kind::LightChange:
        return "light change";
    case Kind::ScreenEdge:
        return "screen edge";
    case Kind::EmissiveRef:
        return "emissive reference";
    }
    return "";
}

EngineTemporalCheck::EngineTemporalCheck(Kind kind) : kind(kind), steps(buildSteps(kind, scene))
{
    // a filter keeps the stability steps whose label contains one of its '|'-separated parts; the other kinds' steps
    // depend on each other
    if (const char* filter = std::getenv("ENGINE_CHECK_STEPS"); filter != nullptr && kind == Kind::Stability) {
        std::erase_if(steps, [&](const Step& step) {
            const std::string label = stepLabel(step);
            for (const auto part : std::views::split(std::string_view(filter), '|')) {
                if (label.contains(std::string_view(part))) {
                    return false;
                }
            }
            return true;
        });
    }
    logInfo(std::format("{} check: {} steps, results follow", kindName(kind), steps.size()), "TemporalCheck");
}

const char* EngineTemporalCheck::glassStateName(GlassState state)
{
    switch (state) {
    case GlassState::Shipped:
        return "shipped";
    case GlassState::PassOff:
        return "pass off";
    case GlassState::ShadowTransmittance:
        return "shadow transmittance";
    case GlassState::BlendGuides:
        return "blended guides";
    }
    return "";
}

GlassSettings EngineTemporalCheck::glassSettingsFor(GlassState state, const GlassSettings& shipped)
{
    GlassSettings settings = shipped;
    switch (state) {
    case GlassState::Shipped:
        break;
    case GlassState::PassOff:
        settings.isPassEnabled = false;
        break;
    case GlassState::ShadowTransmittance:
        settings.isShadowTransmittanceEnabled = true;
        break;
    case GlassState::BlendGuides:
        settings.isGuideBlendEnabled = true;
        break;
    }
    return settings;
}

const char* EngineTemporalCheck::diSetName(DiSet set)
{
    switch (set) {
    case DiSet::Shipped:
        return "shipped";
    case DiSet::Nvidia:
        return "NVIDIA";
    case DiSet::Converged:
        return "converged";
    case DiSet::Reference:
        return "reference";
    case DiSet::ShippedSpatial:
        return "shipped + spatial reuse";
    case DiSet::ShippedPermutation:
        return "shipped + permutation";
    case DiSet::ShippedRegir:
        return "shipped + ReGIR";
    case DiSet::ShippedSpatialPermutation:
        return "shipped + spatial reuse + permutation";
    }
    return "";
}

RtxdiContext::Settings EngineTemporalCheck::diSettingsFor(DiSet set, const RtxdiContext::Settings& shipped)
{
    RtxdiContext::Settings settings = shipped;
    switch (set) {
    case DiSet::Shipped:
    case DiSet::Reference:
        break;
    case DiSet::Nvidia:
        return RtxdiContext::nvidiaSettings();
    case DiSet::Converged:
        return RtxdiContext::convergedSettings();
    case DiSet::ShippedSpatial:
        settings.spatialSamples = RtxdiContext::nvidiaSettings().spatialSamples;
        break;
    case DiSet::ShippedPermutation:
        settings.isPermutationSamplingEnabled = true;
        break;
    case DiSet::ShippedRegir:
        settings.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS;
        break;
    case DiSet::ShippedSpatialPermutation:
        settings.spatialSamples = RtxdiContext::nvidiaSettings().spatialSamples;
        settings.isPermutationSamplingEnabled = true;
        break;
    }
    return settings;
}

std::vector<EngineTemporalCheck::Step> EngineTemporalCheck::buildSteps(Kind kind, CheckScene scene)
{
    std::vector<Step> steps;
    const auto add = [&steps](Step step) { steps.push_back(step); };
    const auto stability = [&add](uint8_t pose, GiSettingsSet set, DebugView view, DiSet di = DiSet::Shipped,
                                  IndirectMode mode = IndirectMode::RestirGi) {
        add({StepType::Stability, pose, set, view, SunState::Default, SunState::Default, Motion::Pan, false, 300,
             di == DiSet::Reference ? kDirectReferenceFrames : 480, kNoChange, di, mode});
    };
    for (uint8_t pose = 0; pose < checkPoses(scene).size(); ++pose) {
        // m8 I13: the glass poses per glass state; no light change, the café runner covers it
        if (scene == CheckScene::Bistro) {
            constexpr std::array kGlassStates{GlassState::Shipped, GlassState::PassOff,
                                              GlassState::ShadowTransmittance, GlassState::BlendGuides};
            switch (kind) {
            case Kind::Stability:
                for (const GlassState glass : kGlassStates) {
                    for (const DebugView view : {DebugView::Lit, DebugView::DirectLighting}) {
                        add({StepType::Stability, pose, GiSettingsSet::Shipped, view, SunState::Default,
                             SunState::Default, Motion::Pan, false, 300, 480, kNoChange, DiSet::Shipped,
                             IndirectMode::RestirGi, glass});
                    }
                }
                break;
            case Kind::LightChange:
            case Kind::EmissiveRef:
                break;
            case Kind::ScreenEdge:
                for (const Motion motion : {Motion::Pan, Motion::Dolly}) {
                    for (const bool isFinal : {false, true}) {
                        add({StepType::Depth, pose, GiSettingsSet::Shipped, DebugView::Depth, SunState::Default,
                             SunState::Default, motion, isFinal, 0, 1, kNoChange});
                    }
                    for (const GlassState glass : kGlassStates) {
                        add({StepType::EdgeReference, pose, GiSettingsSet::Shipped, DebugView::Lit, SunState::Default,
                             SunState::Default, motion, true, 256, 256, 128, DiSet::Shipped, IndirectMode::RestirGi,
                             glass});
                        add({StepType::EdgeMove, pose, GiSettingsSet::Shipped, DebugView::Lit, SunState::Default,
                             SunState::Default, motion, false, 256, 256, 0, DiSet::Shipped, IndirectMode::RestirGi,
                             glass});
                    }
                }
                break;
            }
            continue;
        }
        switch (kind) {
        case Kind::Stability:
            if (scene == CheckScene::BistroCafe) {
                // DI first: its reference, then each DI set against it, then through DLSS-RR
                stability(pose, GiSettingsSet::Shipped, DebugView::DirectLighting, DiSet::Reference);
                for (const DiSet di : {DiSet::Shipped, DiSet::Nvidia, DiSet::Converged}) {
                    stability(pose, GiSettingsSet::Shipped, DebugView::DirectLighting, di);
                }
                stability(pose, GiSettingsSet::Shipped, DebugView::Lit, DiSet::Nvidia);
                stability(pose, GiSettingsSet::Nvidia, DebugView::Lit, DiSet::Nvidia);
                // which of I6's changes to NVIDIA's DI the Lit noise comes from
                for (const DiSet di : {DiSet::ShippedSpatial, DiSet::ShippedPermutation, DiSet::ShippedRegir,
                                       DiSet::ShippedSpatialPermutation}) {
                    for (const DebugView view : {DebugView::DirectLighting, DebugView::Lit}) {
                        stability(pose, GiSettingsSet::Shipped, view, di);
                    }
                }
                // the GI part of the Lit noise once DI is decorrelated
                for (const GiSettingsSet set : {GiSettingsSet::ShippedNoDecorrelation, GiSettingsSet::Nvidia}) {
                    stability(pose, set, DebugView::Lit, DiSet::ShippedSpatialPermutation);
                }
                for (const IndirectMode mode : {IndirectMode::NrcRestirGi, IndirectMode::Nrc}) {
                    stability(pose, GiSettingsSet::Shipped, DebugView::Lit, DiSet::ShippedSpatialPermutation, mode);
                }
                for (const GiSettingsSet set :
                     {GiSettingsSet::Shipped, GiSettingsSet::ShippedNoDecorrelation,
                      GiSettingsSet::ShippedNoFireflyReplacement, GiSettingsSet::ShippedAgeLimit, GiSettingsSet::Nvidia,
                      GiSettingsSet::Converged}) {
                    for (const DebugView view : {DebugView::Lit, DebugView::IndirectRadiance}) {
                        stability(pose, set, view);
                    }
                }
                for (const IndirectMode mode : {IndirectMode::NrcRestirGi, IndirectMode::Nrc}) {
                    for (const DebugView view : {DebugView::Lit, DebugView::IndirectRadiance}) {
                        stability(pose, GiSettingsSet::Shipped, view, DiSet::Shipped, mode);
                    }
                }
                break;
            }
            for (const GiSettingsSet set :
                 {GiSettingsSet::Shipped, GiSettingsSet::ShippedNoDecorrelation, GiSettingsSet::ShippedNoFireflyReplacement,
                  GiSettingsSet::ShippedNoRrProfile, GiSettingsSet::ShippedAgeLimit, GiSettingsSet::Nvidia,
                  GiSettingsSet::Converged}) {
                for (const DebugView view : {DebugView::Lit, DebugView::IndirectRadiance}) {
                    add({StepType::Stability, pose, set, view, SunState::Default, SunState::Default, Motion::Pan,
                         false, 300, 480, kNoChange});
                }
            }
            break;
        case Kind::LightChange:
            for (const GiSettingsSet set : {GiSettingsSet::Shipped, GiSettingsSet::ShippedAgeLimit, GiSettingsSet::Nvidia,
                                            GiSettingsSet::Converged}) {
                for (const auto& [before, after] :
                     {std::pair{SunState::Default, SunState::Moved}, std::pair{SunState::Off, SunState::Default}}) {
                    add({StepType::LightReference, pose, set, DebugView::IndirectRadiance, after, after, Motion::Pan,
                         false, 256, 256, kNoChange});
                    add({StepType::LightChange, pose, set, DebugView::IndirectRadiance, before, after, Motion::Pan,
                         false, 448, 64 + 512, 64});
                }
            }
            break;
        case Kind::ScreenEdge:
            for (const Motion motion : {Motion::Pan, Motion::Dolly}) {
                for (const bool isFinal : {false, true}) {
                    add({StepType::Depth, pose, GiSettingsSet::Shipped, DebugView::Depth, SunState::Default,
                         SunState::Default, motion, isFinal, 0, 1, kNoChange});
                }
                for (const GiSettingsSet set : {GiSettingsSet::Shipped, GiSettingsSet::ShippedAgeLimit, GiSettingsSet::Nvidia}) {
                    for (const DebugView view :
                         {DebugView::Lit, DebugView::DirectLighting, DebugView::IndirectRadiance}) {
                        add({StepType::EdgeReference, pose, set, view, SunState::Default, SunState::Default, motion,
                             true, 256, 256, 128});
                        add({StepType::EdgeMove, pose, set, view, SunState::Default, SunState::Default, motion, false,
                             256, 256, 0});
                    }
                }
            }
            break;
        case Kind::EmissiveRef:
            // independent exact textured-emission reference (BRDF-only, areTriangleLightsSampled=false, glass off):
            // DlssInput = 0 + exact_BRDF_direct + primary_emissive, matching DirectLighting = Power_RIS_direct + primary_emissive
            add({StepType::EmissiveReference, pose, GiSettingsSet::Converged, DebugView::DlssInput, SunState::Off,
                 SunState::Off, Motion::Pan, false, 16, 2048, kNoChange, DiSet::Reference, IndirectMode::RestirGi,
                 GlassState::PassOff});
            add({StepType::EmissiveCompare, pose, GiSettingsSet::Converged, DebugView::DirectLighting, SunState::Off,
                 SunState::Off, Motion::Pan, false, 16, 1024, kNoChange, DiSet::Reference, IndirectMode::RestirGi,
                 GlassState::PassOff});
            break;
        }
    }
    return steps;
}

uint32_t EngineTemporalCheck::stepSeed(const Step& step)
{
    // from what the step is, not where it sits: adding or reordering steps changes no other step's realization
    uint64_t key = std::to_underlying(step.type);
    for (const uint64_t field :
         {uint64_t{step.pose}, uint64_t{std::to_underlying(step.set)}, uint64_t{std::to_underlying(step.view)},
          uint64_t{std::to_underlying(step.sun)}, uint64_t{std::to_underlying(step.sunAfter)},
          uint64_t{std::to_underlying(step.motion)}, uint64_t{step.isFinalPose}}) {
        key = key * 64 + field;
    }
    // only when set: the steps before the DI sets and modes keep their seeds
    if (step.di != DiSet::Shipped || step.mode != IndirectMode::RestirGi) {
        key = (key * 64 + std::to_underlying(step.di)) * 64 + std::to_underlying(step.mode);
    }
    // murmur3 finalizer
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdull;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ull;
    key ^= key >> 33;
    // even, below 2^32 minus a step: RTXDI's buffer parity follows the frame index
    return static_cast<uint32_t>(key) % (UINT32_MAX - kStepSeedStride) & ~1u;
}

WorldLights::Sun EngineTemporalCheck::sunFor(SunState state) const
{
    WorldLights::Sun sun = WorldLights::defaultSun();
    if (scene == CheckScene::Bistro) {
        sun.azimuthDegrees = kBistroGlassSunAzimuthDegrees;
        sun.elevationDegrees = kBistroGlassSunElevationDegrees;
    }
    if (state == SunState::Moved) {
        sun.azimuthDegrees += kSunMoveDegrees;
    } else if (state == SunState::Off) {
        sun.isEnabled = false;
    }
    return sun;
}

EngineTemporalCheck::PoseView EngineTemporalCheck::startPose(const PoseView& pose, Motion motion)
{
    const glm::vec3 forward = glm::normalize(pose.target - pose.position);
    if (motion == Motion::Pan) {
        const float angle = -glm::radians(kPanDegrees);
        const glm::vec3 turned{forward.x * std::cos(angle) + forward.z * std::sin(angle), forward.y,
                               -forward.x * std::sin(angle) + forward.z * std::cos(angle)};
        return {pose.position, pose.position + turned};
    }
    // ahead of the pose, inside its frustum: clear of geometry, which the space behind a pose need not be
    const glm::vec3 position = pose.position + forward * kDollyMeters;
    return {position, position + forward};
}

std::string EngineTemporalCheck::stepLabel(const Step& step) const
{
    const char* view = kDebugViews[std::to_underlying(step.view)].name;
    const char* pose = checkPoses(scene)[step.pose].name;
    const std::string glass =
        step.glass == GlassState::Shipped ? std::string() : std::format(", glass {}", glassStateName(step.glass));
    switch (step.type) {
    case StepType::Stability: {
        const std::string di = step.di == DiSet::Shipped ? std::string() : std::format(", DI {}", diSetName(step.di));
        const std::string mode = step.mode == IndirectMode::RestirGi
            ? std::string()
            : std::format(", {}", indirectModeInfo(step.mode).label);
        return std::format("stability, {}, {}{}{}{}, {}", pose, giSettingsSetName(step.set), di, mode, glass,
                           view);
    }
    case StepType::LightReference:
    case StepType::LightChange:
        return std::format("light change, {}, {}, {}", pose, giSettingsSetName(step.set),
                           step.sun == SunState::Off ? "sun toggle (off -> on)" : "sun move (azimuth +30 deg)");
    case StepType::Depth:
    case StepType::EdgeReference:
    case StepType::EdgeMove:
        return std::format("screen edge, {}, {}, {}{}, {}", pose,
                           step.motion == Motion::Pan ? "pan 10 deg" : "dolly back 1 m", giSettingsSetName(step.set),
                           glass, view);
    case StepType::EmissiveReference:
        return std::format("emissive ref (BRDF-only exact), {}, {}", pose, view);
    case StepType::EmissiveCompare:
        return std::format("emissive compare (Power_RIS DI), {}, {}", pose, view);
    }
    return "";
}

void EngineTemporalCheck::startStep(Camera& camera, Renderer& renderer)
{
    const Step& step = steps[stepIndex];
    RtxdiContext& rtxdi = renderer.getRtxdi();
    if (!startupDiSettings) {
        startupDiSettings = rtxdi.settings;
    }
    renderer.indirectMode = step.mode;
    rtxdi.isReference = step.di == DiSet::Reference;
    rtxdi.settings = diSettingsFor(step.di, *startupDiSettings);
    rtxdi.isGiReference = step.type == StepType::EmissiveReference;
    rtxdi.giReferenceBounces = 0;
    rtxdi.giSettings = giSettingsFor(step.set);
    if (!startupGlassSettings) {
        startupGlassSettings = renderer.glass;
    }
    renderer.glass = glassSettingsFor(step.glass, *startupGlassSettings);
    renderer.debugView.view = step.view;
    renderer.getWorldLights().sun = sunFor(step.sun);
    renderer.getWorldLights().areTriangleLightsSampled = step.type != StepType::EmissiveReference;
    const PoseView& canonical = poses[step.pose];
    const bool isStartPose = step.type == StepType::EdgeMove || (step.type == StepType::Depth && !step.isFinalPose);
    const PoseView pose = isStartPose ? startPose(canonical, step.motion) : canonical;
    camera.lookAt(pose.position, pose.target);

    PostProcess& post = renderer.getPost();
    post.requestSnap();
    // the move compares against its reference's image at one exposure
    if (step.type == StepType::EdgeMove && step.view == DebugView::Lit) {
        const float exposure = post.getLastExposure().exposure;
        if (exposure > 0.0f) {
            pinnedExposure = exposure;
        }
    } else {
        pinnedExposure.reset();
    }
    post.settings.isAutoExposure = !pinnedExposure.has_value();
    if (pinnedExposure) {
        post.settings.manualEv = std::log2(*pinnedExposure);
    }
}

EngineTemporalCheck::Request EngineTemporalCheck::beginFrame(Camera& camera, Renderer& renderer)
{
    Request request;
    RtxdiContext& rtxdi = renderer.getRtxdi();
    if (isFinished) {
        rtxdi.frameIndexOverride.reset();
        camera.jitterPhaseOverride.reset();
        return request;
    }
    renderer.fixedFrameDeltaMs = kFrameDeltaMs;
    if (frame < kStartupFrames) {
        return request;
    }
    if (!hasStartupPose) {
        // the startup camera after its first frames: forward is current
        for (size_t index = 0; index < poses.size(); ++index) {
            const CheckPose& pose = checkPoses(scene)[index];
            poses[index] = pose.isStartup
                ? PoseView{camera.cameraData.cameraPos, camera.cameraData.cameraPos + camera.getForward()}
                : PoseView{pose.position, pose.target};
        }
        hasStartupPose = true;
    }
    const Step& step = steps[stepIndex];
    if (stepFrame == 0) {
        startStep(camera, renderer);
    }
    if (step.changeAt != kNoChange && stepFrame == step.warmupFrames + step.changeAt) {
        if (step.type == StepType::LightChange) {
            renderer.getWorldLights().sun = sunFor(step.sunAfter);
        } else if (step.type == StepType::EdgeMove) {
            const PoseView& pose = poses[step.pose];
            camera.lookAt(pose.position, pose.target, false);
        }
    }
    rtxdi.frameIndexOverride = stepSeed(step) + stepFrame;
    camera.jitterPhaseOverride = stepFrame;
    isCaptureRequested = stepFrame >= step.warmupFrames;
    request.shouldCapture = isCaptureRequested;
    return request;
}

bool EngineTemporalCheck::endFrame(const Camera& camera, const std::optional<Renderer::DisplayCapture>& capture)
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
        if (!capture || (capturedFrames > 0 && capture->extent != extent)) {
            isCaptureMissing = true;
        } else {
            accumulate(camera, *capture);
        }
    }
    const Step& step = steps[stepIndex];
    if (++stepFrame < step.warmupFrames + step.frames) {
        return false;
    }
    finishStep();
    stepFrame = 0;
    if (++stepIndex < steps.size()) {
        return false;
    }
    const std::string summary =
        std::format("{} check: {} steps, {} failed", kindName(kind), steps.size(), failedCount);
    if (failedCount == 0) {
        logInfo(summary, "TemporalCheck");
    } else {
        logError(summary, "TemporalCheck");
    }
    isFinished = true;
    return true;
}

void EngineTemporalCheck::accumulate(const Camera& camera, const Renderer::DisplayCapture& capture)
{
    const Step& step = steps[stepIndex];
    const DecodedFrame decoded = decodeFrame(capture);
    const size_t texels = decoded.luminance.size();
    if (capturedFrames == 0) {
        extent = capture.extent;
        if (step.type == StepType::Stability && isCheckPngRequested()) {
            writeCheckCapturePng(std::format("{} frame", stepLabel(step)), capture);
        }
    }
    nonFinitePixels += decoded.nonFinitePixels;
    double frameSum = 0.0;
    for (const float value : decoded.luminance) {
        frameSum += value;
    }
    frameMeans.push_back(texels > 0 ? frameSum / static_cast<double>(texels) : 0.0);

    switch (step.type) {
    case StepType::Stability:
    case StepType::EmissiveReference:
    case StepType::EmissiveCompare:
        if (sum.empty()) {
            sum.assign(texels, 0.0);
            squaredSum.assign(texels, 0.0);
        }
        for (size_t texel = 0; texel < texels; ++texel) {
            const double value = decoded.luminance[texel];
            sum[texel] += value;
            squaredSum[texel] += value * value;
            if (!previous.empty()) {
                frameChange += std::abs(value - previous[texel]);
            }
        }
        previous = decoded.luminance;
        break;
    case StepType::LightReference:
    case StepType::LightChange:
        break;
    case StepType::Depth: {
        DepthMap& depth = step.isFinalPose ? finalDepth : initialDepth;
        const GpuCameraData& data = camera.cameraData;
        depth.extent = capture.extent;
        depth.view = data.view;
        depth.invView = data.invView;
        depth.invProj = data.invProj;
        depth.viewProj = data.viewProj;
        depth.viewDepth.resize(texels);
        // inverts debug_view.slang depthShade: 1 - log(z / near) / log(far / near)
        const float depthRange = data.farZ / data.nearZ;
        for (size_t texel = 0; texel < texels; ++texel) {
            depth.viewDepth[texel] = data.nearZ * std::pow(depthRange, 1.0f - decoded.luminance[texel]);
        }
        break;
    }
    case StepType::EdgeReference:
        // the first changeAt frames average into the reference; the rest measure the static error against it
        if (capturedFrames < step.changeAt) {
            if (sum.empty()) {
                sum.assign(texels, 0.0);
            }
            for (size_t texel = 0; texel < texels; ++texel) {
                sum[texel] += decoded.luminance[texel];
            }
            break;
        }
        if (capturedFrames == step.changeAt) {
            referenceExtent = extent;
            reference.resize(sum.size());
            for (size_t texel = 0; texel < sum.size(); ++texel) {
                reference[texel] = sum[texel] / step.changeAt;
            }
        }
        accumulateEdge(decoded.luminance, capture.extent);
        break;
    case StepType::EdgeMove:
        accumulateEdge(decoded.luminance, capture.extent);
        break;
    }
    ++capturedFrames;
}

void EngineTemporalCheck::accumulateEdge(const std::vector<float>& luminance, vk::Extent2D captureExtent)
{
    if (masks.empty() || captureExtent != referenceExtent) {
        isCaptureMissing = true;
        return;
    }
    const vk::Extent2D maskExtent = finalDepth.extent;
    FrameRegions regions{};
    for (uint32_t y = 0; y < captureExtent.height; ++y) {
        const uint32_t maskY = y * maskExtent.height / captureExtent.height;
        for (uint32_t x = 0; x < captureExtent.width; ++x) {
            const uint32_t maskX = x * maskExtent.width / captureExtent.width;
            const size_t texel = size_t{y} * captureExtent.width + x;
            RegionSums& sums = regions[std::to_underlying(masks[size_t{maskY} * maskExtent.width + maskX])];
            sums.value += luminance[texel];
            sums.reference += reference[texel];
            sums.absoluteError += std::abs(luminance[texel] - reference[texel]);
        }
    }
    edgeFrames.push_back(regions);
}

void EngineTemporalCheck::buildMasks()
{
    const vk::Extent2D size = finalDepth.extent;
    masks.assign(size_t{size.width} * size.height, Region::Interior);
    if (initialDepth.extent != size || finalDepth.viewDepth.empty()) {
        fail("screen edge depth captures missing or resized");
        masks.clear();
        return;
    }
    for (uint32_t y = 0; y < size.height; ++y) {
        for (uint32_t x = 0; x < size.width; ++x) {
            const size_t texel = size_t{y} * size.width + x;
            const glm::vec2 ndc{(static_cast<float>(x) + 0.5f) / static_cast<float>(size.width) * 2.0f - 1.0f,
                                (static_cast<float>(y) + 0.5f) / static_cast<float>(size.height) * 2.0f - 1.0f};
            const glm::vec4 nearPoint = finalDepth.invProj * glm::vec4(ndc, 1.0f, 1.0f);
            const glm::vec3 viewPoint = glm::vec3(nearPoint) / nearPoint.w;
            const glm::vec3 scaled = viewPoint * (finalDepth.viewDepth[texel] / std::abs(viewPoint.z));
            const glm::vec4 world = finalDepth.invView * glm::vec4(scaled, 1.0f);
            const glm::vec4 clip = initialDepth.viewProj * world;
            const glm::vec2 initialNdc = glm::vec2(clip) / clip.w;
            if (clip.w <= 0.0f || std::abs(initialNdc.x) > 1.0f || std::abs(initialNdc.y) > 1.0f) {
                masks[texel] = Region::Band;
                continue;
            }
            const uint32_t initialX = std::min(
                static_cast<uint32_t>((initialNdc.x * 0.5f + 0.5f) * static_cast<float>(size.width)), size.width - 1);
            const uint32_t initialY = std::min(
                static_cast<uint32_t>((initialNdc.y * 0.5f + 0.5f) * static_cast<float>(size.height)), size.height - 1);
            const float surfaceDepth = initialDepth.viewDepth[size_t{initialY} * size.width + initialX];
            const float pointDepth = std::abs((initialDepth.view * world).z);
            if (pointDepth > surfaceDepth * kDisocclusionScale + kDisocclusionOffsetMeters) {
                masks[texel] = Region::Disoccluded;
            }
        }
    }
}

void EngineTemporalCheck::fail(const std::string& text)
{
    logError(std::format("FAIL {}", text), "TemporalCheck");
    ++failedCount;
}

void EngineTemporalCheck::finishStep()
{
    const Step& step = steps[stepIndex];
    const std::string label = stepLabel(step);
    if (isCaptureMissing || capturedFrames == 0) {
        fail(std::format("{}: capture missing or resized", label));
    } else if (nonFinitePixels > 0) {
        fail(std::format("{}: {} non-finite pixels", label, nonFinitePixels));
    } else {
        switch (step.type) {
        case StepType::Stability:
        case StepType::EmissiveReference:
        case StepType::EmissiveCompare:
            logStability(step);
            break;
        case StepType::LightReference: {
            double total = 0.0;
            for (const double mean : frameMeans) {
                total += mean;
            }
            lightReference = total / static_cast<double>(frameMeans.size());
            lightTolerance = std::max(kSettledError, kNoiseSigmas * blockNoise(frameMeans, kBlockFrames, lightReference));
            break;
        }
        case StepType::LightChange:
            logLightChange(step);
            break;
        case StepType::Depth:
            if (step.isFinalPose) {
                buildMasks();
            }
            break;
        case StepType::EdgeReference:
            for (size_t region = 0; region < edgeTolerance.size(); ++region) {
                std::vector<double> values(edgeFrames.size());
                RegionSums total;
                for (size_t index = 0; index < edgeFrames.size(); ++index) {
                    const RegionSums& sums = edgeFrames[index][region];
                    values[index] = sums.value;
                    total.value += sums.value;
                    total.reference += sums.reference;
                    total.absoluteError += sums.absoluteError;
                }
                const double mean = total.value / static_cast<double>(values.size());
                edgeTolerance[region] = std::max(kSettledError, kNoiseSigmas * blockNoise(values, kBlockFrames, mean));
                edgeSteadyError[region] = total.reference > 0.0 ? total.absoluteError / total.reference : 0.0;
            }
            break;
        case StepType::EdgeMove:
            logEdgeMove(step);
            break;
        }
    }
    sum.clear();
    squaredSum.clear();
    previous.clear();
    frameChange = 0.0;
    frameMeans.clear();
    edgeFrames.clear();
    capturedFrames = 0;
    nonFinitePixels = 0;
    isCaptureMissing = false;
}

void EngineTemporalCheck::logStability(const Step& step)
{
    const double frames = capturedFrames;
    double signal = 0.0;
    for (const double value : sum) {
        signal += value / frames;
    }
    const double frameMean = signal / static_cast<double>(sum.size());
    const bool isDisplayView = step.view == DebugView::Lit;
    const double litCutoff = isDisplayView ? kLitDisplayCutoff : kLitShare * frameMean;
    double deviation = 0.0;
    size_t litPixels = 0;
    size_t flickeringPixels = 0;
    std::vector<double> means(sum.size());
    std::vector<double> variation(sum.size(), 0.0);
    for (size_t texel = 0; texel < sum.size(); ++texel) {
        const double mean = sum[texel] / frames;
        const double pixelDeviation = std::sqrt(std::max(squaredSum[texel] / frames - mean * mean, 0.0));
        means[texel] = mean;
        deviation += pixelDeviation;
        if (mean > litCutoff) {
            ++litPixels;
            flickeringPixels += pixelDeviation > kFlickerCv * mean ? 1 : 0;
            variation[texel] = pixelDeviation / mean;
        }
    }
    if (isCheckPngRequested()) {
        writeCheckLuminancePng(std::format("{} mean", stepLabel(step)), extent, means, checkPngScale(means));
        // coefficient of variation of lit pixels, white at 100%
        writeCheckUnitPng(std::format("{} cv", stepLabel(step)), extent, variation);
    }
    if (step.view == DebugView::DirectLighting || step.type == StepType::EmissiveReference ||
        step.type == StepType::EmissiveCompare) {
        logDirectBias(step, means);
    }
    const double change = signal > 0.0 ? frameChange / (frames - 1.0) / signal : 0.0;
    // a bright or dark event of a static view: the block mean farthest from the step's mean
    double largestSwing = 0.0;
    for (size_t start = 0; start + kBlockFrames <= frameMeans.size(); start += kBlockFrames) {
        double total = 0.0;
        for (size_t index = start; index < start + kBlockFrames; ++index) {
            total += frameMeans[index];
        }
        largestSwing = std::max(largestSwing, std::abs(total / kBlockFrames - frameMean));
    }
    const std::string cutoff = isDisplayView ? std::format("mean > {:g} display", kLitDisplayCutoff)
                                             : std::format("mean > {:.0f}% of the frame's", 100.0 * kLitShare);
    logInfo(std::format("MEASURED {}: {} frames after {}: mean {:.5g}, temporal std {:.2f}%, flicker {:.2f}% of lit "
                        "px (std > {:.0f}% of mean), lit px {:.1f}% ({}), f2f {:.2f}%, largest {}-frame swing {:.2f}%",
                        stepLabel(step), capturedFrames, step.warmupFrames, frameMean,
                        signal > 0.0 ? 100.0 * deviation / signal : 0.0,
                        litPixels > 0 ? 100.0 * static_cast<double>(flickeringPixels) / static_cast<double>(litPixels)
                                      : 0.0,
                        100.0 * kFlickerCv, 100.0 * static_cast<double>(litPixels) / static_cast<double>(sum.size()),
                        cutoff, 100.0 * change, kBlockFrames, frameMean > 0.0 ? 100.0 * largestSwing / frameMean : 0.0),
            "TemporalCheck");
}

void EngineTemporalCheck::logDirectBias(const Step& step, const std::vector<double>& mean)
{
    if (step.type == StepType::EmissiveReference ||
        (step.type != StepType::EmissiveCompare && step.di == DiSet::Reference)) {
        directReference = mean;
        directReferencePose = step.pose;
        return;
    }
    if (directReferencePose != step.pose || directReference.size() != mean.size()) {
        return;
    }
    double meanTotal = 0.0;
    double referenceTotal = 0.0;
    for (size_t texel = 0; texel < mean.size(); ++texel) {
        meanTotal += mean[texel];
        referenceTotal += directReference[texel];
    }
    if (referenceTotal <= 0.0) {
        return;
    }
    const TileErrors tiles = compareTiles(mean, directReference, extent, kDirectTileSize);
    logInfo(std::format("MEASURED {} against DI reference: bias {:+.2f}%, {}x{} tiles median {:.2f}% / p90 {:.2f}% / "
                        "max {:.2f}% ({} tiles, dark leak {:.3f}% in {} tiles, dim bias {:+.2f}% in {} tiles)",
                        stepLabel(step), (meanTotal - referenceTotal) / referenceTotal * 100.0, kDirectTileSize,
                        kDirectTileSize, tiles.median * 100.0, tiles.p90 * 100.0, tiles.max * 100.0, tiles.count,
                        tiles.zeroRefLeak * 100.0, tiles.zeroRefLeakTiles, tiles.dimBias * 100.0, tiles.dimTiles),
            "TemporalCheck");
    if (isCheckPngRequested()) {
        writeCheckErrorPng(std::format("{} error", stepLabel(step)), extent, mean, directReference,
                           checkPngScale(directReference));
    }
}

void EngineTemporalCheck::logLightChange(const Step& step)
{
    double oldTotal = 0.0;
    for (uint32_t index = 0; index < step.changeAt; ++index) {
        oldTotal += frameMeans[index];
    }
    const double oldMean = oldTotal / step.changeAt;
    const std::vector<double> after(frameMeans.begin() + step.changeAt, frameMeans.end());
    const auto frames = static_cast<uint32_t>(after.size());
    // trailing mean of up to kBlockFrames ending at index
    const auto trailingError = [&](uint32_t index) {
        const uint32_t first = index + 1 >= kBlockFrames ? index + 1 - kBlockFrames : 0;
        double total = 0.0;
        for (uint32_t i = first; i <= index; ++i) {
            total += after[i];
        }
        const double mean = total / static_cast<double>(index - first + 1);
        return lightReference > 0.0 ? (mean - lightReference) / lightReference : 0.0;
    };
    const uint32_t settled = settledFrames(frames, kBlockFrames, [&](uint32_t start) {
        return std::abs(trailingError(start + kBlockFrames - 1)) <= lightTolerance;
    });
    std::string samples;
    for (const uint32_t at : {1u, 4u, 16u, 64u, 256u}) {
        if (at <= frames) {
            samples += std::format("{}{}: {:+.1f}%", samples.empty() ? "" : ", ", at, 100.0 * trailingError(at - 1));
        }
    }
    logInfo(std::format("MEASURED {}: settled within {:.1f}% after {} frames; {}-frame mean error at {}; old mean "
                        "{:.5g}, new converged mean {:.5g}",
                        stepLabel(step), 100.0 * lightTolerance, settledText(settled, frames), kBlockFrames, samples,
                        oldMean, lightReference),
            "TemporalCheck");
}

void EngineTemporalCheck::logEdgeMove(const Step& step)
{
    const auto frames = static_cast<uint32_t>(edgeFrames.size());
    const auto windowSums = [&](uint32_t start, Region region) {
        RegionSums total;
        for (uint32_t index = start; index < std::min(start + kBlockFrames, frames); ++index) {
            const RegionSums& sums = edgeFrames[index][std::to_underlying(region)];
            total.value += sums.value;
            total.reference += sums.reference;
            total.absoluteError += sums.absoluteError;
        }
        return total;
    };
    const auto meanError = [](const RegionSums& sums) {
        return sums.reference > 0.0 ? std::abs(sums.value - sums.reference) / sums.reference : 0.0;
    };
    const auto pixelError = [](const RegionSums& sums) {
        return sums.reference > 0.0 ? sums.absoluteError / sums.reference : 0.0;
    };
    std::array<size_t, std::to_underlying(Region::Count)> regionPixels{};
    for (const Region region : masks) {
        ++regionPixels[std::to_underlying(region)];
    }
    const auto share = [&](Region region) {
        return 100.0 * static_cast<double>(regionPixels[std::to_underlying(region)]) / static_cast<double>(masks.size());
    };
    std::string text = std::format("MEASURED {}: band {:.1f}% / disoccluded {:.1f}% of px", stepLabel(step),
                                   share(Region::Band), share(Region::Disoccluded));
    for (const Region region : {Region::Band, Region::Disoccluded}) {
        const char* name = region == Region::Band ? "band" : "disoccluded";
        // a few hundred pixels near black read noise as error: a pan's depth-test edges
        if (static_cast<double>(regionPixels[std::to_underlying(region)]) <
            kMinRegionShare * static_cast<double>(masks.size())) {
            continue;
        }
        if (edgeFrames.empty() || edgeFrames.front()[std::to_underlying(region)].reference <= 0.0) {
            text += std::format(" | {} black in the reference", name);
            continue;
        }
        const double tolerance = edgeTolerance[std::to_underlying(region)];
        const double steadyError = edgeSteadyError[std::to_underlying(region)];
        const uint32_t settled = settledFrames(frames, kBlockFrames, [&](uint32_t start) {
            const RegionSums sums = windowSums(start, region);
            return meanError(sums) <= tolerance && pixelError(sums) <= kSettledPixelRatio * steadyError;
        });
        std::string samples;
        for (const uint32_t at : {1u, 4u, 16u, 64u, 256u}) {
            if (at <= frames) {
                const RegionSums& sums = edgeFrames[at - 1][std::to_underlying(region)];
                samples += std::format("{}{}: mean {:.1f}%, px {:.1f}%", samples.empty() ? "" : "; ", at,
                                       100.0 * meanError(sums), 100.0 * pixelError(sums));
            }
        }
        text += std::format(" | {} settled after {} frames (mean within {:.1f}%, px error within {:.2f}x the static "
                            "{:.1f}%; {})",
                            name, settledText(settled, frames), 100.0 * tolerance, kSettledPixelRatio,
                            100.0 * steadyError, samples);
    }
    logInfo(text, "TemporalCheck");
}
