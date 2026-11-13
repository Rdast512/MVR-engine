#include "engine_cull_check.hpp"
#include "../static_headers/logger.hpp"

#include <cstdlib>
#include <format>
#include <string_view>
#include <utility>

bool EngineCullCheck::isRequested()
{
    const char* text = std::getenv("ENGINE_CULL_CHECK");
    return text != nullptr && std::string_view(text) == "1";
}

EngineCullCheck::Step EngineCullCheck::getStep() const
{
    return poseFrame < kApproachFrames ? Step::Approach
                                       : static_cast<Step>(std::to_underlying(Step::Approach) + 1 + poseFrame -
                                                           kApproachFrames);
}

EngineCullCheck::Request EngineCullCheck::beginFrame(Camera& camera, MeshletCullSettings& culling,
                                                     DebugViewSettings& debugView)
{
    Request request;
    if (frame < kWarmupFrames || poseIndex >= kPoses.size()) {
        return request;
    }
    if (!startupPosition) {
        startupPosition = camera.cameraData.cameraPos;
    }
    const Pose& pose = kPoses[poseIndex];
    const auto rotateTo = [&](float targetYaw, float targetPitch) {
        camera.rotate(targetYaw - yaw, targetPitch - pitch);
        yaw = targetYaw;
        pitch = targetPitch;
    };

    const Step step = getStep();
    const bool isCulled = step <= Step::CulledDepth;
    // every test the culling has, triangle tests included
    culling.isInstanceFrustumEnabled = isCulled;
    culling.isFrustumEnabled = isCulled;
    culling.isConeEnabled = isCulled;
    culling.isOcclusionEnabled = isCulled;
    culling.isSubPixelEnabled = isCulled;
    culling.isTriangleBackfaceEnabled = isCulled;
    culling.isSmallTriangleEnabled = isCulled;

    if (step == Step::Approach) {
        if (poseFrame == 0) {
            camera.teleportTo(pose.isCourtyard ? kCourtyardCenter : *startupPosition);
        }
        // the last approach frame is on the pose, one step from the frame before
        rotateTo(pose.yaw - kApproachStep * static_cast<float>(kApproachFrames - poseFrame), pose.pitch);
    } else {
        rotateTo(pose.yaw, pose.pitch);
    }

    const bool isDepth = step == Step::CulledDepth || step == Step::UnculledDepth;
    debugView.view = isDepth ? DebugView::Depth : DebugView::DiffuseAlbedo;
    request.shouldCapture = step == Step::CulledAlbedo || step == Step::CulledDepth ||
                            step == Step::UnculledAlbedo || step == Step::UnculledDepth;
    return request;
}

uint64_t EngineCullCheck::countDifferences(const std::vector<uint16_t>& culled,
                                           const Renderer::DisplayCapture& unculled)
{
    if (culled.size() != unculled.rgba16f.size()) {
        return ~uint64_t{0};
    }
    uint64_t count = 0;
    for (size_t texel = 0; texel < culled.size(); texel += 4) {
        for (size_t channel = 0; channel < 3; ++channel) {
            if (culled[texel + channel] != unculled.rgba16f[texel + channel]) {
                ++count;
                break;
            }
        }
    }
    return count;
}

bool EngineCullCheck::endFrame(const std::optional<Renderer::DisplayCapture>& capture)
{
    ++frame;
    if (frame <= kWarmupFrames) {
        return false;
    }
    const Step step = getStep();
    ++poseFrame;
    if (step == Step::Approach || step == Step::UnculledSettle) {
        return false;
    }
    if (!capture) {
        isCaptureMissing = true;
    } else if (step == Step::CulledAlbedo) {
        culledAlbedo.assign(capture->rgba16f.begin(), capture->rgba16f.end());
    } else if (step == Step::CulledDepth) {
        culledDepth.assign(capture->rgba16f.begin(), capture->rgba16f.end());
    } else if (step == Step::UnculledAlbedo) {
        albedoDifferences = countDifferences(culledAlbedo, *capture);
    }
    if (step != Step::UnculledDepth) {
        return false;
    }

    const uint64_t depthDifferences = capture ? countDifferences(culledDepth, *capture) : 0;
    const Pose& pose = kPoses[poseIndex];
    const std::string detail = std::format("{} px of albedo and {} px of depth differ with culling off",
                                           albedoDifferences, depthDifferences);
    if (!isCaptureMissing && albedoDifferences == 0 && depthDifferences == 0) {
        ++passedCount;
        logInfo(std::format("PASS {}: {}", pose.name, detail), "CullCheck");
    } else {
        logError(std::format("FAIL {}: {}{}", pose.name, detail, isCaptureMissing ? ", capture missing" : ""),
                 "CullCheck");
    }
    poseFrame = 0;
    albedoDifferences = 0;
    isCaptureMissing = false;
    if (++poseIndex < kPoses.size()) {
        return false;
    }
    const std::string summary = std::format("Cull check: {} of {} poses identical with culling on and off",
                                            passedCount, kPoses.size());
    if (passedCount == kPoses.size()) {
        logInfo(summary, "CullCheck");
    } else {
        logError(summary, "CullCheck");
    }
    return true;
}
