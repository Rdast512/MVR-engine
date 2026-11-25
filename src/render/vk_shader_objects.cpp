#include "vk_shader_objects.hpp"
#include "../util/debug.hpp"
#include "static_headers/logger.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#ifndef ENGINE_SHADER_DIR
    #define ENGINE_SHADER_DIR "./shaders"
#endif

std::vector<char> readPassSpirv(std::string_view file)
{
    const std::string filename = (std::filesystem::path(ENGINE_SHADER_DIR) / "passes" / file).string();
    std::ifstream stream(filename, std::ios::ate | std::ios::binary);
    if (!stream.is_open()) {
        throw std::runtime_error("failed to open shader: " + filename);
    }
    std::vector<char> buffer(stream.tellg());
    stream.seekg(0, std::ios::beg);
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    return buffer;
}

bool isGpuAvResolvePerBinRequested()
{
    const char* text = std::getenv("ENGINE_GPUAV_RESOLVE_PER_BIN");
    return text != nullptr && *text != '\0' && std::string_view(text) != "0";
}

ShaderObjects::ShaderObjects(const vk::raii::Device& device) : device(device)
{
    if (isResolvePerBin) {
        logInfo("TEMPORARY GPU-AV resolve path (ENGINE_GPUAV_RESOLVE_PER_BIN, considerations C-E1-3): per-bin "
                "vkCmdDispatchIndirect, no DGC execution set; not the shipped path",
                "ShaderObjects");
    }
}

void ShaderObjects::init()
{
    createMeshShaders();
    createCullShaders();
    createHiZShaders();
    createFrameShaders();
    createResolveShaders();
    createRtShaders();
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
    visRasterAlpha = createMeshShaderPair({
        .meshFile = "mesh_raster.spv",
        .fragmentFile = "mesh_alpha_fragment.spv",
        .meshEntry = "meshAlphaMain",
        .fragmentEntry = "alphaFragMain",
        .debugName = "VisRasterAlpha",
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
    frameShaders.classify = createComputeShader("classify.spv", "classifyMain", "Classify");
    frameShaders.binArgs =
        createComputeShader("classify.spv", isResolvePerBin ? "binArgsPerBinMain" : "binArgsMain", "BinArgs");
    frameShaders.debugView = createComputeShader("debug_view.spv", "debugViewMain", "DebugView");
    frameShaders.motionCheck = createComputeShader("motion_check.spv", "motionCheckMain", "MotionCheck");
    frameShaders.rayCheck = createComputeShader("rt_check.spv", "rayCheckMain", "RayCheck");
    frameShaders.lightWorld = createComputeShader("light_world.spv", "lightWorldMain", "LightWorld");
    frameShaders.lightPdfMips = createComputeShader("light_pdf_mips.spv", "lightPdfMipsMain", "LightPdfMips");
    frameShaders.rtxdiPresample = createComputeShader("rtxdi_presample.spv", "rtxdiPresampleMain", "RtxdiPresample");
    frameShaders.rtxdiPresampleRegir =
        createComputeShader("rtxdi_presample.spv", "rtxdiPresampleRegirMain", "RtxdiPresampleRegir");
    frameShaders.rtxdiInitial = createComputeShader("rtxdi_initial.spv", "rtxdiInitialMain", "RtxdiInitial");
    frameShaders.rtxdiTemporal = createComputeShader("rtxdi_temporal.spv", "rtxdiTemporalMain", "RtxdiTemporal");
    frameShaders.rtxdiShade = createComputeShader("rtxdi_shade.spv", "rtxdiShadeMain", "RtxdiShade");
    frameShaders.indirect = createComputeShader("indirect.spv", "indirectMain", "Indirect");
    frameShaders.nrcUpdate = createComputeShader("nrc_update.spv", "nrcUpdateMain", "NrcUpdate");
    frameShaders.nrcResolve = createComputeShader("nrc_resolve.spv", "nrcResolveMain", "NrcResolve");
    frameShaders.giTemporal = createComputeShader("rtxdi_gi_temporal.spv", "rtxdiGiTemporalMain", "RtxdiGiTemporal");
    frameShaders.giShade = createComputeShader("rtxdi_gi_shade.spv", "rtxdiGiShadeMain", "RtxdiGiShade");
    frameShaders.giDuplication =
        createComputeShader("rtxdi_gi_duplication.spv", "rtxdiGiDuplicationMain", "RtxdiGiDuplication");
    frameShaders.combine = createComputeShader("combine.spv", "combineMain", "Combine");
    frameShaders.glass = createComputeShader("glass.spv", "glassMain", "Glass");
    frameShaders.exposureHistogram = createComputeShader("exposure.spv", "exposureHistogramMain", "ExposureHistogram");
    frameShaders.exposureAdapt = createComputeShader("exposure.spv", "exposureAdaptMain", "ExposureAdapt");
    frameShaders.post = createComputeShader("post.spv", "postMain", "Post");
    frameShaders.accumulate = createComputeShader("accumulate.spv", "accumulateMain", "Accumulate");
}

void ShaderObjects::createResolveShaders()
{
    struct BinEntry
    {
        MaterialBin bin;
        const char* entry;
        std::string_view debugName;
    };
    constexpr std::array<BinEntry, kMaterialBinCount> kBinEntries = {{
        {MaterialBin::Background, "resolveBackgroundMain", "ResolveBackground"},
        {MaterialBin::Standard, "resolveStandardMain", "ResolveStandard"},
        {MaterialBin::Unlit, "resolveUnlitMain", "ResolveUnlit"},
    }};
    // C-E1-3: GPU-AV disables itself on any INDIRECT_BINDABLE shader, so the temporary per-bin path drops the flag
    const vk::ShaderCreateFlagsEXT bindableFlags =
        isResolvePerBin ? vk::ShaderCreateFlagsEXT{} : vk::ShaderCreateFlagBitsEXT::eIndirectBindable;
    for (const BinEntry& binEntry : kBinEntries) {
        resolveShaders[std::to_underlying(binEntry.bin)] =
            createComputeShader("resolve.spv", binEntry.entry, binEntry.debugName, bindableFlags);
    }
}

void ShaderObjects::createRtShaders()
{
    rtShaders.writeInstances = createComputeShader("rt_instances.spv", "rtInstancesMain", "RtInstances");
}

vk::raii::ShaderEXT ShaderObjects::createComputeShader(std::string_view file, const char* entry,
                                                       std::string_view debugName,
                                                       vk::ShaderCreateFlagsEXT extraFlags) const
{
    const auto spirv = readPassSpirv(file);
    // raytracing/trace.slang [vk::constant_id(1)]; a shader that declares no such constant ignores the entry
    constexpr uint32_t kDebugRaysConstantId = 1;
    const vk::Bool32 debugRays = kDebugRays ? vk::True : vk::False;
    const vk::SpecializationMapEntry debugRaysEntry{
        .constantID = kDebugRaysConstantId, .offset = 0, .size = sizeof(debugRays)};
    const vk::SpecializationInfo specialization{
        .mapEntryCount = 1, .pMapEntries = &debugRaysEntry, .dataSize = sizeof(debugRays), .pData = &debugRays};
    const vk::ShaderCreateInfoEXT createInfo{
        .flags = vk::ShaderCreateFlagBitsEXT::eDescriptorHeap | extraFlags,
        .stage = vk::ShaderStageFlagBits::eCompute,
        .codeType = vk::ShaderCodeTypeEXT::eSpirv,
        .codeSize = spirv.size(),
        .pCode = spirv.data(),
        .pName = entry,
        .pSpecializationInfo = &specialization,
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
    const auto meshSpirv = readPassSpirv(source.meshFile);
    const auto fragSpirv = source.fragmentFile == source.meshFile ? meshSpirv : readPassSpirv(source.fragmentFile);

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

void ShaderObjects::bindMeshState(const vk::raii::CommandBuffer& cmd, RasterBin bin) const
{
    bindShaderPair(cmd, meshShaders(bin));
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
