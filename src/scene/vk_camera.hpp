#pragma once
#include <array>
#include <optional>
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

    // a cut unless isCut is false (camera motion: history reprojects); pitch is clamped to kPitchLimit
    void lookAt(const glm::vec3& position, const glm::vec3& target, bool isCut = true);

    // keeps the orientation; a cut, so the jump is not camera motion
    void teleportTo(const glm::vec3& position);

    // clamped to [10, 150] degrees
    void setFov(float fovVerticalDegrees);
    void addFov(float deltaDegrees); // for scroll-wheel

    [[nodiscard]] float getFovDegrees() const { return glm::degrees(fov); }
    // as of the last updateCameraData
    [[nodiscard]] glm::vec3 getForward() const noexcept { return forward; }

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
    // frameIndex picks the jitter phase; isJitterResolved: DLSS-RR resolves this frame's jitter
    void updateCameraData(uint8_t currentImage, uint64_t frameIndex, bool isJitterResolved);

    // ENGINE_JITTER=1
    // ENGINE_JITTER: 0 off, anything else on; unset: the engine's default
    [[nodiscard]] static std::optional<bool> jitterRequest();
    // Halton(2,3) phases per cycle: 8 * (display / render)^2 (engine spec §12.2)
    [[nodiscard]] uint32_t getJitterPhaseCount() const;
    // subpixel jitter of the raster (cameraData.jitterOffset), resolved by DLSS-RR (PBR plan H1). Frames DLSS-RR does
    // not resolve (debug views, Lit without lights) are not jittered: upscaled to the display, the jitter boils
    bool isJitterEnabled = true;
    // ENGINE_JITTER=1: jitter unresolved frames too, so the culling checks cover the jitter rule (engine spec §12.2)
    bool isJitterForced = false;
    // the frame index the jitter phase follows instead of updateCameraData's: the check runners' per-step index, so a
    // step's frames do not depend on the steps before it
    std::optional<uint64_t> jitterPhaseOverride;

    // after updateCameraData: first frame, focusOn, teleportTo, a projection change, or any swapchain recreation (setRenderExtent);
    // cameraData.prevViewProj equals viewProj then
    [[nodiscard]] bool isCut() const noexcept { return isCutFrame; }

    // stable gpu handles, accessed directly
    VmaAllocator allocator = nullptr;
    SwapChain& swapChain;
    GpuCameraData cameraData = {};
    glm::mat4 prevViewProj = {};
    glm::vec3 prevCameraPos{0.0f};
    glm::vec3 prevRayOrigin{0.0f};
    glm::vec3 prevRayDx{0.0f};
    glm::vec3 prevRayDy{0.0f};
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
