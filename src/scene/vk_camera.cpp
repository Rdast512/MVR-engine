#include "vk_camera.hpp"
#include "util/vk_tracy.hpp"

#include <glm/gtc/constants.hpp>

Camera::Camera(SwapChain& swapChain) : swapChain(swapChain)
{
    // Default until Engine calls focusOn() with the initial asset position.
    cameraData.cameraPos = glm::vec3(2.0f, 2.0f, 6.0f);
    updateProjection();
}

void Camera::focusOn(const glm::vec3& target, float distance)
{
    // Offset: slightly above and back so the model is in frame without clipping nearPlane.
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
    pitch = asinf(glm::clamp(-dir.y, -1.0f, 1.0f));
    if (pitch > kPitchLimit) {
        pitch = kPitchLimit;
    }
    if (pitch < -kPitchLimit) {
        pitch = -kPitchLimit;
    }
    yaw = atan2f(dir.x, dir.z);
}

Camera::~Camera()
{
    for (size_t i = 0; i < cameraBuffersMemory.size(); ++i) {
        if (cameraBuffersMemory[i] == nullptr)
            continue;

        if (cameraBuffersMapped[i] != nullptr) {
            vmaUnmapMemory(allocator, cameraBuffersMemory[i]);
            cameraBuffersMapped[i] = nullptr;
        }

        VkBuffer rawBuf = VK_NULL_HANDLE;
        if (*cameraBuffers[i] != VK_NULL_HANDLE) {
            rawBuf = cameraBuffers[i].release();
            tracyResourceFree(rawBuf, "GPU/CameraUBO");
        }

        vmaDestroyBuffer(allocator, rawBuf, cameraBuffersMemory[i]);
        cameraBuffersMemory[i] = nullptr;
    }
}

// ── Free-fly movement ────────────────────────────────────────────

void Camera::moveForward(float delta) { cameraData.cameraPos += forward * delta; }
void Camera::moveRight(float delta) { cameraData.cameraPos += right * delta; }
void Camera::moveUp(float delta) { cameraData.cameraPos += worldUp * delta; }

void Camera::rotate(float yawDelta, float pitchDelta)
{
    yaw += yawDelta * kMouseSensitivity;
    pitch += pitchDelta * kMouseSensitivity;

    if (pitch > kPitchLimit)
        pitch = kPitchLimit;
    if (pitch < -kPitchLimit)
        pitch = -kPitchLimit;
}

// ── FOV ──────────────────────────────────────────────────────────

void Camera::setFov(float fovVerticalDegrees)
{
    fov = glm::radians(fovVerticalDegrees);
    if (fov < kFovMin)
        fov = kFovMin;
    if (fov > kFovMax)
        fov = kFovMax;
    projDirty = true;
}

void Camera::addFov(float deltaDegrees) { setFov(glm::degrees(fov) + deltaDegrees); }

// ── Recompute basis vectors from yaw/pitch ───────────────────────

void Camera::updateVectors()
{
    forward.x = cosf(pitch) * sinf(yaw);
    forward.y = -sinf(pitch);
    forward.z = cosf(pitch) * cosf(yaw);
    forward = glm::normalize(forward);

    right = glm::normalize(glm::cross(forward, worldUp));
}

// ── Recompute projection matrix ──────────────────────────────────

void Camera::updateProjection()
{
    const float aspect =
        static_cast<float>(swapChain.swapChainExtent.width) / static_cast<float>(swapChain.swapChainExtent.height);

    glm::mat4 proj = glm::perspective(fov, aspect, nearPlane, farPlane);
    proj[1][1] *= -1; // Vulkan Y-flip

    cameraData.proj = proj;
    cameraData.nearZ = nearPlane;
    cameraData.farZ = farPlane;
    cameraData.renderTargetSize = glm::vec2(swapChain.swapChainExtent.width, swapChain.swapChainExtent.height);
    cameraData.invRenderTargetSize = 1.0f / cameraData.renderTargetSize;

    projDirty = false;
}

// ── GPU upload ───────────────────────────────────────────────────

void Camera::updateCameraData(uint8_t currentImage)
{
    ZoneScopedN("Camera::updateCameraData");

    updateVectors();

    if (projDirty)
        updateProjection();

    cameraData.prevViewProj = prevViewProj;

    cameraData.view = glm::lookAt(cameraData.cameraPos, cameraData.cameraPos + forward, worldUp);
    cameraData.viewProj = cameraData.proj * cameraData.view;

    // Inverses — needed by ray tracing, SSR, screen→world reconstruct.
    cameraData.invView = glm::inverse(cameraData.view);
    cameraData.invProj = glm::inverse(cameraData.proj);
    cameraData.invViewProj = glm::inverse(cameraData.viewProj);

    memcpy(this->cameraBuffersMapped[currentImage], &this->cameraData, sizeof(this->cameraData));

    prevViewProj = cameraData.viewProj;
}
