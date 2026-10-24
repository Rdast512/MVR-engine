#pragma once

#include <vulkan/vulkan_raii.hpp>
#include "../core/types.hpp"
#include "../core/vk_resource_manager.hpp"
#include "debug_view.hpp"

#include <array>
#include <string_view>

class DescriptorManager;

// linked mesh + fragment VK_EXT_shader_object pairs, one per RasterVariant; all raster state is dynamic
class ShaderObjects
{
public:
    ShaderObjects(ResourceManager& resourceManager, DescriptorManager& descriptorManager,
                  const vk::raii::Device& device);
    ~ShaderObjects() = default;

    void init();
    void createMeshShaders();

    // binds the variant's shaders and every state a monolithic pipeline would bake; viewport/scissor stay with the
    // caller. state is identical across variants
    void bindMeshState(const vk::raii::CommandBuffer& cmd, RasterVariant variant) const;

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
};
