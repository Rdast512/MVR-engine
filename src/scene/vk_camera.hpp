#pragma once
#include <array>
#include <core/types.hpp>
#include "core/vk_swapchain.hpp"
#include "tracy/Tracy.hpp"

class Camera
{
public:
    Camera(SwapChain& swapChain);
    ~Camera();

    // ── Free-fly movement (camera-local axes) ─────────────────
    void moveForward(float delta); // along forward vector
    void moveRight(float delta); // along right vector
    void moveUp(float delta); // along world-up vector

    // ── Mouse look ────────────────────────────────────────────
    void rotate(float yawDelta, float pitchDelta);

    // Place the camera so it looks at target from a fixed offset (engine start / debug).
    void focusOn(const glm::vec3& target, float distance = 5.0f);

    // ── FOV / projection (mutable: accessors) ─────────────────
    // fovVerticalDegrees clamped to [10, 150].
    void setFov(float fovVerticalDegrees);
    void addFov(float deltaDegrees); // for scroll-wheel

    [[nodiscard]] float getFovDegrees() const { return glm::degrees(fov); }

    [[nodiscard]] float getNearPlane() const { return nearPlane; }
    void setNearPlane(float nearZ)
    {
        nearPlane = nearZ;
        projDirty = true;
    }

    [[nodiscard]] float getFarPlane() const { return farPlane; }
    void setFarPlane(float farZ)
    {
        farPlane = farZ;
        projDirty = true;
    }

    // ── GPU upload ────────────────────────────────────────────
    void updateCameraData(uint8_t currentImage);

    // ── GPU resources (stable handles: direct access) ─────────
    VmaAllocator allocator = nullptr;
    SwapChain& swapChain;
    GpuCameraData cameraData = {};
    glm::mat4 prevViewProj = {};
    std::array<void*, 2> cameraBuffersMapped = {nullptr, nullptr};
    std::array<vk::raii::Buffer, 2> cameraBuffers = {nullptr, nullptr};
    std::array<VmaAllocation, 2> cameraBuffersMemory = {nullptr, nullptr};
    std::array<vk::DeviceAddress, 2> cameraBufferAddresses = {0, 0};

private:
    void updateVectors();
    void updateProjection();

    // Orientation (radians)
    float yaw = -glm::half_pi<float>(); // facing -Z initially
    float pitch = 0.0f;

    // Camera-local basis (recomputed each frame)
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 right{1.0f, 0.0f, 0.0f};
    glm::vec3 worldUp{0.0f, 1.0f, 0.0f};

    // Projection parameters
    float fov = glm::radians(45.0f); // vertical FOV
    float nearPlane = 0.1f;
    float farPlane = 20.0f;
    bool projDirty = true;

    static constexpr float kPitchLimit = glm::radians(89.0f);
    static constexpr float kMouseSensitivity = 0.002f; // rad/pixel
    static constexpr float kFovMin = glm::radians(10.0f);
    static constexpr float kFovMax = glm::radians(150.0f);
};
