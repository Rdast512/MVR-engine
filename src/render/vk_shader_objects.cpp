#include "vk_shader_objects.hpp"
#include "../core/vk_descriptors.hpp"
#include "../util/debug.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

#ifndef ENGINE_SHADER_DIR
    #define ENGINE_SHADER_DIR "./shaders"
#endif

namespace
{
    std::vector<char> readFile(const std::string& filename)
    {
        std::ifstream file(filename, std::ios::ate | std::ios::binary);

        if (!file.is_open()) {
            throw std::runtime_error("failed to open shader: " + filename);
        }
        std::vector<char> buffer(file.tellg());
        file.seekg(0, std::ios::beg);
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        return buffer;
    }
} // namespace

ShaderObjects::ShaderObjects(ResourceManager& resourceManager, DescriptorManager& descriptorManager,
                             const vk::raii::Device& device) :
    device(device), resourceManager(resourceManager), descriptorManager(descriptorManager)
{
}

void ShaderObjects::init()
{
    createMeshShaders();
}

void ShaderObjects::createMeshShaders()
{
    const auto shaderDir = std::filesystem::path(ENGINE_SHADER_DIR);
    const auto meshSpirv = readFile((shaderDir / "passes" / "mesh_raster.spv").string());
    const auto fragSpirv = readFile((shaderDir / "passes" / "mesh_fragment.spv").string());
    const bool useDescriptorHeaps = descriptorManager.descriptorBindingMode == DescriptorBindingMode::DescriptorHeaps;

    // link stage lets the driver optimize the mesh->fragment interface like a monolithic pipeline
    vk::ShaderCreateFlagsEXT linkFlags = vk::ShaderCreateFlagBitsEXT::eLinkStage;
    if (useDescriptorHeaps) {
        // heap mode runs without set layouts or push constant ranges
        linkFlags |= vk::ShaderCreateFlagBitsEXT::eDescriptorHeap;
    }
    const uint32_t setLayoutCount = useDescriptorHeaps ? 0u : 1u;
    const vk::DescriptorSetLayout* setLayouts = useDescriptorHeaps ? nullptr : &*descriptorManager.descriptorSetLayout;

    const std::array createInfos = {
        // mesh-only: no task stage, no vertex input
        vk::ShaderCreateInfoEXT{
            .flags = linkFlags | vk::ShaderCreateFlagBitsEXT::eNoTaskShader,
            .stage = vk::ShaderStageFlagBits::eMeshEXT,
            .nextStage = vk::ShaderStageFlagBits::eFragment,
            .codeType = vk::ShaderCodeTypeEXT::eSpirv,
            .codeSize = meshSpirv.size(),
            .pCode = meshSpirv.data(),
            .pName = "meshMain",
            .setLayoutCount = setLayoutCount,
            .pSetLayouts = setLayouts,
        },
        vk::ShaderCreateInfoEXT{
            .flags = linkFlags,
            .stage = vk::ShaderStageFlagBits::eFragment,
            .codeType = vk::ShaderCodeTypeEXT::eSpirv,
            .codeSize = fragSpirv.size(),
            .pCode = fragSpirv.data(),
            .pName = "fragMain",
            .setLayoutCount = setLayoutCount,
            .pSetLayouts = setLayouts,
        },
    };

    // linked stages must be created in one call
    std::vector<vk::raii::ShaderEXT> shaders = device.createShadersEXT(createInfos);
    meshShader = std::move(shaders[0]);
    fragmentShader = std::move(shaders[1]);
    setDebugName(device, meshShader, "ShaderObject_MeshRaster");
    setDebugName(device, fragmentShader, "ShaderObject_MeshFragment");
}

void ShaderObjects::bindMeshState(const vk::raii::CommandBuffer& cmd) const
{
    // with meshShader/taskShader/geometryShader enabled every graphics stage must be bound explicitly
    constexpr std::array stages = {
        vk::ShaderStageFlagBits::eVertex, vk::ShaderStageFlagBits::eGeometry, vk::ShaderStageFlagBits::eTaskEXT,
        vk::ShaderStageFlagBits::eMeshEXT, vk::ShaderStageFlagBits::eFragment,
    };
    const std::array<vk::ShaderEXT, stages.size()> shaders = {nullptr, nullptr, nullptr, *meshShader, *fragmentShader};
    cmd.bindShadersEXT(stages, shaders);

    // rasterization
    cmd.setRasterizerDiscardEnable(vk::False);
    cmd.setPolygonModeEXT(vk::PolygonMode::eFill);
    cmd.setCullMode(vk::CullModeFlagBits::eBack);
    cmd.setFrontFace(vk::FrontFace::eCounterClockwise);
    cmd.setDepthBiasEnable(vk::False);
    // pipelineFragmentShadingRate is enabled, so the rate is required state
    cmd.setFragmentShadingRateKHR({1, 1}, {vk::FragmentShadingRateCombinerOpKHR::eKeep,
                                           vk::FragmentShadingRateCombinerOpKHR::eKeep});

    // multisample
    const vk::SampleCountFlagBits samples = resourceManager.msaaSamples;
    constexpr vk::SampleMask allSamples = ~0u;
    cmd.setRasterizationSamplesEXT(samples);
    cmd.setSampleMaskEXT(samples, allSamples);
    cmd.setAlphaToCoverageEnableEXT(vk::False);

    // depth / stencil
    cmd.setDepthTestEnable(vk::True);
    cmd.setDepthWriteEnable(vk::True);
    cmd.setDepthCompareOp(vk::CompareOp::eLess);
    cmd.setStencilTestEnable(vk::False);

    // color attachment 0
    cmd.setColorBlendEnableEXT(0, vk::False);
    cmd.setColorWriteMaskEXT(0, vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                    vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
}
