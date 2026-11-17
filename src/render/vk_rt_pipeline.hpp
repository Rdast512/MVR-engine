// SHELVED with SER (considerations C-G1-3): kept in the tree, not built; the frame traces shaded rays with traceClosest
#pragma once

#include "../Constants.h"
#include "core/vk_allocator.hpp"
#include "core/vk_device.hpp"
#include "util/vk_utils.hpp"

#include <array>
#include <string_view>
#include <vulkan/vulkan_raii.hpp>

// one shaded-ray pass (spec/m4/g1_ser_rt_pipeline.md §3.1): raygen + miss + closest-hit from one .spv under
// ENGINE_SHADER_DIR/passes, its shader binding table and a per-slot indirect trace command. Descriptor heaps, no
// pipeline layout; push data goes through pushDataEXT after bind. Reordering is a specialization constant, so a
// pass that toggles it keeps one pipeline per mode
class RtPipeline
{
public:
    struct Source
    {
        std::string_view file;
        const char* raygenEntry;
        const char* missEntry;
        const char* closestHitEntry;
        std::string_view debugName;
        // raytracing/ser.slang kIsSerReorderEnabled; false compiles ReorderThread out
        bool isReorderEnabled = true;
    };

    RtPipeline(const Device& device, const VkAllocator& allocator, const Source& source);
    ~RtPipeline();

    RtPipeline(const RtPipeline&) = delete;
    RtPipeline& operator=(const RtPipeline&) = delete;
    RtPipeline(RtPipeline&&) = delete;
    RtPipeline& operator=(RtPipeline&&) = delete;

    void bind(const vk::raii::CommandBuffer& cmd) const;
    // one launch per pixel of extent; the slot's fence has been waited, so its command is rewritten in place. Every
    // trace of a slot within one frame uses the same extent
    void traceRays(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot, vk::Extent2D extent) const;

private:
    enum ShaderGroup : uint32_t
    {
        RaygenGroup = 0,
        MissGroup,
        HitGroup,
        GroupCount,
    };

    void createPipeline(const Source& source);
    void createShaderBindingTable(std::string_view debugName);

    const Device& device;
    const VkAllocator& allocator;
    vk::raii::Pipeline pipeline = nullptr;
    // raygen, miss and hit regions, one record each, each at shaderGroupBaseAlignment
    vk::raii::Buffer sbtBuffer = nullptr;
    VmaAllocation sbtMemory = nullptr;
    vk::TraceRaysIndirectCommand2KHR commandTemplate{};
    // VkTraceRaysIndirectCommand2KHR per frame slot
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> commands;
};
