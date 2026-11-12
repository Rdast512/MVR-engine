#pragma once

#include <vulkan/vulkan_raii.hpp>
#include "../core/types.hpp"

#include <array>
#include <string_view>

class DescriptorManager;

// linked mesh + fragment VK_EXT_shader_object pairs plus the unlinked compute shaders; all raster state is dynamic
class ShaderObjects
{
public:
    ShaderObjects(DescriptorManager& descriptorManager, const vk::raii::Device& device);
    ~ShaderObjects() = default;

    void init();
    void createMeshShaders();
    void createCullShaders();
    void createHiZShaders();
    void createFrameShaders();

    // vis-buffer raster and every state a monolithic pipeline would bake; viewport/scissor stay with the caller.
    // One state serves every RasterBin until M6 gives AlphaTested its own fragment shader
    void bindMeshState(const vk::raii::CommandBuffer& cmd) const;
    // fullscreen Hi-Z mip view: no culling, no depth; push HiZViewPushData, then draw one mesh task
    void bindHiZViewState(const vk::raii::CommandBuffer& cmd) const;
    // fullscreen display image -> swapchain: no culling, no depth; push PresentPushData, then draw one mesh task
    void bindPresentState(const vk::raii::CommandBuffer& cmd) const;
    static void bindComputeShader(const vk::raii::CommandBuffer& cmd, const vk::raii::ShaderEXT& shader);

    const vk::raii::Device& device;
    DescriptorManager& descriptorManager;
    struct MeshShaderPair
    {
        vk::raii::ShaderEXT mesh = nullptr;
        vk::raii::ShaderEXT fragment = nullptr;
    };

    // passes/mesh_raster.slang + passes/mesh_fragment.slang
    MeshShaderPair visRaster;

    // passes/meshlet_cull.slang
    struct CullShaders
    {
        vk::raii::ShaderEXT cullInstances = nullptr;
        vk::raii::ShaderEXT cullMeshlets = nullptr;
        vk::raii::ShaderEXT cullRejectedMeshlets = nullptr;
        vk::raii::ShaderEXT buildMeshSequences = nullptr;
    };
    CullShaders cullShaders;

    // passes/hiz_build.slang
    struct HiZShaders
    {
        vk::raii::ShaderEXT build = nullptr;
    };
    HiZShaders hizShaders;
    // passes/hiz_view.slang
    MeshShaderPair hizView;
    // passes/present.slang
    MeshShaderPair present;

    // FramePushData compute passes after the late raster
    struct FrameShaders
    {
        // passes/resolve.slang
        vk::raii::ShaderEXT resolve = nullptr;
        // passes/debug_view.slang
        vk::raii::ShaderEXT debugView = nullptr;
    };
    FrameShaders frameShaders;

private:
    struct MeshShaderSource
    {
        // files under ENGINE_SHADER_DIR/passes; both stages may share one module
        std::string_view meshFile;
        std::string_view fragmentFile;
        const char* meshEntry;
        const char* fragmentEntry;
        std::string_view debugName;
    };

    [[nodiscard]] MeshShaderPair createMeshShaderPair(const MeshShaderSource& source) const;
    static void bindShaderPair(const vk::raii::CommandBuffer& cmd, const MeshShaderPair& shaderPair);
    // every state a monolithic pipeline would bake, except viewport/scissor
    static void setRasterState(const vk::raii::CommandBuffer& cmd, vk::CullModeFlags cullMode, bool isDepthEnabled);
    // fullscreen triangle into the swapchain
    static void setFullscreenState(const vk::raii::CommandBuffer& cmd, const MeshShaderPair& shaderPair);
    // file under ENGINE_SHADER_DIR/passes
    [[nodiscard]] vk::raii::ShaderEXT createComputeShader(std::string_view file, const char* entry,
                                                          std::string_view debugName) const;
    [[nodiscard]] bool usesDescriptorHeaps() const;
};
