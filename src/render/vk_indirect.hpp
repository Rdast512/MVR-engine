#pragma once

#include <cstdint>
#include <vulkan/vulkan_raii.hpp>

// M4 G2 (spec/m4/g2_indirect_pass.md): one ray query per pixel from the primary surface, shaded at its hit, stored as
// the pixel's initial ReSTIR GI reservoir (RtxdiContext's GI buffer). A compute pass while SER is shelved
// (considerations C-G1-3). Ray command buffer, heaps bound, after the resolve; FramePushData. Writes the GI
// reservoirs and makes them visible to later compute passes
void recordIndirectPass(const vk::raii::CommandBuffer& cmd, const vk::raii::ShaderEXT& shader,
                        vk::DeviceAddress frameRoot, vk::Extent2D extent);
