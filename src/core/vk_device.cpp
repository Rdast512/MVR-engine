/**
 * @file vk_device.cpp
 * @brief Vulkan device management: instance, physical-device selection, logical device, and queue setup.
 *
 * This translation unit implements the full Vulkan device initialisation pipeline:
 *   - Vulkan instance creation with SDL3 WSI extensions and optional validation
 *   - VK_EXT_debug_utils messenger registration for validation layer output
 *   - Window-surface creation via SDL3
 *   - Physical-device scoring and selection
 *   - Logical-device creation with an exhaustive feature chain (Vulkan 1.1–1.4 +
 *     ray-tracing, mesh shaders, shader objects, present-timing, etc.)
 *   - Queue family resolution with fallback strategies for transfer and compute
 */
#include "vk_device.hpp"
#include <format>
#include "../static_headers/logger.hpp"
#include "../util/debug.hpp"
#include "tracy/Tracy.hpp"
#include "vulkan/vulkan.hpp"
/// Validation layer requested when enableValidationLayers is true.
const std::vector validationLayers = {"VK_LAYER_KHRONOS_validation"};


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


    // Retrieve the WSI extensions required by SDL3 (e.g. VK_KHR_surface, VK_KHR_win32_surface).
    Uint32 count_instance_extensions;
    const char* const* instance_extensions = SDL_Vulkan_GetInstanceExtensions(&count_instance_extensions);

    auto extensionProperties = context.enumerateInstanceExtensionProperties();

    // Verify every SDL3-required extension is advertised by the Vulkan loader before
    // attempting to enable it; fail early with a descriptive message if one is missing.
    for (uint32_t i = 0; i < count_instance_extensions; ++i) {
        if (std::ranges::none_of(extensionProperties,
                                 [glfwExtension = instance_extensions[i]](auto const& extensionProperty) -> auto
                                 { return strcmp(extensionProperty.extensionName, glfwExtension) == 0; })) {
            throw std::runtime_error("Required SDL3 extension not supported: " + std::string(instance_extensions[i]));
        }
    }
    std::cout << "available Instance extensions:\n";
    for (const auto& extension : extensionProperties) {
        std::cout << "  " << extension.extensionName << std::endl;
    }
    // Seed the extension list with the SDL3-required surface extensions, then append
    // engine-specific ones.  EXT_debug_utils is always enabled (not just for validation
    // mode) so that debug labels and object names work in RenderDoc / NVIDIA Nsight.
    std::vector extensions(instance_extensions, instance_extensions + count_instance_extensions);
    extensions.push_back(vk::EXTDebugUtilsExtensionName);
    // The following instance extensions are required by KHR_swapchain_maintenance1 and
    // must be promoted to instance scope before the logical device is created.
    extensions.push_back(vk::KHRDisplayExtensionName);
    extensions.push_back(vk::KHRSurfaceMaintenance1ExtensionName);
    extensions.push_back(vk::KHRGetDisplayProperties2ExtensionName);
    extensions.push_back(vk::KHRGetSurfaceCapabilities2ExtensionName);

    std::cout << "enabled Instance extensions:\n";
    for (const auto& extension : extensions) {
        std::cout << "  " << extension << std::endl;
    }
    // Conditionally request the Khronos validation layer.  Enumerate available layers
    // first and bail out immediately if any requested layer is not present, rather than
    // letting the driver produce a cryptic error later.
    std::vector<char const*> requiredLayers;
    if (enableValidationLayers) {
        requiredLayers.assign(validationLayers.begin(), validationLayers.end());
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

vk::SampleCountFlagBits Device::getMaxUsableSampleCount()
{
    vk::PhysicalDeviceProperties physicalDeviceProperties = physicalDevice.getProperties2().properties;

    // Intersect colour and depth sample-count bitmasks: only counts supported by
    // both attachment types are usable for combined colour+depth MSAA render passes.
    vk::SampleCountFlags counts = physicalDeviceProperties.limits.framebufferColorSampleCounts &
        physicalDeviceProperties.limits.framebufferDepthSampleCounts;
    if (counts & vk::SampleCountFlagBits::e64) {
        return vk::SampleCountFlagBits::e64;
    }
    if (counts & vk::SampleCountFlagBits::e32) {
        return vk::SampleCountFlagBits::e32;
    }
    if (counts & vk::SampleCountFlagBits::e16) {
        return vk::SampleCountFlagBits::e16;
    }
    if (counts & vk::SampleCountFlagBits::e8) {
        return vk::SampleCountFlagBits::e8;
    }
    if (counts & vk::SampleCountFlagBits::e4) {
        return vk::SampleCountFlagBits::e4;
    }
    if (counts & vk::SampleCountFlagBits::e2) {
        return vk::SampleCountFlagBits::e2;
    }

    return vk::SampleCountFlagBits::e1;
}


void Device::pickPhysicalDevice()
{
    ZoneScopedN("Device::pickPhysicalDevice");
    auto devices = instance.enumeratePhysicalDevices();
    if (devices.empty()) {
        throw std::runtime_error("failed to find GPUs with Vulkan support!");
    }

    // Score every enumerated device and keep them sorted in a multimap so the
    // highest-scoring one can be retrieved with a single rbegin() call.
    std::multimap<int, vk::raii::PhysicalDevice> candidates;
    for (const auto& device : devices) {
        auto props2 = device.getProperties2();
        auto deviceProperties = props2.properties;
        auto features2 = device.getFeatures2();
        deviceFeatures = features2.features;
        uint32_t score = 0;

        // Discrete GPUs have a significant performance advantage over integrated ones.
        if (deviceProperties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu) {
            score += 1000;
        }

        // Higher max texture dimension generally correlates with a more capable GPU.
        score += deviceProperties.limits.maxImageDimension2D;

        // Geometry shaders are required by the engine's rendering pipeline.
        if (!deviceFeatures.geometryShader) {
            continue;
        }
        candidates.insert(std::make_pair(score, device));
    }
    if (candidates.rbegin()->first > 0) {
        physicalDevice = candidates.rbegin()->second;
    } else {
        throw std::runtime_error("failed to find a suitable GPU!");
    }
    auto ret = physicalDevice.getQueueFamilyProperties2();

    log_info(std::format("Using physical device: {}",
                         std::string_view(physicalDevice.getProperties2().properties.deviceName.data())),
             "Device");
    log_info(std::format("Queue amount: {}", ret.size()), "Device");
    for (const auto& qfp : ret) {
        const bool graphics =
            (qfp.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eGraphics) != static_cast<vk::QueueFlags>(0);
        const bool compute =
            (qfp.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eCompute) != static_cast<vk::QueueFlags>(0);
        const bool transfer =
            (qfp.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eTransfer) != static_cast<vk::QueueFlags>(0);
        log_info(std::format("Queue family count: {} graphics: {} compute: {} transfer: {}",
                             qfp.queueFamilyProperties.queueCount, graphics, compute, transfer),
                 "Device");
    }
}

void Device::findQueueFamilies(const std::vector<vk::QueueFamilyProperties2>& queueFamilyProperties2)
{
    // --- Graphics queue -------------------------------------------------
    // Pick the first queue family that advertises graphics support.  On virtually
    // every desktop GPU this will be family 0.
    auto graphicsQueueFamilyProperty =
        std::ranges::find_if(queueFamilyProperties2,
                             [](auto const& qfp)
                             {
                                 return (qfp.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eGraphics) !=
                                     static_cast<vk::QueueFlags>(0);
                             });
    graphicsIndex = static_cast<uint32_t>(std::distance(queueFamilyProperties2.begin(), graphicsQueueFamilyProperty));

    // --- Present queue --------------------------------------------------
    // Prefer sharing the graphics queue for present to avoid unnecessary ownership
    // transfers; fall back to any family that supports present if needed.
    presentIndex = physicalDevice.getSurfaceSupportKHR(graphicsIndex, *surface)
        ? graphicsIndex
        : static_cast<uint32_t>(queueFamilyProperties2.size());
    if (presentIndex == queueFamilyProperties2.size()) {
        for (size_t i = 0; i < queueFamilyProperties2.size(); i++) {
            if ((queueFamilyProperties2[i].queueFamilyProperties.queueFlags & vk::QueueFlagBits::eGraphics) &&
                physicalDevice.getSurfaceSupportKHR(static_cast<uint32_t>(i), *surface)) {
                graphicsIndex = static_cast<uint32_t>(i);
                presentIndex = graphicsIndex;
                break;
            }
        }
        if (presentIndex == queueFamilyProperties2.size()) {
            for (size_t i = 0; i < queueFamilyProperties2.size(); i++) {
                if (physicalDevice.getSurfaceSupportKHR(static_cast<uint32_t>(i), *surface)) {
                    presentIndex = static_cast<uint32_t>(i);
                    break;
                }
            }
        }
    }
    if ((graphicsIndex == queueFamilyProperties2.size()) || (presentIndex == queueFamilyProperties2.size())) {
        throw std::runtime_error("Could not find a queue for graphics or present -> terminating");
    }

    // --- Transfer queue -------------------------------------------------
    // Prefer a dedicated DMA queue (transfer-only family) for async uploads and
    // buffer copies; these bypass graphics pipeline scheduling on AMD/NVIDIA.
    transferIndex = static_cast<uint32_t>(queueFamilyProperties2.size()); // sentinel = "not found"
    for (size_t i = 0; i < queueFamilyProperties2.size(); i++) {
        if ((queueFamilyProperties2[i].queueFamilyProperties.queueFlags & vk::QueueFlagBits::eTransfer) &&
            !(queueFamilyProperties2[i].queueFamilyProperties.queueFlags & vk::QueueFlagBits::eGraphics)) {
            transferIndex = static_cast<uint32_t>(i);
            break;
        }
    }

    // --- Compute queue --------------------------------------------------
    // Prefer a dedicated async-compute family (no graphics flag) to allow compute
    // work to overlap with in-flight graphics frames on supporting hardware.
    computeIndex = static_cast<uint32_t>(queueFamilyProperties2.size()); // sentinel = "not found"
    for (size_t i = 0; i < queueFamilyProperties2.size(); i++) {
        const auto flags = queueFamilyProperties2[i].queueFamilyProperties.queueFlags;
        if ((flags & vk::QueueFlagBits::eCompute) && !(flags & vk::QueueFlagBits::eGraphics)) {
            computeIndex = static_cast<uint32_t>(i);
            break;
        }
    }

    // --- Fallback: shared transfer+compute ---------------------------------
    // If either a dedicated transfer or compute family was not found, attempt to
    // find a non-graphics family that supports both operations together (common on
    // Qualcomm / Intel integrated).  If that also fails, fall all the way back to
    // the graphics queue so the indices are always valid.
    if (transferIndex == static_cast<uint32_t>(queueFamilyProperties2.size()) ||
        computeIndex == static_cast<uint32_t>(queueFamilyProperties2.size())) {
        uint32_t sharedTransferComputeIndex = static_cast<uint32_t>(queueFamilyProperties2.size());

        // Pass 1: non-graphics family that can do both transfer and compute.
        for (size_t i = 0; i < queueFamilyProperties2.size(); i++) {
            const auto flags = queueFamilyProperties2[i].queueFamilyProperties.queueFlags;
            const bool supportsTransfer = (flags & vk::QueueFlagBits::eTransfer) != static_cast<vk::QueueFlags>(0);
            const bool supportsCompute = (flags & vk::QueueFlagBits::eCompute) != static_cast<vk::QueueFlags>(0);
            const bool supportsGraphics = (flags & vk::QueueFlagBits::eGraphics) != static_cast<vk::QueueFlags>(0);
            if (supportsTransfer && supportsCompute && !supportsGraphics) {
                sharedTransferComputeIndex = static_cast<uint32_t>(i);
                break;
            }
        }

        // Pass 2: any family (including graphics) that supports both.
        if (sharedTransferComputeIndex == static_cast<uint32_t>(queueFamilyProperties2.size())) {
            for (size_t i = 0; i < queueFamilyProperties2.size(); i++) {
                const auto flags = queueFamilyProperties2[i].queueFamilyProperties.queueFlags;
                const bool supportsTransfer = (flags & vk::QueueFlagBits::eTransfer) != static_cast<vk::QueueFlags>(0);
                const bool supportsCompute = (flags & vk::QueueFlagBits::eCompute) != static_cast<vk::QueueFlags>(0);
                if (supportsTransfer && supportsCompute) {
                    sharedTransferComputeIndex = static_cast<uint32_t>(i);
                    break;
                }
            }
        }

        // Pass 3: last resort – share the graphics queue.
        if (sharedTransferComputeIndex == static_cast<uint32_t>(queueFamilyProperties2.size())) {
            sharedTransferComputeIndex = graphicsIndex;
        }

        transferIndex = sharedTransferComputeIndex;
        computeIndex = sharedTransferComputeIndex;
    }
}

void Device::createLogicalDevice()
{
    ZoneScopedN("Device::createLogicalDevice");
    auto queueFamilyProperties2 = physicalDevice.getQueueFamilyProperties2();
    using PropsChain = vk::StructureChain<vk::QueueFamilyProperties2, vk::QueueFamilyOwnershipTransferPropertiesKHR>;
    // One-liner query
    std::vector<PropsChain> queueFamilyProps = physicalDevice.getQueueFamilyProperties2<PropsChain>();
    findQueueFamilies(queueFamilyProperties2);

    for (size_t i = 0; i < queueFamilyProps.size(); ++i) {
        const auto& props = queueFamilyProps[i].get<vk::QueueFamilyOwnershipTransferPropertiesKHR>();
        log_info(std::format("Queue family {} ownership properties:", i), "Device");
        auto mask = props.optimalImageTransferToQueueFamilies;
        std::string out = std::format("optimalImageTransferToQueueFamilies: 0x{:x} (dec {}) binary {}\n", mask, mask,
                                      std::bitset<32>(mask).to_string());
        // Collect set indices (portable)
        std::vector<uint32_t> setIndices;
        for (uint32_t i = 0; i < queueFamilyProperties2.size() && i < 32; ++i) {
            if (mask & (1u << i))
                setIndices.push_back(i);
        }

        if (setIndices.empty()) {
            out += "  -> no queue families set\n";
        } else {
            out += "  -> queue family indices: ";
            for (size_t j = 0; j < setIndices.size(); ++j) {
                if (j)
                    out += ", ";
                out += std::format("{}", setIndices[j]);
            }
        }
        log_info(out, "Device");
    }

    auto propertiesChain = physicalDevice.getProperties2<
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
        vk::PhysicalDeviceDescriptorBufferPropertiesEXT, vk::PhysicalDeviceFragmentShadingRatePropertiesKHR,
        vk::PhysicalDeviceAccelerationStructurePropertiesKHR, vk::PhysicalDeviceOpacityMicromapPropertiesEXT,
        vk::PhysicalDeviceDepthStencilResolveProperties, vk::PhysicalDeviceDriverProperties,
        vk::PhysicalDeviceMaintenance3Properties, vk::PhysicalDeviceMaintenance4Properties,
        vk::PhysicalDeviceMaintenance5Properties, vk::PhysicalDeviceMaintenance6Properties,
        vk::PhysicalDeviceMaintenance7PropertiesKHR, vk::PhysicalDeviceMaintenance9PropertiesKHR,
        vk::PhysicalDeviceMaintenance10PropertiesKHR, vk::PhysicalDevicePipelineBinaryPropertiesKHR,
        vk::PhysicalDeviceRayTracingPipelinePropertiesKHR,
        vk::PhysicalDevicePartitionedAccelerationStructurePropertiesNV,
        vk::PhysicalDeviceClusterAccelerationStructurePropertiesNV>();
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
    capabilities.descriptorBuffer = propertiesChain.get<vk::PhysicalDeviceDescriptorBufferPropertiesEXT>();
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

    const auto descriptorHeapFeatureQuery =
        physicalDevice.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceDescriptorHeapFeaturesEXT>();
    const bool descriptorHeapFeatureSupported =
        descriptorHeapFeatureQuery.get<vk::PhysicalDeviceDescriptorHeapFeaturesEXT>().descriptorHeap == vk::True;
    descriptorBindingMode =
        descriptorHeapFeatureSupported ? DescriptorBindingMode::DescriptorHeaps : DescriptorBindingMode::LegacySets;

    // Build a pNext feature chain covering every extension the engine depends on.
    // Each structure is zero-initialised by default; only fields set to `true` here
    // are required – the driver will reject device creation if any are unsupported.
    // query for Vulkan features

    vk::StructureChain<
        vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features, vk::PhysicalDeviceVulkan12Features,
        vk::PhysicalDeviceVulkan13Features, vk::PhysicalDeviceVulkan14Features,
        // EXT
        vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT, vk::PhysicalDeviceDescriptorHeapFeaturesEXT,
        vk::PhysicalDeviceDescriptorBufferFeaturesEXT, vk::PhysicalDeviceBlendOperationAdvancedFeaturesEXT,
        vk::PhysicalDeviceMeshShaderFeaturesEXT, vk::PhysicalDeviceDeviceGeneratedCommandsFeaturesEXT,
        vk::PhysicalDeviceMemoryPriorityFeaturesEXT,
        vk::PhysicalDeviceMemoryDecompressionFeaturesEXT, vk::PhysicalDevicePageableDeviceLocalMemoryFeaturesEXT,
        vk::PhysicalDeviceGraphicsPipelineLibraryFeaturesEXT,
        vk::PhysicalDevicePresentTimingFeaturesEXT, vk::PhysicalDeviceRayTracingInvocationReorderFeaturesEXT,
        vk::PhysicalDeviceTexelBufferAlignmentFeaturesEXT, vk::PhysicalDeviceOpacityMicromapFeaturesEXT,
        // KHR
        vk::PhysicalDeviceFragmentShadingRateFeaturesKHR, vk::PhysicalDeviceDeviceAddressCommandsFeaturesKHR,
        vk::PhysicalDeviceAccelerationStructureFeaturesKHR, vk::PhysicalDeviceRayTracingPipelineFeaturesKHR,
        vk::PhysicalDeviceRayQueryFeaturesKHR, vk::PhysicalDeviceRayTracingMaintenance1FeaturesKHR,
        vk::PhysicalDevicePipelineBinaryFeaturesKHR, vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR,
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
                            // Vertex float3@0/12 packing with slang -fvk-use-scalar-layout
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
                        // vk::PhysicalDeviceDescriptorHeapFeaturesEXT
                        {.descriptorHeap = descriptorHeapFeatureSupported},
                        // vk::PhysicalDeviceDescriptorBufferFeaturesEXT
                        {.descriptorBuffer = true},
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
                        // vk::PhysicalDeviceGraphicsPipelineLibraryFeaturesEXT (disabled: extension not enabled)
                        {},
                        // vk::PhysicalDevicePresentTimingFeaturesEXT (disabled: extension not enabled)
                        {},
                        // vk::PhysicalDeviceRayTracingInvocationReorderFeaturesEXT
                        {.rayTracingInvocationReorder = true},
                        // vk::PhysicalDeviceTexelBufferAlignmentFeaturesEXT (disabled: extension not enabled)
                        {},
                        // vk::PhysicalDeviceOpacityMicromapFeaturesEXT
                        {.micromap = true},
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
                        // vk::PhysicalDevicePipelineBinaryFeaturesKHR (disabled: extension not enabled)
                        {},
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

    // Each unique queue family needs exactly one VkDeviceQueueCreateInfo entry.
    // Requesting the same family index twice is a validation error, so we gate each
    // additional queue on it being distinct from all previously added families.
    float queuePriority = 0.0f;
    std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos;

    // Graphics queue – always required.
    queueCreateInfos.push_back(
        {.queueFamilyIndex = graphicsIndex, .queueCount = 1, .pQueuePriorities = &queuePriority});

    // Present queue – only add if it lives in a different family from graphics.
    if (presentIndex != graphicsIndex) {
        queueCreateInfos.push_back(
            {.queueFamilyIndex = presentIndex, .queueCount = 1, .pQueuePriorities = &queuePriority});
    }

    // Transfer queue – only add if dedicated (not shared with graphics or present).
    if (transferIndex != graphicsIndex && transferIndex != presentIndex) {
        queueCreateInfos.push_back(
            {.queueFamilyIndex = transferIndex, .queueCount = 1, .pQueuePriorities = &queuePriority});
    }

    // Compute queue – only add if it is a fully distinct family from all others.
    if (computeIndex != graphicsIndex && computeIndex != presentIndex && computeIndex != transferIndex) {
        queueCreateInfos.push_back(
            {.queueFamilyIndex = computeIndex, .queueCount = 1, .pQueuePriorities = &queuePriority});
    }

    // create a Device
    vk::DeviceCreateInfo deviceCreateInfo{.pNext = &featureChain.get<vk::PhysicalDeviceFeatures2>(),
                                          .queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size()),
                                          .pQueueCreateInfos = queueCreateInfos.data(),
                                          .enabledExtensionCount =
                                              static_cast<uint32_t>(requiredDeviceExtension.size()),
                                          .ppEnabledExtensionNames = requiredDeviceExtension.data()};

    vkdevice = vk::raii::Device(physicalDevice, deviceCreateInfo);

    if (descriptorBindingMode == DescriptorBindingMode::DescriptorHeaps) {
        log_info("Descriptor binding mode: DescriptorHeaps (descriptor heap feature supported)", "Device");
    } else {
        log_info("Descriptor binding mode: LegacySets (descriptor heap feature unsupported on this GPU)", "Device");
    }

    setDebugName(vkdevice, instance, "Instance");
    setDebugName(vkdevice, physicalDevice, "PhysicalDevice");
    setDebugName(vkdevice, surface, "WindowSurface");
    setDebugName(vkdevice, vkdevice, "LogicalDevice");

    // Cache supported MSAA sample count for downstream components (e.g., pipelines, resources).
    msaaSamples = getMaxUsableSampleCount();

    // Print queue family usage
    if (graphicsIndex == presentIndex) {
        log_info(std::format("Using single queue for graphics and present: {}", graphicsIndex), "Device");
    } else {
        log_info("Using separate queues for graphics and present", "Device");
    }

    if (transferIndex != graphicsIndex && transferIndex != presentIndex) {
        log_info(std::format("Using transfer queue family {}", transferIndex), "Device");
    } else {
        log_info("No separate transfer queue found, sharing with graphics/present queue", "Device");
    }

    if (computeIndex != graphicsIndex && computeIndex != presentIndex && computeIndex != transferIndex) {
        log_info(std::format("Using dedicated compute queue family {}", computeIndex), "Device");
    } else if (computeIndex == transferIndex) {
        log_info(std::format("Using shared transfer+compute queue family {}", computeIndex), "Device");
    } else {
        log_info("No separate compute queue found, sharing with graphics/present queue", "Device");
    }

    if (transferIndex != UINT32_MAX) {
        transferQueue = vk::raii::Queue(vkdevice, transferIndex, 0);
    }
    if (computeIndex != UINT32_MAX) {
        computeQueue = vk::raii::Queue(vkdevice, computeIndex, 0);
    }
    graphicsQueue = vk::raii::Queue(vkdevice, graphicsIndex, 0);
    presentQueue = vk::raii::Queue(vkdevice, presentIndex, 0);

    setDebugName(vkdevice, graphicsQueue, "GraphicsQueue");
    setDebugName(vkdevice, presentQueue, "PresentQueue");
    if (transferIndex != UINT32_MAX) {
        setDebugName(vkdevice, transferQueue, "TransferQueue");
    }
    if (computeIndex != UINT32_MAX) {
        setDebugName(vkdevice, computeQueue, "ComputeQueue");
    }
    log_info(std::format("Using graphics queue: {} | present queue: {} | transfer queue: {} | compute queue: {}",
                         graphicsIndex, presentIndex,
                         (transferIndex != UINT32_MAX ? std::to_string(transferIndex) : "N/A"),
                         (computeIndex != UINT32_MAX ? std::to_string(computeIndex) : "N/A")),
             "Device");

    queueFamilyIndices.push_back(graphicsIndex);

    if (presentIndex != graphicsIndex) {
        queueFamilyIndices.push_back(presentIndex);
    }

    if (transferIndex != UINT32_MAX && transferIndex != graphicsIndex && transferIndex != presentIndex) {
        queueFamilyIndices.push_back(transferIndex);
    }

    if (computeIndex != UINT32_MAX && computeIndex != graphicsIndex && computeIndex != presentIndex &&
        computeIndex != transferIndex) {
        queueFamilyIndices.push_back(computeIndex);
    }
}
