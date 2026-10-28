// Smoke test for NrcLibrary: binds every NRC_Vulkan.dll export and drives one
// context through configure + an empty frame, so a mangled-name or ABI mismatch
// after an NRC bump fails here instead of inside the renderer.
#include "render/nrc_library.hpp"
#include "test_support.hpp"

#include <cstdio>
#include <filesystem>
#include <vector>

using test::check;

namespace
{
    constexpr uint32_t kNvidiaVendorId = 0x10DE;
    constexpr nrc_uint2 kFrameDimensions{1920, 1080};

    bool isOk(nrc::Status status, const char* what)
    {
        const bool ok = status == nrc::Status::OK;
        if (!ok) {
            std::printf("  status %d\n", static_cast<int>(status));
        }
        return check(ok, what);
    }

    void logNrc(const char* message, nrc::LogLevel level)
    {
        std::printf("  nrc[%d] %s\n", static_cast<int>(level), message);
    }

    std::vector<const char*> toList(uint32_t (*query)(const char* const*&))
    {
        const char* const* names = nullptr;
        const uint32_t count = query(names);
        return {names, names + count};
    }

    void runContext(const NrcApi& api, const test::VulkanContext& vulkan)
    {
        nrc::vulkan::Context* context = nullptr;
        if (!isOk(api.createContext(vulkan.device, vulkan.gpu, vulkan.instance, context), "Context::Create") ||
            context == nullptr) {
            return;
        }
        check(api.getDevice(context) == vulkan.device, "Context::GetDevice returns the creating device");

        nrc::ContextSettings settings{};
        settings.sceneBoundsMin = {-10.0f, -10.0f, -10.0f};
        settings.sceneBoundsMax = {10.0f, 10.0f, 10.0f};
        settings.frameDimensions = kFrameDimensions;
        settings.trainingDimensions = api.computeIdealTrainingDimensions(kFrameDimensions, 0, 0.0f);

        nrc::BuffersAllocationInfo allocationInfo{};
        isOk(api.getBuffersAllocationInfo(settings, allocationInfo), "Context::GetBuffersAllocationInfo");
        check(allocationInfo[nrc::BufferIdx::QueryRadiance].elementCount > 0, "QueryRadiance has elements");
        check(api.getBufferUsageFlags(context, allocationInfo[nrc::BufferIdx::Counter]) != 0,
              "Context::GetBufferUsageFlags is non-zero");

        if (isOk(api.configure(context, settings, nullptr, nullptr), "Context::Configure")) {
            const nrc::vulkan::Buffers* buffers = api.getBuffers(context);
            check(buffers != nullptr && (*buffers)[nrc::BufferIdx::Counter].resource != VK_NULL_HANDLE,
                  "Context::GetBuffers returns allocated buffers");

            NrcConstants constants{};
            isOk(api.populateShaderConstants(context, constants), "Context::PopulateShaderConstants");
            check(constants.frameDimensions.x == kFrameDimensions.x &&
                      constants.frameDimensions.y == kFrameDimensions.y,
                  "NrcConstants.frameDimensions round-trips");

            VkCommandBuffer cmd = vulkan.beginCommands();
            isOk(api.beginFrame(context, cmd, nrc::FrameSettings{}), "Context::BeginFrame");
            isOk(api.queryAndTrain(context, cmd, nullptr), "Context::QueryAndTrain");
            check(vulkan.submit(cmd), "frame submitted");
            isOk(api.endFrame(context, vulkan.queue), "Context::EndFrame");
            vulkan.waitIdle();
        }

        isOk(api.destroyContext(*context), "Context::Destroy");
    }
}

int main(int /*argc*/, char** argv)
{
    NrcLibrary library;
    check(library.load(std::filesystem::absolute(argv[0]).parent_path()), "NRC_Vulkan.dll loaded, all exports bound");
    if (!library.isLoaded()) {
        return test::finish();
    }
    const NrcApi& api = library.api();

    const nrc_uint2 training = api.computeIdealTrainingDimensions(kFrameDimensions, 0, 0.0f);
    std::printf("  ideal training dimensions %ux%u\n", training.x, training.y);
    check(training.x > 0 && training.y > 0 && training.x <= kFrameDimensions.x && training.y <= kFrameDimensions.y,
          "ComputeIdealTrainingDimensions returns nrc_uint2 by value");
    check(api.getImGuiResolveModeComboString()[0] != '\0', "GetImGuiResolveModeComboString");

    const std::vector<const char*> instanceExtensions = toList(api.getVulkanInstanceExtensions);
    const std::vector<const char*> deviceExtensions = toList(api.getVulkanDeviceExtensions);
    for (const char* name : toList(api.getVulkanDeviceFeatures)) {
        std::printf("  device feature %s\n", name);
    }

    // NRC lists features by name only; the context enables every supported core feature
    test::VulkanContext vulkan;
    const test::ContextStatus status = vulkan.create(instanceExtensions, deviceExtensions, kNvidiaVendorId);
    if (status == test::ContextStatus::Unavailable) {
        return test::kSkipExitCode;
    }

    nrc::GlobalSettings globalSettings{};
    globalSettings.loggerFn = logNrc;
    // a GlobalSettings layout mismatch surfaces here as SDKVersionMismatch
    if (status == test::ContextStatus::Ready && isOk(api.initialize(globalSettings), "Initialize")) {
        runContext(api, vulkan);
        api.shutdown();
    }
    return test::finish();
}
