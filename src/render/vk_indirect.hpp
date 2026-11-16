#pragma once

#include "core/vk_allocator.hpp"
#include "core/vk_device.hpp"
#include "vk_rt_pipeline.hpp"

#include <array>
#include <cstdint>
#include <vulkan/vulkan_raii.hpp>

// M4 G2 (spec/m4/g2_indirect_pass.md): one SER ray per pixel from the primary surface, shaded at its hit, stored as
// the pixel's initial ReSTIR GI reservoir (RtxdiContext's GI buffer). One pipeline per reorder mode
class IndirectPass
{
public:
    // read once per recorded frame; the UI changes it between frames
    struct Settings
    {
        // off: ReorderThread compiled out, the same image. Off by default: with one material model the reorder costs
        // ~0.07 ms and saves less, 0.46 vs 0.42 ms gallery, 0.57 vs 0.55 ms courtyard (spec/m4/g2 §4)
        bool isReorderEnabled = false;
    };

    IndirectPass(const Device& device, const VkAllocator& allocator);

    // ray command buffer, heaps bound, after the resolve; FramePushData. Writes the GI reservoirs and makes them
    // visible to later compute passes
    void record(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot, vk::DeviceAddress frameRoot,
                vk::Extent2D extent) const;

    // false where the device does not reorder: the reorder pipeline would only cost (NVIDIA vk_gltf_renderer)
    [[nodiscard]] bool isReorderSupported() const noexcept { return isReorderAvailable; }

    Settings settings;

private:
    [[nodiscard]] static RtPipeline::Source source(bool isReorderEnabled);

    // indexed by isReorderEnabled
    std::array<RtPipeline, 2> pipelines;
    bool isReorderAvailable;
};
