// Linkage test for the DLSS SDK: nvsdk_ngx_d.lib is MSVC-built and linked through
// ngx_msvc_shim. Initializes NGX, then creates and releases the SR, RR and FG features,
// so a missing CRT piece or ABI mismatch after a DLSS bump fails here instead of in the renderer.
#include "test_support.hpp"

// clang-format off: dlssd helpers use types from helpers_vk without including it
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers_dlssd_vk.h>
#include <nvsdk_ngx_helpers_dlssg_vk.h>
// clang-format on

#include <concepts>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>
#include <vector>

using test::check;

namespace
{
    constexpr uint32_t kNvidiaVendorId = 0x10DE;
    // any stable GUID; NGX keys per-app driver settings by it
    constexpr char kProjectId[] = "8c2b4f36-2a5e-4d1b-9e4c-3f7a6b0d1e52";
    constexpr char kEngineVersion[] = "0";
    // single gpu: create and show on node 0
    constexpr unsigned kNodeMask = 1;
    constexpr unsigned kRenderWidth = 960;
    constexpr unsigned kRenderHeight = 540;
    constexpr unsigned kOutputWidth = 1920;
    constexpr unsigned kOutputHeight = 1080;

    bool isOk(NVSDK_NGX_Result result, const char* what)
    {
        const bool ok = NVSDK_NGX_SUCCEED(result);
        if (!ok) {
            std::printf("  result 0x%08x\n", static_cast<unsigned>(result));
        }
        return check(ok, what);
    }

    // NVSDK_NGX_Parameter is an MSVC-vtable interface: use the C accessors only
    bool isAvailable(NVSDK_NGX_Parameter* capabilities, const char* parameter)
    {
        int available = 0;
        return NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(capabilities, parameter, &available)) && available != 0;
    }

    void createAndRelease(const test::VulkanContext& vulkan, const char* what,
                          std::invocable<VkCommandBuffer, NVSDK_NGX_Handle**> auto&& create)
    {
        NVSDK_NGX_Handle* handle = nullptr;
        VkCommandBuffer cmd = vulkan.beginCommands();
        const NVSDK_NGX_Result result = create(cmd, &handle);
        check(vulkan.submit(cmd), "feature creation submitted");
        vulkan.waitIdle();
        if (isOk(result, what) && handle != nullptr) {
            isOk(NVSDK_NGX_VULKAN_ReleaseFeature(handle), "ReleaseFeature");
        }
    }

    void runFeatures(const test::VulkanContext& vulkan, NVSDK_NGX_Parameter* capabilities)
    {
        NVSDK_NGX_Parameter* parameters = nullptr;
        if (!isOk(NVSDK_NGX_VULKAN_AllocateParameters(&parameters), "AllocateParameters")) {
            return;
        }

        if (check(isAvailable(capabilities, NVSDK_NGX_Parameter_SuperSampling_Available), "DLSS SR available")) {
            createAndRelease(vulkan, "DLSS SR created",
                             [&](VkCommandBuffer cmd, NVSDK_NGX_Handle** handle)
                             {
                                 NVSDK_NGX_DLSS_Create_Params params{};
                                 params.Feature = {kRenderWidth, kRenderHeight, kOutputWidth, kOutputHeight,
                                                   NVSDK_NGX_PerfQuality_Value_Balanced};
                                 return NGX_VULKAN_CREATE_DLSS_EXT1(vulkan.device, cmd, kNodeMask, kNodeMask, handle,
                                                                    parameters, &params);
                             });
        }

        if (check(isAvailable(capabilities, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available),
                  "DLSS RR available")) {
            createAndRelease(vulkan, "DLSS RR created",
                             [&](VkCommandBuffer cmd, NVSDK_NGX_Handle** handle)
                             {
                                 NVSDK_NGX_DLSSD_Create_Params params{};
                                 params.InWidth = kRenderWidth;
                                 params.InHeight = kRenderHeight;
                                 params.InTargetWidth = kOutputWidth;
                                 params.InTargetHeight = kOutputHeight;
                                 params.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_Balanced;
                                 return NGX_VULKAN_CREATE_DLSSD_EXT1(vulkan.device, cmd, kNodeMask, kNodeMask, handle,
                                                                     parameters, &params);
                             });
        }

        if (check(isAvailable(capabilities, NVSDK_NGX_Parameter_FrameGeneration_Available), "DLSS FG available")) {
            createAndRelease(vulkan, "DLSS FG created",
                             [&](VkCommandBuffer cmd, NVSDK_NGX_Handle** handle)
                             {
                                 NVSDK_NGX_DLSSG_Create_Params params{};
                                 params.Width = kOutputWidth;
                                 params.Height = kOutputHeight;
                                 params.NativeBackbufferFormat = VK_FORMAT_B8G8R8A8_UNORM;
                                 params.RenderWidth = kOutputWidth;
                                 params.RenderHeight = kOutputHeight;
                                 return NGX_VK_CREATE_DLSSG(cmd, kNodeMask, kNodeMask, handle, parameters, &params);
                             });
        }

        isOk(NVSDK_NGX_VULKAN_DestroyParameters(parameters), "DestroyParameters");
    }
} // namespace

int main(int /*argc*/, char** argv)
{
    unsigned instanceCount = 0;
    unsigned deviceCount = 0;
    const char** instanceNames = nullptr;
    const char** deviceNames = nullptr;
    isOk(NVSDK_NGX_VULKAN_RequiredExtensions(&instanceCount, &instanceNames, &deviceCount, &deviceNames),
         "RequiredExtensions");
    const std::vector<const char*> instanceExtensions(instanceNames, instanceNames + instanceCount);
    std::vector<const char*> deviceExtensions;
    for (const char* name : std::span(deviceNames, deviceCount)) {
        // core 1.2 bufferDeviceAddress is enabled; enabling the EXT alongside it is invalid
        if (std::strcmp(name, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) != 0) {
            deviceExtensions.push_back(name);
        }
    }

    test::VulkanContext vulkan;
    const test::ContextStatus status = vulkan.create(instanceExtensions, deviceExtensions, kNvidiaVendorId);
    if (status == test::ContextStatus::Unavailable) {
        return test::kSkipExitCode;
    }
    if (status != test::ContextStatus::Ready) {
        return test::finish();
    }

    // feature dlls are resolved next to the executable
    const std::filesystem::path exeDirectory = std::filesystem::absolute(argv[0]).parent_path();
    if (!isOk(NVSDK_NGX_VULKAN_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion,
                                                   exeDirectory.c_str(), vulkan.instance, vulkan.gpu, vulkan.device),
              "VULKAN_Init_with_ProjectID")) {
        return test::finish();
    }

    NVSDK_NGX_Parameter* capabilities = nullptr;
    if (isOk(NVSDK_NGX_VULKAN_GetCapabilityParameters(&capabilities), "GetCapabilityParameters")) {
        runFeatures(vulkan, capabilities);
        isOk(NVSDK_NGX_VULKAN_DestroyParameters(capabilities), "DestroyParameters (capabilities)");
    }

    isOk(NVSDK_NGX_VULKAN_Shutdown1(vulkan.device), "VULKAN_Shutdown1");
    return test::finish();
}
