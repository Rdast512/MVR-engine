#pragma once

#include <vulkan/vulkan_raii.hpp>
#include "../core/types.hpp"
#include "../core/vk_resource_manager.hpp"

class DescriptorManager;

// linked mesh + fragment VK_EXT_shader_object pair; all raster state is dynamic
class ShaderObjects
{
public:
    ShaderObjects(ResourceManager& resourceManager, DescriptorManager& descriptorManager,
                  const vk::raii::Device& device);
    ~ShaderObjects() = default;

    void init();
    void createMeshShaders();

    // binds shaders and every state a monolithic pipeline would bake; viewport/scissor stay with the caller
    void bindMeshState(const vk::raii::CommandBuffer& cmd) const;

    const vk::raii::Device& device;
    ResourceManager& resourceManager;
    DescriptorManager& descriptorManager;
    vk::raii::ShaderEXT meshShader = nullptr;
    vk::raii::ShaderEXT fragmentShader = nullptr;
};
