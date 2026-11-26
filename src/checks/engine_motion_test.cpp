#include "engine_motion_test.hpp"
#include "../static_headers/logger.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <format>
#include <string_view>

namespace
{
    constexpr size_t kMaxLoggedFailures = 3;
} // namespace

bool EngineMotionTest::isRequested()
{
    const char* text = std::getenv("ENGINE_MOTION_TEST");
    return text != nullptr && std::string_view(text) == "1";
}

void EngineMotionTest::place(ObjectStorage& objects, const Transform& transform)
{
    objects.transforms[kTestEntity] = transform;
    objects.prevModelMatrices[kTestEntity] = computeModelMatrix(transform);
}

EngineMotionTest::Request EngineMotionTest::beginFrame(Camera& camera, ObjectStorage& objects)
{
    Request request;
    if (frame < kWarmupFrames || scenarioIndex >= kScenarios.size()) {
        return request;
    }
    if (!originalTransform && objects.size() > kTestEntity) {
        originalTransform = objects.transforms[kTestEntity];
    }
    const bool hasEntity = originalTransform.has_value();
    const bool isEventFrame = scenarioFrame == kEventFrame;
    const auto rotate = [&](float yawDelta, float pitchDelta) {
        camera.rotate(yawDelta, pitchDelta);
        pitch += pitchDelta;
    };
    if (!startupCameraPosition) {
        startupCameraPosition = camera.cameraData.cameraPos;
    }
    if (scenarioFrame == 0) {
        // a settle frame: neither the turn nor the teleport cut is evaluated
        rotate(0.0f, kScenarios[scenarioIndex].startPitch - pitch);
        const glm::vec3 position = kScenarios[scenarioIndex].needsSky ? kCourtyardCenter : *startupCameraPosition;
        if (camera.cameraData.cameraPos != position) {
            camera.teleportTo(position);
        }
    }
    switch (static_cast<Scenario>(scenarioIndex)) {
    case Scenario::StaticScene:
        break;
    case Scenario::CameraYaw:
    case Scenario::CameraYawSky:
        rotate(4.0f, 0.0f);
        break;
    case Scenario::CameraPitchYaw:
        rotate(3.0f, 2.0f);
        break;
    case Scenario::CameraTranslation:
        camera.moveRight(0.02f);
        camera.moveForward(0.03f);
        break;
    case Scenario::EntityTranslation:
        if (hasEntity) {
            objects.transforms[kTestEntity].position.x += 0.02f;
        }
        break;
    case Scenario::EntityRotation:
    case Scenario::MirroredEntityRotation:
        if (hasEntity) {
            objects.transforms[kTestEntity].rotation.y += 0.003f;
        }
        break;
    case Scenario::MirroredCameraYaw:
        if (hasEntity && scenarioFrame == 0) {
            // negative scale flips every triangle's winding
            Transform mirrored = *originalTransform;
            mirrored.scale.x = -mirrored.scale.x;
            place(objects, mirrored);
        }
        rotate(4.0f, 0.0f);
        break;
    case Scenario::LoadWhileMoving:
        if (hasEntity && scenarioFrame == 0) {
            place(objects, *originalTransform);
        }
        camera.moveRight(0.02f);
        if (isEventFrame) {
            // camera looks down -z in view space
            const glm::vec3 forward = -glm::vec3(camera.cameraData.invView[2]);
            request.loadModelAt = camera.cameraData.cameraPos + 3.0f * forward;
        }
        break;
    case Scenario::Teleport:
        if (isEventFrame) {
            camera.teleportTo(camera.cameraData.cameraPos + glm::vec3(0.5f, 0.0f, 0.0f));
        }
        break;
    case Scenario::FovChange:
        if (isEventFrame) {
            camera.setFov(camera.getFovDegrees() + 15.0f);
        }
        break;
    case Scenario::Resize:
        if (isEventFrame) {
            request.windowSize = glm::ivec2(1100, 620);
        }
        break;
    case Scenario::Count:
        break;
    }
    return request;
}

bool EngineMotionTest::endFrame(const GpuMotionCheckStats& stats)
{
    ++frame;
    if (frame <= kWarmupFrames) {
        return false;
    }
    evaluate(stats);
    if (++scenarioFrame < kFramesPerScenario) {
        return false;
    }
    logScenario();
    result = {};
    scenarioFrame = 0;
    if (++scenarioIndex < kScenarios.size()) {
        return false;
    }
    const std::string summary = std::format("Motion test: {} of {} scenarios passed", passedCount, kScenarios.size());
    if (passedCount == kScenarios.size()) {
        logInfo(summary, "MotionTest");
    } else {
        logError(summary, "MotionTest");
    }
    return true;
}

void EngineMotionTest::evaluate(const GpuMotionCheckStats& stats)
{
    const ScenarioInfo& info = kScenarios[scenarioIndex];
    const auto fail = [&](std::string text) {
        result.failures.push_back(std::format("frame {}: {}", scenarioFrame, std::move(text)));
    };
    const float maxMotion = std::bit_cast<float>(stats.maxMotionBits);

    if ((stats.frameFlags & FrameFlag::HistoryReset) != 0) {
        if (info.hasEvent && scenarioFrame >= kEventFrame) {
            // the cut frame: no history, so motion must be exactly zero
            if (result.resetFramesAfterEvent == 0 && stats.movingPixels != 0) {
                fail(std::format("reset frame has motion on {} px, up to {:.3e} px", stats.movingPixels, maxMotion));
            }
            ++result.resetFramesAfterEvent;
        } else if (scenarioFrame >= kSettleFrames) {
            fail("unexpected history reset");
        }
        return;
    }
    if (scenarioFrame < kSettleFrames) {
        return;
    }

    ++result.frames;
    result.emptyFractionSum += static_cast<double>(stats.emptyPixels) / std::max(1u, stats.pixels);
    result.maxStillMotion = std::max(result.maxStillMotion, info.check == Check::NoMotion ? maxMotion : 0.0f);
    if (maxMotion > kMinMovingMotion) {
        ++result.movingFrames;
    }
    switch (info.check) {
    case Check::NoMotion:
        if (maxMotion > kMaxStillMotion) {
            fail(std::format("{} px moving, up to {:.3e} px", stats.movingPixels, maxMotion));
        }
        if (stats.reprojectedErrors != 0) {
            fail(std::format("albedo changed on {} px", stats.reprojectedErrors));
        }
        break;
    case Check::CameraMotion:
        result.maxCameraError = std::max(result.maxCameraError, std::bit_cast<float>(stats.maxCameraErrorBits));
        result.cameraOutliers += stats.cameraMismatches;
        if (stats.cameraMismatches > kMaxCameraOutlierFraction * stats.pixels) {
            fail(std::format("{} px off the camera-only motion, up to {:.3f} px", stats.cameraMismatches,
                             std::bit_cast<float>(stats.maxCameraErrorBits)));
        }
        break;
    case Check::EntityMotion:
    {
        const double compared = std::max(1u, stats.pixels - stats.offscreenPixels);
        result.reprojectedFractionSum += stats.reprojectedErrors / compared;
        result.stillFractionSum += stats.stillErrors / compared;
        break;
    }
    }
}

void EngineMotionTest::logScenario()
{
    const ScenarioInfo& info = kScenarios[scenarioIndex];
    std::vector<std::string> failures = result.failures;
    if (info.check != Check::NoMotion && result.movingFrames != result.frames) {
        failures.push_back(std::format("motion on only {} of {} frames", result.movingFrames, result.frames));
    }
    if (info.hasEvent && result.resetFramesAfterEvent == 0) {
        failures.push_back("no history reset after the event");
    }

    std::string detail = std::format("{} frames", result.frames);
    if (info.hasEvent) {
        detail += std::format(", {} reset frame(s) after the event", result.resetFramesAfterEvent);
    }
    if (info.check == Check::NoMotion) {
        detail += std::format(", max motion {:.2e} px", result.maxStillMotion);
    }
    if (info.check == Check::CameraMotion) {
        detail += std::format(", max error vs camera-only motion {:.4f} px, {} outlier px", result.maxCameraError,
                              result.cameraOutliers);
        if (info.needsSky) {
            const double empty = result.frames > 0 ? result.emptyFractionSum / result.frames : 0.0;
            detail += std::format(", {:.1f}% sky", empty * 100.0);
            if (empty < kMinEmptyFraction) {
                failures.emplace_back("inconclusive: too little sky in view");
            }
        }
        if (result.maxCameraError > kMaxCameraError) {
            failures.push_back(std::format("error vs camera-only motion over {} px", kMaxCameraError));
        }
    }
    if (info.check == Check::EntityMotion && result.frames > 0) {
        const double reprojected = result.reprojectedFractionSum / result.frames;
        const double still = result.stillFractionSum / result.frames;
        detail += std::format(", albedo changed on {:.2f}% of pixels after reprojection, {:.2f}% at zero motion",
                              reprojected * 100.0, still * 100.0);
        if (still < kMinStillFraction) {
            failures.emplace_back("inconclusive: too little albedo change at zero motion");
        } else if (reprojected > kMaxReprojectedToStillRatio * still) {
            failures.emplace_back("reprojection leaves too much of the change");
        }
    }

    if (failures.empty()) {
        ++passedCount;
        logInfo(std::format("PASS {}: {}", info.name, detail), "MotionTest");
        return;
    }
    logError(std::format("FAIL {}: {}", info.name, detail), "MotionTest");
    for (size_t i = 0; i < std::min(failures.size(), kMaxLoggedFailures); ++i) {
        logError(std::format("  {}", failures[i]), "MotionTest");
    }
    if (failures.size() > kMaxLoggedFailures) {
        logError(std::format("  ... {} more", failures.size() - kMaxLoggedFailures), "MotionTest");
    }
}
