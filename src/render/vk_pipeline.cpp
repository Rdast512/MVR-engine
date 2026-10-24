#include "vk_pipeline.hpp"
#include "../core/vk_descriptors.hpp"
#include "../util/debug.hpp"

#include <array>
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

Pipeline::Pipeline(ResourceManager& resourceManager, DescriptorManager& descriptorManager,
                   const vk::raii::Device& device, const vk::Extent2D& swapChainExtent,
                   const vk::Format& swapChainImageFormat) :
    device(device), swapChainExtent(swapChainExtent), swapChainImageFormat(swapChainImageFormat),
    resourceManager(resourceManager), descriptorManager(descriptorManager)
{
}

void Pipeline::init()
{
    createMeshPipeline();
}

void Pipeline::createMeshPipeline()
{
    const auto shaderDir = std::filesystem::path(ENGINE_SHADER_DIR);
    const auto meshSpirv = readFile((shaderDir / "passes" / "mesh_raster.spv").string());
    const auto fragSpirv = readFile((shaderDir / "passes" / "mesh_fragment.spv").string());
    const vk::ShaderModuleCreateInfo meshModuleInfo{
        .codeSize = meshSpirv.size(),
        .pCode = reinterpret_cast<const uint32_t*>(meshSpirv.data()),
    };
    const vk::ShaderModuleCreateInfo fragModuleInfo{
        .codeSize = fragSpirv.size(),
        .pCode = reinterpret_cast<const uint32_t*>(fragSpirv.data()),
    };
    const bool useDescriptorHeaps = descriptorManager.descriptorBindingMode == DescriptorBindingMode::DescriptorHeaps;

    // mesh-only: no task stage, no vertex input
    // maintenance5 allows inline spir-v without a shader module
    const vk::PipelineShaderStageCreateInfo meshShaderStageInfo{
        .pNext = &meshModuleInfo,
        .stage = vk::ShaderStageFlagBits::eMeshEXT,
        .module = {},
        .pName = "meshMain",
    };
    const vk::PipelineShaderStageCreateInfo fragShaderStageInfo{
        .pNext = &fragModuleInfo,
        .stage = vk::ShaderStageFlagBits::eFragment,
        .module = {},
        .pName = "fragMain",
    };
    const std::array shaderStages = {meshShaderStageInfo, fragShaderStageInfo};

    const std::array dynamicStates = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamicState{
        .dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()),
        .pDynamicStates = dynamicStates.data(),
    };

    const vk::Viewport viewport{
        0.0f, 0.0f, static_cast<float>(swapChainExtent.width), static_cast<float>(swapChainExtent.height), 0.0f, 1.0f,
    };
    const vk::Rect2D scissor{vk::Offset2D{0, 0}, swapChainExtent};
    const vk::PipelineViewportStateCreateInfo viewportState{
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };

    const vk::PipelineRasterizationStateCreateInfo rasterizer{
        .depthClampEnable = vk::False,
        .rasterizerDiscardEnable = vk::False,
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eBack,
        .frontFace = vk::FrontFace::eCounterClockwise,
        .depthBiasEnable = vk::False,
        .depthBiasSlopeFactor = 1.0f,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = resourceManager.msaaSamples,
        .sampleShadingEnable = vk::True,
    };

    const vk::PipelineColorBlendAttachmentState colorBlendAttachment{
        .blendEnable = vk::False,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
    const vk::PipelineColorBlendStateCreateInfo colorBlending{
        .logicOpEnable = vk::False,
        .logicOp = vk::LogicOp::eCopy,
        .attachmentCount = 1,
        .pAttachments = &colorBlendAttachment,
    };

    const vk::PipelineDepthStencilStateCreateInfo depthStencil{
        .depthTestEnable = vk::True,
        .depthWriteEnable = vk::True,
        .depthCompareOp = vk::CompareOp::eLess,
        .depthBoundsTestEnable = vk::False,
        .stencilTestEnable = vk::False,
    };

    // heap mode runs without a pipeline layout
    if (useDescriptorHeaps) {
        pipelineLayout = VK_NULL_HANDLE;
    } else {
        const vk::PipelineLayoutCreateInfo pipelineLayoutInfo{
            .setLayoutCount = 1,
            .pSetLayouts = &*descriptorManager.descriptorSetLayout,
        };
        pipelineLayout = vk::raii::PipelineLayout(device, pipelineLayoutInfo);
    }

    setDebugName(device, pipelineLayout, "PipelineLayout_Mesh");

    const vk::Format depthFormat = resourceManager.findDepthFormat();
    vk::PipelineRenderingCreateInfo pipelineRenderingCreateInfo{
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &swapChainImageFormat,
        .depthAttachmentFormat = depthFormat,
    };
    vk::PipelineCreateFlags2CreateInfo pipelineFlags2CreateInfo{
        .pNext = &pipelineRenderingCreateInfo,
        .flags = vk::PipelineCreateFlagBits2::eDescriptorHeapEXT,
    };
    const void* pipelinePNext = useDescriptorHeaps ? static_cast<const void*>(&pipelineFlags2CreateInfo)
                                                   : static_cast<const void*>(&pipelineRenderingCreateInfo);

    // mesh pipelines must not mix with vertex input state
    const vk::GraphicsPipelineCreateInfo pipelineInfo{
        .pNext = pipelinePNext,
        .stageCount = static_cast<uint32_t>(shaderStages.size()),
        .pStages = shaderStages.data(),
        .pVertexInputState = nullptr,
        .pInputAssemblyState = nullptr,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depthStencil,
        .pColorBlendState = &colorBlending,
        .pDynamicState = &dynamicState,
        .layout = *pipelineLayout,
        .renderPass = nullptr,
    };

    pipeline = vk::raii::Pipeline(device, nullptr, pipelineInfo);
    setDebugName(device, pipeline, "GraphicsPipeline_Mesh");
}
