#pragma once
#include "types.hpp"

// Vulkan device stack: instance, device, queues, and surface
class Device
{
    void createInstance();
    void setupDebugMessenger();
    void createSurface();
    void pickPhysicalDevice();

    // resolve queue family indices with dedicated fallbacks
    void findQueueFamilies(const std::vector<vk::QueueFamilyProperties2>& queueFamilyProperties2);

    void createLogicalDevice();

    // max supported msaa sample count across color/depth
    vk::SampleCountFlagBits getMaxUsableSampleCount();

public:
    Device(SDL_Window* window, bool enableValidationLayers = false);
    ~Device() = default;

    void init();

    // -------------------------------------------------------------------------
    // Stable handles / indices — direct access (set during init, not mutated)
    // -------------------------------------------------------------------------

    // required device extensions
    std::vector<const char*> requiredDeviceExtension = {
        // KHR
        vk::KHRSwapchainExtensionName, vk::KHRMaintenance7ExtensionName, vk::KHRMaintenance8ExtensionName,
        vk::KHRMaintenance9ExtensionName, vk::KHRMaintenance10ExtensionName,
        vk::KHRDeferredHostOperationsExtensionName, // required by KHR_acceleration_structure
        vk::KHRAccelerationStructureExtensionName, vk::KHRRayTracingPipelineExtensionName,
        // vk::KHRPipelineBinaryExtensionName,
        vk::KHRFragmentShadingRateExtensionName, vk::KHRRayQueryExtensionName,
        vk::KHRSwapchainMaintenance1ExtensionName, vk::KHRRayTracingMaintenance1ExtensionName,
        vk::KHRPresentId2ExtensionName, // required by EXT_present_timing
        vk::KHRCalibratedTimestampsExtensionName, // required by EXT_present_timing
        // vk::KHRPipelineLibraryExtensionName,       // required by EXT_graphics_pipeline_library
        vk::KHRPresentModeFifoLatestReadyExtensionName, vk::KHRCopyMemoryIndirectExtensionName,
        vk::KHRShaderUntypedPointersExtensionName, vk::KHRDeviceAddressCommandsExtensionName,
        // EXT
        vk::EXTOpacityMicromapExtensionName, vk::EXTMemoryBudgetExtensionName, vk::EXTMemoryPriorityExtensionName,
        vk::EXTMemoryDecompressionExtensionName, vk::EXTDescriptorHeapExtensionName,
        vk::EXTBlendOperationAdvancedExtensionName, vk::EXTMeshShaderExtensionName,
        vk::EXTDeviceGeneratedCommandsExtensionName, vk::EXTPageableDeviceLocalMemoryExtensionName,
        vk::EXTShaderObjectExtensionName,
        // vk::EXTGraphicsPipelineLibraryExtensionName,
        vk::EXTPresentTimingExtensionName, vk::EXTRayTracingInvocationReorderExtensionName,
        vk::EXTExtendedDynamicState3ExtensionName, vk::NVClusterAccelerationStructureExtensionName,
        vk::NVPartitionedAccelerationStructureExtensionName};

    SDL_Window* window = nullptr;
    bool enableValidationLayers = false;

    vk::raii::Context context;
    vk::raii::Instance instance = nullptr;
    vk::raii::DebugUtilsMessengerEXT debugMessenger = nullptr;
    vk::raii::PhysicalDevice physicalDevice = nullptr;
    vk::raii::SurfaceKHR surface = nullptr;
    vk::raii::Device vkdevice = nullptr;
    vk::PhysicalDeviceFeatures deviceFeatures;
    vk::raii::Queue graphicsQueue = nullptr;
    vk::raii::Queue presentQueue = nullptr;
    vk::raii::Queue transferQueue = nullptr;
    vk::raii::Queue computeQueue = nullptr;

    std::vector<uint32_t> queueFamilyIndices;
    vk::SampleCountFlagBits msaaSamples = vk::SampleCountFlagBits::e1;

    uint32_t transferIndex = 0;
    uint32_t computeIndex = 0;
    uint32_t graphicsIndex = 0;
    uint32_t presentIndex = 0;

    HardwareCapabilities capabilities = HardwareCapabilities{};
    DescriptorBindingMode descriptorBindingMode = DescriptorBindingMode::LegacySets;
};
