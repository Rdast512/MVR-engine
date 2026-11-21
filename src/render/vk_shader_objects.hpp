#pragma once

#include <vulkan/vulkan_raii.hpp>
#include "../core/types.hpp"

#include <array>
#include <string_view>
#include <vector>

// Debug builds count the ray-query candidates the ray contract rules out (raytracing/trace.slang kDebugRays,
// spec/m6/d4_ray_contract.md); compute shaders are specialized with it
#ifdef NDEBUG
inline constexpr bool kDebugRays = false;
#else
inline constexpr bool kDebugRays = true;
#endif

// SPIR-V of a file under ENGINE_SHADER_DIR/passes; throws when it cannot be opened
[[nodiscard]] std::vector<char> readPassSpirv(std::string_view file);

// linked mesh + fragment VK_EXT_shader_object pairs plus the unlinked compute shaders; all raster state is dynamic
class ShaderObjects
{
public:
    explicit ShaderObjects(const vk::raii::Device& device);
    ~ShaderObjects() = default;

    void init();
    void createMeshShaders();
    void createCullShaders();
    void createHiZShaders();
    void createFrameShaders();
    void createResolveShaders();
    void createRtShaders();

    // the bin's vis-buffer raster pair and every state a monolithic pipeline would bake; viewport/scissor stay with
    // the caller. The bins differ only in their shaders
    void bindMeshState(const vk::raii::CommandBuffer& cmd, RasterBin bin) const;
    // fullscreen Hi-Z mip view: no culling, no depth; push HiZViewPushData, then draw one mesh task
    void bindHiZViewState(const vk::raii::CommandBuffer& cmd) const;
    // fullscreen display image -> swapchain: no culling, no depth; push PresentPushData, then draw one mesh task
    void bindPresentState(const vk::raii::CommandBuffer& cmd) const;
    static void bindComputeShader(const vk::raii::CommandBuffer& cmd, const vk::raii::ShaderEXT& shader);

    const vk::raii::Device& device;
    struct MeshShaderPair
    {
        vk::raii::ShaderEXT mesh = nullptr;
        vk::raii::ShaderEXT fragment = nullptr;
    };

    // passes/mesh_raster.slang meshMain + passes/mesh_fragment.slang
    MeshShaderPair visRaster;
    // passes/mesh_raster.slang meshAlphaMain + passes/mesh_alpha_fragment.slang (spec/m6/d3_alpha_raster_bin.md)
    MeshShaderPair visRasterAlpha;
    [[nodiscard]] const MeshShaderPair& meshShaders(RasterBin bin) const
    {
        return bin == RasterBin::AlphaTested ? visRasterAlpha : visRaster;
    }

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
        // passes/classify.slang: material bin per pixel and tile lists, then the resolve's DGC sequences
        vk::raii::ShaderEXT classify = nullptr;
        vk::raii::ShaderEXT binArgs = nullptr;
        // passes/debug_view.slang
        vk::raii::ShaderEXT debugView = nullptr;
        // passes/motion_check.slang, MotionCheckPushData; ENGINE_MOTION_TEST only
        vk::raii::ShaderEXT motionCheck = nullptr;
        // passes/rt_check.slang, RayCheckPushData; ENGINE_RT_CHECK only
        vk::raii::ShaderEXT rayCheck = nullptr;
        // passes/light_world.slang; async compute, before the TLAS build
        vk::raii::ShaderEXT lightWorld = nullptr;
        // passes/light_pdf_mips.slang (LightPdfMipsPushData) and passes/rtxdi_presample.slang: the local light PDF's
        // mips, Power_RIS tiles and ReGIR cells, async compute after light_world
        vk::raii::ShaderEXT lightPdfMips = nullptr;
        vk::raii::ShaderEXT rtxdiPresample = nullptr;
        vk::raii::ShaderEXT rtxdiPresampleRegir = nullptr;
        // passes/rtxdi_initial.slang, rtxdi_temporal.slang and rtxdi_shade.slang: ReSTIR DI, after the TLAS wait
        vk::raii::ShaderEXT rtxdiInitial = nullptr;
        vk::raii::ShaderEXT rtxdiTemporal = nullptr;
        vk::raii::ShaderEXT rtxdiShade = nullptr;
        // passes/indirect.slang: the bounce ray query, NRC's query paths
        vk::raii::ShaderEXT indirect = nullptr;
        // passes/nrc_update.slang: NRC's training paths
        vk::raii::ShaderEXT nrcUpdate = nullptr;
        // passes/nrc_resolve.slang: NRC's cache term into the GI reservoirs
        vk::raii::ShaderEXT nrcResolve = nullptr;
        // passes/rtxdi_gi_temporal.slang and rtxdi_gi_shade.slang: ReSTIR GI into indirectRadiance
        vk::raii::ShaderEXT giTemporal = nullptr;
        vk::raii::ShaderEXT giShade = nullptr;
        // passes/rtxdi_gi_duplication.slang: the duplication map for history reduction
        vk::raii::ShaderEXT giDuplication = nullptr;
        // passes/combine.slang: noisyColor, the DLSS-RR color input
        vk::raii::ShaderEXT combine = nullptr;
        // passes/exposure.slang: luminance histogram of the DLSS-RR output, then the adapted exposure
        vk::raii::ShaderEXT exposureHistogram = nullptr;
        vk::raii::ShaderEXT exposureAdapt = nullptr;
        // passes/post.slang: exposure + tonemap into the post target
        vk::raii::ShaderEXT post = nullptr;
    };
    FrameShaders frameShaders;

    // passes/resolve.slang, one per MaterialBin: the resolve's DGC execution set (ResolvePushData)
    std::array<vk::raii::ShaderEXT, kMaterialBinCount> resolveShaders = {nullptr, nullptr, nullptr};

    // async compute RT scene passes
    struct RtShaders
    {
        // passes/rt_instances.slang, RtInstancesPushData
        vk::raii::ShaderEXT writeInstances = nullptr;
    };
    RtShaders rtShaders;

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
    // extraFlags: on top of eDescriptorHeap
    [[nodiscard]] vk::raii::ShaderEXT createComputeShader(std::string_view file, const char* entry,
                                                          std::string_view debugName,
                                                          vk::ShaderCreateFlagsEXT extraFlags = {}) const;
};
