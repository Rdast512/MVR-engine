#include "vk_shader_objects.hpp"
#include "../util/debug.hpp"

#include <array>
#include <filesystem>
#include <format>
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

ShaderObjects::ShaderObjects(const vk::raii::Device& device) : device(device) {}

void ShaderObjects::init()
{
    createMeshShaders();
    createCullShaders();
    createHiZShaders();
    createFrameShaders();
}

void ShaderObjects::createMeshShaders()
{
    visRaster = createMeshShaderPair({
        .meshFile = "mesh_raster.spv",
        .fragmentFile = "mesh_fragment.spv",
        .meshEntry = "meshMain",
        .fragmentEntry = "fragMain",
        .debugName = "VisRaster",
    });
    hizView = createMeshShaderPair({
        .meshFile = "hiz_view.spv",
        .fragmentFile = "hiz_view.spv",
        .meshEntry = "hizViewMesh",
        .fragmentEntry = "hizViewFrag",
        .debugName = "HiZView",
    });
    present = createMeshShaderPair({
        .meshFile = "present.spv",
        .fragmentFile = "present.spv",
        .meshEntry = "presentMesh",
        .fragmentEntry = "presentFrag",
        .debugName = "Present",
    });
}

void ShaderObjects::createCullShaders()
{
    cullShaders.cullInstances = createComputeShader("meshlet_cull.spv", "cullInstances", "CullInstances");
    cullShaders.cullMeshlets = createComputeShader("meshlet_cull.spv", "cullMeshlets", "CullMeshlets");
    cullShaders.cullRejectedMeshlets =
        createComputeShader("meshlet_cull.spv", "cullRejectedMeshlets", "CullRejectedMeshlets");
    cullShaders.buildMeshSequences = createComputeShader("meshlet_cull.spv", "buildMeshSequences", "BuildMeshSequences");
}

void ShaderObjects::createHiZShaders()
{
    hizShaders.build = createComputeShader("hiz_build.spv", "hizBuild", "HiZBuild");
}

void ShaderObjects::createFrameShaders()
{
    frameShaders.resolve = createComputeShader("resolve.spv", "resolveMain", "Resolve");
    frameShaders.debugView = createComputeShader("debug_view.spv", "debugViewMain", "DebugView");
}

vk::raii::ShaderEXT ShaderObjects::createComputeShader(std::string_view file, const char* entry,
                                                       std::string_view debugName) const
{
    const auto spirv = readFile((std::filesystem::path(ENGINE_SHADER_DIR) / "passes" / file).string());
    const vk::ShaderCreateInfoEXT createInfo{
        .flags = vk::ShaderCreateFlagBitsEXT::eDescriptorHeap,
        .stage = vk::ShaderStageFlagBits::eCompute,
        .codeType = vk::ShaderCodeTypeEXT::eSpirv,
        .codeSize = spirv.size(),
        .pCode = spirv.data(),
        .pName = entry,
    };
    vk::raii::ShaderEXT shader = device.createShaderEXT(createInfo);
    setDebugName(device, shader, std::format("ShaderObject_{}_Compute", debugName));
    return shader;
}

void ShaderObjects::bindComputeShader(const vk::raii::CommandBuffer& cmd, const vk::raii::ShaderEXT& shader)
{
    constexpr vk::ShaderStageFlagBits stage = vk::ShaderStageFlagBits::eCompute;
    cmd.bindShadersEXT(stage, *shader);
}

ShaderObjects::MeshShaderPair ShaderObjects::createMeshShaderPair(const MeshShaderSource& source) const
{
    const auto passDir = std::filesystem::path(ENGINE_SHADER_DIR) / "passes";
    const auto meshSpirv = readFile((passDir / source.meshFile).string());
    const auto fragSpirv =
        source.fragmentFile == source.meshFile ? meshSpirv : readFile((passDir / source.fragmentFile).string());

    // link stage lets the driver optimize the mesh->fragment interface like a monolithic pipeline;
    // heap mode runs without set layouts or push constant ranges
    constexpr vk::ShaderCreateFlagsEXT linkFlags =
        vk::ShaderCreateFlagBitsEXT::eLinkStage | vk::ShaderCreateFlagBitsEXT::eDescriptorHeap;

    const std::array createInfos = {
        // mesh-only: no task stage, no vertex input
        vk::ShaderCreateInfoEXT{
            .flags = linkFlags | vk::ShaderCreateFlagBitsEXT::eNoTaskShader,
            .stage = vk::ShaderStageFlagBits::eMeshEXT,
            .nextStage = vk::ShaderStageFlagBits::eFragment,
            .codeType = vk::ShaderCodeTypeEXT::eSpirv,
            .codeSize = meshSpirv.size(),
            .pCode = meshSpirv.data(),
            .pName = source.meshEntry,
        },
        vk::ShaderCreateInfoEXT{
            .flags = linkFlags,
            .stage = vk::ShaderStageFlagBits::eFragment,
            .codeType = vk::ShaderCodeTypeEXT::eSpirv,
            .codeSize = fragSpirv.size(),
            .pCode = fragSpirv.data(),
            .pName = source.fragmentEntry,
        },
    };

    // linked stages must be created in one call
    std::vector<vk::raii::ShaderEXT> shaders = device.createShadersEXT(createInfos);
    MeshShaderPair result{.mesh = std::move(shaders[0]), .fragment = std::move(shaders[1])};
    setDebugName(device, result.mesh, std::format("ShaderObject_{}_Mesh", source.debugName));
    setDebugName(device, result.fragment, std::format("ShaderObject_{}_Fragment", source.debugName));
    return result;
}

void ShaderObjects::bindMeshState(const vk::raii::CommandBuffer& cmd) const
{
    bindShaderPair(cmd, visRaster);
    setRasterState(cmd, vk::CullModeFlagBits::eBack, true);
}

void ShaderObjects::bindHiZViewState(const vk::raii::CommandBuffer& cmd) const { setFullscreenState(cmd, hizView); }

void ShaderObjects::bindPresentState(const vk::raii::CommandBuffer& cmd) const { setFullscreenState(cmd, present); }

void ShaderObjects::setFullscreenState(const vk::raii::CommandBuffer& cmd, const MeshShaderPair& shaderPair)
{
    bindShaderPair(cmd, shaderPair);
    setRasterState(cmd, vk::CullModeFlagBits::eNone, false);
}

void ShaderObjects::bindShaderPair(const vk::raii::CommandBuffer& cmd, const MeshShaderPair& shaderPair)
{
    // with meshShader/taskShader/geometryShader enabled every graphics stage must be bound explicitly
    constexpr std::array stages = {
        vk::ShaderStageFlagBits::eVertex, vk::ShaderStageFlagBits::eGeometry, vk::ShaderStageFlagBits::eTaskEXT,
        vk::ShaderStageFlagBits::eMeshEXT, vk::ShaderStageFlagBits::eFragment,
    };
    const std::array<vk::ShaderEXT, stages.size()> shaders = {nullptr, nullptr, nullptr, *shaderPair.mesh,
                                                              *shaderPair.fragment};
    cmd.bindShadersEXT(stages, shaders);
}

void ShaderObjects::setRasterState(const vk::raii::CommandBuffer& cmd, vk::CullModeFlags cullMode,
                                   bool isDepthEnabled)
{
    // rasterization
    cmd.setRasterizerDiscardEnable(vk::False);
    cmd.setPolygonModeEXT(vk::PolygonMode::eFill);
    cmd.setCullMode(cullMode);
    cmd.setFrontFace(vk::FrontFace::eCounterClockwise);
    cmd.setDepthBiasEnable(vk::False);
    // pipelineFragmentShadingRate is enabled, so the rate is required state
    cmd.setFragmentShadingRateKHR({1, 1}, {vk::FragmentShadingRateCombinerOpKHR::eKeep,
                                           vk::FragmentShadingRateCombinerOpKHR::eKeep});

    // multisample
    constexpr vk::SampleMask allSamples = ~0u;
    cmd.setRasterizationSamplesEXT(vk::SampleCountFlagBits::e1);
    cmd.setSampleMaskEXT(vk::SampleCountFlagBits::e1, allSamples);
    cmd.setAlphaToCoverageEnableEXT(vk::False);

    // depth / stencil
    cmd.setDepthTestEnable(isDepthEnabled ? vk::True : vk::False);
    cmd.setDepthWriteEnable(isDepthEnabled ? vk::True : vk::False);
    // reverse Z (core/culling.slang kClipNear, farthestDepth)
    cmd.setDepthCompareOp(vk::CompareOp::eGreater);
    cmd.setStencilTestEnable(vk::False);

    // color attachment 0
    cmd.setColorBlendEnableEXT(0, vk::False);
    cmd.setColorWriteMaskEXT(0, vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                    vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
}
