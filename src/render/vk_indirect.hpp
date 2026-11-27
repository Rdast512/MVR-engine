#pragma once

#include <cstdint>
#include <vulkan/vulkan_raii.hpp>

// M4 G2 (spec/m4/g2_indirect_pass.md): one ray query per pixel from the primary surface, shaded at its hit, stored as
// indirectRadiance (RawPt) or initial ReSTIR GI reservoir. A compute pass while SER is shelved (C-G1-3).
// needsInputBarrier: true only when indirect reads this frame's DI reservoirs (secondary DI resampling); rayStartBarrier,
// nrcBoundary and sharc.recordResolve already order all other inputs, and every consumer emits its own input barrier.
void recordIndirectPass(const vk::raii::CommandBuffer& cmd, const vk::raii::ShaderEXT& shader,
                        vk::DeviceAddress frameRoot, vk::Extent2D extent, bool needsInputBarrier);
