#include "vk_dlss.hpp"
#include "static_headers/logger.hpp"
#include "util/vk_utils.hpp"

// clang-format off: the dlssd helpers use types from helpers_vk without including it
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers_dlssd_vk.h>
// clang-format on

#include <SDL3/SDL_filesystem.h>

#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    // any stable GUID; NGX keys per-application driver settings and OTA updates by it
    constexpr char kNgxProjectId[] = "8c2b4f36-2a5e-4d1b-9e4c-3f7a6b0d1e52";
    constexpr char kNgxEngineVersion[] = "0";
    // single GPU: create and show on node 0
    constexpr unsigned kNgxNodeMask = 1;
    // engine spec §2.1: every tier targets Quality
    [[nodiscard]] NVSDK_NGX_PerfQuality_Value ngxQuality(Dlss::Quality quality)
    {
        switch (quality) {
        case Dlss::Quality::Dlaa:
            return NVSDK_NGX_PerfQuality_Value_DLAA;
        case Dlss::Quality::Quality:
            return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        case Dlss::Quality::Balanced:
            return NVSDK_NGX_PerfQuality_Value_Balanced;
        case Dlss::Quality::Performance:
            return NVSDK_NGX_PerfQuality_Value_MaxPerf;
        case Dlss::Quality::UltraPerformance:
            return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        }
        return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }

    [[nodiscard]] NVSDK_NGX_RayReconstruction_Hint_Render_Preset ngxPreset(Dlss::Preset preset)
    {
        switch (preset) {
        case Dlss::Preset::Default:
            return NVSDK_NGX_RayReconstruction_Hint_Render_Preset_Default;
        case Dlss::Preset::D:
            return NVSDK_NGX_RayReconstruction_Hint_Render_Preset_D;
        case Dlss::Preset::E:
            return NVSDK_NGX_RayReconstruction_Hint_Render_Preset_E;
        case Dlss::Preset::F:
            return NVSDK_NGX_RayReconstruction_Hint_Render_Preset_F;
        }
        return NVSDK_NGX_RayReconstruction_Hint_Render_Preset_Default;
    }

    [[nodiscard]] std::string ngxResultText(NVSDK_NGX_Result result)
    {
        const std::wstring_view text = GetNGXResultAsString(result);
        // NGX's messages are ASCII
        std::string ascii(text.size(), '?');
        for (size_t i = 0; i < text.size(); ++i) {
            ascii[i] = text[i] < 0x80 ? static_cast<char>(text[i]) : '?';
        }
        return std::format("0x{:08x} {}", static_cast<unsigned>(result), ascii);
    }

    void checkNgx(NVSDK_NGX_Result result, std::string_view what)
    {
        if (NVSDK_NGX_FAILED(result)) {
            const std::string message = std::format("NGX {} failed: {}", what, ngxResultText(result));
            logError(message, "Dlss");
            throw std::runtime_error(message);
        }
    }

    void NVSDK_CONV logNgxMessage(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
    {
        std::string_view text = message;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.remove_suffix(1);
        }
        logInfo(std::string(text), "NGX");
    }

    [[nodiscard]] NVSDK_NGX_FeatureCommonInfo ngxCommonInfo()
    {
        NVSDK_NGX_FeatureCommonInfo info{};
        info.LoggingInfo.LoggingCallback = &logNgxMessage;
        info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
        info.LoggingInfo.DisableOtherLoggingSinks = true;
        return info;
    }

    [[nodiscard]] NVSDK_NGX_FeatureDiscoveryInfo ngxDiscoveryInfo(const NVSDK_NGX_FeatureCommonInfo& common)
    {
        NVSDK_NGX_FeatureDiscoveryInfo info{};
        info.SDKVersion = NVSDK_NGX_Version_API;
        info.FeatureID = NVSDK_NGX_Feature_RayReconstruction;
        info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
        info.Identifier.v.ProjectDesc = {kNgxProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kNgxEngineVersion};
        info.ApplicationDataPath = L"";
        info.FeatureInfo = &common;
        return info;
    }

    [[nodiscard]] std::vector<std::string> ngxExtensionNames(const VkExtensionProperties* properties, uint32_t count)
    {
        std::vector<std::string> names;
        names.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            names.emplace_back(properties[i].extensionName);
        }
        return names;
    }

    // NGX loads nvngx_dlssd.dll from the application folder
    [[nodiscard]] std::wstring ngxApplicationPath()
    {
        const char* base = SDL_GetBasePath();
        return base != nullptr ? std::filesystem::path(base).wstring() : std::filesystem::current_path().wstring();
    }

    [[nodiscard]] NVSDK_NGX_Resource_VK ngxResource(const Dlss::Image& image, vk::Extent2D extent, bool isWritten)
    {
        const VkImageSubresourceRange range{static_cast<VkImageAspectFlags>(image.aspect), 0, 1, 0, 1};
        return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, range,
                                                      static_cast<VkFormat>(image.format), extent.width,
                                                      extent.height, isWritten);
    }
} // namespace

SdkExtensions Dlss::requiredExtensions()
{
    const NVSDK_NGX_FeatureCommonInfo common = ngxCommonInfo();
    const NVSDK_NGX_FeatureDiscoveryInfo info = ngxDiscoveryInfo(common);
    uint32_t count = 0;
    VkExtensionProperties* properties = nullptr;
    checkNgx(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&info, &count, &properties),
          "instance extension query");
    return SdkExtensions{
        .instance = ngxExtensionNames(properties, count),
        .device = [](vk::Instance instance, vk::PhysicalDevice physicalDevice)
        {
            const NVSDK_NGX_FeatureCommonInfo deviceCommon = ngxCommonInfo();
            const NVSDK_NGX_FeatureDiscoveryInfo deviceInfo = ngxDiscoveryInfo(deviceCommon);
            uint32_t deviceCount = 0;
            VkExtensionProperties* deviceProperties = nullptr;
            checkNgx(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, physicalDevice, &deviceInfo,
                                                                         &deviceCount, &deviceProperties),
                  "device extension query");
            return ngxExtensionNames(deviceProperties, deviceCount);
        },
    };
}

Dlss::Dlss(const Device& device, FrameSync& frameSync) :
    device(device), frameSync(frameSync), driverHeap(device, "Dlss")
{
    const NVSDK_NGX_FeatureCommonInfo common = ngxCommonInfo();
    const std::wstring path = ngxApplicationPath();
    checkNgx(NVSDK_NGX_VULKAN_Init_with_ProjectID(kNgxProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kNgxEngineVersion, path.c_str(),
                                               *device.instance, *device.physicalDevice, *device.vkdevice,
                                               vkGetInstanceProcAddr, vkGetDeviceProcAddr, &common),
          "init");
    try {
        checkNgx(NVSDK_NGX_VULKAN_GetCapabilityParameters(&parameters), "capability parameters");
        int needsDriver = 0;
        NVSDK_NGX_Parameter_GetI(parameters, NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver,
                                 &needsDriver);
        int available = 0;
        const NVSDK_NGX_Result availability =
            NVSDK_NGX_Parameter_GetI(parameters, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &available);
        if (NVSDK_NGX_FAILED(availability) || available == 0 || needsDriver != 0) {
            throw std::runtime_error(std::format("DLSS Ray Reconstruction unavailable{}",
                                                 needsDriver != 0 ? ": the driver needs an update" : ""));
        }
    } catch (...) {
        if (parameters != nullptr) {
            NVSDK_NGX_VULKAN_DestroyParameters(parameters);
        }
        NVSDK_NGX_VULKAN_Shutdown1(*device.vkdevice);
        throw;
    }
    logInfo("DLSS Ray Reconstruction available", "Dlss");
}

Dlss::~Dlss()
{
    releaseFeature();
    NVSDK_NGX_VULKAN_DestroyParameters(parameters);
    NVSDK_NGX_VULKAN_Shutdown1(*device.vkdevice);
}

void Dlss::releaseFeature()
{
    if (feature != nullptr) {
        NVSDK_NGX_VULKAN_ReleaseFeature(feature);
        feature = nullptr;
    }
}

vk::Extent2D Dlss::recreate(vk::Extent2D display)
{
    releaseFeature();
    displayExtent = display;
    const NVSDK_NGX_PerfQuality_Value quality = ngxQuality(settings.quality);

    // NGX_DLSSD_GET_OPTIMAL_SETTINGS lives in a D3D-only helper in this SDK; the Vulkan side is the same callback
    void* callback = nullptr;
    checkNgx(NVSDK_NGX_Parameter_GetVoidPointer(parameters, NVSDK_NGX_Parameter_DLSSDOptimalSettingsCallback, &callback),
          "optimal settings callback");
    NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_Width, display.width);
    NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_Height, display.height);
    NVSDK_NGX_Parameter_SetI(parameters, NVSDK_NGX_Parameter_PerfQualityValue, quality);
    NVSDK_NGX_Parameter_SetI(parameters, NVSDK_NGX_Parameter_RTXValue, 0);
    checkNgx(reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback>(callback)(parameters), "optimal settings");
    unsigned width = 0;
    unsigned height = 0;
    NVSDK_NGX_Parameter_GetUI(parameters, NVSDK_NGX_Parameter_OutWidth, &width);
    NVSDK_NGX_Parameter_GetUI(parameters, NVSDK_NGX_Parameter_OutHeight, &height);
    if (width == 0 || height == 0) {
        throw std::runtime_error(std::format("DLSS-RR returned no render size for {}x{}", display.width, display.height));
    }
    renderExtent = vk::Extent2D{width, height};

    // the hint is read per quality mode; an NVIDIA app override still takes precedence (logged by NGX as DRS)
    const unsigned preset = ngxPreset(settings.preset);
    for (const char* hint : {NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA,
                             NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,
                             NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced,
                             NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,
                             NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance}) {
        NVSDK_NGX_Parameter_SetUI(parameters, hint, preset);
    }
    NVSDK_NGX_DLSSD_Create_Params create{};
    create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
    create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
    create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
    create.InWidth = renderExtent.width;
    create.InHeight = renderExtent.height;
    create.InTargetWidth = display.width;
    create.InTargetHeight = display.height;
    create.InPerfQualityValue = quality;
    // motion in render pixels, reverse z, scene-linear color
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                  NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;

    const FrameSync::OneTimeCommands commands = frameSync.computeCommands();
    commands.cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    const NVSDK_NGX_Result result =
        NGX_VULKAN_CREATE_DLSSD_EXT1(*device.vkdevice, *commands.cmd, kNgxNodeMask, kNgxNodeMask, &feature, parameters,
                                     &create);
    submitAndWait(commands.cmd, commands.queue);
    checkNgx(result, "DLSSD feature creation");
    appliedSettings = settings;
    logInfo(std::format("DLSS-RR mode {} preset {}: render {}x{} -> display {}x{}", std::to_underlying(settings.quality),
                        std::to_underlying(settings.preset), renderExtent.width, renderExtent.height, display.width,
                        display.height),
            "Dlss");
    return renderExtent;
}

void Dlss::evaluate(const vk::raii::CommandBuffer& cmd, const Frame& frame) const
{
    NVSDK_NGX_Resource_VK color = ngxResource(frame.color, renderExtent, false);
    NVSDK_NGX_Resource_VK depth = ngxResource(frame.depth, renderExtent, false);
    NVSDK_NGX_Resource_VK motion = ngxResource(frame.motion, renderExtent, false);
    NVSDK_NGX_Resource_VK diffuseAlbedo = ngxResource(frame.diffuseAlbedo, renderExtent, false);
    NVSDK_NGX_Resource_VK specularAlbedo = ngxResource(frame.specularAlbedo, renderExtent, false);
    NVSDK_NGX_Resource_VK normalRoughness = ngxResource(frame.normalRoughness, renderExtent, false);
    NVSDK_NGX_Resource_VK output = ngxResource(frame.output, displayExtent, true);
    // DLSS takes row-major, left-multiply matrices (v * M^T); glm's column-major M has the same 16-float layout
    // (reference/vk_denoise_dlssrr/dlss_rr/src/dlssrr_wrapper.cpp:379-385). Copies: NGX takes non-const pointers
    glm::mat4 worldToView = frame.view;
    glm::mat4 viewToClip = frame.proj;

    NVSDK_NGX_VK_DLSSD_Eval_Params eval{};
    eval.pInColor = &color;
    eval.pInOutput = &output;
    eval.pInDepth = &depth;
    eval.pInMotionVectors = &motion;
    eval.pInDiffuseAlbedo = &diffuseAlbedo;
    eval.pInSpecularAlbedo = &specularAlbedo;
    // packed: roughness in normals.w
    eval.pInNormals = &normalRoughness;
    eval.pInRoughness = &normalRoughness;
    // keep the post exposure feedback outside ray reconstruction
    eval.pInExposureTexture = nullptr;
    // same sign as the raster offset: measured, the opposite sign shimmers ~20x more on a still camera
    eval.InJitterOffsetX = frame.jitterPixels.x;
    eval.InJitterOffsetY = frame.jitterPixels.y;
    eval.InRenderSubrectDimensions = {renderExtent.width, renderExtent.height};
    eval.InReset = frame.isReset ? 1 : 0;
    eval.InMVScaleX = 1.0f;
    eval.InMVScaleY = 1.0f;
    eval.pInWorldToViewMatrix = &worldToView[0][0];
    eval.pInViewToClipMatrix = &viewToClip[0][0];
    eval.InFrameTimeDeltaInMsec = frame.frameDeltaMs;
    // NGX's kernels read and write the engine's heap slots otherwise (C-H1-1)
    driverHeap.bind(cmd);
    checkNgx(NGX_VULKAN_EVALUATE_DLSSD_EXT(*cmd, feature, parameters, &eval), "DLSSD evaluate");
}
