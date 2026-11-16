#include "vk_rt_pipeline.hpp"
#include "../util/debug.hpp"
#include "vk_shader_objects.hpp"

#include <cstring>
#include <format>
#include <vector>

namespace
{
    // raytracing/ser.slang kIsSerReorderEnabled
    constexpr uint32_t kReorderConstantId = 0;

    constexpr vk::DeviceSize alignUp(vk::DeviceSize value, vk::DeviceSize alignment)
    {
        return (value + alignment - 1) / alignment * alignment;
    }
} // namespace

RtPipeline::RtPipeline(const Device& device, const VkAllocator& allocator, const Source& source) :
    device(device), allocator(allocator)
{
    createPipeline(source);
    createShaderBindingTable(source.debugName);
    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        commands[slot] = createHostBuffer(sizeof(vk::TraceRaysIndirectCommand2KHR), allocator.allocator,
                                          device.vkdevice, device.queueFamilyIndices,
                                          std::format("RtTraceCommand_{}_{}", source.debugName, slot), nullptr,
                                          vk::BufferUsageFlagBits2::eIndirectBuffer);
    }
}

RtPipeline::~RtPipeline()
{
    for (HostBuffer& command : commands) {
        destroyHostBuffer(allocator.allocator, command, nullptr);
    }
    destroyVmaBuffer(allocator.allocator, sbtBuffer, sbtMemory);
}

void RtPipeline::createPipeline(const Source& source)
{
    const std::vector<char> spirv = readPassSpirv(source.file);
    const vk::raii::ShaderModule module(device.vkdevice,
                                        vk::ShaderModuleCreateInfo{.codeSize = spirv.size(),
                                                                   .pCode = reinterpret_cast<const uint32_t*>(spirv.data())});
    const vk::Bool32 isReorderEnabled = source.isReorderEnabled ? vk::True : vk::False;
    const vk::SpecializationMapEntry reorderEntry{
        .constantID = kReorderConstantId, .offset = 0, .size = sizeof(isReorderEnabled)};
    const vk::SpecializationInfo specialization{.mapEntryCount = 1,
                                                .pMapEntries = &reorderEntry,
                                                .dataSize = sizeof(isReorderEnabled),
                                                .pData = &isReorderEnabled};
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eRaygenKHR,
                                          .module = *module,
                                          .pName = source.raygenEntry,
                                          .pSpecializationInfo = &specialization},
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eMissKHR, .module = *module, .pName = source.missEntry},
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eClosestHitKHR, .module = *module, .pName = source.closestHitEntry},
    };
    std::array<vk::RayTracingShaderGroupCreateInfoKHR, GroupCount> groups{};
    groups[RaygenGroup] = {.type = vk::RayTracingShaderGroupTypeKHR::eGeneral,
                           .generalShader = 0,
                           .closestHitShader = vk::ShaderUnusedKHR,
                           .anyHitShader = vk::ShaderUnusedKHR,
                           .intersectionShader = vk::ShaderUnusedKHR};
    groups[MissGroup] = {.type = vk::RayTracingShaderGroupTypeKHR::eGeneral,
                         .generalShader = 1,
                         .closestHitShader = vk::ShaderUnusedKHR,
                         .anyHitShader = vk::ShaderUnusedKHR,
                         .intersectionShader = vk::ShaderUnusedKHR};
    // closest-hit only: alpha is OMM 2-state, never any-hit
    groups[HitGroup] = {.type = vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup,
                        .generalShader = vk::ShaderUnusedKHR,
                        .closestHitShader = 2,
                        .anyHitShader = vk::ShaderUnusedKHR,
                        .intersectionShader = vk::ShaderUnusedKHR};

    // raygen invokes closest-hit shading; no shader traces further
    constexpr uint32_t kRecursionDepth = 1;
    const vk::StructureChain createInfo{
        vk::RayTracingPipelineCreateInfoKHR{
            .stageCount = static_cast<uint32_t>(stages.size()),
            .pStages = stages.data(),
            .groupCount = static_cast<uint32_t>(groups.size()),
            .pGroups = groups.data(),
            .maxPipelineRayRecursionDepth = kRecursionDepth,
        },
        // the shaders read the same heaps as the shader objects; no layout
        vk::PipelineCreateFlags2CreateInfo{.flags = vk::PipelineCreateFlagBits2::eDescriptorHeapEXT},
        // hits on CLAS geometry and the closest-hit's ClusterIDNV
        vk::RayTracingPipelineClusterAccelerationStructureCreateInfoNV{.allowClusterAccelerationStructure = vk::True},
    };
    pipeline = device.vkdevice.createRayTracingPipelineKHR(nullptr, nullptr,
                                                           createInfo.get<vk::RayTracingPipelineCreateInfoKHR>());
    setDebugName(device.vkdevice, pipeline, std::format("RtPipeline_{}", source.debugName));
}

void RtPipeline::createShaderBindingTable(std::string_view debugName)
{
    const vk::PhysicalDeviceRayTracingPipelinePropertiesKHR& properties = device.capabilities.rayTracingPipeline;
    const uint32_t handleSize = properties.shaderGroupHandleSize;
    const vk::DeviceSize recordStride = alignUp(handleSize, properties.shaderGroupHandleAlignment);
    const vk::DeviceSize regionStride = alignUp(recordStride, properties.shaderGroupBaseAlignment);

    const std::vector<uint8_t> handles =
        pipeline.getRayTracingShaderGroupHandlesKHR<uint8_t>(0, GroupCount, static_cast<size_t>(handleSize) * GroupCount);

    // written once; ReBAR memory, so no staging copy
    createBuffer(regionStride * GroupCount,
                 vk::BufferUsageFlagBits2::eShaderBindingTableKHR | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, sbtBuffer,
                 sbtMemory, allocator.allocator, device.vkdevice, device.queueFamilyIndices,
                 std::format("RtSbt_{}", debugName), VMA_ALLOCATION_CREATE_MAPPED_BIT,
                 properties.shaderGroupBaseAlignment);
    VmaAllocationInfo allocationInfo{};
    vmaGetAllocationInfo(allocator.allocator, sbtMemory, &allocationInfo);
    auto* mapped = static_cast<uint8_t*>(allocationInfo.pMappedData);
    for (uint32_t group = 0; group < GroupCount; ++group) {
        std::memcpy(mapped + regionStride * group, handles.data() + static_cast<size_t>(handleSize) * group, handleSize);
    }

    const vk::DeviceAddress base = device.vkdevice.getBufferAddress({.buffer = *sbtBuffer});
    commandTemplate = vk::TraceRaysIndirectCommand2KHR{
        .raygenShaderRecordAddress = base + regionStride * RaygenGroup,
        .raygenShaderRecordSize = recordStride,
        .missShaderBindingTableAddress = base + regionStride * MissGroup,
        .missShaderBindingTableSize = recordStride,
        .missShaderBindingTableStride = recordStride,
        .hitShaderBindingTableAddress = base + regionStride * HitGroup,
        .hitShaderBindingTableSize = recordStride,
        .hitShaderBindingTableStride = recordStride,
        .depth = 1,
    };
}

void RtPipeline::bind(const vk::raii::CommandBuffer& cmd) const
{
    cmd.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, *pipeline);
}

void RtPipeline::traceRays(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot, vk::Extent2D extent) const
{
    vk::TraceRaysIndirectCommand2KHR command = commandTemplate;
    command.width = extent.width;
    command.height = extent.height;
    // host writes before the submit are visible to it
    std::memcpy(commands[frameSlot].mapped, &command, sizeof(command));
    cmd.traceRaysIndirect2KHR(commands[frameSlot].address);
}
