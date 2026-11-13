#include "vk_camera.hpp"
#include "Constants.h"
#include "static_headers/logger.hpp"
#include "util/vk_tracy.hpp"
#include "util/vk_utils.hpp"

#include <glm/gtc/constants.hpp>

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
    lookAt(target + glm::vec3(safeDistance * 0.55f, safeDistance * 0.4f, safeDistance * 0.85f), target);
}

void Camera::lookAt(const glm::vec3& position, const glm::vec3& target)
{
    isCutPending = true;
    cameraData.cameraPos = position;

    const glm::vec3 toTarget = target - position;
    const float len = glm::length(toTarget);
    if (len < 1e-5f) {
        return;
    }

    const glm::vec3 dir = toTarget / len;
    // Invert Camera::updateVectors() basis:
    //   forward.x = cos(pitch)*sin(yaw)
    //   forward.y = -sin(pitch)
    //   forward.z = cos(pitch)*cos(yaw)
    pitch = glm::clamp(glm::asin(glm::clamp(-dir.y, -1.0f, 1.0f)), -kPitchLimit, kPitchLimit);
    yaw = glm::atan(dir.x, dir.z);
}

void Camera::teleportTo(const glm::vec3& position)
{
    isCutPending = true;
    cameraData.cameraPos = position;
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
        destroyVmaBuffer(allocator, cameraBuffers[i], cameraBuffersMemory[i]);
    }
}

void Camera::createBuffers(const Device& device, const VkAllocator& vkAllocator)
{
    logInfo("createBuffers() started", "Camera");
    allocator = vkAllocator.allocator;
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        constexpr vk::DeviceSize bufferSize = sizeof(GpuCameraData);
        vk::raii::Buffer buffer({});
        VmaAllocation bufferMem = nullptr;
        createBuffer(bufferSize,
                     vk::BufferUsageFlagBits2::eUniformBuffer | vk::BufferUsageFlagBits2::eStorageBuffer |
                         vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                     vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, buffer,
                     bufferMem, allocator, device.vkdevice, device.queueFamilyIndices,
                     std::format("CameraUniformBuffer_{}", i));
        cameraBuffers[i] = std::move(buffer);
        cameraBuffersMemory[i] = bufferMem;
        void* data = nullptr;
        vmaMapMemory(allocator, bufferMem, &data);
        cameraBuffersMapped[i] = data;
        cameraBufferAddresses[i] = device.vkdevice.getBufferAddress({.buffer = *cameraBuffers[i]});
    }
}

void Camera::moveForward(float delta) { cameraData.cameraPos += forward * delta; }
void Camera::moveRight(float delta) { cameraData.cameraPos += right * delta; }
void Camera::moveUp(float delta) { cameraData.cameraPos += worldUp * delta; }

void Camera::rotate(float yawDelta, float pitchDelta)
{
    yaw += yawDelta * kMouseSensitivity;
    pitch += pitchDelta * kMouseSensitivity;
    pitch = glm::clamp(pitch, -kPitchLimit, kPitchLimit);
}

void Camera::setFov(float fovVerticalDegrees)
{
    fov = glm::clamp(glm::radians(fovVerticalDegrees), kFovMin, kFovMax);
    projDirty = true;
}

void Camera::addFov(float deltaDegrees) { setFov(glm::degrees(fov) + deltaDegrees); }

void Camera::updateVectors()
{
    forward.x = glm::cos(pitch) * glm::sin(yaw);
    forward.y = -glm::sin(pitch);
    forward.z = glm::cos(pitch) * glm::cos(yaw);
    forward = glm::normalize(forward);

    right = glm::normalize(glm::cross(forward, worldUp));
}

void Camera::updateProjection()
{
    const float aspect = static_cast<float>(renderExtent.width) / static_cast<float>(renderExtent.height);

    // reverse Z: near maps to 1, far to 0 (GLM_FORCE_DEPTH_ZERO_TO_ONE)
    glm::mat4 proj = glm::perspective(fov, aspect, farPlane, nearPlane);
    proj[1][1] *= -1; // vulkan y-flip

    cameraData.proj = proj;
    cameraData.nearZ = nearPlane;
    cameraData.farZ = farPlane;
    cameraData.renderTargetSize = glm::vec2(renderExtent.width, renderExtent.height);
    cameraData.invRenderTargetSize = 1.0f / cameraData.renderTargetSize;

    projDirty = false;
}

void Camera::updateCameraData(uint8_t currentImage)
{
    ZoneScopedN("Camera::updateCameraData");

    updateVectors();

    isCutFrame = isCutPending || projDirty;
    isCutPending = false;
    if (projDirty) {
        updateProjection();
    }

    cameraData.view = glm::lookAt(cameraData.cameraPos, cameraData.cameraPos + forward, worldUp);
    cameraData.viewProj = cameraData.proj * cameraData.view;
    // a cut has no previous view: no camera motion instead of a one-frame jump
    cameraData.prevViewProj = isCutFrame ? cameraData.viewProj : prevViewProj;

    // inverses for screen-to-world reconstruction
    cameraData.invView = glm::inverse(cameraData.view);
    cameraData.invProj = glm::inverse(cameraData.proj);
    cameraData.invViewProj = glm::inverse(cameraData.viewProj);

    std::memcpy(cameraBuffersMapped[currentImage], &cameraData, sizeof(cameraData));

    prevViewProj = cameraData.viewProj;
}
