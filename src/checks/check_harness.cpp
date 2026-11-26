#include "check_harness.hpp"
#include "../Constants.h"
#include "../static_headers/logger.hpp"
#include "check_passes.hpp"
#include "check_poses.hpp"
#include "core/vk_asset_buffers.hpp"
#include "core/vk_device.hpp"
#include "engine_bench.hpp"
#include "engine_cull_check.hpp"
#include "engine_dlss_check.hpp"
#include "engine_gi_check.hpp"
#include "engine_guide_capture.hpp"
#include "engine_motion_test.hpp"
#include "engine_ray_check.hpp"
#include "engine_temporal_check.hpp"
#include "lab/engine_lab.hpp"
#include "render/vk_renderer.hpp"
#include "render/vk_shader_objects.hpp"
#include "scene/vk_camera.hpp"
#include "scene/vk_scene.hpp"
#include "render/settings_registry.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <stdexcept>
#include <string>

namespace
{
    // the GI check drives its frames through the bench
    constexpr uint32_t kGiCheckBenchFrames = 50'000;
    // ENGINE_BENCH_FIELD spacing, in model bounding diameters
    constexpr float kBenchFieldSpacing = 1.5f;

    [[nodiscard]] bool isEngineConfigSet()
    {
        const char* text = std::getenv("ENGINE_CONFIG");
        return text != nullptr && *text != '\0';
    }
} // namespace

struct CheckHarness::Runners
{
    std::optional<EngineBench> bench;
    std::optional<EngineMotionTest> motionTest;
    std::optional<EngineGuideCapture> guideCapture;
    std::optional<EngineCullCheck> cullCheck;
    std::optional<EngineGiCheck> giCheck;
    // ENGINE_DLSS_CHECK=1, m8 I14 F25
    std::optional<EngineDlssCheck> dlssCheck;
    // one M8 I1 temporal runner (stability, light change, screen edge)
    std::optional<EngineTemporalCheck> temporalCheck;
    // alongside another runner or alone
    std::optional<EngineRayCheck> rayCheck;
    // ENGINE_LAB=<plan>, alone in its process (full_testing.md §8)
    std::optional<EngineLab> lab;
    // set by attach
    std::optional<CheckHostContext> host;
    // the renderer's frame hook; set by attach when a runner is active
    std::unique_ptr<CheckPasses> passes;
    // the frame's runners asked for a display capture
    bool isCaptureRequested = false;

    [[nodiscard]] bool isAutomated() const noexcept
    {
        return bench || motionTest || guideCapture || cullCheck || giCheck || dlssCheck || rayCheck || temporalCheck ||
            lab;
    }
    // no other runner drives the camera or the scene: the ray check holds the startup view and ends the run
    [[nodiscard]] bool isRayCheckStandalone() const noexcept
    {
        return !bench && !motionTest && !guideCapture && !cullCheck && !giCheck && !dlssCheck && !temporalCheck;
    }
    // ENGINE_CONFIG for the interactive engine and bench runs, over the state the other variables set up
    void applyEngineConfig();
    // the guide capture's renderer state; the model swap goes back to Engine
    void applyGuideCaptureRequest(const EngineGuideCapture::Request& guide, CheckRequest& request);
    // after the device idles, if this frame captured
    [[nodiscard]] std::optional<DisplayCapture> takeCapture() const;
};

std::unique_ptr<CheckHarness> CheckHarness::fromEnvironment()
{
    auto runners = std::make_unique<Runners>();
    Runners& r = *runners;
    if (EngineMotionTest::isRequested()) {
        r.motionTest.emplace();
    }
    if (EngineGuideCapture::isRequested()) {
        r.guideCapture.emplace();
    }
    if (EngineCullCheck::isRequested()) {
        r.cullCheck.emplace();
    }
    if (EngineGiCheck::isRequested()) {
        r.giCheck.emplace();
    }
    if (EngineDlssCheck::isRequested()) {
        r.dlssCheck.emplace();
    }
    if (EngineRayCheck::isRequested()) {
        r.rayCheck.emplace();
    }
    if (const std::optional<EngineTemporalCheck::Kind> kind = EngineTemporalCheck::requestedKind()) {
        // each drives the camera and the lighting settings
        if (r.motionTest || r.guideCapture || r.cullCheck || r.giCheck || r.dlssCheck) {
            logError("temporal check ignored: another scene runner is requested", "Engine");
        } else {
            r.temporalCheck.emplace(*kind);
        }
    }

    if (const std::optional<std::string> planName = EngineLab::requestedPlan()) {
        // one tool per process: it measures the shipped state or tries settings, never both (full_testing.md §8)
        if (r.motionTest || r.guideCapture || r.cullCheck || r.giCheck || r.dlssCheck || r.rayCheck ||
            r.temporalCheck || EngineBench::readConfig(0)) {
            throw std::runtime_error("ENGINE_LAB refused: an engine check or bench variable is set too");
        }
        r.lab.emplace(*planName);
    }

    const uint32_t defaultBenchFrames = r.giCheck ? kGiCheckBenchFrames : 0;
    if (const auto benchConfig = EngineBench::readConfig(defaultBenchFrames)) {
        // TEMPORARY (considerations C-E1-3): the per-bin GPU-AV resolve is not the shipped path, so it never times; the
        // GI check only drives its frames through the bench and stays allowed
        if (!r.giCheck && isGpuAvResolvePerBinRequested()) {
            throw std::runtime_error("bench refused: ENGINE_GPUAV_RESOLVE_PER_BIN is a GPU-AV-only resolve path");
        }
        r.bench.emplace(*benchConfig);
    }

    if (!r.isAutomated() && !isEngineConfigSet()) {
        return nullptr;
    }
    return std::unique_ptr<CheckHarness>(new CheckHarness(std::move(runners)));
}

CheckHarness::CheckHarness(std::unique_ptr<Runners> runners) : runners(std::move(runners)) {}

CheckHarness::~CheckHarness()
{
    if (runners->host) {
        runners->host->renderer.setFrameHook(nullptr);
    }
}

bool CheckHarness::isAutomated() const noexcept { return runners->isAutomated(); }

bool CheckHarness::isJitterEnabledByDefault() const noexcept
{
    // the motion test and the guide normal checks compare frames pixel by pixel, which jitter changes by design
    return !runners->motionTest && !runners->guideCapture && !runners->giCheck;
}

std::filesystem::path CheckHarness::chooseStartupModel(const std::optional<std::filesystem::path>& engineModel,
                                                       const std::filesystem::path& defaultModel) const
{
    const Runners& r = *runners;
    // a check scene (ENGINE_CHECK_SCENE) selects the model of the GI and temporal checks
    if (r.giCheck || r.dlssCheck || r.temporalCheck) {
        if (const char* checkModel = checkSceneModel(readCheckScene())) {
            return std::filesystem::path(ENGINE_MODELS_DIR) / checkModel;
        }
    }
    if (engineModel) {
        return *engineModel;
    }
    if (r.bench && !r.bench->getConfig().modelPath.empty()) {
        return r.bench->getConfig().modelPath;
    }
    return defaultModel;
}

CheckRequest CheckHarness::attach(const CheckHostContext& host)
{
    Runners& r = *runners;
    r.host.emplace(host);
    Camera& camera = host.camera;
    Renderer& renderer = host.renderer;

    const std::string_view benchPose = r.bench ? std::string_view(r.bench->getConfig().pose) : std::string_view();
    if (benchPose == "courtyard") {
        camera.lookAt(kCourtyardPose.position, kCourtyardPose.target);
    }
    // m8 I13's glass poses by name, under their sun (below)
    const auto glassBenchPose = std::ranges::find_if(
        kBistroGlassCheckPoses, [&](const CheckPose& pose) { return r.bench && benchPose == pose.name; });
    if (glassBenchPose != kBistroGlassCheckPoses.end()) {
        camera.lookAt(glassBenchPose->position, glassBenchPose->target);
    }
    // the café interior poses of ENGINE_CHECK_SCENE=bistro_cafe, under the default sun
    const auto cafeBenchPose = std::ranges::find_if(
        kBistroCafeCheckPoses, [&](const CheckPose& pose) { return r.bench && benchPose == pose.name; });
    if (cafeBenchPose != kBistroCafeCheckPoses.end()) {
        camera.lookAt(cafeBenchPose->position, cafeBenchPose->target);
    }
    if (!benchPose.empty() && benchPose != "courtyard" && glassBenchPose == kBistroGlassCheckPoses.end() &&
        cafeBenchPose == kBistroCafeCheckPoses.end()) {
        logError(std::format("ENGINE_BENCH_POSE='{}' is unknown (courtyard, street windows, bar shelf, cafe tables, "
                             "cafe dining); the startup view stays",
                             benchPose),
                 "Bench");
    }
    r.applyEngineConfig();
    // the guide shots show the glTF samples' own lights, as Khronos renders them
    renderer.getWorldLights().sun.isEnabled = !r.guideCapture;
    if (glassBenchPose != kBistroGlassCheckPoses.end()) {
        renderer.getWorldLights().sun.azimuthDegrees = kBistroGlassSunAzimuthDegrees;
        renderer.getWorldLights().sun.elevationDegrees = kBistroGlassSunElevationDegrees;
    }

    if (r.isAutomated()) {
        r.passes = std::make_unique<CheckPasses>(host.device, host.allocator, host.shaderObjects);
        if (r.motionTest) {
            r.passes->enableMotionCheck();
            logInfo("Motion test: scripted motion-vector checks, results follow", "MotionTest");
        }
        if (r.rayCheck) {
            r.passes->enableRayCheck();
            logInfo("Ray check: primary rays against the vis buffer every frame, results follow", "RayCheck");
        }
        renderer.setFrameHook(r.passes.get());
    }

    CheckRequest request;
    if (r.bench && r.bench->getConfig().fieldSide > 0) {
        request.instanceField = CheckRequest::InstanceField{
            .center = glm::vec3(0.0f), .side = r.bench->getConfig().fieldSide, .spacing = kBenchFieldSpacing};
        request.logSubsystem = "Bench";
    }
    return request;
}

void CheckHarness::onInstanceFieldSpawned(EntityId firstEntity, uint32_t count, std::string_view status)
{
    Runners& r = *runners;
    if (r.bench && count > 0) {
        const ModelId fieldModel = r.host->scene.objectStorage.modelIds[firstEntity];
        const float cellSize = kBenchFieldSpacing * 2.0f * r.host->assets.models()[fieldModel].localBounds.w;
        r.bench->setFieldMotion(firstEntity, count, static_cast<uint32_t>(r.bench->getConfig().fieldSide), 0.0f,
                                cellSize);
    }
    logInfo(status, "Bench");
}

CheckRequest CheckHarness::beginFrame()
{
    Runners& r = *runners;
    Camera& camera = r.host->camera;
    Renderer& renderer = r.host->renderer;
    ObjectStorage& objects = r.host->scene.objectStorage;
    CheckRequest request;
    r.isCaptureRequested = false;
    // runners read entity 0's bounds before Engine applies their swap
    const auto modelBounds = [&objects] {
        return objects.size() > 0 ? std::optional(objects.localBounds[0]) : std::nullopt;
    };
    const auto requestCapture = [&r] {
        r.isCaptureRequested = true;
        r.passes->requestDisplayCapture();
    };

    if (r.motionTest) {
        const EngineMotionTest::Request motion = r.motionTest->beginFrame(camera, objects);
        if (motion.loadModelAt) {
            request.load = CheckRequest::Load{.model = INSTANCE_FIELD_MODEL_PATH, .position = *motion.loadModelAt};
        }
        request.windowSize = motion.windowSize;
        request.logSubsystem = "MotionTest";
    }
    if (r.guideCapture) {
        const EngineGuideCapture::Request guide =
            r.guideCapture->beginFrame(camera, renderer.debugView, renderer.getRtxdi().settings, modelBounds());
        r.applyGuideCaptureRequest(guide, request);
    }
    if (r.cullCheck &&
        r.cullCheck->beginFrame(camera, renderer.getCulling().settings, renderer.debugView).shouldCapture) {
        requestCapture();
    }
    if (r.giCheck &&
        r.giCheck->beginFrame(camera, renderer.getRtxdi(), renderer.indirectMode, renderer.debugView).shouldCapture) {
        requestCapture();
    }
    if (r.dlssCheck && r.dlssCheck->beginFrame(camera, renderer).shouldCapture) {
        requestCapture();
    }
    if (r.temporalCheck && r.temporalCheck->beginFrame(camera, renderer).shouldCapture) {
        requestCapture();
    }
    if (r.lab) {
        const EngineLab::Request lab = r.lab->beginFrame(camera, renderer, modelBounds());
        if (lab.model) {
            request.replaceModels = *lab.model;
            request.logSubsystem = "Lab";
        }
        if (lab.shouldCapture) {
            requestCapture();
        }
    }
    if (r.bench) {
        r.bench->stepField(objects);
    }
    return request;
}

bool CheckHarness::endFrame()
{
    Runners& r = *runners;
    const vk::raii::Device& device = r.host->device.vkdevice;
    Renderer& renderer = r.host->renderer;
    bool quit = false;
    if (r.motionTest) {
        // the check reads this frame's stats; speed does not matter here
        device.waitIdle();
        quit = r.motionTest->endFrame(r.passes->getLastMotionCheck());
    }
    if (r.guideCapture) {
        quit = r.guideCapture->endFrame(r.takeCapture()) || quit;
    }
    if (r.cullCheck) {
        quit = r.cullCheck->endFrame(r.takeCapture()) || quit;
    }
    if (r.giCheck) {
        quit = r.giCheck->endFrame(r.takeCapture()) || quit;
    }
    if (r.dlssCheck) {
        quit = r.dlssCheck->endFrame(r.takeCapture()) || quit;
    }
    if (r.temporalCheck) {
        quit = r.temporalCheck->endFrame(r.host->camera, r.takeCapture()) || quit;
    }
    if (r.lab) {
        quit = r.lab->endFrame(r.host->camera, r.takeCapture()) || quit;
    }
    if (r.rayCheck) {
        device.waitIdle();
        quit = r.rayCheck->endFrame(r.passes->getLastRayCheck(), r.isRayCheckStandalone()) || quit;
    }
    if (r.bench) {
        quit = r.bench->onFrame(renderer.getGpuTimer(), renderer.getCulling().getStats(), renderer.getRayTracingStats(),
                                renderer.getLightPrepStats(), renderer.getCpuStats(), renderer.getGlassCounters()) ||
            quit;
    }
    return quit;
}

bool CheckHarness::finish()
{
    const Runners& r = *runners;
    if (r.rayCheck) {
        r.rayCheck->logSummary();
    }
    bool isPassed = true;
    if (r.motionTest) {
        isPassed = r.motionTest->isPassed() && isPassed;
    }
    if (r.guideCapture) {
        isPassed = r.guideCapture->isPassed() && isPassed;
    }
    if (r.cullCheck) {
        isPassed = r.cullCheck->isPassed() && isPassed;
    }
    if (r.giCheck) {
        isPassed = r.giCheck->isPassed() && isPassed;
    }
    if (r.dlssCheck) {
        isPassed = r.dlssCheck->isPassed() && isPassed;
    }
    if (r.rayCheck) {
        isPassed = r.rayCheck->isPassed() && isPassed;
    }
    if (r.temporalCheck) {
        isPassed = r.temporalCheck->isPassed() && isPassed;
    }
    if (r.lab) {
        isPassed = r.lab->isPassed() && isPassed;
    }
    return isPassed;
}

void CheckHarness::Runners::applyEngineConfig()
{
    const char* text = std::getenv("ENGINE_CONFIG");
    if (text == nullptr || *text == '\0') {
        return;
    }
    Renderer& renderer = host->renderer;
    Camera& camera = host->camera;
    const LabConfigParse parse = parseLabConfig(text, captureLabConfig(renderer, camera));
    // the checks measure the shipped state; a lab plan sets its own configs (full_testing.md §0)
    if (motionTest || guideCapture || cullCheck || giCheck || rayCheck || temporalCheck || lab) {
        logInfo("ENGINE_CONFIG ignored: engine checks and lab plans set their own settings", "Engine");
        return;
    }
    if (!parse.config) {
        throw std::runtime_error(std::format("ENGINE_CONFIG: {}", parse.error));
    }
    applyLabConfig(*parse.config, renderer, camera);
    logInfo(std::format("ENGINE_CONFIG applied: {}", formatLabConfig(*parse.config)), "Engine");
}

void CheckHarness::Runners::applyGuideCaptureRequest(const EngineGuideCapture::Request& guide, CheckRequest& request)
{
    Renderer& renderer = host->renderer;
    if (guide.model) {
        request.replaceModels = *guide.model;
        request.logSubsystem = "GuideCapture";
    }
    WorldLights::Sun& sun = renderer.getWorldLights().sun;
    sun.isEnabled = guide.sun.has_value();
    if (guide.sun) {
        sun.azimuthDegrees = guide.sun->azimuthDegrees;
        sun.elevationDegrees = guide.sun->elevationDegrees;
    }
    renderer.indirectMode = guide.indirectMode;
    renderer.getRtxdi().isReference = guide.isRtxdiReference;
    renderer.getWorldLights().areTriangleLightsSampled = guide.areTriangleLightsSampled;
    renderer.glass = guide.glass;
    renderer.getRtxdi().frameIndexOverride = guide.frameIndex;
    host->camera.jitterPhaseOverride = guide.jitterPhase;
    PostProcess::Settings& post = renderer.getPost().settings;
    post.isAutoExposure = !guide.manualEv.has_value();
    post.manualEv = guide.manualEv.value_or(post.manualEv);
    if (guide.shouldCapture) {
        isCaptureRequested = true;
        passes->requestDisplayCapture();
    }
}

std::optional<DisplayCapture> CheckHarness::Runners::takeCapture() const
{
    if (!isCaptureRequested) {
        return std::nullopt;
    }
    host->device.vkdevice.waitIdle();
    return passes->getLastDisplayCapture();
}
