#pragma once

#include "core/object_storage.hpp"

#include <glm/vec3.hpp>
#include <glm/vec2.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

class AssetBuffers;
class Camera;
class Device;
class Renderer;
class Scene;
class ShaderObjects;
class VkAllocator;

// what only Engine's scene and window actions can do for the runners before a frame
struct CheckRequest
{
    struct Load
    {
        std::filesystem::path model;
        glm::vec3 position{0.0f};
    };
    struct InstanceField
    {
        glm::vec3 center{0.0f};
        int side = 0;
        float spacing = 0.0f;
    };

    // loaded at the origin, then every older entity is unloaded (guide capture, lab); logSubsystem names the runner
    std::optional<std::filesystem::path> replaceModels;
    std::string_view logSubsystem = "Checks";
    // adds an entity (motion test)
    std::optional<Load> load;
    // arrives as SDL_EVENT_WINDOW_RESIZED in a later poll (motion test)
    std::optional<glm::ivec2> windowSize;
    // startup only (bench ENGINE_BENCH_FIELD); Engine reports the spawned range with onInstanceFieldSpawned
    std::optional<InstanceField> instanceField;
};

// the engine objects the runners drive; the harness never owns them
struct CheckHostContext
{
    const Device& device;
    const VkAllocator& allocator;
    const ShaderObjects& shaderObjects;
    Scene& scene;
    const AssetBuffers& assets;
    Renderer& renderer;
    Camera& camera;
};

// engine_checks.dll: every runner of one process, the check passes and ENGINE_CONFIG
class CheckHarness
{
public:
    // nullptr when no runner variable and no ENGINE_CONFIG is set. Throws when ENGINE_LAB comes with a check or bench
    // variable, or the bench with ENGINE_GPUAV_RESOLVE_PER_BIN outside the GI check
    [[nodiscard]] static std::unique_ptr<CheckHarness> fromEnvironment();
    ~CheckHarness();
    CheckHarness(const CheckHarness&) = delete;
    CheckHarness& operator=(const CheckHarness&) = delete;
    CheckHarness(CheckHarness&&) = delete;
    CheckHarness& operator=(CheckHarness&&) = delete;

    // a runner owns the camera, the scene and the frame pace: uncapped present, no camera input, no unfocused pacing.
    // false when only ENGINE_CONFIG is set
    [[nodiscard]] bool isAutomated() const noexcept;
    // the runner's default when ENGINE_JITTER is unset; the motion test, guide capture and GI check compare pixels
    [[nodiscard]] bool isJitterEnabledByDefault() const noexcept;
    // check scene > ENGINE_MODEL > bench model > defaultModel
    [[nodiscard]] std::filesystem::path chooseStartupModel(const std::optional<std::filesystem::path>& engineModel,
                                                           const std::filesystem::path& defaultModel) const;

    // after the renderer exists, before its swapchain resources: startup pose and sun, ENGINE_CONFIG, check passes,
    // frame hook. The returned request carries the bench's instance field
    [[nodiscard]] CheckRequest attach(const CheckHostContext& host);
    // status: Engine's spawn line, logged under the bench
    void onInstanceFieldSpawned(EntityId firstEntity, uint32_t count, std::string_view status);

    // before drawFrame. Sets renderer and camera state itself; returns what only Engine's scene actions can do
    [[nodiscard]] CheckRequest beginFrame();
    // after drawFrame, not minimized; waits for the device itself when a runner reads this frame. true: quit
    [[nodiscard]] bool endFrame();
    // after the loop's waitIdle: summaries, then the verdict of every runner
    [[nodiscard]] bool finish();

private:
    struct Runners;
    explicit CheckHarness(std::unique_ptr<Runners> runners);
    std::unique_ptr<Runners> runners;
};
