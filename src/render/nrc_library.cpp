#include "nrc_library.hpp"

#include "../static_headers/logger.hpp"

#include <format>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace
{
    constexpr wchar_t kNrcDllName[] = L"NRC_Vulkan.dll";

    // names from `llvm-readobj --coff-exports NRC_Vulkan.dll`; recheck on every NRC bump
    struct ExportBinder
    {
        HMODULE module;
        bool isComplete = true;

        template <typename Fn>
        void bind(Fn& slot, const char* mangledName)
        {
            slot = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, mangledName)));
            if (slot == nullptr) {
                logError(std::format("NRC export not found: {}", mangledName), "NRC");
                isComplete = false;
            }
        }
    };
}

NrcLibrary::~NrcLibrary()
{
    if (module != nullptr) {
        FreeLibrary(static_cast<HMODULE>(module));
    }
}

bool NrcLibrary::load(const std::filesystem::path& directory)
{
    if (module != nullptr) {
        return true;
    }

    const std::filesystem::path dllPath = directory / kNrcDllName;
    HMODULE handle = LoadLibraryExW(dllPath.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (handle == nullptr) {
        logError(std::format("Failed to load {} (error {})", dllPath.string(), GetLastError()), "NRC");
        return false;
    }

    NrcApi api{};
    ExportBinder binder{handle};
    binder.bind(api.getVulkanInstanceExtensions, "?GetVulkanInstanceExtensions@vulkan@nrc@@YAIAEAPEBQEBD@Z");
    binder.bind(api.getVulkanDeviceExtensions, "?GetVulkanDeviceExtensions@vulkan@nrc@@YAIAEAPEBQEBD@Z");
    binder.bind(api.getVulkanDeviceFeatures, "?GetVulkanDeviceFeatures@vulkan@nrc@@YAIAEAPEBQEBD@Z");
    binder.bind(api.initialize, "?Initialize@vulkan@nrc@@YA?AW4Status@2@AEBUGlobalSettings@2@@Z");
    binder.bind(api.shutdown, "?Shutdown@vulkan@nrc@@YAXXZ");
    binder.bind(api.computeIdealTrainingDimensions, "?ComputeIdealTrainingDimensions@nrc@@YA?AUnrc_uint2@@AEBU2@IM@Z");
    binder.bind(api.getImGuiResolveModeComboString, "?GetImGuiResolveModeComboString@nrc@@YAPEBDXZ");
    binder.bind(api.createContext,
                "?Create@Context@vulkan@nrc@@SA?AW4Status@3@PEAUVkDevice_T@@PEAUVkPhysicalDevice_T@@"
                "PEAUVkInstance_T@@AEAPEAV123@@Z");
    binder.bind(api.destroyContext, "?Destroy@Context@vulkan@nrc@@SA?AW4Status@3@AEAV123@@Z");
    binder.bind(api.getBuffersAllocationInfo,
                "?GetBuffersAllocationInfo@Context@vulkan@nrc@@SA?AW4Status@3@AEBUContextSettings@3@"
                "AEAUBuffersAllocationInfo@3@@Z");
    binder.bind(api.configure,
                "?Configure@Context@vulkan@nrc@@QEAA?AW4Status@3@AEBUContextSettings@3@PEBUBuffers@23@PEBD@Z");
    binder.bind(api.populateShaderConstants,
                "?PopulateShaderConstants@Context@vulkan@nrc@@QEBA?AW4Status@3@AEAUNrcConstants@@@Z");
    binder.bind(api.beginFrame,
                "?BeginFrame@Context@vulkan@nrc@@QEAA?AW4Status@3@PEAUVkCommandBuffer_T@@AEBUFrameSettings@3@@Z");
    binder.bind(api.queryAndTrain,
                "?QueryAndTrain@Context@vulkan@nrc@@QEAA?AW4Status@3@PEAUVkCommandBuffer_T@@PEAM@Z");
    binder.bind(api.resolve,
                "?Resolve@Context@vulkan@nrc@@QEAA?AW4Status@3@PEAUVkCommandBuffer_T@@PEAUVkImageView_T@@@Z");
    binder.bind(api.train, "?Train@Context@vulkan@nrc@@QEAA?AW4Status@3@PEAUVkCommandBuffer_T@@PEAM@Z");
    binder.bind(api.endFrame, "?EndFrame@Context@vulkan@nrc@@QEAA?AW4Status@3@PEAUVkQueue_T@@@Z");
    binder.bind(api.getBuffers, "?GetBuffers@Context@vulkan@nrc@@QEBAPEBUBuffers@23@XZ");
    binder.bind(api.getDevice, "?GetDevice@Context@vulkan@nrc@@QEBAPEAUVkDevice_T@@XZ");
    binder.bind(api.getBufferUsageFlags, "?GetBufferUsageFlags@Context@vulkan@nrc@@QEBAIAEBUAllocationInfo@3@@Z");

    if (!binder.isComplete) {
        FreeLibrary(handle);
        return false;
    }

    table = api;
    module = handle;
    return true;
}
