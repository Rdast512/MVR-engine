#include "vk_camera.hpp"
#include "util/vk_tracy.hpp"
#include "util/vk_utils.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

Camera::Camera(SwapChain& swapChain) : swapChain(swapChain)
{
    // default until focusOn() is called
    cameraData.cameraPos = glm::vec3(2.0f, 2.0f, 6.0f);
    updateProjection();
}

void Camera::focusOn(const glm::vec3& target, float distance)
{
    // above and behind so the model stays clear of nearPlane
    const float safeDistance = glm::max(distance, nearPlane * 4.0f);
    cameraData.cameraPos = target + glm::vec3(safeDistance * 0.55f, safeDistance * 0.4f, safeDistance * 0.85f);

    const glm::vec3 toTarget = target - cameraData.cameraPos;
    const float len = glm::length(toTarget);
    if (len < 1e-5f) {
        return;
    }

    const glm::vec3 dir = toTarget / len;
    // Invert Camera::updateVectors() basis:
    //   forward.x = cos(pitch)*sin(yaw)
    //   forward.y = -sin(pitch)
    //   forward.z = cos(pitch)*cos(yaw)
    pitch = std::clamp(std::asin(glm::clamp(-dir.y, -1.0f, 1.0f)), -kPitchLimit, kPitchLimit);
    yaw = std::atan2(dir.x, dir.z);
}

Camera::~Camera()
{
    for (size_t i = 0; i < cameraBuffersMemory.size(); ++i) {
        if (cameraBuffersMemory[i] == nullptr) {
            continue;
        }

        if (cameraBuffersMapped[i] != nullptr) {
            vmaUnmapMemory(allocator, cameraBuffersMemory[i]);
            cameraBuffersMapped[i] = nullptr;
        }
        destroyVmaBuffer(allocator, cameraBuffers[i], cameraBuffersMemory[i], "GPU/CameraUBO");
    }
}

void Camera::moveForward(float delta) { cameraData.cameraPos += forward * delta; }
void Camera::moveRight(float delta) { cameraData.cameraPos += right * delta; }
void Camera::moveUp(float delta) { cameraData.cameraPos += worldUp * delta; }

void Camera::rotate(float yawDelta, float pitchDelta)
{
    yaw += yawDelta * kMouseSensitivity;
    pitch += pitchDelta * kMouseSensitivity;
    pitch = std::clamp(pitch, -kPitchLimit, kPitchLimit);
}

void Camera::setFov(float fovVerticalDegrees)
{
    fov = std::clamp(glm::radians(fovVerticalDegrees), kFovMin, kFovMax);
    projDirty = true;
}

void Camera::addFov(float deltaDegrees) { setFov(glm::degrees(fov) + deltaDegrees); }

void Camera::updateVectors()
{
    forward.x = std::cos(pitch) * std::sin(yaw);
    forward.y = -std::sin(pitch);
    forward.z = std::cos(pitch) * std::cos(yaw);
    forward = glm::normalize(forward);

    right = glm::normalize(glm::cross(forward, worldUp));
}

void Camera::updateProjection()
{
    const float aspect =
        static_cast<float>(swapChain.swapChainExtent.width) / static_cast<float>(swapChain.swapChainExtent.height);

    glm::mat4 proj = glm::perspective(fov, aspect, nearPlane, farPlane);
    proj[1][1] *= -1; // vulkan y-flip

    cameraData.proj = proj;
    cameraData.nearZ = nearPlane;
    cameraData.farZ = farPlane;
    cameraData.renderTargetSize = glm::vec2(swapChain.swapChainExtent.width, swapChain.swapChainExtent.height);
    cameraData.invRenderTargetSize = 1.0f / cameraData.renderTargetSize;

    projDirty = false;
}

void Camera::updateCameraData(uint8_t currentImage)
{
    ZoneScopedN("Camera::updateCameraData");

    updateVectors();

    if (projDirty) {
        updateProjection();
    }

    cameraData.prevViewProj = prevViewProj;

    cameraData.view = glm::lookAt(cameraData.cameraPos, cameraData.cameraPos + forward, worldUp);
    cameraData.viewProj = cameraData.proj * cameraData.view;

    // inverses for screen-to-world reconstruction
    cameraData.invView = glm::inverse(cameraData.view);
    cameraData.invProj = glm::inverse(cameraData.proj);
    cameraData.invViewProj = glm::inverse(cameraData.viewProj);

    std::memcpy(cameraBuffersMapped[currentImage], &cameraData, sizeof(cameraData));

    prevViewProj = cameraData.viewProj;
}
