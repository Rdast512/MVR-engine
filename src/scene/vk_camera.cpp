#include "vk_camera.hpp"
#include "Constants.h"
#include "static_headers/logger.hpp"
#include "util/vk_tracy.hpp"
#include "util/vk_utils.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>

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

void Camera::lookAt(const glm::vec3& position, const glm::vec3& target, bool isCut)
{
    isCutPending = isCutPending || isCut;
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
    // texture detail of the display resolution DLSS reconstructs (engine spec §12.2)
    cameraData.lodBias = std::log2(static_cast<float>(renderExtent.width) /
                                   static_cast<float>(swapChain.swapChainExtent.width));

    projDirty = false;
}

namespace
{
    // radical inverse of index in base
    float halton(uint32_t index, uint32_t base)
    {
        float result = 0.0f;
        float fraction = 1.0f;
        while (index > 0) {
            fraction /= static_cast<float>(base);
            result += fraction * static_cast<float>(index % base);
            index /= base;
        }
        return result;
    }
} // namespace

std::optional<bool> Camera::jitterRequest()
{
    const char* text = std::getenv("ENGINE_JITTER");
    if (text == nullptr) {
        return std::nullopt;
    }
    return std::string_view(text) != "0";
}

uint32_t Camera::getJitterPhaseCount() const
{
    const vk::Extent2D display = swapChain.swapChainExtent;
    const double areaRatio = (static_cast<double>(display.width) * display.height) /
                             (static_cast<double>(renderExtent.width) * renderExtent.height);
    return std::max(1u, static_cast<uint32_t>(std::lround(kJitterBasePhases * areaRatio)));
}

glm::vec2 Camera::computeJitterOffset(uint64_t frameIndex) const
{
    // index 0 is (0, 0) in every base: start the cycle at 1
    const auto index = static_cast<uint32_t>(frameIndex % getJitterPhaseCount()) + 1;
    const glm::vec2 pixels(halton(index, 2) - 0.5f, halton(index, 3) - 0.5f);
    return 2.0f * pixels / glm::vec2(renderExtent.width, renderExtent.height);
}

void Camera::updateCameraData(uint8_t currentImage, uint64_t frameIndex, bool isJitterResolved)
{
    ZoneScopedN("Camera::updateCameraData");

    updateVectors();

    isCutFrame = isCutPending || projDirty;
    isCutPending = false;
    if (projDirty) {
        updateProjection();
    }

    // raster and resolve only: viewProj and every matrix derived from it stay unjittered
    const bool isJittered = isJitterEnabled && (isJitterResolved || isJitterForced);
    cameraData.jitterOffset = isJittered ? computeJitterOffset(jitterPhaseOverride.value_or(frameIndex)) : glm::vec2(0.0f);
    cameraData.view = glm::lookAt(cameraData.cameraPos, cameraData.cameraPos + forward, worldUp);
    cameraData.viewProj = cameraData.proj * cameraData.view;
    // a cut has no previous view: no camera motion instead of a one-frame jump
    cameraData.prevViewProj = isCutFrame ? cameraData.viewProj : prevViewProj;
    cameraData.prevCameraPos = isCutFrame ? cameraData.cameraPos : prevCameraPos;

    // inverses for screen-to-world reconstruction
    cameraData.invView = glm::inverse(cameraData.view);
    cameraData.invProj = glm::inverse(cameraData.proj);
    cameraData.invViewProj = glm::inverse(cameraData.viewProj);

    // world-space ray basis at unit view depth for jittered pixel centers (m8 O4):
    // c.xy / c.w = pixelNdc - jitterOffset, c.w = -v.z = viewDepth > 0
    {
        const glm::dvec3 rightAxis(cameraData.invView[0]);
        const glm::dvec3 upAxis(cameraData.invView[1]);
        const glm::dvec3 forwardAxis(-glm::dvec3(cameraData.invView[2]));
        const double invP00 = 1.0 / static_cast<double>(cameraData.proj[0][0]);
        const double invP11 = 1.0 / static_cast<double>(cameraData.proj[1][1]);
        const double invW = static_cast<double>(cameraData.invRenderTargetSize.x);
        const double invH = static_cast<double>(cameraData.invRenderTargetSize.y);
        const double jx = static_cast<double>(cameraData.jitterOffset.x);
        const double jy = static_cast<double>(cameraData.jitterOffset.y);
        cameraData.rayDx = glm::vec3(rightAxis * (2.0 * invW * invP00));
        cameraData.rayDy = glm::vec3(upAxis * (2.0 * invH * invP11));
        cameraData.rayOrigin = glm::vec3(forwardAxis + rightAxis * ((-1.0 + invW - jx) * invP00) +
                                         upAxis * ((-1.0 + invH - jy) * invP11));
    }
    cameraData.prevRayOrigin = isCutFrame ? cameraData.rayOrigin : prevRayOrigin;
    cameraData.prevRayDx = isCutFrame ? cameraData.rayDx : prevRayDx;
    cameraData.prevRayDy = isCutFrame ? cameraData.rayDy : prevRayDy;

    std::memcpy(cameraBuffersMapped[currentImage], &cameraData, sizeof(cameraData));

    prevViewProj = cameraData.viewProj;
    prevCameraPos = cameraData.cameraPos;
    prevRayOrigin = cameraData.rayOrigin;
    prevRayDx = cameraData.rayDx;
    prevRayDy = cameraData.rayDy;
}
