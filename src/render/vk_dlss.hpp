#pragma once
#include "core/vk_device.hpp"
#include "core/vk_frame_sync.hpp"
#include "vk_driver_heap.hpp"

#include <cstdint>

#include <glm/glm.hpp>
#include <vulkan/vulkan_raii.hpp>

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

// DLSS Ray Reconstruction through NGX (PBR plan H1, engine spec §12.1): the DLSSD feature at the render size of the
// chosen quality mode (Quality by default), HW reverse-Z depth, packed normal + roughness, HDR color with the engine's
// exposure (H2). Required: a GPU or driver without RR throws at construction
class Dlss
{
public:
    // the modes RR's optimal-settings query answers; UltraQuality has no RR render size
    enum class Quality : uint8_t
    {
        Dlaa,
        Quality,
        Balanced,
        Performance,
        UltraPerformance,
    };
    // RR render presets of the pinned SDK; Default lets the driver pick, the removed and reserved letters are left out
    enum class Preset : uint8_t
    {
        Default,
        D,
        E,
        F,
    };
    struct Settings
    {
        Quality quality = Quality::Quality;
        Preset preset = Preset::Default;

        bool operator==(const Settings&) const = default;
    };

    // one NGX input or output; the image stays in the layout its pass left it in
    struct Image
    {
        vk::Image image;
        vk::ImageView view;
        vk::Format format = vk::Format::eUndefined;
        vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor;
    };

    struct Frame
    {
        Image color;
        Image depth;
        Image motion;
        Image diffuseAlbedo;
        Image specularAlbedo;
        Image normalRoughness;
        // display resolution, written
        Image output;
        // 1x1 R32F: the exposure scale post applies before the tonemap
        Image exposure;
        // render pixels, the offset the raster added to every vertex (y down)
        glm::vec2 jitterPixels{0.0f};
        // unjittered
        glm::mat4 view{1.0f};
        glm::mat4 proj{1.0f};
        float frameDeltaMs = 0.0f;
        bool isReset = false;
    };

    // NGX's instance and device extensions for Ray Reconstruction, for Device
    [[nodiscard]] static SdkExtensions requiredExtensions();

    Dlss(const Device& device, FrameSync& frameSync);
    ~Dlss();

    Dlss(const Dlss&) = delete;
    Dlss& operator=(const Dlss&) = delete;
    Dlss(Dlss&&) = delete;
    Dlss& operator=(Dlss&&) = delete;

    // device idle: recreates the feature for displayExtent with settings and returns its render extent
    [[nodiscard]] vk::Extent2D recreate(vk::Extent2D displayExtent);
    // settings differ from the feature's: the render extent may change, so the caller rebuilds its targets
    [[nodiscard]] bool isRecreateNeeded() const noexcept { return settings != appliedSettings; }
    // NGX binds its own pipelines and descriptor sets: rebind the heaps after it
    void evaluate(const vk::raii::CommandBuffer& cmd, const Frame& frame) const;

    [[nodiscard]] vk::Extent2D getRenderExtent() const noexcept { return renderExtent; }
    [[nodiscard]] vk::Extent2D getDisplayExtent() const noexcept { return displayExtent; }

    // edited by the UI; applied by the next recreate
    Settings settings;

private:
    void releaseFeature();

    const Device& device;
    FrameSync& frameSync;
    // bound before evaluate: NGX's kernels use the driver's descriptor heap
    DriverDescriptorHeap driverHeap;
    NVSDK_NGX_Parameter* parameters = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    vk::Extent2D renderExtent{};
    vk::Extent2D displayExtent{};
    Settings appliedSettings;
};
