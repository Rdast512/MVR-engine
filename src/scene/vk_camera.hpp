#pragma once
#include <array>
#include <core/types.hpp>
#include "core/vk_allocator.hpp"
#include "core/vk_swapchain.hpp"

class Camera
{
public:
    explicit Camera(SwapChain& swapChain);
    ~Camera();

    // free-fly movement (camera-local axes)
    void moveForward(float delta); // along forward vector
    void moveRight(float delta); // along right vector
    void moveUp(float delta); // along world-up vector

    void rotate(float yawDelta, float pitchDelta);

    // look at target from a fixed offset
    void focusOn(const glm::vec3& target, float distance = 5.0f);

    // a cut; pitch is clamped to kPitchLimit
    void lookAt(const glm::vec3& position, const glm::vec3& target);

    // keeps the orientation; a cut, so the jump is not camera motion
    void teleportTo(const glm::vec3& position);

    // clamped to [10, 150] degrees
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

    // aspect ratio and renderTargetSize; the render resolution, not the display's
    void setRenderExtent(vk::Extent2D extent)
    {
        renderExtent = extent;
        projDirty = true;
    }

    // one persistently mapped GpuCameraData buffer per frame slot; freed by the destructor
    void createBuffers(const Device& device, const VkAllocator& allocator);
    // frameIndex picks the jitter phase
    void updateCameraData(uint8_t currentImage, uint64_t frameIndex);

    // ENGINE_JITTER=1
    [[nodiscard]] static bool isJitterRequested();
    // Halton(2,3) phases per cycle: 8 * (display / render)^2 (engine spec §12.2)
    [[nodiscard]] uint32_t getJitterPhaseCount() const;
    // subpixel jitter of the raster (cameraData.jitterOffset); off until DLSS resolves it (PBR plan H1), since the
    // display would shimmer by half a pixel
    bool isJitterEnabled = false;

    // after updateCameraData: first frame, focusOn, teleportTo, a projection change, or any swapchain recreation (setRenderExtent);
    // cameraData.prevViewProj equals viewProj then
    [[nodiscard]] bool isCut() const noexcept { return isCutFrame; }

    // stable gpu handles, accessed directly
    VmaAllocator allocator = nullptr;
    SwapChain& swapChain;
    GpuCameraData cameraData = {};
    glm::mat4 prevViewProj = {};
    glm::vec3 prevCameraPos{0.0f};
    std::array<void*, 2> cameraBuffersMapped = {nullptr, nullptr};
    std::array<vk::raii::Buffer, 2> cameraBuffers = {nullptr, nullptr};
    std::array<VmaAllocation, 2> cameraBuffersMemory = {nullptr, nullptr};
    std::array<vk::DeviceAddress, 2> cameraBufferAddresses = {0, 0};

private:
    void updateVectors();
    void updateProjection();
    // NDC offset of the frame's phase, (-0.5, 0.5] px per axis
    [[nodiscard]] glm::vec2 computeJitterOffset(uint64_t frameIndex) const;

    // orientation in radians
    float yaw = -glm::half_pi<float>(); // facing -Z initially
    float pitch = 0.0f;

    // recomputed each frame
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 right{1.0f, 0.0f, 0.0f};
    glm::vec3 worldUp{0.0f, 1.0f, 0.0f};

    vk::Extent2D renderExtent{1, 1};
    float fov = glm::radians(45.0f); // vertical FOV
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    bool projDirty = true;
    // set by focusOn and teleportTo, consumed by updateCameraData
    bool isCutPending = true;
    bool isCutFrame = true;

    static constexpr float kPitchLimit = glm::radians(89.0f);
    static constexpr float kMouseSensitivity = 0.002f; // rad/pixel
    static constexpr float kFovMin = glm::radians(10.0f);
    static constexpr float kFovMax = glm::radians(150.0f);
    static constexpr double kJitterBasePhases = 8.0;
};
