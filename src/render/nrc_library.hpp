#pragma once

#include <NrcVk.h>

#include <filesystem>

// NRC_Vulkan.dll exports MSVC-mangled C++ that the Itanium-ABI toolchain cannot link,
// so exports are bound by mangled name. x64 has one calling convention and passes this
// as the first argument, so members are called as free functions taking self.
// holds only while every signature uses PODs, enums, pointers or <= 8-byte struct returns
struct NrcApi
{
    uint32_t (*getVulkanInstanceExtensions)(const char* const*& outStringArray) = nullptr;
    uint32_t (*getVulkanDeviceExtensions)(const char* const*& outStringArray) = nullptr;
    uint32_t (*getVulkanDeviceFeatures)(const char* const*& outStringArray) = nullptr;
    nrc::Status (*initialize)(const nrc::GlobalSettings& settings) = nullptr;
    void (*shutdown)() = nullptr;
    nrc_uint2 (*computeIdealTrainingDimensions)(const nrc_uint2& frameDimensions, uint32_t numTrainingIterations,
                                                float avgTrainingVerticesPerPath) = nullptr;
    const char* (*getImGuiResolveModeComboString)() = nullptr;

    nrc::Status (*createContext)(VkDevice device, VkPhysicalDevice gpu, VkInstance instance,
                                 nrc::vulkan::Context*& outContext) = nullptr;
    nrc::Status (*destroyContext)(nrc::vulkan::Context& context) = nullptr;
    nrc::Status (*getBuffersAllocationInfo)(const nrc::ContextSettings& contextSettings,
                                            nrc::BuffersAllocationInfo& outBuffersAllocationInfo) = nullptr;

    nrc::Status (*configure)(nrc::vulkan::Context* self, const nrc::ContextSettings& contextSettings,
                             const nrc::vulkan::Buffers* buffers, const char* networkConfigFilename) = nullptr;
    nrc::Status (*populateShaderConstants)(const nrc::vulkan::Context* self, NrcConstants& outConstants) = nullptr;
    nrc::Status (*beginFrame)(nrc::vulkan::Context* self, VkCommandBuffer cmd,
                              const nrc::FrameSettings& frameSettings) = nullptr;
    nrc::Status (*queryAndTrain)(nrc::vulkan::Context* self, VkCommandBuffer cmd, float* trainingLossPtr) = nullptr;
    nrc::Status (*resolve)(nrc::vulkan::Context* self, VkCommandBuffer cmd, VkImageView outputBuffer) = nullptr;
    nrc::Status (*train)(nrc::vulkan::Context* self, VkCommandBuffer cmd, float* trainingLossPtr) = nullptr;
    nrc::Status (*endFrame)(nrc::vulkan::Context* self, VkQueue queue) = nullptr;
    const nrc::vulkan::Buffers* (*getBuffers)(const nrc::vulkan::Context* self) = nullptr;
    VkDevice (*getDevice)(const nrc::vulkan::Context* self) = nullptr;
    VkBufferUsageFlags (*getBufferUsageFlags)(const nrc::vulkan::Context* self,
                                              const nrc::AllocationInfo& allocationInfo) = nullptr;
};

// owns the NRC_Vulkan.dll module; call api().shutdown() before destruction
class NrcLibrary
{
public:
    NrcLibrary() = default;
    ~NrcLibrary();

    NrcLibrary(const NrcLibrary&) = delete;
    NrcLibrary& operator=(const NrcLibrary&) = delete;
    NrcLibrary(NrcLibrary&&) = delete;
    NrcLibrary& operator=(NrcLibrary&&) = delete;

    // false if the dll is missing or any export is unresolved; api() stays empty then
    [[nodiscard]] bool load(const std::filesystem::path& directory);

    [[nodiscard]] bool isLoaded() const { return module != nullptr; }
    [[nodiscard]] const NrcApi& api() const { return table; }

private:
    NrcApi table{};
    void* module = nullptr; // HMODULE; keeps windows.h out of the header
};
