/**
 * @file vk_device.cpp
 * @brief Vulkan device management: instance, physical-device selection, logical device, and queue setup.
 *
 * This translation unit implements the full Vulkan device initialisation pipeline:
 *   - Vulkan instance creation with SDL3 WSI extensions and optional validation
 *   - VK_EXT_debug_utils messenger registration for validation layer output
 *   - Window-surface creation via SDL3
 *   - Physical-device selection: required extensions/features, then scoring
 *   - Logical-device creation with an exhaustive feature chain (Vulkan 1.1–1.4 +
 *     ray-tracing, mesh shaders, shader objects, present-timing, etc.)
 *   - Queue family resolution with fallback strategies for transfer and compute
 */
#include "vk_device.hpp"
#include <array>
#include <bit>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include "../static_headers/logger.hpp"
#include "../util/debug.hpp"
#include "util/vk_tracy.hpp"
#include "vulkan/vulkan.hpp"

namespace
{
// Requested when enableValidationLayers is true.
constexpr std::array kValidationLayers = {"VK_LAYER_KHRONOS_validation"};

// Promoted-to-core extensions are enabled through Vulkan1xFeatures and must not be listed here,
// see docs/vulkan_extensions_reference.md "Promoted-to-Core Features".
// Optional extensions (NV) are appended in createLogicalDevice when supported.
constexpr std::array kRequiredDeviceExtensions = {
    // KHR
    vk::KHRSwapchainExtensionName,
    vk::KHRMaintenance7ExtensionName,
    vk::KHRMaintenance8ExtensionName,
    vk::KHRMaintenance9ExtensionName,
    vk::KHRMaintenance10ExtensionName,
    vk::KHRDeferredHostOperationsExtensionName, // required by KHR_acceleration_structure
    vk::KHRAccelerationStructureExtensionName,
    vk::KHRRayTracingPipelineExtensionName,
    // vk::KHRPipelineBinaryExtensionName,
    vk::KHRFragmentShadingRateExtensionName,
    vk::KHRRayQueryExtensionName,
    vk::KHRSwapchainMaintenance1ExtensionName,
    vk::KHRRayTracingMaintenance1ExtensionName,
    vk::KHRPresentId2ExtensionName, // required by EXT_present_timing
    vk::KHRCalibratedTimestampsExtensionName, // required by EXT_present_timing
    // vk::KHRPipelineLibraryExtensionName,       // required by EXT_graphics_pipeline_library
    vk::KHRPresentModeFifoLatestReadyExtensionName,
    vk::KHRCopyMemoryIndirectExtensionName,
    vk::KHRShaderUntypedPointersExtensionName,
    vk::KHRDeviceAddressCommandsExtensionName,
    // EXT
    vk::EXTOpacityMicromapExtensionName,
    vk::EXTMemoryBudgetExtensionName,
    vk::EXTMemoryPriorityExtensionName,
    vk::EXTMemoryDecompressionExtensionName,
    vk::EXTDescriptorHeapExtensionName,
    vk::EXTBlendOperationAdvancedExtensionName,
    vk::EXTMeshShaderExtensionName,
    vk::EXTDeviceGeneratedCommandsExtensionName,
    vk::EXTPageableDeviceLocalMemoryExtensionName,
    vk::EXTShaderObjectExtensionName,
    // vk::EXTGraphicsPipelineLibraryExtensionName,
    vk::EXTPresentTimingExtensionName,
    vk::EXTRayTracingInvocationReorderExtensionName,
    vk::EXTExtendedDynamicState3ExtensionName,
};

using QueueFamilyChain = vk::StructureChain<vk::QueueFamilyProperties2, vk::QueueFamilyOwnershipTransferPropertiesKHR>;

[[nodiscard]] bool isExtensionSupported(const std::vector<vk::ExtensionProperties>& available, std::string_view name)
{
    return std::ranges::any_of(available, [name](const vk::ExtensionProperties& extension)
                               { return name == extension.extensionName.data(); });
}

// Comma-separated list of unmet requirements; empty when the GPU is usable.
// Other feature bits are still enforced by vkCreateDevice.
[[nodiscard]] std::string missingRequirements(const vk::raii::PhysicalDevice& device)
{
    std::string missing;
    const auto append = [&missing](std::string_view requirement)
    { missing += std::format("{}{}", missing.empty() ? "" : ", ", requirement); };

    const std::vector<vk::ExtensionProperties> available = device.enumerateDeviceExtensionProperties();
    for (const char* extension : kRequiredDeviceExtensions) {
        if (!isExtensionSupported(available, extension)) {
            append(extension);
        }
    }
    if (!device.getFeatures2().features.geometryShader) {
        append("geometryShader");
    }
    // the descriptor heap feature struct may only be queried when its extension exists
    if (isExtensionSupported(available, vk::EXTDescriptorHeapExtensionName) &&
        !device.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceDescriptorHeapFeaturesEXT>()
             .get<vk::PhysicalDeviceDescriptorHeapFeaturesEXT>()
             .descriptorHeap) {
        append("descriptorHeap");
    }
    return missing;
}

// First family accepted by the predicate (index, flags).
template <typename Predicate>
[[nodiscard]] std::optional<uint32_t> findFamilyIf(std::span<const vk::QueueFamilyProperties> families,
                                                   Predicate accept)
{
    for (uint32_t family = 0; family < families.size(); ++family) {
        if (accept(family, families[family].queueFlags)) {
            return family;
        }
    }
    return std::nullopt;
}

// First family with all required flags and none of the excluded ones.
[[nodiscard]] std::optional<uint32_t> findFamily(std::span<const vk::QueueFamilyProperties> families,
                                                 vk::QueueFlags required, vk::QueueFlags excluded = {})
{
    return findFamilyIf(families, [=](uint32_t, vk::QueueFlags flags)
                        { return (flags & required) == required && !(flags & excluded); });
}

void logQueueFamilies(std::span<const QueueFamilyChain> chains)
{
    for (uint32_t family = 0; family < chains.size(); ++family) {
        const vk::QueueFamilyProperties& properties =
            chains[family].get<vk::QueueFamilyProperties2>().queueFamilyProperties;
        const uint32_t transferTargets =
            chains[family].get<vk::QueueFamilyOwnershipTransferPropertiesKHR>().optimalImageTransferToQueueFamilies;
        std::string targets;
        for (uint32_t target = 0; target < 32; ++target) {
            if (transferTargets & (1u << target)) {
                targets += std::format("{}{}", targets.empty() ? "" : ",", target);
            }
        }
        log_info(std::format("Queue family {}: count={} flags={} optimalImageTransferTo=[{}]", family,
                             properties.queueCount, vk::to_string(properties.queueFlags), targets),
                 "Device");
    }
}
} // namespace


Device::Device(SDL_Window* window, bool enableValidationLayers) :
    window(window), enableValidationLayers(enableValidationLayers)
{
}

void Device::init()
{
    ZoneScopedN("Device::init");
    createInstance();
    setupDebugMessenger();
    createSurface();
    pickPhysicalDevice();
    createLogicalDevice();
}

void Device::createInstance()
{
    ZoneScopedN("Device::createInstance");

    // Describe the application to the Vulkan loader.  The version fields are
    // informational; apiVersion selects the highest Vulkan API revision to use.
    constexpr vk::ApplicationInfo appInfo{.pApplicationName = "Hello Triangle",
                                          .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
                                          .pEngineName = "No Engine",
                                          .engineVersion = VK_MAKE_VERSION(1, 0, 0),
                                          .apiVersion = vk::ApiVersion14};

    // WSI extensions required by SDL3 (e.g. VK_KHR_surface, VK_KHR_win32_surface)
    Uint32 sdlExtensionCount = 0;
    const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);

    // fail early with the missing name instead of a generic instance-creation error
    const std::vector<vk::ExtensionProperties> extensionProperties = context.enumerateInstanceExtensionProperties();
    for (uint32_t i = 0; i < sdlExtensionCount; ++i) {
        if (!isExtensionSupported(extensionProperties, sdlExtensions[i])) {
            throw std::runtime_error("Required SDL3 extension not supported: " + std::string(sdlExtensions[i]));
        }
    }

    std::vector<const char*> extensions(sdlExtensions, sdlExtensions + sdlExtensionCount);
    extensions.insert(extensions.end(),
                      {
                          // always on, not only with validation: object names and labels for RenderDoc / Nsight
                          vk::EXTDebugUtilsExtensionName,
                          // instance side of KHR_swapchain_maintenance1
                          vk::KHRDisplayExtensionName,
                          vk::KHRSurfaceMaintenance1ExtensionName,
                          vk::KHRGetDisplayProperties2ExtensionName,
                          vk::KHRGetSurfaceCapabilities2ExtensionName,
                      });
    std::string enabledNames;
    for (const char* extension : extensions) {
        enabledNames += std::format("{}{}", enabledNames.empty() ? "" : ", ", extension);
    }
    log_info(std::format("Enabled instance extensions: {}", enabledNames), "Device");

    // Conditionally request the Khronos validation layer.  Enumerate available layers
    // first and bail out immediately if any requested layer is not present, rather than
    // letting the driver produce a cryptic error later.
    std::vector<char const*> requiredLayers;
    if (enableValidationLayers) {
        requiredLayers.assign(kValidationLayers.begin(), kValidationLayers.end());
    }
    auto layerProperties = context.enumerateInstanceLayerProperties();
    if (std::ranges::any_of(requiredLayers,
                            [&layerProperties](auto const& requiredLayer)
                            {
                                return std::ranges::none_of(
                                    layerProperties, [requiredLayer](auto const& layerProperty)
                                    { return strcmp(layerProperty.layerName, requiredLayer) == 0; });
                            })) {
        throw std::runtime_error("One or more required layers are not supported!");
    }


    vk::InstanceCreateInfo createInfo{.pApplicationInfo = &appInfo,
                                      .enabledLayerCount = static_cast<uint32_t>(requiredLayers.size()),
                                      .ppEnabledLayerNames = requiredLayers.data(),
                                      .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
                                      .ppEnabledExtensionNames = extensions.data()};

    // Attempt to create the instance; a SystemError here usually means a driver
    // version mismatch or a missing extension, so surface the original message.
    try {
        instance = vk::raii::Instance(context, createInfo);
    } catch (const vk::SystemError& err) {
        throw std::runtime_error("Failed to create Vulkan instance: " + std::string(err.what()));
    }
}

/**
 * @brief VK_EXT_debug_utils message callback.
 *
 * Prints all validation/performance/general messages from enabled layers to stderr.
 * Returning VK_FALSE tells the driver that the Vulkan call that triggered the
 * message should NOT be aborted (returning VK_TRUE would only be appropriate for
 * testing layer behaviour itself).
 *
 * @param severity  Severity bitmask (verbose / info / warning / error).
 * @param type      Message category (general / validation / performance).
 * @param pCallbackData  Structured message payload including pMessage string.
 * @param           User data pointer; unused, kept for ABI conformance.
 * @return vk::False – do not abort the triggering Vulkan call.
 */
static VKAPI_ATTR vk::Bool32 VKAPI_CALL debugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT severity,
                                                      vk::DebugUtilsMessageTypeFlagsEXT type,
                                                      const vk::DebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                      void*)
{
    std::cerr << "validation layer: type " << to_string(type) << " msg: " << pCallbackData->pMessage << std::endl;
    return vk::False;
}


void Device::setupDebugMessenger()
{
    ZoneScopedN("Device::setupDebugMessenger");
    if (!enableValidationLayers)
        return;

    // Subscribe to all severity levels and all message categories so that nothing
    // from the validation layers is silently discarded during development.
    vk::DebugUtilsMessageSeverityFlagsEXT severityFlags(vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose |
                                                        vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                                                        vk::DebugUtilsMessageSeverityFlagBitsEXT::eError);
    vk::DebugUtilsMessageTypeFlagsEXT messageTypeFlags(vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                                                       vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance |
                                                       vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation);
    vk::DebugUtilsMessengerCreateInfoEXT debugUtilsMessengerCreateInfoEXT{
        .messageSeverity = severityFlags, .messageType = messageTypeFlags, .pfnUserCallback = &debugCallback};
    debugMessenger = instance.createDebugUtilsMessengerEXT(debugUtilsMessengerCreateInfoEXT);
}


void Device::createSurface()
{
    ZoneScopedN("Device::createSurface");
    VkSurfaceKHR _surface;
    if (!SDL_Vulkan_CreateSurface(window, *instance, nullptr, &_surface)) {
        throw std::runtime_error("Failed to create surface");
    }
    surface = vk::raii::SurfaceKHR(instance, _surface);
}

vk::SampleCountFlagBits Device::getMaxUsableSampleCount() const
{
    const vk::PhysicalDeviceLimits& limits = capabilities.properties2.properties.limits;
    // only counts usable by both attachment types; e1 is always set, so bit_floor is never 0
    const vk::SampleCountFlags counts = limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts;
    return static_cast<vk::SampleCountFlagBits>(std::bit_floor(static_cast<uint32_t>(counts)));
}


void Device::pickPhysicalDevice()
{
    ZoneScopedN("Device::pickPhysicalDevice");
    auto devices = instance.enumeratePhysicalDevices();

    std::optional<size_t> bestIndex;
    uint32_t bestScore = 0;
    for (size_t i = 0; i < devices.size(); ++i) {
        const vk::PhysicalDeviceProperties properties = devices[i].getProperties2().properties;
        const std::string_view name = properties.deviceName.data();
        if (const std::string missing = missingRequirements(devices[i]); !missing.empty()) {
            log_info(std::format("Skipping GPU {}: missing {}", name, missing), "Device");
            continue;
        }
        // discrete first; max texture size as a rough capability tiebreak
        const uint32_t score = (properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu ? 1000u : 0u) +
            properties.limits.maxImageDimension2D;
        if (!bestIndex || score > bestScore) {
            bestIndex = i;
            bestScore = score;
        }
    }
    if (!bestIndex) {
        throw std::runtime_error("No GPU supports the required Vulkan extensions and features (see log)");
    }
    physicalDevice = devices[*bestIndex];
    log_info(std::format("Using physical device: {}",
                         std::string_view(physicalDevice.getProperties2().properties.deviceName.data())),
             "Device");
}

void Device::findQueueFamilies(std::span<const vk::QueueFamilyProperties> families)
{
    const auto supportsPresent = [this](uint32_t family)
    { return physicalDevice.getSurfaceSupportKHR(family, *surface) == vk::True; };

    // one family for graphics + present avoids an ownership transfer before present
    const std::optional<uint32_t> graphicsPresent =
        findFamilyIf(families, [&](uint32_t family, vk::QueueFlags flags)
                     { return (flags & vk::QueueFlagBits::eGraphics) && supportsPresent(family); });
    const std::optional<uint32_t> graphics =
        graphicsPresent ? graphicsPresent : findFamily(families, vk::QueueFlagBits::eGraphics);
    const std::optional<uint32_t> present =
        graphicsPresent ? graphicsPresent
                        : findFamilyIf(families, [&](uint32_t family, vk::QueueFlags) { return supportsPresent(family); });
    if (!graphics || !present) {
        throw std::runtime_error("Could not find a queue family for graphics or present");
    }
    graphicsIndex = *graphics;
    presentIndex = *present;

    // Dedicated (non-graphics) families let uploads and async compute overlap the frame.
    // Only a missing one falls back: shared non-graphics transfer+compute, any transfer+compute, graphics.
    const vk::QueueFlags transferCompute = vk::QueueFlagBits::eTransfer | vk::QueueFlagBits::eCompute;
    const uint32_t shared = findFamily(families, transferCompute, vk::QueueFlagBits::eGraphics)
                                .or_else([&] { return findFamily(families, transferCompute); })
                                .value_or(graphicsIndex);
    transferIndex = findFamily(families, vk::QueueFlagBits::eTransfer, vk::QueueFlagBits::eGraphics).value_or(shared);
    computeIndex = findFamily(families, vk::QueueFlagBits::eCompute, vk::QueueFlagBits::eGraphics).value_or(shared);
}

void Device::queryCapabilities()
{
    ZoneScopedN("Device::queryCapabilities");
    const std::vector<vk::ExtensionProperties> availableExtensions =
        physicalDevice.enumerateDeviceExtensionProperties();
    capabilities.hasClusterAccelerationStructure =
        isExtensionSupported(availableExtensions, vk::NVClusterAccelerationStructureExtensionName);
    capabilities.hasPartitionedAccelerationStructure =
        isExtensionSupported(availableExtensions, vk::NVPartitionedAccelerationStructureExtensionName);
    log_info(std::format("Optional NV extensions: clusterAS={} partitionedAS={}",
                         capabilities.hasClusterAccelerationStructure,
                         capabilities.hasPartitionedAccelerationStructure),
             "Device");

    vk::StructureChain<
        vk::PhysicalDeviceProperties2, vk::PhysicalDeviceVulkan11Properties, vk::PhysicalDeviceVulkan12Properties,
        vk::PhysicalDeviceVulkan13Properties,
        vk::PhysicalDeviceVulkan14Properties, // after that comes things that may or may not be ext or promoted to khr
        vk::PhysicalDeviceBlendOperationAdvancedPropertiesEXT, vk::PhysicalDeviceDescriptorHeapPropertiesEXT,
        vk::PhysicalDeviceDescriptorIndexingPropertiesEXT, // Maybe not ext
        vk::PhysicalDeviceMeshShaderPropertiesEXT, vk::PhysicalDeviceDeviceGeneratedCommandsPropertiesEXT,
        vk::PhysicalDeviceMemoryDecompressionPropertiesEXT,
        vk::PhysicalDeviceHostImageCopyPropertiesEXT, // Maybe not ext
        vk::PhysicalDeviceTexelBufferAlignmentPropertiesEXT, // Maybe not ext + after that comes only khr when baseline
                                                             // 2060
        vk::PhysicalDeviceFragmentShadingRatePropertiesKHR,
        vk::PhysicalDeviceAccelerationStructurePropertiesKHR, vk::PhysicalDeviceOpacityMicromapPropertiesEXT,
        vk::PhysicalDeviceDepthStencilResolveProperties, vk::PhysicalDeviceDriverProperties,
        vk::PhysicalDeviceMaintenance3Properties, vk::PhysicalDeviceMaintenance4Properties,
        vk::PhysicalDeviceMaintenance5Properties, vk::PhysicalDeviceMaintenance6Properties,
        vk::PhysicalDeviceMaintenance7PropertiesKHR, vk::PhysicalDeviceMaintenance9PropertiesKHR,
        vk::PhysicalDeviceMaintenance10PropertiesKHR, vk::PhysicalDevicePipelineBinaryPropertiesKHR,
        vk::PhysicalDeviceRayTracingPipelinePropertiesKHR,
        vk::PhysicalDevicePartitionedAccelerationStructurePropertiesNV,
        vk::PhysicalDeviceClusterAccelerationStructurePropertiesNV>
        propertiesChain;
    // structs of unsupported extensions must not be in the query chain
    if (!capabilities.hasPartitionedAccelerationStructure) {
        propertiesChain.unlink<vk::PhysicalDevicePartitionedAccelerationStructurePropertiesNV>();
    }
    if (!capabilities.hasClusterAccelerationStructure) {
        propertiesChain.unlink<vk::PhysicalDeviceClusterAccelerationStructurePropertiesNV>();
    }
    physicalDevice.getProperties2(&propertiesChain.get<vk::PhysicalDeviceProperties2>());
    capabilities.properties2 = propertiesChain.get<vk::PhysicalDeviceProperties2>();
    capabilities.vulkan11 = propertiesChain.get<vk::PhysicalDeviceVulkan11Properties>();
    capabilities.vulkan12 = propertiesChain.get<vk::PhysicalDeviceVulkan12Properties>();
    capabilities.vulkan13 = propertiesChain.get<vk::PhysicalDeviceVulkan13Properties>();
    capabilities.vulkan14 = propertiesChain.get<vk::PhysicalDeviceVulkan14Properties>();
    capabilities.blendOperationAdvanced = propertiesChain.get<vk::PhysicalDeviceBlendOperationAdvancedPropertiesEXT>();
    capabilities.descriptorHeap = propertiesChain.get<vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
    capabilities.descriptorIndexing = propertiesChain.get<vk::PhysicalDeviceDescriptorIndexingPropertiesEXT>();
    capabilities.meshShader = propertiesChain.get<vk::PhysicalDeviceMeshShaderPropertiesEXT>();
    capabilities.deviceGeneratedCommands =
        propertiesChain.get<vk::PhysicalDeviceDeviceGeneratedCommandsPropertiesEXT>();
    {
        const auto& dgc = capabilities.deviceGeneratedCommands;
        log_info(std::format("DGC limits: maxSeq={} maxTokens={} maxTokenOffset={} maxStride={} "
                             "maxPipelines={} maxShaderObjects={} stages={:#x} pipelineBind={:#x} "
                             "shaderBind={:#x} xfb={} multiDrawCount={}",
                             dgc.maxIndirectSequenceCount, dgc.maxIndirectCommandsTokenCount,
                             dgc.maxIndirectCommandsTokenOffset, dgc.maxIndirectCommandsIndirectStride,
                             dgc.maxIndirectPipelineCount, dgc.maxIndirectShaderObjectCount,
                             static_cast<uint32_t>(dgc.supportedIndirectCommandsShaderStages),
                             static_cast<uint32_t>(dgc.supportedIndirectCommandsShaderStagesPipelineBinding),
                             static_cast<uint32_t>(dgc.supportedIndirectCommandsShaderStagesShaderBinding),
                             static_cast<bool>(dgc.deviceGeneratedCommandsTransformFeedback),
                             static_cast<bool>(dgc.deviceGeneratedCommandsMultiDrawIndirectCount)),
                 "Device");
    }
    capabilities.memoryDecompression = propertiesChain.get<vk::PhysicalDeviceMemoryDecompressionPropertiesEXT>();
    capabilities.hostImageCopy = propertiesChain.get<vk::PhysicalDeviceHostImageCopyPropertiesEXT>();
    {
        auto& hic = capabilities.hostImageCopy;
        capabilities.hostImageCopySrcLayouts.resize(hic.copySrcLayoutCount);
        capabilities.hostImageCopyDstLayouts.resize(hic.copyDstLayoutCount);

        vk::PhysicalDeviceHostImageCopyProperties layoutProps{
            .copySrcLayoutCount = hic.copySrcLayoutCount,
            .pCopySrcLayouts = capabilities.hostImageCopySrcLayouts.data(),
            .copyDstLayoutCount = hic.copyDstLayoutCount,
            .pCopyDstLayouts = capabilities.hostImageCopyDstLayouts.data(),
        };
        vk::PhysicalDeviceProperties2 props2{.pNext = &layoutProps};
        physicalDevice.getProperties2(&props2);

        hic.copySrcLayoutCount = layoutProps.copySrcLayoutCount;
        hic.copyDstLayoutCount = layoutProps.copyDstLayoutCount;
        hic.pCopySrcLayouts = capabilities.hostImageCopySrcLayouts.data();
        hic.pCopyDstLayouts = capabilities.hostImageCopyDstLayouts.data();

        const bool hasGeneralSrc =
            std::ranges::find(capabilities.hostImageCopySrcLayouts, vk::ImageLayout::eGeneral) !=
            capabilities.hostImageCopySrcLayouts.end();
        const bool hasGeneralDst =
            std::ranges::find(capabilities.hostImageCopyDstLayouts, vk::ImageLayout::eGeneral) !=
            capabilities.hostImageCopyDstLayouts.end();

        std::string uuidHex;
        uuidHex.reserve(VK_UUID_SIZE * 2);
        for (const uint8_t byte : hic.optimalTilingLayoutUUID) {
            uuidHex += std::format("{:02x}", byte);
        }
        log_info(std::format("hostImageCopy: srcLayouts={} dstLayouts={} generalSrc={} generalDst={} "
                             "identicalMemTypes={} uuid={}",
                             hic.copySrcLayoutCount, hic.copyDstLayoutCount, hasGeneralSrc, hasGeneralDst,
                             static_cast<bool>(hic.identicalMemoryTypeRequirements), uuidHex),
                 "Device");
    }
    capabilities.texelBufferAlignment = propertiesChain.get<vk::PhysicalDeviceTexelBufferAlignmentPropertiesEXT>();
    capabilities.fragmentShadingRate = propertiesChain.get<vk::PhysicalDeviceFragmentShadingRatePropertiesKHR>();
    capabilities.accelerationStructure = propertiesChain.get<vk::PhysicalDeviceAccelerationStructurePropertiesKHR>();
    capabilities.depthStencilResolve = propertiesChain.get<vk::PhysicalDeviceDepthStencilResolveProperties>();
    capabilities.driverProperties = propertiesChain.get<vk::PhysicalDeviceDriverProperties>();
    capabilities.maintenance3 = propertiesChain.get<vk::PhysicalDeviceMaintenance3Properties>();
    capabilities.maintenance4 = propertiesChain.get<vk::PhysicalDeviceMaintenance4Properties>();
    capabilities.maintenance5 = propertiesChain.get<vk::PhysicalDeviceMaintenance5Properties>();
    {
        const auto& m5 = capabilities.maintenance5;
        log_info(std::format("maintenance5: earlyCoverageAfterCount={} earlySampleMaskBeforeCount={} "
                             "depthStencilSwizzleOne={} polygonModePointSize={} "
                             "nonStrict1pxParallelogram={} nonStrictWideParallelogram={}",
                             static_cast<bool>(m5.earlyFragmentMultisampleCoverageAfterSampleCounting),
                             static_cast<bool>(m5.earlyFragmentSampleMaskTestBeforeSampleCounting),
                             static_cast<bool>(m5.depthStencilSwizzleOneSupport),
                             static_cast<bool>(m5.polygonModePointSize),
                             static_cast<bool>(m5.nonStrictSinglePixelWideLinesUseParallelogram),
                             static_cast<bool>(m5.nonStrictWideLinesUseParallelogram)),
                 "Device");
    }
    capabilities.maintenance6 = propertiesChain.get<vk::PhysicalDeviceMaintenance6Properties>();
    capabilities.maintenance7 = propertiesChain.get<vk::PhysicalDeviceMaintenance7PropertiesKHR>();
    capabilities.maintenance9 = propertiesChain.get<vk::PhysicalDeviceMaintenance9PropertiesKHR>();
    capabilities.maintenance10 = propertiesChain.get<vk::PhysicalDeviceMaintenance10PropertiesKHR>();
    capabilities.pipelineBinary = propertiesChain.get<vk::PhysicalDevicePipelineBinaryPropertiesKHR>();
    capabilities.rayTracingPipeline = propertiesChain.get<vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();
    capabilities.partitionedAccelerationStructure =
        propertiesChain.get<vk::PhysicalDevicePartitionedAccelerationStructurePropertiesNV>();
    capabilities.clusterAccelerationStructure =
        propertiesChain.get<vk::PhysicalDeviceClusterAccelerationStructurePropertiesNV>();
}

void Device::createLogicalDevice()
{
    ZoneScopedN("Device::createLogicalDevice");
    const std::vector<QueueFamilyChain> queueFamilyChains =
        physicalDevice.getQueueFamilyProperties2<QueueFamilyChain>();
    logQueueFamilies(queueFamilyChains);
    std::vector<vk::QueueFamilyProperties> families;
    families.reserve(queueFamilyChains.size());
    for (const QueueFamilyChain& chain : queueFamilyChains) {
        families.push_back(chain.get<vk::QueueFamilyProperties2>().queueFamilyProperties);
    }
    findQueueFamilies(families);

    queryCapabilities();

    // Build a pNext feature chain covering every extension the engine depends on.
    // Each structure is zero-initialised by default; only fields set to `true` here
    // are required – the driver will reject device creation if any are unsupported.
    vk::StructureChain<
        vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features, vk::PhysicalDeviceVulkan12Features,
        vk::PhysicalDeviceVulkan13Features, vk::PhysicalDeviceVulkan14Features,
        // EXT
        vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT, vk::PhysicalDeviceDescriptorHeapFeaturesEXT,
        vk::PhysicalDeviceBlendOperationAdvancedFeaturesEXT,
        vk::PhysicalDeviceMeshShaderFeaturesEXT, vk::PhysicalDeviceDeviceGeneratedCommandsFeaturesEXT,
        vk::PhysicalDeviceMemoryPriorityFeaturesEXT,
        vk::PhysicalDeviceMemoryDecompressionFeaturesEXT, vk::PhysicalDevicePageableDeviceLocalMemoryFeaturesEXT,
        vk::PhysicalDevicePresentTimingFeaturesEXT, vk::PhysicalDeviceRayTracingInvocationReorderFeaturesEXT,
        vk::PhysicalDeviceOpacityMicromapFeaturesEXT, vk::PhysicalDeviceShaderObjectFeaturesEXT,
        // KHR
        vk::PhysicalDeviceFragmentShadingRateFeaturesKHR, vk::PhysicalDeviceDeviceAddressCommandsFeaturesKHR,
        vk::PhysicalDeviceAccelerationStructureFeaturesKHR, vk::PhysicalDeviceRayTracingPipelineFeaturesKHR,
        vk::PhysicalDeviceRayQueryFeaturesKHR, vk::PhysicalDeviceRayTracingMaintenance1FeaturesKHR,
        vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR,
        vk::PhysicalDeviceMaintenance7FeaturesKHR, vk::PhysicalDeviceMaintenance8FeaturesKHR,
        vk::PhysicalDeviceMaintenance9FeaturesKHR, vk::PhysicalDeviceMaintenance10FeaturesKHR,
        vk::PhysicalDeviceCopyMemoryIndirectFeaturesKHR, vk::PhysicalDevicePresentModeFifoLatestReadyFeaturesKHR,
        vk::PhysicalDeviceShaderUntypedPointersFeaturesKHR, vk::PhysicalDeviceClusterAccelerationStructureFeaturesNV,
        vk::PhysicalDevicePartitionedAccelerationStructureFeaturesNV>
        featureChain = {// vk::PhysicalDeviceFeatures2
                        {.features =
                             {
                                 .geometryShader = true,
                                 .sampleRateShading = true,
                                 .multiDrawIndirect = true,
                                 .samplerAnisotropy = true,
                                 // BC7 KTX2 textures from tools/gltf_ktx2
                                 .textureCompressionBC = true,
                                 .shaderInt64 = true,
                             }},
                        // vk::PhysicalDeviceVulkan11Features
                        {},
                        // vk::PhysicalDeviceVulkan12Features
                        // Designators must follow the struct field order (ISO C++).
                        {
                            // mesh.slang: uint8_t* meshletTriangles via PhysicalStorageBuffer
                            .storageBuffer8BitAccess = true,
                            .shaderInt8 = true,
                            .descriptorIndexing = true,
                            .shaderSampledImageArrayNonUniformIndexing = true,
                            .shaderStorageBufferArrayNonUniformIndexing = true,
                            .descriptorBindingPartiallyBound = true,
                            .descriptorBindingVariableDescriptorCount = true,
                            .runtimeDescriptorArray = true,
                            // GpuVertex float3@0/12 packing with slang -fvk-use-scalar-layout
                            .scalarBlockLayout = true,
                            .bufferDeviceAddress = true,
                            .vulkanMemoryModel = true,
                            .vulkanMemoryModelDeviceScope = true,
                        },
                        // vk::PhysicalDeviceVulkan13Features
                        {.synchronization2 = true,
                         .textureCompressionASTC_HDR = false, // rtx 2060 super not supported
                         .dynamicRendering = true,
                         .maintenance4 = true},
                        // vk::PhysicalDeviceVulkan14Features
                        {.globalPriorityQuery = true,
                         .dynamicRenderingLocalRead = true,
                         .maintenance5 = true,
                         .maintenance6 = true,
                         .hostImageCopy = true},
                        // vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT
                        {.extendedDynamicState = true},
                        // vk::PhysicalDeviceDescriptorHeapFeaturesEXT (required, checked in pickPhysicalDevice)
                        {.descriptorHeap = true},
                        // vk::PhysicalDeviceBlendOperationAdvancedFeaturesEXT
                        {.advancedBlendCoherentOperations = false},
                        // vk::PhysicalDeviceMeshShaderFeaturesEXT
                        {.taskShader = true, .meshShader = true, .meshShaderQueries = true},
                        // vk::PhysicalDeviceDeviceGeneratedCommandsFeaturesEXT
                        {.deviceGeneratedCommands = true},
                        // vk::PhysicalDeviceMemoryPriorityFeaturesEXT
                        {.memoryPriority = true},
                        // vk::PhysicalDeviceMemoryDecompressionFeaturesEXT
                        {.memoryDecompression = true},
                        // vk::PhysicalDevicePageableDeviceLocalMemoryFeaturesEXT
                        {.pageableDeviceLocalMemory = true},
                        // vk::PhysicalDevicePresentTimingFeaturesEXT (extension enabled, feature not requested yet)
                        {},
                        // vk::PhysicalDeviceRayTracingInvocationReorderFeaturesEXT
                        {.rayTracingInvocationReorder = true},
                        // vk::PhysicalDeviceOpacityMicromapFeaturesEXT
                        {.micromap = true},
                        // vk::PhysicalDeviceShaderObjectFeaturesEXT
                        {.shaderObject = true},
                        // vk::PhysicalDeviceFragmentShadingRateFeaturesKHR
                        {.pipelineFragmentShadingRate = true,
                         .primitiveFragmentShadingRate = true,
                         .attachmentFragmentShadingRate = true},
                        // vk::PhysicalDeviceDeviceAddressCommandsFeaturesKHR
                        {.deviceAddressCommands = true},
                        // vk::PhysicalDeviceAccelerationStructureFeaturesKHR
                        {.accelerationStructure = true},
                        // vk::PhysicalDeviceRayTracingPipelineFeaturesKHR
                        {.rayTracingPipeline = true,
                         .rayTracingPipelineTraceRaysIndirect = true,
                         .rayTraversalPrimitiveCulling = true},
                        // vk::PhysicalDeviceRayQueryFeaturesKHR
                        {.rayQuery = true},
                        // vk::PhysicalDeviceRayTracingMaintenance1FeaturesKHR
                        {.rayTracingMaintenance1 = true, .rayTracingPipelineTraceRaysIndirect2 = true},
                        // vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR
                        {.swapchainMaintenance1 = true},
                        // vk::PhysicalDeviceMaintenance7FeaturesKHR
                        {.maintenance7 = true},
                        // vk::PhysicalDeviceMaintenance8FeaturesKHR
                        {.maintenance8 = true},
                        // vk::PhysicalDeviceMaintenance9FeaturesKHR
                        // Keep false while using Khronos Best Practices (VVL #12449, SDK ≤1.4.357).
                        // BP ValidateImageInQueue does: qf_count = last_usage.queue_family_index + 1
                        // then qf_props.back(). On first image use last_usage is still
                        // VK_QUEUE_FAMILY_IGNORED (0xFFFFFFFF) → count wraps to 0 → empty
                        // vector .back() writes to 0xFFFFFFFFFFFFFFE0 (sizeof(VkQueueFamilyProperties2)
                        // element -1, pNext at +8). Submit queue family from getDeviceQueue is fine;
                        // IGNORED is VVL's "never used" sentinel, not an app QFOT barrier mistake.
                        // Re-enable when VVL skips IGNORED/UNDEFINED before that query.
                        {.maintenance9 = false},
                        // vk::PhysicalDeviceMaintenance10FeaturesKHR
                        {.maintenance10 = true},
                        // vk::PhysicalDeviceCopyMemoryIndirectFeaturesKHR
                        {.indirectMemoryCopy = true, .indirectMemoryToImageCopy = true},
                        // vk::PhysicalDevicePresentModeFifoLatestReadyFeaturesKHR
                        {.presentModeFifoLatestReady = true},
                        // vk::PhysicalDeviceShaderUntypedPointersFeaturesKHR};
                        {.shaderUntypedPointers = true},
                        // vk::PhysicalDeviceClusterAccelerationStructureFeaturesNV
                        {.clusterAccelerationStructure = true},
                        // vk::PhysicalDevicePartitionedAccelerationStructureFeaturesNV
                        {.partitionedAccelerationStructure = true}};
    if (!capabilities.hasClusterAccelerationStructure) {
        featureChain.unlink<vk::PhysicalDeviceClusterAccelerationStructureFeaturesNV>();
    }
    if (!capabilities.hasPartitionedAccelerationStructure) {
        featureChain.unlink<vk::PhysicalDevicePartitionedAccelerationStructureFeaturesNV>();
    }

    std::vector<const char*> enabledExtensions(kRequiredDeviceExtensions.begin(), kRequiredDeviceExtensions.end());
    if (capabilities.hasClusterAccelerationStructure) {
        enabledExtensions.push_back(vk::NVClusterAccelerationStructureExtensionName);
    }
    if (capabilities.hasPartitionedAccelerationStructure) {
        enabledExtensions.push_back(vk::NVPartitionedAccelerationStructureExtensionName);
    }

    // one create info per distinct family; duplicates are a validation error.
    // the same set feeds concurrent sharing, where order does not matter
    queueFamilyIndices = {graphicsIndex, presentIndex, transferIndex, computeIndex};
    std::ranges::sort(queueFamilyIndices);
    const auto duplicates = std::ranges::unique(queueFamilyIndices);
    queueFamilyIndices.erase(duplicates.begin(), duplicates.end());

    const float queuePriority = 0.0f;
    std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos;
    queueCreateInfos.reserve(queueFamilyIndices.size());
    for (const uint32_t family : queueFamilyIndices) {
        queueCreateInfos.push_back({.queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &queuePriority});
    }

    vk::DeviceCreateInfo deviceCreateInfo{.pNext = &featureChain.get<vk::PhysicalDeviceFeatures2>(),
                                          .queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size()),
                                          .pQueueCreateInfos = queueCreateInfos.data(),
                                          .enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size()),
                                          .ppEnabledExtensionNames = enabledExtensions.data()};

    vkdevice = vk::raii::Device(physicalDevice, deviceCreateInfo);

    setDebugName(vkdevice, instance, "Instance");
    setDebugName(vkdevice, physicalDevice, "PhysicalDevice");
    setDebugName(vkdevice, surface, "WindowSurface");
    setDebugName(vkdevice, vkdevice, "LogicalDevice");

    // Cache supported MSAA sample count for downstream components (e.g., pipelines, resources).
    msaaSamples = getMaxUsableSampleCount();

    createQueues();
}

void Device::createQueues()
{
    graphicsQueue = vk::raii::Queue(vkdevice, graphicsIndex, 0);
    presentQueue = vk::raii::Queue(vkdevice, presentIndex, 0);
    transferQueue = vk::raii::Queue(vkdevice, transferIndex, 0);
    computeQueue = vk::raii::Queue(vkdevice, computeIndex, 0);

    setDebugName(vkdevice, graphicsQueue, "GraphicsQueue");
    setDebugName(vkdevice, presentQueue, "PresentQueue");
    setDebugName(vkdevice, transferQueue, "TransferQueue");
    setDebugName(vkdevice, computeQueue, "ComputeQueue");

    log_info(std::format("Queue families: graphics={} present={} transfer={}{} compute={}{} (distinct {})",
                         graphicsIndex, presentIndex, transferIndex,
                         hasDedicatedTransferQueue() ? " (dedicated)" : " (shared)", computeIndex,
                         computeIndex != graphicsIndex ? " (async)" : " (shared)", queueFamilyIndices.size()),
             "Device");
}
