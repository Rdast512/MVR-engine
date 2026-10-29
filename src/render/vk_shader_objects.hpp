#pragma once

#include <vulkan/vulkan_raii.hpp>
#include "../core/types.hpp"
#include "../core/vk_resource_manager.hpp"
#include "debug_view.hpp"

#include <array>
#include <string_view>

class DescriptorManager;

// linked mesh + fragment VK_EXT_shader_object pairs, one per RasterVariant, plus the unlinked compute shaders;
// all raster state is dynamic
class ShaderObjects
{
public:
    ShaderObjects(ResourceManager& resourceManager, DescriptorManager& descriptorManager,
                  const vk::raii::Device& device);
    ~ShaderObjects() = default;

    void init();
    void createMeshShaders();
    void createCullShaders();

    // binds the variant's shaders and every state a monolithic pipeline would bake; viewport/scissor stay with the
    // caller. only the cull mode differs between state groups
    void bindMeshState(const vk::raii::CommandBuffer& cmd, RasterVariant variant, MeshStateGroup group) const;
    static void bindComputeShader(const vk::raii::CommandBuffer& cmd, const vk::raii::ShaderEXT& shader);

    const vk::raii::Device& device;
    ResourceManager& resourceManager;
    DescriptorManager& descriptorManager;
    struct MeshShaderPair
    {
        vk::raii::ShaderEXT mesh = nullptr;
        vk::raii::ShaderEXT fragment = nullptr;
    };

    // indexed by RasterVariant
    std::array<MeshShaderPair, kRasterVariantCount> meshShaders;

    // passes/meshlet_cull.slang
    struct CullShaders
    {
        vk::raii::ShaderEXT cullMeshlets = nullptr;
        vk::raii::ShaderEXT buildMeshSequences = nullptr;
    };
    CullShaders cullShaders;

    [[nodiscard]] const MeshShaderPair& pair(RasterVariant variant) const
    {
        return meshShaders[std::to_underlying(variant)];
    }

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
    // file under ENGINE_SHADER_DIR/passes
    [[nodiscard]] vk::raii::ShaderEXT createComputeShader(std::string_view file, const char* entry,
                                                          std::string_view debugName) const;
    [[nodiscard]] bool usesDescriptorHeaps() const;
};
