#pragma once
#include "Constants.h"
#include "core/types.hpp"
#include "core/vk_allocator.hpp"
#include "core/vk_device.hpp"
#include "util/vk_utils.hpp"

#include <array>

#include <vulkan/vulkan_raii.hpp>

class ShaderObjects;

// PBR plan H2, engine spec §12.3: auto exposure from a luminance histogram of the post input, then one tonemap
// curve into the display-resolution post target that present shows. The exposure lives in a 1x1 R32F image
// (RenderTargets), shared by adaptation and tonemapping
class PostProcess
{
public:
    struct Settings
    {
        bool isAutoExposure = true;
        // added to the auto exposure, in stops
        float compensationEv = 0.0f;
        // log2 of the exposure scale while auto exposure is off
        float manualEv = 0.0f;
        // clamp of log2(exposure scale)
        float minEv = -24.0f;
        float maxEv = 6.0f;
        // inverse half-lives for rising and falling metered luminance
        float adaptationSpeedUp = 2.0f;
        float adaptationSpeedDown = 1.0f;

        bool operator==(const Settings&) const = default;
    };

    PostProcess(const Device& device, const VkAllocator& allocator, const ShaderObjects& shaderObjects);
    ~PostProcess();

    PostProcess(const PostProcess&) = delete;
    PostProcess& operator=(const PostProcess&) = delete;
    PostProcess(PostProcess&&) = delete;
    PostProcess& operator=(PostProcess&&) = delete;

    // after input was written: histogram -> exposure -> tonemap into the post target. input: resource heap, sampled,
    // at displayExtent (DLSS-RR's output, or noisyColor / the accumulated mean while the denoiser is off)
    void record(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot, vk::DeviceAddress frameRoot, uint32_t input,
                vk::Extent2D displayExtent, float deltaSeconds);
    // after the slot's fence: keeps what the slot's last record measured
    void readBack(uint32_t frameSlot);
    // the next record jumps to the target exposure, e.g. after the exposure image was recreated
    void requestSnap() noexcept { isSnapPending = true; }
    [[nodiscard]] const GpuExposureReadback& getLastExposure() const noexcept { return lastExposure; }

    Settings settings;

private:
    const VkAllocator& allocator;
    const ShaderObjects& shaderObjects;
    // two 32-bit words per weighted bin, GPU-only: zeroed by the first record, then cleared by the exposure pass
    vk::raii::Buffer histogram = nullptr;
    VmaAllocation histogramMemory = nullptr;
    vk::DeviceAddress histogramAddress = 0;
    bool isHistogramZeroed = false;
    // the CPU reads the copy in cached memory, never write-combined VRAM
    std::array<CounterReadback, MAX_FRAMES_IN_FLIGHT> readbacks;
    std::array<bool, MAX_FRAMES_IN_FLIGHT> isReadbackPending{};
    GpuExposureReadback lastExposure{};
    bool isSnapPending = true;
};
